/* pc_racing_line.cpp - PC: the racing line on the road (pc_racing_line.h).
 * No original: new PC code.  Linux only: the whole body is left out of the
 * Windows build (this file is compiled there too, by the game's GLOB). */
#ifndef _WIN32

#include "libc.h"
#include "game/app/pc_racing_line.h"
#include "game/race/core/T130_race_ps2_base.h"        /* RacePS2Base (getReplayMode) */
#include "game/race/core/T121_race_entry.h"           /* RaceEntryCar::state */
#include "game/race/core/T153_race_course.h"          /* RaceCourse::getLapLength */
#include "game/camera/T383_camera_manager.h"          /* CameraManager::views, current */
#include "game/car/physics/T302_car_state.h"          /* RaceCarState, car_view_x / y / z, car_lap_distance */
#include "game/race/dynamics/T305_dynamics_conductor.h" /* DynamicsConductor::lines */
#include "game/race/ai/T368_enemy_lines.h"            /* CourseLines, EnemyLines, lines_ground_height */
#include "game/race/ai/T371_line_segment.h"           /* line_point_at */
#include "game/race/core/T125_race_organization.h" /* RaceOrganization: backgroundMode, racePreviewMode, loggerMode */
#include "game/app/pc_debugcam.h"                     /* pc_debugcam_owns */
#include "engine/pgl/G001_flare.h"                    /* GsContext, gfx_alloc_tail, strip_*, gfx_* */
#include "platform/log.h"
#include "platform/paths.h"                           /* ini_path */
#include "platform/settings.h"
#include "platform/os/ini_posix.h"                    /* GetPrivateProfileStringA */

#include <math.h>
#include <string.h>
#include <strings.h>

/* The drawing context (engine/pgl/pgl_port.h): its state is read to be put
 * back after the strip. */
extern "C" GsContext g_pglCtx;

namespace {

constexpr int kLineRacing = 6;              /* the racing line (LINE_RACING, car_input_internal.h) */
constexpr int kSamples = 60;                /* the strip's points */
constexpr f32 kFirst = 2.0f;                /* m ahead of the car: the first point */
constexpr f32 kStep = 2.0f;                 /* m between two points */
constexpr f32 kHalfWidth = 0.125f;          /* m: half the strip's width */
constexpr f32 kLift = 0.04f;                /* m above the road */
constexpr f32 kProbe = 1.0f;                /* m above a point: where a road height probe starts */
constexpr f32 kFarRoad = 1.5f;              /* m: a road found from every sector is taken this near the point before */
constexpr f32 kJump = 50.0f;                /* m: a lap distance moved more in a frame is a cut (a replay's) */
constexpr int kLogEvery = 6;                /* the log: every 6th frame of a view */
constexpr int kViews = 4;                   /* CameraManager::views */
constexpr int kStripVerts = 64;             /* a strip's vertices at most (VU1_MAX_VERTS) */
constexpr int kFadeIn = 4;                  /* samples from nothing to full at the near end (8 m) */
constexpr int kFadeOut = 15;                /* and back to nothing at the far end (30 m) */
/* The strip's colour (GS: R, G, B 0..255, A 0x80 = opaque) at full. */
constexpr u32 kRed = 0x40, kGreen = 0xD8, kBlue = 0xFF, kAlpha = 0x58;

/* GsContext bits read to restore the state (engine/pgl/pgl_port.h). */
constexpr u16 kPrimTme = 0x010, kPrimAbe = 0x040;
constexpr u64 kTestAte = 1ull << 0, kTestDate = 1ull << 14;

/* One point of the strip: its lap distance, its centre in the view's space
 * (x, the road's height, -y), and its two vertices. */
struct Sample {
    f32 dist;
    int node;                               /* the line's node line_point_at returned (its segment's first) */
    int miss;                               /* the road probe (road_height): 0 / 1 found, 2 off the road */
    int taken;                              /* the probed road is the height (else the point before's) */
    f32 lineHeight;                         /* the line's own height there (between its nodes' road heights) */
    f32 centre[3];
    f32 left[3];
    f32 right[3];
};

struct ViewState {
    int hint;                               /* the node line_point_at returned for the first point */
    int sector;                             /* the runway sector the first point's road was found in */
    int hasFirst;                           /* firstHeight holds the last frame's first point's height */
    f32 firstHeight;
    unsigned frame;                         /* the view's frames (the log's frame number) */
    const RaceCarState *car;                /* the car of the last frame */
    f32 dist;                               /* its lap distance */
};

ViewState g_views[kViews] = {{0, -1, 0, 0.0f, 0, NULL, 0.0f}, {0, -1, 0, 0.0f, 0, NULL, 0.0f},
                             {0, -1, 0, 0.0f, 0, NULL, 0.0f}, {0, -1, 0, 0.0f, 0, NULL, 0.0f}};
Sample g_samples[kSamples];
/* The last course whose line 6 (or its lack) was logged: its lines and its
 * lap length (another course may be loaded at the same address). */
const EnemyLines *g_loggedLines;
f32 g_loggedLap;

/* 1 the first time for a course (then remembered as the logged one); the
 * views' hints, which index that course's nodes and runway sectors, start
 * over. */
int first_for_course(const EnemyLines *lines, f32 lap)
{
    if (g_loggedLines == lines && g_loggedLap == lap)
        return 0;
    g_loggedLines = lines;
    g_loggedLap = lap;
    for (ViewState &v : g_views) {
        v.hint = 0;
        v.sector = -1;
        v.hasFirst = 0;
    }
    return 1;
}

/* The road height under (x, y) of the lines' space, probed from kProbe
 * above z.  The runway query searches the hint sector and its neighbours:
 * first *sector (the sector the last probe found), then the sector of the
 * line node `node', then every sector (it then takes the highest road under
 * the point).  *sector: the sector found; *miss: 0 found from a hint, 1
 * found from every sector, 2 off the road. */
f32 road_height(CourseLines *cl, int node, int *sector, f32 x, f32 y, f32 z, int *miss)
{
    const int hints[3] = {*sector, cl->data->line[kLineRacing]->node[node].sector, -1};
    for (int k = 0; k < 3; k++) {
        if (k < 2 && hints[k] < 0)
            continue;
        int found = hints[k], off = 0;
        const f32 h = lines_ground_height(cl->course, x, y, z + kProbe, &found, &off);
        if (off == 0) {
            *sector = found;
            *miss = k < 2 ? 0 : 1;
            return h;
        }
    }
    *miss = 2;
    return 0.0f;
}

/* Line 6's nodes, once for a course: the index, kind, x / y / height
 * (the lines' space), lap distance, radius, heading and runway sector. */
void log_nodes(CourseLines *cl, f32 lap)
{
    const Line *line = cl->data->line[kLineRacing];
    pclog::write("racing line: line 6, %d nodes, lap %.3f", line->count, lap);
    for (int n = 0; n < line->count; n++) {
        const LineNode &nd = line->node[n];
        pclog::write("racing line: node %d kind %d x %.3f y %.3f h %.3f dist %.3f radius %.3f angle %.4f sector %d", n,
                     nd.kind, nd.x, nd.y, nd.height, nd.dist, nd.radius, nd.angle, nd.sector);
    }
}

const char *miss_text(int miss, int taken)
{
    return miss == 2 ? " miss" : miss == 1 ? (taken ? " far" : " far-kept") : "";
}

void log_frame(RacePS2Base *race, CameraManager *cm, RaceCarState *car, CourseLines *cl, ViewState *vs,
               f32 lap, f32 dist)
{
    const f32 vx = car_view_x(car, 0.0f), vy = car_view_y(car, 0.0f), vz = car_view_z(car, 0.0f);
    f32 ax = 0.0f, ay = 0.0f, ah = 0.0f;    /* the line at the car's own distance (lines' space) */
    const int atNode = line_point_at(cl, kLineRacing, vs->hint, dist, &ax, &ay, &ah);
    const f32 off = sqrtf((ax - vx) * (ax - vx) + (-ay - vz) * (-ay - vz));
    int sector = -1, missCar = 0;
    const f32 carRoad = road_height(cl, atNode >= 0 ? atNode : g_samples[0].node, &sector, vx, -vz, vy, &missCar);
    const Sample &s0 = g_samples[0];
    const f32 d0 = sqrtf((s0.centre[0] - vx) * (s0.centre[0] - vx) + (s0.centre[2] - vz) * (s0.centre[2] - vz));
    pclog::write("racing line: frame %u view %d car %d mode %d lap %.3f dist %.3f view %.3f %.3f %.3f "
                 "ground %.3f carroad %.3f%s atcar %.3f %.3f %.3f off %.3f s0 %.3f %.3f %.3f d0 %.3f",
                 vs->frame, cm->current, (int)cm->views[cm->current].targetInfo.index[0], race->getReplayMode(), lap,
                 dist, vx, vy, vz, car->viewGroundPos[1], carRoad, miss_text(missCar, 1), ax, ah, -ay, off,
                 s0.centre[0], s0.centre[1], s0.centre[2], d0);
    for (int i = 0; i < kSamples; i++) {
        const Sample &s = g_samples[i];
        pclog::write("racing line: sample %u %d d %.3f p %.3f %.3f %.3f%s line %.3f l %.3f %.3f %.3f r %.3f %.3f %.3f",
                     vs->frame, i, s.dist, s.centre[0], s.centre[1], s.centre[2], miss_text(s.miss, s.taken), s.lineHeight,
                     s.left[0], s.left[1], s.left[2], s.right[0], s.right[1], s.right[2]);
    }
}

/* [Debug] RacingLineForce=on: the strip drawn (read once, at the first
 * frame; a stand-in until the strip has its own setting). */
int force_on()
{
    static int on = -1;
    if (on < 0) {
        char v[16];
        GetPrivateProfileStringA("Debug", "RacingLineForce", "off", v, sizeof(v), paths::ini_path().c_str());
        on = strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0;
        if (on)
            pclog::write("racing line: drawn ([Debug] RacingLineForce=on)");
    }
    return on;
}

/* The strip is drawn in this view: a live race (not a replay, nor a demo:
 * the attract demo, the race behind a menu), no race preview (the licence's
 * demonstration), not the logger, not the debug camera's view. */
int shown(RacePS2Base *race, CameraManager *cm)
{
    const RaceOrganization &org = race->getOrganization();
    return race->getReplayMode() == RACE_LIVE && org.backgroundMode == 0 && org.racePreviewMode == 0 &&
           org.loggerMode == 0 && !pc_debugcam_owns(cm->current);
}

/* The strip: the samples' left and right vertices in turn (a triangle
 * strip), in buffers of gfx_alloc_tail (read when the frame is drawn),
 * cut into strips of kStripVerts at most, each starting on the last pair of
 * the one before.  Its alpha rises over the first kFadeIn samples and falls
 * over the last kFadeOut.  Drawn under the view's matrix, as the cars:
 * depth tested but not written (the cars and the course behind keep their
 * depth), blended by its alpha into the colour alone (the frame's alpha,
 * which the cars left for the effects after them, is kept), no texture, no
 * culling, the fog as the course left it; the state is put back after. */
void draw_strip()
{
    constexpr int n = kSamples * 2;
    PrimVertex *xyz = (PrimVertex *)gfx_alloc_tail((n * (int)sizeof(PrimVertex) + 15) / 16);
    u32 *rgba = (u32 *)gfx_alloc_tail((n * 4 + 15) / 16);
    for (int i = 0; i < kSamples; i++) {
        const Sample &s = g_samples[i];
        f32 f = 1.0f;
        if (i < kFadeIn)
            f = (f32)(i + 1) / (f32)(kFadeIn + 1);
        if (kSamples - 1 - i < kFadeOut)
            f = fminf(f, (f32)(kSamples - 1 - i) / (f32)kFadeOut);
        const u32 a = (u32)((f32)kAlpha * f + 0.5f);
        const u32 c = kRed | kGreen << 8 | kBlue << 16 | a << 24;
        xyz[2 * i] = {s.left[0], s.left[1], s.left[2]};
        xyz[2 * i + 1] = {s.right[0], s.right[1], s.right[2]};
        rgba[2 * i] = c;
        rgba[2 * i + 1] = c;
    }

    /* the state before */
    const GsContext &ctx = g_pglCtx;
    const int wasTexture = (ctx.primAttr & kPrimTme) != 0, wasBlend = (ctx.primAttr & kPrimAbe) != 0;
    const int wasAlphaTest = (ctx.reg[GS_REG_TEST] & kTestAte) != 0;
    const int wasDestAlpha = (ctx.reg[GS_REG_TEST] & kTestDate) != 0;
    const int wasDepthTest = ctx.depthTest, wasDepthWrite = ctx.zMask == 0, wasCull = ctx.cullEnable;
    const u8 blendAbcd = (u8)(ctx.reg[GS_REG_ALPHA] & 0xFF), blendFix = (u8)(ctx.reg[GS_REG_ALPHA] >> 32);
    const u32 writeMask = ~(u32)(ctx.reg[GS_REG_FRAME] >> 32);

    gfx_enable(PGL_CAP_DEPTH_TEST);
    gfx_depth_mask(0);
    gfx_disable(PGL_CAP_TEXTURE);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    gfx_disable(PGL_CAP_DEST_ALPHA);
    gfx_disable(PGL_CAP_CULL_FACE);
    gfx_enable(PGL_CAP_BLEND);
    gfx_blend_func(PGL_BLEND_ALPHA, 1.0f);
    gfx_set_frame_write_mask(0x00FFFFFFu);  /* (FRAME_WRITE_RGB) */
    strip_batch_begin();
    for (int first = 0; first + 2 < n; first += kStripVerts - 2) {
        const int count = n - first < kStripVerts ? n - first : kStripVerts;
        strip_draw(&xyz[first], NULL, &rgba[first], NULL, count);
    }
    strip_batch_end();

    /* back as it was */
    gfx_set_frame_write_mask(writeMask);
    gfx_blend_func_raw(blendAbcd, blendFix);
    if (!wasBlend)
        gfx_disable(PGL_CAP_BLEND);
    if (wasCull)
        gfx_enable(PGL_CAP_CULL_FACE);
    if (wasDestAlpha)
        gfx_enable(PGL_CAP_DEST_ALPHA);
    if (wasAlphaTest)
        gfx_enable(PGL_CAP_ALPHA_TEST);
    if (wasTexture)
        gfx_enable(PGL_CAP_TEXTURE);
    gfx_depth_mask(wasDepthWrite ? 1 : 0);
    if (!wasDepthTest)
        gfx_disable(PGL_CAP_DEPTH_TEST);
}

} // namespace

void pc_racing_line_draw(RacePS2Base *race, CameraManager *cm)
{
    const int logged = settings::debug_racing_line_log();
    const int drawn = force_on() && shown(race, cm);
    if (!logged && !drawn)
        return;
    if (cm->current < 0 || cm->current >= kViews)
        return;
    RaceEntryCar *entry = cm->views[cm->current].targetInfo.car[0];
    if (entry == NULL || entry->state == NULL)
        return;
    RaceCarState *car = entry->state;
    CourseLines *cl = &car->dc->lines;
    if (cl->course == NULL)
        return;
    const f32 lap = cl->course->getLapLength();
    if (cl->data == NULL || cl->data->line[kLineRacing] == NULL) {
        if (first_for_course(cl->data, lap) && logged)
            pclog::write("racing line: no line 6 on this course (lap %.3f): no strip", lap);
        return;
    }
    if (!(lap > 0.0f))
        return;
    ViewState *vs = &g_views[cm->current];
    if (first_for_course(cl->data, lap) && logged)
        log_nodes(cl, lap);

    /* the points: the car's lap distance (as drawCars, t 0) and on.  The
     * height is the road's under each point: the line's own, interpolated
     * between its nodes, leaves the road where the nodes are far apart (a
     * metre under it on a banked turn of the speedway, metres on the hills
     * of a dirt course).  A road found only from every sector (the highest
     * under the point: a bridge over it would do) is taken near the height
     * of the point before (the first point: of the last frame's first);
     * else, and off the road, the point before's height (none: the
     * line's own) */
    const f32 dist = car_lap_distance(car, 0.0f);
    f32 moved = fabsf(dist - vs->dist);
    if (moved > lap * 0.5f)
        moved = lap - moved;
    if (vs->car != car || moved > kJump) {  /* another car, or a cut: no point before */
        vs->sector = -1;
        vs->hasFirst = 0;
    }
    vs->car = car;
    vs->dist = dist;
    int hint = vs->hint;
    int sector = vs->sector;
    for (int i = 0; i < kSamples; i++) {
        f32 d = dist + kFirst + kStep * (f32)i;
        while (d >= lap)
            d -= lap;
        while (d < 0.0f)
            d += lap;
        f32 x = 0.0f, y = 0.0f, h = 0.0f;
        const int node = line_point_at(cl, kLineRacing, hint, d, &x, &y, &h);
        if (node < 0)
            return;                         /* (no segment: the line's nodes do not cover the lap) */
        hint = node;
        if (i == 0)
            vs->hint = node;
        Sample &s = g_samples[i];
        s.dist = d;
        s.node = node;
        s.lineHeight = h;
        int found = sector;
        const f32 road = road_height(cl, node, &found, x, y, h, &s.miss);
        const int hasRef = i > 0 || vs->hasFirst;
        const f32 ref = i > 0 ? g_samples[i - 1].centre[1] : vs->firstHeight;
        const int take = s.miss == 0 || (s.miss == 1 && (!hasRef || fabsf(road - ref) < kFarRoad));
        if (take)
            sector = found;
        s.taken = take;
        s.centre[0] = x;
        s.centre[1] = take ? road : hasRef ? ref : h;
        s.centre[2] = -y;
        if (i == 0) {
            vs->sector = take ? found : -1;
            vs->hasFirst = 1;
            vs->firstHeight = s.centre[1];
        }
    }

    /* the two sides: perpendicular to the way the strip runs at each point
     * (towards the next point; the last towards it from the one before) */
    for (int i = 0; i < kSamples; i++) {
        const Sample &a = g_samples[i < kSamples - 1 ? i : i - 1];
        const Sample &b = g_samples[i < kSamples - 1 ? i + 1 : i];
        f32 fx = b.centre[0] - a.centre[0], fz = b.centre[2] - a.centre[2];
        const f32 len = sqrtf(fx * fx + fz * fz);
        if (len > 1e-4f) {
            fx /= len;
            fz /= len;
        } else {
            fx = 0.0f;
            fz = 1.0f;
        }
        Sample &s = g_samples[i];
        const f32 sx = -fz * kHalfWidth, sz = fx * kHalfWidth;
        s.left[0] = s.centre[0] + sx;
        s.left[1] = s.centre[1] + kLift;
        s.left[2] = s.centre[2] + sz;
        s.right[0] = s.centre[0] - sx;
        s.right[1] = s.centre[1] + kLift;
        s.right[2] = s.centre[2] - sz;
    }

    if (drawn)
        draw_strip();
    if (vs->frame++ % kLogEvery == 0 && logged)
        log_frame(race, cm, car, cl, vs, lap, dist);
}

#endif /* !_WIN32 */
