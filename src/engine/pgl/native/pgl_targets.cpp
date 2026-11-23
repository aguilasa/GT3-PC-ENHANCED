/* pgl_targets.cpp - native render targets by VRAM address, presentation and
 * readbacks (docs/renderer.md "Targets", graphics.md 7 and 9).
 *
 * Every GS frame / Z buffer the game draws into gets a native resource,
 * keyed by its base block and size (VRAM is reused for differently sized
 * buffers within a frame: each size is its own resource; the registry's
 * owner map tells which one the GS memory holds).  All targets are drawn at
 * the output scales sx = the width of the picture shown / 640, sy = its
 * height / 448, times the resolution scale (picture_scales; they follow
 * window resizes, display modes, the picture's aspect and the setting:
 * follow_output, targets_frame_aspect): a W x H buffer is
 * (W sx) x (H sy) native pixels, a one-field buffer (W >= 512, H <= 256: the
 * 640 x 224 frame buffers) (W sx) x (2 H sy), so the field buffer and the
 * 640 x 448 race screen have the same native size.  The picture is 4:3 (the
 * menus: sx = sy) or, for a frame with a race view, the whole window (the
 * dynamic aspect, docs/renderer.md "Aspect": the race draws its view and HUD
 * squeezed into the 640 x 448 GS frame, sx != sy unsqueezes them).
 *   colour: R8G8B8A8_UNORM (a GS byte b stored as b / 255, alpha too)
 *   depth:  D32_FLOAT_S8X24 (Z / 2^bits exactly; typeless for the SRV of
 *           the depth copies)
 * A Z buffer has no size of its own on the GS (ZBUF is a base and a
 * format): the frame drawn with it decides which GS pixels its words hold.
 * Its native resource is shared by every colour target of its native size
 * (D3D11 needs equal sizes), so its GS geometry is the one of the last frame
 * that wrote it (depth_target_written), not of the first one it met. */
#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_win32.h"   /* (the Win32 calls off Windows too) */
#include "engine/pgl/native/pgl_capture.h"  /* PC (2026-11-06): the frame capture's copies, clears, present */

#include <algorithm>
#include <atomic>                               /* PC (2026-11-15): (per_sample_frame) */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "platform/fpu.h"
#include "platform/frame_monitor.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): N_ - the screenshot's reasons, shown through tr */
#include "platform/frame_step.h"
#include "platform/osd.h"
#include "platform/song_osd.h"                  /* PC: the race's song changer */
#include "platform/cockpit_osd.h"               /* PC (2026-11-08): the cockpit camera's calibration */
#include "platform/challenge_osd.h"             /* PC (2026-11-14): the challenge modes' panel */
#include "platform/texture_menu.h"              /* PC (2026-11-13): the texture interface */
#include "platform/gpu.h"
#include "platform/log.h"
#include "platform/screenshot.h"               /* PC (2026-11-15): the screenshot's picture */
#include "platform/settings.h"
#include "platform/settings_menu.h"
#include "platform/system_menu.h"

namespace pglr {

namespace {

float    g_scaleX = 1.0f, g_scaleY = 1.0f;
/* The aspect of the picture of the frame executed last (and presented
 * next): 0 the 4:3 picture, else the window's (targets_frame_aspect). */
float    g_picAspect = 0.0f;
/* That race frame is drawn over the screens' canvas (gpu.h "Screens";
 * pglr_set_picture_multi): its picture the canvas's size. */
bool     g_picMulti = false;
uint64_t g_frameNo = 1;
std::vector<std::unique_ptr<Target>> g_targets;

/* PC (2026-11-06): the frame capture's record of a copy, resolve, clear or
 * pass over target t (pgl_capture.h; ~0u when not capturing) */
static uint32_t cap_record(const Target &t, uint8_t kind, const char *note, uint32_t count)
{
    if (!cap::on())
        return ~0u;
    cap::Draw d;
    d.kind = kind;
    d.note = note;
    d.count = count;
    d.target = t.base;
    d.targetW = (uint16_t)t.nw;
    d.targetH = (uint16_t)t.nh;
    return cap::draw_begin(d);
}
/* PC (CPU time): moves when a target is made or goes, or a target's or a
 * buffer's size changes: the last lookups of color_target / depth_target
 * (a draw state each, mostly the same targets one after another) hold
 * while it stays. */
uint32_t g_targetsGen = 1;
struct ColorHit {
    uint32_t gen = 0, base = 0;
    int fallbackW = 0, fallbackH = 0;
    Target *t = nullptr;
} g_colorHit;
struct DepthHit {
    uint32_t gen = 0, base = 0;
    int nw = 0, nh = 0, samples = 0;
    Target *t = nullptr;
} g_depthHit;

/* The GS sizes of the draw buffers by base (execution order; kept across
 * frames: a buffer set up once keeps its size). */
struct Known {
    int w, h;
    uint32_t psm;
    float dx = 1.0f, dy = 1.0f;     /* PC (2026-11-14): the buffer's detail (target_detail) */
};
std::unordered_map<uint32_t, Known> g_known;

/* The displayed buffer (gfx_set_display_fb) */
struct Display {
    bool valid = false;
    bool enabled = true;
    uint32_t base = 0, psm = 0, fbw = 0;
    int dx = 0, dy = 0, dw = 0, dh = 0;
} g_disp;

unsigned int g_gameFpu = 0;

void native_size(int w, int h, int &nw, int &nh, float dx = 1.0f, float dy = 1.0f)
{
    const int vmul = (w >= 512 && h <= 256) ? 2 : 1;
    nw = std::min(16384, std::max(1, (int)std::ceil((float)w * g_scaleX * dx - 0.01f)));
    nh = std::min(16384, std::max(1, (int)std::ceil((float)(h * vmul) * g_scaleY * dy - 0.01f)));
}

/* PC (2026-11-14): a target's native size at the output scale and its
 * detail (target_detail) */
void native_size_of(const Target &t, int &nw, int &nh)
{
    native_size(t.gsW, t.gsH, nw, nh, t.detailX, t.detailY);
}

/* The picture's size in the back buffer: the largest rectangle of the race
 * frame's aspect (aspect > 0: the window's own, so all of it, unless the
 * game held it to its limits or the window changed since), else the
 * largest 4:3 one. */
void picture_size(float aspect, int bw, int bh, int &pw, int &ph)
{
    pw = bw;
    ph = bh;
    if (aspect > 0.0f) {
        const float w = (float)bh * aspect, h = (float)bw / aspect;
        if (w < (float)bw - 1.0f)
            pw = std::max(1, (int)(w + 0.5f));
        else if (h < (float)bh - 1.0f)
            ph = std::max(1, (int)(h + 0.5f));
        return;
    }
    if (bw * 3 > bh * 4)
        pw = bh * 4 / 3;
    else
        ph = bw * 3 / 4;
}

/* The scales of that picture at the resolution scale ([Graphics]
 * RenderScale, 10 .. 400 % of the picture shown: below 100 the targets are
 * smaller than the window and the picture is stretched onto it, above it
 * they are larger and filtered down - supersampling, present_now), each held
 * to the native texture limit (16384: the 640 x 448 screen), the picture at
 * most kMaxPixels native pixels (both scaled down alike beyond it, the shape
 * kept: every colour target of the screen's size is that large, its depth
 * buffer twice, MSAA times its samples), at least 1/20. */
constexpr float kMaxPixels = 64.0f * 1024.0f * 1024.0f;
void picture_scales(float aspect, float &sx, float &sy)
{
    int bw = 0, bh = 0, pw, ph;
    if (aspect > 0.0f && g_picMulti && gpu::screen_count() > 1)
        gpu::canvas_size(&bw, &bh);             /* (the screens' canvas) */
    else
        gpu::backbuffer_size(&bw, &bh);
    if (bw <= 0 || bh <= 0) {
        sx = sy = 1.0f;
        return;
    }
    picture_size(aspect, bw, bh, pw, ph);
    const float scale = (float)settings::render_scale() / 100.0f;
    sx = std::min(16384.0f / 640.0f, (float)pw / 640.0f * scale);
    sy = std::min(16384.0f / 448.0f, (float)ph / 448.0f * scale);
    if (sx * sy * 640.0f * 448.0f > kMaxPixels) {
        const float k = std::sqrt(kMaxPixels / (sx * sy * 640.0f * 448.0f));
        sx *= k;
        sy *= k;
    }
    sx = std::max(0.05f, sx);
    sy = std::max(0.05f, sy);
}

/* MSAA (docs/renderer.md "Anti-aliasing"): the samples per pixel of the
 * targets made at the output scale (targets_frame_settings), and the most
 * the device has for both target formats at each count. */
int g_samples = 1;

int supported_samples(int want)
{
    if (dev()->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_1)
        return 1;                           /* (no multisampled depth to read: the resolve pass) */
    for (int n = want; n > 1; n >>= 1) {
        UINT qc = 0, qd = 0;
        if (SUCCEEDED(dev()->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)n, &qc)) &&
            SUCCEEDED(dev()->CheckMultisampleQualityLevels(DXGI_FORMAT_R32G8X24_TYPELESS, (UINT)n, &qd)) &&
            qc > 0 && qd > 0)
            return n;
    }
    return 1;
}

/* The resources of t at its native size and sample count (cleared).  A
 * multisampled target is read through its resolve (target_copy_srv); its
 * multisampled texture view is for the resolve passes. */
/* A target's own surface (its copy and mirror not counted). */
uint64_t target_bytes(const Target &t)
{
    return (uint64_t)t.nw * (uint64_t)t.nh * (uint64_t)std::max(1, t.samples) * (t.depth ? 8u : 4u);
}

/* PC (2026-11-13): the surfaces of retired targets, kept for the next target
 * of the same kind, size and sample count (create): a buffer the game
 * allocates anew every frame may land at another VRAM block each time, a new
 * target at each - with the old ones' surfaces at hand making one costs
 * nothing.  At most kSpareCount of them and kSpareBytes; one not taken for
 * kSpareFrames frames goes. */
struct Spare {
    bool depth = false;
    int nw = 0, nh = 0, samples = 1;
    uint64_t bytes = 0, at = 0;
    Com<ID3D11Texture2D> tex;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv, dsvRead;
    Com<ID3D11ShaderResourceView> srv, srvMs;
};
constexpr size_t kSpareCount = 32;
constexpr uint64_t kSpareBytes = 768ull << 20;
constexpr uint64_t kSpareFrames = 300;
std::vector<Spare> g_spares;
uint64_t g_spareBytes = 0;
unsigned g_surfCreated = 0, g_surfReused = 0, g_retired = 0;

void spare_keep(Target &t)
{
    if (!t.tex)
        return;
    const uint64_t bytes = target_bytes(t);
    if (g_spares.size() >= kSpareCount || g_spareBytes + bytes > kSpareBytes)
        return;                             /* (released with the target) */
    Spare s;
    s.depth = t.depth;
    s.nw = t.nw;
    s.nh = t.nh;
    s.samples = std::max(1, t.samples);
    s.bytes = bytes;
    s.at = g_frameNo;
    s.tex = std::move(t.tex);
    s.rtv = std::move(t.rtv);
    s.dsv = std::move(t.dsv);
    s.dsvRead = std::move(t.dsvRead);
    s.srv = std::move(t.srv);
    s.srvMs = std::move(t.srvMs);
    g_spareBytes += bytes;
    g_spares.push_back(std::move(s));
}

bool spare_take(Target &t)
{
    for (size_t i = 0; i < g_spares.size(); i++) {
        Spare &s = g_spares[i];
        if (s.depth != t.depth || s.nw != t.nw || s.nh != t.nh || s.samples != std::max(1, t.samples))
            continue;
        t.tex = std::move(s.tex);
        t.rtv = std::move(s.rtv);
        t.dsv = std::move(s.dsv);
        t.dsvRead = std::move(s.dsvRead);
        t.srv = std::move(s.srv);
        t.srvMs = std::move(s.srvMs);
        g_spareBytes -= s.bytes;
        g_spares.erase(g_spares.begin() + (ptrdiff_t)i);
        g_surfReused++;
        return true;
    }
    return false;
}

bool create(Target &t)
{
    const UINT n = (UINT)std::max(1, t.samples);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)t.nw;
    d.Height = (UINT)t.nh;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = n;
    d.Usage = D3D11_USAGE_DEFAULT;
    t.srv.reset();
    t.srvMs.reset();
    t.rectGen = 0;                          /* (the next copy's refresh whole) */
    /* PC (2026-11-13): a retired target's surface and views of this kind,
     * size and sample count, if one is at hand (cleared below as a new one) */
    const bool spare = spare_take(t);
    if (!spare)
        g_surfCreated++;
    if (!t.depth) {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (!spare) {
            if (FAILED(dev()->CreateTexture2D(&d, nullptr, t.tex.put())))
                return false;
            if (FAILED(dev()->CreateRenderTargetView(t.tex.get(), nullptr, t.rtv.put())))
                return false;
            if (FAILED(dev()->CreateShaderResourceView(t.tex.get(), nullptr, n == 1 ? t.srv.put() : t.srvMs.put())))
                return false;
        }
        ID3D11RenderTargetView *rtv = t.rtv.get();
        const uint32_t capId = cap_record(t, cap::K_CLEAR, "a colour target made: cleared", 0);
        q::run([rtv, capId] {
            const float zero[4] = {0, 0, 0, 0};
            cap::gpu_begin(capId);
            qdc()->ClearRenderTargetView(rtv, zero);
            cap::gpu_end(capId);
        });
        /* PC (2026-11-05): its alpha bound (Target::alphaLo): cleared, so 0 -
         * set on the submit stage, in order with the draws into it */
        Target *tp = &t;
        sq::run([tp] { tp->alphaLo = tp->alphaHi = 0; });
        t.alphaPos = sq::head();            /* PC: (the submit workers wait for it: Target::alphaPos) */
        t.execAlpha = 0;                    /* PC (2026-11-06): (the executor's note of it: cleared, so 0) */
    } else {
        d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (!spare) {
            if (FAILED(dev()->CreateTexture2D(&d, nullptr, t.tex.put())))
                return false;
            D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
            dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            dv.ViewDimension = n == 1 ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DMS;
            if (FAILED(dev()->CreateDepthStencilView(t.tex.get(), &dv, t.dsv.put())))
                return false;
            t.dsvRead.reset();
            if (n == 1) {
                /* PC (2026-11-05): the depth read-only, the stencil written: bound
                 * with the texture below by the draws whose alpha mirror pass is
                 * folded in (pgl_exec.cpp draw_passes) */
                dv.Flags = D3D11_DSV_READ_ONLY_DEPTH;
                if (FAILED(dev()->CreateDepthStencilView(t.tex.get(), &dv, t.dsvRead.put())))
                    t.dsvRead.reset();
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            sv.ViewDimension = n == 1 ? D3D11_SRV_DIMENSION_TEXTURE2D : D3D11_SRV_DIMENSION_TEXTURE2DMS;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(dev()->CreateShaderResourceView(t.tex.get(), &sv, n == 1 ? t.srv.put() : t.srvMs.put())))
                return false;
        }
        ID3D11DepthStencilView *dsv = t.dsv.get();
        const uint32_t capId = cap_record(t, cap::K_CLEAR, "a depth target made: cleared", 0);
        q::run([dsv, capId] {
            cap::gpu_begin(capId);
            qdc()->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
            cap::gpu_end(capId);
        });
    }
    return true;
}

/* The GS pixels the target's native pixels stand for. */
void set_geometry(Target &t, uint32_t fbw, int gsW, int gsH)
{
    const float sx = (float)t.nw / (float)gsW, sy = (float)t.nh / (float)gsH;
    if (t.fbw != fbw || t.gsW != gsW || t.gsH != gsH || t.sx != sx || t.sy != sy)
        g_targetsGen++;                     /* PC: (the lookups' caches) */
    t.fbw = fbw;
    t.gsW = gsW;
    t.gsH = gsH;
    t.sx = sx;
    t.sy = sy;
}

/* A depth target takes the GS geometry of the frame that wrote it (logged
 * once per change: a Z buffer shared by frames of two GS sizes, e.g. bufB38 /
 * unkAC4.buf[1] at block 0x8C0, 640 x 224 in the menus' car renders, 640 x
 * 448 in the races). */
void depth_geometry(Target &t, uint32_t fbw, int gsW, int gsH, const char *what)
{
    if (t.gsW == gsW && t.gsH == gsH && t.fbw == fbw)
        return;
    char key[64];
    snprintf(key, sizeof(key), "pgl-depth-geom-%X-%dx%d-%dx%d", t.base, t.gsW, t.gsH, gsW, gsH);
    log_once(key, "pgl: Z buffer at block 0x%X now holds a %dx%d %s (it held a %dx%d one): its texels follow",
             t.base, gsW, gsH, what, t.gsW, t.gsH);
    if (depth_log())
        pclog::write("pgl depth: frame %llu: Z buffer at block 0x%X (native %dx%d) now holds a %dx%d %s, FBW %u"
                     " (it held a %dx%d one, FBW %u)", (unsigned long long)g_frameNo, t.base, t.nw, t.nh, gsW,
                     gsH, what, fbw, t.gsW, t.gsH, t.fbw);
    set_geometry(t, fbw, gsW, gsH);
}

/* PC (2026-11-13): the targets made since the last report (next_frame's
 * churn line): how many, their bytes, the last one. */
struct MadeCount {
    int count = 0;
    uint64_t bytes = 0;
    uint32_t base = 0;
    int gsW = 0, gsH = 0, nw = 0, nh = 0, samples = 0;
    bool depth = false;
};
MadeCount g_made;

Target *make(uint32_t base, bool depth, uint32_t fbw, uint32_t psm, int gsW, int gsH, int nw, int nh,
             int samples)
{
    std::unique_ptr<Target> t(new Target());
    t->base = base;
    t->depth = depth;
    t->psm = psm;
    t->nw = nw;
    t->nh = nh;
    t->samples = samples;
    set_geometry(*t, fbw, gsW, gsH);
    t->lastUse = g_frameNo;
    if (!create(*t)) {
        log_once("pgl-target-create", "pgl: could not create a %dx%d render target", nw, nh);
        return nullptr;
    }
    g_made.count++;                         /* PC (2026-11-13): (the churn line) */
    g_made.bytes += target_bytes(*t);
    g_made.base = base;
    g_made.gsW = gsW;
    g_made.gsH = gsH;
    g_made.nw = nw;
    g_made.nh = nh;
    g_made.samples = samples;
    g_made.depth = depth;
    g_targets.push_back(std::move(t));
    g_targetsGen++;
    if (flicker_on())
        flicker_event("%s target made at 0x%X: GS %dx%d, native %dx%d, %d sample(s)", depth ? "a depth" : "a colour",
                      base, gsW, gsH, nw, nh, samples);
    return g_targets.back().get();
}

void size_of(uint32_t base, int fallbackW, int fallbackH, int &w, int &h)
{
    auto it = g_known.find(base);
    if (it != g_known.end()) {
        w = it->second.w;
        h = it->second.h;
    } else {
        w = fallbackW;
        h = fallbackH;
    }
    w = std::max(1, std::min(w, 2048));
    h = std::max(1, std::min(h, 2048));
}

}  /* namespace */

void targets_init()
{
    picture_scales(0.0f, g_scaleX, g_scaleY);   /* the 4:3 picture (follow_output) */
    host_text_scale(g_scaleX, g_scaleY);    /* PC: (the vector fonts' rasterizing) */
}

float output_scale()
{
    return g_scaleY;
}

void target_size(uint32_t base, uint32_t fbwPixels, uint32_t psm, int w, int h)
{
    (void)fbwPixels;
    if (w <= 0 || h <= 0)
        return;
    Known &k = g_known[base];
    /* (PC 2026-11-14: the detail stays with the same size: target_detail
     * comes before the buffer is set as the draw buffer) */
    const bool same = k.w == w && k.h == h;
    const float dx = same ? k.dx : 1.0f, dy = same ? k.dy : 1.0f;
    if (!same)
        g_targetsGen++;                     /* PC: (the lookups' caches) */
    k = Known{w, h, psm, dx, dy};
}

void target_detail(uint32_t base, int w, int h, float dx, float dy)
{
    if (w <= 0 || h <= 0)
        return;
    dx = std::max(1.0f, std::min(dx, 16.0f));
    dy = std::max(1.0f, std::min(dy, 16.0f));
    Known &k = g_known[base];
    if (k.w != w || k.h != h || k.dx != dx || k.dy != dy)
        g_targetsGen++;                     /* (the lookups' caches) */
    if (k.w != w || k.h != h)
        k.psm = 0;
    k.w = w;
    k.h = h;
    k.dx = dx;
    k.dy = dy;
}

Target *color_target(uint32_t base, uint32_t fbw, uint32_t psm, int fallbackW, int fallbackH)
{
    /* PC (CPU time): the last lookup again (g_targetsGen) */
    if (g_colorHit.gen == g_targetsGen && g_colorHit.base == base && g_colorHit.fallbackW == fallbackW &&
        g_colorHit.fallbackH == fallbackH) {
        Target *t = g_colorHit.t;
        t->lastUse = g_frameNo;
        t->freedAt = 0;                     /* PC (2026-11-13): (drawn again: target_freed) */
        t->fbw = fbw;
        t->psm = psm;
        return t;
    }
    int w, h;
    size_of(base, fallbackW, fallbackH, w, h);
    /* PC (2026-11-14): the buffer's detail (target_detail; 1 for most) */
    float dx = 1.0f, dy = 1.0f;
    {
        auto it = g_known.find(base);
        if (it != g_known.end() && it->second.w == w && it->second.h == h) {
            dx = it->second.dx;
            dy = it->second.dy;
        }
    }
    Target *found = nullptr;
    for (auto &t : g_targets) {
        if (!t->depth && t->base == base && t->gsW == w && t->gsH == h && t->detailX == dx && t->detailY == dy) {
            t->lastUse = g_frameNo;
            t->freedAt = 0;
            t->fbw = fbw;
            t->psm = psm;
            found = t.get();
            break;
        }
    }
    if (found == nullptr) {
        int nw, nh;
        native_size(w, h, nw, nh, dx, dy);
        found = make(base, false, fbw, psm, w, h, nw, nh, g_samples);
        if (found == nullptr)
            return nullptr;
        found->detailX = dx;
        found->detailY = dy;
        if (dx != 1.0f || dy != 1.0f) {
            char key[64];
            snprintf(key, sizeof(key), "pgl-detail-%dx%d-%.2fx%.2f", w, h, dx, dy);
            log_once(key, "pgl: the %dx%d buffer at block 0x%X drawn at %.2f x %.2f times the output scale (native"
                          " %dx%d; [Graphics] ReflectionResolution)", w, h, base, dx, dy, found->nw, found->nh);
        }
    }
    g_colorHit = ColorHit{g_targetsGen, base, fallbackW, fallbackH, found};
    return found;
}

Target *find_color_target_sized(uint32_t base, int w, int h)
{
    for (auto &t : g_targets) {
        if (!t->depth && t->base == base && t->gsW == w && t->gsH == h)
            return t.get();
    }
    return nullptr;
}

/* By base and native size only: the 640 x 224 field buffers and the 640 x
 * 448 race screen share one native depth, as they share the GS's Z words.
 * Its GS geometry is set when a draw writes it (depth_target_written).  Its
 * sample count is the colour target's (D3D11 binds them together). */
Target *depth_target(uint32_t zbase, uint32_t zpsm, const Target &color)
{
    /* PC (CPU time): the last lookup again (g_targetsGen) */
    if (g_depthHit.gen == g_targetsGen && g_depthHit.base == zbase && g_depthHit.nw == color.nw &&
        g_depthHit.nh == color.nh && g_depthHit.samples == color.samples) {
        Target *t = g_depthHit.t;
        t->lastUse = g_frameNo;
        t->freedAt = 0;
        t->psm = zpsm;
        return t;
    }
    for (auto &t : g_targets) {
        if (t->depth && t->base == zbase && t->nw == color.nw && t->nh == color.nh &&
            t->samples == color.samples) {
            t->lastUse = g_frameNo;
            t->freedAt = 0;
            t->psm = zpsm;
            g_depthHit = DepthHit{g_targetsGen, zbase, color.nw, color.nh, color.samples, t.get()};
            return t.get();
        }
    }
    for (auto &t : g_targets) {
        if (t->depth && t->base == zbase) {
            log_once("pgl-depth-sizes", "pgl: Z buffer at block 0x%X used with targets of two sizes (%dx%d, %dx%d):"
                     " separate native depth buffers", zbase, t->nw, t->nh, color.nw, color.nh);
            break;
        }
    }
    return make(zbase, true, color.fbw, zpsm, color.gsW, color.gsH, color.nw, color.nh, color.samples);
}

/* Drawn as a frame, the depth target takes the frame's GS geometry at once:
 * the draw's vertices are converted with it (sx, sy). */
Target *depth_frame_target(uint32_t base, uint32_t fbw, uint32_t psm, int fallbackW, int fallbackH)
{
    int w, h;
    size_of(base, fallbackW, fallbackH, w, h);
    int nw, nh;
    native_size(w, h, nw, nh);
    for (auto &t : g_targets) {
        if (t->depth && t->base == base && t->nw == nw && t->nh == nh) {
            t->lastUse = g_frameNo;
            t->freedAt = 0;
            depth_geometry(*t, fbw, w, h, "Z-format frame");
            return t.get();
        }
    }
    return make(base, true, fbw, psm, w, h, nw, nh, g_samples);
}

void depth_target_written(Target &z, const Target &color)
{
    depth_geometry(z, color.fbw, color.gsW, color.gsH, "frame's depth");
}

bool depth_log()
{
    static int state = -1;
    static int lines = 0;
    if (state < 0) {
        const char *e = getenv("GT3PC_DEPTH_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging depth target geometry and Z texture reads (GT3PC_DEPTH_LOG, 600 lines)");
    }
    return state == 1 && lines++ < 600;
}

bool flare_log(bool line)
{
    static int state = -1;
    static int lines = 0;
    if (state < 0) {
        const char *e = getenv("GT3PC_FLARE_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging readbacks and flare glow texels (GT3PC_FLARE_LOG, 600 lines)");
    }
    if (state != 1 || lines >= 600)
        return false;
    if (line)
        lines++;
    return true;
}

static Target *find_latest(uint32_t base, bool depth)
{
    Target *best = nullptr;
    for (auto &t : g_targets) {
        if (t->depth == depth && t->base == base && (best == nullptr || t->lastUse > best->lastUse))
            best = t.get();
    }
    return best;
}

Target *find_color_target(uint32_t base) { return find_latest(base, false); }
Target *find_depth_target(uint32_t base) { return find_latest(base, true); }

/* A multisampled depth target's resolve: its sample 0 of every pixel
 * written into the copy's depth by a full-screen pass (D3D11 resolves no
 * depth format). */
static void resolve_depth(Target &t)
{
    /* (queued: the D3D thread, pgl_queue.h; the state objects found here) */
    d3d_invalidate();
    const uint32_t capId = cap_record(t, cap::K_RESOLVE, "depth resolve (a pass)", 3);
    ID3D11DepthStencilView *dsv = t.copyDsv.get();
    const D3D11_VIEWPORT vp = {0, 0, (float)t.nw, (float)t.nh, 0, 1};
    ID3D11RasterizerState *rs = raster_state(0, false);
    ID3D11BlendState *bs = blend_plain(0);
    ID3D11DepthStencilState *ds = depth_state(false, pgl::gs::Compare::Always, true);
    q::run([dsv, vp, rs, bs, ds] {
        ID3D11ShaderResourceView *none = nullptr;
        qdc()->PSSetShaderResources(0, 1, &none);
        qdc()->OMSetRenderTargets(0, nullptr, dsv);
        qdc()->RSSetViewports(1, &vp);
        qdc()->RSSetState(rs);
        qdc()->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(ds, 0);
    });
    bind_vs(Vs::Full);
    bind_ps(Ps::DepthResolve);
    ID3D11ShaderResourceView *srv = t.srvMs.get();
    q::run([srv, capId] {
        ID3D11ShaderResourceView *none = nullptr;
        qdc()->PSSetShaderResources(0, 1, &srv);
        qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cap::gpu_begin(capId);
        qdc()->Draw(3, 0);
        cap::gpu_end(capId);
        qdc()->PSSetShaderResources(0, 1, &none);
        qdc()->OMSetRenderTargets(0, nullptr, nullptr);
    });
    d3d_invalidate();
}

uint32_t g_copiesWhole, g_copiesPart;
uint32_t g_copiesWhy[COPY_WHY_N];
/* PC (2026-11-06): the copies' reasons by name (the frame capture's notes) */
static const char *const kCopyWhyNames[COPY_WHY_N] = {"copy: other", "copy: DATE", "copy: C = Ad",
                                                      "copy: a target texture", "copy: a Z texture"};

/* A multisampled colour target's resolve where it was written since the
 * copy (Target::dirty*): the samples' mean by a pass scissored to it (D3D11
 * resolves whole surfaces only). */
static void resolve_color_rect(Target &t)
{
    /* (queued: the D3D thread, pgl_queue.h; the state objects found here) */
    d3d_invalidate();
    const uint32_t capId = cap_record(t, cap::K_RESOLVE, "colour resolve of the part written (a pass)", 3);
    ID3D11RenderTargetView *rtv = t.copyRtv.get();
    const D3D11_VIEWPORT vp = {0, 0, (float)t.nw, (float)t.nh, 0, 1};
    const D3D11_RECT r = {t.dirtyL, t.dirtyT, t.dirtyR, t.dirtyB};
    ID3D11RasterizerState *rs = raster_state(0, true);
    ID3D11BlendState *bs = blend_plain(15);
    ID3D11DepthStencilState *ds = depth_state(false, pgl::gs::Compare::Always, false);
    q::run([rtv, vp, r, rs, bs, ds] {
        ID3D11ShaderResourceView *none2[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none2);   /* (the copy may be bound as a draw's texture / DATE source) */
        qdc()->OMSetRenderTargets(1, &rtv, nullptr);
        qdc()->RSSetViewports(1, &vp);
        qdc()->RSSetScissorRects(1, &r);
        qdc()->RSSetState(rs);
        qdc()->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(ds, 0);
    });
    bind_vs(Vs::Full);
    bind_ps(Ps::ColorResolve);
    ID3D11ShaderResourceView *srv = t.srvMs.get();
    q::run([srv, capId] {
        ID3D11ShaderResourceView *none = nullptr;
        qdc()->PSSetShaderResources(0, 1, &srv);
        qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cap::gpu_begin(capId);
        qdc()->Draw(3, 0);
        cap::gpu_end(capId);
        qdc()->PSSetShaderResources(0, 1, &none);
        qdc()->OMSetRenderTargets(0, nullptr, nullptr);
    });
    d3d_invalidate();
}

void target_written_rect(Target &t, int l, int top, int r, int b)
{
    if (t.rectGen != t.gen) {
        /* written since its copy without a place (or never tracked): all */
        t.dirtyL = t.dirtyT = 0;
        t.dirtyR = t.nw;
        t.dirtyB = t.nh;
    } else {
        l = std::max(l, 0);
        top = std::max(top, 0);
        r = std::min(r, t.nw);
        b = std::min(b, t.nh);
        if (r > l && b > top) {
            if (t.dirtyR <= t.dirtyL || t.dirtyB <= t.dirtyT) {
                t.dirtyL = l;
                t.dirtyT = top;
                t.dirtyR = r;
                t.dirtyB = b;
            } else {
                t.dirtyL = std::min(t.dirtyL, l);
                t.dirtyT = std::min(t.dirtyT, top);
                t.dirtyR = std::max(t.dirtyR, r);
                t.dirtyB = std::max(t.dirtyB, b);
            }
        }
    }
    t.gen++;
    t.rectGen = t.gen;
}

/* The copy (single-sampled), brought up to date: a copy of a single-sampled
 * target, the resolve of a multisampled one (colour: the samples' mean;
 * depth: sample 0).  A colour target written since only in a known part
 * (Target::dirty*) has that part refreshed - not for a whole-picture read
 * (whole: FXAA's source, the multisampled pictures other targets read): the
 * anti-aliased race views lost pieces of the cars (a roof, a tail light,
 * the wheels as they turned) wherever the copy kept what an earlier refresh
 * had seen (user report), so those refresh it all. */
ID3D11ShaderResourceView *target_copy_srv(Target &t, bool whole, int why)
{
    ProfScope prof(PROF_TARGETS);           /* (the verbose monitor's profile) */
    const bool ms = t.samples > 1;
    if (!t.copy) {
        D3D11_TEXTURE2D_DESC d;
        t.tex->GetDesc(&d);
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        /* (PC: a render-target bind flag on the copy, tried for a copy by a
         * pass, had CopySubresourceRegion into it stall the driver for
         * tens of ms a frame - 2026-11-03) */
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (ms && t.depth ? D3D11_BIND_DEPTH_STENCIL : 0) |
                      (ms && !t.depth ? D3D11_BIND_RENDER_TARGET : 0);
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, t.copy.put())))
            return t.srv.get();
        if (ms && !t.depth && FAILED(dev()->CreateRenderTargetView(t.copy.get(), nullptr, t.copyRtv.put())))
            t.copyRtv.reset();              /* (refreshed whole) */
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = t.depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(dev()->CreateShaderResourceView(t.copy.get(), &sv, t.copySrv.put()))) {
            t.copy.reset();
            return t.srv.get();
        }
        if (ms && t.depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
            dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            if (FAILED(dev()->CreateDepthStencilView(t.copy.get(), &dv, t.copyDsv.put()))) {
                t.copy.reset();
                t.copySrv.reset();
                return t.srv.get();
            }
        }
        t.copyGen = 0;
    }
    if (t.copyGen != t.gen) {
        const int prevSec = exec_section_want(PGLR_SEC_TARGET_COPY);    /* PC: (the GPU sections) */
        if (why >= 0 && why < COPY_WHY_N)
            g_copiesWhy[why]++;
        /* PC: only the part written since, when known (Target::dirty*);
         * GT3PC_PGL_COPY_WHOLE=1: all of it every time (a measurement: the
         * part's edges against the whole) */
        static const bool wholeAlways = getenv("GT3PC_PGL_COPY_WHOLE") != nullptr &&
                                        atoi(getenv("GT3PC_PGL_COPY_WHOLE")) > 0;
        const bool part = !whole && !wholeAlways && !t.depth && t.copyGen != 0 && t.rectGen == t.gen &&
                          !(t.dirtyL <= 0 && t.dirtyT <= 0 && t.dirtyR >= t.nw && t.dirtyB >= t.nh) &&
                          (!ms || (t.copyRtv && t.srvMs));
        if (part) {
            if (t.dirtyR > t.dirtyL && t.dirtyB > t.dirtyT) {
                if (!ms) {
                    /* PC (a measurement, 2026-11-03): GT3PC_PGL_DATE_COPY=4 -
                     * 16 x 16 pixels of the part instead of all of it (the
                     * picture wrong): what the copies' bytes cost against
                     * their waits for the draws before them */
                    static const bool tiny = getenv("GT3PC_PGL_DATE_COPY") != nullptr &&
                                             strcmp(getenv("GT3PC_PGL_DATE_COPY"), "4") == 0;
                    const D3D11_BOX box = {(UINT)t.dirtyL, (UINT)t.dirtyT, 0,
                                           tiny ? (UINT)std::min(t.dirtyL + 16, t.dirtyR) : (UINT)t.dirtyR,
                                           tiny ? (UINT)std::min(t.dirtyT + 16, t.dirtyB) : (UINT)t.dirtyB, 1};
                    ID3D11Texture2D *dst = t.copy.get(), *src = t.tex.get();
                    const uint32_t capId = cap_record(t, cap::K_COPY, kCopyWhyNames[why >= 0 && why < COPY_WHY_N ? why : 0],
                                                      (uint32_t)((box.right - box.left) * (box.bottom - box.top)));
                    q::run([dst, src, box, capId] {
                        cap::gpu_begin(capId);
                        qdc()->CopySubresourceRegion(dst, 0, box.left, box.top, 0, src, 0, &box);
                        cap::gpu_end(capId);
                    });
                } else {
                    resolve_color_rect(t);
                }
                g_copiesPart++;
            }
        } else if (!ms) {
            ID3D11Texture2D *dst = t.copy.get(), *src = t.tex.get();
            const uint32_t capId = cap_record(t, cap::K_COPY, kCopyWhyNames[why >= 0 && why < COPY_WHY_N ? why : 0],
                                              (uint32_t)(t.nw * t.nh));
            q::run([dst, src, capId] {
                cap::gpu_begin(capId);
                qdc()->CopyResource(dst, src);
                cap::gpu_end(capId);
            });
        } else if (!t.depth) {
            ID3D11Texture2D *dst = t.copy.get(), *src = t.tex.get();
            const uint32_t capId = cap_record(t, cap::K_RESOLVE, "colour resolve, whole", (uint32_t)(t.nw * t.nh));
            q::run([dst, src, capId] {
                cap::gpu_begin(capId);
                qdc()->ResolveSubresource(dst, 0, src, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
                cap::gpu_end(capId);
            });
        } else {
            resolve_depth(t);
        }
        if (!part)
            g_copiesWhole++;
        t.copyGen = t.gen;
        t.rectGen = t.gen;
        t.dirtyL = t.dirtyT = t.dirtyR = t.dirtyB = 0;
        if (prevSec >= 0)
            exec_section_want(prevSec);
    }
    return t.copySrv.get();
}

ID3D11ShaderResourceView *target_srv(Target &t)
{
    return t.samples > 1 ? target_copy_srv(t, true) : t.srv.get();
}

ID3D11Texture2D *target_tex1(Target &t)
{
    if (t.samples > 1) {
        target_copy_srv(t, true);
        return t.copy.get();
    }
    return t.tex.get();
}


/* The copy and the resolve go with the resources (re-made at the next use). */
static void drop_copy(Target &t)
{
    t.copy.reset();
    t.copySrv.reset();
    t.copyDsv.reset();
    t.copyRtv.reset();
    t.copyGen = 0;
}

/* PC (2026-11-04): the alpha mirror (Target::mirror*; docs/renderer.md 3,
 * "DATE").  A colour target's mirror lives in the stencil of the depth
 * target it is drawn with; a draw without one binds that depth target with
 * the depth test off (its depth untouched), or, when the target has never
 * had one of its size, a stencil buffer of the target's own.  A depth target
 * holds one colour target's mirror at a time (mirrorOf): the frames of a
 * double-buffered screen share a Z buffer, so each frame's mirror is lost
 * to the other's and synced again at its first DATE draw - one copy and one
 * pass a frame, where the DATE draws took one copy each run. */
void target_mirror_invalidate(Target &t)
{
    t.mirrorValid = false;
}

void targets_mirror_invalidate_all()
{
    for (auto &tp : g_targets)
        tp->mirrorValid = false;
}

/* PC (2026-11-06, a measurement: GT3PC_PGL_BOUND_LOG=1, pgl_exec.cpp): the
 * alpha bound of every colour target drawn this frame, as the frame ends */
void targets_log_alpha_bounds()
{
    char line[1024];
    int at = snprintf(line, sizeof(line), "pgl bound: frame %llu, the colour targets' alpha bounds:",
                      (unsigned long long)g_frameNo);
    for (auto &tp : g_targets) {
        const Target &t = *tp;
        if (t.depth || t.lastUse != g_frameNo)
            continue;
        if (t.alphaLo < 0)
            at += snprintf(line + at, sizeof(line) - at, " 0x%X %dx%d unknown;", t.base, t.gsW, t.gsH);
        else
            at += snprintf(line + at, sizeof(line) - at, " 0x%X %dx%d %d..%d;", t.base, t.gsW, t.gsH, t.alphaLo,
                           t.alphaHi);
        if (at > (int)sizeof(line) - 64)
            break;
    }
    pclog::write("%s", line);
}

static void mirror_forget(Target &gone)
{
    for (auto &tp : g_targets) {
        Target &o = *tp;
        if (o.mirrorZ == &gone) {
            o.mirrorZ = nullptr;
            o.mirrorValid = false;
        }
        if (o.mirrorOf == &gone)
            o.mirrorOf = nullptr;
    }
}

/* PC (2026-11-06): claim - the caller will write the stencil (a DATE draw,
 * which syncs it; a covering flat alpha write that makes the mirror valid):
 * the depth target's stencil becomes t's, the target it held before loses
 * its mirror.  Without it (any other draw: it keeps no mirror while the
 * stencil is another's, so writes no stencil) the stencil stays whose it
 * was.  Until then every draw into a target a DATE draw had ever read
 * claimed it: the frame, the display and a replay's blur buffers share one
 * depth target of the frame's size, so their turns cost the frame's mirror
 * a sync each - the user's F3 view counted four a frame in a replay,
 * 1.4 ms of GPU at 150 % - while none of those draws touched the stencil. */
ID3D11DepthStencilView *target_mirror_dsv(Target &t, Target *z, bool claim)
{
    if (t.depth)
        return nullptr;
    if (z != nullptr) {
        if (t.mirrorZ != z) {
            t.mirrorZ = z;
            t.mirrorValid = false;
        }
        if (z->mirrorOf != &t) {
            if (!claim) {
                t.mirrorValid = false;      /* (the stencil is another's: t keeps nothing) */
                return z->dsv.get();
            }
            if (z->mirrorOf != nullptr)
                z->mirrorOf->mirrorValid = false;   /* (its mirror is overwritten from now on) */
            z->mirrorOf = &t;
            t.mirrorValid = false;
        }
        return z->dsv.get();
    }
    Target *p = t.mirrorZ;
    if (p != nullptr && p->mirrorOf == &t && p->nw == t.nw && p->nh == t.nh && p->samples == t.samples && p->dsv)
        return p->dsv.get();
    /* the target's own stencil buffer (its depth never read or written) */
    if (t.mirrorTex) {
        D3D11_TEXTURE2D_DESC have;
        t.mirrorTex->GetDesc(&have);
        if ((int)have.Width != t.nw || (int)have.Height != t.nh || (int)have.SampleDesc.Count != std::max(1, t.samples)) {
            t.mirrorTex.reset();
            t.mirrorDsv.reset();
        }
    }
    if (!t.mirrorDsv) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)t.nw;
        d.Height = (UINT)t.nh;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        d.SampleDesc.Count = (UINT)std::max(1, t.samples);
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = d.SampleDesc.Count == 1 ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DMS;
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, t.mirrorTex.put())) ||
            FAILED(dev()->CreateDepthStencilView(t.mirrorTex.get(), &dv, t.mirrorDsv.put()))) {
            t.mirrorTex.reset();
            t.mirrorDsv.reset();
            log_once("pgl-mirror-create", "pgl: no stencil buffer for the alpha mirror of a %dx%d target: its DATE"
                     " draws read a copy", t.nw, t.nh);
            return nullptr;
        }
        t.mirrorValid = false;
    }
    if (t.mirrorZ != nullptr) {
        if (t.mirrorZ->mirrorOf == &t)
            t.mirrorZ->mirrorOf = nullptr;
        t.mirrorZ = nullptr;
        t.mirrorValid = false;
    }
    return t.mirrorDsv.get();
}

/* PC: GS resolution for a colour target from now on (one native pixel per
 * GS pixel; it keeps that size for good).  For the small effect buffers the
 * game draws scan-masked rows into (T449's wheel-face blur: the face drawn
 * still on every row and turned on the odd rows of a w x 3w buffer, then
 * shrunk 2:1 by a bilinear sprite sampling exactly between the two rows):
 * at s native pixels per GS row the shrink's bilinear taps fall on two
 * native rows of one GS row - still or turned, not their mean - and the
 * face came out in stripes.  The flare occlusion queries' work buffer and
 * result strip (pgl_exec.cpp): their halvings and texel reads are exact on
 * GS pixels only.  What the target holds is resolved into the new resource
 * (linear or point samples at the GS pixel centres).  docs/renderer.md 5. */
/* Colour target t re-created at nw x nh native pixels and `samples' MSAA
 * samples, what it holds resampled into it (linear or point samples; a
 * multisampled one from its resolve).  False if that failed (t as it was). */
static bool resample(Target &t, int nw, int nh, bool linear, int samples)
{
    if (flicker_on())
        flicker_event("the colour target at 0x%X re-made: native %dx%d -> %dx%d, %d sample(s)", t.base, t.nw, t.nh,
                      nw, nh, samples);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)nw;
    d.Height = (UINT)nh;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = (UINT)std::max(1, samples);
    d.Usage = D3D11_USAGE_DEFAULT;
    /* (a multisampled one readable too, as create's: the resolve of a part,
     * target_copy_srv - without it every refresh resolved the whole
     * target, after the first rescale) */
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> tex;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11ShaderResourceView> srv, srvMs;
    if (FAILED(dev()->CreateTexture2D(&d, nullptr, tex.put())) ||
        FAILED(dev()->CreateRenderTargetView(tex.get(), nullptr, rtv.put())) ||
        FAILED(dev()->CreateShaderResourceView(tex.get(), nullptr, samples <= 1 ? srv.put() : srvMs.put())))
        return false;
    ID3D11ShaderResourceView *old = target_srv(t);
    if (old == nullptr)
        return false;
    /* the old pixels, as readback resolves them (DECAL, the RGBA bytes),
     * through a unit quad of its own: the dynamic vertex stream may hold the
     * vertices of the VU1 draw being set up */
    static Com<ID3D11Buffer> quad;
    if (!quad) {
        const GsVtx v[6] = {
            {0, 0, 0, 1, 0, 0, 0x80808080u, 255}, {1, 0, 0, 1, 1, 0, 0x80808080u, 255},
            {0, 1, 0, 1, 0, 1, 0x80808080u, 255}, {1, 0, 0, 1, 1, 0, 0x80808080u, 255},
            {1, 1, 0, 1, 1, 1, 0x80808080u, 255}, {0, 1, 0, 1, 0, 1, 0x80808080u, 255},
        };
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(v);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = v;
        if (FAILED(dev()->CreateBuffer(&bd, &init, quad.put())))
            return false;
    }
    d3d_invalidate();
    set_targets(rtv.get(), nullptr, nw, nh);
    set_raster(raster_state(0, false));
    set_blend(blend_plain(15));
    set_depth(depth_state(false, pgl::gs::Compare::Always, false));
    set_vb(quad.get(), sizeof(GsVtx));
    set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    bind_vs(Vs::Gs);
    CbGs cg = {1, 1, 0, 0};
    bind_vs_cb(&cg, sizeof(cg));
    bind_ps(Ps::Main);
    CbPs cp = {};
    cp.texInvW = 1;
    cp.texInvH = 1;
    cp.flags = PSF_TME | (1u << PSF_TFX_SHIFT) | PSF_TCC;
    cp.rowScale = 1;
    bind_ps_cb(&cp, sizeof(cp));
    set_srv0(old);
    set_sampler0(sampler_state(linear, linear, 1, 1));
    q::run([] { qdc()->Draw(6, 0); });
    d3d_invalidate();
    t.tex = std::move(tex);
    t.rtv = std::move(rtv);
    t.srv = std::move(srv);
    t.srvMs = std::move(srvMs);
    drop_copy(t);
    t.samples = std::max(1, samples);
    t.nw = nw;
    t.nh = nh;
    set_geometry(t, t.fbw, t.gsW, t.gsH);
    g_targetsGen++;                         /* PC: (the lookups' caches: native size, samples) */
    t.gen++;
    t.execAlpha = -1;                       /* PC (2026-11-06): (resampled: its alpha no longer known flat) */
    target_mirror_invalidate(t);            /* PC: (the alpha mirror: synced again) */
    return true;
}

bool target_gs_resolution(Target &t, bool linear, const char *why)
{
    if (t.depth || t.gsW <= 0 || t.gsH <= 0)
        return false;
    t.gsRes = true;                         /* (the output scale and MSAA no longer apply: targets_rescale) */
    if ((t.nw == t.gsW && t.nh == t.gsH && t.samples == 1) || !resample(t, t.gsW, t.gsH, linear, 1))
        return false;
    char key[48];
    snprintf(key, sizeof(key), "pgl-gs-res-%dx%d", t.gsW, t.gsH);
    log_once(key, "pgl: the %dx%d buffer at block 0x%X %s: drawn at GS resolution from now on", t.gsW, t.gsH,
             t.base, why);
    return true;
}

/* PC (2026-11-06): [Graphics] EnvMapDetail (settings.h; docs/renderer.md 3,
 * "Environment map detail").  The cars' environment map: the course's env
 * view is copied into a 256 x 256 GS buffer and blurred there, 32 sprite
 * passes a frame, and the env passes (ENVMTX / ENVMAP) sample it for the car
 * bodies' reflections.  Drawn at the resolution scale like every target
 * (1152 x 926 at 150 %) the blur is 34 Mpx of pixel work a frame, 18 % of
 * the user's SSR5 frame, where the PS2 blurred 65k pixels.  `original': a
 * colour target of up to 256 x 256 GS pixels that an env pass sampled over
 * the frame just ended (Target::envRead, the executor's note of the draw's
 * program) is re-made at its GS size at the frame's end, what it holds
 * resampled, and stays there (gsRes, as the scan-masked effect buffers): the
 * reflections drawn from the PS2's texels, softer at a high scale; not exact
 * against the scaled picture, a frame with no env map the same.  One no env
 * pass samples any more goes back to the scale, as every one does when the
 * setting is `scaled'.  (A first form of the setting, "small buffers read by
 * sprites alone at their GS size", rested on a misreading of the capture -
 * the blurred buffer taken for a night glow - and took the rear-view mirror's
 * 64 x 128 scene buffer instead, read by one sprite; the user's log of which
 * program reads each buffer set it right.) */
void targets_env_maps()
{
    const bool on = settings::env_map_detail() == settings::EnvMapDetail::original;
    for (auto &tp : g_targets) {
        Target &t = *tp;
        const bool env = t.envRead;
        t.envRead = false;
        if (t.depth || t.gsW <= 0 || t.gsH <= 0)
            continue;
        const bool used = t.lastUse == g_frameNo;
        if (t.envNative) {
            if (on && (env || !used))
                continue;                   /* (kept while the setting holds and it is an env map, or rests this frame) */
            int nw, nh;
            native_size_of(t, nw, nh);
            t.envNative = false;
            t.gsRes = false;
            if ((nw != t.nw || nh != t.nh || t.samples != g_samples) && !resample(t, nw, nh, true, g_samples))
                log_once("pgl-envmap-back", "pgl: an environment map could not be re-made at the scale (0x%X)", t.base);
            continue;
        }
        if (!on || !env || !used || t.gsRes || t.gsW > 256 || t.gsH > 256)
            continue;
        char key[48];
        snprintf(key, sizeof(key), "pgl-envmap-%X-%dx%d", t.base, t.gsW, t.gsH);
        if (t.nw == t.gsW && t.nh == t.gsH && t.samples == 1) {
            t.envNative = true;             /* (at its GS size already: the scale is 100 %) */
            t.gsRes = true;
            continue;
        }
        if (resample(t, t.gsW, t.gsH, true, 1)) {
            t.envNative = true;
            t.gsRes = true;
            log_once(key, "pgl: the %dx%d environment map at block 0x%X at its GS size ([Graphics] EnvMapDetail=original)",
                     t.gsW, t.gsH, t.base);
        } else {
            log_once(key, "pgl: the %dx%d environment map at block 0x%X could not be re-made at its GS size"
                          " ([Graphics] EnvMapDetail=original)", t.gsW, t.gsH, t.base);
        }
    }
}

/* PC: the output scale follows the picture shown (the window resized, the
 * display mode changed; docs/renderer.md "Targets"): every target goes to
 * scale s.  Colour targets keep what they hold (resampled, bilinear: the
 * frame shown and those the effects read back); depth targets start cleared
 * (the frames clear them before use).  Those at GS resolution stay. */
static void targets_rescale(float sx, float sy)
{
    const float oldX = g_scaleX, oldY = g_scaleY;
    g_scaleX = sx;
    g_scaleY = sy;
    host_text_scale(sx, sy);                /* PC: (the vector fonts' rasterizing) */
    int n = 0;
    for (auto &tp : g_targets) {
        Target &t = *tp;
        if (t.gsRes || t.gsW <= 0 || t.gsH <= 0)
            continue;
        int nw, nh;
        native_size_of(t, nw, nh);              /* (PC 2026-11-14: with its detail) */
        if (nw == t.nw && nh == t.nh)
            continue;
        if (t.depth) {
            t.tex.reset();
            t.dsv.reset();
            t.dsvRead.reset();
            drop_copy(t);
            t.nw = nw;
            t.nh = nh;
            if (!create(t))
                log_once("pgl-target-create", "pgl: could not create a %dx%d render target", nw, nh);
            set_geometry(t, t.fbw, t.gsW, t.gsH);
            g_targetsGen++;                 /* PC: (the lookups' caches) */
            t.gen++;
            if (t.mirrorOf != nullptr)
                target_mirror_invalidate(*t.mirrorOf);  /* PC: (its stencil cleared) */
        } else if (!resample(t, nw, nh, true, t.samples)) {
            log_once("pgl-target-create", "pgl: could not create a %dx%d render target", nw, nh);
            continue;
        }
        n++;
    }
    pclog::write("pgl: output scale %.3f x %.3f -> %.3f x %.3f (%d targets at the new size; the screen %dx%d,"
                 " resolution scale %d%%)", oldX, oldY, sx, sy, n, (int)std::ceil(640.0f * sx - 0.01f),
                 (int)std::ceil(448.0f * sy - 0.01f), settings::render_scale());
}

/* MSAA (docs/renderer.md "Anti-aliasing"): the setting's sample count (the
 * most the device has up to it) for every target at the output scale - the
 * colour targets re-created with it, what they hold kept (point samples at
 * the same size, from the old resolve), the depth targets re-created
 * cleared (the frames clear them before use).  Those at GS resolution stay
 * single-sampled. */
static void frame_samples(int want);

/* PC (2026-11-15): AGSS (settings.h per_sample_shading) on for the frame -
 * its targets multisampled and the per-sample shaders there (pgl_d3d.h
 * per_sample_shaders) - for draw_passes, which then binds them for the 3D
 * (VU1) draws (pgl_exec.cpp) */
static std::atomic<bool> g_perSample{false};

bool per_sample_frame()
{
    return g_perSample.load(std::memory_order_relaxed);
}

void targets_frame_settings()
{
    const settings::AntiAliasing aa = settings::anti_aliasing();
    frame_samples(settings::msaa_samples(aa));
    const bool on = settings::per_sample_shading(aa) && g_samples > 1 && per_sample_shaders();
    if (on != g_perSample.load(std::memory_order_relaxed)) {
        g_perSample.store(on, std::memory_order_relaxed);
        pclog::write("pgl: AGSS %s", on ? "on: the 3D draws shaded at every sample" : "off");
    }
}

static void frame_samples(int want)
{
    static int s_want = -1;
    if (want == s_want)
        return;
    s_want = want;
    const int n = supported_samples(want);
    if (n != want)
        log_once("pgl-msaa-support", "pgl: %dx MSAA is not supported by the device: %dx", want, n);
    if (n == g_samples)
        return;
    const int old = g_samples;
    g_samples = n;
    int changed = 0;
    for (auto &tp : g_targets) {
        Target &t = *tp;
        if (t.gsRes || t.samples == n)
            continue;
        if (t.depth) {
            t.tex.reset();
            t.dsv.reset();
            t.dsvRead.reset();
            drop_copy(t);
            t.samples = n;
            if (!create(t))
                log_once("pgl-target-create", "pgl: could not create a %dx%d render target", t.nw, t.nh);
            g_targetsGen++;                 /* PC: (the lookups' caches: the samples) */
            t.gen++;
            if (t.mirrorOf != nullptr)
                target_mirror_invalidate(*t.mirrorOf);  /* PC: (its stencil cleared) */
        } else if (!resample(t, t.nw, t.nh, false, n)) {
            log_once("pgl-target-create", "pgl: could not create a %dx%d render target", t.nw, t.nh);
            continue;
        }
        changed++;
    }
    pclog::write("pgl: MSAA %dx -> %dx (%d targets re-created)", old, n, changed);
}

/* After each present: the scales of the picture shown (picture_scales),
 * applied once they have stayed the same for 15 frames (a window being
 * dragged larger is not resampled at every step); a new resolution scale
 * at once. */
static void follow_output()
{
    static float pendingX = 0.0f, pendingY = 0.0f;
    static int stable = 0;
    static int s_renderScale = -1;
    const int renderScale = settings::render_scale();
    const bool scaleChanged = s_renderScale >= 0 && renderScale != s_renderScale;
    s_renderScale = renderScale;
    float sx, sy;
    picture_scales(g_picAspect, sx, sy);
    if (std::fabs(sx - g_scaleX) < 0.001f && std::fabs(sy - g_scaleY) < 0.001f) {
        stable = 0;
        return;
    }
    if (scaleChanged) {
        stable = 0;
        pendingX = sx;
        pendingY = sy;
        targets_rescale(sx, sy);
        return;
    }
    if (std::fabs(sx - pendingX) >= 0.001f || std::fabs(sy - pendingY) >= 0.001f) {
        pendingX = sx;
        pendingY = sy;
        stable = 0;
        return;
    }
    if (++stable < 15)
        return;
    stable = 0;
    targets_rescale(sx, sy);
}

/* Before a frame executes: its picture (pglr_set_picture_aspect, recorded
 * with it).  A change between the 4:3 picture and the window's (a race view
 * begins or ends) rescales the targets at once, before the frame draws. */
void targets_frame_aspect(float aspect, bool multi)
{
    const bool race = aspect > 0.0f, was = g_picAspect > 0.0f;
    const bool wasMulti = g_picMulti;
    g_picAspect = race ? aspect : 0.0f;
    g_picMulti = race && multi;
    if (race == was && g_picMulti == wasMulti)
        return;
    float sx, sy;
    picture_scales(g_picAspect, sx, sy);
    if (std::fabs(sx - g_scaleX) >= 0.001f || std::fabs(sy - g_scaleY) >= 0.001f)
        targets_rescale(sx, sy);
}

/* ======================================================================== */
/* Presentation                                                             */
/* ======================================================================== */

/* What target t holds as a 24-bit BMP at path (native size). */
static bool write_target_bmp(Target &t, const char *path)
{
    ID3D11Texture2D *src = target_tex1(t);
    if (src == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    if (FAILED(dev()->CreateTexture2D(&d, nullptr, staging.put())))
        return false;
    dc()->CopyResource(staging.get(), src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dc()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
        return false;
    FILE *f = fopen(path, "wb");
    if (f != nullptr) {
        const uint32_t w = d.Width, h = d.Height;
        const uint32_t row = (w * 3 + 3) & ~3u;
        uint8_t hdr[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
        put32(2, 54 + row * h);
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, h);
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, row * h);
        fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> line(row, 0);
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src = (const uint8_t *)m.pData + (size_t)(h - 1 - y) * m.RowPitch;
            for (uint32_t x = 0; x < w; x++) {
                line[3 * x + 0] = src[4 * x + 2];
                line[3 * x + 1] = src[4 * x + 1];
                line[3 * x + 2] = src[4 * x + 0];
            }
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
    }
    dc()->Unmap(staging.get(), 0);
    return f != nullptr;
}

/* Debug aid: GT3PC_FRAME_DUMP=<directory> writes every GT3PC_FRAME_DUMP_EVERY-th
 * (default 60) displayed buffer there as frame_NNNNN.bmp (native size, RGB). */
static void dump_frame(Target &t)
{
    static int state = -1;
    static char dir[260];
    static unsigned every = 60;
    static unsigned count = 0;
    if (state < 0) {
        state = 0;
        const char *d = getenv("GT3PC_FRAME_DUMP");
        if (d != nullptr && d[0] != 0) {
            snprintf(dir, sizeof(dir), "%s", d);
            const char *e = getenv("GT3PC_FRAME_DUMP_EVERY");
            if (e != nullptr && atoi(e) > 0)
                every = (unsigned)atoi(e);
            state = 1;
            pclog::write("pgl: dumping every %u-th frame to %s", every, dir);
        }
    }
    if (state != 1 || (count++ % every) != 0)
        return;
    char path[300];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "frame_%05u.bmp", dir, count - 1);
    write_target_bmp(t, path);
}

/* GT3PC_PGL_TRACE: the picture of the frame F9 traced, at its present - the
 * displayed buffer at native size, trace_frame_N.bmp in GT3PC_TRACE_DIR (or
 * next to the executable) - and every other colour target of 128 x 128 GS
 * pixels or more the frame used, as it holds at the present,
 * trace_frame_N_<base>_<w>x<h>.bmp (an offscreen view, a blur pyramid: what
 * the picture was made of). */
static uint64_t g_traceDumpFrame = 0;

void trace_dump_next(uint64_t frame)
{
    g_traceDumpFrame = frame;
}

static void trace_dump(Target &t)
{
    char dir[MAX_PATH] = "";
    const char *e = getenv("GT3PC_TRACE_DIR");
    if (e != nullptr && e[0] != 0) {
        snprintf(dir, sizeof(dir), "%s", e);
    } else {
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char *slash = strrchr(dir, PGL_PATH_SEP[0]);
        if (slash != nullptr)
            *slash = 0;
    }
    char path[MAX_PATH + 48];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "trace_frame_%llu.bmp", dir, (unsigned long long)g_traceDumpFrame);
    if (write_target_bmp(t, path))
        pclog::write("pgl trace: frame %llu's picture (the buffer at 0x%X, %dx%d native, GS %dx%d; shown rows %d..%d,"
                     " columns %d..%d) in %s", (unsigned long long)g_traceDumpFrame, t.base, t.nw, t.nh, t.gsW, t.gsH,
                     g_disp.dy, g_disp.dy + g_disp.dh, g_disp.dx, g_disp.dx + g_disp.dw, path);
    for (auto &o : g_targets) {
        if (o.get() == &t || o->depth || o->gsW < 128 || o->gsH < 128 || o->lastUse + 1 < t.lastUse)
            continue;
        snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "trace_frame_%llu_%X_%dx%d.bmp", dir, (unsigned long long)g_traceDumpFrame,
                 o->base, o->gsW, o->gsH);
        if (write_target_bmp(*o, path))
            pclog::write("pgl trace: frame %llu's buffer at 0x%X (%dx%d native, GS %dx%d) in %s",
                         (unsigned long long)g_traceDumpFrame, o->base, o->nw, o->nh, o->gsW, o->gsH, path);
    }
    g_traceDumpFrame = 0;
}

/* PC (2026-11-06): the frame capture's pictures (pgl_capture.h): the frame's
 * displayed buffer (frame.bmp) and every colour target of 64 x 64 GS pixels
 * or more it used, as they hold at the present (target_<base>_<w>x<h>.bmp),
 * into the capture's directory */
static void capture_dump(Target &t, const char *dir)
{
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "frame.bmp", dir);
    int n = write_target_bmp(t, path) ? 1 : 0;
    for (auto &o : g_targets) {
        if (o.get() == &t || o->depth || o->gsW < 64 || o->gsH < 64 || o->lastUse + 1 < t.lastUse)
            continue;
        snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "target_%X_%dx%d.bmp", dir, o->base, o->gsW, o->gsH);
        if (write_target_bmp(*o, path))
            n++;
    }
    pclog::write("pgl capture: %d target picture(s) written to %s", n, dir);
}

/* PC (2026-11-15): the screenshot (platform/screenshot.h).  Asked for, the
 * shown part of the displayed buffer - native pixels, the buffer as the
 * picture's draw reads it (MSAA / AGSS: its resolve; FXAA: applied in place
 * before) - is copied at the present into a staging texture of its own; at the
 * presents after it is read once the GPU has made it (Map's DO_NOT_WAIT: no
 * frame waits for the GPU; after kShotTries a waited read), then handed to
 * screenshot::save, which writes it on a thread of its own.  One at a time:
 * another request waits for the next present after it. */
struct Shot {
    Com<ID3D11Texture2D> staging;
    int w = 0, h = 0;
    int tries = 0;
};
static Shot g_shot;
/* (racebench, the GPU frames behind with no swap chain: read 2 .. 22 presents
 * after; the game's swap chain keeps it ~3 frames behind at most) */
constexpr int kShotTries = 120;

static void screenshot_take(Target *t, ID3D11ShaderResourceView *picture, const CbPresent &shown)
{
    if (!screenshot::take_request())
        return;
    ID3D11Texture2D *src = t != nullptr && picture != nullptr ? target_tex1(*t) : nullptr;
    if (src == nullptr) {
        screenshot::failed(N_("NO PICTURE SHOWN"));
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    /* the shown part (gfx_set_display_area: shown's u, v) in native pixels */
    auto at = [](float f, UINT size) { return (UINT)std::max(0.0f, std::min((float)size, std::floor(f * (float)size + 0.5f))); };
    const UINT x0 = at(shown.u0, d.Width), x1 = at(shown.u1, d.Width);
    const UINT y0 = at(shown.v0, d.Height), y1 = at(shown.v1, d.Height);
    if (x1 <= x0 || y1 <= y0 || d.Format != DXGI_FORMAT_R8G8B8A8_UNORM || d.SampleDesc.Count != 1) {
        pclog::write("pgl: screenshot: the picture is %ux%u (format %u, %u samples), its shown part %u..%u x %u..%u",
                     d.Width, d.Height, (unsigned)d.Format, d.SampleDesc.Count, x0, x1, y0, y1);
        screenshot::failed(N_("THE PICTURE CANNOT BE READ"));
        return;
    }
    D3D11_TEXTURE2D_DESC s = {};
    s.Width = x1 - x0;
    s.Height = y1 - y0;
    s.MipLevels = 1;
    s.ArraySize = 1;
    s.Format = d.Format;
    s.SampleDesc.Count = 1;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    if (FAILED(dev()->CreateTexture2D(&s, nullptr, staging.put()))) {
        pclog::write("pgl: screenshot: no %ux%u staging texture", s.Width, s.Height);
        screenshot::failed(N_("NOT ENOUGH VIDEO MEMORY"));
        return;
    }
    const D3D11_BOX box = {x0, y0, 0, x1, y1, 1};
    dc()->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src, 0, &box);   /* (dc(): after the queued resolve) */
    g_shot.staging = std::move(staging);
    g_shot.w = (int)s.Width;
    g_shot.h = (int)s.Height;
    g_shot.tries = 0;
}

static void screenshot_read()
{
    if (!g_shot.staging)
        return;
    D3D11_MAPPED_SUBRESOURCE m;
    const bool wait = ++g_shot.tries >= kShotTries;
    const HRESULT hr = dc()->Map(g_shot.staging.get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return;                                 /* (the next present) */
    const int w = g_shot.w, h = g_shot.h;
    if (FAILED(hr)) {
        g_shot.staging.reset();
        pclog::write("pgl: screenshot: the %dx%d copy could not be read (0x%08X)", w, h, (unsigned)hr);
        screenshot::failed(N_("THE PICTURE COULD NOT BE READ"));
        return;
    }
    std::vector<uint8_t> px;
    bool ok = true;
    try {
        px.resize((size_t)w * h * 4);
    } catch (const std::bad_alloc &) {
        ok = false;
    }
    if (ok)
        for (int y = 0; y < h; y++)
            memcpy(px.data() + (size_t)y * w * 4, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    dc()->Unmap(g_shot.staging.get(), 0);
    g_shot.staging.reset();
    pclog::write("pgl: screenshot: the %dx%d picture read %d present(s) after its copy%s", w, h, g_shot.tries,
                 wait ? " (waited for)" : "");
    if (!ok) {
        pclog::write("pgl: screenshot: no memory for the %dx%d picture", w, h);
        screenshot::failed(N_("NOT ENOUGH MEMORY"));
        return;
    }
    screenshot::save(std::move(px), w, h);
}

/* fresh: a picture the game presents (not again: pglr_present_again) */
static void present_now(bool fresh = true)
{
    ID3D11RenderTargetView *bb = gpu::backbuffer();
    if (bb == nullptr)
        return;
    flicker_poll();                             /* (GT3PC_FLICKER: F8) */
    Target *t = (g_disp.valid && g_disp.enabled) ? find_color_target(g_disp.base) : nullptr;
    if (g_disp.valid && g_disp.enabled && t == nullptr)
        log_once("pgl-present-none", "pgl: the displayed buffer (block 0x%X) was never drawn: black", g_disp.base);
    ID3D11ShaderResourceView *picture = t != nullptr ? target_srv(*t) : nullptr;    /* (a resolve first) */
    d3d_invalidate();
    const float black[4] = {0, 0, 0, 1};
    /* the screens (gpu.h): a race frame drawn over the canvas shows each its
     * part (a multi-screen frame: pglr_set_picture_multi), any other frame
     * is the game window's, the others black */
    const int screens = gpu::screen_count();
    const bool multi = g_picAspect > 0.0f && g_picMulti && screens > 1;
    int canvasW = 1, canvasH = 1;
    gpu::canvas_size(&canvasW, &canvasH);
    /* the display area of the buffer (gfx_set_display_area); the display
     * offset (DISPLAY2.DX / DY, screen adjust) is not applied */
    CbPresent shown = {0, 0, 1, 1};
    if (t != nullptr && g_disp.dw > 0 && g_disp.dh > 0) {
        shown.u0 = std::min(1.0f, (float)g_disp.dx / (float)t->gsW);
        shown.v0 = std::min(1.0f, (float)g_disp.dy / (float)t->gsH);
        shown.u1 = std::min(1.0f, (float)(g_disp.dx + g_disp.dw) / (float)t->gsW);
        shown.v1 = std::min(1.0f, (float)(g_disp.dy + g_disp.dh) / (float)t->gsH);
    }
    for (int si = 0; si < screens; si++) {
        ID3D11RenderTargetView *rtv = si == 0 ? bb : gpu::screen_backbuffer(si);
        if (rtv == nullptr)
            continue;
        /* (queued: the D3D thread, pgl_queue.h; the state objects found here) */
        q::run([rtv, black] { qdc()->ClearRenderTargetView(rtv, black); });
        if (t == nullptr || picture == nullptr || (si > 0 && !multi))
            continue;
        int bw = 0, bh = 0;
        gpu::screen_size(si, &bw, &bh);
        /* the picture: a race frame at its aspect (the whole window), else
         * the largest 4:3 rectangle, black bars (picture_size); a part of
         * the canvas over the whole screen */
        int pw = bw, ph = bh;
        if (!multi)
            picture_size(g_picAspect, bw, bh, pw, ph);
        const float vw = (float)pw, vh = (float)ph;
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = std::floor(((float)bw - vw) * 0.5f);
        vp.TopLeftY = std::floor(((float)bh - vh) * 0.5f);
        vp.Width = vw;
        vp.Height = vh;
        vp.MaxDepth = 1.0f;
        ID3D11RasterizerState *rs = raster_state(0, false);
        ID3D11BlendState *bs = blend_plain(15);
        ID3D11DepthStencilState *ds = depth_state(false, pgl::gs::Compare::Always, false);
        q::run([rtv, vp, rs, bs, ds] {
            qdc()->OMSetRenderTargets(1, &rtv, nullptr);
            qdc()->RSSetViewports(1, &vp);
            qdc()->RSSetState(rs);
            qdc()->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
            qdc()->OMSetDepthStencilState(ds, 0);
        });
        bind_vs(Vs::Full);
        bind_ps(Ps::Present);
        CbPresent cb = shown;
        if (multi) {
            int cx = 0, cw = canvasW;
            gpu::canvas_rect(si, &cx, &cw);
            const float du = cb.u1 - cb.u0;
            const float a = (float)cx / (float)std::max(1, canvasW), b = (float)(cx + cw) / (float)std::max(1, canvasW);
            cb.u1 = cb.u0 + du * b;
            cb.u0 = cb.u0 + du * a;
        }
        /* a picture larger than the screen (resolution scale above 100 %):
         * each screen pixel the mean of its part, up to 16 x 16 taps (one
         * bilinear tap skipped texels from 2 : 1 on - shimmering edges, no
         * supersampling) */
        {
            const float rx = (cb.u1 - cb.u0) * (float)t->nw / vw, ry = (cb.v1 - cb.v0) * (float)t->nh / vh;
            if (rx > 1.01f || ry > 1.01f) {
                cb.tapsX = std::min(16.0f, std::max(1.0f, std::ceil(rx - 0.01f)));
                cb.tapsY = std::min(16.0f, std::max(1.0f, std::ceil(ry - 0.01f)));
                cb.stepU = (cb.u1 - cb.u0) / vw / cb.tapsX;
                cb.stepV = (cb.v1 - cb.v0) / vh / cb.tapsY;
            }
        }
        bind_ps_cb(&cb, sizeof(cb));
        ID3D11SamplerState *s = sampler_state(true, true, 1, 1);
        const uint32_t capId = cap_record(*t, cap::K_PRESENT, "the picture onto the screen", (uint32_t)(pw * ph));
        q::run([picture, s, capId] {
            ID3D11ShaderResourceView *none = nullptr;
            qdc()->PSSetShaderResources(0, 1, &picture);
            qdc()->PSSetSamplers(0, 1, &s);
            qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cap::gpu_begin(capId);
            qdc()->Draw(3, 0);
            cap::gpu_end(capId);
            qdc()->PSSetShaderResources(0, 1, &none);
        });
    }
    /* PC (2026-11-15): a screenshot (platform/screenshot.h): the copy of an
     * earlier one read once it is there, else this picture copied if one is
     * asked for (fresh or again: a paused game's too) */
    screenshot_read();
    if (!g_shot.staging)
        screenshot_take(t, picture, shown);
    if (t != nullptr && picture != nullptr)
        dump_frame(*t);
    if (fresh && g_traceDumpFrame != 0 && t != nullptr && picture != nullptr)
        trace_dump(*t);
    if (fresh && t != nullptr && picture != nullptr) {
        /* PC (2026-11-06): the frame capture's pictures of the frame's targets */
        const char *cdir = cap::dump_dir();
        if (cdir != nullptr)
            capture_dump(*t, cdir);
    }
    if (fresh && flicker_on()) {
        const float uv[4] = {shown.u0, shown.v0, shown.u1, shown.v1};
        const bool drawn = t != nullptr && picture != nullptr;
        flicker_capture(drawn ? picture : nullptr, uv, drawn ? t->gsW : 640, drawn ? t->gsH : 448,
                        drawn ? t->base : ~0u);
    }
    challenge_osd::present(bb);                 /* PC (2026-11-14): the challenge modes' panel, under the menus */
    settings_menu::present(bb);                 /* F2: the settings menu (its own state) */
    system_menu::present(bb);                   /* Esc: Back, Restart, Exit (its own state) */
    frame_monitor::present(bb);                 /* F3: the frame times (its own state) */
    frame_step::present(bb);                    /* F5 / F6: the tick step's pause */
    osd::present(bb);                           /* a short message (the replay speed) */
    song_osd::present(bb);                      /* PC: the race's song changer (its own state) */
    cockpit_osd::present(bb);                   /* PC (2026-11-08): the cockpit camera's calibration */
    texture_menu::present(bb);                  /* PC (2026-11-13): the texture interface (F10) */
    /* (the D3D thread has run the picture's draw by now - the overlays'
     * context or this dc() waited for it: Present needs it anyway; the
     * overlays left their own state, forgotten without a queued call) */
    dc()->OMSetRenderTargets(0, nullptr, nullptr);
    d3d_forget();
    gpu::present();
    follow_output();
}

void present_target(uint32_t base, uint32_t psm, uint32_t fbw, int dx, int dy, int dw, int dh)
{
    g_disp.valid = true;
    g_disp.enabled = true;
    g_disp.base = base;
    g_disp.psm = psm;
    g_disp.fbw = fbw;
    g_disp.dx = dx;
    g_disp.dy = dy;
    g_disp.dw = dw;
    g_disp.dh = dh;
    present_now();
}

void present_black()
{
    g_disp.enabled = false;
    present_now();
}

void present_last()
{
    g_disp.enabled = true;
    present_now();
}

void present_again()
{
    present_now(false);
}

/* ======================================================================== */
/* Readbacks                                                                */
/* ======================================================================== */

/* The resources of the readbacks of one size and source rectangle: the w x h
 * resolve target and its staging copy. */
struct Readback {
    int w = 0, h = 0;
    Com<ID3D11Texture2D> tmp;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11Texture2D> staging;
};
static std::unordered_map<uint64_t, std::unique_ptr<Readback>> g_readbacks;

static bool readback_resources(Readback &rb, int w, int h)
{
    if (rb.tmp && rb.w == w && rb.h == h)
        return true;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    rb.tmp.reset();
    rb.rtv.reset();
    if (FAILED(dev()->CreateTexture2D(&d, nullptr, rb.tmp.put())) ||
        FAILED(dev()->CreateRenderTargetView(rb.tmp.get(), nullptr, rb.rtv.put())))
        return false;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    rb.staging.reset();
    if (FAILED(dev()->CreateTexture2D(&d, nullptr, rb.staging.put())))
        return false;
    rb.w = w;
    rb.h = h;
    return true;
}

static void readback_copy_out(ID3D11Texture2D *staging, const CmdReadback &r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    const HRESULT hr = dc()->Map(staging, 0, D3D11_MAP_READ, 0, &m);     /* (waits for the GPU) */
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    g_readbackWait += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (FAILED(hr))
        return;
    for (int y = 0; y < r.h; y++)
        memcpy((uint8_t *)r.dst + (size_t)y * r.w * 4, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
               (size_t)r.w * 4);
    dc()->Unmap(staging, 0);
}

/* Debug aid: GT3PC_READBACK_LOG=1 logs (first 400 lines, with the frame
 * number) every readback that did not also run in the frame before - the
 * flare strip, read every frame, once per run of frames; the car icons at a
 * race start, each once - with the target it read and what came back (how
 * many pixels have alpha, how many colour): one-off readbacks happen in a
 * single frame, which a GT3PC_PGL_TRACE press cannot aim at. */
static bool readback_log(const CmdReadback &r)
{
    static int state = -1;
    static int lines = 0;
    static std::unordered_map<uint64_t, uint64_t> last;     /* readback -> the frame it last ran in */
    if (state < 0) {
        const char *e = getenv("GT3PC_READBACK_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging the readbacks (GT3PC_READBACK_LOG, 400 lines)");
    }
    if (state != 1)
        return false;
    const uint64_t key = ((uint64_t)r.sbp << 44) ^ ((uint64_t)(uint32_t)r.w << 32) ^
                         ((uint64_t)(uint32_t)r.h << 20) ^ (uint64_t)(uintptr_t)r.dst;
    uint64_t &f = last[key];
    const bool fresh = f + 1 < g_frameNo;
    f = g_frameNo;
    return fresh && lines++ < 400;
}

/* gs_download_image: the GS pixels of the colour target that holds block
 * sbp (the registry's owner of the block; the most recent target based there
 * when nothing owns it), point samples at their centres, resolved into a w x
 * h target and copied out through a staging texture, within the frame: the
 * CPU waits for the GPU, as the PS2 stalled its lists for every readback.
 * PC: the flare results (flare_readback) were once delivered one frame late
 * to spare the wait; but texel q of the strip is the frame's q-th occlusion
 * query, and the lamps and course lights queried change from one frame to
 * the next, so last frame's texels gave the glows (drawn without a depth
 * test, weighted by them) other lights' visibility: glows flickering and
 * showing through the cars.
 * PC: the owner, not the target used there last: CarIconMaker (T184) draws
 * every car's icon in one frame, each into a 64 x 32 buffer that the VRAM
 * allocator puts at the base of the previous car's 128 x 128 reflection
 * buffer (cleared black, alpha 0x80; the icon's block is given back only at
 * the next download, after the reflection's).  Both targets were used in
 * that frame and the older one won, so every icon but car 0's (the player's)
 * came back an opaque black box (user report: result board, split times). */
void readback(const CmdReadback &r)
{
    /* PC: the flare result strip's only for GT3PC_FLARE_LOG: the glows read
     * their texels on the GPU (exec_model), the CPU does not wait here */
    if ((r.flags & READBACK_FLARE) && !flare_log(false)) {
        if (r.cb != nullptr) {
            fpu::ScopedFpuMode game(g_gameFpu);
            r.cb(r.madr, r.arg);
        }
        return;
    }
    int ox = 0, oy = 0;
    Target *t = registry_colour_target_at(r.sbp, ox, oy);
    const bool owned = t != nullptr;
    Target *latest = find_color_target(r.sbp);
    if (t == nullptr) {
        t = latest;
        ox = oy = 0;
    } else if (latest != nullptr && latest != t) {
        char key[64];
        snprintf(key, sizeof(key), "pgl-readback-owner-%X-%dx%d-%dx%d", r.sbp, t->gsW, t->gsH, latest->gsW,
                 latest->gsH);
        log_once(key, "pgl: readback of block 0x%X from the %dx%d target that holds it (the last one used there"
                 " is %dx%d)", r.sbp, t->gsW, t->gsH, latest->gsW, latest->gsH);
    }
    const bool ok = t != nullptr && r.w > 0 && r.h > 0 && r.dst != nullptr && (r.spsm & 0x3F) == 0;
    if (!ok) {
        log_once("pgl-readback", "pgl: readback of block 0x%X (psm 0x%X, %dx%d) has no native source:"
                 " zeros", r.sbp, r.spsm, r.w, r.h);
        if (r.dst != nullptr && r.w > 0 && r.h > 0)
            memset(r.dst, 0, (size_t)r.w * (size_t)r.h * 4);
    } else {
        const uint64_t key = ((uint64_t)r.sbp << 44) ^ ((uint64_t)(uint32_t)r.sx << 32) ^
                             ((uint64_t)(uint32_t)r.sy << 20) ^ ((uint64_t)(uint32_t)r.w << 10) ^
                             (uint64_t)(uint32_t)r.h ^ ((uint64_t)(uintptr_t)r.dst << 13);
        std::unique_ptr<Readback> &rbp = g_readbacks[key];
        if (!rbp)
            rbp.reset(new Readback());
        Readback &rb = *rbp;
        ID3D11ShaderResourceView *src = target_srv(*t);    /* (a resolve first) */
        if (src != nullptr && readback_resources(rb, r.w, r.h)) {
            d3d_invalidate();
            const float x0 = (float)(ox + r.sx), y0 = (float)(oy + r.sy);
            const float x1 = x0 + (float)r.w, y1 = y0 + (float)r.h;
            const float W = (float)r.w, H = (float)r.h;
            GsVtx v[6] = {
                {0, 0, 0, 1, x0, y0, 0x80808080u, 255}, {W, 0, 0, 1, x1, y0, 0x80808080u, 255},
                {0, H, 0, 1, x0, y1, 0x80808080u, 255}, {W, 0, 0, 1, x1, y0, 0x80808080u, 255},
                {W, H, 0, 1, x1, y1, 0x80808080u, 255}, {0, H, 0, 1, x0, y1, 0x80808080u, 255},
            };
            ID3D11Buffer *vb = nullptr;
            const uint32_t first = stream_vertices(v, sizeof(GsVtx), 6, &vb);
            set_targets(rb.rtv.get(), nullptr, r.w, r.h);
            set_raster(raster_state(0, false));
            set_blend(blend_plain(15));
            set_depth(depth_state(false, pgl::gs::Compare::Always, false));
            set_vb(vb, sizeof(GsVtx));
            set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            bind_vs(Vs::Gs);
            CbGs cg = {W, H, 0, 0};
            bind_vs_cb(&cg, sizeof(cg));
            bind_ps(Ps::Main);
            CbPs cp = {};
            cp.texInvW = 1.0f / (float)t->gsW;
            cp.texInvH = 1.0f / (float)t->gsH;
            cp.flags = PSF_TME | (1u << PSF_TFX_SHIFT) | PSF_TCC;     /* DECAL, RGBA: the bytes */
            cp.rowScale = 1;
            bind_ps_cb(&cp, sizeof(cp));
            set_srv0(src);
            set_sampler0(sampler_state(false, false, 1, 1));
            dc()->Draw(6, first);
            dc()->CopyResource(rb.staging.get(), rb.tmp.get());
            readback_copy_out(rb.staging.get(), r);     /* waits for the GPU */
            d3d_invalidate();
            /* PC: its native pixels kept for an upload of the same pixels
             * (the car icons: docs/renderer.md "High-resolution images") */
            registry_hires_readback(*t, ox + r.sx, oy + r.sy, r.w, r.h, r.dst);
        }
    }
    if (readback_log(r)) {
        int alpha = 0, colour = 0;
        if (r.dst != nullptr && r.w > 0 && r.h > 0) {
            const uint8_t *p = static_cast<const uint8_t *>(r.dst);
            for (int i = 0; i < r.w * r.h; i++, p += 4) {
                alpha += p[3] != 0;
                colour += (p[0] | p[1] | p[2]) != 0;
            }
        }
        if (ok)
            pclog::write("pgl readback: frame %llu: block 0x%X %dx%d from the %dx%d target at 0x%X (%s): %d of %d"
                         " pixels with alpha, %d with colour", (unsigned long long)g_frameNo, r.sbp, r.w, r.h,
                         t->gsW, t->gsH, t->base, owned ? "the block's owner" : "nothing owns the block: the"
                         " last one used there", alpha, r.w * r.h, colour);
        else
            pclog::write("pgl readback: frame %llu: block 0x%X %dx%d (psm 0x%X): no native source, zeros",
                         (unsigned long long)g_frameNo, r.sbp, r.w, r.h, r.spsm);
    }
    if (ok && flare_log(true)) {
        /* every frame: the first 48 texels' R (the flare queries' results, in
         * query order) */
        char texels[48 * 3 + 1];
        int n = 0;
        const uint8_t *p = static_cast<const uint8_t *>(r.dst);
        for (int i = 0; i < r.w * r.h && i < 48; i++)
            n += snprintf(texels + n, sizeof(texels) - (size_t)n, " %02X", p[4 * i]);
        texels[n] = 0;
        pclog::write("pgl flare: frame %llu: readback of block 0x%X %dx%d (target %dx%d, native %dx%d), R:%s",
                     (unsigned long long)g_frameNo, r.sbp, r.w, r.h, t->gsW, t->gsH, t->nw, t->nh, texels);
    }
    if (r.cb != nullptr) {
        fpu::ScopedFpuMode game(g_gameFpu);
        r.cb(r.madr, r.arg);
    }
}

/* ======================================================================== */
/* Frames                                                                   */
/* ======================================================================== */

uint64_t frame_number()
{
    return g_frameNo;
}

void next_frame()
{
    g_frameNo++;
    /* PC (2026-11-13): a line when targets are made at a rate no steady
     * scene has - a buffer the game allocates anew each frame that lands at
     * another VRAM block each time is a new target at each (a tyre's tread
     * blur buffer at four times its size did: 17 MB a target with 8x MSAA,
     * 92 of them alive, stalls of 100 .. 400 ms; user report 2026-11-13).
     * Every kChurnFrames frames, the first kChurnLines times: what was made,
     * what is alive, the last one made.  A load or an output-scale change
     * makes a dozen at once and stays under the count. */
    constexpr int kChurnFrames = 600, kChurnCount = 24, kChurnLines = 40;
    static int churnLines = 0;
    if (g_frameNo % kChurnFrames == 0) {
        if (g_made.count >= kChurnCount && churnLines < kChurnLines) {
            churnLines++;
            uint64_t alive = 0;
            for (auto &tp : g_targets)
                alive += target_bytes(*tp);
            static unsigned lastCreated = 0, lastReused = 0;
            pclog::write("pgl targets: %d made in the last %d frames (%llu MB; %u surfaces created, %u taken from"
                         " retired targets), %d alive (%llu MB), %d spare surfaces (%llu MB); the last a %dx%d %s buffer"
                         " at block 0x%X (native %dx%d, %d sample(s)): a buffer that moves in VRAM is a new target at"
                         " each place",
                         g_made.count, kChurnFrames, (unsigned long long)(g_made.bytes >> 20),
                         g_surfCreated - lastCreated, g_surfReused - lastReused, (int)g_targets.size(),
                         (unsigned long long)(alive >> 20), (int)g_spares.size(),
                         (unsigned long long)(g_spareBytes >> 20), g_made.gsW, g_made.gsH,
                         g_made.depth ? "Z" : "colour", g_made.base, g_made.nw, g_made.nh, g_made.samples);
            lastCreated = g_surfCreated;
            lastReused = g_surfReused;
        }
        g_made = MadeCount();
    }
    /* targets unused for a while go (per-frame effect buffers at sizes no
     * longer used).  PC (2026-11-13): and those whose buffer the game freed
     * (target_freed: its VRAM given back, nothing may read it again) once
     * kFreedKeep frames have passed without a draw into them - a buffer the
     * game allocates and frees every frame (the smoke's colour and Z buffers
     * on the dirt courses, a quarter of the screen each: 37 and 75 MB at
     * 150 % with 8x MSAA; the tyres' blur buffers) is at the same block
     * frame after frame while the allocator's free list stays in order, and
     * at another one each frame when it does not - each a new target kept
     * 600 frames: 77 alive, 5.5 GB, the GPU's memory overrun, stalls of 100
     * .. 470 ms (user report "a major stutter", 2026-11-13).  Their surfaces
     * are kept for the next target of their size (spare_keep). */
    constexpr uint64_t kFreedKeep = 3;
    for (size_t i = 0; i < g_targets.size();) {
        Target &t = *g_targets[i];
        /* (not the buffer on display: the game frees its frame buffers at a
         * mode change and the last picture stays up through the load) */
        const bool shown = !t.depth && g_disp.valid && t.base == g_disp.base;
        if (t.lastUse + 600 < g_frameNo || (t.freedAt != 0 && !shown && t.freedAt + kFreedKeep <= g_frameNo)) {
            mirror_forget(t);               /* PC: (the alpha mirrors' pointers to it) */
            registry_target_released(t);
            spare_keep(t);
            g_targets.erase(g_targets.begin() + (ptrdiff_t)i);
            g_targetsGen++;
            g_retired++;
        } else {
            i++;
        }
    }
    for (size_t i = 0; i < g_spares.size();) {
        if (g_spares[i].at + kSpareFrames < g_frameNo) {
            g_spareBytes -= g_spares[i].bytes;
            g_spares.erase(g_spares.begin() + (ptrdiff_t)i);
        } else {
            i++;
        }
    }
    /* A base's sizes beyond kMaxSizes go too, the least recently used first,
     * once unused for 2 frames: a buffer sized by something that changes
     * every frame (the split screen's letterboxed views followed the window's
     * aspect while it was dragged) made a new size each frame, each kept for
     * 600 frames - at a high output scale with MSAA a hundred megabytes a
     * frame, seconds of paging (user report: the split screen froze while
     * the window was resized). */
    constexpr int kMaxSizes = 8;
    std::unordered_map<uint64_t, int> sizes;
    for (auto &tp : g_targets)
        sizes[((uint64_t)tp->base << 1) | (tp->depth ? 1u : 0u)]++;
    for (auto &group : sizes) {
        while (group.second > kMaxSizes) {
            size_t oldest = g_targets.size();
            for (size_t i = 0; i < g_targets.size(); i++) {
                const Target &t = *g_targets[i];
                if ((((uint64_t)t.base << 1) | (t.depth ? 1u : 0u)) == group.first && t.lastUse + 2 <= g_frameNo &&
                    (oldest == g_targets.size() || t.lastUse < g_targets[oldest]->lastUse))
                    oldest = i;
            }
            if (oldest == g_targets.size())
                break;
            char key[64];
            snprintf(key, sizeof(key), "pgl-target-sizes-%llX", (unsigned long long)group.first);
            log_once(key, "pgl: more than %d sizes of the %s buffer at block 0x%X: the least recently used go",
                     kMaxSizes, (group.first & 1) ? "Z" : "colour", (unsigned)(group.first >> 1));
            mirror_forget(*g_targets[oldest]);  /* PC: (the alpha mirrors' pointers to it) */
            registry_target_released(*g_targets[oldest]);
            spare_keep(*g_targets[oldest]);     /* PC (2026-11-13) */
            g_targets.erase(g_targets.begin() + (ptrdiff_t)oldest);
            g_targetsGen++;
            g_retired++;
            group.second--;
        }
    }
}

void target_freed(uint32_t base)
{
    base &= 0x3FFF;
    for (auto &tp : g_targets)
        if (tp->base == base && tp->freedAt == 0)
            tp->freedAt = g_frameNo;
}

bool target_alive(const Target *t)
{
    for (const auto &tp : g_targets)
        if (tp.get() == t)
            return true;
    return false;
}

void target_stats(int *alive, unsigned *created, unsigned *reused, unsigned *retired)
{
    if (alive != nullptr)
        *alive = (int)g_targets.size();
    if (created != nullptr)
        *created = g_surfCreated;
    if (reused != nullptr)
        *reused = g_surfReused;
    if (retired != nullptr)
        *retired = g_retired;
}

void set_game_fpu_mode(unsigned int mode)
{
    g_gameFpu = mode;
}

unsigned int game_fpu_mode()
{
    return g_gameFpu;
}

}  /* namespace pglr */
