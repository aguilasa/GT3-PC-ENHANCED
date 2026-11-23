/* pgl_render.h - the native renderer of pgl (Direct3D 11), internal.
 *
 * pgl (the port's src/engine/pgl/*.cpp) records what its PS2 packets meant
 * through pgl_bridge.h; this renderer keeps the records in a command list,
 * executes the list when the frame is kicked and presents the displayed
 * frame buffer.  Modules:
 *   pgl_render.cpp    the bridge: recording, frame lists, sub-lists, the
 *                     immediate transfers, presentation entry points
 *   pgl_exec.cpp      execution: the GS register state, the GIF decoder,
 *                     primitive assembly, draw state -> D3D11, batching,
 *                     the VU1 draws, the effect recipes
 *   pgl_targets.cpp   native render targets by VRAM address, presentation,
 *                     readbacks
 *   pgl_textures.cpp  the VRAM address registry and the texture cache
 *                     (gstex decoding)
 *   pgl_d3d.cpp       D3D11 state objects, shaders, dynamic buffers
 *   pgl_models.cpp    decoded VifModel meshes (gt3vif) and the vertex data
 *                     of the VU1 draws
 * docs/renderer.md describes the design.  Native code: STL, D3D11 (no
 * prelude); runs on game threads (one at a time), every D3D11 block under
 * fpu::DefaultFpuMode. */
#ifndef PGL_NATIVE_PGL_RENDER_H
#define PGL_NATIVE_PGL_RENDER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include "engine/pgl/native/pgl_intrin.h"
/* winuser.h's DrawState (DrawStateA / W) would rename pgl::gs::DrawState */
#ifdef DrawState
#undef DrawState
#endif

#include "engine/pgl/native/pgl_bridge.h"
#include "engine/pgl/native/pgl_queue.h"
#include "engine/pgl/native/pgl_state_gs.h"
#include "engine/pgl/native/pgl_state_prim.h"
#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/vif_model.h"

namespace pglr {

/* PC (CPU time, 2026-11-03): a * b (mod 2^64) inline.  The game is 32-bit:
 * a 64-bit product was a call of the runtime's _allmul, ~6% of the
 * executor's time in the keys' hashes (comb, hash_bytes, translate_memo). */
inline uint64_t mul64(uint64_t a, uint64_t b)
{
#if defined(_M_IX86)
    const uint32_t al = (uint32_t)a, ah = (uint32_t)(a >> 32), bl = (uint32_t)b, bh = (uint32_t)(b >> 32);
    return __emulu(al, bl) + ((uint64_t)(al * bh + ah * bl) << 32);
#else
    return a * b;
#endif
}

/* PC (CPU time, 2026-11-03): bytes the same, 16 at a time without a branch
 * (SSE2; the rest by memcmp) - the constants of most draws equal what is
 * bound since the VU1 draws' own values are in their records (Vu1Inst), and
 * memcmp went through all 448 bytes of CbVu1 for each (5 % of a 32-car
 * frame's executor time).  (PC, 2026-11-05: here for pgl_exec.cpp too.) */
inline bool same_bytes(const void *a, const void *b, size_t bytes)
{
    const __m128i *pa = static_cast<const __m128i *>(a), *pb = static_cast<const __m128i *>(b);
    const size_t n = bytes / 16;
    __m128i eq = _mm_set1_epi32(-1);
    for (size_t i = 0; i < n; i++)
        eq = _mm_and_si128(eq, _mm_cmpeq_epi32(_mm_loadu_si128(pa + i), _mm_loadu_si128(pb + i)));
    return _mm_movemask_epi8(eq) == 0xFFFF &&
           (bytes % 16 == 0 || memcmp(pa + n, pb + n, bytes % 16) == 0);
}

/* PC (CPU time): a hasher for unordered_maps keyed by a 64-bit hash already
 * (the standard one runs FNV-1a over its eight bytes) */
struct KeyHash {
    size_t operator()(uint64_t k) const { return (size_t)(k ^ (k >> 32)); }
};

/* ======================================================================== */
/* COM helper                                                               */
/* ======================================================================== */

template <class T> class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com &o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com &operator=(const Com &o) { if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    Com(Com &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com &operator=(Com &&o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    /* (PC: released on the D3D thread after the calls queued before, which
     * may use it: q::release_queued) */
    void reset() { if (p_) { if (!q::release_queued(p_)) p_->Release(); p_ = nullptr; } }
    T *get() const { return p_; }
    T **put() { reset(); return &p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T *p_ = nullptr;
};

/* ======================================================================== */
/* Commands                                                                 */
/* ======================================================================== */

enum CmdType : uint8_t {
    CMD_REGS,           /* pgl's register flush */
    CMD_AD,             /* one A+D write */
    CMD_TARGET,         /* a render target's size */
    CMD_PRIM,           /* GS vertices */
    CMD_GIF,            /* a GIF block in the packet buffer */
    CMD_STRIP,          /* VU1 primitive program */
    CMD_TRI16,          /* tri_draw16 */
    CMD_MODEL,          /* a VU1 model part */
    CMD_UPLOAD,         /* an image upload */
    CMD_TEX1_UPLOAD,    /* a Tex1 set upload */
    CMD_READBACK,       /* a download */
    CMD_SUBCALL,        /* a recorded sub-list */
    CMD_PFX_COLOR,      /* the colour correction recipe */
    CMD_PFX_SPLIT,      /* the plane split recipe */
    CMD_FLARE_QUERY,    /* a lens-flare occlusion query (PglrFlareQuery) */
    CMD_HALF_BLEND,     /* the begin / end of draws at half strength (pglr_half_blend) */
    CMD_DESIGN,         /* the 2D design squeeze of the draws that follow (pglr_set_design_scale) */
    CMD_LINE_WIDTH,     /* the width of the lines that follow (pglr_set_line_width) */
    CMD_POST_AA,        /* a race view's 3D picture done: FXAA over it when chosen (pglr_post_aa) */
    CMD_GPU_SECTION,    /* GPU timing: the draws after it are this section's (pglr_gpu_section) */
    CMD_TEX_SOURCE      /* PC: the texture selected is a VRAM-less Tex1 set's (pglr_texture_source) */
};

/* PC (2026-11-05): key - the command's structure hashed as it is recorded
 * (cmd_regs_key / cmd_model_key, on the game thread, which has the time):
 * the resolved runs (pgl_exec.cpp run_scan_key) mix the keys of a run's
 * commands instead of reading their fields again. */
struct CmdRegs   { uint64_t mask; uint64_t reg[13]; uint64_t gen[10]; uint64_t key; };
struct CmdAd     { uint32_t addr; uint32_t pad; uint64_t value; };
/* (PC 2026-11-14: dx, dy above 0 - a buffer's detail, pglr_target_detail;
 * 0 for a size, w -1 for a buffer freed) */
struct CmdTarget { uint32_t base, fbw, psm; int32_t w, h; float dx, dy; };
struct CmdPrim   { uint32_t prim, flags; int32_t n; /* GsVertex[n] follow */ };
struct CmdGif    { const void *data; uint32_t qwc, flags; };
struct CmdStrip  {
    PglrVu1 vu; uint32_t prim; const float *xyz; const float *st; const uint32_t *rgba;
    uint32_t constRgba; int32_t n; int32_t nStrips; uint32_t flags; uint8_t lengths[64];
};
struct CmdTri16  { PglrVu1 vu; uint32_t prim; const int16_t *xyzw; const int16_t *st; const uint32_t *rgba; int32_t nTris; uint32_t flags; };
struct CmdModel  { PglrVu1 vu; PglrModelPart part; uint32_t flags; uint32_t pad; uint64_t key; };
struct CmdUpload { uint32_t dbp, dbw, dpsm; int32_t dx, dy, w, h; const void *data; };
/* vramBase: the block the set was linked for when the upload was recorded
 * (tex1_upload_packet baked its BITBLTBUF headers into the packet then; only
 * the pixels are DMA references, read when the list runs).  PC: followed by
 * copyBytes bytes, the set's header and tables then (registry_tex1_copy):
 * the game relinks them in place while the executor runs the list. */
struct CmdTex1   { const void *img; uint32_t vramBase; uint32_t copyBytes; };
/* flags: READBACK_FLARE, the flare result strip's (pglr_readback_flare) */
struct CmdReadback {
    uint32_t sbp, sbw, spsm; int32_t sx, sy, w, h; void *dst;
    PglrReadbackFunc cb; uint32_t madr, arg;
    uint32_t flags;
};
enum : uint32_t { READBACK_FLARE = 1u };
struct CmdSubcall { const void *key; };
struct CmdPfxColor { uint32_t src, dst, fbw; int32_t w, h; uint32_t colR, colG, colB; uint8_t curve[768]; };
struct CmdPfxSplit { uint32_t src, r, g, b; int32_t w, h; };
struct CmdHalfBlend { int32_t begin; uint32_t base, fbw, psm; int32_t w, h; };
struct CmdDesign { float kx, ky; };
struct CmdLineWidth { float px; };
struct CmdPostAa { uint32_t base; int32_t w, h; };
struct CmdSection { int32_t id; };
struct CmdTexSource { const void *img; uint32_t vramBase; uint64_t tex0; uint32_t copyBytes; };  /* (+ as CmdTex1) */

/* PC (2026-11-05): the structure keys of the commands (CmdRegs::key,
 * CmdModel::key) - a hash of the fields a resolved run depends on, the
 * variable ones left out: in a register flush the registers written and
 * their values but a TEX0 / TEX2's TBP0 and CBP (a set's VRAM place: the
 * binds read the registers themselves); in a model part the model, the
 * part, the lighting program, the PRIM override and the VU1 program (its
 * matrices, lighting block and morph weight are read at each replay). */
constexpr uint64_t kCmdTexAddrMask = ~(0x3FFFull | (0x3FFFull << 37));
inline uint64_t cmd_mix(uint64_t h, uint64_t v)
{
    h = mul64(h ^ v, 0xFF51AFD7ED558CCDull);
    return h ^ (h >> 29);
}
/* PC (2026-11-06): an A+D write's key for the resolved runs, worked out as it
 * is recorded (pglr_ad; pgl_exec.cpp): 0 for a write a run may not hold (a
 * vertex, a transfer: the run ends before it), else never 0 - the address and
 * the value, a TEX0 / TEX2's TBP0 and CBP left out. */
uint64_t cmd_ad_key(uint32_t addr, uint64_t value);
inline uint64_t cmd_regs_key(uint64_t mask, const uint64_t *reg, const uint64_t *gen)
{
    /* (the shadows' order: pgl_state_gs.h REG_* - TEX2 8, TEX0 10) */
    uint64_t h = cmd_mix(0x3C6EF372FE94F82Bull, mask);
    uint32_t lo = (uint32_t)mask, hi = (uint32_t)(mask >> 32);
    unsigned long b;
    while (lo != 0) {
        _BitScanForward(&b, lo);
        lo &= lo - 1;
        const unsigned long r = b & 15;
        if (r < 13) {
            uint64_t v = reg[r];
            if (r == 8 || r == 10)
                v &= kCmdTexAddrMask;
            h = cmd_mix(h, v);
        }
    }
    while (hi != 0) {
        _BitScanForward(&b, hi);
        hi &= hi - 1;
        if (b < 10)
            h = cmd_mix(h, gen[b]);
    }
    return h;
}
inline uint64_t cmd_model_key(const PglrVu1 &vu, const PglrModelPart &part, uint32_t flags)
{
    uint64_t h = cmd_mix(0xA54FF53A5F1D36F1ull, (uint64_t)(uintptr_t)part.model);
    h = cmd_mix(h, (uint64_t)(uint32_t)part.part | (uint64_t)part.lightProg << 32);
    h = cmd_mix(h, (uint64_t)part.primMask | (uint64_t)part.primAttr << 32);
    if (part.hideOn != 0.0f) {
        /* PC (2026-11-07): the hide box (a run with it is another run) */
        uint32_t w[6];
        memcpy(w, part.hideC, 12);
        memcpy(w + 3, part.hideH, 12);
        h = cmd_mix(h, (uint64_t)w[0] | (uint64_t)w[1] << 32);
        h = cmd_mix(h, (uint64_t)w[2] | (uint64_t)w[3] << 32);
        h = cmd_mix(h, (uint64_t)w[4] | (uint64_t)w[5] << 32);
        h = cmd_mix(h, (uint64_t)(part.hideOn == 2.0f ? 2 : 1));   /* (the part whole, or the box) */
    }
    return cmd_mix(h, (uint64_t)flags | (uint64_t)vu.prog << 32);
}

/* A command list: commands in an arena, in program order. */
class CmdList {
public:
    void clear() { data_.clear(); offs_.clear(); keys_.clear(); }
    bool empty() const { return offs_.empty(); }
    size_t size() const { return offs_.size(); }
    /* Appends a command of type t with `bytes' of payload; returns it. */
    void *add(CmdType t, size_t bytes);
    CmdType type(size_t i) const { return (CmdType)data_[offs_[i]]; }
    const void *payload(size_t i) const { return &data_[offs_[i] + 16]; }
    /* PC (2026-11-05): a command's structure key (CmdRegs::key, CmdModel::key)
     * beside the types, so the resolved runs' scan (pgl_exec.cpp
     * run_scan_key) reads a packed array instead of each command's payload
     * (a cache line each: 6 % of the executor) */
    uint64_t key(size_t i) const { return keys_[i]; }
    void set_last_key(uint64_t k) { keys_.back() = k; }

private:
    std::vector<uint8_t> data_;
    std::vector<uint32_t> offs_;
    std::vector<uint64_t> keys_;
};

/* ======================================================================== */
/* Render targets                                                           */
/* ======================================================================== */

struct Target {
    uint32_t base = 0;          /* 64-word blocks */
    bool     depth = false;     /* a Z buffer (else colour) */
    uint32_t fbw = 0;           /* buffer width, 64-pixel units */
    uint32_t psm = 0;           /* the format it was created for (CT32 / CT24 / CT16 / Z*) */
    int      gsW = 0, gsH = 0;  /* GS size */
    int      nw = 0, nh = 0;    /* native size */
    float    sx = 1, sy = 1;    /* native pixels per GS pixel */
    bool     gsRes = false;     /* kept at GS resolution (target_gs_resolution) */
    /* PC (2026-11-14): native pixels per output-scale pixel, by axis (1: the
     * output scale): the buffer's detail the game asked for when it was made
     * (pglr_target_detail: the road reflection, [Graphics]
     * ReflectionResolution); a target of another detail is another target */
    float    detailX = 1.0f, detailY = 1.0f;
    int      samples = 1;       /* MSAA samples per pixel (docs/renderer.md "Anti-aliasing") */
    Com<ID3D11Texture2D> tex;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv;
    /* PC (2026-11-05): a depth target's view with the depth read-only (the
     * stencil written): bound with its texture (srv) by the draws whose alpha
     * mirror pass is folded into them (pgl_exec.cpp draw_passes; single-
     * sampled targets) */
    Com<ID3D11DepthStencilView> dsvRead;
    /* its texture view (single-sampled targets; a multisampled one is read
     * through its resolve: target_srv) */
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11ShaderResourceView> srvMs;        /* a multisampled target's (its resolve passes) */
    uint32_t objId = 0;         /* registry object (0: not registered yet) */
    uint64_t gen = 1;           /* bumped by draws into it */
    /* the copy that draws reading the target sample (graphics.md 7.2); of a
     * multisampled target, its resolve */
    Com<ID3D11Texture2D> copy;
    Com<ID3D11ShaderResourceView> copySrv;
    Com<ID3D11DepthStencilView> copyDsv;        /* a multisampled depth target's resolve, written so */
    Com<ID3D11RenderTargetView> copyRtv;        /* a multisampled colour target's (a resolve of a part) */
    uint64_t copyGen = 0;
    /* PC: where it was written since the copy was brought up to date (native
     * pixels, [l, r) x [t, b); empty: r <= l), valid while rectGen == gen
     * (target_written_rect; any other gen++ makes the next refresh whole).
     * A colour copy is refreshed there only: the in-car heat-haze / rain
     * drops read the target they draw into, a TEXFLUSH and a fresh copy
     * before each, which copied (or with MSAA resolved) the whole screen a
     * few hundred times a frame (user report: replays slow). */
    int dirtyL = 0, dirtyT = 0, dirtyR = 0, dirtyB = 0;
    uint64_t rectGen = 0;
    uint64_t lastUse = 0;
    /* PC (2026-11-13): the frame in which the game gave the buffer's VRAM
     * back (target_freed); 0: it has not, or a draw looked the target up
     * since.  A freed target goes after kFreedKeep frames (next_frame), not
     * 600. */
    uint64_t freedAt = 0;
    /* PC (2026-11-04): bit 7 of the colour target's alpha mirrored in a
     * stencil buffer, which the DATE draws test instead of reading a copy
     * (docs/renderer.md 3, "DATE"): mirrorZ - the depth target whose stencil
     * holds it (bound with it; its mirrorOf is this target), or none - then
     * the target's own stencil buffer (mirrorTex / mirrorDsv, made when a
     * draw without a depth buffer needs it); mirrorValid - the stencil holds
     * every alpha write since the last sync (pgl_exec.cpp mirror_sync);
     * dated - a DATE draw read it: the alpha writes keep the mirror only
     * from then on (target_mirror_dsv). */
    Target *mirrorZ = nullptr;
    Target *mirrorOf = nullptr;             /* (a depth target: the colour target whose mirror it holds) */
    Com<ID3D11Texture2D> mirrorTex;
    Com<ID3D11DepthStencilView> mirrorDsv;
    bool mirrorValid = false;
    bool dated = false;
    /* PC (2026-11-06): [Graphics] EnvMapDetail (targets_env_maps): an env pass
     * (ENVMTX / ENVMAP) sampled the target this frame - it is a car
     * environment map (the executor's note) - and whether the setting holds
     * it at its GS size now (gsRes set by it) */
    bool envRead = false;
    bool envNative = false;
    /* PC (2026-11-07): a DATE draw into the target since its alpha was last
     * written flat over all of it (pgl_exec.cpp draw_bookkeep; cleared with
     * execAlpha below, flush_batch): the lazy mirror lets the mirror go only
     * while none has - a DATE draw is one of a sequence (a car's env pairs,
     * with its body parts between: a drop there cost a full sync at the next
     * pair, the user's replay capture, three a frame), and the sequence ends
     * where the game clears the alpha (before the cars, after the sky).  A
     * draw count since the last DATE draw was tried first (48): it kept the
     * passes of the course's largest draws, right after the sky (14 Mpx),
     * and told the executor nothing for a replayed run, whose parts' boxes
     * the stage's walk finds (frames 11929, 7188). */
    bool dateSinceFlat = false;
    /* PC (2026-11-05): a bound on the alpha a colour target holds (GS units;
     * -1: unknown) - the alpha mirror's plans narrow the alpha a draw
     * textured from the target can write (pgl_exec.cpp alpha_write_range).
     * The submit stage's: set from 0 when the target is made (cleared), by
     * every alpha write into it (target_alpha_written: a sprite batch that
     * covers the target starts it over, any other widens it), unknown after
     * a pass that writes alphas it does not bound (the flare strip). */
    int alphaLo = -1, alphaHi = -1;
    /* PC (2026-11-05, the submit workers: pgl_exec.cpp run_work): the submit
     * ring's position after the last record that may write the bound (the
     * executor's note; a worker building a run that reads the bound waits
     * for the submit thread to pass it) */
    uint64_t alphaPos = 0;
    /* PC (2026-11-06): the executor's own note of the target's alpha: the one
     * value every pixel holds (0..255) after a sprite batch covering the
     * target wrote a flat alpha with nothing gating a pixel (pgl_exec.cpp
     * flush_batch), or -1 once anything else writes its alpha (draw_bookkeep,
     * the passes).  A DATE draw that finds the mirror invalid then clears the
     * stencil to that value's bit instead of syncing from the alpha
     * (mirror_sync). */
    int execAlpha = -1;
};

/* ======================================================================== */
/* Interfaces between the modules                                           */
/* ======================================================================== */

/* pgl_targets.cpp */
void targets_init();
float output_scale();
/* Before a frame executes: its picture's aspect (0: the 4:3 picture, else a
 * race frame drawn at the window's aspect; pglr_set_picture_aspect), and
 * whether it is drawn over the screens' canvas (pglr_set_picture_multi). */
void targets_frame_aspect(float aspect, bool multi);
/* A draw buffer's GS size (CMD_TARGET, gfx_set_draw_buffer / _target_b);
 * fbwPixels: the buffer width in pixels. */
void target_size(uint32_t base, uint32_t fbwPixels, uint32_t psm, int w, int h);
/* PC (2026-11-14): the w x h buffer at base drawn at dx x dy times the
 * output scale from now on (pglr_target_detail; 1, 1 back to it): the draw
 * targets made for it are that much finer (color_target).  A size of
 * another w x h for base clears it (target_size). */
void target_detail(uint32_t base, int w, int h, float dx, float dy);
/* PC (2026-11-13): the game freed the VRAM of the buffer at base (a
 * CMD_TARGET of size -1, the frame's last commands): its targets are dead. */
void target_freed(uint32_t base);
void target_stats(int *alive, unsigned *created, unsigned *reused, unsigned *retired);
/* PC (2026-11-18): t is a live target (one of the targets kept; a debug
 * check, GT3PC_PGL_STALE_LOG: a scan of them) */
bool target_alive(const Target *t);
/* The colour target at base for a draw: the size known for base, else the
 * fallback (the scissor's extent).  Created when missing. */
Target *color_target(uint32_t base, uint32_t fbw, uint32_t psm, int fallbackW, int fallbackH);
/* The depth target at zbase for draws into `color' (D3D11 wants equal
 * sizes: one native resource per (zbase, native size), whatever the GS sizes
 * of the colour targets). */
Target *depth_target(uint32_t zbase, uint32_t zpsm, const Target &color);
/* A draw into `color' wrote z (depth_target's): z's texels now stand for
 * color's GS pixels (its GS size and FBW), which a later Z texture read
 * (the depth copies) maps them by. */
void depth_target_written(Target &z, const Target &color);
/* The depth target at base drawn as a frame (FRAME with a Z format, the
 * depth copies of graphics.md 8.3), in that frame's GS geometry. */
Target *depth_frame_target(uint32_t base, uint32_t fbw, uint32_t psm, int fallbackW, int fallbackH);
/* GT3PC_DEPTH_LOG=1: true while depth-target lines are to be logged (the
 * first 600; each call counts one). */
bool depth_log();
/* GT3PC_FLARE_LOG=1: true while readback / flare glow lines are to be
 * logged (the first 600; `line' counts one). */
bool flare_log(bool line);
/* The most recently used target at base (nullptr if none). */
Target *find_color_target(uint32_t base);
Target *find_depth_target(uint32_t base);
/* The copy of a target the draws that read it sample (made when stale):
 * refreshed where written since (DATE, C = Ad, a draw reading its own
 * target), or all of it (whole: a read of the whole picture). */
/* PC (2026-11-03): why (CopyWhy): what the copy serves, counted when one is
 * made (g_copiesWhy; the `pgl stats' line). */
enum CopyWhy { COPY_OTHER, COPY_DATE, COPY_AD, COPY_TEXTURE, COPY_ZTEX, COPY_WHY_N };
extern uint32_t g_copiesWhy[COPY_WHY_N];
ID3D11ShaderResourceView *target_copy_srv(Target &t, bool whole = false, int why = COPY_OTHER);
/* A draw wrote [l, r) x [top, b) of t (native pixels, clamped here): gen
 * bumped, the part kept for the copy's refresh (Target::dirty*). */
void target_written_rect(Target &t, int l, int top, int r, int b);
/* PC (2026-11-04): the stencil buffer that holds t's alpha mirror for a draw
 * into t with depth target z (nullptr: none): z's, else the paired one, else
 * t's own (made here); the mirror is invalid when it moved.  A nullptr for a
 * target that can have none. */
ID3D11DepthStencilView *target_mirror_dsv(Target &t, Target *z, bool claim);
/* t's pixels changed outside the draws that keep the mirror (a pass, a
 * resample): the mirror is synced again before the next DATE draw. */
void target_mirror_invalidate(Target &t);
/* PC (2026-11-05, a measurement: GT3PC_PGL_MIRROR_RESYNC=1, exec_begin_frame):
 * every mirror invalid, so each frame's first DATE draw syncs it - the game's
 * double-buffered frames in the benchmark. */
void targets_mirror_invalidate_all();
/* PC (2026-11-06, a measurement: GT3PC_PGL_BOUND_LOG=1): the alpha bound of
 * every colour target drawn this frame, logged (pgl_exec.cpp exec_end_frame) */
void targets_log_alpha_bounds();
/* The copies' refreshes this frame, whole and partial (GT3PC_PGL_STATS;
 * pgl_exec zeroes them per frame). */
extern uint32_t g_copiesWhole, g_copiesPart;
/* PC (2026-11-03): the GPU sections (pgl_exec.cpp): the section the
 * commands queued from now on count to (marked when it changes); the one
 * before (-1: the frame is not timed). */
int exec_section_want(int id);
/* What a draw samples a target through: its own view, or for a
 * multisampled one its resolve (made when the target changed since). */
ID3D11ShaderResourceView *target_srv(Target &t);
/* The single-sampled texture of what the target holds (copies, readbacks):
 * its own, or its resolve. */
ID3D11Texture2D *target_tex1(Target &t);
/* Before a frame executes: the anti-aliasing setting (settings.h) applied -
 * a new MSAA sample count re-creates every target at the output scale with
 * it (colour kept, depth cleared). */
void targets_frame_settings();
/* PC (2026-11-15): AGSS is on for the frame (targets_frame_settings): the 3D
 * draws into a multisampled target are shaded at every sample (Ps::MainSs). */
bool per_sample_frame();
/* PC (2026-11-06): at a frame's end - the cars' environment maps to or from
 * their GS size by [Graphics] EnvMapDetail (Target::envNative), the frame's
 * notes of env-pass reads cleared (Target::envRead) */
void targets_env_maps();
/* The colour target at base of GS size w x h, if there is one. */
Target *find_color_target_sized(uint32_t base, int w, int h);
/* A colour target at GS resolution from now on (its pixels resolved into
 * it: linear or point samples at the GS pixel centres; `why' for the log);
 * false if it already is (or is a depth target). */
bool target_gs_resolution(Target &t, bool linear, const char *why);
void present_target(uint32_t base, uint32_t psm, uint32_t fbw, int dx, int dy, int dw, int dh);
void present_black();
void present_last();
void present_again();           /* what is shown, as it is (pglr_present_again) */
void readback(const CmdReadback &r);
/* The readbacks' wait for the GPU in the frame's execution, ms (the frame
 * monitor's GPU WAIT: pglr_kick). */
extern double g_readbackWait;

/* PC: the execution's CPU profile (the frame monitor's verbose view,
 * platform/frame_monitor.h; pgl_exec.cpp): the time of each kind of work
 * without what it calls that is profiled too (a scope's own time), in the
 * frames of that view (g_profOn, set as a frame begins), and counts. */
enum ProfCat : uint8_t {
    PROF_OTHER,         /* the command loop, sub-lists, what no scope holds */
    PROF_GS,            /* GS register writes, vertices, GIF blocks: the GS stream into batches */
    PROF_MODEL,         /* VU1 model parts (exec_model) */
    PROF_STRIP,         /* VU1 strips / tri16: their vertices decoded on the CPU */
    PROF_UPLOAD,        /* image and Tex1 uploads into the registry */
    PROF_EFFECT,        /* effect recipes: colour correction, plane split, flare queries, half blend, FXAA */
    PROF_READBACK,      /* downloads */
    PROF_STATE,         /* a draw state from the GS registers (compute_state) */
    PROF_TEXTURE,       /* a texture's lookup (texture_bind) */
    PROF_DECODE,        /* a texture decoded (a cache miss) */
    PROF_TARGETS,       /* target copies and resolves (target_copy_srv, target_srv) */
    PROF_SUBMIT,        /* the D3D bindings and draw calls (draw_passes) */
    PROF_STREAM,        /* vertices / indices into the dynamic buffers */
    PROF_MESH,          /* a model's mesh found / checked (model_mesh) */
    PROF_MESH_DECODE,   /* a model decoded (a new or changed model, a preXyz part) */
    PROF_FLUSH,         /* a GS batch into its draw (flush_batch: the bounds, the vertex stage) */
    PROF_CLUT,          /* a CLUT load (registry_clut_load: TEX0 / TEX2 with CLD) */
    PROF_TARGET_SET,    /* a frame's targets sized (CMD_TARGET) */
    PROF_CB,            /* shader constants written (bind_vs_cb, bind_ps_cb) */
    PROF_RUN,           /* PC (2026-11-05): a resolved run replayed (run_replay: its own walk, not the draws') */
    PROF_COUNT
};
enum ProfCount : uint8_t {
    PCOUNT_DRAWS, PCOUNT_STATES, PCOUNT_TEXBINDS, PCOUNT_DECODES, PCOUNT_PARTS, PCOUNT_GSCMDS, PCOUNT_UPLOADS,
    PCOUNT_COPIES, PCOUNT_GSVERTS, PCOUNT_BATCHES, PCOUNT_CLUTS, PCOUNT_CLUTMISS, PCOUNT_COMPOSES, PCOUNT_CBS,
    PCOUNT_SYNCS, PCOUNT_CLUTSLOW, PCOUNT_TEXSLOW, PCOUNT_REGIONS, PCOUNT_PALMISS, PCOUNT_DIRECT, PCOUNT_CLIPPED,
    PCOUNT_MERGED, PCOUNT_CLIP_BEHIND, PCOUNT_CLIP_NEAR, PCOUNT_CLIP_FAR, PCOUNT_CLIP_XY, PCOUNT_CLIP_NOBOX,
    PCOUNT_TEXSTATES,
    /* PC (2026-11-04): why a VU1 part did not join the held draw (merge_join,
     * one each): no held draw or one that may not grow (two passes, C = Ad,
     * a target's texture), another draw state, another vertex stage / cull,
     * other buffers (another model) or a base vertex, its indices not the
     * next, its batch out of order or past the draw's 32, its record's depth
     * / flat shading, other pass constants, no texture slot left, other
     * vertex constants (the matrix) */
    PCOUNT_MJ_NONE, PCOUNT_MJ_STATE, PCOUNT_MJ_VS, PCOUNT_MJ_BUF, PCOUNT_MJ_INDEX, PCOUNT_MJ_BATCH, PCOUNT_MJ_REC,
    PCOUNT_MJ_PS, PCOUNT_MJ_SLOTS, PCOUNT_MJ_CB,
    /* PC (2026-11-05): the resolved runs (pgl_exec.cpp): runs replayed,
     * runs resolved (recorded) and the parts the replays drew */
    PCOUNT_RUN_HIT, PCOUNT_RUN_MISS, PCOUNT_RUN_PARTS,
    /* PC (2026-11-05): why a texture bind took the slow path (texture_bind):
     * a set's texture whose by-set front entry held another (a collision)
     * or an older state of it; not a set's at TBP0 (an image, a target's
     * alpha, a VRAM-less set) */
    PCOUNT_TEXSLOW_SET, PCOUNT_TEXSLOW_OTHER,
    /* PC (2026-11-05): draws that may write a GS alpha above 0x80 into a
     * colour target (GT3PC_PGL_ALPHA_LOG=1 only: pgl_exec.cpp alpha_log_check) */
    PCOUNT_ALPHA_HI,
    /* PC (2026-11-05): the C = Ad forms' passes as one draw of two instances
     * (pgl_d3d.h PSF_AD_INST), and those that stayed two draws */
    PCOUNT_AD_INST, PCOUNT_AD_TWICE,
    /* PC (2026-11-05): the alpha mirror's passes (queue_mirror_pass: a draw
     * drawn again for its alpha's bit) and syncs (mirror_sync) */
    PCOUNT_MIRROR_PASS, PCOUNT_MIRROR_SYNC,
    /* PC (2026-11-05): the mirror passes folded into their draws as one more
     * instance (draw_passes: mirror_fold_ok) */
    PCOUNT_MIRROR_FOLD,
    /* PC (2026-11-05): the resolved runs' textures (TexIdent): parts whose
     * texture was bound from its recorded identity, replays that resolved
     * their textures as ever (a run not identified whole, an identity that
     * failed, a decoded texture dropped since), CLUT loads left out */
    PCOUNT_RUN_TEXFREE, PCOUNT_RUN_WALK, PCOUNT_RUN_CLUTFREE,
    /* ... why a part's texture was resolved: no record (not identified when
     * recorded), its identity failed, a load its palette reads failed */
    PCOUNT_RUN_WALK_NOREC, PCOUNT_RUN_WALK_IDENT, PCOUNT_RUN_WALK_PAL,
    /* PC (2026-11-05): replayed runs whose draws went to the submit stage as
     * one record (pgl_exec.cpp RunWork) */
    PCOUNT_RUN_WORK,
    /* PC (2026-11-05): a run's work cut in two by something queued mid-run
     * that must follow its draws so far (the alpha mirror's sync) */
    PCOUNT_RUN_CUT,
    /* PC (2026-11-06, step 3): a replayed run's draws the submit stage walked
     * itself (their area, clipping and skips: pgl_exec.cpp run_submit_steps) */
    PCOUNT_RUN_WALKED,
    /* PC (2026-11-06): the alpha mirror let go instead of a large mirror pass
     * (pgl_exec.cpp submit_prologue, work_state_fill: the next DATE draw syncs
     * it), synced by a stencil clear alone (the alpha known flat:
     * Target::execAlpha), and made valid again by a covering flat alpha
     * write (flush_batch) */
    PCOUNT_MIRROR_DROP, PCOUNT_MIRROR_CLEAR, PCOUNT_MIRROR_REVAL,
    /* PC (2026-11-06): sprite batches covering the target with a flat colour
     * and depth done as clears, not drawn (pgl_exec.cpp fast_clear) */
    PCOUNT_FAST_CLEAR,
    /* PC (2026-11-06): C = Ad draws drawn from the doubled index stream (their
     * two passes as one draw's two halves: pgl_exec.cpp sub_vu1) */
    PCOUNT_AD_DOUBLE,
    /* PC (2026-11-10): [Graphics] AnisotropicFiltering - the texture states
     * given the filtering over the footprint (pgl_exec.cpp aniso_ok) */
    PCOUNT_ANISO, PCOUNT_N
};
extern bool g_profOn;
/* PC (2026-11-05): the counts of the calling stage (the executor's, the
 * submit thread's: a thread's pointer to its own; summed at the frame's end,
 * pgl_exec.cpp) */
extern thread_local uint32_t *g_profCount;
void prof_push(ProfCat c);
void prof_pop();
struct ProfScope {
    bool on;
    /* (the executor's and the submit thread's: the D3D thread's calls are not profiled) */
    explicit ProfScope(ProfCat c) : on(g_profOn && !q::on_d3d_thread()) { if (on) prof_push(c); }
    ~ProfScope() { if (on) prof_pop(); }
    ProfScope(const ProfScope &) = delete;
    ProfScope &operator=(const ProfScope &) = delete;
};
uint64_t frame_number();
void next_frame();
/* PC: the execution's clock in seconds, set as each frame begins executing
 * (exec_begin_frame): when the caches' entries were last used (their
 * eviction by age and memory, not by a count of frames - at 235 frames a
 * second 300 frames were a second and a half, and every lap decoded the
 * course's models and textures again where they came back into view). */
extern double g_execSeconds;
/* GT3PC_PGL_TRACE: the picture of frame `frame' (just traced) written at its
 * present (trace_frame_N.bmp). */
void trace_dump_next(uint64_t frame);
/* The game's FPU mode while the renderer executes (callbacks into game
 * code run in it). */
void set_game_fpu_mode(unsigned int mode);
unsigned int game_fpu_mode();

/* pgl_textures.cpp */
struct TexBinding {
    ID3D11ShaderResourceView *srv = nullptr;
    float x0 = 0, y0 = 0, w = 1, h = 1;     /* the GS texels the native texture holds */
    int   alphaMode = 0;                    /* 0 the stored alpha, 1 TA0 (CT24), 2 CT16 bit 7; PC (2026-11-13) 3: a
                                               replacement texture (pgl_texrepl.h) - the usual alpha (255 = the
                                               GS's 0x80), sampled through its mip levels */
    Target *target = nullptr;               /* a render target sampled (else an upload) */
    bool  depthTex = false;                 /* a depth target's SRV */
    /* (GT3PC_PGL_TRACE) the mean of the decoded texels, R | G << 8 | B << 16
     * | A << 24 (GS alpha); hasMean false: not decoded here (a target, a
     * surface, a plane) */
    uint32_t mean = 0;
    bool  hasMean = false;
    bool  linear = false;                   /* sampled bilinear whatever TEX1 says (a high-res image) */
    /* (the F9 trace) the decoded texture bound (pgl_textures.cpp's CachedTex;
     * nullptr: none - a target, a surface, a plane) and whether the binds'
     * front cache gave it */
    const void *src = nullptr;
    bool  fromFront = false;
    bool  fromSet = false;                  /* PC: a Tex1 set's texture (the GPU sections: a car's draw) */
    /* PC (2026-11-04): the least and greatest alpha of the decoded texels
     * (hasAlpha; not for a target, a surface, a plane, a host image): whether
     * a draw's alpha writes have one value of bit 7 (the alpha mirror,
     * pgl_exec.cpp mirror_plan) */
    uint8_t aMin = 0, aMax = 255;
    bool  hasAlpha = false;
};
void registry_init();
/* (the F9 trace) where a draw's texture came from - what the decoded texture
 * was decoded from, how many frames ago, ahead or at a draw, through the
 * front cache - and what owns TBP0's block now, flagged when that is not the
 * source: the evidence of a texture drawn with another one's picture. */
void registry_trace_texture(uint64_t tex0, const TexBinding &b, char *out, size_t n);
/* PC (GT3PC_PGL_PROFILE, 2026-11-03): with the profile's log line - the
 * textures decoded more than once in its `frames' frames (`pgl decoded
 * again' lines: what they are, what owns their blocks), the notes cleared. */
void registry_decode_report(uint32_t frames);
/* (the F9 trace) the owners of VRAM, one `pgl owners' line per run of blocks
 * with the same owner (and, for a Tex1 set, the same copy). */
void registry_trace_owners();
/* Debug aid: whether GT3PC_TEX_STRESS names kind (or `all'): fronts, clut,
 * evict (pgl_textures.cpp), thread (pgl_render.cpp). */
bool stress_on(const char *kind);
/* PC (docs/renderer.md "High-resolution images"): a readback of src's w x h
 * GS pixels at (x, y) gave `pixels' (PSMCT32): their native pixels are kept,
 * by the content, and an upload of the same pixels later is sampled from
 * them (the car icons: drawn at the output scale, read back to 64 x 32,
 * uploaded again by the race display and the result board). */
void registry_hires_readback(Target &src, int x, int y, int w, int h, const void *pixels);
void registry_upload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, int dx, int dy, int w, int h,
                     const void *data, size_t bytes);
/* A Tex1 set's uploads at the VRAM base vramBase (the set's base when the
 * upload was recorded: registry_tex1_base then); copy: registry_tex1_copy's
 * bytes taken then (nullptr: the set read at img, the game waiting). */
void registry_tex1_upload(const void *img, uint32_t vramBase, const void *copy);
uint32_t registry_tex1_base(const void *img);
/* (the game thread, at the record) the set's header, register blocks and
 * upload records - what the executor reads of it - into out
 * (registry_tex1_copy_size bytes): the game relinks a set in place
 * (tex1_set_vram_base) while the executor thread runs the frame before. */
size_t registry_tex1_copy_size(const void *img);
void registry_tex1_copy(const void *img, void *out);
/* A set (re)loaded at img (the game thread, the executor idle): a new load
 * stamp; its textures are decoded ahead (pgl_prewarm.cpp). */
void registry_tex1_loaded(void *img);
/* PC (ROADMAP 18; CMD_TEX_SOURCE): the draws with TEX0 tex0 sample the set
 * at img, linked for vramBase but never uploaded (it found no VRAM), and the
 * CLUT loads of that TEX0's CBP read it; img nullptr: none (every frame
 * starts so).  copy: as registry_tex1_upload's. */
void registry_texture_source(const void *img, uint32_t vramBase, uint64_t tex0, const void *copy);
/* pgl_host_image.cpp (pglr_host_image_add): TEX0 (CLD masked off) names a
 * host image - bound into out, brought up to date - else false. */
bool host_image_bind(uint64_t tex0Key, TexBinding &out);
/* PC (pgl_movie.cpp): a movie's textures opened or let go (the executor, at
 * each list), and the draw that samples the picture shown. */
void movie_frame();
bool movie_bind(TexBinding &out);
/* pgl_targets.cpp: the output scale set (pglr_text_scale_x / _scale). */
void host_text_scale(float sx, float sy);
/* A draw into t writes its blocks (the RGB / Z layer, the top-byte layer). */
void registry_target_written(Target &t, bool rgb, bool top);
void registry_target_released(Target &t);
/* The colour target whose pixels the GS memory at `block' holds (the owner
 * of its RGB layer) and where that block lies in it (GS pixels); nullptr if
 * the block's owner is not a colour target. */
Target *registry_colour_target_at(uint32_t block, int &ox, int &oy);
/* pfx_split_rgb_planes (graphics.md 8.6): the R, G, B bytes of src's GS
 * pixels 0..w-1 x 0..h-1 (a capture of its native pixels) become the top
 * bytes - the PSMT8H / T4HL / T4HH texels - of the w x h PSMCT32 planes at
 * blocks[0], [1], [2]. */
void registry_split_planes(Target &src, const uint32_t blocks[3], int w, int h);
void registry_clut_load(uint64_t tex0);      /* a TEX0 / TEX2 write with CLD != 0 */
/* Resolves the texture of TEX0 / TEX1 / CLAMP / TEXA; false: nothing to
 * sample (the caller skips the draw).  writeMask: the colour channels the
 * draw writes (pgl::gs::WRITE_*; none of R, G, B: a PSMCT32 texture is read
 * for its alpha, from the owner of its top bytes).  drawing: the colour
 * target of the draw (sampling it gives its copy). */
bool texture_bind(uint64_t tex0, uint64_t tex1, uint64_t clamp, uint64_t texa, uint8_t writeMask,
                  Target *drawing, TexBinding &out);
void registry_end_frame();
/* PC (2026-11-07): the decoded textures' bytes in the cache (the log's
 * memory line, pgl_prewarm.cpp; read without the lock: a figure). */
size_t registry_cache_bytes();

/* PC (CPU time, 2026-11-03): the uploads of a Tex1 set under a CLUT's or a
 * texture's blocks (their indices and versions, at most kUploadRefs) - what
 * a cache entry of that set checks instead of the set's data version: ~15
 * sets of a 32-car race change a small upload (a lamp's CLUT) every frame,
 * and each such change threw away every load and bind of its set.
 * PC (CPU time, 2026-11-05): up to 16 - with 4, a texture under more than
 * 4 of its set's uploads (a page with several CLUTs in its corners) fell
 * back to the set's data version, which a lamp's CLUT moved every frame,
 * and every texture of ~15 sets of a 32-car race took the slow path each
 * frame (TEX SLOW 642 a frame, ~1.4 us each). */
constexpr uint32_t kUploadRefs = 16;
struct UploadRefs {
    uint8_t n = 0xFF;                       /* 0xFF: none kept (over kUploadRefs) */
    uint8_t idx[kUploadRefs] = {};
    uint32_t ver[kUploadRefs] = {};
};
/* PC (2026-11-05, the resolved runs' textures: pgl_exec.cpp run_replay):
 * what a texture bind read, in the terms of the objects it read it from
 * rather than the VRAM addresses - a Tex1 set's texture: the set (its
 * object and parse), TBP0's place in it, the uploads under its footprint
 * and the TEX0 / CLAMP / TEXA fields without the addresses; a render
 * target's: the target and TBP0's place in it.  A bind whose identity holds
 * (texture_ident_check: the same set owns the live TBP0 at the same place,
 * its uploads under the texture unchanged, no decoded texture dropped since)
 * binds the same texture, so a run of model parts resolved once binds its
 * parts' textures from their recorded identities instead of resolving each
 * TEX0 through the owner map, the CLUT buffer and the front caches again.
 * kind 0: not identified so (an image, a surface, a plane, a VRAM-less
 * set's, a host image, the target being drawn, a target's alpha). */
struct TexIdent {
    uint8_t kind = 0;                       /* 0 none, 1 a Tex1 set's, 2 a render target's */
    uint8_t layer = 0;
    uint32_t obj = 0, parseVer = 0, contentVer = 0;
    uint32_t ofs = 0;                       /* TBP0's blocks from the set's (the target's) base */
    uint64_t tex0 = 0, clamp = 0, texa = 0; /* TEX0 without TBP0 / CBP / CLD; CLAMP; TEXA */
    UploadRefs refs;
    Target *target = nullptr;
    uint64_t epoch = 0;                     /* g_tagEpoch: the decoded texture it points at is kept while it stays */
    const void *ct = nullptr;               /* the decoded texture (pgl_textures.cpp CachedTex) */
    uint32_t ctGen = 0;                     /* PC (2026-11-13): its gen then (a replacement loaded since moves it) */
};
/* ... and a CLUT load's: the set, CBP's place in it, the fields (CPSM, CSM,
 * CSA, T8) and the uploads under the CLUT's blocks (kind 0: not a set's).
 * rows: the rows of the CLUT buffer it writes (a bit each, 32). */
struct ClutIdent {
    uint8_t kind = 0;
    uint32_t obj = 0, parseVer = 0, contentVer = 0, ofs = 0;
    uint32_t fields = 0;
    uint32_t rows = 0;
    UploadRefs refs;
};
void texture_ident_capture(bool on);        /* identities taken (a run recorded or replayed), or not (no cost) */
void texture_last_ident(TexIdent &out);     /* of texture_bind's last call */
/* The recorded bind stands for TEX0 / CLAMP / TEXA as they are now (the
 * decoded texture it names is marked used). */
bool texture_ident_check(const TexIdent &id, uint64_t tex0, uint64_t clamp, uint64_t texa);
uint32_t clut_load_serial();                /* the indexed CLUT loads so far (registry_clut_load) */
bool clut_load_indexed(uint64_t tex0);      /* the TEX0 names an indexed format: a write with CLD loads */
void clut_last_ident(ClutIdent &out);       /* of the last indexed load */
bool clut_ident_check(const ClutIdent &id, uint64_t tex0);  /* the recorded load reads the same CLUT now */

/* pgl_models.cpp */
/* The decoded mesh of a part's model (cached) and its buffers; preXyz parts
 * are decoded for the draw.  The part's batches lie in [firstBatch,
 * endBatch) of mesh->batches (their part still to be checked).  false:
 * nothing to draw.  PC: batchVb, its vertices' batches (a merged draw's:
 * pgl_exec.cpp MergedDraw; null for the decoded-for-the-draw ones). */
/* PC (2026-11-05): serial, if given - the decode the mesh is of (a new
 * number at every decode of the model: the resolved runs, pgl_exec.cpp,
 * check their models by it). */
/* PC (2026-11-06): ib2 / nVerts, if given - the model's doubled index stream
 * and the vertices of one copy (pgl_models.cpp make_mesh_buffers; null / 0:
 * none - a streamed part, or the batches do not tile the indices). */
bool model_mesh(const PglrModelPart &part, const gt3vif::Mesh **mesh, ID3D11Buffer **vb,
                ID3D11Buffer **ib, uint32_t *baseVertex, uint32_t *baseIndex, uint32_t *firstBatch,
                uint32_t *endBatch, ID3D11Buffer **batchVb, uint64_t *serial = nullptr,
                ID3D11Buffer **ib2 = nullptr, uint32_t *nVerts = nullptr);
/* PC (docs/renderer.md "Overlays"): a ModelSet's packets loaded, in its
 * draw order (the game thread): the overlays found - packets whose
 * triangles lie on an earlier packet's, a layer above the highest they lie
 * on; model_overlay (the executor thread): that packet's layer (0: not an
 * overlay; vu1_depth draws it nearer by it) */
void models_set_loaded(const void *const *packets, int n);
int model_overlay(const void *model);
/* PC (2026-11-05): a number that moves whenever the overlays change (the
 * resolved runs keep their parts' layers while it stays). */
uint32_t models_overlay_gen();
/* PC (2026-11-05): the game writes the model frame after frame (the tyre
 * packets, rebuilt in place: decoded again at each frame) - its parts stay
 * out of the resolved runs. */
bool model_volatile(const void *model);
/* pglr_model_written (any thread): the model is decoded again at its next
 * use. */
void models_written(const void *model);
/* pglr_model_loaded (the game thread): as models_written, and the model is
 * decoded ahead (pgl_prewarm.cpp). */
void models_loaded(const void *model);
void models_end_frame();
/* PC (2026-11-07): the decoded meshes' bytes in the cache (the log's
 * memory line; a figure). */
size_t models_cache_bytes();

/* pgl_prewarm.cpp: PC (docs/renderer.md 7, "Decoding ahead") - what the game
 * loads, decoded on a worker thread before its first draw.  A job holds a
 * copy of what it decodes (never the game's memory) and is run, then
 * deleted, on the worker. */
namespace pre {
struct Job {
    virtual ~Job() = default;
    virtual void run() = 0;
};
void start();                           /* (pglr_init; GT3PC_PREWARM=0: no worker) */
bool on();
void post(Job *job);                    /* (taken over) */
/* The log's counts: made on the worker (n of them, bytes decoded), taken at
 * a draw, decoded at a draw after all (the executor's thread). */
enum Kind { MODEL, TEXTURE };
void made(Kind k, uint32_t n, size_t bytes);
void taken(Kind k);
void late(Kind k);
/* QPC seconds, as g_execSeconds counts them (any thread). */
double seconds();
}  /* namespace pre */

/* pgl_flicker.cpp: GT3PC_FLICKER (docs/renderer.md "Debug aids") - the last
 * pictures presented and the draws of their frames kept; F8 writes the
 * pictures that differ from both neighbours (a flicker of one frame) and
 * what their frames drew differently. */
struct FlickerDraw {
    uint8_t  kind;              /* 0 GS primitives, 1 VU1, 2 a skipped GS batch, 3 a skipped VU1 draw */
    uint8_t  why;               /* skipped: 1 nothing passes, 2 no target, 3 no texture, 4 scissor, 5 other */
    uint16_t prim;              /* PRIM */
    uint32_t count;             /* vertices / indices */
    uint32_t rt;                /* FRAME's base block (~0: none) */
    uint32_t tex;               /* TEX0: TBP0 | PSM << 16 | 1 << 31 (0: untextured) */
    uint32_t texMean;           /* the decoded texels' mean (0: not decoded there) */
    uint32_t state;             /* a hash of the blend, the tests, the masks, the scissor */
    uint32_t data;              /* a hash of the vertices (GS) or of the VU1 matrices and range */
    int16_t  x0, y0, x1, y1;    /* the GS pixels of rt it can touch */
    uint32_t srv;               /* the texture view it samples (its address's low bits; 0: none) */
    uint32_t ps;                /* a hash of its pixel shader constants (CbPs) */
    uint32_t rgba;              /* GS primitives: the first vertex's colour */
    float    u, v;              /* ... its texel coordinates */
    uint32_t bound;             /* the D3D state the binding cache had wrong at its draw (d3d_check_bound bits) */
    uint32_t rtv;               /* the render target view drawn into (its address's low bits) */
    /* what the GPU did with it (a pipeline statistics query, read frames
     * later; ~0: not read): vertices read, primitives rasterized, pixel
     * shader invocations */
    uint32_t gpuVerts, gpuPrims, gpuPixels;
    /* PC: the samples that passed its depth and alpha tests (an occlusion
     * query; ~0: not read): with gpuPixels, a triangle that failed a test
     * (shader runs kept, fewer passing: the alpha test or a late depth test;
     * fewer runs: an early depth test) from one not rasterized (gpuPrims) */
    uint32_t gpuPassed;
    /* what the GPU held for it that differs from what was written (read with
     * the counts; d3d_verify_resolve bits: 1 VS constants, 2 PS constants,
     * 4 vertices, 8 indices) */
    uint32_t gpuBad;
};
bool flicker_on();
void flicker_draw(const FlickerDraw &d);
/* A pipeline statistics query begun for the draw to be kept next; ended
 * after its passes by flicker_query_end, which also copies what the GPU
 * holds for the draw (d3d_verify_copy).  nullptr: none. */
ID3D11Query *flicker_query_begin();
void flicker_query_end(ID3D11Query *q, bool indexed, uint32_t first, uint32_t count, int32_t baseVertex);
/* Something of note in the frame executing, before the next draw (a
 * stream restarted, a texture decoded, a target made, a binding wrong). */
void flicker_event(const char *fmt, ...);
void flicker_end_frame(uint64_t frame);             /* the frame executed: its draws kept */
/* present_now: the picture shown (uv: its part of the target, gsW x gsH GS
 * pixels; nullptr: black), the displayed buffer's base. */
void flicker_capture(ID3D11ShaderResourceView *picture, const float uv[4], int gsW, int gsH, uint32_t disp);
void flicker_poll();                                /* present_now: F8 */

/* pgl_exec.cpp */
void exec_init();
void exec_list(const CmdList &list);
void exec_begin_frame();
void exec_end_frame();

/* Sub-lists (pgl_render.cpp) */
const CmdList *sub_list(const void *key);

/* Logging */
void log_once(const char *key, const char *fmt, ...);

}  /* namespace pglr */

#endif /* PGL_NATIVE_PGL_RENDER_H */
