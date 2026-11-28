/* T131 - RacePS2Base, the drawing of a race view (u045).
 *
 * render (GameObject slot 2) runs drawView (the 3D view), virtual_45 (the
 * 2D overlays, empty here: RaceBasic draws them) and virtual_46 (the error
 * view).  drawView draws a full-screen view; drawSubView a view into a
 * sub-rectangle of the target (the split screen).  Both
 *   - render the dynamic environment map of the scene (envmapUpdate),
 *   - draw the course (the vision lists of CourseData), the reflections on
 *     the road (drawReflections: drawCarReflections / drawLampReflections into a small buffer, blurred and
 *     blended over the screen), the cars (drawCars, farthest first), the
 *     rear-view mirror (drawMirror), the tyre smoke and the lens flare
 *     (FlareClass at +0x9830, lights from addCarLamps / addCourseLights),
 *   - copy the result into the target;
 * drawView also does the replay depth-of-field blur (ReplayBlur at
 * +0x97CC), the rain, the colour correction, the vignette (PC: not), the afterimage
 * (captureAfterimage / drawAfterimage: the frame saved into three VRAM
 * planes and drawn over the next frames with a fading alpha) and the
 * camera's frame effect (the previous frame mixed in).
 *
 * Includes (u045's data pass): the classes retail's T131 saw, in its order.
 * Its .rodata (0x003280C8..0x003282E8) is "bad_alloc" (<new>), the error
 * view's message, then the type names in reverse definition order:
 * OperatorList, List / OperatorHolder, OperatorHolder, Cell / OperatorHolder,
 * RaceCameraBase, the GameStatus's garage lists, UnitArena<64>, and Operator
 * (the non-polymorphic base, after all the others) - T130's set without
 * T130's own three classes, whose key functions are there.  So UnitArena<64>
 * is instantiated first (T153), the GameStatus (T429) comes before RaceBase's
 * RaceCameraBase, and the menu operator classes (T216) after T130's header.
 * No race display (T154 / T157: neither their names nor the "" of
 * RaceMessageDisplay::clear in .sdata are in retail's T131).
 */
// DATA_START 0x003280C8
#include "types.h"
#include <stddef.h>                         /* size_t, NULL */
#include <new>                              /* (no use: "bad_alloc", the first .rodata string) */
#include "libgraph.h"                       /* SCE_GS_PSM* */
#define GAME_T153_EMITS_TYPEINFO            /* UnitArena<64>'s type name */
#include "game/race/core/T153_race_course.h"          /* RaceCourse::data */
#include "game/save/T429_game_status.h"          /* (no use: the garage classes retail's T131 saw) */
#include "game/camera/T383_race_camera_base.h"     /* (RaceCameraBase, through RaceBase) */
#include "game/race/core/T130_race_ps2_base.h"        /* RacePS2Base (render, slots 44..55, captureAfterimage), MirrorView;
                                               + T121 RaceEntryBase / RaceEntryCar, T129 RaceBase, T159
                                               FlareClass, T173 RaceSmoke, T165 ParticleManager, T167
                                               RigidBodyManager, T182 ReplayBlur, T185 ErrorEventView */
#include "game/menu/framework/T216_menu_transitive.h"      /* (no use: the menu operator classes retail's T131 saw) */
#include "game/race/core/T131_race_ps2_render.h"      /* own: Matrix44, race_car_in_view, clear_frame_alpha / write_alpha_depth / F_00180EB0 */
#include "game/app/T002_render_thread.h"        /* GameObject */
#include "engine/gfx/T514_gs_buffer.h"            /* GSBuffer */
#include "engine/gfx/T512_render_context.h"       /* RenderContext g_002F4230, GSBufferPair */
#include "game/camera/T383_camera_manager.h"       /* CameraManager, CameraView (CameraSet, T388) */
#include "game/camera/T410_camera_base.h"          /* CameraBase */
#include "game/effects/T277_dynamic_envmap_camera.h"    /* DynamicEnvmapCamera::followCar */
#include "game/effects/T276_envmap_render.h"        /* envmapUpdate */
#include "game/race/core/T121_race_entry.h"           /* RaceEntryCar (draw wrappers), RaceEntryBase::sortByDistance,
                                               F_00179460 */
#include "game/race/core/T122_race_event_queue.h"     /* RaceEventQueue::post */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization (entries, course, loggerMode, eventQueue) */
#include "game/course/T179_course_data.h"          /* CourseData, course_draw_model .. course_submit_flares */
#include "game/course/T178_course_env.h"           /* CourseEnv, CourseFog, course_env_apply .. course_env_set_reflection */
#include "game/course/T457_runway.h"               /* Runway::hasLights */
#include "game/car/model/T180_race_car_model.h"       /* RaceCarModel::getCarModel */
#include "game/car/model/T446_car_model.h"            /* CarModel::callback (lod, lodDist) */
#include "game/car/model/T181_car_lighting.h"         /* CarRenderCtx */
#include "game/car/model/T184_car_icon_maker.h"       /* GsTexPacket */
#include "game/car/physics/T289_car_mode.h"             /* car_display_level, CAR_SHOW_* */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, car_view_x / y / z */
#include "game/sound/T288_car_sound.h"            /* DynCarState::noController (RaceCarState::st) */
#include "game/car/model/T449_race_tire_model.h"      /* brake_textures_link / _unlink */
#include "engine/gfx/T451_model_set.h"            /* modelset_set_* */
#include "game/course/T453_vision_list.h"          /* VisionList, VisionPoint, vision_nearest */
#include "engine/text/T496_font.h"                 /* Font g_002F1968 */
#include "engine/pgl/G001_flare.h"                /* gfx_*, gs_*, pgl_*, vu0_*, pfx_*, weather_draw, box_visible */
#include "game/race/core/race_ps2_render_internal.h"                           /* the unit's constants */
#include "game/enhanced.h"                  /* ENH_RACE_AFTERIMAGE */
#include "game/app/pc_frame.h"                  /* PC: the dynamic frame rate */
#include "game/app/pc_aspect.h"                 /* PC: the dynamic aspect */
#include "game/app/pc_detail.h"                 /* PC: the levels of detail, the render distance */
#include "game/app/pc_course_scale.h"           /* PC: the course scale (the mirror's far clip) */
#include "game/app/pc_headcam.h"                /* PC: the cockpit camera's own car (drawCars) */
#include "game/app/pc_customcam.h"              /* PC: a custom camera's own car (drawCars) */
#ifndef _WIN32
#include "game/app/pc_racing_line.h"            /* PC: the racing line on the road (drawView, drawSubView) */
#endif
#include "game/app/pc_debugcam.h"               /* PC: the debug camera */

/* GSBufferPair's current / other buffer.  Retail takes the pair's address
 * (g_002F4230+0xAC4 / +0xA50, kept in a register) and reads cur at +0x70
 * from it: the form of an inline accessor (the original's were most likely
 * GSBufferPair members).  Local here, not in T512_render_context.h: an
 * inline function in a widely included header shifts egcs's label numbers,
 * which changes other units' .gcc_except_table (u008's notes; u054's T157
 * gets four more regions). */
static inline GSBuffer *pair_current(GSBufferPair *p) { return &p->buf[p->cur]; }
static inline GSBuffer *pair_other(GSBufferPair *p) { return &p->buf[1 - p->cur]; }

/* The render context's fields as retail reads them: through &g_002F4230 in
 * a register (lui / addiu, then lw 0x18(reg) ...), the form of inline
 * accessors taking the context (u021's cars_projection has the same wide
 * test).  Local for the reason above. */
static inline int rc_field_mode(RenderContext *rc) { return rc->unk18; }
static inline f32 rc_field_blend(RenderContext *rc) { return rc->unk14; }
/* The y scale of every race view's projection, x * 0.75 on a 16:9 screen.
 * PC: the dynamic aspect (game/app/pc_aspect.h): squeezed by kx / ky for the
 * window's aspect (the 16:9 option is dormant: rc->wide stays 0). */
static inline void proj_y_scale(RenderContext *rc)
{
    F_00257EF0(PROJ_Y_SCALE);
    (void)rc;
    if (pc_aspect_kx() != 1.0f || pc_aspect_ky() != 1.0f)
        vu0_scale(pc_aspect_kx(), pc_aspect_ky(), 1.0f);
}

/* PC: the dynamic frame rate (game/app/pc_frame.h).  A race view's camera
 * blend is kept per screen - the camera manager's current view (the split
 * screen draws both through one manager, setCurrentView between them) - and
 * its shot is the view's camera and mount: the view button (bumper / chase),
 * a replay's camera change cut, not blended. */
static const void *pc_view_key(CameraManager *cm)
{
    return cm->F_00209BE0();
}

static u32 pc_view_shot(CameraManager *cm)
{
    VariableCameraBase *cam = cm->F_00209BE0()->camera;
    if (cam == NULL)
        return 0;
    /* PC (2026-11-14): a custom camera is a shot of its own (its mount is
     * the bumper's or the chase view's: game/app/pc_customcam.h) */
    OnBoardCamera *custom = cm->pcCustom();
    return (u32)(size_t)cam * 31u + (u32)cam->getMount() + (custom != NULL ? (u32)custom->pcCustom * 256u : 0u);
}

/* min.s / max.s: egcs's <? / >? operators (T131 has no -ffast-math, under
 * which a ternary would give them); GCC 14 has none. */
#define MINF(a, b) ((a) < (b) ? (a) : (b))
#define MAXF(a, b) ((a) > (b) ? (a) : (b))

/* PC: the dynamic frame rate (game/app/pc_frame.h).  A race view drawn
 * between two ticks shows the models' animations at the shown time: the
 * course's animation tracks (course_pose_anims) and every car's body
 * (RaceCarModel::pcPoseBegin: the wheel shake and steering switches, the
 * lamp blinks, the tracks, the wing and the Pod's morphs), dt x the lag back
 * from the tick's state, which is put back once the view is drawn.  Only
 * what stepped in the last tick moves (pc_anim_offset); the palettes at the
 * frame's end (pc_pose_palettes).  One pose at a time (the views are not
 * nested). */
enum { PC_POSE_CARS = 64 };
static RaceCarModel::PcPose s_pcPoseSaved[PC_POSE_CARS];
static int s_pcPosing;

/* The palettes (CLUT animations) at the shown time: the renderer reads them
 * when the frame's lists execute, after the race's draw and after its
 * post-draw slot applied the tick's (virtual_3), so they are written at the
 * frame's end (pc_frame_at_end, registered by every race view): the
 * course's (every animation, as course_step_anims(-1) steps them) and each
 * car's (RaceCarModel::pcPosePalettes).  Once the frames are no longer
 * drawn between ticks (paused, a menu), the state's colours are written
 * back once. */
static int s_pcPalettesPosed;

static void pc_pose_palettes(void *arg)
{
    RaceOrganization *org = (RaceOrganization *)arg;
    const int pose = g_pcViewLag > 0.0f;

    if (!pose && !s_pcPalettesPosed)
        return;
    CourseData *cd = org->course->data;
    const f32 ofs = pose ? pc_anim_offset(cd) : 0.0f;
    if (!pose || ofs != 0.0f)
        course_pose_clut_anims(cd, ofs);
    RaceEntryBase *entries = org->entries;
    for (int i = 0; i < entries->count; i++)
        entries->cars[i]->model.pcPosePalettes(pose);
    s_pcPalettesPosed = pose;
}

class PcAnimPose {
public:
    explicit PcAnimPose(RaceOrganization *o) : org(o), courseOfs(0.0f), cars(0), active(0)
    {
        pc_frame_at_end(pc_pose_palettes, org);
        if (g_pcViewLag <= 0.0f || s_pcPosing)
            return;
        s_pcPosing = active = 1;
        CourseData *cd = org->course->data;
        courseOfs = pc_anim_offset(cd);
        if (courseOfs != 0.0f)
            course_pose_anims(cd, courseOfs);
        RaceEntryBase *entries = org->entries;
        cars = entries->count < PC_POSE_CARS ? entries->count : PC_POSE_CARS;
        for (int i = 0; i < cars; i++)
            entries->cars[i]->model.pcPoseBegin(&s_pcPoseSaved[i]);
    }
    ~PcAnimPose()
    {
        if (!active)
            return;
        if (courseOfs != 0.0f)
            course_pose_anims(org->course->data, 0.0f);
        for (int i = 0; i < cars; i++)
            org->entries->cars[i]->model.pcPoseEnd(&s_pcPoseSaved[i]);
        s_pcPosing = 0;
    }

private:
    RaceOrganization *org;
    f32 courseOfs;
    int cars;
    int active;
};

// FUNC 0x0017E6C0 RacePS2Base::render
//   GameObject::render: while renderEnabled, the 3D view, the 2D overlays and
//   the error view.
void RacePS2Base::render(GSBuffer &buf)
{
    if (renderEnabled != 0) {
        drawView(&buf);
        if (!pc_debugcam_on())              /* PC: no HUD under the debug camera (game/app/pc_debugcam.h) */
            virtual_45(&buf);
        virtual_46(&buf);
    }
}

static inline void buf_set_psm(GSBuffer *b, int psm) { b->psm = psm; }
static inline void blur_set_fnumber(ReplayBlur *b, f32 f) { b->fNumber = f; }
static inline void blur_set_focus(ReplayBlur *b, f32 f) { b->focus = f; }
// FUNC 0x0017E740 RacePS2Base::drawView
//   Draws the full-screen race view into buf.  screen: a colour buffer the
//   size of the render context's current Z buffer (g_002F4230.unkAC4, which
//   is the depth buffer of the view); envmap: the 256 x 256 dynamic
//   environment map.
//   Matching (99.1%): left is the frame effect's min.s, which retail
//   computes straight into $f12 (ours: min.s $f0 then mov.s $f12; the min
//   is tied to the call result).  Tried: <? with the call / a variable /
//   swapped operands / an inline min helper / a float-first prototype of
//   gfx_frame_effect; the reference compiler gives the same.
void RacePS2Base::drawView(GSBuffer *buf)
{
    pc_aspect_race_frame();                 /* PC: the picture at the window's aspect */
    gfx_gpu_section(GFX_SEC_SCENE);         /* PC: GPU time by section (GT3PC_GPU_SECTIONS) */
    PcAnimPose pose(org);                   /* PC: the animations at the shown time */
    CourseData *course = org->course->data;
    CourseEnv *env = course->env;
    CameraManager *cm = virtual_6();
    pc_debugcam_ground(course->runway);     /* PC: before any pass's view (game/app/pc_debugcam.h) */

    GSBuffer screen(pair_current(&g_002F4230.unkAC4), SCE_GS_PSMCT32);   /* sp+0x00 */
    screen.setField(rc_field_mode(&g_002F4230) != 0 ? PGL_FIELD_FRAME_SHIFT : PGL_FIELD_FRAME);
    modelset_set_tex_lod_size(env->texLodSize);
    GSBuffer envmap(SCE_GS_PSMCT16, 0x100, 0x100);                         /* sp+0x40 */
    envmapUpdate(&envmap, course, cm, screen.addr, 0.0f);

    /* the projection and the view */
    screen.setDrawBuffer();
    pair_current(&g_002F4230.unkAC4)->setZBuffer();
    gs_set_clear_color(0);
    gs_clear_buffers(PGL_CLEAR_COLOR | PGL_CLEAR_DEPTH);
    /* PC: the camera of the tick (some cameras read the cars' interpolated
     * positions as they apply their view), then blended between the last two
     * ticks between them (the dynamic frame rate, game/app/pc_frame.h) */
    const f32 lag = pc_frame_lag();         /* (the frame's: a cut sets g_pcViewLag to 0, pc_view_interpolate) */
    g_pcViewLag = 0.0f;
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    vu0_load_identity_regs();
    cm->setFarClip(env->farClip * pc_render_scale());  /* PC: the render distance (game/app/pc_detail.h) */
    cm->applyProjection();
    proj_y_scale(&g_002F4230);             /* implicit: F_00257EF0, vu0_scale (the helper's) */
    /* PC: the field of view settings (game/app/pc_detail.h; PC 2026-11-09:
     * not the cockpit camera's view, its own) */
    pc_fov_apply(replayMode != RACE_LIVE, (cm->pcCockpit() != 0 && pc_headcam_on()) || cm->pcCustom() != NULL);
    pc_debugcam_projection(cm->current);    /* PC: the debug camera's (game/app/pc_debugcam.h) */
    modelset_set_projection_scale(1.0f);
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
    vu0_load_identity_regs();
    cm->applyView();
    g_pcViewLag = lag;
    if (!pc_debugcam_owns(cm->current))     /* PC (the debug camera moves every frame itself) */
        pc_view_interpolate(pc_view_key(cm), pc_view_shot(cm));

    /* the course */
    course_env_apply(env);
    course_draw_model(course, course->model54);
    course_env_draw_sun_disc(env, &flare);
    F_001A9240(course);
    gs_clear_buffers(PGL_CLEAR_DEPTH);
    write_alpha_depth(&screen);
    drawCourse();
    if (course->model24 != NULL)                    /* the course has road reflections */
        drawReflections(&screen, &envmap);
    if (org->loggerMode == 0) {
        RigidBodyManager::SetLightVector((const Vec3 *)env->lightB[0].pos);
        rigidBodies.draw();
    }
    clear_frame_alpha(0x80000000);

    /* the cars: LOD by distance, the tyres' motion blur */
    drawCars(&envmap, -1, 1, 0.0f);
#ifndef _WIN32
    pc_racing_line_draw(this, cm);          /* PC: the racing line on the road (game/app/pc_racing_line.h) */
#endif

    /* the rear-view mirror: live race, a camera that shows it, the centre
     * screen of an i.LINK wall (isCenterScreen) */
    if (replayMode == RACE_LIVE) {
        if (cm->isDrawBackMirror() != 0) {
            if (cm->isCenterScreen() != 0) {
                setMirrorRect(320, 52, 240, 64, 1.3f);
                drawMirror(&screen, 0);
            }
        }
    }

    /* the lens flare: the sun, the lamps, the course's lights; the sparks */
    gfx_gpu_section(GFX_SEC_EFFECTS);       /* PC */
    flare.setNoOcclusionTest(0);
    flare.begin(screen);
    course_env_set_sun(env, &flare);
    if (env->flags & ENV_LAMPS)
        addCarLamps(0.0f);
    if (env->flags & ENV_COURSE_LIGHTS)
        addCourseLights();
    flare.flush();
    particles.draw();
    gfx_post_antialias(screen.addr, screen.width, screen.height);  /* PC: FXAA, when chosen, before the 2D */

    /* into the target; depth: a copy of the view's Z buffer like buf */
    gfx_gpu_section(GFX_SEC_COPY);          /* PC */
    GSBuffer depth(buf, SCE_GS_PSMZ24);                                     /* sp+0x80 */
    depth.blit(pair_current(&g_002F4230.unkAC4), PGL_FILTER_NEAREST, PGL_FILTER_NEAREST);
    /* PC (2026-11-07): [Graphics] ReplayDepthOfField (game/app/pc_detail.h):
     * the steps per doubling of the blur behind the focus (the original's 2),
     * 0 for no depth of field - the plain copy below, as a live race. */
    const int blurSteps = pc_replay_blur_steps();
    if (replayMode != RACE_LIVE && (((const u8 *)&env->flags)[1] & 1) && blurSteps > 0) {    /* lbu +0x11: ENV_REPLAY_BLUR */
        gfx_gpu_section(GFX_SEC_REPLAY_BLUR);   /* PC */
        buf->setDrawBuffer();
        gfx_enable(PGL_CAP_DEPTH_TEST);
        gfx_disable(PGL_CAP_BLEND);
        /* implicit: GSBuffer::copy (the inline copy constructor) */
        GSBuffer work(g_002F4230.bufB38);   /* sp+0xC0 the blur's levels, in the 640 x 224 Z buffer's VRAM */
        buf_set_psm(&work, SCE_GS_PSMCT24);
        blur.setFilm(env->blurFilmDiag, 1.333f);
        blur_set_fnumber(&blur, env->blurFNumber);
        blur.farSteps = blurSteps;          /* PC (2026-11-07): the setting's steps (the constructor's 2 otherwise) */
        f32 targetDist = cm->virtual_10();  /* the camera's distance to its target */
        blur_set_focus(&blur, (targetDist < env->blurFocusMin) ? env->blurFocusMin : targetDist);
        blur.setFromProjection();
        blur.draw(buf, &screen, &depth, &work, rc_field_mode(&g_002F4230), rc_field_blend(&g_002F4230));
    } else {
        gfx_disable(PGL_CAP_DEPTH_TEST);
        buf->blitField(&screen, rc_field_mode(&g_002F4230), rc_field_blend(&g_002F4230));
    }
    screen.freeVram();
    buf->setDrawBuffer();
    depth.setZBuffer();

    /* the rain, around the eye (no translation) */
    pgl_push_matrix();
    vu0_reset_translation();
    int effects = cm->virtual_11();
    if ((env->flags & ENV_WEATHER) && (effects & CAM_EFFECT_WEATHER)) {
        gfx_gpu_section(GFX_SEC_WEATHER);   /* PC */
        gfx_set_packet_access(PGL_PKT_UNCACHED_ACCEL);
        weather_draw(weather, buf->addr);   /* addr: not read by weather_draw */
        gfx_set_packet_access(PGL_PKT_CACHED);
    }
    pgl_pop_matrix();
    gfx_gpu_section(GFX_SEC_SMOKE);         /* PC */
    smoke.render(&depth, 0);
    gfx_gpu_section(GFX_SEC_FLARE);         /* PC */
    flare.draw();
    gfx_gpu_section(GFX_SEC_POST);          /* PC */

    /* the colour correction: in replays when the camera overrides the
     * course's levels or colour, else when the course has it (lbu +0x10,
     * >> 7: ENV_COLOUR_CORRECT) */
    if (replayMode == RACE_LIVE ? (env->flags >> 7) & 1 : cm->F_0020A038()) {
        f32 cc[6];                          /* sp+0xC0 lo, hi, r, g, b, saturation */
        cm->getColourCorrection(env, cc);
        GSBuffer work(SCE_GS_PSMCT32, 0x80, 0x20);                          /* sp+0x100 the filter's work VRAM */
        buf->setDrawBuffer();
        buf->setTexture();
        pgl_color(0x808080);
        pfx_color_correction(cc[2], cc[3], cc[4], cc[0], cc[1], cc[5], work.addr);
    }
    gfx_disable(PGL_CAP_DEPTH_TEST);
    /* PC: no vignette (the replays' ring, T183, when the course has
     * ENV_VIGNETTE: removed at the user's request, 2026-10-20) */
    buf->setDrawBuffer();
    g_002F1968.setStyle(NULL);
    cm->begin2D(org, buf);                  /* not read */

    /* the afterimage: captured on the first frame (0.5 s) and whenever the
     * camera cross-fades */
    if (frameBits == 0) {
        afterimageTime = 0.5f;
        captureAfterimage(buf);
    } else if (cm->crossFade != 0 && g_pcTickFrame) {     /* PC: once, in the tick's frame */
        afterimageTime = cm->crossFadeTime;
        captureAfterimage(buf);
    }
    drawAfterimage(buf);

    /* the camera's frame effect: buf (target B) = lerp(buf, the other frame
     * buffer (the draw target A), level), the colour only */
    if (cm->frameEffect != 0) {
        buf->setTargetB();
        pair_other(&g_002F4230.frame)->setDrawBuffer();
        gfx_set_frame_write_mask(0x00FFFFFF);
        gfx_depth_mask(0);
        f32 level = cm->F_00209498();
        gfx_frame_effect(PGL_FX_MIX_A_INTO_B, MINF(level, 0.8f));
        gfx_set_frame_write_mask(0xFFFFFFFF);
        gfx_depth_mask(1);
    }
    gfx_disable(PGL_CAP_CULL_FACE);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    gfx_disable(PGL_CAP_DEST_ALPHA);
    gfx_disable(PGL_CAP_BLEND);
    frameBits = (u8)((frameBits << 1) | 1);
    gfx_gpu_section(GFX_SEC_OTHER);         /* PC */
}

// FUNC 0x0017EFC8 RacePS2Base::virtual_45
//   Empty: the 2D overlays of the derived modes (RaceBasic::virtual_45).
void RacePS2Base::virtual_45(GSBuffer *buf)
{
}

// FUNC 0x0017EFD0 RacePS2Base::virtual_46
//   Posts the error event when the course has no vision list, then draws the
//   error view.
void RacePS2Base::virtual_46(GSBuffer *buf)
{
    RaceOrganization *o = org;
    if (o->course->data->visionList == NULL)
        o->eventQueue.post(RACE_EVENT_ERROR, (int)(size_t)"VisionList is invalid or empty", 0);
    errorView.draw(buf);
}

// FUNC 0x0017F040 RacePS2Base::drawSubView
//   Draws the race view into the rectangle of buf (rectW x rectH; the split
//   screen views): a colour buffer of its own, the depth in the 640 x 224 Z
//   buffer's VRAM.  sub != 0: the second view (half the far clip, the
//   course's sub vision list, a blank environment map if the course has
//   ENV_SUB_NO_ENVMAP, no rigid bodies, the mirror with argument 1).  zoom
//   scales the projection.  No sparks, rain or screen effects.
//   Matching (99.6%): left is the order of three loads before
//   depth.setField (retail rectW, rectH, then rc->unk18).  Tried the six
//   orders of the depth set-up, a set-up helper, the field mode in a
//   variable.
void RacePS2Base::drawSubView(GSBuffer *buf, int sub, f32 zoom)
{
    pc_aspect_race_frame();                 /* PC: the picture at the window's aspect */
    PcAnimPose pose(org);                   /* PC: the animations at the shown time */
    CourseData *course = org->course->data;
    CourseEnv *env = course->env;
    CameraManager *cm = virtual_6();
    pc_debugcam_ground(course->runway);     /* PC: before any pass's view (game/app/pc_debugcam.h) */

    GSBuffer screen;                                                        /* sp+0x00 */
    screen.psm = SCE_GS_PSMCT32;
    screen.width = buf->rectW;
    screen.height = buf->rectH;
    screen.setField(rc_field_mode(&g_002F4230) != 0 ? PGL_FIELD_FRAME_SHIFT : PGL_FIELD_FRAME);
    screen.allocVram();
    GSBuffer depth;                                                         /* sp+0x40 */
    buf_set_psm(&depth, SCE_GS_PSMZ24);
    depth.width = buf->rectW;
    depth.height = buf->rectH;
    depth.setField(rc_field_mode(&g_002F4230) != 0 ? PGL_FIELD_FRAME_SHIFT : PGL_FIELD_FRAME);
    depth.setAddr(g_002F4230.bufB38.addr);
    modelset_set_tex_lod_size(env->texLodSize);
    GSBuffer envmap(SCE_GS_PSMCT16, 0x100, 0x100);                         /* sp+0x80 */
    /* PC: the levels of detail off (game/app/pc_detail.h): the second view
     * as the first, the full course (here and below) */
    const int lodSub = sub != 0 && pc_lod();
    if (!lodSub || !(env->flags & ENV_SUB_NO_ENVMAP)) {
        envmapUpdate(&envmap, course, cm, screen.addr, 0.0f);
    } else {
        envmap.setDrawBuffer();
        gs_set_clear_color(0x80000000);
        gs_clear_buffers(PGL_CLEAR_COLOR);
    }

    /* the projection (the rectangle's aspect, zoom) and the view */
    screen.setDrawBuffer();
    depth.setZBuffer();
    gs_set_clear_color(0);
    gs_clear_buffers(PGL_CLEAR_COLOR | PGL_CLEAR_DEPTH);
    const f32 lag = pc_frame_lag();         /* PC: the tick's camera (drawView); the frame's lag after it */
    g_pcViewLag = 0.0f;
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    vu0_load_identity_regs();
    cm->setFarClip((lodSub ? env->farClip * 0.5f : env->farClip) * pc_render_scale());   /* PC: the render distance */
    cm->applyProjection();
    if (pc_aspect_race()) {
        /* PC: the view a single player's screen of its own aspect (a split
         * screen's; user request): the 4:3 base squeezed by the view's kx /
         * ky as drawView's by the frame's (game/app/pc_aspect.h
         * pc_view_scales), no zoom */
        f32 vkx, vky, vsx, vsy;
        if (buf->hasRect != 0)
            pc_view_scales((f32)buf->rectW, (f32)buf->rectH, &vkx, &vky, &vsx, &vsy);
        else
            pc_view_scales((f32)buf->width, (f32)buf->displayHeight(), &vkx, &vky, &vsx, &vsy);
        F_00257EF0(PROJ_Y_SCALE);
        vu0_scale(vkx, vky, 1.0f);
    } else {
    proj_y_scale(&g_002F4230);             /* implicit: F_00257EF0, vu0_scale (the helper's) */
    f32 aspect;
    if (buf->hasRect != 0)
        aspect = (f32)buf->rectH / (f32)buf->rectW;
    else
        aspect = (f32)buf->displayHeight() / (f32)buf->width;
    vu0_scale(aspect * PROJ_ASPECT * zoom, zoom, 1.0f);
    }
    /* PC: the field of view settings (game/app/pc_detail.h; PC 2026-11-09:
     * not the cockpit camera's view, its own) */
    pc_fov_apply(replayMode != RACE_LIVE, (cm->pcCockpit() != 0 && pc_headcam_on()) || cm->pcCustom() != NULL);
    pc_debugcam_projection(cm->current);    /* PC: the debug camera's (game/app/pc_debugcam.h) */
    modelset_set_projection_scale(1.0f);
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
    vu0_load_identity_regs();
    cm->applyView();
    g_pcViewLag = lag;
    if (!pc_debugcam_owns(cm->current))     /* PC: blended between ticks (not the debug camera) */
        pc_view_interpolate((const u8 *)pc_view_key(cm) + 1 + sub, pc_view_shot(cm));

    /* the course */
    course_env_apply(env);
    course_draw_model(course, course->model54);
    course_env_draw_sun_disc(env, &flare);
    F_001A9240(course);
    gs_clear_buffers(PGL_CLEAR_DEPTH);
    write_alpha_depth(&screen);
    modelset_set_tex_lod(1);
    modelset_set_tex_set(sub != 0 ? 2 : -1);   /* (the levels of detail off: set 0, T451) */
    if (lodSub)
        course_draw(course, course->visionSub, cm, course->env->visionSub);
    else
        course_draw(course, course->visionList, cm, course->env->visionMain);
    modelset_set_tex_lod(0);
    if (course->model24 != NULL)                    /* the course has road reflections */
        drawReflections(&screen, &envmap);
    clear_frame_alpha(0x80000000);

    /* the cars: the second view with LOD by distance, the first with LOD 0;
     * no tyre blur */
    drawCars(&envmap, sub != 0 ? -1 : 0, sub != 0 && !lodSub ? 1 : 0, 0.0f);   /* PC: the tyre blur with the full detail */
    if ((sub == 0 || !lodSub) && org->loggerMode == 0) {
        RigidBodyManager::SetLightVector((const Vec3 *)env->lightB[0].pos);
        rigidBodies.draw();
    }
#ifndef _WIN32
    pc_racing_line_draw(this, cm);          /* PC: the racing line on the road, the strip of this view's car (game/app/pc_racing_line.h) */
#endif

    /* the rear-view mirror (the rectangle is the one drawView set) */
    if (replayMode == RACE_LIVE) {
        if (cm->isDrawBackMirror() != 0) {
            if (cm->isCenterScreen() != 0)
                drawMirror(&screen, 1);
        }
    }

    /* the lens flare (PC: the levels of detail off, as drawView's: the
     * occlusion test, the lights of every course) */
    flare.setNoOcclusionTest(pc_lod() ? 1 : 0);
    flare.begin(screen);
    course_env_set_sun(env, &flare);
    if (env->flags & ENV_LAMPS) {
        if (!pc_lod() || !(env->flags & ENV_SPLIT_NO_LIGHTS))
            addCarLamps(0.0f);
    }
    if (env->flags & ENV_COURSE_LIGHTS) {
        if (!pc_lod() || !(env->flags & ENV_SPLIT_NO_LIGHTS))
            addCourseLights();
    }
    flare.flush();
    if (sub != 0 && !lodSub)
        particles.draw();                   /* PC: the sparks, as drawView's */
    smoke.render(&depth, pc_lod() ? 1 : 0);
    flare.draw();
    gfx_post_antialias(screen.addr, screen.width, screen.height);  /* PC: FXAA, when chosen, before the 2D */

    gfx_disable(PGL_CAP_DEPTH_TEST);
    buf->blitField(&screen, rc_field_mode(&g_002F4230), rc_field_blend(&g_002F4230));
    gfx_disable(PGL_CAP_CULL_FACE);
    gfx_disable(PGL_CAP_DEPTH_TEST);
}

// FUNC 0x0017F610 RacePS2Base::drawCourse
//   The course through the main vision list (texture sets by LOD).
void RacePS2Base::drawCourse()
{
    CourseData *course = org->course->data;
    CameraManager *cm = virtual_6();
    modelset_set_tex_lod(1);
    modelset_set_tex_set(-1);
    course_draw(course, course->visionList, cm, course->env->visionMain);
    modelset_set_tex_lod(0);
}

// FUNC 0x0017F688 race_car_in_view
//   Whether the 8 m box around the car's interpolated position (t: the
//   interpolation argument of car_view_x / y / z) is in view.
int race_car_in_view(RaceCarState *car, f32 t)
{
    f32 x = car_view_x(car, t);
    f32 y = car_view_y(car, t);
    f32 z = car_view_z(car, t);
    f32 lo[4];                                                              /* sp+0x00 */
    f32 hi[4];                                                              /* sp+0x10 */
    lo[0] = x - 4.0f;
    lo[1] = y - 4.0f;
    lo[2] = z - 4.0f;
    hi[0] = x + 4.0f;
    hi[1] = y + 4.0f;
    hi[2] = z + 4.0f;
    return box_visible(lo, hi) != 0;
}

static inline void model_set_lod_dist(RaceCarModel *m, f32 d) { F_00179460(&m->carModel)->callback.lodDist = d; }

/* PC: the cockpit view (the extra cameras, OnBoardCamera::pcNextView) is the
 * bumper view with the car drawn round the eye - but only below this part of
 * the picture's height.  The body's cabin is flat painted cards, seen through
 * the glass from outside; from the seat they stand between the eye and the
 * road, on many cars across the whole windscreen. */
#define PC_COCKPIT_CUT 0.60f

// FUNC 0x0017F730 RacePS2Base::drawCars
//   Draws the cars, farthest first (sortByDistance from the eye, then
//   sortEnd .. sortPrev): the ones whose model is enabled, that the camera
//   lets through (F_00209370), shown at least as ghosts (a ghost with the
//   scan mask) and in view.  lod: the CarModel LOD for all of them (-1: by
//   distance); lod > 0 draws the wheels (drawTireCylinders), else the tyres with
//   motion blur (drawTires: for the camera's target tyreBlur != 0, for the
//   others 2 when tyreBlur is set).  The camera's target gets half the
//   course's LOD distance.  envBuf: the scene's dynamic environment map
//   (its own VRAM copy when the runway has lights: the glows are drawn
//   into it).  Per car: its lights (CarRenderCtx), the shadow passes
//   (drawShadow per light), the body, the wheels or tyres.
//   RaceEntryCar::drawTires takes (blur, scale, t, lod): retail loads lod
//   last, in the cross-jumped tail of the two calls (u042 revisit).
void RacePS2Base::drawCars(GSBuffer *envBuf, int lod, int tyreBlur, f32 t)
{
    modelset_set_cull_far(1);
    brake_textures_link();
    CourseData *course = org->course->data;
    CourseEnv *env = course->env;
    CameraManager *cm = virtual_6();
    RaceEntryBase *entries = org->entries;
    f32 eyeX = cm->getPositionX();
    f32 eyeY = cm->getPositionY();
    f32 eyeZ = cm->getPositionZ();
    entries->sortByDistance(eyeX, eyeY, eyeZ);

    GsTexPacket envRegs;                                                    /* sp+0x00 */
    GSBuffer envTex;                                                        /* sp+0x50 */
    envTex.copy(envBuf);
    if (course->runway->hasLights != 0)
        envTex.allocVram();
    envTex.makeTexRegs(&envRegs.regs);
    modelset_set_env_tex(&envRegs.regs);

    RaceEntryCar *target = cm->views[cm->current].targetInfo.car[0];
    const int pcCockpit = lod != 2 ? cm->pcCockpit() : 0;   /* PC: not the mirror's (lod 2); 1 or 2: the slot */
    /* PC (2026-11-14): a custom camera's view (game/app/pc_customcam.h; not
     * the mirror's): its car is drawn whole, whichever fixed view its type
     * makes it, but for what its eye sits in */
    OnBoardCamera *pcCustom = lod != 2 ? cm->pcCustom() : NULL;
    if (lod > 0 && !pc_lod())
        lod = -1;                           /* PC: the levels of detail off: the real tyres (game/app/pc_detail.h) */
    /* PC: the cars, then the ghosts (pcPass 1).  A ghost is the mean of the
     * frame with and without it (gfx_half_blend_*), so what is drawn after it
     * does not show through it: a car drawn after the ghost lost the parts
     * behind it to what was behind the ghost, and with the two side by side
     * the distance sort swapped them from frame to frame - it flickered like
     * z-fighting (user report).  The PS2's ghost rows (SCANMSK) showed the car
     * through the ghost in either order. */
    for (int pcPass = 0; pcPass < 2; pcPass++)
    for (RaceEntryCar *car = entries->sortEnd; car != NULL; car = car->sortPrev) {
        if (car->modelEnabled == 0)
            continue;
        RaceCarState *state = car->state;
        int show = car_display_level(state);
        if ((show == CAR_SHOW_GHOST) != (pcPass == 1))
            continue;                       /* PC */
        const int pcOwn = pcCockpit && car == target;   /* PC: the cockpit view's own car */
        const int pcCustomOwn = pcCustom != NULL && car == target;  /* PC (2026-11-14): a custom camera's */
        if (cm->F_00209370(org, car) == 0 && !pcOwn && !pcCustomOwn)
            continue;
        if (show < CAR_SHOW_GHOST)
            continue;
        if (!race_car_in_view(state, t))
            continue;
        int ghost = show == CAR_SHOW_GHOST;
        if (ghost) {
            car->F_00177878();
            /* PC: the ghost at half strength (gfx_half_blend_*, G001_flare.h):
             * progressive.  The PS2 drew it on the rows of the field not
             * shown (PGL_SCANMASK_OTHER_FIELD), which blitField's field filter
             * averaged with the rows between: half a ghost. */
            gfx_half_blend_begin();
        }
        F_00179460(car->model.getCarModel())->callback.lod = lod;
        if (car == target)
            model_set_lod_dist(&car->model, env->carLodDist * 0.5f);
        else
            model_set_lod_dist(&car->model, env->carLodDist);

        CarRenderCtx lights;                                                /* sp+0x90 */
        lights.setup(course, state, 0.0f);
        if (lights.lit != 0) {
            DynamicEnvmapCamera cam;                                        /* sp+0x2E0 */
            cam.followCar(state, 0.0f);
            lights.drawGlows(&envTex, envBuf, &cam, car == target);
            /* implicit: ~CameraBase (cam's implicit destructor) */
        }   /* cam's implicit destructor: CameraBase::~CameraBase (flags 2) */
        /* PC (2026-11-07): the cockpit camera's own car is drawn whole, but
         * for the driver's head the eye sits in (game/app/pc_headcam.h) */
        const int pcHead = pcOwn && pc_headcam_on();
        if (pcOwn && !pcHead) {
            pgl_push_attrib(PGL_ATTR_SCISSOR);
            gfx_scissor_below(PC_COCKPIT_CUT);
        }
        if (pcHead)
            pc_headcam_hide(car, pcCockpit, 1);     /* (pcCockpit: the slot, 1 or 2) */
        if (pcCustomOwn)
            pc_customcam_hide(pcCustom, car, 1);    /* PC (2026-11-14) */
        for (int i = 0; i < lights.passes; i++) {
            lights.applyShadowPass(i);
            car->drawShadow(t);
        }
        if (F_001A8618(env) != 0)
            car->F_001778C0(t);
        lights.applyDefault();
        car->F_001778E8(t);
        lights.apply();
        /* PC (2026-11-11): the cockpit camera's car's body with its menu
         * model (PC 2026-11-15: the car model's own setting, the
         * calibrator's; pc_headcam.h): its glass shells left out, no head
         * box (it has no driver); the race model's hiding back for the rest */
        const int pcMenuBody = pcHead && car->model.pcMenuFile != NULL && pc_headcam_menu_model(car);
        if (pcMenuBody) {
            car->model.pcMenuDraw = 1;
            pc_headcam_hide_file(car->model.pcMenuFile);
        }
        car->drawBody(t);
        if (pcMenuBody) {
            car->model.pcMenuDraw = 0;
            pc_headcam_hide(car, pcCockpit, 1);
        }
        if (lod > 0) {
            car->drawTireCylinders(env->brakeGlowScale, t);
        } else if (car == target) {
            car->drawTires(tyreBlur != 0, env->brakeGlowScale, t, lod);
        } else {
            car->drawTires(tyreBlur != 0 ? 2 : 0, env->brakeGlowScale, t, lod);
        }
        car->F_00177988(0.0f);
        if (pcHead)
            pc_headcam_hide(car, pcCockpit, 0);     /* PC */
        if (pcCustomOwn)
            pc_customcam_hide(pcCustom, car, 0);    /* PC (2026-11-14) */
        if (pcOwn && !pcHead)
            pgl_pop_attrib();
        gs_set_scanmask_mode(PGL_SCANMASK_OFF);
        if (ghost)
            gfx_half_blend_end();           /* PC */
    }
    modelset_set_env_tex(NULL);
    modelset_set_cull_far(0);
    brake_textures_unlink();
}

// FUNC 0x0017FB68 RacePS2Base::drawReflections
//   The reflections on the road (CourseEnv's shadow* fields, T178 names):
//   model34 first into the screen's alpha (? the reflecting surfaces), then
//   into a reflW x reflH buffer with its own depth the course's
//   reflected parts (model24, course_draw_reflection_a), the cars mirrored in the ground
//   (drawCarReflections) and their lamps' reflections (drawLampReflections); blurred
//   reflBlur times (+-0.5 texel, bilinear) and blended over the screen
//   with reflBlend.
void RacePS2Base::drawReflections(GSBuffer *screen, GSBuffer *envBuf)
{
    CourseData *course = org->course->data;
    CourseEnv *env = course->env;
    CameraManager *cm = virtual_6();
    if (course->model34 != NULL) {
        gfx_set_frame_write_mask(0xFF000000);
        gs_set_clear_color(0);
        gs_clear_buffers(PGL_CLEAR_COLOR);
        gfx_set_frame_write_mask(0xFFFFFFFF);
        f32 x = cm->getPositionX();
        f32 y = cm->getPositionY();
        f32 z = cm->getPositionZ();
        course_draw_reflection_b(course, x, y, z);
    }
    pgl_push_attrib(PGL_ATTR_DRAW_BUFFER | PGL_ATTR_DEPTH | PGL_ATTR_TARGET_B | PGL_ATTR_FOG |
                    PGL_ATTR_VIEWPORT | PGL_ATTR_SCISSOR);                  /* 0x8C7 */
    GSBuffer refl(SCE_GS_PSMCT32, env->reflW, env->reflH);              /* sp+0x00 */
    GSBuffer reflZ(&refl, SCE_GS_PSMZ24);                                   /* sp+0x40 */
    /* PC (2026-11-14): [Graphics] ReflectionResolution (game/app/pc_detail.h;
     * user: "A graphics setting to increase the reflection resolution"): the
     * buffer - 64 x 128 GS pixels stretched over the 640 x 448 screen, 10 x
     * 3.5 times coarser - drawn by the renderer at half the screen's
     * resolution or at the screen's, its GS size and the draws into it
     * unchanged; the blur's half-texel steps below that much shorter (as the
     * PS2's own with a buffer that much larger: the same blur, finer) */
    f32 pcDetailX, pcDetailY;
    pc_reflection_scale(screen->width, screen->displayHeight(), refl.width, refl.height, &pcDetailX, &pcDetailY);
    const int pcDetail = pcDetailX != 1.0f || pcDetailY != 1.0f;
    if (pcDetail)
        gfx_pc_target_detail(refl.addr, refl.width, refl.height, pcDetailX, pcDetailY);
    refl.setDrawBuffer();
    reflZ.setZBuffer();
    modelset_set_projection_scale(1.0f);
    gs_set_clear_color(0);
    gs_clear_buffers(PGL_CLEAR_COLOR | PGL_CLEAR_DEPTH);
    course_fog_apply(&env->reflFog, 1.0f);
    {
        f32 x = cm->getPositionX();
        f32 y = cm->getPositionY();
        f32 z = cm->getPositionZ();
        course_draw_reflection_a(course, x, y, z);
    }
    clear_frame_alpha(0x80000000);
    if (env->flags & ENV_REFL_CARS)
        drawCarReflections(envBuf, 0.0f);
    if (env->flags & ENV_REFL_LAMPS)
        drawLampReflections(0.0f);

    /* the blur: the buffer drawn over itself, half a texel off */
    if (env->reflBlur != 0) {
        refl.setTexture();
        gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);
        gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
        gfx_disable(PGL_CAP_DEPTH_TEST);
        gfx_disable(PGL_CAP_ALPHA_TEST);
        gfx_disable(PGL_CAP_BLEND);
        gfx_disable(PGL_CAP_FOG);
        pgl_color(0x80FFFFFF);
        for (int pass = 0; pass < env->reflBlur; pass++) {
            f32 off = (pass & 1) ? 0.5f : -0.5f;
            f32 x = 0.0f, y = 0.0f;
            f32 w = (f32)refl.width;
            f32 h = (f32)refl.displayHeight();
            const f32 offX = off / pcDetailX, offY = off / pcDetailY;   /* PC (2026-11-14): (1, 1: the original's) */
            gs_draw_textured_sprite_strips_f(x, y, w, h, x + offX, y + offY,
                                             (f32)refl.width + offX, (f32)refl.height + offY);
            gfx_texflush();
        }
    }
    pgl_pop_attrib();
    modelset_set_projection_scale(1.0f);

    /* over the screen */
    refl.setTexture();
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
    gfx_disable(PGL_CAP_DEPTH_TEST);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    gfx_enable(PGL_CAP_BLEND);
    gfx_disable(PGL_CAP_FOG);
    gfx_blend_func(env->reflBlend, 0.0f);
    pgl_color(0x80FFFFFF);
    f32 off = -0.5f;
    if (env->reflBlur & 1)
        off = 0.0f;
    f32 w = (f32)screen->width;
    f32 h = (f32)screen->displayHeight();
    /* (PC 2026-11-14: the half texel the blur left, of the finer buffer) */
    const f32 offX = off / pcDetailX, offY = off / pcDetailY;
    gs_draw_textured_sprite_strips_f(0.0f, 0.0f, w, h, offX + 0.0f, offY + 0.0f,
                                     (f32)refl.width + offX, (f32)refl.height + offY);
    /* PC (2026-11-14): the block back to the output scale for the buffers
     * after this one there (read as a texture above, before this) */
    if (pcDetail)
        gfx_pc_target_detail(refl.addr, refl.width, refl.height, 1.0f, 1.0f);
}

// FUNC 0x0017FFE8 RacePS2Base::drawCarReflections
//   The cars mirrored in the ground (setReflectionPlane: the front faces
//   turn clockwise) into the reflection buffer, farthest first; not the
//   ones without a controller (noController: replay / ghost cars).  The
//   normal matrix of each is the inverse of its mirror matrix times the
//   view matrix saved first.  envBuf: the environment map; t is not used.
void RacePS2Base::drawCarReflections(GSBuffer *envBuf, f32 t)
{
    modelset_set_cull_far(1);
    gfx_front_face(PGL_FRONT_CW);
    gfx_enable(PGL_CAP_NORMAL_MATRIX);
    Matrix44 view;                                                          /* sp+0x00 */
    pgl_get_matrix(&view.m[0][0]);
    CameraManager *cm = virtual_6();
    RaceEntryBase *entries = org->entries;
    CourseData *course = org->course->data;
    f32 eyeX = cm->getPositionX();
    f32 eyeY = cm->getPositionY();
    f32 eyeZ = cm->getPositionZ();
    entries->sortByDistance(eyeX, eyeY, eyeZ);
    GsTexPacket envRegs;                                                    /* sp+0x40 */
    envBuf->makeTexRegs(&envRegs.regs);
    modelset_set_env_tex(&envRegs.regs);
    /* PC: the reflection's own detail ([Graphics] ReflectionDetail, game/app/
     * pc_detail.h), whatever the levels of detail: the original's (LOD 2
     * below, the tyre cylinders) or the full detail.  32 cars at the full
     * detail were a third of a wet race's draws, into this small blurred
     * buffer (user's choice: a setting of its own, 2026-11-03) */
    modelset_pc_set_detail(pc_reflection_full() ? PC_DETAIL_FULL : PC_DETAIL_GAME);
    /* PC (2026-11-14): a custom camera's own car is drawn (drawCars), so its
     * reflection is too, an onboard type's as well (the bumper view's car
     * has none) */
    RaceEntryCar *pcCustomCar = cm->pcCustom() != NULL ? cm->views[cm->current].targetInfo.car[0] : NULL;

    for (RaceEntryCar *car = entries->sortEnd; car != NULL; car = car->sortPrev) {
        if (cm->F_00209370(org, car) == 0 && car != pcCustomCar)
            continue;
        RaceCarState *state = car->state;
        if (state->st.noController != 0)
            continue;
        CarRenderCtx lights;                                                /* sp+0x90 */
        lights.setup(course, state, 0.0f);
        pgl_push_matrix();
        car->setReflectionPlane(0.0f);
        pgl_push_matrix();
        vu0_invert_rigid();
        pgl_mult_matrix_90(&view.m[0][0]);
        Matrix44 normal;                                                    /* sp+0x2E0 */
        pgl_get_matrix(&normal.m[0][0]);
        pgl_set_normal_matrix(&normal.m[0][0]);
        pgl_pop_matrix();
        lights.apply();
        F_00179460(car->model.getCarModel())->callback.lod = 2;     /* (PC: the full detail: 0, T451) */
        car->drawBody(0.0f);
        if (pc_lod())
            car->drawTireCylinders(org->course->data->env->brakeGlowScale, 0.0f);
        else
            car->drawTires(0, org->course->data->env->brakeGlowScale, 0.0f, -1);  /* PC: the real tyres */
        pgl_pop_matrix();
    }
    modelset_pc_set_detail(PC_DETAIL_SETTING);  /* PC: [Graphics] Lod again */
    modelset_set_env_tex(NULL);
    gfx_disable(PGL_CAP_NORMAL_MATRIX);
    gfx_front_face(PGL_FRONT_CCW);
    modelset_set_cull_far(0);
}

// FUNC 0x00180218 RacePS2Base::drawLampReflections
//   The reflections of the cars' lamps on the road (FlareClass's reflection
//   list), the same cars as drawCarReflections.  Quirk: modelset_set_cull_far(1) is
//   not reset to 0 at the end (drawCars / drawCarReflections do it).  t is not
//   used.
void RacePS2Base::drawLampReflections(f32 t)
{
    modelset_set_cull_far(1);
    CameraManager *cm = virtual_6();
    RaceEntryBase *entries = org->entries;
    course_env_set_reflection(org->course->data->env, &flare);
    flare.beginReflections();
    RaceEntryCar *pcCustomCar = cm->pcCustom() != NULL ? cm->views[cm->current].targetInfo.car[0] : NULL;  /* PC (as drawCarReflections) */
    for (RaceEntryCar *car = entries->sortEnd; car != NULL; car = car->sortPrev) {
        RaceCarState *state = car->state;
        if (cm->F_00209370(org, car) == 0 && car != pcCustomCar)
            continue;
        if (state->st.noController != 0)
            continue;
        pgl_push_matrix();
        car->setReflectionPlane(0.0f);
        car->drawLampReflections(&flare, 0.0f);
        pgl_pop_matrix();
    }
    flare.endReflections();
}

// FUNC 0x00180330 RacePS2Base::drawMirror
//   The rear-view mirror: the view behind (the view matrix mirrored with
//   vu0_scale(1, 1, -1), front faces clockwise; far clip 120, 60 when
//   sub != 0) into the rectangle `mirror' of a copy of screen, with a blank
//   64 x 64 environment map and the mirror vision list (the alternative one
//   when sub != 0 and the course has it), the cars at LOD 2 and the smoke;
//   then a black frame around it and its depth cleared to the nearest (?
//   nothing drawn later shows through).
void RacePS2Base::drawMirror(GSBuffer *screen, int sub)
{
    /* implicit: GSBuffer::copy (the inline copy constructor) */
    GSBuffer view(*screen);                                                 /* sp+0x00 */
    /* PC: the dynamic aspect (game/app/pc_aspect.h): the mirror keeps its
     * 4:3 size on the screen, as a HUD element: its rectangle (in the 640 x
     * 448 design) placed as the HUD of the view it is in maps it - the
     * buffer as a screen of its own (pc_view_scales: the whole frame, kx /
     * ky about its centre; a split screen's view, its own) - and its
     * projection against it, below, so that the view inside is the 4:3 one */
    const f32 kx = pc_aspect_kx(), ky = pc_aspect_ky();
    f32 mx = mirror.x, my = mirror.y, mw = mirror.w, mh = mirror.h;
    if (pc_aspect_race()) {
        const f32 vw = (f32)view.width, vh = (f32)view.displayHeight();
        f32 vkx, vky, sx, sy;
        pc_view_scales(vw, vh, &vkx, &vky, &sx, &sy);
        sx *= pc_hud_safe();                /* (the HUD safe zone: the mirror with the HUD) */
        sy *= pc_hud_safe();
        mx = vw * 0.5f + (mx + mw * 0.5f - 320.0f) * sx - mw * sx * 0.5f;
        mw *= sx;
        my = vh * 0.5f + (my + mh * 0.5f - 224.0f) * sy - mh * sy * 0.5f;
        mh *= sy;
    }
    view.setRect((int)mx, (int)my, (int)mw, (int)mh);
    CourseData *course = org->course->data;
    CourseEnv *env = course->env;
    CameraManager *cm = virtual_6();
    pgl_push_attrib(PGL_ATTR_DRAW_BUFFER | PGL_ATTR_DEPTH | PGL_ATTR_TARGET_B | PGL_ATTR_FOG |
                    PGL_ATTR_VIEWPORT | PGL_ATTR_SCISSOR);                  /* 0x8C7 */
    gfx_front_face(PGL_FRONT_CW);
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    pgl_push_matrix();
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
    pgl_push_matrix();
    GSBuffer envmap(SCE_GS_PSMCT16, 0x40, 0x40);                           /* sp+0x40 */
    envmap.setDrawBuffer();
    gs_set_clear_color(0x80000000);
    gs_clear_buffers(PGL_CLEAR_COLOR);

    /* the projection and the view */
    view.setDrawBuffer();
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    vu0_load_identity_regs();
    /* PC: the levels of detail off (game/app/pc_detail.h): the split
     * screen's mirror as the single view's; the render distance */
    cm->setFarClip((sub != 0 && pc_lod() ? 60.0f : 120.0f) * pc_render_scale() *
                   pc_course_far_factor(course));  /* PC: and the course scale's (game/app/pc_course_scale.h) */
    cm->applyProjection();
    /* implicit: F_00257EF0, vu0_scale (proj_y_scale's).  PC: against the
     * rectangle squeezed above: x by kx / ky (its aspect below already took
     * ky / kx), the 4:3 view */
    F_00257EF0(PROJ_Y_SCALE);
    if (kx != 1.0f || ky != 1.0f)
        vu0_scale(kx / ky, 1.0f, 1.0f);
    f32 zoom = mirror.zoom;
    f32 aspect;
    if (view.hasRect != 0)
        aspect = (f32)view.rectH / (f32)view.rectW;
    else
        aspect = (f32)view.displayHeight() / (f32)view.width;
    vu0_scale(aspect * PROJ_ASPECT * zoom, zoom, 1.0f);
    modelset_set_projection_scale(1.0f);
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
    vu0_load_identity_regs();
    vu0_scale(1.0f, 1.0f, -1.0f);
    cm->applyView();
    /* PC: blended between ticks as the view it is in (game/app/pc_frame.h):
     * the tick's camera under cars drawn at the shown time jittered */
    pc_view_interpolate((const u8 *)pc_view_key(cm) + 3 + sub, pc_view_shot(cm));

    /* the course */
    course_env_apply(env);
    course_fog_apply(&env->mirrorFog, 1.0f);
    gs_set_clear_color_f(0.0f, 0.0f, 0.0f, 0.0f);
    gs_clear_buffers(PGL_CLEAR_COLOR | PGL_CLEAR_DEPTH);
    course_draw_model(course, course->model60);
    gs_clear_buffers(PGL_CLEAR_DEPTH);
    modelset_set_tex_lod(1);
    modelset_set_tex_set(2);                /* (the levels of detail off: set 0, T451) */
    if (!pc_lod())
        course_draw(course, course->visionList, cm, course->env->visionMain);  /* PC: the full course */
    else if (sub != 0 && course->visionMirrorAlt != NULL)
        course_draw(course, course->visionMirrorAlt, cm, course->env->visionMirror);
    else
        course_draw(course, course->visionMirror, cm, course->env->visionMirror);
    modelset_set_tex_lod(0);
    clear_frame_alpha(0x80000000);

    /* the cars, the smoke */
    drawCars(&envmap, 2, 0, 0.0f);
    smoke.draw(1);

    /* the frame */
    gfx_disable(PGL_CAP_DEPTH_TEST);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    int right = view.rectW - 1;
    int bottom = view.rectH - 1;
    pgl_color(0);
    /* PC (2026-11-09): the frame as the GS pixels the PS2's lines covered -
     * the rectangle's outermost ring, a GS pixel wide - in flat sprites
     * (user: "The rearview mirror border gets slightly off the original
     * alignment and gets thinner at higher resolutions").  A GS line is the
     * GPU's one-pixel line at the window's resolution, on the first window
     * pixel of its GS pixel: at a scale s the frame was one window pixel
     * thin, and the right and bottom lines s - 1 window pixels inside the
     * picture's edge (the mirror's view showing beyond them).  The sprites,
     * in the same colour and state, cover the whole GS pixels (at 1:1 the
     * PS2's picture), placed in the draw window as the lines were (a flat
     * sprite takes no window origin; in a field buffer the window's y is in
     * field lines, the sprite's in frame lines).  Retail:
     *     gs_draw_line(0, 0, 0, bottom);
     *     gs_draw_line(0, bottom, right, bottom);
     *     gs_draw_line(right, bottom, right, 0);
     *     gs_draw_line(right, 0, 0, 0); */
    {
        s32 wx, wy, ww, wh;
        gfx_get_draw_area(&wx, &wy, &ww, &wh);
        if (view.halfHeight != 0)
            wy <<= 1;
        gs_draw_flat_sprite(wx, wy, wx + 1, wy + bottom + 1);                          /* left */
        gs_draw_flat_sprite(wx + right, wy, wx + right + 1, wy + bottom + 1);          /* right */
        gs_draw_flat_sprite(wx + 1, wy, wx + right, wy + 1);                           /* top */
        gs_draw_flat_sprite(wx + 1, wy + bottom, wx + right, wy + bottom + 1);         /* bottom */
    }
    gfx_enable(PGL_CAP_DEPTH_TEST);
    gs_set_clear_depth(0.0f);
    gs_clear_buffers(PGL_CLEAR_DEPTH);
    gs_set_clear_depth(1.0f);
    pgl_matrix_mode(PGL_MTX_PROJECTION);
    pgl_pop_matrix();
    pgl_matrix_mode(PGL_MTX_MODELVIEW);
    pgl_pop_matrix();
    gfx_front_face(PGL_FRONT_CCW);
    pgl_pop_attrib();
}

// FUNC 0x001807B0 clear_frame_alpha
//   Clears the alpha channel of the frame to rgba's alpha (write mask
//   0xFF000000: only alpha), then all channels writable again.
void clear_frame_alpha(u32 rgba)
{
    gfx_set_frame_write_mask(0xFF000000);
    gs_set_clear_color(rgba);
    gs_clear_buffers(PGL_CLEAR_COLOR);
    gfx_set_frame_write_mask(0xFFFFFFFF);
}

// FUNC 0x001807F8 write_alpha_depth
//   Before the course: the depth cleared, then buf (the buffer being drawn)
//   as a full-size texture sprite at the depth of gs_set_sprite_zf(1, 0xFF)
//   with nothing of the colour written (write mask 0), depth test ALWAYS
//   and alpha test NOTEQUAL 0: only the depth of the pixels whose alpha is
//   set (by F_001A9240's model58) is written, just above the cleared depth
//   (? for a later depth test: the flare's occlusion test or the replay
//   blur).  Leaves the depth test LEQUAL and the write mask 0x00FFFFFF
//   (colour, not alpha).
void write_alpha_depth(GSBuffer *buf)
{
    gs_clear_buffers(PGL_CLEAR_DEPTH);
    buf->setTexture();
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_NEAREST);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_NEAREST);
    gfx_enable(PGL_CAP_DEPTH_TEST);
    gfx_enable(PGL_CAP_ALPHA_TEST);
    gfx_depth_func(PGL_DEPTHFUNC_ALWAYS);
    gfx_alpha_func(PGL_ATEST_NOTEQUAL, 0.0f);
    gfx_set_frame_write_mask(0);
    gs_set_sprite_zf(1, 0xFF);
    f32 w = (f32)buf->width;
    f32 h = (f32)buf->displayHeight();
    gs_draw_textured_sprite_strips_f(0.0f, 0.0f, w, h, 0.0f, 0.0f, (f32)buf->width, (f32)buf->height);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    gfx_depth_func(PGL_DEPTHFUNC_LEQUAL);
    gfx_set_frame_write_mask(0x00FFFFFF);
}

// FUNC 0x001808E8 RacePS2Base::addCarLamps
//   The lamps of the cars the camera is not inside of
//   (CameraManager::virtual_12) into the flare.
//   Matching (94.4%): retail reloads o->entries before the loop (a0) as
//   well as on the back edge; egcs's PRE reuses the count's load instead.
//   Only a volatile read of the count (a fakematch, not kept) stops it;
//   inline count / car accessors (also const ones) and -fno-gcse (worse
//   elsewhere) were tried.
void RacePS2Base::addCarLamps(f32 t)
{
    RaceOrganization *o = org;
    CameraManager *cm = virtual_6();
    course_env_set_lamps(org->course->data->env, &flare);
    int count = o->entries->count;     /* blez / slt: compared signed */
    for (int i = 0; i < count; i++) {
        RaceEntryCar *car = o->entries->cars[i];
        if (cm->virtual_12(o, i) != 0)
            car->addLamps(&flare, t);
    }
}

// FUNC 0x001809E8 RacePS2Base::addCourseLights
//   The course's lights into the flare: the flare source groups visible
//   from the vision list point nearest the eye (all of them when the course
//   has no vision list).
void RacePS2Base::addCourseLights()
{
    CourseData *course = org->course->data;
    CameraManager *cm = virtual_6();
    course_env_set_flare_types(org->course->data->env, &flare);
    f32 x = cm->getPositionX();
    f32 y = cm->getPositionY();
    f32 z = cm->getPositionZ();
    const u16 *groups;
    if (course->visionList == NULL)
        groups = NULL;
    else if (pc_render_scale() > 1.0f)
        /* PC: the render distance (game/app/pc_detail.h): the lights of the
         * parts drawn (course_draw) */
        groups = pc_vision_parts(course->visionList, x, y, z, (pc_render_scale() - 1.0f) * course->env->farClip);
    else
        groups = vision_nearest(course->visionList, x, y, z)->parts;
    course_submit_flares(course, &flare, groups);
}

// FUNC 0x00180AE0 RacePS2Base::captureAfterimage
//   Splits the frame buffer shown last (the other one of g_002F4230.frame)
//   into the three VRAM planes at vramB74 (red; green +0x23000; blue
//   +0x46000; 640 x 224) and starts the afterimage: afterimage = max(1 -
//   (1/60) / afterimageTime, 0).  buf is not used.
//   The enhanced engine (race_logic.md D1, ENH_RACE_AFTERIMAGE): it keeps
//   the GS field mode of the capture (Lupo 2 L2 0x0017EC80).
/* PC: the aspect of the picture captured (game/app/pc_aspect.h): the frame
 * shown before a race's first is a 4:3 menu or loading screen. */
static f32 s_afterimageAspect = PC_ASPECT_BASE;

void RacePS2Base::captureAfterimage(GSBuffer *buf)
{
    s_afterimageAspect = pc_aspect_prev();  /* PC */
    GSBuffer work(SCE_GS_PSMCT32, 0x40, 0x80);                              /* sp+0x00 3 pages of work VRAM */
    u32 planeR = g_002F4230.vramB74;
    u32 planeG = planeR + 0x23000;
    u32 planeB = planeR + 0x46000;
    pair_other(&g_002F4230.frame)->setTexture();
    pfx_split_rgb_planes(planeR, planeG, planeB, 0x280, 0xE0, work.addr);
#if ENH_RACE_AFTERIMAGE
    afterimageField = (u8)gs_get_field_mode();     /* Lupo 2 (L2 0x0017ED14) */
#endif
    f32 level = 1.0f - 0.0166666676f / afterimageTime;     /* 0x3C888889 0x1.111112p-6f = 1/60: one frame (race_ps2_render_internal.h) */
    afterimage = MAXF(level, 0.0f);
}

// FUNC 0x00180BF8 RacePS2Base::drawAfterimage
//   While afterimage > 0: the three saved planes (PSMT8H through an
//   identity CLUT) drawn over buf, each into its colour channel (write
//   masks 0xFF, 0xFF00, 0xFF0000), 640 x 448 from 640 x 224, blended
//   (Cs - Cd) x afterimage^3 + Cd.
//   The enhanced engine (race_logic.md D1, ENH_RACE_AFTERIMAGE; Lupo 2 L2
//   0x0017EDB0): nothing while the race runs in the background (behind a
//   menu), and when the current field is not the capture's the planes are
//   read half a line up (the odd field) or down, so the trail does not
//   jitter between the fields (PC: not, progressive).  (Lupo 2 also draws it later, after the 2D
//   overlays, RacePS2Base::virtual_46: not taken, GT3's trail leaves the
//   HUD sharp.)
void RacePS2Base::drawAfterimage(GSBuffer *buf)
{
#if ENH_RACE_AFTERIMAGE
    if (org->backgroundMode != 0)           /* Lupo 2 (L2 0x0017EDDC) */
        return;
#endif
    if (0.0f < afterimage) {
        buf->setDrawBuffer();
        GSBuffer clut(SCE_GS_PSMCT32, 0x40, 0x20);                          /* sp+0x00 */
        u32 clutAddr = clut.addr;
        pfx_build_identity_clut32(clutAddr);
        gfx_texflush();
        pgl_color(0x80FFFFFF);
        gs_set_clut(clutAddr, SCE_GS_PSMCT32);
        gs_set_clut_csa(0);
        gfx_set_clut_mode(PGL_CLUT_T8);
        gfx_disable(PGL_CAP_DEPTH_TEST);
        gfx_disable(PGL_CAP_ALPHA_TEST);
        gfx_disable(PGL_CAP_DEST_ALPHA);
        gfx_disable(PGL_CAP_FOG);
        gfx_enable(PGL_CAP_BLEND);
        f32 a = afterimage;
        gfx_blend_func(PGL_BLEND_SEL(PGL_SEL_CS, PGL_SEL_CD, PGL_SEL_FIX, PGL_SEL_CD), a * a * a);  /* 0x64 */
        gfx_depth_mask(0);
        gfx_set_texture_size(0x280, 0xE0);
        gfx_tex_param(PGL_TEXPARAM_WMS, PGL_WRAP_REGION_CLAMP);
        gfx_tex_param(PGL_TEXPARAM_WMT, PGL_WRAP_REGION_CLAMP);
        gfx_set_texture_region(PGL_REGION_U, 0, 0x27F);
        gfx_set_texture_region(PGL_REGION_V, 0, 0xDF);
        gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_NEAREST);
        gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_NEAREST);
        u32 planeR = g_002F4230.vramB74;
        u32 planeG = planeR + 0x23000;
        u32 planeB = planeR + 0x46000;
        f32 v0 = 0.0f;                      /* the planes' first line */
        /* PC: no field correction, progressive.  The enhanced engine read the
         * planes half a line up (the odd field) or down when the field was
         * not the capture's (Lupo 2 L2 0x0017EF00..0x0017EF34:
         * v0 = field == PGL_FIELD_HALF_ODD ? -0.5 : 0.5); there is one
         * field now (gs_get_field_mode: PGL_FIELD_FRAME). */
        /* PC: the captured picture where the window showed it (a 4:3 one
         * in its 4:3 rectangle of a race frame, not stretched over the
         * window: game/app/pc_aspect.h), its black bars fading alike */
        int x0, y0, x1, y1;
        pc_aspect_picture_rect(s_afterimageAspect, &x0, &y0, &x1, &y1);
        const f32 fx0 = (f32)x0, fy0 = (f32)y0, fx1 = (f32)x1, fy1 = (f32)y1;
        gfx_set_texture_buffer(0, planeR, SCE_GS_PSMT8H, 0x280);
        gfx_set_frame_write_mask(0x000000FF);
        gs_draw_textured_sprite_strips_f(fx0, fy0, fx1, fy1, 0.0f, v0, 640.0f, 224.0f + v0);
        gfx_set_texture_buffer(0, planeG, SCE_GS_PSMT8H, 0x280);
        gfx_set_frame_write_mask(0x0000FF00);
        gs_draw_textured_sprite_strips_f(fx0, fy0, fx1, fy1, 0.0f, v0, 640.0f, 224.0f + v0);
        gfx_set_texture_buffer(0, planeB, SCE_GS_PSMT8H, 0x280);
        gfx_set_frame_write_mask(0x00FF0000);
        gs_draw_textured_sprite_strips_f(fx0, fy0, fx1, fy1, 0.0f, v0, 640.0f, 224.0f + v0);
        if (x0 > 0 || y0 > 0 || x1 < 640 || y1 < 448) {
            gfx_set_frame_write_mask(0x00FFFFFF);
            pgl_color(0x80000000);
            if (x0 > 0) {
                gs_draw_flat_sprite(0, 0, x0, 448);
                gs_draw_flat_sprite(x1, 0, 640, 448);
            }
            if (y0 > 0) {
                gs_draw_flat_sprite(0, 0, 640, y0);
                gs_draw_flat_sprite(0, y1, 640, 448);
            }
            pgl_color(0x80FFFFFF);
        }
        gfx_set_frame_write_mask(0xFFFFFFFF);
        gfx_depth_mask(1);
        gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
        gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);
    }
}

// FUNC 0x00180EB0 F_00180EB0
//   Returns its argument (no callers: an inline function emitted here).
void *F_00180EB0(void *p)
{
    return p;
}
