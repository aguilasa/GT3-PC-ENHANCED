/* pgl_exec.cpp - executes a frame's command list on Direct3D 11
 * (docs/renderer.md "Execution").
 *
 * The executor keeps the GS register state the recorded commands describe -
 * pgl's register flushes, A+D writes, the GIF blocks the game wrote (decoded
 * here as data: PACKED / REGLIST / IMAGE tags), the PRIM of the VU1 draws -
 * and turns primitives into native draws:
 *   - GS-space primitives (pgl's 2D paths, GIF blocks): the GS vertex queue
 *     of the PRIM type (points, lines, strips, fans, sprites; XYZ2 kicks,
 *     XYZ3 does not), vertices converted to native pixels of the target,
 *     batched while the state stays the same;
 *   - VU1 work (strips, tri_draw16, model parts): the vertex shader does what
 *     the VU1 programs did (docs/models.md 5).
 * The state of a draw is pgl::gs::translate of the shadows of PRIM.CTXT's
 * context: blend, depth, alpha-test passes, write masks, scissor, fog; the
 * frame / Z buffers resolve to native targets (pgl_targets.cpp), TEX0 to a
 * native texture (pgl_textures.cpp).
 *
 * Coordinates (docs/renderer.md "Pixels"): a GS window position w (pixels,
 * (X - OFX) / 16) is native w x s + 0.5 (s = native / GS size), so GS pixel
 * x covers native pixels [x s, (x + 1) s) and integer-edged sprites fill
 * whole blocks; a sprite's texture coordinates are shifted by slope x (0.5 /
 * s - 0.5) so that texel centres stay where they are at 1:1 (exact at s = 1,
 * 1:1 copies of native-size targets stay pixel exact at any s).  2D drawn
 * for the 4:3 screen and squeezed by k into the GS frame (the race's dynamic
 * aspect: CMD_DESIGN) is 1:1 in design pixels: the shift is slope x (0.5 / s
 * - 0.5 k).  A multisampled target (MSAA) maps w to native w s, without the
 * 0.5: there the 0.5 put integer edges on the pixel centres, half of the
 * edge pixels' samples outside (the left / top column and row of a
 * full-screen copy blended with what was there, again at every copy: a smear
 * along the picture's left and top edges, user report); the sprites' shift
 * is then slope x (- 0.5 k) (pix / s - 0.5 k in general), which keeps 1:1
 * copies exact as well.  A sprite's samples are held to the texels of the
 * GS's own first and last samples (GsVtx tu0..tv1, emit_sprite): its native
 * pixels outside those read past them, an atlas' next picture (docs/
 * renderer.md 4, "Sprite samples"). */
#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_win32.h"   /* (the Win32 calls off Windows too) */
#include "engine/pgl/native/pgl_capture.h"  /* PC (2026-11-06): the frame capture (F11) */

#include <algorithm>
#include <atomic>
#include "engine/pgl/native/pgl_intrin.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "engine/pgl/native/gs_tex_format.h"
#include "platform/frame_monitor.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/sample_prof.h"
#include "platform/settings.h"
#include "platform/window.h"

namespace pglr {

void flush_batch();

namespace {

namespace gs = pgl::gs;
namespace pr = pgl::prim;

/* PC (2026-11-05): the flare queries' batch (exec_flare_query, below) */
void flare_queries_flush();
bool flare_queries_pending();
bool flare_sprite_ok();

/* GS register addresses */
enum : uint32_t {
    A_PRIM = 0x00, A_RGBAQ = 0x01, A_ST = 0x02, A_UV = 0x03, A_XYZF2 = 0x04, A_XYZ2 = 0x05,
    A_FOG = 0x0A, A_XYZF3 = 0x0C, A_XYZ3 = 0x0D, A_PRMODE = 0x1B,
    A_BITBLTBUF = 0x50, A_TRXPOS = 0x51, A_TRXREG = 0x52, A_TRXDIR = 0x53
};

/* GsContext.reg[] / gen[] order -> GS addresses (context 2: + 1) */
const uint8_t kCtxAddr[gs::REG_COUNT] = {0x4C, 0x18, 0x4A, 0x4E, 0x40, 0x42, 0x47, 0x08, 0x16, 0x14, 0x06,
                                          0x34, 0x36};
const uint8_t kGenAddr[gs::GEN_COUNT] = {0x46, 0x44, 0x45, 0x3D, 0x49, 0x1A, 0x22, 0x3B, 0x1C, 0x3F};

int8_t g_addrReg[0x80];     /* address -> reg[] index (-1) */
int8_t g_addrCtx[0x80];     /* address -> context 0 / 1 */
int8_t g_addrGen[0x80];     /* address -> gen[] index (-1) */

/* A vertex as the GS registers held it at its XYZ write. */
struct Vtx {
    uint16_t x = 0, y = 0;
    uint32_t z = 0;
    uint8_t f = 0;
    uint32_t rgba = 0;
    float s = 0, t = 0, q = 1;
    uint16_t u = 0, v = 0;
};

struct GsState {
    uint64_t ctx[2][gs::REG_COUNT] = {};
    uint64_t gen[gs::GEN_COUNT] = {};
    uint32_t prim = 0;
    bool fullColor = false;                 /* PC: PRIM_FULL_COLOR (prim holds the GS's bits) */
    uint64_t prmode = 0;
    /* vertex attribute registers */
    uint32_t rgba = 0x80808080u;
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0;
    uint8_t fog = 0;
    /* the vertex queue */
    Vtx queue[2];
    Vtx fan0;
    int count = 0;
    /* transfers */
    uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
};
GsState g;

struct Xfer {
    bool active = false;
    uint32_t dbp = 0, dbw = 0, dpsm = 0;
    int dx = 0, dy = 0, w = 0, h = 0;
    uint32_t bytes = 0;
    std::vector<uint8_t> data;
};
Xfer g_xfer;

/* The state of the current batch. */
struct BatchState {
    bool valid = false;
    bool skip = false;              /* nothing drawn (no passes, no target, no texture) */
    gs::DrawState ds;
    uint32_t type = 0;              /* PRIM type */
    Target *rt = nullptr;           /* colour target (the depth target for depthFrame) */
    Target *zt = nullptr;           /* depth buffer (nullptr: no depth access) */
    bool depthFrame = false;        /* FRAME with a Z format: a depth copy */
    bool tme = false, fst = false, flat = false;
    TexBinding tex;
    ID3D11SamplerState *samp = nullptr;
    float texW = 1, texH = 1;
    uint16_t ofx = 0, ofy = 0;
    float sx = 1, sy = 1;
    float pix = 0.5f;                   /* native = GS x s + pix (see the file comment) */
    uint8_t zpsm = 0x31;
    D3D11_RECT scissor = {};
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    CbPs ps = {};
    ID3D11ShaderResourceView *dateSrv = nullptr;    /* DATE: the copy of rt the destination alpha is read from */
    bool dateStencil = false;           /* PC (2026-11-04): DATE by the alpha mirror's stencil (no copy) */
    uint64_t tex0 = 0;                  /* (GT3PC_PGL_TRACE) */
    /* PC (CPU time): the D3D states of this state's passes (draw_passes:
     * [pass][rep] - a C = Ad form's two passes, [2] the two as one draw of
     * two instances; the rasterizer by cull), found once instead of at each
     * draw; nullptr not yet (PC, 2026-11-05: the blend states found with the
     * state, state_passes - a resolved run's states keep them) */
    ID3D11BlendState *blend[2][3] = {};
    ID3D11RasterizerState *raster[4] = {};
    /* PC (2026-11-05): what its passes write (state_passes): a draw's
     * bookkeeping on the executor (draw_bookkeep), its passes on the submit
     * stage */
    bool wroteRgb = false, wroteTop = false, wroteZ = false;
};
BatchState g_bs;
/* PC (2026-11-05): the submit stage has the state as it is (post_state);
 * false after compute_state / texture_refresh changed it */
bool g_subPosted = false;
uint32_t g_stateSerial = 0;             /* PC: compute_state's count (MergedDraw) */
bool g_dirty = true;
/* PC (CPU time, 2026-11-03): only texture registers written since the state
 * (texture_state); GT3PC_PGL_TEXSTATE=0 - the whole state each time */
bool g_dirtyTex = false;
bool g_texStateOn = true;
/* The 2D design squeeze of the draws (CMD_DESIGN, pglr_set_design_scale): a
 * GS pixel of them is (g_designX, g_designY) design pixels of the 4:3 screen
 * the game drew them for. */
float g_designX = 1.0f, g_designY = 1.0f;
/* The width of the GS lines in native pixels (CMD_LINE_WIDTH,
 * pglr_set_line_width): above 1 they are drawn as quads (emit_line). */
float g_lineWidth = 1.0f;
std::vector<GsVtx> g_verts;
/* PC: the batch's render-target sprites whose samples emit_sprite holds: the
 * first of their 6 vertices in g_verts, their corners on the screen and in
 * the texture (cleared with g_verts). */
struct HeldSprite {
    size_t at;
    float x0, x1, y0, y1;
    float u0, u1, v0, v1;
};
std::vector<HeldSprite> g_held;
int g_subDepth = 0;

/* PC: a TEXFLUSH while the batch samples the target it draws into (it reads
 * the target's copy).  The primitives after it must see what was drawn
 * before it: once, the batch ended there and the next primitive took a fresh
 * copy - each heat-haze / rain drop (particles.c: a TEXFLUSH per drop)
 * drew alone after a refresh of the copy, a few hundred GPU write-then-read
 * dependencies a frame (user report: replays slow).  Now the area written
 * since the copy (the target's dirty part and the batch's vertices) is kept
 * (g_fbRect); a primitive that reads none of it joins the batch (the copy
 * holds what it reads), one that reads some ends it and takes the fresh copy
 * first (feedback_check).  Cleared by a new state (its copy is fresh). */
bool g_fbPending = false;
/* (the parts written since the copy, each as drawn: the target's part from
 * before the batch, then the batch's vertices between two TEXFLUSHes, one
 * rectangle each - one bounding box of them all, the drops spread over the
 * screen, took a third of the drops for overlapping: user report, the
 * third) */
std::vector<D3D11_RECT> g_fbRects;
size_t g_segStart;                      /* g_verts' first vertex after the last TEXFLUSH */

/* The run of vertices since the last TEXFLUSH into g_fbRects (their
 * bounding box, native pixels, 2 more around). */
void fb_segment_end()
{
    if (g_verts.size() > g_segStart) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (size_t i = g_segStart; i < g_verts.size(); i++) {
            x0 = std::min(x0, g_verts[i].x);
            y0 = std::min(y0, g_verts[i].y);
            x1 = std::max(x1, g_verts[i].x);
            y1 = std::max(y1, g_verts[i].y);
        }
        g_fbRects.push_back({(LONG)std::floor(x0) - 2, (LONG)std::floor(y0) - 2, (LONG)std::ceil(x1) + 2,
                             (LONG)std::ceil(y1) + 2});
    }
    g_segStart = g_verts.size();
}

/* GPU time by section (below, exec_begin_frame); sec_active: this frame is
 * timed */
bool sec_on();
bool sec_active();
void sec_mark(int id);
/* PC: a mark only when id is not the section marked last; the one before
 * (-1: not timed).  g_secGame: the game's current section (CMD_SECTION);
 * sec_last: the section marked last (-1: none) */
int sec_want(int id);
int sec_last();
extern int g_secGame;

/* DATE (TEST.DATE: a pixel is written only where bit 7 of the destination
 * alpha equals DATM).  The pixel shader reads the destination alpha from a
 * copy of the target (D3D11 cannot read the bound target), taken when a DATE
 * draw state is set up, unless the draws into the target since the last copy
 * were all DATE draws: a run of DATE draws (the env-map passes of a model,
 * one per VU1 part) reads the alpha as it was before the run.  Inexact only
 * where a DATE draw that writes alpha overlaps itself or an earlier draw of
 * its run. */
struct DateRun {
    Target *rt = nullptr;               /* the target of the last DATE draw */
    uint64_t gen = 0;                   /* its gen after that draw */
} g_dateRun;

/* PC (2026-11-04): DATE by the stencil instead (docs/renderer.md 3, "DATE"):
 * bit 7 of a colour target's alpha is mirrored in the stencil of the depth
 * buffer it is drawn with (Target::mirror*, target_mirror_dsv) - a draw that
 * writes the alpha with one value of bit 7 writes the mirror by its stencil
 * op (mirror_plan: the value known from its texture's and its vertices'
 * alpha, the lighting, the alpha test); one whose bit varies clears the
 * mirror where it draws and is drawn again as a mirror pass (queue_mirror_pass:
 * the same pixels, its alpha worked out again, the stencil set where the bit
 * is 1, no colour) - and a DATE draw is a stencil test, no copy.  Exact but
 * where a draw without depth writes overlaps itself with both values (the GS
 * keeps the last, the mirror the first 1); the copies' inexactness (a DATE
 * draw's own alpha writes unseen by the draws after it in its run) is gone.
 * The mirror is kept only for targets a DATE draw has read (Target::dated),
 * synced from a copy at their first DATE draw when invalid (mirror_sync: at
 * each frame of a double-buffered screen, the two frames sharing one Z
 * buffer).  GT3PC_PGL_DATE_STENCIL=0: the copies as before (exec_init). */
bool g_dateStencil = true;
/* What a draw does to the mirror (mirror_plan): its stencil op and ref for
 * its colour pass; pass - a mirror pass follows (keep1: the bit the mirror
 * already holds where it drew, which that pass leaves; the pass's depth
 * test: func, without the write). */
struct MirrorPlan {
    StencilOp op = StencilOp::None;
    uint8_t ref = 0;
    bool pass = false;
    bool keep1 = false;
    gs::Compare func = gs::Compare::Always;
    bool test = false;
    uint8_t cause = 0;                      /* PC (2026-11-05): why the pass (mirror_cause: 1 texture, 2 vertex, 3 both, 4 neither) */
};

/* PC (2026-11-03): GT3PC_PGL_CLIP_ALL=1 - every VU1 draw through the clipper
 * (gs_vu1), none without it (vu1_draw, vs_vu1_direct): to compare.  Read at
 * exec_init. */
bool g_noDirect = false;
/* PC (2026-11-05): GT3PC_PGL_AD_INST=0 - a C = Ad form's two passes as two
 * draws always, never as one draw of two instances (pgl_d3d.h PSF_AD_INST):
 * to compare (the pictures must be the same).  Read at exec_init. */
bool g_adInstOn = true;
/* PC (2026-11-06): GT3PC_PGL_DATE_PAIR=0 - a C = Ad draw that flips the
 * alpha mirror's bit (the cars' env passes: a DATE test, the alpha written
 * of the other value) as two draws, the first keeping the stencil and the
 * second inverting it, as before; on, it is one draw of two instances whose
 * stencil op counts (pgl_d3d.h StencilOp::DatePair): to compare (the
 * pictures must be the same).  Read at exec_init. */
bool g_datePairOn = true;
/* PC (2026-11-05, measurements of the GPU's work - the pictures wrong):
 * GT3PC_PGL_AD_ONCE=1 - a C = Ad draw of two instances drawn as one (Cs Ad /
 * 255: half the add); GT3PC_PGL_MIRROR_PASS=0 - the alpha mirror's passes
 * not drawn (the mirror wrong where a draw's alpha bit varies).  What the
 * second rasterization of the env passes and the mirror passes cost, by the
 * GPU sections with and without.  Read at exec_init. */
bool g_adOnce = false;
bool g_mirrorPassOn = true;
/* PC (2026-11-05): the mirror passes folded into their draws as one more
 * instance (mirror_fold_ok, draw_passes; docs/renderer.md 3, "DATE").
 * Off: on the user's GTX 1660 SUPER the fold cost the GPU more than the
 * passes did (SV_Depth and the depth buffer read as a texture turn the early
 * depth test and the depth compression off for the heaviest draws: cars 7.5
 * -> 11.3 ms, the frame 14.1 -> 17.9; the passes' cost is their pixel work,
 * a third rasterization, not their draw calls).  GT3PC_PGL_MIRROR_FOLD=1
 * turns it on (exec_init). */
bool g_mirrorFoldOn = false;
/* PC (2026-11-06): the lazy mirror (docs/renderer.md 3, "DATE").  The user's
 * SSR5 capture (frame 61937, 150 %) kept the mirror through the whole course
 * because the night sky is a DATE draw: 683 mirror passes, 33 Mpx of pixel
 * work, 26 of them in the frame's first tenth, where the next DATE draw into
 * the frame came 4,300 calls later; its sync copied the frame and passed
 * over it (9.3 Mpx) although a flat clear had just written its alpha.  So:
 * (1) a draw whose mirror pass would cover an eighth of the target or more
 * lets the mirror go instead (submit_prologue: the draw's area; a run's
 * state: the largest pass its draws needed the last time, RunState::
 * passArea) - a DATE draw still tests, writes no pass, and the next DATE
 * draw syncs; (2) a sync of a single-sampled target reads the target itself,
 * no copy, and when the executor knows the alpha flat over the target
 * (Target::execAlpha) it is a stencil clear alone; (3) a sprite batch
 * covering the target with a flat alpha and nothing gating a pixel makes the
 * mirror valid again by its own stencil writes (flush_batch).  Exact: the
 * stencil holds the alpha's bit wherever a DATE draw tests it.
 * GT3PC_PGL_MIRROR_LAZY=0: as before (every pass drawn while the mirror is
 * valid, the sync from the alpha); GT3PC_PGL_MIRROR_SYNC_COPY=1: the sync
 * from a copy of the target, as before (exec_init). */
bool g_mirrorLazyOn = true;
bool g_mirrorSyncCopy = false;
constexpr uint32_t kMirrorDropFrac = 8;     /* a pass over 1 / this of the target: the mirror let go */
uint32_t g_mirrorDropPx = 0;                /* GT3PC_PGL_MIRROR_DROP_PX=N: that many pixels instead (a test: 1 lets it go at every pass) */
/* PC (2026-11-06): the fast clears (flush_batch, fast_clear; docs/renderer.md
 * 3, "Fast clears").  The game clears its frames with sprites - the colour,
 * the depth (a sprite writing nothing but Z), the alpha alone - and at 150 %
 * each of them rasterizes 4.7 Mpx: the user's day-grid capture (frame 9733)
 * had 23.5 Mpx of them against a scene of 27.  A sprite batch covering the
 * target with one flat colour and depth, nothing gating a pixel, goes as
 * ClearRenderTargetView / ClearDepthStencilView instead; an alpha-only one
 * whose value the target holds everywhere already (Target::execAlpha) writes
 * nothing.  GT3PC_PGL_FAST_CLEAR=0: drawn as before (exec_init). */
bool g_fastClearOn = true;
/* PC (2026-11-06): the doubled index stream (pgl_models.cpp make_mesh_buffers;
 * docs/renderer.md 3, "C = Ad") - a C = Ad draw's two passes as one draw's
 * two halves, so consecutive env passes join (sub_vu1, merge_join).
 * GT3PC_PGL_AD_DOUBLE=0: the two-instance draws as before (exec_init; read
 * by pgl_models.cpp too, which then builds no doubled stream). */
bool g_adDoubleOn = true;
/* PC (2026-11-05): GT3PC_PGL_MIRROR_LOG=1 - why each mirror pass is needed
 * (mirror_cause: the vertex and texture alpha ranges), the first 40 logged;
 * the stats line counts the causes always. */
bool g_mirrorLog = false;
/* PC (2026-11-05): the targets' alpha bounds (pgl_render.h Target::alphaLo,
 * target_alpha_written, alpha_write_range): a draw textured from a target
 * whose alpha is bounded writes a bounded alpha - the cars' env passes
 * sample the env map, a CT16 target whose alpha the game never draws (its
 * draws mask it; it is copied whole from another such target), read through
 * TEXA as one of two values, so every env pass needed a mirror pass
 * (~2,600 of a 32-car frame's 3,400, the user's GT3PC_PGL_MIRROR_LOG run).
 * GT3PC_PGL_ALPHA_BOUND=0: off, as before (exec_init). */
bool g_alphaBoundOn = true;

/* PC (CPU and GPU time, 2026-11-03): the executor's last indexed draw, held
 * back (q::hold) - a draw that goes on from where it ends, with nothing
 * queued between (no state, constants, buffers or copies changed: they
 * would have been queued, and released it first), extends it: the
 * consecutive parts of a model with one texture and one state, ~2,700 of a
 * 32-car race frame's draws.  The same calls, fewer of them.  Not with
 * GT3PC_FLICKER or the F9 trace (they look at each draw), nor without the
 * D3D thread (the context is used directly then).  GT3PC_PGL_MERGE=0: off
 * (exec_init). */
/* PC (CPU and GPU time, 2026-11-03): and the next batches of the model with
 * the same state and constants join it with textures of their own - each
 * in a slot (up to kMergeSlots texture / sampler pairs), its values in the
 * parts' table (up to kMergeParts parts: pgl_d3d.h CbParts; vs_vu1 finds a
 * vertex's part by its first vertex): one draw for a run of a car's parts,
 * where each was a draw, its texture bound alone (merge_join, in vu1_draw
 * before anything is bound).  A part's bookkeeping (the written rectangle,
 * the DATE run, the Z buffer) is done as it joins.  Joins: one pass, no C =
 * Ad, a decoded texture (not a target's), the same record values but the
 * texture's, the vertices after the last part's, a mesh of its own (base
 * vertex 0). */
struct MergedDraw {
    uint32_t count = 0, first = 0;
    int32_t base = 0;
    uint32_t inst = 0;                      /* PC: its record (vu1_inst): the same one to join */
    uint32_t instances = 1;                 /* PC (2026-11-05): 2 for a C = Ad draw of two instances (PSF_AD_INST) */
    int rep = 0;                            /* PC (2026-11-05): its pass's repetition (draw_passes: 0 / 1, 2 the two as one) */
    bool mergeable = false;                 /* (parts may join: merge_join) */
    uint32_t serial = 0;                    /* g_stateSerial */
    Vs vs = Vs::Vu1;
    int cull = 0;
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    CbPs ps = {};                           /* the pass's constants, the texture's fields aside */
    float zFromW[4] = {};
    float flat = 0.0f;
    Ps shader = Ps::Main;                   /* (its pixel shader: draw_merged's merged variant of it) */
    uint32_t firstBatch = 0, lastBatch = 0; /* its first and last parts' batches (the records by batch) */
    bool wroteZ = false;                    /* its state's passes write depth (the mirror pass's depth test) */
    uint32_t nSlots = 0, nParts = 0;
    ID3D11ShaderResourceView *srv[kMergeSlots] = {};
    ID3D11SamplerState *samp[kMergeSlots] = {};
    Vu1Part parts[kMergeParts];
    MirrorPlan plan;                        /* PC (2026-11-04): the parts' (all the same: merge_join) */
    bool folded = false;                    /* PC (2026-11-05): its mirror pass is its last instance (no pass after it) */
    uint32_t capId = ~0u;                   /* PC (2026-11-06): the frame capture's record of it (pgl_capture.h) */
    uint32_t capParts = 1;                  /* PC (2026-11-06): its parts (merge_join's, the joins': nParts counts gaps too) */
    bool doubled = false;                   /* PC (2026-11-06): a C = Ad draw from the doubled index stream (sub_vu1) */
    float pad = 0.0f;                       /* PC (2026-11-06): its record's pad (4 x the mesh's vertices: vs_vu1's rep) */
    D3D11_RECT passBox = {};                /* PC (2026-11-07): the union of its parts' areas so far (the lazy mirror's
                                             * note of a run's state, RunState::passArea: its one pass covers them all;
                                             * the frame capture's area of the draw, cap::draw_area) */
};
thread_local MergedDraw g_merged;       /* PC (2026-11-05): the submit stage's, per thread (the submit workers) */
bool g_mergeOn = true;

/* What vu1_draw hands draw_passes for a merged draw's start (null: none) */
struct Vu1Cur {
    Vs vs;
    int cull;
    ID3D11Buffer *vb, *ib;
    uint32_t batch;                         /* its batch in the mesh (~0: streamed vertices) */
    const Vu1Inst *inst;
};
thread_local const Vu1Cur *g_vu1Cur = nullptr;

struct Stats {
    uint32_t cmds[32];
    uint32_t draws, verts, skipped, prims;
    uint32_t skipNoPass, skipTarget, skipScissor, skipTex, skipDepthFrame;
    uint32_t skipRefl;                      /* PC (2026-11-05): env passes left out ([Graphics] CarReflections) */
    uint32_t mirrorPasses, mirrorSyncs;     /* PC (2026-11-04): the alpha mirror's passes and syncs */
    uint32_t runsHit, runsMiss;             /* PC (2026-11-05): the resolved runs replayed and resolved */
    uint32_t runTexFree, runTexWalk;        /* PC (2026-11-05): replayed parts' textures from their records, resolved */
    uint32_t flareBatches;                  /* PC (2026-11-05): the flare queries' batches drawn */
    uint32_t mirrorFolds;                   /* PC (2026-11-05): the mirror passes folded into their draws */
    /* PC (2026-11-05): why the mirror passes were needed (mirror_cause): the
     * texture's alpha alone varies, the vertex alpha alone, both, neither */
    uint32_t mirrorTex, mirrorVtx, mirrorBoth, mirrorOther;
};
/* PC (2026-11-05): a stage's own (the executor's, the submit thread's:
 * t_stats), summed at the frame's end (exec_end_frame) */
Stats g_statsA, g_statsB;
thread_local Stats *t_stats = &g_statsA;
int g_statsEvery = -1;

/* ======================================================================== */
/* The submit stage's records (PC, 2026-11-05)                              */
/* ======================================================================== */

/* The executor (stage A: exec_list - the GS registers, the registry, the
 * targets, the models, the draw states, the resolved runs) and the submit
 * stage (B: a draw's record, constants, lighting block, its merged draw, its
 * passes, the binding cache and the D3D thread's closures - what vu1_submit
 * and draw_passes did on the executor until 2026-11-05) run on two threads
 * (pgl_queue.h sq; docs/renderer.md 3, "The submit thread"): the executor
 * queues the stage's records - a draw state when it changes (post_state),
 * then a record per draw (SubVu1, SubGs) - and goes on with the next
 * commands.  The stage never reads the executor's structures: a state comes
 * with what it took from its targets by value (SubState), a draw with what
 * the executor decided for it (its area and clipping, its copy of the
 * target, whether the target keeps its alpha mirror), and the bookkeeping
 * of what a draw writes (the targets' written parts and generations, the
 * registry, the DATE run) is the executor's, done before the record goes
 * (draw_bookkeep) - so the next command's decisions (a copy of the target
 * for a texture of it, for C = Ad, for the mirror's sync) see it, and the
 * copies, syncs and passes the executor queues (through the stage's ring:
 * q::t_forward) come before the draws that follow them.  The frame's draws
 * are the same, in the same order; the benchmark's pictures stay the exact
 * ones with the thread on and off (GT3PC_PGL_SUBMIT_THREAD=0). */

/* A draw state as the submit stage has it: compute_state's result and what
 * it took from its targets (the views bound, the native size: the executor
 * goes on changing the targets - a resample re-makes them) */
struct SubState {
    BatchState bs;                          /* (rt and zt null here: never read by the stage) */
    uint32_t serial = 0;                    /* g_stateSerial */
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;  /* (draw_dsv: the depth buffer's, the alpha mirror's, or none) */
    /* PC (2026-11-05): the depth buffer's view with the depth read-only and
     * its texture, when dsv is the depth buffer's own (Target::dsvRead): a
     * draw with its mirror pass folded in binds these (draw_passes) */
    ID3D11DepthStencilView *dsvRead = nullptr;
    ID3D11ShaderResourceView *zsrv = nullptr;
    /* PC (2026-11-05): the colour target drawn into, for its alpha bound
     * alone (Target::alphaLo: the stage's own fields; nothing else of it is
     * read here) */
    Target *rtAlpha = nullptr;
    int nw = 0, nh = 0;
    int samples = 1;                        /* PC (2026-11-15): the colour target's sample count (AGSS: draw_passes) */
    bool hasZ = false;
};
thread_local SubState g_sub;                /* the stage's current state (PC, 2026-11-05: per thread - the workers) */

/* A VU1 draw's record: its program state and lighting block in the frame's
 * list (stable while the frame executes), its buffers, the constants' head
 * (Vu1Head: exec_model's part of CbVu1), the batch's alpha range and
 * normals, and the executor's decisions */
struct Vu1Head {
    int32_t program, specExp, format;
    float morph, flareO, flareGpu, flareX, flareY;
    float hideC[4], hideH[4];               /* PC (2026-11-07): the hide box (CbVu1's; the part's hideC / hideOn / hideH) */
};
/* PC (2026-11-07): the hide box of a part into a head. */
inline void head_hide(Vu1Head &h, const PglrModelPart &p)
{
    h.hideC[0] = p.hideC[0];
    h.hideC[1] = p.hideC[1];
    h.hideC[2] = p.hideC[2];
    h.hideC[3] = p.hideOn;
    h.hideH[0] = p.hideH[0];
    h.hideH[1] = p.hideH[1];
    h.hideH[2] = p.hideH[2];
    h.hideH[3] = 0.0f;
}
struct SubVu1 {
    const PglrVu1 *vu;
    const float (*L)[4];
    ID3D11Buffer *vb, *ib, *bvb;
    ID3D11Buffer *ib2;                      /* PC (2026-11-06): the model's doubled index stream (or null) */
    uint32_t nVerts;                        /* PC (2026-11-06): the vertices of one copy in vb (0: not doubled) */
    uint32_t indexCount, firstIndex;
    int32_t baseVertex;
    uint32_t batch;                         /* (~0: streamed vertices) */
    Vu1Head head;
    int overlay;
    int aMin, aMax;
    float n2;
    D3D11_RECT area;                        /* (hasArea) the native pixels it can touch */
    bool hasArea, inside;                   /* inside: the clipper's planes (drawn without it) */
    bool mirrorRange;                       /* its alpha range wanted (the target kept its mirror before this draw) */
    bool mirror;                            /* the target keeps its alpha mirror through this draw */
    bool noPass;                            /* PC (2026-11-06): no mirror pass after it (the mirror let go: g_mirrorLazyOn) */
    ID3D11ShaderResourceView *dateSrv;      /* the copy of the target its C = Ad / DATE reads (or none) */
};
/* A GS batch's record: its vertices streamed (vb, first .. first + count),
 * their alpha range, and the executor's decisions as above */
struct SubGs {
    ID3D11Buffer *vb;
    uint32_t first, count;
    D3D11_RECT area;
    int vLo, vHi;
    bool mirror;
    bool noPass;                            /* PC (2026-11-06): (as SubVu1's) */
    bool fullCover;                         /* PC (2026-11-05): its sprites cover the whole target (batch_covers_target) */
    ID3D11ShaderResourceView *dateSrv;
    /* PC (2026-11-06): the batch done as clears (fast_clear: FAST_* bits; 0:
     * drawn): the colour (as the pixel shader would write it), the depth, the
     * alpha mirror's stencil value */
    uint8_t fast;
    uint8_t fastStencil;
    float fastZ;
    float fastColor[4];
    /* PC (2026-11-06): the frame capture's note of the mirror at a covering
     * batch (bits: 1 the target dated, 2 the mirror valid before the
     * prologue, 4 valid after it, 8 made valid again here, 16 a flat alpha) */
    uint8_t mirrorNote;
    /* PC (2026-11-07): the frame capture's note of a covering batch drawn,
     * not done as clears: why (flush_batch; null: not covering, or cleared) */
    const char *drawnWhy;
};
enum : uint8_t { FAST_COLOR = 1, FAST_DEPTH = 2, FAST_STENCIL = 4, FAST_SKIP = 8 };

/* PC (2026-11-05, ROADMAP 15 "The parallel executor", step 1): a replayed
 * run's draws as ONE record for the submit stage (RunWork) instead of a
 * state record at every texture change (~750 bytes copied into the ring
 * each: 3,500 a 32-car frame) and a draw record per part.  The executor
 * walks the run as before (the registers, the uploads, the textures'
 * identities, the areas, the bookkeeping: everything the commands after the
 * run depend on, in frame order) and notes per draw what it decided
 * (WorkStep: its texture's record, its area, its mirror flags) and per
 * state what the stage takes from the targets (WorkState: post_state's
 * views); the stage builds the state and draw records itself (run_submit)
 * from these and the run (RunRec: its states, parts, models and textures -
 * stable while the frame executes: a run replaced is kept until the frame
 * ends, g_runGrave; its texture records are not rewritten while a work of
 * it is pending, RunRec::pending).  The same draws with the same values in
 * the same order: exact.  Not so (the per-draw records as before): a frame
 * that does not pipeline, GPU sections timed (their marks are per draw), a
 * run with a C = Ad shader form (its copy per draw must come between its
 * draws), a depth frame, or a draw of the target it draws into
 * (RunRec::workOk).  GT3PC_PGL_RUN_WORK=0: off (exec_init).
 *
 * PC (2026-11-06, step 3 - the executor's own walk on the stage): the
 * executor no longer walks a run's draws at all.  Per step it does what the
 * commands after the run depend on and nothing else: the register writes,
 * the texture's identity (run_replay_tex; a target texture's view as of this
 * frame), the CLUT loads, and per state the stage's views (WorkState) with
 * the state's verdicts - the mirror flags, the reflection limit, the texture
 * in effect - and the state's books (draw_bookkeep: the scissor stands for
 * the draws' areas, a superset).  The stage walks the run's steps itself
 * (run_submit_steps: the matrix group's box test, the draw's area and
 * clipping, the reflection skip, the draw record) from the run and these
 * notes: a step whose texture was resolved after all is noted (WorkNote: the
 * fresh record, or the skip the resolve left), a target texture's view per
 * work (WorkView).  A state whose first draw syncs the alpha mirror (the
 * frame's first DATE draw) stays the executor's (WorkState::walk false: the
 * sync's place is the first draw not skipped, which only the walk knows), as
 * does a traced frame.  The same decisions from the same inputs - the
 * state's copy, the command's matrices, the run's boxes - in the same order:
 * exact.  GT3PC_PGL_RUN_WALK=0: the executor walks the draws as before, the
 * stage takes its notes (exec_init). */
struct RunTex;
struct RunRec;
struct WorkState {
    uint32_t serial;                        /* g_stateSerial */
    uint16_t state;                         /* RunRec::states */
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv, *dsvRead;
    ID3D11ShaderResourceView *zsrv;
    Target *rtAlpha;
    int nw, nh;
    int samples;                            /* PC (2026-11-15): the colour target's (AGSS: draw_passes) */
    bool hasZ;
    /* PC (step 3): the state's verdicts for the stage's walk - whether the
     * stage walks its steps (else the executor's notes, WorkStep), the alpha
     * mirror flags of its draws (mirror_on; constant over the state: no sync
     * inside a walked one), the reflection skip's limit (0: none), and the
     * texture in effect as the state begins (the record or a fresh resolve;
     * null: none) with its view, and whether its draws are skipped (a
     * resolve that found nothing to sample) */
    const RunTex *tex;
    ID3D11ShaderResourceView *srv;
    int reflLim;
    bool walk, skip, mirror, mirrorRange;
    bool noPass;                            /* PC (2026-11-06): its draws write no mirror pass (the mirror let go) */
};
enum : uint8_t { WF_HASAREA = 1, WF_INSIDE = 2, WF_MIRROR = 4, WF_MIRROR_RANGE = 8, WF_NOPASS = 16 };
struct WorkStep {
    const RunTex *tex;                      /* its texture (the run's record, or one resolved at this replay); null untextured */
    ID3D11ShaderResourceView *srv;          /* a target texture's view as of this frame (null: the record's) */
    uint32_t indexCount, firstIndex;
    uint32_t step;                          /* PC (step 3): the step (RunRec::steps) */
    uint16_t part;                          /* RunRec::parts */
    uint16_t state;                         /* RunWork::states */
    uint16_t batch;
    uint8_t aMin, aMax, flags;              /* WF_* */
    float n2;
    D3D11_RECT area;
};
/* PC (step 3): a step whose texture the executor resolved after all (its
 * record failed, or it has none): the fresh record (null: untextured), and
 * whether the resolve left the draws skipped - in effect until the next
 * texture event */
struct WorkNote {
    uint32_t step;
    const RunTex *tex;
    ID3D11ShaderResourceView *srv;
    bool skip;
};
/* PC (step 3): a target texture's view as of this frame, for the work's
 * steps whose record is of that target (run_tex_srv, once per work) */
struct WorkView {
    Target *t;
    ID3D11ShaderResourceView *srv;
};
/* PC (2026-11-05, step 2 - the submit workers): a run's alpha bound writes
 * (target_alpha_written), gathered on the worker and done by the submit
 * thread as it publishes the run: in the runs' order, where the serial
 * submission did them (the unions commute among themselves) */
struct AlphaDelta {
    Target *t;
    int lo, hi;
    bool full;
};
struct RunWork {
    RunRec *r;
    const CmdList *list;
    size_t first;                           /* the run's first command in the list */
    WorkState *states;
    WorkStep *steps;
    uint32_t nStates, nSteps;
    /* PC (step 3): the run's steps this work covers [stepFirst, stepEnd)
     * (RunRec::steps; a cut ends one and begins the next at the step at
     * hand), whether it begins mid-state (states[0] is the state in effect),
     * the executor's notes of the textures resolved after all, the target
     * textures' views */
    uint32_t stepFirst, stepEnd;
    bool cut;
    WorkNote *notes;
    uint32_t nNotes;
    WorkView views[4];
    uint32_t nViews;
    /* PC (step 2): built by a submit worker - its captured calls, its alpha
     * bound writes, the targets whose alpha it writes (their alphaPos after
     * its record), the submit ring's position it must see run before it
     * reads a bound (needB: Target::alphaPos), done when built */
    q::Capture cap;
    std::vector<AlphaDelta> alpha;
    std::vector<Target *> topTargets;
    uint64_t needB = 0;
    std::atomic<uint32_t> done{0};
    int32_t capRun = -1;                    /* PC (2026-11-06): the run's number in the frame capture (-1 none) */
};
void run_publish(RunWork &w);               /* (the submit stage) a worker's run spliced in */
void work_push(RunWork *w);                 /* (the executor) a run to the workers */
bool workers_on();
/* PC (2026-11-05): the run's work so far posted and a new one begun for
 * the rest - before anything the executor queues mid-run that the GPU must
 * run AFTER the run's earlier draws (the alpha mirror's sync from a copy of
 * the target: the frame's first DATE draw inside a car run - the sync came
 * before the whole run's draws, so the parts drawn before it had no alpha
 * in the mirror and their env passes failed the DATE test: user report, the
 * reflection cut off along the parts; the benchmark with
 * GT3PC_PGL_MIRROR_RESYNC=1 showed it) */
void run_work_cut();
void run_work_cut_if(const Target *t);      /* (if the run wrote t: its draws before a read of it) */
void run_work_post(RunWork *w);             /* (the executor) a work to the stage (nothing without draws) */
/* A frame's records (reset as the frame begins: the frame before has
 * drained), in chunks that stay where they are while the stage reads them */
template <class T> struct WorkPool {
    std::vector<std::unique_ptr<T[]>> chunks;
    std::vector<size_t> sizes;
    size_t cur = 0, used = 0;
    static constexpr size_t kChunk = 4096;
    void reset()
    {
        cur = 0;
        used = 0;
    }
    T *alloc(size_t n)
    {
        while (cur < chunks.size() && used + n > sizes[cur]) {
            cur++;
            used = 0;
        }
        if (cur == chunks.size()) {
            const size_t sz = std::max(kChunk, n);
            chunks.emplace_back(new T[sz]);
            sizes.push_back(sz);
            used = 0;
        }
        T *p = chunks[cur].get() + used;
        used += n;
        return p;
    }
};
WorkPool<RunWork> g_workRuns;
WorkPool<WorkState> g_workStates;
WorkPool<WorkStep> g_workSteps;
WorkPool<WorkNote> g_workNotes;             /* PC (step 3) */
extern WorkPool<RunTex> g_workTexs;         /* (defined with RunTex, below) */
RunWork *g_work = nullptr;                  /* (the executor) the run being walked for the stage, or null */
bool g_runWorkOn = true;
bool g_runWalkOn = true;                    /* PC (step 3): the stage walks the runs' steps (GT3PC_PGL_RUN_WALK=0: the executor) */
bool g_runWalk = false;                     /* PC (step 3): (the executor) this replay's states are the stage's to walk */
uint32_t g_runStepCur = 0;                  /* PC (step 3): (the executor) the step at hand (RunRec::steps) */
void work_state_fill(WorkState &ws, uint16_t state);    /* (the executor) a state's views and verdicts for the stage */
void run_submit(const RunWork &w);          /* (the submit stage) the run's records built and submitted */

void queue_mirror_pass(const MirrorPlan &plan, bool zw, bool flat, int kind, uint32_t count, uint32_t first,
                       int32_t base, uint32_t inst, bool perSample = false);

/* PC (2026-11-03): an indexed (VU1) draw - its record copied to the GPU if
 * not yet (vu1_inst_upload), its first instance the record */
/* (PC, 2026-11-05: instances - 2 for a C = Ad draw of two, pgl_d3d.h
 * PSF_AD_INST: its record in two consecutive places) */
/* (PC, 2026-11-06: capId - the frame capture's record of the draw, ~0u none:
 * its GPU queries either side) */
inline void draw_indexed(Rings *rg, uint32_t count, uint32_t first, int32_t base, uint32_t inst, uint32_t lights,
                         uint32_t instances, uint32_t capId)
{
    vu1_inst_upload(rg, inst + instances - 1);  /* (PC, 2026-11-05: rg - the submitting thread's rings) */
    light_upload(rg, lights);               /* PC: (its lighting block, light_block) */
    cap::gpu_begin(capId);
    qdc()->DrawIndexedInstanced(count, instances, first, base, vu1_inst_slot(inst));
    cap::gpu_end(capId);
}

/* PC (2026-11-06): the frame capture's clock (pgl_capture.h: the executor's
 * commands and runs, the stage's draws) */
inline int64_t cap_now()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
inline uint32_t cap_ns(int64_t t0)
{
    static int64_t f = 0;
    if (f == 0) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
    }
    const double ns = (double)(cap_now() - t0) * 1e9 / (double)f;
    return ns < 0 ? 0 : ns > 4e9 ? 0 : (uint32_t)ns;
}
int32_t g_capLastRun = -1;                  /* (the executor) the run replayed last, its capture number */

void merged_queue()
{
    const MergedDraw &m = g_merged;
    const uint32_t lights = light_mark();
    const uint32_t instances = m.instances;
    Rings *const rg = thread_rings();
    const uint32_t capId = m.capId;
    cap::draw_parts(capId, (uint16_t)std::max(1u, m.capParts), (uint8_t)m.nSlots);  /* PC (2026-11-06) */
    if (m.capParts > 1)                     /* PC (2026-11-07): (its area: all its parts', not the first's) */
        cap::draw_area(capId, m.passBox.left, m.passBox.top, m.passBox.right, m.passBox.bottom);
    if (m.nParts <= 1) {
        const uint32_t count = m.count, first = m.first, inst = m.inst;
        const int32_t base = m.base;
        q::run([rg, count, first, base, inst, lights, instances, capId] {
            draw_indexed(rg, count, first, base, inst, lights, instances, capId);
        });
        if (m.plan.pass && !m.folded)       /* PC: (the alpha mirror; 2026-11-05: unless folded into the draw) */
            queue_mirror_pass(m.plan, m.wroteZ, m.flat != 0.0f, 1, count, first, base, inst, ps_per_sample(m.shader));
        return;
    }
    Vu1Inst rec = {};
    rec.texW = rec.texH = 1.0f;
    rec.flat = m.flat;
    rec.parts = (float)m.nParts;
    memcpy(rec.zFromW, m.zFromW, sizeof(rec.zFromW));
    rec.texRect[1] = (float)m.firstBatch;   /* (vs_vu1: a vertex's part, by its batch) */
    rec.lightBase = -1.0f;                  /* (the parts' records have theirs) */
    rec.pad = m.pad;                        /* PC (2026-11-06): (4 x the mesh's vertices: the doubled stream's rep) */
    const uint32_t seq = vu1_inst_parts(m.parts, m.nParts, rec, instances);
    draw_merged(m.srv, m.samp, m.nSlots, m.shader, m.count, m.first, m.base, seq, lights, instances, capId);
    if (m.plan.pass && !m.folded)           /* PC: (the alpha mirror: the parts' records as they are) */
        queue_mirror_pass(m.plan, m.wroteZ, m.flat != 0.0f, 2, m.count, m.first, m.base, seq, ps_per_sample(m.shader));
}

/* A part's record in a merged draw, at its batch's place (the batches
 * between, drawn by no part, the last record again): slot k, its texture
 * flags, 2^TW x 2^TH, the texture rectangle, its lighting (Vu1Inst's). */
void merged_part(MergedDraw &m, uint32_t slot, uint32_t batch, uint32_t texFlags, const Vu1Inst &inst)
{
    const uint32_t k = batch - m.firstBatch;
    while (m.nParts < k) {
        m.parts[m.nParts] = m.parts[m.nParts - 1];
        m.nParts++;
    }
    Vu1Part &p = m.parts[m.nParts++];
    p.firstVertex = batch;
    p.slot = slot;
    p.texFlags = texFlags;
    p.pad0 = 0;
    p.texW = inst.texW;
    p.texH = inst.texH;
    p.pad1 = p.pad2 = 0.0f;
    memcpy(p.texRect, inst.texRect, sizeof(p.texRect));
    p.lightBase = inst.lightBase;
    p.program = inst.program;
    p.specExp = inst.specExp;
    p.pad3 = 0.0f;
    m.lastBatch = batch;
}

/* ---- The alpha mirror (g_dateStencil) ---------------------------------- */

void queue_pass_state(ID3D11RenderTargetView *rtv, const D3D11_VIEWPORT &vp, ID3D11RasterizerState *rs,
                      ID3D11BlendState *bs, const float *factor, ID3D11DepthStencilState *ds,
                      ID3D11DepthStencilView *dsv, uint8_t stencilRef);
void queue_pass_draw(ID3D11ShaderResourceView *srv0, ID3D11ShaderResourceView *srv1, ID3D11SamplerState *s);
extern bool g_tracing;                  /* (the F9 trace: below) */
extern uint32_t g_traceN;

/* The range of the alpha the vertex stage gives a draw's fragments (0 ..
 * 255, whole numbers): a GS batch's or an unlit VU1 draw's vertex alphas (lo
 * .. hi as given), else the lighting program's result over the batch's
 * vertex alphas aMin .. aMax and normals of length up to sqrt(n2) (vs_vu1,
 * the alpha channel of each: the bounds of every term).  Never narrower
 * than the truth. */
void light_alpha_range(int program, int specExp, const float (*L)[4], int aMin, int aMax, float n2, int &lo,
                       int &hi)
{
    auto finish = [&](double a, double b) {
        if (a > b)
            std::swap(a, b);
        lo = (int)std::floor(std::max(0.0, std::min(255.0, a)));
        hi = (int)std::floor(std::max(0.0, std::min(255.0, b)));
    };
    const double nMax = std::sqrt((double)std::max(0.0f, n2));
    auto dmax = [&](int row, int i) {   /* the most of max(L[row..row+2] . N, 0)'s component i */
        const double x = L[row][i], y = L[row + 1][i], z = L[row + 2][i];
        return nMax * std::sqrt(x * x + y * y + z * z);
    };
    auto pow_e = [&](double x) {
        const double x4 = x * x * x * x;
        if (specExp <= 4)
            return x4;
        const double x8 = x4 * x4;
        return specExp <= 8 ? x8 : x8 * x8;
    };
    switch (program) {
    case VU1P_DEFAULT:
        lo = aMin;
        hi = aMax;
        return;
    case VU1P_LOCAL:
        lo = hi = 128;
        return;
    case VU1P_DIFFUSE:
    case VU1P_SPEC2: {
        double a = L[3][3], b = L[3][3];
        for (int i = 0; i < 4; i++) {
            double d = dmax(0, i);
            if (program == VU1P_SPEC2 && i >= 2)
                d = pow_e(d);
            const double t = (double)L[4 + i][3] * d;
            a += std::min(0.0, t);
            b += std::max(0.0, t);
        }
        const double top = L[8][3];
        finish(std::max(0.0, std::min(a, top)), std::max(0.0, std::min(b, top)));
        return;
    }
    case VU1P_SPEC4: {
        double a = L[3][3], b = L[3][3];
        for (int i = 0; i < 4; i++) {
            const double t = (double)L[4 + i][3] * dmax(0, i), u = (double)L[11 + i][3] * pow_e(dmax(8, i));
            a += std::min(0.0, t) + std::min(0.0, u);
            b += std::max(0.0, t) + std::max(0.0, u);
        }
        const double top = L[15][3];
        finish(std::max(0.0, std::min(a, top)), std::max(0.0, std::min(b, top)));
        return;
    }
    case VU1P_ENVMTX:
    case VU1P_ENVMAP: {
        /* c.a (L[4] + L[5] min(V . N / |V|, 0)).w, the min in -|N| .. 0 */
        const double f0 = L[4][3], f1 = (double)L[4][3] - (double)L[5][3] * nMax;
        const double c[4] = {aMin * f0, aMin * f1, aMax * f0, aMax * f1};
        double a = c[0], b = c[0];
        for (int k = 1; k < 4; k++) {
            a = std::min(a, c[k]);
            b = std::max(b, c[k]);
        }
        finish(std::trunc(a), std::trunc(b));
        return;
    }
    case VU1P_CONST: {
        const double top = L[1][3];
        const double p = std::max(0.0, std::min((double)aMin * L[0][3] + L[2][3], top));
        const double q = std::max(0.0, std::min((double)aMax * L[0][3] + L[2][3], top));
        finish(p, q);
        return;
    }
    default:                                /* (the flare program: its texel) */
        lo = 0;
        hi = 255;
        return;
    }
}

/* The range of the alpha the vertex stage gives the draw at hand (vu1_draw,
 * flush_batch: before draw_passes / merge_join) */
thread_local int g_vLo = 0, g_vHi = 255;
/* PC (2026-11-06): the stage's note of the state at hand (RunState::passArea)
 * while it walks a run's steps (run_submit_steps), else null */
thread_local uint32_t *t_passState = nullptr;

/* PC (2026-11-05): the draws into b's target keep its alpha mirror: a target
 * a DATE draw has read, whose mirror is valid now (draw_passes: before the
 * frame's first DATE draw there is nothing to keep, its sync takes the
 * alpha as it is) */
inline bool mirror_on(const BatchState &b)
{
    return g_dateStencil && !b.depthFrame && b.rt != nullptr && b.rt->dated && b.rt->mirrorValid;
}

/* light_alpha_range through a memo (the parts of a car share a few blocks
 * and vertex ranges: the square roots once each) */
void light_alpha_range_memo(int32_t lightBase, int program, int specExp, const float (*L)[4], int aMin, int aMax,
                            float n2, int &lo, int &hi)
{
    if (program == VU1P_DEFAULT) {
        lo = aMin;
        hi = aMax;
        return;
    }
    /* (the entry by the block's place in the lights' ring, checked against
     * the block itself: a place holds other blocks over time) */
    struct Memo {
        int32_t lightBase;
        int program, specExp, aMin, aMax;
        uint32_t n2;
        int lo, hi;
        float L[16][4];
    };
    static thread_local Memo memo[256];     /* PC (2026-11-05): (per thread: the submit workers) */
    uint32_t n2bits;
    memcpy(&n2bits, &n2, 4);
    const uint32_t h = ((uint32_t)lightBase * 2654435761u) ^ ((uint32_t)program << 7) ^ ((uint32_t)aMin << 11) ^
                       ((uint32_t)aMax << 3) ^ (n2bits >> 7) ^ ((uint32_t)specExp << 20);
    Memo &m = memo[h & 255u];
    if (lightBase >= 0 && m.lightBase == lightBase && m.program == program && m.specExp == specExp &&
        m.aMin == aMin && m.aMax == aMax && m.n2 == n2bits && memcmp(m.L, L, sizeof(m.L)) == 0) {
        lo = m.lo;
        hi = m.hi;
        return;
    }
    light_alpha_range(program, specExp, L, aMin, aMax, n2, lo, hi);
    if (lightBase >= 0) {
        m.lightBase = lightBase;
        m.program = program;
        m.specExp = specExp;
        m.aMin = aMin;
        m.aMax = aMax;
        m.n2 = n2bits;
        m.lo = lo;
        m.hi = hi;
        memcpy(m.L, L, sizeof(m.L));
    }
}

/* What a pass of the current state does to the target's alpha mirror
 * (MirrorPlan; mask: the pass's channels, zw: it writes depth): the alpha
 * its fragments write, from the vertex alpha vLo .. vHi, the texture's
 * (TexBinding aMin / aMax, TEXA for a target's), TFX and TCC, the pass's
 * alpha test (ps_body's maths, over the ranges' ends) and FBA - one value of
 * bit 7, or both (a mirror pass).  A DATE draw keeps the stencil test. */
/* The range of the alpha a pass of state b writes (GS units, before FBA):
 * from the vertex alpha vLo .. vHi, the texture's (TexBinding aMin / aMax,
 * TEXA for a target's), TFX and TCC, the pass's alpha test (ps_body's maths,
 * over the ranges' ends).  False: nothing passes.  Never narrower than the
 * truth. */
bool alpha_write_range(const BatchState &b, const gs::Pass &pass, int vLo, int vHi, int &lo, int &hi,
                       int *tLoOut = nullptr, int *tHiOut = nullptr)
{
    lo = vLo;
    hi = vHi;
    const uint32_t fl = b.ps.flags;
    if (tLoOut != nullptr)
        *tLoOut = *tHiOut = -1;             /* PC: (the texture's range, for mirror_cause; -1: none) */
    if (b.tme && (fl & PSF_TCC) != 0) {
        int tLo = 0, tHi = 255;
        const int am = (int)((fl >> PSF_AMODE_SHIFT) & 3);
        /* PC (2026-11-05): a target's alpha bound (Target::alphaLo): what its
         * texels' alpha can be - through TEXA for a CT16 target (TA0 for bit
         * 7 clear, TA1 for set: one of them when the bound has one bit) */
        const Target *tt = (g_alphaBoundOn && b.tex.target != nullptr && !b.tex.depthTex &&
                            b.tex.target->alphaLo >= 0) ? b.tex.target : nullptr;
        if (am == 1) {
            tLo = tHi = (int)b.ps.ta0;
        } else if (am == 2) {
            tLo = std::min((int)b.ps.ta0, (int)b.ps.ta1);
            tHi = std::max((int)b.ps.ta0, (int)b.ps.ta1);
            if (tt != nullptr && (tt->alphaLo >= 128) == (tt->alphaHi >= 128))
                tLo = tHi = tt->alphaHi >= 128 ? (int)b.ps.ta1 : (int)b.ps.ta0;
        } else if (b.tex.hasAlpha) {
            tLo = b.tex.aMin;
            tHi = b.tex.aMax;
        } else if (tt != nullptr) {
            tLo = tt->alphaLo;
            tHi = tt->alphaHi;
        }
        if (tLoOut != nullptr) {
            *tLoOut = tLo;
            *tHiOut = tHi;
        }
        const int tfx = (int)((fl >> PSF_TFX_SHIFT) & 3);
        if (tfx == 0) {
            lo = (tLo * lo) / 128;
            hi = (tHi * hi) / 128;
        } else if (tfx == 2) {
            lo = tLo + lo;
            hi = tHi + hi;
        } else {
            lo = tLo;
            hi = tHi;
        }
        lo = std::min(lo, 255);
        hi = std::min(hi, 255);
    }
    if (pass.gate != gs::AlphaGate::None) {
        const int ref = b.ds.aref;
        const bool p = pass.gate == gs::AlphaGate::Pass;
        switch (b.ds.alphaFunc) {
        case gs::Compare::Never: if (p) hi = -1; break;
        case gs::Compare::Always: if (!p) hi = -1; break;
        case gs::Compare::Less: if (p) hi = std::min(hi, ref - 1); else lo = std::max(lo, ref); break;
        case gs::Compare::LessEqual: if (p) hi = std::min(hi, ref); else lo = std::max(lo, ref + 1); break;
        case gs::Compare::Equal: if (p) { lo = std::max(lo, ref); hi = std::min(hi, ref); } break;
        case gs::Compare::GreaterEqual: if (p) lo = std::max(lo, ref); else hi = std::min(hi, ref - 1); break;
        case gs::Compare::Greater: if (p) lo = std::max(lo, ref + 1); else hi = std::min(hi, ref); break;
        case gs::Compare::NotEqual: if (!p) { lo = std::max(lo, ref); hi = std::min(hi, ref); } break;
        }
    }
    return lo <= hi;
}

/* PC (2026-11-05): debug aid GT3PC_PGL_ALPHA_LOG=1 - the draws that may
 * write a GS alpha above 0x80 into a colour target (the vertex and texture
 * ranges: never narrower than the truth, so a count of 0 proves there are
 * none), counted (the profile's ALPHA>80) and the first logged.  Such alphas
 * are C = Ad weights above 1.0 when read back (the GS's Ad / 128, up to
 * 2.0): the user's SSR5 run counted 846 a frame - the course's env view
 * (T276: its textures' alphas reach 0xFF), read back as weights by its
 * glint passes - which is why a target's alpha cannot be stored doubled for
 * a one-pass C = Ad (pgl_d3d.h PSF_AD_INST). */
bool g_alphaLog = false;

void alpha_log_check(const SubState &s, const gs::Pass &pass, uint8_t mask, bool indexed)
{
    const BatchState &b = s.bs;
    if ((mask & gs::WRITE_A) == 0 || (b.ds.fbmsk & 0x80000000u) != 0)
        return;
    int lo, hi;
    if (!alpha_write_range(b, pass, g_vLo, g_vHi, lo, hi) || hi <= 128)
        return;
    g_profCount[PCOUNT_ALPHA_HI]++;
    static int logged = 0;
    if (logged < 16) {
        logged++;
        pclog::write("pgl alpha: a %s draw may write alphas %d..%d (above 0x80) into a %dx%d target (FRAME psm %02X,"
                     " PRIM %03X, ALPHA %02X, %s%s, vertex alpha %d..%d)%s", indexed ? "VU1" : "GS", lo, hi, s.nw, s.nh,
                     b.ds.fpsm, gs::encodePrim(b.ds.prim), b.ds.blend.selectors, b.tme ? "textured" : "untextured",
                     b.tme && (b.ps.flags & PSF_TCC) ? " (TCC: the texture's alpha)" : "", g_vLo, g_vHi,
                     logged == 16 ? " (no more of these lines)" : "");
    }
}

/* PC (2026-11-05): why a mirror pass is needed - which of the alpha's inputs
 * varies within the draw (the vertex alpha: vLo .. vHi, the lighting's bound
 * for a VU1 draw; the texture's: tLo .. tHi, -1 untextured) - counted in the
 * stats line (mirrorTex / Vtx / Both / Other) and, with
 * GT3PC_PGL_MIRROR_LOG=1, the first 40 logged: what a narrower bound would
 * have to know to spare the pass */
uint8_t mirror_cause(const SubState &s, int vLo, int vHi, int tLo, int tHi, int lo, int hi)
{
    const BatchState &b = s.bs;
    const bool tv = tLo >= 0 && tLo != tHi, vv = vLo != vHi;
    const uint8_t cause = (tv && vv) ? 3 : tv ? 1 : vv ? 2 : 4;
    if (!g_mirrorLog)
        return cause;
    static int logged = 0;
    if (logged >= 40)
        return cause;
    logged++;
    const uint32_t fl = b.ps.flags;
    char tex[200], bound[48] = "";
    if (b.tex.target != nullptr) {
        /* (PC, 2026-11-05: the target's alpha bound, Target::alphaLo) */
        if (b.tex.target->alphaLo >= 0)
            snprintf(bound, sizeof(bound), " bounded %d..%d", b.tex.target->alphaLo, b.tex.target->alphaHi);
        else
            snprintf(bound, sizeof(bound), " unbounded");
    }
    if (tLo < 0)
        snprintf(tex, sizeof(tex), "no texture alpha");
    else
        snprintf(tex, sizeof(tex), "texture alpha %d..%d (%s%s, TEXA %d / %d mode %d, TFX %d)", tLo, tHi,
                 b.tex.target != nullptr ? "a target's" : "an upload's", bound, (int)b.ps.ta0, (int)b.ps.ta1,
                 (int)((fl >> PSF_AMODE_SHIFT) & 3), (int)((fl >> PSF_TFX_SHIFT) & 3));
    pclog::write("pgl mirror: a pass for a %s draw writing alpha %d..%d: vertex alpha %d..%d, %s; %s, ALPHA %02X,"
                 " into a %dx%d target%s", b.flat ? "flat" : "Gouraud", lo, hi, vLo, vHi, tex,
                 b.dateStencil ? "DATE" : "no DATE", b.ds.blend.selectors, s.nw, s.nh,
                 logged == 40 ? " (no more of these lines)" : "");
    return cause;
}

/* (a pass drawn, or folded: its cause into the stats) */
void mirror_cause_count(const MirrorPlan &plan)
{
    switch (plan.cause) {
    case 1: t_stats->mirrorTex++; break;
    case 2: t_stats->mirrorVtx++; break;
    case 3: t_stats->mirrorBoth++; break;
    default: t_stats->mirrorOther++; break;
    }
}

/* PC (2026-11-05): the target's alpha bound (Target::alphaLo) takes a pass's
 * alpha writes: full - the pass rewrites every pixel of the target (a
 * sprite batch covering it, no test gating a pixel: batch_covers_target,
 * draw_passes), so the bound starts over from the pass's range; else the
 * range widens it (an unknown bound stays unknown).  The range is the
 * pass's as mirror_plan sees it (never narrower than the truth), FBA's bit
 * 7 applied. */
thread_local std::vector<AlphaDelta> *t_alphaDeltas = nullptr;  /* (a submit worker's run: its deltas) */

/* PC (2026-11-06, a measurement: GT3PC_PGL_BOUND_LOG=1) - whether a one-pass
 * C = Ad could stand on the targets' alpha bounds: the alpha writes above
 * 0x80 into each target (the first logged: what pushes a frame's bound past
 * 1.0), the C = Ad draws by their target's bound (known and at most 0x80, or
 * not) and whether they test DATE; the targets' bounds at each 100th frame's
 * end (targets_log_alpha_bounds) */
bool g_boundLog = false;
std::atomic<uint32_t> g_boundHiWrites{0};  /* alpha writes that may exceed 0x80 this frame */
std::atomic<uint32_t> g_boundAdDate128{0}, g_boundAdDateWide{0}, g_boundAdPlain128{0}, g_boundAdPlainWide{0};

void target_alpha_written(Target &t, const BatchState &b, const gs::Pass &pass, bool full)
{
    if (!g_alphaBoundOn)
        return;
    int lo, hi;
    int tLo = -1, tHi = -1;
    if (!alpha_write_range(b, pass, g_vLo, g_vHi, lo, hi, &tLo, &tHi))
        return;                             /* (nothing written) */
    if (b.ps.flags & PSF_FBA) {
        hi = hi < 128 ? hi + 128 : (lo >= 128 ? hi : 255);
        lo = 128;
    }
    if (g_boundLog && hi > 128) {
        g_boundHiWrites.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logged{0};
        const int n = logged.fetch_add(1, std::memory_order_relaxed);
        if (n < 24)
            pclog::write("pgl bound: an alpha write %d..%d into 0x%X %dx%d (%s; vertex alpha %d..%d, %s; ALPHA %02X,"
                         " FBMSK %08X, FBA %d, %s, %s)%s", lo, hi, t.base, t.gsW, t.gsH, full ? "covering" : "a draw",
                         g_vLo, g_vHi,
                         b.tme ? (b.tex.target != nullptr ? "a target's texture" : "a set's / an upload's texture")
                               : "untextured",
                         b.ds.blend.selectors, b.ds.fbmsk, (b.ps.flags & PSF_FBA) ? 1 : 0,
                         b.dateStencil || b.ds.date ? "DATE" : "no DATE", b.flat ? "flat" : "Gouraud",
                         n == 23 ? " (no more of these lines)" : "");
        if (n < 24 && b.tme)
            pclog::write("pgl bound:   its texture alpha %d..%d (TFX %d, TCC %d, mode %d, TEXA %d / %d)", tLo, tHi,
                         (int)((b.ps.flags >> PSF_TFX_SHIFT) & 3), (b.ps.flags & PSF_TCC) ? 1 : 0,
                         (int)((b.ps.flags >> PSF_AMODE_SHIFT) & 3), (int)b.ps.ta0, (int)b.ps.ta1);
    }
    if (t_alphaDeltas != nullptr) {
        t_alphaDeltas->push_back(AlphaDelta{&t, lo, hi, full});   /* PC: (a submit worker: done at the publish) */
        return;
    }
    if (full) {
        t.alphaLo = lo;
        t.alphaHi = hi;
    } else if (t.alphaLo >= 0) {
        t.alphaLo = std::min(t.alphaLo, lo);
        t.alphaHi = std::max(t.alphaHi, hi);
    }
}

MirrorPlan mirror_plan(const SubState &s, const gs::Pass &pass, uint8_t mask, int vLo, int vHi)
{
    const BatchState &b = s.bs;
    MirrorPlan m;
    m.test = s.hasZ && b.ds.depthTest;
    m.func = b.ds.depthFunc;
    if (b.dateStencil) {
        m.op = StencilOp::DateKeep;
        m.ref = b.ds.datm ? 1 : 0;
    }
    const bool aw = (mask & gs::WRITE_A) != 0 && (b.ds.fbmsk & 0x80000000u) == 0;
    if (!aw)
        return m;
    int lo, hi, tLo, tHi;
    if (!alpha_write_range(b, pass, vLo, vHi, lo, hi, &tLo, &tHi))
        return m;                           /* (nothing written) */
    const uint32_t fl = b.ps.flags;
    bool uniform;
    int bit;
    if (fl & PSF_FBA) {
        uniform = true;
        bit = 1;
    } else {
        const int b0 = lo >= 128 ? 1 : 0, b1 = hi >= 128 ? 1 : 0;
        uniform = b0 == b1;
        bit = b1;
    }
    /* (debug aid: GT3PC_PGL_MIRROR_ALL=1 - a mirror pass for every alpha
     * write, the ranges' verdicts not trusted: to compare) */
    static const bool all = getenv("GT3PC_PGL_MIRROR_ALL") != nullptr && atoi(getenv("GT3PC_PGL_MIRROR_ALL")) > 0;
    if (all)
        uniform = false;
    if (!uniform)
        m.cause = mirror_cause(s, vLo, vHi, tLo, tHi, lo, hi);  /* PC: (the stats' causes, GT3PC_PGL_MIRROR_LOG) */
    if (b.dateStencil) {
        const int datm = b.ds.datm ? 1 : 0;
        if (uniform) {
            m.op = bit == datm ? StencilOp::DateKeep : StencilOp::DateInvert;
        } else {
            m.op = StencilOp::DateKeep;     /* (the mirror pass flips the bit where it differs) */
            m.pass = true;
            m.keep1 = datm != 0;
        }
        m.ref = (uint8_t)datm;
    } else {
        m.op = StencilOp::Replace;
        if (uniform) {
            m.ref = (uint8_t)bit;
        } else {
            m.ref = 0;                      /* (cleared; the mirror pass sets the 1s) */
            m.pass = true;
            m.keep1 = false;
        }
    }
    return m;
}

/* PC (2026-11-06): the native pixels of a draw's area within the scissor and
 * the target (the lazy mirror's measure of a pass) */
inline uint32_t area_px(const D3D11_RECT &a, const D3D11_RECT &sc, int nw, int nh)
{
    const LONG l = std::max(std::max(a.left, sc.left), (LONG)0), t = std::max(std::max(a.top, sc.top), (LONG)0);
    const LONG r = std::min(std::min(a.right, sc.right), (LONG)nw), b = std::min(std::min(a.bottom, sc.bottom), (LONG)nh);
    return (r > l && b > t) ? (uint32_t)(r - l) * (uint32_t)(b - t) : 0u;
}
inline uint32_t mirror_drop_px(const Target &t)
{
    if (g_mirrorDropPx != 0)
        return g_mirrorDropPx;
    return (uint32_t)std::max(1, t.nw * t.nh) / kMirrorDropFrac;
}
/* PC (2026-11-06): whether a target's DATE draws are over for now: the lazy
 * mirror lets the mirror go only then - a DATE draw is one of a sequence (a
 * car's env pairs, its body parts between them), and a drop inside one costs
 * a full sync at the next member.  PC (2026-11-07): the sequence is over once
 * the game writes the alpha flat over the target (Target::dateSinceFlat,
 * draw_bookkeep / flush_batch): the clear after the sky, the one before the
 * cars.  A count of draws since the last DATE draw (48) was tried first: it
 * kept the passes of the course's largest draws, which follow the sky at
 * once (14 Mpx, frame 11929), and when made to count bounded DATE draws
 * alone it saw none of a replayed run's, whose boxes the stage's walk finds,
 * not the executor (four syncs a frame, frame 7188). */
inline bool mirror_quiet(const Target &t)
{
    return !t.dateSinceFlat;
}
/* PC (2026-11-07): the union of two rectangles (a merged draw's pass area) */
inline D3D11_RECT rect_union(const D3D11_RECT &a, const D3D11_RECT &b)
{
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
/* PC (2026-11-06): whether a depth test gates a state's pixels: one with the
 * function ALWAYS (the game's clears have it) gates nothing */
inline bool depth_gates(const BatchState &b, bool hasZ)
{
    return hasZ && b.ds.depthTest && b.ds.depthFunc != gs::Compare::Always;
}
/* PC (2026-11-06): whether a draw of state b with the vertex alphas vLo ..
 * vHi needs a mirror pass (mirror_plan's verdict for its colour passes: an
 * alpha write whose bit 7 may take both values) - the executor's half of
 * letting the mirror go (submit_prologue) */
bool mirror_needs_pass(const BatchState &b, int vLo, int vHi)
{
    if (b.depthFrame)
        return false;
    for (int p = 0; p < b.ds.passCount && p < 2; p++) {
        const gs::Pass &pass = b.ds.pass[p];
        const uint8_t mask = pass.writeMask & b.ds.frameWriteMask;
        if ((mask & gs::WRITE_A) == 0 || (b.ds.fbmsk & 0x80000000u) != 0)
            continue;
        int lo, hi;
        if (!alpha_write_range(b, pass, vLo, vHi, lo, hi))
            continue;
        if (b.ps.flags & PSF_FBA)
            continue;                       /* (bit 7 set: one value) */
        if ((lo >= 128) != (hi >= 128))
            return true;
    }
    return false;
}
/* PC (2026-11-06): what the executor knows of a draw's vertex alphas for
 * that verdict: a GS batch's range as given, a VU1 draw's through its
 * lighting program (light_alpha_range: worked out only when the draw is
 * large enough to matter) */
struct MirrorIn {
    int vLo = 0, vHi = 255;
    bool lit = false;
    int program = 0, specExp = 0;
    const float (*L)[4] = nullptr;
    int aMin = 0, aMax = 255;
    float n2 = 0.0f;
};
inline void mirror_in_range(const MirrorIn &in, int &lo, int &hi)
{
    if (!in.lit || in.L == nullptr) {
        lo = in.vLo;
        hi = in.vHi;
        return;
    }
    light_alpha_range_memo((int32_t)(((uintptr_t)in.L >> 4) & 0x7FFFFFFFu), in.program, in.specExp, in.L, in.aMin,
                           in.aMax, in.n2, lo, hi);
}

/* The mirror brought up to date from the target's alpha: the stencil cleared,
 * then a pass over it that writes 1 where the alpha has bit 7
 * (Ps::MirrorSync).  PC (2026-11-06): the pass reads the target itself when
 * single-sampled (no copy; the colour target is not bound meanwhile), a copy
 * for a multisampled one; and when the executor knows the alpha flat over
 * the target (Target::execAlpha) the stencil is cleared to its bit alone. */
void mirror_sync(Target &t, ID3D11DepthStencilView *dsv)
{
    if (!t.rtv)
        return;
    if (g_mirrorLazyOn && t.execAlpha >= 0) {
        const UINT8 bit = t.execAlpha >= 128 ? kMirrorBit : 0;
        uint32_t capId = ~0u;
        if (cap::on()) {
            cap::Draw d;
            d.kind = cap::K_CLEAR;
            d.target = t.base;
            d.targetW = (uint16_t)t.nw;
            d.targetH = (uint16_t)t.nh;
            d.note = bit != 0 ? "mirror sync: the stencil cleared to 1 (the alpha flat, bit 7 set)"
                              : "mirror sync: the stencil cleared to 0 (the alpha flat, bit 7 clear)";
            capId = cap::draw_begin(d);
        }
        q::run([dsv, bit, capId] {
            cap::gpu_begin(capId);
            qdc()->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 0.0f, bit);
            cap::gpu_end(capId);
        });
        t.mirrorValid = true;
        t_stats->mirrorSyncs++;
        g_profCount[PCOUNT_MIRROR_SYNC]++;
        g_profCount[PCOUNT_MIRROR_CLEAR]++;
        return;
    }
    const bool direct = !g_mirrorSyncCopy && t.samples <= 1 && t.srv;
    ID3D11ShaderResourceView *src = direct ? t.srv.get() : target_copy_srv(t, true, COPY_DATE);
    if (src == nullptr)
        return;
    d3d_invalidate();
    uint32_t capId = ~0u;
    if (cap::on()) {
        /* PC (2026-11-06): the frame capture's record */
        cap::Draw d;
        d.kind = cap::K_CLEAR;
        d.target = t.base;
        d.targetW = (uint16_t)t.nw;
        d.targetH = (uint16_t)t.nh;
        d.note = direct ? "mirror sync: the stencil cleared (the pass reads the target)"
                        : "mirror sync: the stencil cleared (the pass reads a copy)";
        d.count = (uint32_t)(t.execAlpha + 1);    /* (the capture: the executor's flat alpha + 1, 0 unknown) */
        d.stencilRef = t.dated ? 1 : 0;
        capId = cap::draw_begin(d);
    }
    q::run([dsv, capId] {
        cap::gpu_begin(capId);
        qdc()->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 0.0f, 0);
        cap::gpu_end(capId);
    });
    const D3D11_VIEWPORT vp = {0, 0, (float)t.nw, (float)t.nh, 0, 1};
    queue_pass_state(direct ? nullptr : t.rtv.get(), vp, raster_state(0, false), blend_plain(0), nullptr,
                     depth_state(false, gs::Compare::Always, false, StencilOp::Replace, 1), dsv, 1);
    bind_vs(Vs::Full);
    bind_ps(Ps::MirrorSync);
    queue_pass_draw(nullptr, src, nullptr);
    d3d_invalidate();
    t.mirrorValid = true;
    t_stats->mirrorSyncs++;
    g_profCount[PCOUNT_MIRROR_SYNC]++;
}

/* The mirror pass of the draw just queued (plan.pass): the same pixels
 * (kind 0: a GS batch of count vertices from first; 1: an indexed draw with
 * record inst; 2: a merged draw - its parts' records and texture slots as
 * the draw left them), the mirror pass's pixel shader (mirror_ps: where the
 * alpha's bit 7 is the one held it discards) over the pass's depth test
 * (zw: the colour pass wrote depth - equal depths pass too), no colour, the
 * stencil set (Replace 1) or flipped (a DATE draw: DateInvert) where it is
 * not.  Everything bound as before afterwards: the executor's cache is
 * right. */
void queue_mirror_pass(const MirrorPlan &plan, bool zw, bool flat, int kind, uint32_t count, uint32_t first,
                       int32_t base, uint32_t inst, bool perSample)
{
    g_profCount[PCOUNT_MIRROR_PASS]++;      /* PC: (the profile's count; GT3PC_PGL_MIRROR_PASS=0: counted, not drawn) */
    mirror_cause_count(plan);
    if (!g_mirrorPassOn)
        return;
    gs::Compare f = plan.func;
    if (zw) {
        if (f == gs::Compare::Greater)
            f = gs::Compare::GreaterEqual;
        else if (f == gs::Compare::Less)
            f = gs::Compare::LessEqual;
        else if (f == gs::Compare::NotEqual)
            f = gs::Compare::Always;
    }
    const bool date = plan.op == StencilOp::DateKeep;
    const uint8_t ref = date ? plan.ref : 1;
    ID3D11DepthStencilState *dss = depth_state(plan.test, f, false, date ? StencilOp::DateInvert : StencilOp::Replace,
                                               ref);
    /* (PC 2026-11-15: an AGSS draw's pass per sample too - it marks the
     * samples the draw wrote, as the draw's own alpha test let them) */
    ID3D11PixelShader *ps = mirror_ps(flat, kind == 2, perSample && kind != 0);
    ID3D11Buffer *cb = mirror_cb(plan.keep1);
    static ID3D11BlendState *const s_noWrite = blend_plain(0);  /* (nothing written: the stencil alone) */
    ID3D11BlendState *bs = s_noWrite;
    uint8_t prevRef = 0;
    ID3D11DepthStencilState *prevDss = bound_depth(&prevRef);
    ID3D11PixelShader *prevPs = bound_ps();
    ID3D11BlendState *prevBs = bound_blend();
    uint32_t capId = ~0u;
    if (cap::on()) {
        /* PC (2026-11-06): the frame capture's record of the pass */
        cap::Draw d;
        d.kind = cap::K_MIRROR;
        d.cause = plan.cause;
        d.stencilOp = (uint8_t)(date ? StencilOp::DateInvert : StencilOp::Replace);
        d.stencilRef = ref;
        d.zTest = plan.test;
        d.zFunc = (uint8_t)f;
        d.flat = flat;
        d.count = count;
        d.parts = (uint16_t)(kind == 2 ? std::max(1u, g_merged.nParts) : 1u);
        d.target = g_sub.rtAlpha != nullptr ? g_sub.rtAlpha->base : 0;
        d.targetW = (uint16_t)g_sub.nw;
        d.targetH = (uint16_t)g_sub.nh;
        d.note = kind == 0 ? "GS batch's" : kind == 1 ? "part's" : "merged draw's";
        capId = cap::draw_begin(d);
    }
    q::run([dss, ref, ps, cb, bs, prevDss, prevRef, prevPs, prevBs, kind, count, first, base, inst, capId] {
        qdc()->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(dss, stencil_ref_byte(ref));     /* PC (2026-11-06): (the mirror's bit) */
        qdc()->PSSetShader(ps, nullptr, 0);
        qdc()->PSSetConstantBuffers(1, 1, &cb);
        cap::gpu_begin(capId);
        if (kind == 0)
            qdc()->Draw(count, first);
        else
            qdc()->DrawIndexedInstanced(count, 1, first, base, vu1_inst_slot(inst));
        cap::gpu_end(capId);
        qdc()->PSSetShader(prevPs, nullptr, 0);
        qdc()->OMSetDepthStencilState(prevDss, stencil_ref_byte(prevRef));
        qdc()->OMSetBlendState(prevBs, nullptr, 0xFFFFFFFFu);
    });
    t_stats->mirrorPasses++;
    if (g_tracing)
        pclog::write("pgl trace %u: alpha mirror pass of the draw before (keeps %d, %s, depth test %d func %d%s)",
                     g_traceN++, plan.keep1 ? 1 : 0, date ? "DATE: the bit flipped" : "the bit set", plan.test ? 1 : 0,
                     (int)f, zw ? ", after depth writes" : "");
}

/* PC (2026-11-05): the mirror pass folded into its draw as one more instance
 * (pgl_d3d.h PSF_MIRROR, StencilOp::FoldReplace / FoldDate; docs/renderer.md
 * 3, "DATE").  Where a separate pass drew the part again with its own state
 * - six binding changes and a draw call around each of a 32-car frame's
 * ~2,600 env passes, 1.6 .. 2.2 ms of the GPU (the A/B runs of 2026-11-05)
 * - the draw's last instance is the pass: the pixel shader (FOLD_PS) repeats
 * the depth test itself from the depth buffer's texture, bound read-only as
 * the depth-stencil view (Target::dsvRead: the draw writes no depth), and
 * writes SV_Depth - the buffer's own value for the colour instances, which
 * pass the fixed test (EQUAL) and keep their stencil op, another value for
 * the mirror instance's fragments whose alpha bit is not the one held, which
 * fail it and mark the mirror by the depth-fail op (the rest discard).  The
 * same fragments, tests and marks as the draw and its pass made, in one
 * draw; exact (the benchmark's pictures).  Not for a state with depth writes
 * (its record's places are counted before the pass is planned, sub_vu1), a
 * depth frame, a multisampled depth buffer (its texture is a resolve) or a
 * target with a stencil buffer of its own; nor for the measurements
 * GT3PC_PGL_MIRROR_PASS=0 (the passes counted, not drawn) and
 * GT3PC_PGL_AD_ONCE=1 (one Ad instance where the shader expects two).
 * GT3PC_PGL_MIRROR_FOLD=0: the passes apart, as before. */
inline bool mirror_fold_ok(const SubState &s, const MirrorPlan &plan)
{
    return g_mirrorFoldOn && g_mirrorPassOn && plan.pass && !s.bs.wroteZ && !s.bs.depthFrame &&
           s.dsvRead != nullptr && s.zsrv != nullptr;
}
/* (the state's part of it, for the records' places: sub_vu1) */
inline bool mirror_fold_maybe(const SubState &s, bool mirror)
{
    return mirror && g_mirrorFoldOn && g_mirrorPassOn && !s.bs.wroteZ && !s.bs.depthFrame &&
           s.dsvRead != nullptr && s.zsrv != nullptr;
}
inline uint32_t mirror_fold_flags(const SubState &s, const MirrorPlan &plan)
{
    return PSF_MIRROR | (plan.keep1 ? PSF_MIRROR_KEEP1 : 0u) | ((uint32_t)s.bs.ds.depthFunc << PSF_ZFUNC_SHIFT) |
           ((s.hasZ && s.bs.ds.depthTest) ? PSF_ZTEST : 0u);
}

/* PC (2026-11-03): where a DATE draw's destination alpha comes from
 * (docs/renderer.md 9, "DATE without copies"; exec_init).  DATE_COPY: a
 * copy of the target at the start of its run of DATE draws (g_dateRun).
 * DATE_EACH: a copy before every DATE draw - the exact reference, the GS's
 * own semantics (GT3PC_PGL_DATE_COPY=2, a measurement: slow).  DATE_NONE:
 * whatever copy the target has (GT3PC_PGL_DATE_COPY=0, a measurement: the
 * picture wrong after alpha writes). */
enum DateMode { DATE_COPY, DATE_EACH, DATE_NONE };
DateMode g_dateMode = DATE_COPY;

/* Debug aid: GT3PC_PGL_TRACE=1 - F9 in the game window logs the draws of the
 * next frame executed (`pgl trace` lines: target, write mask, PRIM, blend,
 * tests, texture, VU1 lighting program) and the draw states it skipped, with
 * the reason. */
int g_traceOn = -1;
bool g_tracing = false;
int  g_traceFrames = 0;                 /* frames of the F9 trace still to come (two: see flare_trace_depth) */
bool g_traceNext = false;               /* the next frame is traced too */
bool g_probeFromFlares = false;         /* the probe points are flare queries' (not GT3PC_PGL_PROBE's) */
uint32_t g_traceN = 0;
int g_traceProg = -1;                   /* the VU1 lighting program of the draw (-1: GS primitives) */
/* the VU1 constants of the draw (nullptr: GS primitives) and the mean colour
 * of its vertices (lighting modes with a colour stream only) */
thread_local const CbVu1 *g_traceVu1 = nullptr;
thread_local const Vu1Inst *g_traceInst = nullptr;  /* PC: its own record (Vu1Inst: the depth from w) */
thread_local int g_traceOverlay = 0;    /* PC: its overlay layer (docs/renderer.md "Overlays") */
bool g_traceHasVcol = false;
float g_traceVcol[4] = {};

/* Debug aid: GT3PC_PGL_SKIP=<kinds> (comma separated) leaves out the draws of
 * those kinds, to find which pass makes a wrong picture: local, envmtx,
 * envmap, lights (DIFFUSE / SPEC2 / SPEC4), const - VU1 model parts by
 * lighting program; ad - draws blended with C = Ad; alpha - draws that write
 * only the alpha; self - draws that sample their own target (the in-car
 * heat-haze / rain drops, blurs). */
struct SkipSet {
    int state = -1;                     /* -1 not read, 0 off, 1 on */
    bool local = false, envmtx = false, envmap = false, lights = false, cnst = false;
    bool ad = false, alpha = false, self = false;
} g_skip;

bool skip_on()
{
    if (g_skip.state < 0) {
        g_skip.state = 0;
        const char *e = getenv("GT3PC_PGL_SKIP");
        if (e != nullptr && e[0] != 0) {
            std::string s = std::string(",") + e + ",";
            auto has = [&](const char *k) { return s.find(std::string(",") + k + ",") != std::string::npos; };
            g_skip.local = has("local");
            g_skip.envmtx = has("envmtx");
            g_skip.envmap = has("envmap");
            g_skip.lights = has("lights");
            g_skip.cnst = has("const");
            g_skip.ad = has("ad");
            g_skip.alpha = has("alpha");
            g_skip.self = has("self");
            g_skip.state = 1;
            pclog::write("pgl: GT3PC_PGL_SKIP=%s: those draws are left out (a debug aid)", e);
        }
    }
    return g_skip.state == 1;
}

/* Debug aid: GT3PC_PGL_PROBE=x,y[;x,y ...] (up to four GS pixels of the draw
 * buffer, e.g. of the 640 x 448 race screen) - in the frame F9 traces
 * (GT3PC_PGL_TRACE), each point's pixel is read back after every draw into a
 * colour target that holds it and logged when it changed (`pgl probe` lines
 * with the number of the trace line of the draw): the passes that make the
 * colour of a wrong pixel, in order, and what each left there.  Each read
 * waits for the GPU (the traced frame only). */
struct ProbeSet {
    int state = -1;                     /* -1 not read, 0 off, 1 on */
    int n = 0;
    int x[4] = {}, y[4] = {};
    Com<ID3D11Texture2D> staging;       /* 1 x 1, the targets' RGBA8 */
    std::unordered_map<uint64_t, uint32_t> last;    /* (target, point) -> the pixel read last */
} g_probe;

bool probe_on()
{
    if (g_probe.state < 0) {
        g_probe.state = 0;
        const char *e = getenv("GT3PC_PGL_PROBE");
        for (const char *s = e; s != nullptr && *s != 0 && g_probe.n < 4;) {
            int x = 0, y = 0, used = 0;
            if (sscanf(s, " %d , %d%n", &x, &y, &used) != 2 || used <= 0)
                break;
            g_probe.x[g_probe.n] = x;
            g_probe.y[g_probe.n] = y;
            g_probe.n++;
            s += used;
            while (*s == ';' || *s == ' ' || *s == '/')
                s++;
        }
        if (g_probe.n > 0) {
            g_probe.state = 1;
            pclog::write("pgl: GT3PC_PGL_PROBE=%s: %d point(s) read back after each draw of a traced frame (F9)", e,
                         g_probe.n);
        }
    }
    return g_probe.state == 1;
}

/* The execution's CPU profile (pgl_render.h ProfScope): a stack of open
 * scopes, each one's own ticks (the TSC) by kind.  PC (2026-11-05): one per
 * stage (the executor's, the submit thread's: t_prof, set as the thread
 * starts), summed at the frame's end. */
struct ProfOpen {
    uint8_t cat;
    uint64_t start, child;
};
constexpr int kProfDepth = 32;
struct ProfState {
    ProfOpen stack[kProfDepth];
    int depth;
    uint64_t ticks[PROF_COUNT];
    uint32_t count[PCOUNT_N];
};
ProfState g_profA, g_profB;
thread_local ProfState *t_prof = &g_profA;
uint64_t g_profTsc0;                    /* the frame's start (exec_begin_frame): ticks against QPC */
int64_t g_profQpc0;
const char *const kProfNames[PROF_COUNT] = {"OTHER", "GS STREAM", "MODELS", "VU1 STRIPS", "UPLOADS", "EFFECTS",
                                            "READBACK", "STATE", "TEX LOOKUP", "TEX DECODE", "TARGET COPY",
                                            "D3D SUBMIT", "VTX STREAM", "MESH CHECK", "MESH DECODE", "GS FLUSH",
                                            "CLUT LOAD", "TARGET SET", "CONSTANTS", "RUN REPLAY"};
const char *const kProfCountNames[PCOUNT_N] = {"DRAWS", "STATES", "TEX BINDS", "DECODES", "PARTS", "GS CMDS",
                                               "UPLOADS", "COPIES", "GS VERTS", "BATCHES", "CLUTS", "CLUT MISS",
                                               "COMPOSES", "CB WRITES", "D3D SYNCS", "CLUT SLOW", "TEX SLOW",
                                               "VRAM GENS", "PAL MISS", "VU1 DIRECT", "VU1 CLIPPED", "MERGED",
                                               "CLIP BEHIND", "CLIP NEAR", "CLIP FAR", "CLIP XY", "CLIP NOBOX",
                                               "TEX STATES", "MJ NONE", "MJ STATE", "MJ VS", "MJ BUF", "MJ INDEX",
                                               "MJ BATCH", "MJ REC", "MJ PS", "MJ SLOTS", "MJ CB", "RUNS HIT",
                                               "RUNS MISS", "RUN PARTS", "TEX SLOW SET", "TEX SLOW OTHER",
                                               "ALPHA>80", "AD 2INST", "AD TWICE", "MIRROR PASS", "MIRROR SYNC",
                                               "MIRROR FOLD", "RUN TEX FREE", "RUN WALKS", "RUN CLUT FREE",
                                               "RUN WALK NOREC", "RUN WALK IDENT", "RUN WALK PAL", "RUN WORKS",
                                               "RUN CUTS", "RUN WALKED", "MIRROR DROP", "MIRROR CLEAR",
                                               "MIRROR REVAL", "FAST CLEARS", "AD DOUBLE", "ANISO TEX"};

/* PC: GT3PC_PGL_PROFILE=N: the profile's mean over N frames in the log too
 * (exec_end_frame) - the executor's time by kind, the D3D thread's busy
 * time, the executor's waits for it, the counts; the profile then runs in
 * every frame (else in the verbose monitor's alone). */
int g_profLogEvery = -1;
double g_profLogMs[PROF_COUNT];
double g_profLogCounts[PCOUNT_N];
double g_profLogD3d, g_profLogSync, g_profLogWall;
double g_profLogSub, g_profLogSubWait;  /* PC (2026-11-05): the submit thread's */
uint32_t g_profLogFrames;

int prof_log_every()
{
    if (g_profLogEvery < 0) {
        const char *e = getenv("GT3PC_PGL_PROFILE");
        g_profLogEvery = (e != nullptr && atoi(e) > 0) ? atoi(e) : 0;
        if (g_profLogEvery > 0)
            pclog::write("pgl: GT3PC_PGL_PROFILE=%d: the executor's CPU profile logged every %d frames",
                         g_profLogEvery, g_profLogEvery);
    }
    return g_profLogEvery;
}

/* The kind of a command's own work */
ProfCat prof_cat_of(uint8_t type)
{
    switch (type) {
    case CMD_REGS: case CMD_AD: case CMD_PRIM: case CMD_GIF: return PROF_GS;
    case CMD_TARGET: return PROF_TARGET_SET;
    case CMD_STRIP: case CMD_TRI16: return PROF_STRIP;
    case CMD_MODEL: return PROF_MODEL;
    case CMD_UPLOAD: case CMD_TEX1_UPLOAD: return PROF_UPLOAD;
    case CMD_READBACK: return PROF_READBACK;
    case CMD_PFX_COLOR: case CMD_PFX_SPLIT: case CMD_FLARE_QUERY: case CMD_HALF_BLEND: case CMD_POST_AA:
        return PROF_EFFECT;
    }
    return PROF_OTHER;
}
/* PC (CPU time, 2026-11-03): by a table (the switch ran for every command,
 * ~2 % of the executor with the profile off) */
struct ProfCats {
    uint8_t cat[256];
    ProfCats()
    {
        for (int t = 0; t < 256; t++)
            cat[t] = (uint8_t)prof_cat_of((uint8_t)t);
    }
};
const ProfCats g_profCats;
inline ProfCat prof_cat(uint8_t type) { return (ProfCat)g_profCats.cat[type]; }

/* Debug aid: GT3PC_PGL_STATS=N logs what every N-th frame executed. */
/* (Stats, g_stats: above, with the alpha mirror's helpers) */

/* ======================================================================== */
/* Logging                                                                  */
/* ======================================================================== */

void log_state(const gs::DrawState &ds)
{
    static bool dateAlpha = false;          /* PC (CPU time: log_once takes a lock, per draw here) */
    if (ds.date && (ds.frameWriteMask & gs::WRITE_A) != 0 && !dateAlpha) {
        dateAlpha = true;
        log_once("pgl-date-alpha", g_dateStencil
                 ? "pgl: DATE draws that write alpha: the draws after them in their run see those writes (the"
                   " alpha mirror)"
                 : "pgl: DATE draws that write alpha: the destination alpha is the one before their run of DATE"
                   " draws (their own alpha writes are not seen)");
    }
    if (ds.inexact == 0)
        return;
    static std::vector<uint32_t> seen;
    const uint32_t id = ds.inexact | ((uint32_t)ds.blend.selectors << 16) | ((ds.blend.fix > 0x80 ? 1u : 0u) << 24);
    if (std::find(seen.begin(), seen.end(), id) != seen.end())
        return;
    seen.push_back(id);
    static const char *names[] = {"blend form without a fixed-function equivalent", "blend FIX > 0x80",
                                  "blend C = Ad", "reserved blend selector", "PABE", "COLCLAMP off",
                                  "partial FBMSK bytes", "dithering", "AA1 antialiasing",
                                  "PRMODECONT.AC = 0", "16-bit frame"};
    char key[64];
    snprintf(key, sizeof(key), "pgl-inexact-%04X-%02X-%d", ds.inexact, ds.blend.selectors, ds.blend.fix > 0x80);
    std::string what;
    for (int i = 0; i < 11; i++) {
        if (ds.inexact & (1u << i)) {
            if (!what.empty())
                what += ", ";
            what += names[i];
        }
    }
    log_once(key, "pgl: inexact GS state (%s): ALPHA 0x%02X FIX 0x%02X, FRAME psm 0x%02X FBMSK 0x%08X",
             what.c_str(), ds.blend.selectors, ds.blend.fix, ds.fpsm, ds.fbmsk);
}

/* GT3PC_PGL_TRACE: the texture of TEX0 as text */
void trace_tex(char *out, size_t n, uint64_t tex0v, const TexBinding *bound)
{
    const gs::Tex0 t = gs::decodeTex0(tex0v);
    char from[72] = "";
    if (bound != nullptr && bound->target != nullptr)
        snprintf(from, sizeof(from), " from the target at 0x%X", bound->target->base);
    else if (bound != nullptr && bound->hasMean)
        /* the decoded texels' mean: the palette the draw got (a wrong CLUT
         * slice or copy shows as another colour than the texture's) */
        snprintf(from, sizeof(from), " from an upload / plane, mean %02X %02X %02X a %02X", bound->mean & 0xFF,
                 (bound->mean >> 8) & 0xFF, (bound->mean >> 16) & 0xFF, bound->mean >> 24);
    else if (bound != nullptr)
        snprintf(from, sizeof(from), " from an upload / plane");
    /* where an upload's texture came from and what owns its block now
     * (registry_trace_texture: a draw with another texture's picture) */
    char source[512] = "";
    if (bound != nullptr && bound->target == nullptr) {
        source[0] = ' ';
        source[1] = '(';
        registry_trace_texture(tex0v, *bound, source + 2, sizeof(source) - 3);
        strcat(source, ")");
    }
    snprintf(out, n, "tex 0x%X tbw %u %s %ux%u tfx %u tcc %u cbp 0x%X csa %u%s%s", (unsigned)(t.tbp0 & 0x3FFF),
             t.tbw, gstex::psm_name(t.psm), 1u << std::min<uint32_t>(t.tw, 11), 1u << std::min<uint32_t>(t.th, 11),
             t.tfx, t.tcc ? 1u : 0u, (unsigned)t.cbp, t.csa, from, source);
}

/* GT3PC_PGL_TRACE: a draw state compute_state gave up (frame: FRAME). */
void trace_skip(const char *why, uint64_t frame, uint64_t tex0v, bool tme)
{
    if (!g_tracing)
        return;
    const gs::Frame f = gs::decodeFrame(frame);
    char tex[192] = "untextured";
    if (tme)
        trace_tex(tex, sizeof(tex), tex0v, nullptr);
    pclog::write("pgl trace %u: skipped (%s): FRAME 0x%X psm %02X FBMSK %08X | PRIM %03X | %s", g_traceN++, why,
                 ((uint32_t)f.fbp * 32) & 0x3FFF, f.psm, f.fbmsk, gs::encodePrim(g_bs.ds.prim), tex);
}

/* GT3PC_PGL_TRACE: one draw of the current state (all its passes); area:
 * the native pixels it can touch (null: the scissor's). */
void trace_draw(bool indexed, uint32_t count, const D3D11_RECT *area)
{
    const BatchState &b = g_bs;
    const gs::DrawState &s = b.ds;
    char tex[768] = "untextured";
    if (b.tme)
        trace_tex(tex, sizeof(tex), b.tex0, &b.tex);
    char prog[24] = "";
    if (indexed && g_traceProg >= 0)
        snprintf(prog, sizeof(prog), " program %d", g_traceProg);
    /* what else makes the colour: the VU1 vertices' mean colour (GS units),
     * the LOCAL programs' specular colour (L[4]), FOGCOL and the VU1 fog
     * (F = f + 0.1 + (255 - f) min(start, w) / w) */
    std::string more;
    char part[160];
    /* where: GS primitives their vertices' box in GS pixels of the target
     * and their texel range, VU1 draws the box of their area; the scissor */
    if (!indexed && !g_verts.empty()) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
        for (const GsVtx &v : g_verts) {
            x0 = std::min(x0, v.x);
            y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x);
            y1 = std::max(y1, v.y);
            u0 = std::min(u0, v.u);
            v0 = std::min(v0, v.v);
            u1 = std::max(u1, v.u);
            v1 = std::max(v1, v.v);
        }
        snprintf(part, sizeof(part), " | GS (%.3f,%.3f)-(%.3f,%.3f)", (x0 - b.pix) / b.sx, (y0 - b.pix) / b.sy,
                 (x1 - b.pix) / b.sx, (y1 - b.pix) / b.sy);
        more += part;
        if (b.tme) {
            snprintf(part, sizeof(part), " uv (%.3f,%.3f)-(%.3f,%.3f)", u0, v0, u1, v1);
            more += part;
        }
    } else if (area != nullptr) {
        snprintf(part, sizeof(part), " | GS about (%.1f,%.1f)-(%.1f,%.1f)", area->left / b.sx, area->top / b.sy,
                 area->right / b.sx, area->bottom / b.sy);
        more += part;
    }
    snprintf(part, sizeof(part), " | scissor GS (%.2f,%.2f)-(%.2f,%.2f) of native %dx%d", b.scissor.left / b.sx,
             b.scissor.top / b.sy, b.scissor.right / b.sx, b.scissor.bottom / b.sy, b.rt->nw, b.rt->nh);
    more += part;
    if (indexed && g_traceHasVcol) {
        snprintf(part, sizeof(part), " | vertex mean %.1f %.1f %.1f a %.1f", g_traceVcol[0], g_traceVcol[1],
                 g_traceVcol[2], g_traceVcol[3]);
        more += part;
    }
    if (indexed && g_traceVu1 != nullptr && g_traceVu1->program == VU1P_LOCAL) {
        snprintf(part, sizeof(part), " | spec %.1f %.1f %.1f", g_traceVu1->L[4][0], g_traceVu1->L[4][1],
                 g_traceVu1->L[4][2]);
        more += part;
    }
    if (indexed && g_traceInst != nullptr) {
        /* PC: a depth not from w and the projection (docs/renderer.md 4,
         * "Precision") and an overlay's layer ("Overlays") */
        const int zm = (int)g_traceInst->zFromW[2];
        if (zm != 1) {
            snprintf(part, sizeof(part), " | Z %s", zm == 2 ? "from w (matrix)" : "from clip z");
            more += part;
        }
        if (g_traceOverlay != 0) {
            snprintf(part, sizeof(part), " | overlay %d", g_traceOverlay);
            more += part;
        }
    }
    if (s.fog) {
        snprintf(part, sizeof(part), " | fogcol %02X %02X %02X", s.fogColor.r, s.fogColor.g, s.fogColor.b);
        more += part;
        if (indexed && g_traceVu1 != nullptr) {
            snprintf(part, sizeof(part), " f %.1f start %.3f", g_traceVu1->viewCenter[3] - 0.1f,
                     g_traceVu1->limits[3]);
            more += part;
        }
    }
    pclog::write("pgl trace %u: %s %u%s | FRAME 0x%X psm %02X %dx%d FBMSK %08X | ZBUF %s0x%X | PRIM %03X | ALPHA %02X FIX"
                 " %02X%s | ATST %d ref %02X afail %d | DATE %d datm %d | ZTST %d func %d write %d | %d pass(es) | %s%s",
                 g_traceN++, indexed ? "vu1" : "gs", count, prog, b.rt->base, s.fpsm, b.rt->gsW, b.rt->gsH, s.fbmsk,
                 b.zt != nullptr ? "" : "(none) ", b.zt != nullptr ? b.zt->base : 0u, gs::encodePrim(s.prim),
                 s.blend.selectors, s.blend.fix, s.blend.enable ? "" : " (off)", (int)s.alphaFunc, s.aref,
                 (int)s.afail, s.date ? 1 : 0, s.datm ? 1 : 0, s.depthTest ? 1 : 0, (int)s.depthFunc,
                 s.depthWrite ? 1 : 0, s.passCount, tex, more.c_str());
}

/* GT3PC_PGL_PROBE: the probe points' pixels of t after the draw traced last
 * (logged when they changed). */
void probe_draw(Target &t)
{
    if (!g_tracing || !probe_on() || t.depth || !t.tex)
        return;
    if (!g_probe.staging) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 1;
        d.Height = 1;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, g_probe.staging.put()))) {
            log_once("pgl-probe-staging", "pgl: GT3PC_PGL_PROBE: no staging texture: probes off");
            g_probe.state = 0;
            return;
        }
    }
    for (int i = 0; i < g_probe.n; i++) {
        const int x = g_probe.x[i], y = g_probe.y[i];
        if (x < 0 || y < 0 || x >= t.gsW || y >= t.gsH)
            continue;
        /* the native pixel at the GS pixel's centre (GS pixel x covers
         * native [x s, (x + 1) s)) */
        const UINT nx = (UINT)std::max(0, std::min(t.nw - 1, (int)(((float)x + 0.5f) * t.sx)));
        const UINT ny = (UINT)std::max(0, std::min(t.nh - 1, (int)(((float)y + 0.5f) * t.sy)));
        const D3D11_BOX box = {nx, ny, 0, nx + 1, ny + 1, 1};
        dc()->CopySubresourceRegion(g_probe.staging.get(), 0, 0, 0, 0, target_tex1(t), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(dc()->Map(g_probe.staging.get(), 0, D3D11_MAP_READ, 0, &m)))
            continue;
        uint32_t px;
        memcpy(&px, m.pData, 4);
        dc()->Unmap(g_probe.staging.get(), 0);
        const uint64_t key = ((uint64_t)(uintptr_t)&t << 2) | (uint64_t)i;
        auto it = g_probe.last.find(key);
        if (it != g_probe.last.end() && it->second == px)
            continue;
        char was[40] = "first read";
        if (it != g_probe.last.end())
            snprintf(was, sizeof(was), "was %02X %02X %02X a %02X", it->second & 0xFF, (it->second >> 8) & 0xFF,
                     (it->second >> 16) & 0xFF, it->second >> 24);
        pclog::write("pgl probe (%d,%d) of 0x%X %dx%d after trace %u: %02X %02X %02X a %02X (%s)", x, y, t.base,
                     t.gsW, t.gsH, g_traceN - 1, px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF, px >> 24, was);
        g_probe.last[key] = px;
    }
}

/* ======================================================================== */
/* State                                                                    */
/* ======================================================================== */

inline int iceil(float v)
{
    return (int)std::ceil(v - 1e-4f);
}

float z_norm(uint8_t zpsm)
{
    switch (zpsm & 0x0F) {
    case 0x0: return 1.0f / 4294967296.0f;
    case 0x1: return 1.0f / 16777216.0f;
    default: return 1.0f / 65536.0f;
    }
}

/* PC (CPU time): gs::translate is a function of FRAME, ZBUF, TEST, ALPHA,
 * FBA, SCISSOR, XYOFFSET, PABE, COLCLAMP, DTHE, PRMODECONT, SCANMSK, FOGCOL
 * and the PRIM bits alone: its results by those (direct-mapped; a frame
 * has a few dozen). */
struct TranslateMemo {
    uint64_t key[14];
    bool valid = false;
    gs::DrawState ds;
};
TranslateMemo g_translateMemo[128];

gs::DrawState translate_memo(const gs::Regs &r, uint32_t primBits)
{
    const uint64_t key[14] = {r.reg[gs::REG_FRAME], r.reg[gs::REG_ZBUF], r.reg[gs::REG_TEST], r.reg[gs::REG_ALPHA],
                              r.reg[gs::REG_FBA], r.reg[gs::REG_SCISSOR], r.reg[gs::REG_XYOFFSET],
                              r.gen[gs::GEN_PABE], r.gen[gs::GEN_COLCLAMP], r.gen[gs::GEN_DTHE],
                              r.gen[gs::GEN_PRMODECONT], r.gen[gs::GEN_SCANMSK], r.gen[gs::GEN_FOGCOL], primBits};
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint64_t k : key)
        h = mul64(h ^ k, 0xFF51AFD7ED558CCDull);
    TranslateMemo &m = g_translateMemo[(h ^ (h >> 29)) % 128];
    if (!m.valid || memcmp(m.key, key, sizeof(key)) != 0) {
        m.ds = gs::translate(r, primBits);
        memcpy(m.key, key, sizeof(key));
        m.valid = true;
    }
    return m.ds;
}

bool state_texture(BatchState &b, const gs::Regs &r, uint32_t &flags);
void state_flags(BatchState &b, const gs::Regs &r, uint32_t flags);

/* PC (2026-11-05): the arguments compute_state looked its targets up by (the
 * resolved runs: run_targets_same looks them up again) */
struct TargetArgs {
    uint32_t base, fbw, psm;
    int fw, fh;
    uint32_t zbase, zpsm;
    bool depthFrame, hasZ;
};
TargetArgs g_targetArgs;
bool g_stateTexFail;                    /* (compute_state skipped the state for its texture: run_rec_state) */

void compute_state()
{
    ProfScope prof(PROF_STATE);
    g_profCount[PCOUNT_STATES]++;
    BatchState &b = g_bs;
    b = BatchState();
    b.valid = true;
    g_subPosted = false;                /* PC: (the submit stage gets it with its first draw: post_state) */
    g_targetArgs = TargetArgs();        /* PC: (none looked up yet: run_rec_state) */
    g_stateTexFail = false;
    g_stateSerial++;                    /* PC: (a merged draw's batches have one state: MergedDraw) */
    g_fbPending = false;                /* PC: a texture of the target's copy is fresh (g_fbRects) */
    g_fbRects.clear();
    g_segStart = 0;
    const uint32_t prim = g.prim;
    const int ctxt = (prim >> 9) & 1;
    gs::Regs r;
    for (int i = 0; i < gs::REG_COUNT; i++)
        r.reg[i] = g.ctx[ctxt][i];
    for (int i = 0; i < gs::GEN_COUNT; i++)
        r.gen[i] = g.gen[i];
    uint32_t primBits = prim;
    const bool ac = (g.gen[gs::GEN_PRMODECONT] & 1) != 0;
    if (!ac)
        primBits = (prim & 7) | ((uint32_t)g.prmode & 0x7F8);
    b.ds = translate_memo(r, primBits);
    if (!ac)
        b.ds.inexact &= (uint16_t)~gs::INEXACT_PRMODE;
    log_state(b.ds);
    b.type = prim & 7;
    switch (b.type) {
    case 0: b.topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 1: case 2:
        /* (wide lines: quads, emit_line) */
        b.topo = g_lineWidth > 1.0f ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
    default: b.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    }
    b.tex0 = r.reg[gs::REG_TEX0];
    if (b.ds.passCount == 0) {
        b.skip = true;
        t_stats->skipNoPass++;
        trace_skip("nothing passes the tests", r.reg[gs::REG_FRAME], b.tex0, b.ds.prim.tme);
        return;
    }

    /* frame */
    const gs::Frame f = gs::decodeFrame(r.reg[gs::REG_FRAME]);
    const uint32_t base = ((uint32_t)f.fbp * 32) & 0x3FFF;
    const gs::Scissor &sc = b.ds.scissor;
    const int fw = (int)sc.x1 + 1, fh = (int)sc.y1 + 1;
    /* PC (2026-11-05): what the targets were looked up by (the resolved runs
     * look them up again before a replay: run_targets_same) */
    g_targetArgs = TargetArgs{base, f.fbw, f.psm, fw, fh, 0, 0, (f.psm & 0x30) == 0x30, false};
    if ((f.psm & 0x30) == 0x30) {
        b.depthFrame = true;
        b.rt = depth_frame_target(base, f.fbw, f.psm, fw, fh);
    } else {
        b.rt = color_target(base, f.fbw, f.psm, fw, fh);
    }
    if (b.rt == nullptr) {
        b.skip = true;
        t_stats->skipTarget++;
        trace_skip("no target", r.reg[gs::REG_FRAME], b.tex0, b.ds.prim.tme);
        return;
    }
    /* PC: small effect buffers whose GS pixels are the data go to GS
     * resolution (pgl_targets.cpp target_gs_resolution, docs/renderer.md 5):
     *   - scan-masked rows into one (at most 64 GS pixels across: T449's
     *     wheel-face blur, the only one; the ghost cars mask the race screen):
     *     GS rows the game then averages by a 2:1 shrink;
     *   - one of at most 64 x 64 that reads itself: the flare occlusion
     *     query's work buffer, halved in place by bilinear sprites whose taps
     *     fall midway between two GS pixels (flare_reduce, flare_build_reduce).
     *     At s native pixels per GS pixel they average native pairs, the last
     *     texel is no longer the box's mean, and the copy's first native
     *     column is the pixel left of the box (x0 s is not a whole native
     *     pixel); at GS resolution the copy takes the screen at the GS pixel
     *     centres and the halvings are the GS's;
     *   - one of at most 64 x 64 drawn from a small buffer already at GS
     *     resolution: the result strip (a texel per query, read back). */
    if (!b.depthFrame) {
        const bool smallRt = b.rt->gsW <= 64 && b.rt->gsH <= 64;
        if (b.ds.scanmask >= 2 && b.rt->gsW <= 64) {
            target_gs_resolution(*b.rt, true, "takes scan-masked rows");
        } else if (smallRt && b.ds.prim.tme) {
            const gs::Tex0 t = gs::decodeTex0(b.tex0);
            const uint32_t tbp = t.tbp0 & 0x3FFF;
            int ox = 0, oy = 0;
            Target *src = (t.psm & 0x30) == 0x30 ? nullptr : registry_colour_target_at(tbp, ox, oy);
            if (src == b.rt)
                target_gs_resolution(*b.rt, false, "reads itself (the flare queries' halvings)");
            else if (src != nullptr && src->gsW <= 64 && src->gsH <= 64 && src->nw == src->gsW &&
                     src->nh == src->gsH)
                target_gs_resolution(*b.rt, false, "is drawn from a GS-resolution buffer (the flare results)");
        }
    }
    b.sx = b.rt->sx;
    b.sy = b.rt->sy;
    b.pix = b.rt->samples > 1 ? 0.0f : 0.5f;
    b.zpsm = b.ds.zpsm;

    /* depth buffer */
    if (!b.depthFrame) {
        bool needZ = b.ds.depthTest && b.ds.depthFunc != gs::Compare::Always;
        for (int p = 0; p < b.ds.passCount; p++)
            needZ = needZ || b.ds.pass[p].depthWrite;
        if (needZ) {
            const gs::Zbuf z = gs::decodeZbuf(r.reg[gs::REG_ZBUF]);
            g_targetArgs.zbase = ((uint32_t)z.zbp * 32) & 0x3FFF;
            g_targetArgs.zpsm = 0x30u | z.psm;
            g_targetArgs.hasZ = true;
            b.zt = depth_target(g_targetArgs.zbase, g_targetArgs.zpsm, *b.rt);
            if (b.zt == nullptr) {
                b.skip = true;
                return;
            }
        }
    }

    /* raster */
    b.ofx = b.ds.offset.ofx;
    b.ofy = b.ds.offset.ofy;
    b.scissor.left = std::max(0, std::min(b.rt->nw, iceil(sc.x0 * b.sx)));
    b.scissor.top = std::max(0, std::min(b.rt->nh, iceil(sc.y0 * b.sy)));
    b.scissor.right = std::max(0, std::min(b.rt->nw, iceil((sc.x1 + 1) * b.sx)));
    b.scissor.bottom = std::max(0, std::min(b.rt->nh, iceil((sc.y1 + 1) * b.sy)));
    if (b.scissor.right <= b.scissor.left || b.scissor.bottom <= b.scissor.top || sc.x1 < sc.x0 || sc.y1 < sc.y0) {
        b.skip = true;
        t_stats->skipScissor++;
        return;
    }
    b.flat = !b.ds.prim.iip;

    /* texture */
    b.tme = b.ds.prim.tme;
    b.fst = b.ds.prim.fst;
    uint32_t flags = 0;
    if (b.tme) {
        if (!state_texture(b, r, flags)) {
            g_stateTexFail = true;          /* PC: (the resolved runs: no state to keep) */
            return;
        }
    } else if (b.depthFrame) {
        log_once("pgl-depthframe-flat", "pgl: an untextured draw into a depth buffer as a frame: not native");
        b.skip = true;
        return;
    }
    state_flags(b, r, flags);
}

/* PC (2026-11-10): [Graphics] AnisotropicFiltering (settings.h), taken as each
 * frame begins executing (exec_begin_frame): 1 off, 2 .. 16 the most.  The
 * resolved runs keep the level they were recorded at (RunRec::aniso: one of
 * another level is resolved again, run_try). */
int g_aniso = 1;

/* PC (2026-11-10): draw state b's bilinear texture filtered over the
 * footprint (PSF_ANISO; pgl_d3d.cpp aniso_rgb: up to g_aniso taps along its
 * long axis, each the GS's bilinear sample, weighted by its alpha - a texel
 * the alpha hides lends its neighbours no colour, the PCSX2 artifact the
 * user named; the alpha the centre's, so the alpha test, the blend and the
 * frame's alpha are the original's): a 3D draw's (STQ, not a sprite: the 2D
 * and the HUD keep the GS's sampling) of a texture decoded from the GS's
 * memory - not a target (the effects' copies and blurs sample at exact
 * offsets), a depth texture or a host image (its texels not the GS's).
 * (First form, 2026-11-10: the hardware's anisotropic filter, which
 * averages the colours alone, so only textures whose alpha hid nothing - the
 * user's rally stage: 7 states given it, 65 kept out; Smokey Mountain North:
 * 208 of its 572 textures have texels of alpha 0, 364 one alpha value.) */
bool aniso_ok(const BatchState &b)
{
    if (g_aniso <= 1 || b.fst || b.ds.prim.type == gs::PrimType::Sprite)
        return false;
    if (b.tex.target != nullptr || b.tex.depthTex || b.tex.linear)
        return false;
    g_profCount[PCOUNT_ANISO]++;
    return true;
}

/* PC (2026-11-10, a check): GT3PC_PGL_ANISO_ZERO=1 - the anisotropic draws
 * take their centre's colour alone (CbPs::pad1 = 1, aniso_rgb): the pictures
 * must equal those without the filtering (the flag's other path the old
 * one, the taps' weights aside); =2 (a comparison) the taps unweighted by
 * their alpha, as a hardware anisotropic filter averages */
int g_anisoZero = 0;

/* PC (CPU time, 2026-11-03): the texture part of a draw state (compute_state;
 * texture_state alone, after texture registers only): the binding, the
 * sampler, the texture's constants and PS flags (into flags).  False: the
 * state is skipped (b.skip). */
bool state_texture(BatchState &b, const gs::Regs &r, uint32_t &flags)
{
    {
        const uint64_t tex0 = r.reg[gs::REG_TEX0];
        bool bound;
        {
            ProfScope prof(PROF_TEXTURE);
            g_profCount[PCOUNT_TEXBINDS]++;
            bound = texture_bind(tex0, r.reg[gs::REG_TEX1], r.reg[gs::REG_CLAMP], r.gen[gs::GEN_TEXA],
                                 b.ds.frameWriteMask, b.rt, b.tex);
        }
        if (!bound) {
            b.skip = true;
            t_stats->skipTex++;
            trace_skip("no texture", r.reg[gs::REG_FRAME], tex0, true);
            return false;
        }
        if (b.tex.target != nullptr && b.tex.target == b.zt)
            b.tex.srv = target_copy_srv(*b.zt, false, COPY_ZTEX);
        if (b.depthFrame != b.tex.depthTex) {
            log_once("pgl-depthframe", "pgl: a depth buffer drawn from a colour texture (or the reverse):"
                     " not native");
            b.skip = true;
            return false;
        }
        const gs::Tex0 t = gs::decodeTex0(tex0);
        if (b.tex.depthTex && b.tex.target != nullptr && depth_log())
            pclog::write("pgl depth: frame %llu: Z texture at block 0x%X (%ux%u) read as the %dx%d pixels of the"
                         " Z buffer at block 0x%X (native %dx%d), into block 0x%X (%dx%d)",
                         (unsigned long long)frame_number(), (unsigned)(t.tbp0 & 0x3FFF), 1u << t.tw,
                         1u << t.th, b.tex.target->gsW, b.tex.target->gsH, b.tex.target->base,
                         b.tex.target->nw, b.tex.target->nh, b.rt->base, b.rt->gsW, b.rt->gsH);
        b.texW = (float)(1u << std::min<uint32_t>(t.tw, 11));
        b.texH = (float)(1u << std::min<uint32_t>(t.th, 11));
        const gs::Tex1 t1 = gs::decodeTex1(r.reg[gs::REG_TEX1]);
        const gs::Clamp cl = gs::decodeClamp(r.reg[gs::REG_CLAMP]);
        bool magLin = t1.mmag;
        const bool minLin = t1.mmin == 1 || t1.mmin == 4 || t1.mmin == 5;
        bool linear = magLin;
        if (t1.lcm || b.fst)
            linear = t1.k <= 0 ? magLin : minLin;   /* LOD = K */
        if (b.tex.linear)
            linear = true;                          /* a high-res image (registry_hires_readback) */
        /* PC (2026-11-13): a replacement texture (pgl_texrepl.h): linear,
         * through its mip levels; [Graphics] AnisotropicFiltering as for the
         * game's own (aniso_rgb's taps, weighted by alpha) */
        const bool repl = b.tex.alphaMode == 3;
        if (repl)
            linear = true;
        if (cl.wms == 3 || cl.wmt == 3)
            log_once("pgl-region-repeat", "pgl: REGION_REPEAT texture wrap: drawn as REPEAT");
        /* PC: REPEAT wraps at the 2^TW x 2^TH space, and the native texture
         * holds only (x0, y0, w, h) of it: a render target sampled holds its
         * 640 x 448 of 1024 x 512, and Direct3D wrapped at those edges - an
         * edge pixel's bilinear sample, half a GS texel past the picture at
         * the output scale, took the far edge's colour (the sky in a line
         * along the bottom of the screen, the right edge's along the left:
         * user report).  An axis the texture does not cover whole is clamped
         * (the GS read the memory beyond the picture there, not its far
         * edge); an upload holds the whole space and wraps as ever. */
        const bool wholeU = b.tex.x0 <= 0.0f && b.tex.w >= b.texW;
        const bool wholeV = b.tex.y0 <= 0.0f && b.tex.h >= b.texH;
        const int wrapU = (cl.wms == 0 || cl.wms == 3) && wholeU ? 0 : 1;
        const int wrapV = (cl.wmt == 0 || cl.wmt == 3) && wholeV ? 0 : 1;
        b.samp = repl ? sampler_state_repl(wrapU, wrapV, 1) : sampler_state(linear, linear, wrapU, wrapV);
        flags |= PSF_TME | ((uint32_t)t.tfx << PSF_TFX_SHIFT) | (t.tcc ? PSF_TCC : 0) |
                 ((uint32_t)b.tex.alphaMode << PSF_AMODE_SHIFT) | (g.fullColor ? PSF_FULL_COLOR : 0u) |
                 (linear && aniso_ok(b) ? PSF_ANISO : 0u);     /* PC (2026-11-10): [Graphics] AnisotropicFiltering */
        b.ps.texX0 = b.tex.x0;
        b.ps.texY0 = b.tex.y0;
        b.ps.texInvW = 1.0f / b.tex.w;
        b.ps.texInvH = 1.0f / b.tex.h;
    }
    return true;
}

void state_passes(BatchState &b);

/* The rest of a draw state's PS flags (after the texture's, in flags) and
 * constants (compute_state). */
void state_flags(BatchState &b, const gs::Regs &r, uint32_t flags)
{
    if (b.ds.fog)
        flags |= PSF_FGE;
    if (b.ds.fba)
        flags |= PSF_FBA;
    if (b.ds.blend.enable) {
        flags |= (uint32_t)b.ds.blend.premul << PSF_PREMUL_SHIFT;
        if (b.ds.blend.c == gs::CSource::Fix)
            flags |= PSF_CFIX;
    }
    if (b.ds.blend.ad == gs::AdMode::Shader && !b.depthFrame) {
        /* C = Ad in the shader (pgl_state_gs.h AdMode): Cd and Ad from a
         * copy of the target, taken before each draw (draw_passes) */
        flags |= PSF_BLEND_AD;
        b.ps.adSel = (float)b.ds.blend.adSel;
    }
    if (b.ds.scanmask >= 2)
        flags |= (uint32_t)b.ds.scanmask << PSF_SCAN_SHIFT;
    if (b.ds.date && !b.depthFrame && g_dateStencil && target_mirror_dsv(*b.rt, b.zt, true) != nullptr) {
        /* PC (2026-11-04): DATE by the alpha mirror's stencil (g_dateStencil):
         * the target keeps its mirror from now on, synced before the draw
         * when invalid (draw_passes) */
        b.dateStencil = true;
        b.rt->dated = true;
    } else if (b.ds.date && !b.depthFrame) {
        /* DATE: the destination alpha from a copy of the target (g_dateRun;
         * PC: DATE_EACH - a copy before every DATE draw, draw_passes) */
        if (g_dateRun.rt == b.rt && g_dateRun.gen == b.rt->gen && b.rt->copySrv && g_dateMode != DATE_EACH)
            b.dateSrv = b.rt->copySrv.get();
        else if (b.rt->copySrv && g_dateMode == DATE_NONE)
            b.dateSrv = b.rt->copySrv.get();    /* PC: (GT3PC_PGL_DATE_COPY=0, a measurement) */
        else
            b.dateSrv = target_copy_srv(*b.rt, false, COPY_DATE);
        flags |= PSF_DATE | (b.ds.datm ? PSF_DATM : 0u);
    }
    b.ps.flags = flags;
    b.ps.aref = (float)b.ds.aref;
    b.ps.fix = (float)b.ds.blend.fix;
    b.ps.rowScale = b.sy;
    b.ps.fogR = b.ds.fogColor.r;
    b.ps.fogG = b.ds.fogColor.g;
    b.ps.fogB = b.ds.fogColor.b;
    const gs::Texa ta = gs::decodeTexa(r.gen[gs::GEN_TEXA]);
    b.ps.ta0 = ta.ta0;
    b.ps.ta1 = ta.ta1;
    /* PC (2026-11-10): PSF_ANISO's taps at most, and the check (aniso_rgb) -
     * for every draw alike (a merged draw's parts share the constants); 0
     * with the filtering off, as before */
    b.ps.pad1 = (float)g_anisoZero;
    b.ps.pad2 = g_aniso > 1 ? (float)g_aniso : 0.0f;
    state_passes(b);
}

/* PC (2026-11-05): the blend state of pass p's repetition rep (draw_passes:
 * C = Ad as two DEST_ALPHA passes, AdMode::TwoPass - the first adds Cs Ad /
 * 255 without writing alpha or Z, the second adds it again with the pass's
 * writes); rep 2: the two as one draw of two instances (pgl_d3d.h
 * PSF_AD_INST: the pass's writes, the alpha equation that keeps Ad for the
 * first instance); mask: the pass's channels */
ID3D11BlendState *pass_blend(const BatchState &b, uint8_t mask, int reps, int rep)
{
    if (b.depthFrame)
        return blend_plain(0);
    gs::Blend bl = b.ds.blend;
    if (rep == 2)
        return blend_state(bl, mask, true);
    if (reps == 2 && rep != 0)
        bl.dst = gs::BlendFactor::One;
    return blend_state(bl, (reps == 2 && rep == 0) ? (uint8_t)(mask & gs::WRITE_RGB) : mask);
}

/* The passes of a C = Ad form (AdMode::TwoPass): 2 for a pass that writes
 * colour, else 1 */
inline int pass_reps(const BatchState &b, uint8_t mask)
{
    return (!b.depthFrame && b.ds.blend.ad == gs::AdMode::TwoPass && (mask & gs::WRITE_RGB) != 0) ? 2 : 1;
}

/* PC (2026-11-05): what a state's passes write (BatchState wrote*: the
 * executor's bookkeeping of each draw, draw_bookkeep) and their blend
 * states (found with the state: the submit stage looks none up, a resolved
 * run's states keep them) - as draw_passes walks the passes */
void state_passes(BatchState &b)
{
    b.wroteRgb = b.wroteTop = b.wroteZ = false;
    for (int p = 0; p < b.ds.passCount && p < 2; p++) {
        const gs::Pass &pass = b.ds.pass[p];
        uint8_t mask = pass.writeMask & b.ds.frameWriteMask;
        bool zw = pass.depthWrite && b.zt != nullptr;
        if (b.depthFrame) {
            mask = 0;
            zw = true;
        } else if (mask == 0 && !zw) {
            continue;
        }
        const int reps = pass_reps(b, mask);
        for (int rep = 0; rep < reps; rep++)
            b.blend[p][rep] = pass_blend(b, mask, reps, rep);
        if (reps == 2)
            b.blend[p][2] = pass_blend(b, mask, reps, 2);
        b.wroteRgb = b.wroteRgb || (mask & gs::WRITE_RGB) != 0;
        b.wroteTop = b.wroteTop || (mask & gs::WRITE_A) != 0;
        b.wroteZ = b.wroteZ || zw;
    }
}

/* PC (CPU time, 2026-11-03): only texture registers written since the state
 * (TEX0, TEX1, CLAMP, TEX2, MIPTBP1 / 2, TEXA, a CLUT load: g_dirtyTex) - its
 * texture part resolved again (state_texture), the rest kept: the GS's tests,
 * blend, targets, scissor and the D3D states found for them depend on none of
 * those.  Each part of a car changes its texture and nothing else, and the
 * whole state was worked out again for every one (STATE 1.3 ms of a 32-car
 * race frame).  False: the whole state is wanted - a skipped or depth-frame
 * state, a target of 64 x 64 or less (its resolution follows what it is drawn
 * from, compute_state). */
constexpr uint32_t kTexFlags = kPsTexFlags;     /* (PSF_TME, TFX, TCC, the alpha mode, full colours) */

/* PC (2026-11-05): the texture part of state b resolved again from the
 * texture registers as they are (texture_state's work; a resolved run's
 * replay binds its draws' textures so, run_replay): false - the state is
 * skipped (b.skip, as state_texture leaves it). */
bool texture_refresh(BatchState &b)
{
    const int ctxt = (g.prim >> 9) & 1;     /* (the context as compute_state takes it) */
    gs::Regs r;
    r.reg[gs::REG_FRAME] = g.ctx[ctxt][gs::REG_FRAME];      /* (trace_skip's) */
    r.reg[gs::REG_TEX0] = g.ctx[ctxt][gs::REG_TEX0];
    r.reg[gs::REG_TEX1] = g.ctx[ctxt][gs::REG_TEX1];
    r.reg[gs::REG_CLAMP] = g.ctx[ctxt][gs::REG_CLAMP];
    r.gen[gs::GEN_TEXA] = g.gen[gs::GEN_TEXA];
    b.tex0 = r.reg[gs::REG_TEX0];
    g_subPosted = false;                    /* PC: (the submit stage gets the state again) */
    /* (as compute_state: a texture of the target's copy is fresh) */
    g_fbPending = false;
    g_fbRects.clear();
    g_segStart = 0;
    if (b.tme) {
        uint32_t flags = 0;
        if (!state_texture(b, r, flags))
            return false;                   /* (skipped, as the whole state would be) */
        b.ps.flags = (b.ps.flags & ~kTexFlags) | flags;
    }
    const gs::Texa ta = gs::decodeTexa(r.gen[gs::GEN_TEXA]);
    b.ps.ta0 = ta.ta0;
    b.ps.ta1 = ta.ta1;
    return true;
}

bool texture_state()
{
    BatchState &b = g_bs;
    if (!b.valid || b.skip || b.depthFrame || b.rt == nullptr || (b.rt->gsW <= 64 && b.rt->gsH <= 64))
        return false;
    ProfScope prof(PROF_STATE);
    g_profCount[PCOUNT_TEXSTATES]++;
    texture_refresh(b);
    return true;
}

/* The state brought up to date for a draw: what it took - 0 nothing (the
 * state stood), 1 the whole state (compute_state), 2 its texture part alone
 * (texture_state).  PC (2026-11-05): told to the resolved runs' recording. */
enum { STATE_KEPT = 0, STATE_WHOLE = 1, STATE_TEXTURE = 2 };

int ensure_state()
{
    if (g_dirty || !g_bs.valid) {
        if (!g_verts.empty())
            flush_batch();
        compute_state();
        g_dirty = false;
        g_dirtyTex = false;
        return STATE_WHOLE;
    }
    if (g_dirtyTex) {
        if (!g_verts.empty())
            flush_batch();
        g_dirtyTex = false;
        if (g_texStateOn && texture_state())
            return STATE_TEXTURE;
        compute_state();
        return STATE_WHOLE;
    }
    return STATE_KEPT;
}

/* ======================================================================== */
/* Drawing                                                                  */
/* ======================================================================== */

/* GT3PC_FLICKER (pgl_flicker.cpp): FNV-1a of n bytes, from h. */
uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u)
{
    const uint8_t *s = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

/* GT3PC_FLICKER: the draw of the current state (kind 0 / 1), or the batch it
 * skipped (2 / 3), as a record of the frame; area: the native pixels it can
 * touch (null: the scissor's), data: its vertices' hash.  A skipped one is
 * kept at once (flicker_note), a draw after its passes (draw_passes: the
 * bindings checked at each). */
FlickerDraw flicker_record(int kind, uint32_t count, const D3D11_RECT *area, uint32_t data)
{
    const BatchState &b = g_bs;
    const gs::DrawState &s = b.ds;
    const bool noScissor = b.scissor.right <= b.scissor.left || b.scissor.bottom <= b.scissor.top;
    FlickerDraw d = {};
    d.kind = (uint8_t)kind;
    d.gpuVerts = d.gpuPrims = d.gpuPixels = d.gpuPassed = ~0u;
    d.gpuBad = 0;
    if (kind >= 2)
        d.why = !b.valid ? 5 : s.passCount == 0 ? 1 : b.rt == nullptr ? 2 : noScissor ? 4
              : (s.prim.tme && b.tex.srv == nullptr) ? 3 : 5;
    d.prim = (uint16_t)gs::encodePrim(s.prim);
    d.count = count;
    d.rt = b.rt != nullptr ? b.rt->base : ~0u;
    if (s.prim.tme) {
        const gs::Tex0 t = gs::decodeTex0(b.tex0);
        d.tex = (uint32_t)(t.tbp0 & 0x3FFF) | (uint32_t)t.psm << 16 | 1u << 31;
        d.texMean = (kind < 2 && b.tex.hasMean) ? b.tex.mean : 0;
    }
    const uint32_t st[] = {(uint32_t)s.blend.selectors, (uint32_t)s.blend.fix, (uint32_t)s.blend.enable,
                           (uint32_t)s.alphaFunc, (uint32_t)s.aref, (uint32_t)s.afail, (uint32_t)s.date,
                           (uint32_t)s.datm, (uint32_t)s.depthTest, (uint32_t)s.depthFunc, (uint32_t)s.depthWrite,
                           (uint32_t)s.fbmsk, (uint32_t)s.passCount, (uint32_t)s.fpsm,
                           b.zt != nullptr ? b.zt->base : ~0u, (uint32_t)b.scissor.left, (uint32_t)b.scissor.top,
                           (uint32_t)b.scissor.right, (uint32_t)b.scissor.bottom};
    d.state = fnv(st, sizeof(st));
    d.data = data;
    D3D11_RECT w = b.scissor;
    if (area != nullptr) {
        if (noScissor) {
            w = *area;
        } else {
            w.left = std::max(w.left, area->left);
            w.top = std::max(w.top, area->top);
            w.right = std::min(w.right, area->right);
            w.bottom = std::min(w.bottom, area->bottom);
        }
    }
    if (b.rt != nullptr && b.sx > 0.0f && b.sy > 0.0f) {
        auto gsx = [&](LONG v) { return (int16_t)std::max(-32768.0f, std::min(32767.0f, std::floor(v / b.sx))); };
        auto gsy = [&](LONG v) { return (int16_t)std::max(-32768.0f, std::min(32767.0f, std::floor(v / b.sy))); };
        d.x0 = gsx(w.left);
        d.y0 = gsy(w.top);
        d.x1 = gsx(w.right);
        d.y1 = gsy(w.bottom);
    }
    if (kind < 2) {
        d.srv = b.tme ? (uint32_t)(uintptr_t)b.tex.srv : 0;
        d.ps = fnv(&b.ps, sizeof(b.ps));
        if (kind == 0 && !g_verts.empty()) {
            d.rgba = g_verts[0].rgba;
            d.u = g_verts[0].u;
            d.v = g_verts[0].v;
        }
    }
    return d;
}

void flicker_note(int kind, uint32_t count, const D3D11_RECT *area, uint32_t data)
{
    flicker_draw(flicker_record(kind, count, area, data));
}

/* PC (2026-11-04): the stencil buffer bound with the draw's target - its
 * depth buffer's, or for a target with an alpha mirror and no depth buffer
 * the mirror's (nullptr: none) */
/* (claim: the draw will write the stencil - a DATE draw, a covering flat
 * alpha write - so the depth target's stencil becomes this target's;
 * target_mirror_dsv) */
ID3D11DepthStencilView *draw_dsv(bool claim)
{
    const BatchState &b = g_bs;
    ID3D11DepthStencilView *dsv = b.zt != nullptr ? b.zt->dsv.get() : nullptr;
    if (g_dateStencil && !b.depthFrame && b.rt->dated) {
        ID3D11DepthStencilView *m = target_mirror_dsv(*b.rt, b.zt, claim);
        if (m != nullptr)
            dsv = m;
    }
    return dsv;
}

/* ======================================================================== */
/* The submit stage (PC, 2026-11-05: on the submit thread while the frame   */
/* pipelines, pgl_queue.h sq; its state g_sub, the records SubVu1 / SubGs)  */
/* ======================================================================== */

/* The executor's state for the submit stage (SubState), queued; the stage
 * takes it as its own (g_sub) before the records that follow. */
/* What the stage takes from state b's targets (SubState's views; PC,
 * 2026-11-05: a run's work takes them too, WorkState) */
void state_views(const BatchState &b, WorkState &w)
{
    w.serial = g_stateSerial;
    w.rtv = nullptr;
    w.dsv = w.dsvRead = nullptr;
    w.zsrv = nullptr;
    w.rtAlpha = nullptr;
    w.nw = w.nh = 0;
    w.samples = 1;
    if (b.rt != nullptr) {
        w.rtv = b.depthFrame ? nullptr : b.rt->rtv.get();
        w.dsv = b.depthFrame ? b.rt->dsv.get() : draw_dsv(b.dateStencil);
        w.nw = b.rt->nw;
        w.nh = b.rt->nh;
        w.samples = b.rt->samples;          /* PC (2026-11-15) */
        /* PC (2026-11-05): the depth buffer read-only with its texture (the
         * draws whose mirror pass is folded in: mirror_fold_ok), when the
         * view bound is the depth buffer's own */
        if (!b.depthFrame && b.zt != nullptr && b.zt->samples <= 1 && w.dsv != nullptr &&
            w.dsv == b.zt->dsv.get()) {
            w.dsvRead = b.zt->dsvRead.get();
            w.zsrv = b.zt->srv.get();
        }
        w.rtAlpha = b.depthFrame ? nullptr : b.rt;      /* PC (2026-11-05): (its alpha bound) */
    }
    w.hasZ = b.zt != nullptr;
}

/* (the submit stage) the state from its record and views */
void sub_state_set(const BatchState &bs, const WorkState &w)
{
    SubState &s = g_sub;
    s.bs = bs;
    s.serial = w.serial;
    s.rtv = w.rtv;
    s.dsv = w.dsv;
    s.dsvRead = w.dsvRead;
    s.zsrv = w.zsrv;
    s.rtAlpha = w.rtAlpha;
    s.nw = w.nw;
    s.nh = w.nh;
    s.samples = w.samples;                  /* PC (2026-11-15) */
    s.hasZ = w.hasZ;
    s.bs.rt = nullptr;                      /* (the stage never reads the targets) */
    s.bs.zt = nullptr;
}

void post_state()
{
    const BatchState &b = g_bs;
    WorkState w;
    state_views(b, w);
    w.state = 0;
    const BatchState bs = b;                /* (by value: the executor goes on changing g_bs) */
    sq::run([bs, w] { sub_state_set(bs, w); });
    g_subPosted = true;
}

/* (the submit stage) the draw's targets, scissor, texture and DATE copy
 * (PC, 2026-11-05: dsv - the state's, or the depth buffer's read-only view
 * for a draw with its mirror pass folded in; set_targets forgets slot 0 when
 * the targets change, so the texture is bound again here) */
void bind_targets(const SubState &s, ID3D11ShaderResourceView *dateSrv, ID3D11DepthStencilView *dsv)
{
    const BatchState &b = s.bs;
    if (b.depthFrame)
        set_targets(nullptr, s.dsv, s.nw, s.nh);
    else
        set_targets(s.rtv, dsv, s.nw, s.nh);
    set_scissor(b.scissor);
    if (b.tme) {
        set_srv0(b.tex.srv);
        set_sampler0(b.samp);
    }
    if (dateSrv != nullptr)
        set_srv1(dateSrv);
}

/* PC (2026-11-03): a held VU1 draw (MergedDraw) of count indices from first,
 * its record inst; c: its pass's PS constants as bound, ps the pixel shader
 * bound; single: its state draws one pass (parts may join it then,
 * merge_join). */
/* PC (2026-11-06): whether a draw's texture lets it join a merged draw: a
 * decoded texture's view as ever; a target's only for a draw from the doubled
 * index stream (the cars' env passes sample the env map, a target) and then
 * a single-sampled colour target (its own view, no copy to refresh between
 * the parts) that is not the one drawn into.  A write to the target between
 * two parts takes another draw state, which ends the merged draw. */
inline bool tex_joinable(const SubState &s, bool doubled)
{
    const BatchState &b = s.bs;
    if (b.tex.srv == nullptr)
        return false;
    if (b.tex.target == nullptr)
        return true;
    return doubled && !b.tex.depthTex && b.tex.target->samples <= 1 && b.tex.target != s.rtAlpha;
}

void merged_start(uint32_t count, uint32_t first, int32_t base, uint32_t inst, const CbPs &c, Ps ps, bool single,
                  const MirrorPlan &plan, uint32_t instances, bool fold, bool doubled)
{
    MergedDraw &m = g_merged;
    const BatchState &b = g_sub.bs;
    m.count = count;
    m.first = first;
    m.base = base;
    m.inst = inst;
    m.instances = instances;
    m.nParts = m.nSlots = 0;
    m.wroteZ = b.wroteZ;                    /* (state_passes: as the passes drawn so far accumulated it) */
    m.plan = plan;                          /* PC: (the alpha mirror: the parts that join have the same) */
    m.folded = fold;                        /* PC (2026-11-05): (its mirror pass its last instance: no pass after) */
    m.doubled = doubled;                    /* PC (2026-11-06): (the doubled index stream: its parts' two passes in order) */
    const Vu1Cur *v = g_vu1Cur;
    m.pad = v != nullptr ? v->inst->pad : 0.0f;
    /* (PC, 2026-11-05: a C = Ad draw of two instances is never single - its
     * parts' order, draw_passes; a draw with its mirror pass folded in may
     * grow: its colour instance covers every part before the mirror instance
     * marks them all, as the merged draw and its one mirror pass did.  PC,
     * 2026-11-06: one from the doubled index stream grows - the stream holds
     * each part's two passes in order) */
    m.mergeable = single && ((c.flags & PSF_AD_INST) == 0 || doubled) && v != nullptr && v->batch != ~0u &&
                  base == 0 && (b.ps.flags & PSF_BLEND_AD) == 0 && !skip_on() &&
                  (!b.tme || tex_joinable(g_sub, doubled)) && merge_supported();
    m.firstBatch = m.lastBatch = v != nullptr ? v->batch : 0;
    if (!m.mergeable)
        return;
    m.serial = g_sub.serial;
    m.vs = v->vs;
    m.cull = v->cull;
    m.vb = v->vb;
    m.ib = v->ib;
    m.ps = c;
    m.ps.flags &= ~kPsTexFlags;
    memcpy(m.zFromW, v->inst->zFromW, sizeof(m.zFromW));
    m.flat = v->inst->flat;
    m.shader = ps;
    m.srv[0] = b.tme ? b.tex.srv : nullptr;
    m.samp[0] = b.tme ? b.samp : nullptr;
    m.nSlots = 1;
    merged_part(m, 0, v->batch, c.flags & kPsTexFlags, *v->inst);
}

/* (the submit stage) The passes of its state (the vertex stage already
 * bound).  area: the native pixels the draw can touch (null: the scissor's;
 * for the trace and GT3PC_FLICKER - the executor keeps the targets' written
 * parts, draw_bookkeep); inst: an indexed (VU1) draw's record (vu1_inst);
 * mirror: the target keeps its alpha mirror through this draw (its stencil
 * ops, a mirror pass); dateSrv: the copy of the target the draw's C = Ad /
 * DATE reads.  PC (2026-11-05): the copies, the mirror's sync, the GPU
 * section and the bookkeeping, which were here, are the executor's before
 * the draw's record (submit_prologue): so every pass comes before the
 * draw's own bindings, and nothing need be bound again (until then a pass
 * here forgot the vertex stage bound by the caller, and the draw went out
 * with the pass's full-screen vertex shader: black triangles over the sky,
 * user report, fixed first by binding it again). */
void draw_passes(bool indexed, uint32_t count, uint32_t first, int32_t baseVertex, int cull, const D3D11_RECT *area,
                 uint32_t inst, bool mirror, ID3D11ShaderResourceView *dateSrv, bool fullCover, bool noPass,
                 bool doubled)
{
    ProfScope prof(PROF_SUBMIT);
    SubState &s = g_sub;
    BatchState &b = s.bs;
    /* (PC, 2026-11-05: a colour target's bindings come with each pass below,
     * with the depth-stencil view its mirror fold needs) */
    if (b.depthFrame)
        bind_targets(s, dateSrv, s.dsv);
    ID3D11RasterizerState *&rs = b.raster[cull & 3];
    if (rs == nullptr)
        rs = raster_state(cull, true);
    set_raster(rs);
    if (g_tracing)
        trace_draw(indexed, count, area);   /* (the executor's state, g_bs: a traced frame does not pipeline) */
    FlickerDraw fd;                         /* PC (CPU time): GT3PC_FLICKER's only (zeroed, a memset a draw) */
    const bool flicker = flicker_on();
    if (flicker) {
        uint32_t data;
        if (indexed) {
            const uint32_t range[3] = {count, first, (uint32_t)baseVertex};
            data = fnv(range, sizeof(range));
            if (g_traceVu1 != nullptr)          /* (the matrices, the viewport, the limits) */
                data = fnv(g_traceVu1, offsetof(CbVu1, ndcMap), data);
        } else {
            data = fnv(g_verts.data(), g_verts.size() * sizeof(GsVtx));
        }
        fd = flicker_record(indexed ? 1 : 0, count, area, data);
    }
    ID3D11Query *gpuQuery = flicker ? flicker_query_begin() : nullptr;
    for (int p = 0; p < b.ds.passCount; p++) {
        const gs::Pass &pass = b.ds.pass[p];
        uint8_t mask = pass.writeMask & b.ds.frameWriteMask;
        bool zw = pass.depthWrite && s.hasZ;
        if (b.depthFrame) {
            mask = 0;
            zw = true;
        } else if (mask == 0 && !zw) {
            continue;
        }
        CbPs c = b.ps;
        if (pass.gate != gs::AlphaGate::None)
            c.flags |= ((uint32_t)pass.gate << PSF_GATE_SHIFT) | ((uint32_t)b.ds.alphaFunc << PSF_ATST_SHIFT);
        if (indexed) {
            /* PC (2026-11-03): a VU1 draw's texture rectangle is its record's
             * (Vu1Inst): the constants stay from texture to texture */
            c.flags |= PSF_INST_RECT;
            c.texX0 = c.texY0 = c.texInvW = c.texInvH = 0.0f;
        }
        /* C = Ad as two DEST_ALPHA passes (AdMode::TwoPass): the first adds
         * Cs Ad / 255 without writing alpha or Z (Ad and the depth test stay
         * as they were), the second adds it again with the pass's writes.
         * PC (2026-11-05): as one draw of two instances when it may (pgl_d3d.h
         * PSF_AD_INST: the form's dst ONE, no depth writes, the alpha
         * mirror's stencil untouched) - one submission and one state */
        const int reps = pass_reps(b, mask);
        /* PC (2026-11-04): the depth state with the alpha mirror's stencil
         * part (mirror_plan: by the alpha this pass writes).  PC (CPU time,
         * 2026-11-05): kept while its inputs are the same - the state, the
         * pass, the vertex and texture alpha ranges, the texture flags (as
         * merge_join keeps its own); a C = Ad form's second pass's plan
         * decides whether the two go as one draw */
        auto plan_of = [&](uint8_t m) {
            MirrorPlan plan;
            if (!mirror)
                return plan;
            struct Last {
                uint32_t serial = ~0u;
                int p = -1, vLo = -1, vHi = -1, tLo = -1, tHi = -1;
                uint8_t mask = 0;
                uint32_t flags = 0;
                float ta0 = 0, ta1 = 0;
                MirrorPlan plan;
            };
            static thread_local Last last;  /* PC (2026-11-05): (per thread: the submit workers) */
            /* (PC, 2026-11-05: a target's alpha bound as the texture's range) */
            const Target *tt = b.tex.target != nullptr && !b.tex.depthTex ? b.tex.target : nullptr;
            const int tLo = b.tex.hasAlpha ? b.tex.aMin : tt != nullptr ? tt->alphaLo - 300 : -1;
            const int tHi = b.tex.hasAlpha ? b.tex.aMax : tt != nullptr ? tt->alphaHi - 300 : -1;
            if (last.serial != s.serial || last.p != p || last.mask != m || last.vLo != g_vLo ||
                last.vHi != g_vHi || last.tLo != tLo || last.tHi != tHi || last.flags != b.ps.flags ||
                last.ta0 != b.ps.ta0 || last.ta1 != b.ps.ta1) {
                last.serial = s.serial;
                last.p = p;
                last.mask = m;
                last.vLo = g_vLo;
                last.vHi = g_vHi;
                last.tLo = tLo;
                last.tHi = tHi;
                last.flags = b.ps.flags;
                last.ta0 = b.ps.ta0;
                last.ta1 = b.ps.ta1;
                last.plan = mirror_plan(s, pass, m, g_vLo, g_vHi);
            }
            return last.plan;
        };
        bool twoInst = false, adPair = false;
        if (reps == 2) {
            /* (the first instance must leave the stencil as the second's
             * DATE test wants it: not DateInvert; a mirror pass after the
             * draw, Replace, DateKeep are as the two draws had them.
             * PC (2026-11-06): DateInvert too - as StencilOp::DatePair, the
             * count through bits 0..1 (pgl_d3d.h): the first instance
             * leaves the mirror's bit clear for the second's test, the
             * second sets it; GT3PC_PGL_DATE_PAIR=0: the two draws) */
            const MirrorPlan p1 = plan_of(mask);
            const bool form = g_adInstOn && b.ds.blend.dst == gs::BlendFactor::One && !zw;
            twoInst = form && (p1.op != StencilOp::DateInvert || g_datePairOn);
            /* PC (2026-11-05): the pair that flips the mirror's bit (an env
             * pass whose alpha bit is known, since the targets' alpha
             * bounds) goes as two draws of one instance each but with the
             * two-instance form's blend state, shader and constants - the
             * second drawn from the record's second copy, whose rep is 1
             * (vu1_inst): one blend state and one set of constants for both
             * instead of the RGB-only and the RGBA ones - fewer changes
             * between the ~1,900 pairs of a 32-car frame (VU1 draws: a GS
             * batch has no record) */
            adPair = form && !twoInst && indexed;
            g_profCount[twoInst ? (doubled ? PCOUNT_AD_DOUBLE : PCOUNT_AD_INST) : PCOUNT_AD_TWICE]++;
            if (g_boundLog && s.rtAlpha != nullptr) {
                /* PC (2026-11-06, a measurement: GT3PC_PGL_BOUND_LOG): the draw
                 * by its target's bound - known and within 1.0 (a DATE draw
                 * then reads Ad = 0x80 exactly where it passes) or not */
                const bool within = s.rtAlpha->alphaLo >= 0 && s.rtAlpha->alphaHi <= 128;
                std::atomic<uint32_t> &n = b.dateStencil ? (within ? g_boundAdDate128 : g_boundAdDateWide)
                                                         : (within ? g_boundAdPlain128 : g_boundAdPlainWide);
                n.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_alphaLog && !b.depthFrame && (mask & gs::WRITE_A) != 0)
            alpha_log_check(s, pass, mask, indexed);     /* PC: (GT3PC_PGL_ALPHA_LOG: GS alphas above 0x80 written) */
        for (int rep = twoInst ? 2 : 0; rep < (twoInst ? 3 : reps); rep++) {
            ID3D11BlendState *&bs = b.blend[p & 1][adPair ? 2 : rep];   /* (adPair: the two-instance form's) */
            const uint8_t m = (reps == 2 && rep == 0) ? (uint8_t)(mask & gs::WRITE_RGB) : mask;
            const bool z = (reps == 2 && rep == 0) ? false : zw;
            /* (GT3PC_PGL_AD_ONCE: a measurement; PC 2026-11-06: one instance of
             * the doubled stream - its halves are the two passes) */
            uint32_t instances = (rep == 2 && !g_adOnce && !doubled) ? 2u : 1u;
            if (bs == nullptr)
                bs = pass_blend(b, mask, reps, adPair ? 2 : rep);    /* (found with the state, state_passes: not here) */
            MirrorPlan plan = plan_of(m);
            /* PC (2026-11-06): the lazy mirror - the stage's note of the
             * largest pass a run's state needed (RunState::passArea: before
             * the executor's verdict takes it away), and that verdict: no
             * pass after this draw, the mirror let go */
            const D3D11_RECT passRect = area != nullptr ? *area : b.scissor;   /* (no area known - the sky: the scissor) */
            if (plan.pass && t_passState != nullptr) {
                const uint32_t px = area_px(passRect, b.scissor, s.nw, s.nh);
                if (px > *t_passState)
                    *t_passState = px;
            }
            if (noPass)
                plan.pass = false;
            /* PC (2026-11-06): the two-instance draw that flips the bit counts
             * (DatePair) where the pair's second draw inverted */
            const StencilOp stOp = (twoInst && plan.op == StencilOp::DateInvert) ? StencilOp::DatePair : plan.op;
            CbPs cr = c;
            if (rep == 2 || adPair)
                cr.flags |= PSF_AD_INST;
            const uint32_t drawInst = inst + (adPair && rep == 1 ? 1u : 0u);   /* (the pair's second: the next copy) */
            /* PC (2026-11-05): the mirror pass as one more instance of this
             * draw (mirror_fold_ok): its flags in the constants, the depth
             * test EQUAL with the fold's stencil ops, the depth buffer bound
             * read-only with its texture */
            const bool fold = mirror_fold_ok(s, plan) && !(rep == 2 && g_adOnce) && !doubled;
            const bool foldDate = plan.op == StencilOp::DateKeep;
            const uint8_t ref = fold ? (foldDate ? plan.ref : (uint8_t)1) : plan.ref;
            if (fold) {
                cr.flags |= mirror_fold_flags(s, plan);
                instances++;
            }
            ID3D11DepthStencilState *dss;
            if (b.depthFrame)
                dss = depth_state(false, gs::Compare::Always, true);
            else if (fold)
                dss = depth_state(true, gs::Compare::Equal, false,
                                  foldDate ? StencilOp::FoldDate : StencilOp::FoldReplace, ref);
            else
                dss = depth_state(s.hasZ && b.ds.depthTest, b.ds.depthFunc, z, stOp, plan.ref);
            /* PC (2026-11-15, user request: AGSS, settings.h): a 3D (VU1)
             * draw into a multisampled target shaded at every sample - its
             * texture, colour, fog and alpha test at each sample of the
             * GPU's pattern (supersampling); the GS batches (2D, the
             * full-screen effects) as MSAA's.  (The fold needs a single
             * sample: never both) */
            const bool perSample = indexed && !fold && per_sample_frame() && s.samples > 1;   /* (the stage's view of
                                                                                                   * the target: b.rt is
                                                                                                   * null here) */
            const Ps ps = b.depthFrame ? Ps::DepthCopy
                          : fold       ? (b.flat ? Ps::MainFlatFold : Ps::MainFold)
                          : perSample  ? (b.flat ? Ps::MainFlatSs : Ps::MainSs)
                                       : (b.flat ? Ps::MainFlat : Ps::Main);
            if (fold) {
                bind_targets(s, dateSrv, s.dsvRead);
                set_srv_z(s.zsrv);
            } else if (!b.depthFrame) {
                set_srv_z(nullptr);
                bind_targets(s, dateSrv, s.dsv);
            }
            bind_ps(ps);
            bind_ps_cb(&cr, sizeof(cr));
            set_blend(bs);
            set_depth(dss, ref);
            if (flicker) {
                /* (GT3PC_FLICKER: what the binding cache has wrong) */
                char what[256];
                const uint32_t bad = d3d_check_bound(b.tme, dateSrv != nullptr, indexed, what, sizeof(what));
                if (bad != 0 && (fd.bound & bad) != bad)
                    flicker_event("bound state differs from the cache: %s", what);
                fd.bound |= bad;
            }
            /* PC (2026-11-06): the frame capture's record of the draw
             * (pgl_capture.h): what it is, as the pass has it */
            auto cap_rec = [&](uint8_t kind) -> uint32_t {
                if (!cap::on())
                    return ~0u;
                cap::Draw d;
                d.kind = kind;
                d.adForm = (uint8_t)(twoInst ? (doubled ? cap::AD_DOUBLE : cap::AD_2INST)
                                     : adPair ? (rep == 0 ? cap::AD_PAIR1 : cap::AD_PAIR2)
                                     : reps == 2 ? (rep == 0 ? cap::AD_TWICE1 : cap::AD_TWICE2)
                                     : b.ds.blend.ad == gs::AdMode::Shader ? cap::AD_SHADER : cap::AD_NONE);
                d.program = (uint8_t)(indexed && g_traceVu1 != nullptr ? g_traceVu1->program : 255);
                d.prim = (uint8_t)(indexed ? 255 : (b.type & 7));
                d.blendSel = b.ds.blend.enable ? b.ds.blend.selectors : 0;
                d.writeMask = m;
                d.zTest = s.hasZ && b.ds.depthTest;
                d.zWrite = z;
                d.zFunc = (uint8_t)b.ds.depthFunc;
                d.date = b.dateStencil || b.ds.date;
                d.datm = b.ds.datm;
                d.stencilOp = (uint8_t)stOp;
                d.stencilRef = plan.ref;
                d.mirrorPass = plan.pass && !fold;
                d.fold = fold;
                d.cause = plan.cause;
                d.flat = b.flat;
                d.slots = b.tme ? 1 : 0;
                d.instances = (uint16_t)instances;
                d.count = count;
                d.mergeWhy = (uint8_t)(indexed ? ((cr.flags & PSF_AD_INST) != 0 ? cap::MW_AD_INST : cap::t_mergeWhy) : 0);
                d.depthFrame = b.depthFrame;
                d.tme = b.tme;
                d.texTarget = b.tme && b.tex.target != nullptr;
                d.atst = (uint8_t)b.ds.alphaFunc;   /* PC (2026-11-07): (the alpha test: a drawn covering batch's) */
                d.aref = b.ds.aref;
                d.agate = (uint8_t)pass.gate;
                d.target = s.rtAlpha != nullptr ? s.rtAlpha->base : 0;
                d.targetW = (uint16_t)s.nw;
                d.targetH = (uint16_t)s.nh;
                const D3D11_RECT &r = area != nullptr ? *area : b.scissor;
                auto c16 = [](LONG v) { return (int16_t)std::max((LONG)-32768, std::min((LONG)32767, v)); };
                d.x0 = c16(r.left);
                d.y0 = c16(r.top);
                d.x1 = c16(r.right);
                d.y1 = c16(r.bottom);
                if (!indexed)
                    d.note = cap::t_note;   /* PC (2026-11-07): (a covering GS batch drawn: why, sub_gs) */
                return cap::draw_begin(d);
            };
            if (indexed && g_mergeOn && q::running() && !flicker && !g_tracing) {
                /* PC (2026-11-03): held back - the next draw may go on from
                 * where it ends, or join it with a texture of its own
                 * (q::hold, MergedDraw, merge_join).  PC (2026-11-05): a draw
                 * of the same pass only, and never a draw of two instances
                 * - its first instance would cover the next part before
                 * the second wrote this part's alpha, which the next part's
                 * first pass reads where they overlap (the GS draws each
                 * part whole, in order: the pictures differed) */
                MergedDraw &md = g_merged;
                if (q::t_holder && q::g_held == &merged_queue && md.nParts <= 1 && md.base == baseVertex &&
                    md.first + md.count == first && md.inst == drawInst &&
                    ((cr.flags & PSF_AD_INST) == 0 || (doubled && md.doubled)) && md.doubled == doubled &&
                    md.instances == instances && md.folded == fold && md.rep == rep && md.plan.op == plan.op &&
                    md.plan.ref == plan.ref && md.plan.pass == plan.pass && md.plan.keep1 == plan.keep1) {
                    md.count += count;
                    if (g_vu1Cur != nullptr && g_vu1Cur->batch != ~0u && g_vu1Cur->batch > md.lastBatch)
                        md.lastBatch = g_vu1Cur->batch;  /* (its record part 0's: merged_part's gap) */
                    else
                        md.mergeable = false;
                    g_profCount[PCOUNT_MERGED]++;
                    md.capParts++;
                    md.passBox = rect_union(md.passBox, passRect);  /* PC (2026-11-07): (its one pass covers this part too) */
                    if (plan.pass && t_passState != nullptr) {
                        const uint32_t px = area_px(md.passBox, b.scissor, s.nw, s.nh);
                        if (px > *t_passState)
                            *t_passState = px;
                    }
                } else {
                    q::release_held();
                    merged_start(count, first, baseVertex, drawInst, cr, ps,
                                 b.ds.passCount == 1 && (reps == 1 || doubled) && !b.depthFrame, plan, instances, fold,
                                 doubled);
                    md.rep = rep;
                    md.capId = cap_rec(cap::K_VU1);
                    md.capParts = 1;
                    md.passBox = passRect;  /* PC (2026-11-07) */
                    q::hold(&merged_queue);
                }
            } else if (indexed) {
                const uint32_t lights = light_mark();
                Rings *const rg = thread_rings();
                const uint32_t capId = cap_rec(cap::K_VU1);
                q::run([rg, count, first, baseVertex, drawInst, lights, instances, capId] {
                    draw_indexed(rg, count, first, baseVertex, drawInst, lights, instances, capId);
                });
                if (plan.pass && !fold)     /* PC: (the alpha mirror; 2026-11-15: per sample as the draw) */
                    queue_mirror_pass(plan, z, b.flat, 1, count, first, baseVertex, drawInst, perSample);
            } else {
                const uint32_t capId = cap_rec(cap::K_GS);
                q::run([count, first, instances, capId] {
                    cap::gpu_begin(capId);
                    if (instances == 1)
                        qdc()->Draw(count, first);
                    else
                        qdc()->DrawInstanced(count, instances, first, 0);
                    cap::gpu_end(capId);
                });
                if (plan.pass && !fold)     /* PC: (the alpha mirror) */
                    queue_mirror_pass(plan, z, b.flat, 0, count, first, 0, 0);
            }
            /* PC (2026-11-05): the target's alpha bound takes this pass's
             * alpha writes (target_alpha_written); a sprite batch covering
             * the target with nothing gating a pixel starts it over */
            if (!b.depthFrame && s.rtAlpha != nullptr && (m & gs::WRITE_A) != 0 &&
                (b.ds.fbmsk & 0x80000000u) == 0 && !(rep == 2 && g_adOnce))
                target_alpha_written(*s.rtAlpha, b, pass,
                                     fullCover && b.ds.passCount == 1 && pass.gate == gs::AlphaGate::None &&
                                         !b.ds.date && !b.dateStencil && !depth_gates(b, s.hasZ) &&
                                         ((b.ps.flags >> PSF_SCAN_SHIFT) & 3) < 2);
            if (fold) {
                g_profCount[PCOUNT_MIRROR_FOLD]++;
                t_stats->mirrorFolds++;
                mirror_cause_count(plan);
                if (g_tracing)
                    pclog::write("pgl trace %u: alpha mirror pass folded into the draw as its instance %u (keeps %d,"
                                 " %s, depth test %d func %d)", g_traceN++, instances - 1, plan.keep1 ? 1 : 0,
                                 foldDate ? "DATE: the bit flipped" : "the bit set", (s.hasZ && b.ds.depthTest) ? 1 : 0,
                                 (int)b.ds.depthFunc);
            }
            t_stats->draws++;
            g_profCount[PCOUNT_DRAWS]++;
            t_stats->verts += count;
        }
    }
    if (gpuQuery != nullptr)
        flicker_query_end(gpuQuery, indexed, first, count, baseVertex);
    if (flicker) {
        fd.rtv = (uint32_t)(uintptr_t)(b.depthFrame ? (void *)s.dsv : (void *)s.rtv);
        flicker_draw(fd);
    }
}

/* ---- the executor's half of a draw (stage A) ---------------------------- */

/* Debug aid GT3PC_PGL_SKIP (SkipSet): the draw at hand left out; program:
 * an indexed draw's lighting program (-1: a GS batch) */
bool draw_skipped(bool indexed, int program)
{
    if (!skip_on())
        return false;
    const BatchState &b = g_bs;
    const int prog = indexed ? program : -1;
    return (g_skip.local && prog == VU1P_LOCAL) || (g_skip.envmtx && prog == VU1P_ENVMTX) ||
           (g_skip.envmap && prog == VU1P_ENVMAP) || (g_skip.cnst && prog == VU1P_CONST) ||
           (g_skip.lights && (prog == VU1P_DIFFUSE || prog == VU1P_SPEC2 || prog == VU1P_SPEC4)) ||
           (g_skip.ad && b.ds.blend.ad != gs::AdMode::None) ||
           (g_skip.alpha && !b.depthFrame && (b.ds.frameWriteMask & gs::WRITE_RGBA) == gs::WRITE_A) ||
           (g_skip.self && b.tme && b.tex.target != nullptr && b.tex.target == b.rt);
}

/* What the draw at hand writes (its state's passes: BatchState wrote*) into
 * the targets' and the registry's books - before its record goes to the
 * submit stage, so that the commands after it see it (where draw_passes did
 * it after the draw: nothing comes between).  area: the native pixels it
 * can touch (null: the scissor's); dateSrv: its DATE copy (the DATE run,
 * g_dateRun). */
/* PC (2026-11-05, a run's work: RunWork): the bookkeeping of a run's draws
 * gathered by their states' targets - the written rectangles' union, which
 * layers were written, the Z buffer - and done once (run_book_flush: at the
 * run's end, and before anything in the run reads a target's books - a
 * texture resolved after all, the mirror's sync, a target texture's resolve
 * of a target the run writes).  The books after the run are the same: the
 * rectangles are unions, the marks idempotent, a target's generation is
 * compared, not counted.  Per draw it was ~7 % of the executor (the
 * sampling profile: target_written_rect, registry_target_written, the
 * depth target's geometry). */
struct RunBook {
    Target *rt = nullptr, *zt = nullptr;
    D3D11_RECT rect = {};
    bool any = false, rgb = false, top = false, z = false, depthFrame = false;
};
std::vector<RunBook> g_runBook;             /* one per RunRec::targets entry of the run being walked */
size_t g_runBookCur = 0;                    /* the current state's entry */
uint16_t g_runStateCur = 0;                 /* (a run's work) the state in effect (RunRec::states: a cut's new work starts from it) */

void run_book_flush()
{
    for (RunBook &k : g_runBook) {
        if (!k.any)
            continue;
        if (k.depthFrame) {
            target_written_rect(*k.rt, k.rect.left, k.rect.top, k.rect.right, k.rect.bottom);
            registry_target_written(*k.rt, true, false);
        } else {
            if (k.rgb || k.top) {
                target_written_rect(*k.rt, k.rect.left, k.rect.top, k.rect.right, k.rect.bottom);
                registry_target_written(*k.rt, k.rgb, k.top);
            }
            if (k.z) {
                depth_target_written(*k.zt, *k.rt);
                k.zt->gen++;
                registry_target_written(*k.zt, true, false);
            }
        }
        k.any = k.rgb = k.top = k.z = false;
    }
}

/* (the books of target t, if the run wrote it) */
void run_book_flush_if(const Target *t)
{
    for (const RunBook &k : g_runBook)
        if (k.any && (k.rt == t || k.zt == t)) {
            run_book_flush();
            return;
        }
}

/* PC (2026-11-06): ([Graphics] EnvMapDetail) a VU1 draw of lighting program
 * prog samples target t: an env pass marks it as a car environment map for the
 * frame (targets_env_maps) */
inline void target_read_by_vu1(Target *t, int prog)
{
    if (prog == VU1P_ENVMTX || prog == VU1P_ENVMAP)
        t->envRead = true;
}

/* PC (2026-11-18, the user's crash of 2026-11-18 - D3D11's Flush reading
 * address 0 - and the run records found written over before it, 2026-11-08
 * too: a command index + 0x10000, byte 178 of a 208-byte block, a Target's
 * envRead): GT3PC_PGL_STALE_LOG=1 - a draw's state naming a texture target
 * that is gone (target_alive: a scan of the live ones) logged, the first 20;
 * where: what the state was taken from */
bool stale_log_on()
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT3PC_PGL_STALE_LOG");
        on = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (on)
            pclog::write("pgl: GT3PC_PGL_STALE_LOG=1: a draw's state naming a texture target that is gone is logged"
                         " (the first 20)");
    }
    return on != 0;
}

void stale_check(const Target *t, const char *where)
{
    static int logged = 0;
    if (t == nullptr || logged >= 20 || target_alive(t))
        return;
    logged++;
    pclog::write("pgl stale: frame %llu: %s names texture target %p, which is gone (program %d, a run's work %s)%s",
                 (unsigned long long)frame_number(), where, (const void *)t, g_traceProg,
                 g_work != nullptr ? "yes" : "no", logged == 20 ? " (no more of these lines)" : "");
}

void draw_bookkeep(const D3D11_RECT *area, ID3D11ShaderResourceView *dateSrv)
{
    BatchState &b = g_bs;
    if ((b.dateStencil || b.ds.date) && b.rt != nullptr && !b.depthFrame)
        b.rt->dateSinceFlat = true;         /* PC (2026-11-07): (the lazy mirror: no drop until the alpha is flat again) */
    if (b.wroteTop && b.rt != nullptr && !b.depthFrame)
        b.rt->execAlpha = -1;               /* PC (2026-11-06): (its alpha no longer known flat: mirror_sync) */
    /* PC: where (the scissor's part of the area) */
    D3D11_RECT w = b.scissor;
    if (area != nullptr) {
        w.left = std::max(w.left, area->left);
        w.top = std::max(w.top, area->top);
        w.right = std::min(w.right, area->right);
        w.bottom = std::min(w.bottom, area->bottom);
    }
    if (g_work != nullptr && g_runBookCur < g_runBook.size()) {
        /* PC: (a run's work: gathered, done once - RunBook) */
        RunBook &k = g_runBook[g_runBookCur];
        if (!k.any) {
            k.rt = b.rt;
            k.zt = b.zt;
            k.rect = w;
            k.depthFrame = b.depthFrame;
            k.any = true;
        } else if (w.right > w.left && w.bottom > w.top) {
            if (k.rect.right <= k.rect.left || k.rect.bottom <= k.rect.top) {
                k.rect = w;
            } else {
                k.rect.left = std::min(k.rect.left, w.left);
                k.rect.top = std::min(k.rect.top, w.top);
                k.rect.right = std::max(k.rect.right, w.right);
                k.rect.bottom = std::max(k.rect.bottom, w.bottom);
            }
        }
        k.rgb |= b.wroteRgb;
        k.top |= b.wroteTop;
        k.z |= b.wroteZ && !b.depthFrame;
        return;
    }
    if (b.depthFrame) {
        target_written_rect(*b.rt, w.left, w.top, w.right, w.bottom);
        registry_target_written(*b.rt, true, false);
        return;
    }
    if (b.wroteRgb || b.wroteTop) {
        target_written_rect(*b.rt, w.left, w.top, w.right, w.bottom);
        registry_target_written(*b.rt, b.wroteRgb, b.wroteTop);
        if (dateSrv != nullptr && b.ds.date) {
            /* a run of DATE draws goes on (g_dateRun; a C = Ad draw in the
             * shader alone does not start one) */
            g_dateRun.rt = b.rt;
            g_dateRun.gen = b.rt->gen;
        }
    }
    if (b.wroteZ) {
        /* the Z words now hold this frame's pixels (a Z buffer shared by
         * frames of two GS sizes: pgl_targets.cpp) */
        depth_target_written(*b.zt, *b.rt);
        b.zt->gen++;
        registry_target_written(*b.zt, true, false);
    }
}

/* Before a draw's record goes to the submit stage: the copy of the target
 * its C = Ad / DATE reads (dateSrv), the GPU section, the alpha mirror's
 * sync and whether the target keeps the mirror (mirrorRange: before the
 * sync, for the draw's alpha range, as the range was taken before it;
 * mirror: through the draw), its state if the stage has not got it, its
 * bookkeeping. */
void submit_prologue(const D3D11_RECT *area, bool indexed, ID3D11ShaderResourceView *&dateSrv, bool &mirrorRange,
                     bool &mirror, const MirrorIn &in, bool &noPass, bool post = true)
{
    BatchState &b = g_bs;
    mirrorRange = mirror_on(b);
    noPass = false;
    /* C = Ad in the shader reads Cd / Ad as the draws before this one left
     * them: a fresh copy (consecutive model parts share one draw state) */
    if ((b.ps.flags & PSF_BLEND_AD) != 0 ||
        (g_dateMode == DATE_EACH && b.ds.date && !b.depthFrame && b.dateSrv != nullptr))
        b.dateSrv = target_copy_srv(*b.rt, false, (b.ps.flags & PSF_BLEND_AD) != 0 ? COPY_AD : COPY_DATE);
                                                /* PC: (DATE_EACH: before every DATE draw, the reference) */
    dateSrv = b.dateSrv;
    /* PC: the GPU sections - within the scene, a VU1 draw textured from a
     * Tex1 set is a car's.  PC (2026-11-05): and a car's other draws stay
     * the car's - its env passes sample the env map (a target), its shadow
     * is untextured - instead of flipping back to the scene at each (two
     * marks a part: a 32-car frame ran out of marks, and FLARE / POST /
     * OTHER read 0): a VU1 draw not from a set keeps the section marked
     * last, a GS batch is the game's section */
    if (sec_active()) {
        int want = g_secGame;
        if (g_secGame == PGLR_SEC_SCENE && indexed) {
            if (b.tme && b.tex.fromSet)
                want = PGLR_SEC_CARS;
            else if (sec_last() == PGLR_SEC_CARS)
                want = PGLR_SEC_CARS;
        }
        sec_want(want);
    }
    /* PC (2026-11-04): the target's alpha mirror kept by this draw (its
     * stencil ops, a mirror pass).  PC (2026-11-05): kept only while it is
     * valid - a DATE draw that finds it invalid syncs it first from a copy
     * of the alpha, which holds every write made so far; the draws before
     * the frame's first DATE draw (the course, the sky) keep nothing (no
     * stencil ops, no mirror passes: the GPU's share of the mirror in a
     * frame with few DATE draws) */
    const bool dated = g_dateStencil && !b.depthFrame && b.rt->dated;
    if (dated && b.dateStencil && !b.rt->mirrorValid) {
        ID3D11DepthStencilView *mdsv = draw_dsv(true);
        if (mdsv != nullptr) {
            if (g_work != nullptr)
                run_work_cut();             /* PC: (the sync's copy must come after the run's draws so far) */
            mirror_sync(*b.rt, mdsv);
        }
    }
    if (indexed && b.tme && b.tex.target != nullptr) {
        if (stale_log_on())
            stale_check(b.tex.target, "submit_prologue's state");   /* PC (2026-11-18) */
        target_read_by_vu1(b.tex.target, g_traceProg);  /* PC (2026-11-06): ([Graphics] EnvMapDetail) */
    }
    mirror = dated && b.rt->mirrorValid;
    /* PC (2026-11-06): the lazy mirror (g_mirrorLazyOn) - a draw whose mirror
     * pass would cover an eighth of the target or more lets the mirror go
     * instead (it keeps nothing; the next DATE draw syncs).  Never a DATE
     * draw: it is one of a sequence (the sky's panels, a car's env pairs),
     * and the next one would sync the whole target again - the user's SSR5
     * capture (frame 10251) paid ten full syncs for the sky's ten panels,
     * 47 Mpx, where their one pass was 3.9.  No area known (a box around the
     * eye, the sky): the scissor, which the draw may fill. */
    if (mirror && g_mirrorLazyOn && !b.dateStencil && mirror_quiet(*b.rt) &&
        area_px(area != nullptr ? *area : b.scissor, b.scissor, b.rt->nw, b.rt->nh) >= mirror_drop_px(*b.rt)) {
        int lo, hi;
        mirror_in_range(in, lo, hi);
        if (mirror_needs_pass(b, lo, hi)) {
            target_mirror_invalidate(*b.rt);
            g_profCount[PCOUNT_MIRROR_DROP]++;
            mirror = false;
        }
    }
    if (post && !g_subPosted)               /* PC (2026-11-05): (not for a run's work: its states go with it, RunWork) */
        post_state();
    draw_bookkeep(area, dateSrv);
}

/* PC (2026-11-03): a VU1 batch joins the held merged draw (MergedDraw) -
 * true: done, nothing bound for it (its bookkeeping is the executor's,
 * draw_bookkeep, as every draw's).  Its constants (cb, bound for the draw's
 * first part), state (one compute_state), record (inst, but the texture's
 * values), vertex stage, cull, buffers and the pass's PS constants (but the
 * texture's) must be the draw's; its indices follow the draw's, its
 * vertices the last part's; mirror: the target keeps its alpha mirror. */
bool merge_join(const CbVu1 &cb, const Vu1Inst &inst, Vs vs, int cull, ID3D11Buffer *vb, ID3D11Buffer *ib,
                uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t batch, bool mirror, bool dbl)
{
    MergedDraw &m = g_merged;
    const SubState &s = g_sub;
    const BatchState &b = s.bs;
    /* PC (2026-11-04): why not, counted (the profile's MJ counts) */
    if (!q::t_holder || q::g_held != &merged_queue || !m.mergeable || g_tracing || flicker_on() || m.doubled != dbl) {
        g_profCount[PCOUNT_MJ_NONE]++;
        cap::t_mergeWhy = cap::MW_NONE;     /* PC (2026-11-06): (the frame capture: why the part begins a draw) */
        return false;
    }
    if (m.serial != s.serial) {
        g_profCount[PCOUNT_MJ_STATE]++;
        cap::t_mergeWhy = cap::MW_STATE;
        return false;
    }
    if (m.vs != vs || m.cull != cull) {
        g_profCount[PCOUNT_MJ_VS]++;
        cap::t_mergeWhy = cap::MW_VS;
        return false;
    }
    if (m.vb != vb || m.ib != ib || baseVertex != 0) {
        g_profCount[PCOUNT_MJ_BUF]++;
        cap::t_mergeWhy = cap::MW_BUF;
        return false;
    }
    if (m.first + m.count != firstIndex) {
        g_profCount[PCOUNT_MJ_INDEX]++;
        cap::t_mergeWhy = cap::MW_INDEX;
        return false;
    }
    if (batch == ~0u || batch <= m.lastBatch || batch - m.firstBatch >= kMergeParts) {
        g_profCount[PCOUNT_MJ_BATCH]++;
        cap::t_mergeWhy = cap::MW_BATCH;
        return false;
    }
    if (m.flat != inst.flat || memcmp(m.zFromW, inst.zFromW, sizeof(m.zFromW)) != 0) {
        g_profCount[PCOUNT_MJ_REC]++;
        cap::t_mergeWhy = cap::MW_REC;
        return false;
    }
    /* the pass's constants as draw_passes binds them, the texture's aside */
    const gs::Pass &pass = b.ds.pass[0];
    CbPs c = b.ps;
    if (pass.gate != gs::AlphaGate::None)
        c.flags |= ((uint32_t)pass.gate << PSF_GATE_SHIFT) | ((uint32_t)b.ds.alphaFunc << PSF_ATST_SHIFT);
    c.flags |= PSF_INST_RECT;
    if (dbl)
        c.flags |= PSF_AD_INST;             /* PC (2026-11-06): (as draw_passes binds a doubled draw's) */
    c.texX0 = c.texY0 = c.texInvW = c.texInvH = 0.0f;
    /* PC (2026-11-04): the same stencil op and mirror pass (the alpha mirror:
     * a part's own texture may make its alpha's bit 7 vary; the plan of the
     * part before kept while its inputs are the same).  PC (2026-11-05): and
     * the pass folded into the draw the same way (its flags in the constants,
     * compared below with them) */
    MirrorPlan plan;
    if (mirror) {
        struct Last {
            uint32_t serial = ~0u;
            int vLo = -1, vHi = -1, tLo = -1, tHi = -1;
            uint32_t flags = 0;
            MirrorPlan plan;
        };
        static thread_local Last last;      /* PC (2026-11-05): (per thread: the submit workers) */
        const Target *tt = b.tex.target != nullptr && !b.tex.depthTex ? b.tex.target : nullptr;
        const int tLo = b.tex.hasAlpha ? b.tex.aMin : tt != nullptr ? tt->alphaLo - 300 : -1;
        const int tHi = b.tex.hasAlpha ? b.tex.aMax : tt != nullptr ? tt->alphaHi - 300 : -1;
        if (last.serial != s.serial || last.vLo != g_vLo || last.vHi != g_vHi || last.tLo != tLo ||
            last.tHi != tHi || last.flags != b.ps.flags) {
            last.serial = s.serial;
            last.vLo = g_vLo;
            last.vHi = g_vHi;
            last.tLo = tLo;
            last.tHi = tHi;
            last.flags = b.ps.flags;
            last.plan = mirror_plan(s, pass, pass.writeMask & b.ds.frameWriteMask, g_vLo, g_vHi);
        }
        plan = last.plan;
        if (mirror_fold_ok(s, plan))
            c.flags |= mirror_fold_flags(s, plan);
    }
    const uint32_t texFlags = c.flags & kPsTexFlags;
    c.flags &= ~kPsTexFlags;
    if (memcmp(&c, &m.ps, sizeof(c)) != 0) {
        g_profCount[PCOUNT_MJ_PS]++;
        cap::t_mergeWhy = cap::MW_PS;
        return false;
    }
    if (mirror) {
        const MirrorPlan &p = plan;
        if (p.op != m.plan.op || p.ref != m.plan.ref || p.pass != m.plan.pass || p.keep1 != m.plan.keep1) {
            g_profCount[PCOUNT_MJ_PS]++;
            cap::t_mergeWhy = cap::MW_PS;
            return false;
        }
    }
    uint32_t slot = 0;
    if (b.tme) {
        if (!tex_joinable(s, dbl)) {
            g_profCount[PCOUNT_MJ_NONE]++;
            cap::t_mergeWhy = cap::MW_TARGET_TEX;
            return false;
        }
        while (slot < m.nSlots && !(m.srv[slot] == b.tex.srv && m.samp[slot] == b.samp))
            slot++;
        if (slot == m.nSlots && m.nSlots >= kMergeSlots) {
            g_profCount[PCOUNT_MJ_SLOTS]++;
            cap::t_mergeWhy = cap::MW_SLOTS;
            return false;
        }
    }
    /* (PC, 2026-11-04: the constants but the lighting, which the records
     * carry - the whole block without the lights' ring) */
    if (!vs_cb_bound(&cb, light_supported() ? kCbVu1Head : sizeof(cb))) {
        g_profCount[PCOUNT_MJ_CB]++;
        cap::t_mergeWhy = cap::MW_CB;
        return false;
    }
    if (b.tme && slot == m.nSlots) {
        m.srv[slot] = b.tex.srv;
        m.samp[slot] = b.samp;
        m.nSlots++;
    }
    merged_part(m, slot, batch, texFlags, inst);
    m.count += indexCount;
    m.capParts++;                           /* PC (2026-11-06): (the frame capture's part count) */
    g_profCount[PCOUNT_MERGED]++;
    t_stats->verts += indexCount;
    return true;
}

/* (the submit stage) A GS batch's draw (SubGs): its vertex stage, then its
 * passes. */
/* PC (2026-11-06): the batch as the clears it is (SubGs::fast; the executor
 * decided, flush_batch): the colour as ps_body writes a flat untextured
 * sprite's (c / 255, the alpha with FBA; the same UNORM bytes), the depth as
 * vs_gs passes a vertex's (one value over the sprite), the alpha mirror's bit
 * where the draw's Replace would set it; its alpha bound as draw_passes
 * keeps it.  False: a view is missing - drawn after all. */
bool fast_clear(const SubGs &d)
{
    const SubState &s = g_sub;
    const BatchState &b = s.bs;
    if (((d.fast & FAST_COLOR) != 0 && s.rtv == nullptr) ||
        ((d.fast & (FAST_DEPTH | FAST_STENCIL)) != 0 && s.dsv == nullptr))
        return false;
    const gs::Pass &p0 = b.ds.pass[0];
    const uint8_t mask = p0.writeMask & b.ds.frameWriteMask;
    auto rec = [&](const char *note) -> uint32_t {
        if (!cap::on())
            return ~0u;
        cap::Draw c;
        c.kind = cap::K_CLEAR;
        c.target = s.rtAlpha != nullptr ? s.rtAlpha->base : 0;
        c.targetW = (uint16_t)s.nw;
        c.targetH = (uint16_t)s.nh;
        c.writeMask = mask;
        c.zWrite = (d.fast & FAST_DEPTH) != 0;
        c.count = d.count;
        c.stencilRef = d.mirrorNote;        /* (the capture's stencil_ref column: SubGs::mirrorNote's bits) */
        c.mirrorPass = d.mirror ? 1 : 0;
        c.note = note;
        c.x0 = c.y0 = 0;
        c.x1 = (int16_t)std::min(s.nw, 32767);
        c.y1 = (int16_t)std::min(s.nh, 32767);
        return cap::draw_begin(c);
    };
    if (d.fast & FAST_COLOR) {
        ID3D11RenderTargetView *rtv = s.rtv;
        float col[4];
        memcpy(col, d.fastColor, sizeof(col));
        const uint32_t capId = rec("fast clear: the colour (a covering flat sprite)");
        q::run([rtv, col, capId] {
            cap::gpu_begin(capId);
            qdc()->ClearRenderTargetView(rtv, col);
            cap::gpu_end(capId);
        });
    }
    if (d.fast & (FAST_DEPTH | FAST_STENCIL)) {
        ID3D11DepthStencilView *dsv = s.dsv;
        const UINT flags = ((d.fast & FAST_DEPTH) ? D3D11_CLEAR_DEPTH : 0u) | ((d.fast & FAST_STENCIL) ? D3D11_CLEAR_STENCIL : 0u);
        const float z = d.fastZ;
        const UINT8 st = d.fastStencil;
        const uint32_t capId = rec((d.fast & FAST_DEPTH) == 0 ? "fast clear: the alpha mirror's bit (a covering flat sprite)"
                                   : (d.fast & FAST_STENCIL) == 0 ? "fast clear: the depth (a covering flat sprite)"
                                                                  : "fast clear: the depth and the alpha mirror's bit (a covering flat sprite)");
        q::run([dsv, flags, z, st, capId] {
            cap::gpu_begin(capId);
            qdc()->ClearDepthStencilView(dsv, flags, z, st);
            cap::gpu_end(capId);
        });
    }
    if (d.fast == FAST_SKIP)
        rec("fast clear: nothing to write (the alpha is there already)");
    /* the target's alpha bound (as draw_passes keeps it for the draw) */
    if (!b.depthFrame && s.rtAlpha != nullptr && (mask & gs::WRITE_A) != 0 && (b.ds.fbmsk & 0x80000000u) == 0)
        target_alpha_written(*s.rtAlpha, b, p0,
                             d.fullCover && b.ds.passCount == 1 && p0.gate == gs::AlphaGate::None && !b.ds.date &&
                                 !b.dateStencil && !depth_gates(b, s.hasZ) &&
                                 ((b.ps.flags >> PSF_SCAN_SHIFT) & 3) < 2);
    g_profCount[PCOUNT_FAST_CLEAR]++;
    return true;
}

void sub_gs(const SubGs &d)
{
    const SubState &s = g_sub;
    g_vLo = d.vLo;                          /* (the alpha mirror: mirror_plan) */
    g_vHi = d.vHi;
    if (d.fast != 0 && fast_clear(d))       /* PC (2026-11-06): (the clears it is, not drawn) */
        return;
    set_vb(d.vb, sizeof(GsVtx));
    set_topology(s.bs.topo);
    bind_vs(Vs::Gs);
    CbGs cb = {(float)s.nw, (float)s.nh, 0, 0};
    bind_vs_cb(&cb, sizeof(cb));
    cap::t_note = d.drawnWhy;               /* PC (2026-11-07): (the frame capture: a covering batch drawn, why) */
    draw_passes(false, d.count, d.first, 0, 0, &d.area, 0, d.mirror, d.dateSrv, d.fullCover, d.noPass, false);
    cap::t_note = nullptr;
}

/* PC (2026-11-05): the batch's sprites rewrite every pixel of the target (a
 * clear, a whole copy; the target's alpha bound, target_alpha_written): a
 * sprite batch of up to kCoverSprites sprites, the scissor and the target
 * single-sampled, whose rectangles cover every pixel centre of 0..nw x 0..nh
 * - a sprite from x0 to x1 covers the columns x0 <= i + 0.5 < x1 (the
 * top-left rule), checked on the grid of the rectangles' edges.  PC
 * (2026-11-07): 32 sprites - the game's Z clears are 20 tiles (the user's
 * captures), which the limit of 16 kept drawn, 4.7 Mpx each at 150 %. */
constexpr size_t kCoverSprites = 32;
bool batch_covers_target()
{
    const BatchState &b = g_bs;
    if (b.type != (uint32_t)gs::PrimType::Sprite || b.rt == nullptr || b.depthFrame || b.rt->samples > 1)
        return false;
    const size_t n = g_verts.size() / 6;
    if (n == 0 || n > kCoverSprites || g_verts.size() % 6 != 0)
        return false;
    const int W = b.rt->nw, H = b.rt->nh;
    if (b.scissor.left > 0 || b.scissor.top > 0 || b.scissor.right < W || b.scissor.bottom < H)
        return false;
    struct R { int x0, y0, x1, y1; } r[kCoverSprites];
    int xs[2 * kCoverSprites + 2], ys[2 * kCoverSprites + 2], nx = 0, ny = 0;
    for (size_t i = 0; i < n; i++) {
        const GsVtx *v = &g_verts[i * 6];
        float fx0 = v[0].x, fx1 = fx0, fy0 = v[0].y, fy1 = fy0;
        for (int k = 1; k < 6; k++) {
            fx0 = std::min(fx0, v[k].x);
            fx1 = std::max(fx1, v[k].x);
            fy0 = std::min(fy0, v[k].y);
            fy1 = std::max(fy1, v[k].y);
        }
        r[i].x0 = (int)std::ceil(fx0 - 0.5f);
        r[i].x1 = (int)std::ceil(fx1 - 0.5f);
        r[i].y0 = (int)std::ceil(fy0 - 0.5f);
        r[i].y1 = (int)std::ceil(fy1 - 0.5f);
        xs[nx++] = std::max(0, std::min(W, r[i].x0));
        xs[nx++] = std::max(0, std::min(W, r[i].x1));
        ys[ny++] = std::max(0, std::min(H, r[i].y0));
        ys[ny++] = std::max(0, std::min(H, r[i].y1));
    }
    xs[nx++] = 0;
    xs[nx++] = W;
    ys[ny++] = 0;
    ys[ny++] = H;
    std::sort(xs, xs + nx);
    std::sort(ys, ys + ny);
    nx = (int)(std::unique(xs, xs + nx) - xs);
    ny = (int)(std::unique(ys, ys + ny) - ys);
    for (int j = 0; j + 1 < ny; j++) {
        for (int i = 0; i + 1 < nx; i++) {
            bool covered = false;
            for (size_t k = 0; k < n && !covered; k++)
                covered = r[k].x0 <= xs[i] && r[k].x1 >= xs[i + 1] && r[k].y0 <= ys[j] && r[k].y1 >= ys[j + 1];
            if (!covered)
                return false;
        }
    }
    return true;
}

}  /* namespace */

void flush_batch()
{
    if (g_verts.empty())
        return;
    ProfScope prof(PROF_FLUSH);         /* (the verbose monitor's profile) */
    g_profCount[PCOUNT_BATCHES]++;
    g_profCount[PCOUNT_GSVERTS] += (uint32_t)g_verts.size();
    if (g_fbPending)
        fb_segment_end();               /* (drawn now: still to be seen by what reads the copy) */
    g_segStart = 0;
    /* PC: the vertices' bounds (native pixels; a pixel more around for the
     * points' and lines' width and the edges' rounding) */
    float x0 = g_verts[0].x, y0 = g_verts[0].y, x1 = x0, y1 = y0;
    for (const GsVtx &v : g_verts) {
        x0 = std::min(x0, v.x);
        y0 = std::min(y0, v.y);
        x1 = std::max(x1, v.x);
        y1 = std::max(y1, v.y);
    }
    SubGs d;
    d.area = {(LONG)std::floor(x0) - 2, (LONG)std::floor(y0) - 2, (LONG)std::ceil(x1) + 2, (LONG)std::ceil(y1) + 2};
    if (!g_bs.valid || g_bs.skip) {
        if (flicker_on())
            flicker_note(2, (uint32_t)g_verts.size(), &d.area, fnv(g_verts.data(), g_verts.size() * sizeof(GsVtx)));
        g_verts.clear();
        g_held.clear();
        return;
    }
    d.count = (uint32_t)g_verts.size();
    d.first = stream_vertices(g_verts.data(), sizeof(GsVtx), d.count, &d.vb);
    /* PC (2026-11-04): the vertices' alpha range (the alpha mirror: mirror_plan;
     * GT3PC_PGL_ALPHA_LOG's check; PC, 2026-11-05: and the target's alpha
     * bound, target_alpha_written - always, then) */
    bool flatColor = true, flatZ = true;    /* PC (2026-11-06): (one colour, one depth: the fast clears) */
    {
        int lo = 255, hi = 0;
        const uint32_t rgba0 = g_verts[0].rgba;
        const float z0 = g_verts[0].z;
        for (const GsVtx &v : g_verts) {
            const int a = (int)(v.rgba >> 24);
            lo = std::min(lo, a);
            hi = std::max(hi, a);
            flatColor &= v.rgba == rgba0;
            flatZ &= v.z == z0;
        }
        d.vLo = lo;
        d.vHi = hi;
    }
    const bool coversAll = batch_covers_target();
    const bool covers = g_bs.wroteTop && coversAll;
    d.fullCover = g_alphaBoundOn && covers;   /* PC (2026-11-05) */
    d.fast = 0;
    d.drawnWhy = nullptr;
    if (!draw_skipped(false, -1)) {
        /* PC (2026-11-05): the flare queries waiting for their batch go first,
         * unless this draw is one of their own sprites (flare_queries_flush) */
        if (flare_queries_pending() && !flare_sprite_ok())
            flare_queries_flush();
        /* PC (2026-11-05): the executor's half, then the record for the
         * submit stage (sub_gs: at once when the frame does not pipeline) */
        bool mirrorRange;
        MirrorIn in;
        in.vLo = d.vLo;
        in.vHi = d.vHi;
        const int alphaBefore = g_bs.rt != nullptr ? g_bs.rt->execAlpha : -1;   /* PC (2026-11-06): (before this batch) */
        d.mirrorNote = (uint8_t)(g_bs.rt != nullptr && g_bs.rt->mirrorValid ? 2 : 0);
        submit_prologue(&d.area, false, d.dateSrv, mirrorRange, d.mirror, in, d.noPass);
        if (g_bs.rt != nullptr)
            d.mirrorNote |= (uint8_t)((g_bs.rt->dated ? 1 : 0) | (g_bs.rt->mirrorValid ? 4 : 0));
        /* PC (2026-11-06): a flat alpha written over the whole target with
         * nothing gating a pixel (a clear, the alpha-only clears): the
         * executor's note of it (Target::execAlpha: a DATE draw's sync is a
         * stencil clear then), and the mirror valid again by the batch's
         * own stencil writes when it was not (the lazy mirror) */
        if (covers && g_bs.rt != nullptr) {
            const BatchState &b = g_bs;
            const gs::Pass &p0 = b.ds.pass[0];
            const bool flat = b.ds.passCount == 1 && p0.gate == gs::AlphaGate::None && !b.ds.date && !b.dateStencil &&
                              !depth_gates(b, b.zt != nullptr) && ((b.ps.flags >> PSF_SCAN_SHIFT) & 3) < 2 &&
                              !b.tme && d.vLo == d.vHi && (b.ds.fbmsk & 0xFF000000u) == 0 &&
                              (p0.writeMask & b.ds.frameWriteMask & gs::WRITE_A) != 0;
            if (flat) {
                d.mirrorNote |= 16;
                if (g_mirrorLazyOn && g_dateStencil && b.rt->dated && !b.rt->mirrorValid && !d.mirror &&
                    draw_dsv(true) != nullptr) {
                    d.mirror = true;
                    d.noPass = false;
                    d.mirrorNote |= 8;
                    b.rt->mirrorValid = true;
                    g_profCount[PCOUNT_MIRROR_REVAL]++;
                }
                b.rt->execAlpha = (b.ps.flags & PSF_FBA) ? (d.vLo | 0x80) : d.vLo;
                b.rt->dateSinceFlat = false;    /* PC (2026-11-07): (the DATE sequence before it is over: mirror_quiet) */
            }
        }
        /* PC (2026-11-06): the fast clears (g_fastClearOn, fast_clear) - the
         * batch covers the target with one colour and one depth, nothing
         * gates a pixel (no test, DATE, scan mask, blend, fog, texture or
         * colour scaling), a 32-bit frame whose FBMSK masks whole bytes: what
         * it writes goes as clears.  The colour needs every channel, or RGB
         * with the alpha the target holds everywhere (Target::execAlpha);
         * the alpha alone only when the target holds it already; the depth
         * with no depth test (or ALWAYS).  Anything else: drawn. */
        /* PC (2026-11-07): and why a covering batch is drawn after all: the
         * frame capture's note of it (SubGs::drawnWhy, sub_gs) */
        const char *why = nullptr;
        if (g_fastClearOn && coversAll && g_bs.rt != nullptr && !flicker_on() && !g_tracing) {
            const BatchState &b = g_bs;
            const gs::Pass &p0 = b.ds.pass[0];
            const uint32_t fl = b.ps.flags;
            const uint8_t mask = p0.writeMask & b.ds.frameWriteMask;
            bool maskOk = true;
            for (int k = 0; k < 4; k++) {
                const uint32_t v = (b.ds.fbmsk >> (8 * k)) & 0xFFu;
                maskOk &= v == 0 || v == 0xFFu;
            }
            /* (a sprite writing no colour - the depth alone, mask 0 - may be
             * textured, blended or of any colours: nothing of them reaches
             * the target; the user's SSR5 capture draws its Z clears so, 20
             * textured tiles a frame twice) */
            const bool colourFree = mask == 0;
            const bool colourOk = colourFree || (flatColor && !b.tme && !b.ds.blend.enable &&
                                                 (fl & (PSF_FGE | PSF_BLEND_AD | (7u << PSF_PREMUL_SHIFT))) == 0 &&
                                                 b.ds.fpsm == 0);
            if (b.ds.passCount != 1)
                why = "drawn: two passes";
            else if (p0.gate != gs::AlphaGate::None)
                why = "drawn: the alpha test";
            else if (b.ds.date || b.dateStencil)
                why = "drawn: DATE";
            else if (((fl >> PSF_SCAN_SHIFT) & 3) >= 2)
                why = "drawn: a scan mask";
            else if (!colourOk)
                why = "drawn: the colour (textured, blended, fogged, scaled or not flat)";
            else if (!maskOk)
                why = "drawn: FBMSK within a byte";
            else {
                const bool zw = p0.depthWrite && b.zt != nullptr;
                const uint32_t rgba = g_verts[0].rgba;
                int a = (int)(rgba >> 24);
                if ((fl & PSF_FBA) && a < 128)
                    a += 128;                   /* (ps_body's aOut) */
                uint8_t fast = 0;
                if ((mask & gs::WRITE_RGB) == gs::WRITE_RGB) {
                    const int ca = (mask & gs::WRITE_A) != 0 ? a : alphaBefore;
                    if (ca >= 0) {
                        fast |= FAST_COLOR;
                        d.fastColor[0] = (float)(rgba & 0xFFu) / 255.0f;
                        d.fastColor[1] = (float)((rgba >> 8) & 0xFFu) / 255.0f;
                        d.fastColor[2] = (float)((rgba >> 16) & 0xFFu) / 255.0f;
                        d.fastColor[3] = (float)ca / 255.0f;
                    } else {
                        why = "drawn: RGB with the target's alpha unknown";
                    }
                } else if (mask == gs::WRITE_A) {
                    if (alphaBefore != a)       /* (there already: nothing to write) */
                        why = "drawn: a new alpha over the target (an alpha-only clear)";
                } else if (mask != 0) {
                    why = "drawn: a subset of the channels";
                }
                if (zw) {
                    if (flatZ && (!b.ds.depthTest || b.ds.depthFunc == gs::Compare::Always)) {
                        fast |= FAST_DEPTH;
                        d.fastZ = g_verts[0].z;
                    } else if (why == nullptr) {
                        why = flatZ ? "drawn: the depth tested" : "drawn: the depth varies";
                    }
                }
                if (why == nullptr && d.mirror && (mask & gs::WRITE_A) != 0) {
                    fast |= FAST_STENCIL;       /* (the mirror's bit: the draw's Replace) */
                    d.fastStencil = a >= 128 ? kMirrorBit : 0;
                }
                if (why == nullptr)
                    d.fast = fast != 0 ? fast : FAST_SKIP;
            }
        } else if (coversAll && g_bs.rt != nullptr) {
            why = !g_fastClearOn ? "drawn: GT3PC_PGL_FAST_CLEAR=0" : "drawn: a traced frame";
        }
        d.drawnWhy = why;
        sq::run([d] { sub_gs(d); });
        if (g_bs.wroteTop && g_bs.rt != nullptr)
            g_bs.rt->alphaPos = sq::head(); /* PC: (its alpha bound written here: the submit workers wait for it) */
        if (g_tracing && !g_bs.depthFrame && (g_bs.wroteRgb || g_bs.wroteTop))
            probe_draw(*g_bs.rt);       /* (GT3PC_PGL_PROBE: after the draw) */
    }
    g_verts.clear();
    g_held.clear();
}

namespace {

/* ======================================================================== */
/* GS-space primitives                                                      */
/* ======================================================================== */

GsVtx convert(const Vtx &v)
{
    const BatchState &b = g_bs;
    GsVtx o;
    const float wx = (float)((int32_t)v.x - (int32_t)b.ofx) * 0.0625f;
    const float wy = (float)((int32_t)v.y - (int32_t)b.ofy) * 0.0625f;
    o.x = wx * b.sx + b.pix;
    o.y = wy * b.sy + b.pix;
    o.z = pr::depthOf(v.z, b.zpsm);
    if (!b.tme) {
        o.u = o.v = 0;
        o.q = 1;
    } else if (b.fst) {
        o.u = (float)v.u * 0.0625f;
        o.v = (float)v.v * 0.0625f;
        o.q = 1;
    } else {
        o.u = v.s * b.texW;
        o.v = v.t * b.texH;
        o.q = v.q;
    }
    o.rgba = v.rgba;
    o.fog = (float)v.f;
    return o;
}

/* PC: a primitive after a TEXFLUSH (g_fbPending): where it reads its own
 * target (its texture coordinates in the target's native pixels, two more
 * around for the filter) - if that meets what was drawn since the copy
 * (g_fbRect), the batch is drawn and the state taken again, the copy
 * refreshed there, before it joins. */
void feedback_check(const Vtx *v, int n)
{
    if (!g_fbPending)
        return;
    const BatchState &b = g_bs;
    if (!b.tme || b.tex.target == nullptr || b.tex.target != b.rt || b.tex.w <= 0.0f || b.tex.h <= 0.0f) {
        g_fbPending = false;
        return;
    }
    const Target &t = *b.rt;
    const float kx = (float)t.nw / b.tex.w, ky = (float)t.nh / b.tex.h;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < n; i++) {
        float tu, tv;
        if (b.fst) {
            tu = (float)v[i].u * 0.0625f;
            tv = (float)v[i].v * 0.0625f;
        } else {
            const float q = v[i].q != 0.0f ? v[i].q : 1.0f;
            tu = v[i].s * b.texW / q;
            tv = v[i].t * b.texH / q;
        }
        const float px = (tu - b.tex.x0) * kx, py = (tv - b.tex.y0) * ky;
        x0 = std::min(x0, px);
        y0 = std::min(y0, py);
        x1 = std::max(x1, px);
        y1 = std::max(y1, py);
    }
    const float l = std::floor(x0) - 2.0f, tp = std::floor(y0) - 2.0f;
    const float r = std::ceil(x1) + 2.0f, bt = std::ceil(y1) + 2.0f;
    bool meets = false;
    for (const D3D11_RECT &w : g_fbRects) {
        if (r > (float)w.left && l < (float)w.right && bt > (float)w.top && tp < (float)w.bottom) {
            meets = true;
            break;
        }
    }
    if (!meets)
        return;                         /* the copy holds what it reads */
    flush_batch();
    g_dirty = true;
    ensure_state();                     /* the copy refreshed where written (g_fbPending cleared) */
}

void emit_point(const Vtx &k)
{
    ensure_state();
    feedback_check(&k, 1);
    if (g_bs.skip)
        return;
    g_verts.push_back(convert(k));
}

void emit_line(const Vtx &a, const Vtx &k)
{
    ensure_state();
    const Vtx pv[2] = {a, k};
    feedback_check(pv, 2);
    if (g_bs.skip)
        return;
    if (g_lineWidth <= 1.0f) {
        g_verts.push_back(convert(k));
        g_verts.push_back(convert(a));
        return;
    }
    /* a wide line (CMD_LINE_WIDTH): a quad g_lineWidth native pixels across,
     * its ends squared off half that beyond the points (a strip's joints
     * covered) */
    const GsVtx A = convert(a), B = convert(k);
    float dx = B.x - A.x, dy = B.y - A.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-4f) {
        dx /= len;
        dy /= len;
    } else {
        dx = 1.0f;
        dy = 0.0f;
    }
    const float h = g_lineWidth * 0.5f;
    const float nx = -dy * h, ny = dx * h, ex = dx * h, ey = dy * h;
    GsVtx c[4] = {A, A, B, B};
    c[0].x = A.x - ex + nx; c[0].y = A.y - ey + ny;
    c[1].x = A.x - ex - nx; c[1].y = A.y - ey - ny;
    c[2].x = B.x + ex + nx; c[2].y = B.y + ey + ny;
    c[3].x = B.x + ex - nx; c[3].y = B.y + ey - ny;
    g_verts.push_back(c[0]);
    g_verts.push_back(c[2]);
    g_verts.push_back(c[1]);
    g_verts.push_back(c[2]);
    g_verts.push_back(c[3]);
    g_verts.push_back(c[1]);
}

void emit_tri(const Vtx &k, const Vtx &a, const Vtx &b)
{
    ensure_state();
    const Vtx pv[3] = {k, a, b};
    feedback_check(pv, 3);
    if (g_bs.skip)
        return;
    g_verts.push_back(convert(k));
    g_verts.push_back(convert(a));
    g_verts.push_back(convert(b));
}

void emit_sprite(const Vtx &a, const Vtx &k)
{
    ensure_state();
    const Vtx pv[2] = {a, k};
    feedback_check(pv, 2);
    if (g_bs.skip)
        return;
    GsVtx A = convert(a), B = convert(k);
    if (g_bs.tme && !g_bs.fst) {
        /* STQ sprites: S / Q per corner */
        A.u /= (A.q != 0 ? A.q : 1);
        A.v /= (A.q != 0 ? A.q : 1);
        B.u /= (B.q != 0 ? B.q : 1);
        B.v /= (B.q != 0 ? B.q : 1);
        A.q = B.q = 1;
    }
    /* the corners as given (before the texel centre shift): the held
     * sprites' meeting edges below */
    const HeldSprite self = {g_verts.size(),
                             std::min(A.x, B.x), std::max(A.x, B.x), std::min(A.y, B.y), std::max(A.y, B.y),
                             std::min(A.u, B.u), std::max(A.u, B.u), std::min(A.v, B.v), std::max(A.v, B.v)};
    if (g_bs.tme) {
        /* keep the texel centres of a 1:1 mapping (see the file comment) -
         * in design pixels for a design-squeezed texture (CMD_DESIGN: a GS
         * pixel is 1 / k of one; slope x (pix / s - 0.5 k)); a render
         * target's copy stays in GS pixels (1:1 to its native texels) */
        const bool design = g_bs.tex.target == nullptr;
        const float kx = design ? g_designX : 1.0f;
        const float ky = design ? g_designY : 1.0f;
        const float dxw = (float)((int32_t)k.x - (int32_t)a.x) * 0.0625f;
        const float dyw = (float)((int32_t)k.y - (int32_t)a.y) * 0.0625f;
        /* a render target's copy: its samples held inside the texels of the
         * GS's own first and last samples (the first pixel's - the sprite's
         * corner coordinate - and the last one's, a pixel's slope short of
         * the far corner), on the inner side where one lies on a texel
         * boundary.  The game samples at texel corners for its blurs (the
         * bilinear filter averages two rows / columns), so its last sample
         * took half of the texel beyond the picture it copies - in the menus'
         * blur pyramid (the levels side by side in one buffer) half of the
         * next level's sky: a line along the screen's bottom, the far edge's
         * colours along the right (user report; the PS2 drew it too, in the
         * TV's overscan).  The native pixels outside the GS's samples read
         * further still.  Not an upload's: a glyph or an image fades into its
         * transparent border as before (held, text lost its soft edges: user
         * report) */
        const bool hold = g_bs.tex.target != nullptr;
        auto inside = [](float first, float last, float *lo, float *hi) {
            *lo = std::floor(std::min(first, last)) + 0.5f;
            *hi = std::ceil(std::max(first, last)) - 0.5f;
            if (*hi < *lo)
                *hi = *lo;                  /* (one sample, on a boundary) */
        };
        if (dxw != 0) {
            const float slope = (B.u - A.u) / dxw;
            const float cu = slope * (g_bs.pix / g_bs.sx - 0.5f * kx);
            if (hold)
                inside(A.u, B.u - slope * kx, &B.tu0, &B.tu1);
            A.u += cu;
            B.u += cu;
        }
        if (dyw != 0) {
            const float slope = (B.v - A.v) / dyw;
            const float cv = slope * (g_bs.pix / g_bs.sy - 0.5f * ky);
            if (hold)
                inside(A.v, B.v - slope * ky, &B.tv0, &B.tv1);
            A.v += cv;
            B.v += cv;
        }
        /* Sprites that tile one picture (meeting on the screen where their
         * texture ranges meet: the wet courses' blur, 60 tiles of a 64 x 128
         * copy over the 640 x 448 view) are not held on the edge they share:
         * there the GS filters across from one tile into the next; held, each
         * tile's edge rows stopped at its own edge texels, a step of up to
         * half a texel - five pixels at that zoom - at every tile edge, a
         * grid of blocks crawling with the picture (user report, Route 5
         * wet).  Their outer edges stay held. */
        if (hold) {
            const float e = 1e-3f;
            for (HeldSprite &p : g_held) {
                const bool rows = std::min(p.y1, self.y1) - std::max(p.y0, self.y0) > e &&
                                  std::min(p.v1, self.v1) - std::max(p.v0, self.v0) > e;
                const bool cols = std::min(p.x1, self.x1) - std::max(p.x0, self.x0) > e &&
                                  std::min(p.u1, self.u1) - std::max(p.u0, self.u0) > e;
                const bool uUp = (B.u >= A.u) == (B.x >= A.x), vUp = (B.v >= A.v) == (B.y >= A.y);
                for (int side = 0; side < 2; side++) {
                    /* side 0: p before this one (left / above), 1: after */
                    const HeldSprite &l = side == 0 ? p : self, &r = side == 0 ? self : p;
                    if (rows && std::fabs(l.x1 - r.x0) < e &&
                        std::fabs(uUp ? l.u1 - r.u0 : r.u1 - l.u0) < e) {
                        const bool selfLow = (side == 0) == uUp;   /* this sprite's low u edge is the shared one */
                        (selfLow ? B.tu0 : B.tu1) = selfLow ? -1e30f : 1e30f;
                        for (size_t i = p.at; i < p.at + 6 && i < g_verts.size(); i++)
                            (selfLow ? g_verts[i].tu1 : g_verts[i].tu0) = selfLow ? 1e30f : -1e30f;
                    }
                    if (cols && std::fabs(l.y1 - r.y0) < e &&
                        std::fabs(vUp ? l.v1 - r.v0 : r.v1 - l.v0) < e) {
                        const bool selfLow = (side == 0) == vUp;
                        (selfLow ? B.tv0 : B.tv1) = selfLow ? -1e30f : 1e30f;
                        for (size_t i = p.at; i < p.at + 6 && i < g_verts.size(); i++)
                            (selfLow ? g_verts[i].tv1 : g_verts[i].tv0) = selfLow ? 1e30f : -1e30f;
                    }
                }
            }
            g_held.push_back(self);
        }
    }
    GsVtx c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = B;
        c[i].x = (i & 1) ? B.x : A.x;
        c[i].u = (i & 1) ? B.u : A.u;
        c[i].y = (i & 2) ? B.y : A.y;
        c[i].v = (i & 2) ? B.v : A.v;
    }
    g_verts.push_back(c[0]);
    g_verts.push_back(c[1]);
    g_verts.push_back(c[2]);
    g_verts.push_back(c[1]);
    g_verts.push_back(c[3]);
    g_verts.push_back(c[2]);
}

/* The GS vertex queue: a vertex arrives (kick: XYZ2 / XYZF2). */
void vertex(const Vtx &v, bool kick)
{
    switch (g.prim & 7) {
    case 0:     /* POINT */
        if (kick)
            emit_point(v);
        break;
    case 1:     /* LINE */
        g.queue[g.count++] = v;
        if (g.count == 2) {
            if (kick)
                emit_line(g.queue[0], v);
            g.count = 0;
        }
        break;
    case 2:     /* LINE_STRIP */
        if (g.count == 0) {
            g.queue[0] = v;
            g.count = 1;
        } else {
            if (kick)
                emit_line(g.queue[0], v);
            g.queue[0] = v;
        }
        break;
    case 3:     /* TRIANGLE */
        if (g.count < 2) {
            g.queue[g.count++] = v;
        } else {
            if (kick)
                emit_tri(v, g.queue[0], g.queue[1]);
            g.count = 0;
        }
        break;
    case 4:     /* TRI_STRIP */
        if (g.count < 2) {
            g.queue[g.count++] = v;
        } else {
            if (kick)
                emit_tri(v, g.queue[0], g.queue[1]);
            g.queue[0] = g.queue[1];
            g.queue[1] = v;
        }
        break;
    case 5:     /* TRI_FAN */
        if (g.count == 0) {
            g.fan0 = v;
            g.count = 1;
        } else if (g.count == 1) {
            g.queue[0] = v;
            g.count = 2;
        } else {
            if (kick)
                emit_tri(v, g.fan0, g.queue[0]);
            g.queue[0] = v;
        }
        break;
    case 6:     /* SPRITE */
        if (g.count == 0) {
            g.queue[0] = v;
            g.count = 1;
        } else {
            if (kick)
                emit_sprite(g.queue[0], v);
            g.count = 0;
        }
        break;
    default:
        break;
    }
}

Vtx current_vertex(uint16_t x, uint16_t y, uint32_t z, uint8_t f)
{
    Vtx v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.rgba = g.rgba;
    v.s = g.s;
    v.t = g.t;
    v.q = g.q;
    v.u = g.u;
    v.v = g.v;
    return v;
}

/* ======================================================================== */
/* Register writes                                                          */
/* ======================================================================== */

void set_prim(uint32_t prim)
{
    const bool fullColor = (prim & pgl::prim::PRIM_FULL_COLOR) != 0;    /* PC: no GS bit, kept apart */
    prim &= 0x7FF;
    if (prim != g.prim || fullColor != g.fullColor)
        g_dirty = true;
    g.prim = prim;
    g.fullColor = fullColor;
    g.count = 0;
}

void start_transfer(uint64_t trxdir)
{
    const uint32_t xdir = (uint32_t)(trxdir & 3);
    g_xfer.active = false;
    if (xdir == 3)
        return;
    if (xdir != 0) {
        log_once(xdir == 1 ? "pgl-xfer-l2h" : "pgl-xfer-l2l", "pgl: GS %s transfer in a GIF block: not native",
                 xdir == 1 ? "local -> host" : "local -> local");
        return;
    }
    g_xfer.dbp = (uint32_t)(g.bitbltbuf >> 32) & 0x3FFF;
    g_xfer.dbw = (uint32_t)(g.bitbltbuf >> 48) & 0x3F;
    g_xfer.dpsm = (uint32_t)(g.bitbltbuf >> 56) & 0x3F;
    g_xfer.dx = (int)((g.trxpos >> 32) & 0x7FF);
    g_xfer.dy = (int)((g.trxpos >> 48) & 0x7FF);
    g_xfer.w = (int)(g.trxreg & 0xFFF);
    g_xfer.h = (int)((g.trxreg >> 32) & 0xFFF);
    g_xfer.bytes = gstex::image_bytes(g_xfer.dpsm, (uint32_t)g_xfer.w, (uint32_t)g_xfer.h);
    g_xfer.data.clear();
    g_xfer.active = g_xfer.bytes > 0;
}

void image_data(const uint8_t *p, size_t bytes)
{
    if (!g_xfer.active)
        return;
    const size_t want = g_xfer.bytes - g_xfer.data.size();
    const size_t take = std::min(want, bytes);
    g_xfer.data.insert(g_xfer.data.end(), p, p + take);
    if (g_xfer.data.size() >= g_xfer.bytes) {
        flush_batch();
        ProfScope prof(PROF_UPLOAD);    /* (an upload inside a GIF block) */
        g_profCount[PCOUNT_UPLOADS]++;
        registry_upload(g_xfer.dbp, g_xfer.dbw, g_xfer.dpsm, g_xfer.dx, g_xfer.dy, g_xfer.w, g_xfer.h,
                        g_xfer.data.data(), g_xfer.data.size());
        g_xfer.active = false;
        g_dirty = true;
    }
}

/* PC (2026-11-05, the resolved runs' textures: "The textures of a run"
 * below): a TEX0 / TEX2 write with CLD - its CLUT load.  In a replay the
 * load is deferred (g_clutDefer: checked against the run's record of it,
 * numbered, not done - done later only before a part's texture that must be
 * resolved after all, or at the run's end for the rows that survive it); in
 * a recording it is done and noted (run_rec_clut). */
extern bool g_clutDefer;
void run_clut_deferred(uint64_t tex0);
void run_rec_clut();
struct RunRec;
extern std::unique_ptr<RunRec> g_runRec;
inline void run_clut_load(uint64_t tex0)
{
    if (g_clutDefer) {
        run_clut_deferred(tex0);
        return;
    }
    const uint32_t s0 = clut_load_serial();
    registry_clut_load(tex0);
    if (g_runRec && clut_load_serial() != s0)
        run_rec_clut();
}

/* A write of a GS register in its 64-bit format (A+D, REGLIST, pgl). */
void write_reg(uint32_t addr, uint64_t v)
{
    addr &= 0xFF;
    if (addr < 0x80 && g_addrReg[addr] >= 0) {
        const int c = g_addrCtx[addr];
        const int i = g_addrReg[addr];
        if (i == gs::REG_TEX2) {
            /* TEX2: PSM and the CLUT fields of TEX0 */
            const uint64_t mask = (0x3Full << 20) | (~0ull << 37);
            const uint64_t t0 = (g.ctx[c][gs::REG_TEX0] & ~mask) | (v & mask);
            g.ctx[c][gs::REG_TEX2] = v;
            if ((t0 & ~(7ull << 61)) != (g.ctx[c][gs::REG_TEX0] & ~(7ull << 61)))
                g_dirtyTex = true;          /* PC: (the texture alone: texture_state) */
            g.ctx[c][gs::REG_TEX0] = t0;
            if ((v >> 61) != 0) {
                run_clut_load(t0);          /* PC: (the resolved runs: deferred in a replay) */
                g_dirtyTex = true;
            }
            return;
        }
        if (i == gs::REG_TEX0 && (v >> 61) != 0) {
            run_clut_load(v);               /* PC: (the resolved runs: deferred in a replay) */
            g_dirtyTex = true;
        }
        if (g.ctx[c][i] != v) {
            g.ctx[c][i] = v;
            /* PC: the texture's registers, the texture alone (texture_state) */
            if (i == gs::REG_TEX0 || i == gs::REG_TEX1 || i == gs::REG_CLAMP || i == gs::REG_MIPTBP1 ||
                i == gs::REG_MIPTBP2)
                g_dirtyTex = true;
            else
                g_dirty = true;
        }
        return;
    }
    if (addr < 0x80 && g_addrGen[addr] >= 0) {
        const int i = g_addrGen[addr];
        if (i == gs::GEN_TEXFLUSH) {
            /* TEXFLUSH: what was drawn so far is what the next texture reads
             * see.  A batch that samples the target it draws into (or its Z
             * buffer) reads the copy taken when its state was resolved, so
             * primitives that read the ones before them through a TEXFLUSH
             * (the flare query's in-place reduction, flare_reduce /
             * flare_build_reduce: each halving reads the previous one; blur
             * passes over themselves) must not share it: the batch ends and
             * the next primitive resolves its texture again (a fresh copy). */
            if (g_bs.valid && g_bs.tme && g_bs.tex.target != nullptr &&
                (g_bs.tex.target == g_bs.rt || g_bs.tex.target == g_bs.zt)) {
                if (g_bs.tex.target == g_bs.rt && !g_bs.depthFrame) {
                    /* PC: what the next primitives must see (g_fbRects):
                     * first the target's part written since the copy
                     * before this batch (all if written somewhere unknown),
                     * then each run of this batch's vertices that a
                     * TEXFLUSH ends */
                    if (!g_fbPending) {
                        const Target &t = *g_bs.rt;
                        g_fbRects.clear();
                        if (t.rectGen != t.gen)
                            g_fbRects.push_back({0, 0, (LONG)t.nw, (LONG)t.nh});
                        else if (t.dirtyR > t.dirtyL && t.dirtyB > t.dirtyT)
                            g_fbRects.push_back({t.dirtyL, t.dirtyT, t.dirtyR, t.dirtyB});
                        g_fbPending = true;
                    }
                    fb_segment_end();
                } else {
                    flush_batch();
                    g_dirty = true;
                }
            }
            return;
        }
        if (g.gen[i] != v) {
            g.gen[i] = v;
            if (i == gs::GEN_TEXA)
                g_dirtyTex = true;          /* PC: (the texture alone: texture_state) */
            else
                g_dirty = true;
        }
        return;
    }
    switch (addr) {
    case A_PRIM:
        set_prim((uint32_t)v);
        break;
    case A_RGBAQ:
        g.rgba = (uint32_t)v;
        g.q = pgl::u2f((uint32_t)(v >> 32));
        break;
    case A_ST:
        g.s = pgl::u2f((uint32_t)v);
        g.t = pgl::u2f((uint32_t)(v >> 32));
        break;
    case A_UV:
        g.u = (uint16_t)(v & 0x3FFF);
        g.v = (uint16_t)((v >> 16) & 0x3FFF);
        break;
    case A_FOG:
        g.fog = (uint8_t)(v >> 56);
        break;
    case A_XYZF2:
    case A_XYZF3:
        g.fog = (uint8_t)(v >> 56);
        vertex(current_vertex((uint16_t)v, (uint16_t)(v >> 16), (uint32_t)(v >> 32) & 0xFFFFFF, g.fog),
               addr == A_XYZF2);
        break;
    case A_XYZ2:
    case A_XYZ3:
        vertex(current_vertex((uint16_t)v, (uint16_t)(v >> 16), (uint32_t)(v >> 32), g.fog), addr == A_XYZ2);
        break;
    case A_PRMODE:
        if (g.prmode != v)
            g_dirty = true;
        g.prmode = v;
        break;
    case A_BITBLTBUF:
        g.bitbltbuf = v;
        break;
    case A_TRXPOS:
        g.trxpos = v;
        break;
    case A_TRXREG:
        g.trxreg = v;
        break;
    case A_TRXDIR:
        start_transfer(v);
        break;
    default:
        break;      /* SIGNAL, FINISH, LABEL, HWREG ... */
    }
}

/* One PACKED qword for register descriptor reg. */
void packed(uint32_t reg, const uint8_t *qw)
{
    uint32_t w[4];
    memcpy(w, qw, 16);
    uint64_t lo, hi;
    memcpy(&lo, qw, 8);
    memcpy(&hi, qw + 8, 8);
    switch (reg) {
    case 0x0:
        write_reg(A_PRIM, w[0] & 0x7FF);
        break;
    case 0x1:
        g.rgba = (w[0] & 0xFF) | ((w[1] & 0xFF) << 8) | ((w[2] & 0xFF) << 16) | ((w[3] & 0xFF) << 24);
        break;
    case 0x2:
        g.s = pgl::u2f(w[0]);
        g.t = pgl::u2f(w[1]);
        g.q = pgl::u2f(w[2]);
        break;
    case 0x3:
        g.u = (uint16_t)(w[0] & 0x3FFF);
        g.v = (uint16_t)(w[1] & 0x3FFF);
        break;
    case 0x4: {
        const bool adc = ((w[3] >> 15) & 1) != 0;
        g.fog = (uint8_t)((w[3] >> 4) & 0xFF);
        vertex(current_vertex((uint16_t)w[0], (uint16_t)w[1], (w[2] >> 4) & 0xFFFFFF, g.fog), !adc);
        break;
    }
    case 0x5: {
        const bool adc = ((w[3] >> 15) & 1) != 0;
        vertex(current_vertex((uint16_t)w[0], (uint16_t)w[1], w[2], g.fog), !adc);
        break;
    }
    case 0xA:
        g.fog = (uint8_t)((w[3] >> 4) & 0xFF);
        break;
    case 0xE:
        write_reg((uint32_t)(hi & 0xFF), lo);
        break;
    case 0xF:
    case 0xB:
        break;
    default:            /* TEX0_1/2, CLAMP_1/2, XYZF3, XYZ3: the low 64 bits */
        write_reg(reg, lo);
        break;
    }
}

void exec_gif(const CmdGif &c)
{
    const uint8_t *p = static_cast<const uint8_t *>(c.data);
    uint32_t left = c.qwc;
    if (p == nullptr)
        return;
    while (left > 0) {
        uint64_t lo, hi;
        memcpy(&lo, p, 8);
        memcpy(&hi, p + 8, 8);
        p += 16;
        left--;
        const uint32_t nloop = (uint32_t)(lo & 0x7FFF);
        const bool pre = ((lo >> 46) & 1) != 0;
        const uint32_t primField = (uint32_t)((lo >> 47) & 0x7FF);
        const uint32_t flg = (uint32_t)((lo >> 58) & 3);
        uint32_t nreg = (uint32_t)((lo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        if (flg == 0) {
            if (pre)
                write_reg(A_PRIM, primField);
            for (uint32_t l = 0; l < nloop; l++) {
                for (uint32_t r = 0; r < nreg; r++) {
                    if (left == 0)
                        return;
                    packed((uint32_t)((hi >> (4 * r)) & 0xF), p);
                    p += 16;
                    left--;
                }
            }
        } else if (flg == 1) {
            const uint32_t total = nloop * nreg;
            const uint32_t qwords = (total + 1) / 2;
            if (qwords > left)
                return;
            for (uint32_t i = 0; i < total; i++) {
                const uint32_t reg = (uint32_t)((hi >> (4 * (i % nreg))) & 0xF);
                uint64_t v;
                memcpy(&v, p + 8 * i, 8);
                if (reg == 0xE || reg == 0xF)
                    continue;
                write_reg(reg, v);
            }
            p += 16 * qwords;
            left -= qwords;
        } else {
            const uint32_t q = std::min(nloop, left);
            image_data(p, (size_t)q * 16);
            p += 16 * q;
            left -= q;
        }
    }
}

void exec_regs(const CmdRegs &c)
{
    /* PC (CPU time, 2026-11-05): the set bits alone, in the same order as
     * before (context 1's registers, context 2's, the general ones: bits 0..12,
     * 16..28, 32..41 ascending) - a flush writes a few registers, and 36 bit
     * tests ran for each of a race frame's 6,000 flushes */
    uint32_t lo = (uint32_t)c.mask, hi = (uint32_t)(c.mask >> 32);
    unsigned long i;
    while (lo != 0) {
        _BitScanForward(&i, lo);
        lo &= lo - 1;
        if (i < 16) {
            if (i < (unsigned long)gs::REG_COUNT)
                write_reg(kCtxAddr[i], c.reg[i]);
        } else if (i - 16 < (unsigned long)gs::REG_COUNT) {
            write_reg(kCtxAddr[i - 16] + 1u, c.reg[i - 16]);
        }
    }
    while (hi != 0) {
        _BitScanForward(&i, hi);
        hi &= hi - 1;
        if (i < (unsigned long)gs::GEN_COUNT)
            write_reg(kGenAddr[i], c.gen[i]);
    }
}

void exec_prim(const CmdPrim &c)
{
    set_prim(c.prim);
    const pr::GsVertex *v = reinterpret_cast<const pr::GsVertex *>(&c + 1);
    for (int i = 0; i < c.n; i++) {
        g.rgba = v[i].rgba;
        g.u = v[i].u;
        g.v = v[i].v;
        g.fog = v[i].f;
        g.s = g.t = 0;
        g.q = 1;
        vertex(current_vertex(v[i].x, v[i].y, v[i].z, v[i].f), v[i].kick);
    }
}

/* ======================================================================== */
/* VU1 draws                                                                */
/* ======================================================================== */

bool light_program(uint32_t prog, int32_t &program, int32_t &e)
{
    e = 16;
    switch (prog) {
    case 0x04E: program = VU1P_DEFAULT; return true;
    case 0x057: program = VU1P_SPEC2; e = 4; return true;
    case 0x082: program = VU1P_SPEC2; e = 8; return true;
    case 0x0AF: program = VU1P_SPEC2; return true;
    case 0x0DE: program = VU1P_SPEC4; e = 4; return true;
    case 0x115: program = VU1P_SPEC4; e = 8; return true;
    case 0x14C: program = VU1P_SPEC4; return true;
    case 0x185: program = VU1P_DIFFUSE; return true;
    case 0x1AD: program = VU1P_LOCAL; e = 4; return true;
    case 0x1E1: program = VU1P_LOCAL; e = 8; return true;
    case 0x21A: program = VU1P_LOCAL; return true;
    case 0x257: program = VU1P_ENVMTX; return true;
    case 0x297: program = VU1P_ENVMAP; return true;
    case 0x2DB: program = VU1P_CONST; return true;
    case 0x2F7: program = VU1P_FLARE; return true;
    default: program = VU1P_DEFAULT; return false;
    }
}

/* PC: where a VU1 draw of the vertices in box [bmin, bmax] (object space)
 * lands in the target (native pixels): its 8 corners through C and the
 * viewport as vs_vu1 / project_vu1 take them, 2 pixels around.  False when
 * unknown (no box, a corner at or behind the eye): the scissor's then.  For
 * the target's written part (target_written_rect): the copies that draws
 * reading the target take (DATE, C = Ad, a texture of it) are refreshed
 * there only - each one a whole MSAA resolve before (user: MSAA 8x slow). */
/* (why a batch takes the clipper: the profile's CLIP counts) */
enum : uint8_t { CLIPWHY_NOBOX = 1, CLIPWHY_BEHIND = 2, CLIPWHY_NEAR = 4, CLIPWHY_XY = 8, CLIPWHY_FAR = 16 };
thread_local uint8_t g_clipWhy;             /* PC (2026-11-06): (per thread: the stage walks the runs' draws, step 3) */

/* (b: the draw's state - the executor's g_bs, or the stage's copy of a run's
 * state: the GS offset and the scale of the target) */
bool vu1_area(const BatchState &b, const PglrVu1 &vu, const float *bmin, const float *bmax, D3D11_RECT &out,
              bool *inside)
{
    *inside = false;
    g_clipWhy = CLIPWHY_NOBOX;
    if (bmin == nullptr || bmax == nullptr)
        return false;
    g_clipWhy = 0;
    const float offX = (float)b.ofx * 0.0625f, offY = (float)b.ofy * 0.0625f;
    /* PC (CPU time, 2026-11-03): the 8 corners 4 at a time (SSE: corner i
     * takes x from bmin / bmax by bit 0, y by bit 1, z by bit 2 - lanes 0..3
     * corners 0..3, the second vector 4..7), each axis' two products with
     * its matrix row as P * M did, summed in the same order: the same floats
     * as one corner at a time */
    const __m128 xs = _mm_setr_ps(bmin[0], bmax[0], bmin[0], bmax[0]);
    const __m128 ys = _mm_setr_ps(bmin[1], bmin[1], bmax[1], bmax[1]);
    const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    __m128 pc[2][4], mag[2][2];
    for (int k = 0; k < 4; k++) {
        const __m128 ax = _mm_mul_ps(xs, _mm_set1_ps(vu.m[k]));
        const __m128 ay = _mm_mul_ps(ys, _mm_set1_ps(vu.m[4 + k]));
        const __m128 axy = _mm_add_ps(ax, ay);
        const __m128 t = _mm_set1_ps(vu.m[12 + k]);
        for (int h = 0; h < 2; h++) {
            const __m128 az = _mm_set1_ps((h ? bmax[2] : bmin[2]) * vu.m[8 + k]);
            pc[h][k] = _mm_add_ps(_mm_add_ps(axy, az), t);
            if (k >= 2) {
                /* (the products' magnitudes: the z test's spare, below) */
                mag[h][k - 2] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_and_ps(ax, absMask), _mm_and_ps(ay, absMask)),
                                                      _mm_and_ps(az, absMask)),
                                           _mm_and_ps(t, absMask));
            }
        }
    }
    for (int h = 0; h < 2; h++)
        if (_mm_movemask_ps(_mm_cmpgt_ps(pc[h][3], _mm_set1_ps(1e-4f))) != 0xF) {
            g_clipWhy |= CLIPWHY_BEHIND;
            return false;
        }
    /* PC: inside the clipper's six planes (gs_vu1's clip_dist: |x|, |y|
     * <= 2w, |z| <= w), a thousandth of w to spare for the GPU's own
     * rounding of the same products.  For z the spare is the rounding's
     * own bound instead (1e-5 of the products' magnitudes, ~170 times
     * float's): the frustum's z / w nears (f + n) / (f - n) far away,
     * within a thousandth of 1 from a few hundred near distances on -
     * two thirds of a 32-car race's parts went through the clipper for
     * that (CLIP FAR, user capture 2026-11-03) */
    bool in = true;
    __m128 x0 = _mm_set1_ps(1e30f), y0 = x0, x1 = _mm_set1_ps(-1e30f), y1 = x1;
    for (int h = 0; h < 2; h++) {
        const __m128 w = pc[h][3], w2 = _mm_add_ps(w, w), m = _mm_mul_ps(_mm_set1_ps(1e-3f), w);
        const __m128 mz = _mm_mul_ps(_mm_set1_ps(1e-5f), _mm_add_ps(mag[h][0], mag[h][1]));
        const int zOut = _mm_movemask_ps(_mm_cmplt_ps(_mm_sub_ps(w, _mm_and_ps(pc[h][2], absMask)), mz)) |
                         _mm_movemask_ps(_mm_cmpunord_ps(pc[h][2], pc[h][2]));
        if (zOut != 0) {
            in = false;
            const int neg = _mm_movemask_ps(_mm_cmplt_ps(pc[h][2], _mm_setzero_ps()));
            g_clipWhy |= ((zOut & neg) ? CLIPWHY_NEAR : 0) | ((zOut & ~neg) ? CLIPWHY_FAR : 0);  /* (z >= -w / z <= w) */
        }
        const __m128 xyIn = _mm_and_ps(_mm_cmpge_ps(_mm_sub_ps(w2, _mm_and_ps(pc[h][0], absMask)), m),
                                       _mm_cmpge_ps(_mm_sub_ps(w2, _mm_and_ps(pc[h][1], absMask)), m));
        if (_mm_movemask_ps(xyIn) != 0xF) {
            in = false;
            g_clipWhy |= CLIPWHY_XY;
        }
        const __m128 gx = _mm_add_ps(_mm_div_ps(_mm_mul_ps(_mm_set1_ps(vu.viewScale[0]), pc[h][0]), w),
                                     _mm_set1_ps(vu.viewCenter[0]));
        const __m128 gy = _mm_add_ps(_mm_div_ps(_mm_mul_ps(_mm_set1_ps(vu.viewScale[1]), pc[h][1]), w),
                                     _mm_set1_ps(vu.viewCenter[1]));
        const __m128 px = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(gx, _mm_set1_ps(offX)), _mm_set1_ps(b.sx)), _mm_set1_ps(b.pix));
        const __m128 py = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(gy, _mm_set1_ps(offY)), _mm_set1_ps(b.sy)), _mm_set1_ps(b.pix));
        x0 = _mm_min_ps(x0, px);
        y0 = _mm_min_ps(y0, py);
        x1 = _mm_max_ps(x1, px);
        y1 = _mm_max_ps(y1, py);
    }
    *inside = in;
    alignas(16) float lo[4], hi[4], lo2[4], hi2[4];
    _mm_store_ps(lo, x0);
    _mm_store_ps(hi, x1);
    _mm_store_ps(lo2, y0);
    _mm_store_ps(hi2, y1);
    const float fx0 = std::min(std::min(lo[0], lo[1]), std::min(lo[2], lo[3]));
    const float fx1 = std::max(std::max(hi[0], hi[1]), std::max(hi[2], hi[3]));
    const float fy0 = std::min(std::min(lo2[0], lo2[1]), std::min(lo2[2], lo2[3]));
    const float fy1 = std::max(std::max(hi2[0], hi2[1]), std::max(hi2[2], hi2[3]));
    if (!(fx1 - fx0 < 1e6f && fy1 - fy0 < 1e6f))
        return false;
    out = {(LONG)std::floor(fx0) - 2, (LONG)std::floor(fy0) - 2, (LONG)std::ceil(fx1) + 2, (LONG)std::ceil(fy1) + 2};
    return true;
}

/* PC (docs/renderer.md 4, "Precision"): the GS Z from the clip w.  A
 * perspective projection makes clip z = A w + B, so Z = viewScale.z (A +
 * B / w) + viewCenter.z (rec.zFromW: the draw's record, gs_zw).  A and B from the projection
 * the matrix was sent with (vu.projZ) when the combined matrix agrees (its
 * z column A times its w column, the translation row's + B), else from the
 * combined matrix alone (B then rounded as its translation row: the same
 * for every draw with that matrix), else (not of that form) pc.z as the
 * microcode takes it.  An overlay of `layer` layers (docs/renderer.md
 * "Overlays") is drawn kOverlayRel x its distance nearer per layer from w,
 * else kOverlayUnits GS units per layer. */
constexpr double kOverlayRel = 1e-5;
constexpr float kOverlayUnits = 4.0f;

void vu1_depth(const PglrVu1 &vu, int layer, Vu1Inst &rec)
{
    const float *m = vu.m;
    double ww = 0.0, zw = 0.0, zz = 0.0;
    for (int r = 0; r < 3; r++) {
        ww += (double)m[r * 4 + 3] * m[r * 4 + 3];
        zw += (double)m[r * 4 + 2] * m[r * 4 + 3];
        zz += (double)m[r * 4 + 2] * m[r * 4 + 2];
    }
    int mode = 0;
    double a = 0.0, b = 0.0;
    if (ww > 1e-20) {
        const double ac = zw / ww;
        double res = 0.0;
        for (int r = 0; r < 3; r++) {
            const double d = m[r * 4 + 2] - ac * m[r * 4 + 3];
            res += d * d;
        }
        if (res <= 1e-10 * zz) {
            const double tz = m[14], tw = m[15];
            mode = 2;
            a = ac;
            b = tz - ac * tw;
            if (vu.projZ[2] != 0.0f) {
                const double ap = vu.projZ[0], bp = vu.projZ[1];
                if (std::fabs(ac - ap) <= 1e-5 * std::fabs(ap) &&
                    std::fabs((tz - ap * tw) - bp) <= 1e-5 * (std::fabs(tz) + std::fabs(ap * tw) + std::fabs(bp))) {
                    mode = 1;
                    a = ap;
                    b = bp;
                }
            }
        }
    }
    if (mode != 0) {
        const double za = (double)vu.viewScale[2] * a + vu.viewCenter[2];
        double zb = (double)vu.viewScale[2] * b;
        zb += std::fabs(zb) * kOverlayRel * layer;
        rec.zFromW[0] = (float)za;
        rec.zFromW[1] = (float)zb;
        rec.zFromW[3] = 0.0f;
    } else {
        rec.zFromW[0] = rec.zFromW[1] = 0.0f;
        rec.zFromW[3] = kOverlayUnits * (float)layer;
    }
    rec.zFromW[2] = (float)mode;
}

/* PC (2026-11-05): the resolved runs' recording (below): what a VU1 draw
 * took of the state (ensure_state's answer) and the draw itself */
struct RunRec;
extern std::unique_ptr<RunRec> g_runRec;    /* (defined with the runs below) */
void run_rec_draw(const PglrVu1 &vu, int event, uint32_t prim, uint32_t indexCount, uint32_t firstIndex,
                  int32_t baseVertex, const float *bmin, const float *bmax, uint32_t batch, ID3D11Buffer *bvb,
                  int aMin, int aMax, float n2);
void run_rec_prim(uint32_t prim);
/* PC (2026-11-05): a replay's word on the draw at hand (run_replay_part):
 * its batch lies inside the clipper's planes with the others of its matrix
 * group, and writes within rect (the group's box through the matrix:
 * submit_vu1 takes them instead of its own vu1_area) */
struct RunHint {
    bool valid = false;
    D3D11_RECT rect = {};
};
RunHint g_runHint;

void submit_vu1(const PglrVu1 &vu, const Vu1Head &head, const float (*L)[4], ID3D11Buffer *vb, ID3D11Buffer *ib,
                uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, const float *bmin, const float *bmax,
                int overlay, uint32_t batch, ID3D11Buffer *bvb, int aMin, int aMax, float n2, ID3D11Buffer *ib2,
                uint32_t nVerts);

/* One VU1 run: the program state vu, PRIM prim, an indexed triangle list
 * (its vertices in box [bmin, bmax] when known: the area it writes); an
 * overlay's layer (docs/renderer.md "Overlays"); PC: its batch in a model's
 * own buffers and their vertices' batches (bvb; ~0 / null: streamed
 * vertices - not merged: MergedDraw).  head: the constants' head (the
 * program, its exponent, the format, the morph weight, the flare's); L: the
 * lighting block.  vu and L must stay where they are while the frame
 * executes (the frame's list: the submit stage reads them there).
 * PC (2026-11-05): the state's part here, the draw's in submit_vu1 (a
 * resolved run's replay calls that alone, its states from the run). */
void vu1_draw(const PglrVu1 &vu, uint32_t prim, const Vu1Head &head, const float (*L)[4], ID3D11Buffer *vb,
              ID3D11Buffer *ib, uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex,
              const float *bmin = nullptr, const float *bmax = nullptr, int overlay = 0, uint32_t batch = ~0u,
              ID3D11Buffer *bvb = nullptr, int aMin = 0, int aMax = 255, float n2 = 1e30f,
              ID3D11Buffer *ib2 = nullptr, uint32_t nVerts = 0)
{
    flush_batch();
    set_prim(prim);         /* the program's GIF tag has PRE */
    if (g_runRec)
        run_rec_prim(prim);                 /* PC: (the resolved runs' recording: the GS state after) */
    if (vu.prog == 0 || indexCount == 0)
        return;
    const int event = ensure_state();
    if (g_runRec)                           /* PC: (the resolved runs' recording) */
        run_rec_draw(vu, event, prim, indexCount, firstIndex, baseVertex, bmin, bmax, batch, bvb, aMin, aMax, n2);
    BatchState &b = g_bs;
    if (b.skip || b.depthFrame) {
        if (b.skip && flicker_on())
            flicker_note(3, indexCount, nullptr, 0);
        return;
    }
    submit_vu1(vu, head, L, vb, ib, indexCount, firstIndex, baseVertex, bmin, bmax, overlay, batch, bvb, aMin, aMax,
               n2, ib2, nVerts);
}

/* (the submit stage) The draw of a VU1 run (SubVu1) under its state (g_sub:
 * valid, not skipped, not a depth frame): its record, constants, the merged
 * draw it joins or its own passes. */
void sub_vu1(const SubVu1 &d)
{
    const SubState &s = g_sub;
    const BatchState &b = s.bs;
    const PglrVu1 &vu = *d.vu;
    const float (*L)[4] = d.L;
    /* PC (CPU time, 2026-11-03): one block kept (zeroed once, 448 bytes a
     * part before): every field is set here, the rest stays 0 (Vu1Inst has
     * the draw's own) */
    static thread_local CbVu1 cb = {};      /* PC (2026-11-05): (per thread: the submit workers) */
    memcpy(cb.C, vu.m, sizeof(cb.C));
    memcpy(cb.viewScale, vu.viewScale, 16);
    memcpy(cb.viewCenter, vu.viewCenter, 16);
    memcpy(cb.limits, vu.limits, 16);
    cb.program = d.head.program;
    cb.specExp = d.head.specExp;
    cb.format = d.head.format;
    cb.morph = d.head.morph;
    cb.flareO = d.head.flareO;
    cb.flareGpu = d.head.flareGpu;
    cb.flareX = d.head.flareX;
    cb.flareY = d.head.flareY;
    memcpy(cb.hideC, d.head.hideC, sizeof(cb.hideC));   /* PC (2026-11-07): the hide box */
    memcpy(cb.hideH, d.head.hideH, sizeof(cb.hideH));
    /* PC (CPU time, 2026-11-05): what follows from the program state alone -
     * the depth from w (vu1_depth: double precision, per batch), the culling
     * - and with the state the NDC map, worked out once for the batches that
     * share them (a part's, consecutive parts' with one matrix: most) */
    static thread_local struct {
        PglrVu1 vu;
        int overlay;
        uint32_t serial;
        float zFromW[4];
        float ndcMap[4];
        int cull;
        bool valid;
    } last;
    const bool sameVu = last.valid && last.overlay == d.overlay && same_bytes(&vu, &last.vu, sizeof(vu));
    /* PC (2026-11-03): the draw's own values in its record (Vu1Inst) - CbVu1's
     * texW, texH, flatShade and zFromW stay 0, so the constants stay the same
     * from part to part of a model */
    Vu1Inst inst;
    if (sameVu) {
        memcpy(inst.zFromW, last.zFromW, sizeof(inst.zFromW));
    } else {
        vu1_depth(vu, d.overlay, inst);
        last.vu = vu;
        last.overlay = d.overlay;
        memcpy(last.zFromW, inst.zFromW, sizeof(last.zFromW));
        last.serial = ~0u;                  /* (the NDC map and the culling below) */
        last.valid = true;
    }
    inst.texW = b.tme ? b.texW : 1.0f;
    inst.texH = b.tme ? b.texH : 1.0f;
    inst.flat = b.flat ? 1.0f : 0.0f;
    inst.parts = 0.0f;
    inst.texRect[0] = b.tme ? b.ps.texX0 : 0.0f;
    inst.texRect[1] = b.tme ? b.ps.texY0 : 0.0f;
    inst.texRect[2] = b.tme ? b.ps.texInvW : 0.0f;
    inst.texRect[3] = b.tme ? b.ps.texInvH : 0.0f;
    /* PC (2026-11-04): its lighting in the record (the block in the lights'
     * ring, or -1: CbVu1's), so parts with materials of their own merge.
     * (PC, 2026-11-05: the block is copied into the constants only where
     * they carry it - without the ring, or for the trace's lines) */
    const bool lightRing = light_supported();
    if (!lightRing || g_tracing)
        memcpy(cb.L, L, sizeof(cb.L));
    inst.lightBase = lightRing ? (float)light_block(L) : -1.0f;
    inst.program = (float)cb.program;
    inst.specExp = (float)cb.specExp;
    inst.pad = (float)(4u * d.nVerts);      /* PC (2026-11-06): (the copy's place goes in bits 0..1: vu1_inst; vs_vu1's rep) */
    const size_t cbBytes = lightRing ? kCbVu1Head : sizeof(cb);
    int cull;
    if (last.serial == s.serial) {
        memcpy(cb.ndcMap, last.ndcMap, sizeof(cb.ndcMap));
        cull = last.cull;
    } else {
        const float offX = (float)b.ofx * 0.0625f, offY = (float)b.ofy * 0.0625f;
        const float nw = (float)s.nw, nh = (float)s.nh;
        cb.ndcMap[0] = 2.0f * vu.viewScale[0] * b.sx / nw;
        cb.ndcMap[1] = 2.0f * ((vu.viewCenter[0] - offX) * b.sx + b.pix) / nw;
        cb.ndcMap[2] = 2.0f * vu.viewScale[1] * b.sy / nh;
        cb.ndcMap[3] = 2.0f * ((vu.viewCenter[1] - offY) * b.sy + b.pix) / nh;
        /* culling: vu1Prog + 0xA culls the clockwise (GL y-up) faces, + 0x14
         * the counter-clockwise ones (docs/models.md 4.6) */
        const uint32_t off = vu.prog >= 0x33E ? vu.prog - 0x33E : vu.prog - 0x320;
        cull = off == 0x0A ? 1 : off == 0x14 ? 2 : 0;
        const bool mirrored = (vu.viewScale[0] < 0) != (vu.viewScale[1] > 0);
        if (mirrored && cull != 0)
            cull = 3 - cull;
        memcpy(last.ndcMap, cb.ndcMap, sizeof(last.ndcMap));
        last.cull = cull;
        last.serial = s.serial;
    }
    cb.zNorm = z_norm(b.zpsm);
    /* PC (GPU time, 2026-11-03): a batch inside the clipper's planes is drawn
     * without it (vs_vu1_direct: gs_vu1's inside path), the flare program's
     * discs aside (its vertex stage drops an unlit one past the far plane);
     * the executor found whether it is (SubVu1 inside: vu1_area, a replay's
     * hint) */
    const bool direct = d.inside && cb.program != VU1P_FLARE && !g_noDirect;
    const Vs vs = direct ? Vs::Vu1Direct : Vs::Vu1;
    /* PC (2026-11-04): the alpha its fragments get from the vertex stage (the
     * alpha mirror: mirror_plan), when the target keeps one (or for
     * GT3PC_PGL_ALPHA_LOG's check, alpha_log_check) */
    /* (PC, 2026-11-05: and for the target's alpha bound while it is known -
     * a draw writing alpha into such a target widens it by its range) */
    /* (PC, step 2: a submit worker cannot read the bound as of the run's
     * place - it is the submit thread's - so it takes the range whenever the
     * target's alpha is written, and the publish unions it if the bound is
     * known then: the same bound) */
    if (d.mirrorRange || g_alphaLog ||
        (g_alphaBoundOn && b.wroteTop && s.rtAlpha != nullptr &&
         (q::t_capture != nullptr || s.rtAlpha->alphaLo >= 0)))
        light_alpha_range_memo((int32_t)inst.lightBase, cb.program, cb.specExp, L, d.aMin, d.aMax, d.n2, g_vLo, g_vHi);
    else
        g_vLo = 0, g_vHi = 255;
    /* PC (2026-11-03): into the held draw with its texture, if it may join
     * (MergedDraw) - nothing bound for it then */
    uint32_t batch = d.batch;
    if (d.bvb == nullptr || batch > 0xFFFF)
        batch = ~0u;                        /* (the batches' buffer holds 16 bits) */
    /* PC (2026-11-06): a C = Ad draw from the doubled index stream (pgl_models.cpp
     * make_mesh_buffers): the two passes draw_passes would make of it (its
     * form: pass_reps, twoInst) as the two halves of one draw of the doubled
     * range - one instance, its record once, and it may join the next part's
     * (merge_join) */
    const gs::Pass &p0 = b.ds.pass[0];
    const uint8_t mask0 = p0.writeMask & b.ds.frameWriteMask;
    const bool dbl = g_adDoubleOn && g_adInstOn && g_datePairOn && !g_adOnce && d.ib2 != nullptr && d.nVerts != 0 &&
                     d.baseVertex == 0 && !b.depthFrame && b.ds.passCount == 1 &&
                     b.ds.blend.ad == gs::AdMode::TwoPass && b.ds.blend.dst == gs::BlendFactor::One &&
                     (mask0 & gs::WRITE_RGB) != 0 && !(p0.depthWrite && s.hasZ) && !g_tracing && !flicker_on();
    ID3D11Buffer *const ib = dbl ? d.ib2 : d.ib;
    const uint32_t count = dbl ? 2u * d.indexCount : d.indexCount;
    const uint32_t first = dbl ? 2u * d.firstIndex : d.firstIndex;
    cap::t_mergeWhy = cap::MW_FIRST;        /* PC (2026-11-06): (the frame capture: merge_join says why not) */
    if (g_mergeOn && merge_join(cb, inst, vs, cull, d.vb, ib, count, first, d.baseVertex, batch, d.mirror, dbl)) {
        /* PC (2026-11-07): the lazy mirror's note of a run's state (RunState::
         * passArea, draw_passes) takes the merged draw's whole pass: its one
         * pass after the draw covers this part too - a course run's draw of
         * five parts needed 2.3 Mpx of pass where its first part's box was
         * 0.27 (the user's replay capture, frame 11929), and the state's note
         * of the first part alone never let the mirror go.  The union of the
         * parts' boxes, not their sum: a car's parts' boxes lie over each
         * other (frame 7188: the sum let near cars go, four syncs) */
        MergedDraw &m = g_merged;
        m.passBox = rect_union(m.passBox, d.hasArea ? d.area : b.scissor);  /* (and the capture's area of the draw) */
        if (d.mirror && m.plan.pass && t_passState != nullptr) {
            const uint32_t px = area_px(m.passBox, b.scissor, s.nw, s.nh);
            if (px > *t_passState)
                *t_passState = px;
        }
        return;
    }
    bind_vs(vs);
    bind_vs_cb(&cb, cbBytes);
    set_vb(d.vb, sizeof(gt3vif::Vertex));
    set_ib(ib);
    set_batch_vb(d.bvb);
    set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    /* (PC, 2026-11-05: a C = Ad form's record in two places - its draw of
     * two instances reads the second too, pgl_d3d.h PSF_AD_INST; one more
     * when its mirror pass may be folded in as one more instance, which
     * draw_passes decides with the pass's plan: mirror_fold_maybe.  PC,
     * 2026-11-06: once for a draw from the doubled stream - one instance,
     * no fold) */
    const uint32_t instSeq = vu1_inst(inst, dbl ? 1u : (b.ds.blend.ad == gs::AdMode::TwoPass ? 2u : 1u) +
                                                          (mirror_fold_maybe(s, d.mirror) ? 1u : 0u));
    const Vu1Cur cur = {vs, cull, d.vb, ib, batch, &inst};
    g_vu1Cur = &cur;
    g_traceVu1 = &cb;
    g_traceInst = &inst;
    g_traceOverlay = d.overlay;
    draw_passes(true, count, first, d.baseVertex, cull, d.hasArea ? &d.area : nullptr, instSeq, d.mirror, d.dateSrv,
                false, d.noPass, dbl);
    g_vu1Cur = nullptr;
    g_traceVu1 = nullptr;
    g_traceInst = nullptr;
    g_traceOverlay = 0;
}

/* (the executor) A VU1 run's draw under the state as it is (g_bs: valid, not
 * skipped, not a depth frame): its area and clipping, the executor's half
 * (submit_prologue), its record for the submit stage (sub_vu1: at once when
 * the frame does not pipeline). */
/* (the profile's counts: a VU1 draw drawn without the clipper or through it,
 * and why - the box behind the eye, past the near / far planes, past the
 * guard band - the first of those - or none known) */
void count_direct(bool direct)
{
    g_profCount[direct ? PCOUNT_DIRECT : PCOUNT_CLIPPED]++;
    if (direct)
        return;
    const int why = (g_clipWhy & CLIPWHY_BEHIND) ? PCOUNT_CLIP_BEHIND
                    : (g_clipWhy & CLIPWHY_NEAR)   ? PCOUNT_CLIP_NEAR
                    : (g_clipWhy & CLIPWHY_FAR)    ? PCOUNT_CLIP_FAR
                    : (g_clipWhy & CLIPWHY_XY)     ? PCOUNT_CLIP_XY
                    : (g_clipWhy & CLIPWHY_NOBOX)  ? PCOUNT_CLIP_NOBOX : -1;
    if (why >= 0)
        g_profCount[why]++;
}

/* PC (2026-11-05): [Graphics] CarReflections (settings.h) - an env pass
 * (the environment map on the cars: ENVMTX / ENVMAP into a frame-sized
 * target, not the course's env view) of a part under the size, or any,
 * left out: a far car's reflection is a few pixels for two rasterizations a
 * part, DATE and C = Ad.  True: left out. */
/* (the limit for draws of state b: 0 none - a small target is not the frame;
 * PC 2026-11-06: taken per state for the stage's walk of a run, WorkState) */
int refl_limit(const BatchState &b)
{
    if (b.rt == nullptr || b.rt->gsW < 512)
        return 0;
    return std::max(0, settings::car_reflection_min_px());
}

bool reflection_skipped_lim(int program, bool hasArea, const D3D11_RECT &area, int lim, float sx, float sy)
{
    if (lim <= 0 || !((program == VU1P_ENVMTX || program == VU1P_ENVMAP) && hasArea))
        return false;
    const float w = (float)(area.right - area.left) / sx;
    const float h = (float)(area.bottom - area.top) / sy;
    if (!(w < (float)lim && h < (float)lim))
        return false;
    t_stats->skipRefl++;
    return true;
}

bool reflection_skipped(int program, bool hasArea, const D3D11_RECT &area)
{
    return reflection_skipped_lim(program, hasArea, area, refl_limit(g_bs), g_bs.sx, g_bs.sy);
}

void submit_vu1(const PglrVu1 &vu, const Vu1Head &head, const float (*L)[4], ID3D11Buffer *vb, ID3D11Buffer *ib,
                uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, const float *bmin, const float *bmax,
                int overlay, uint32_t batch, ID3D11Buffer *bvb, int aMin, int aMax, float n2, ID3D11Buffer *ib2,
                uint32_t nVerts)
{
    SubVu1 d;
    d.vu = &vu;
    d.L = L;
    d.vb = vb;
    d.ib = ib;
    d.bvb = bvb;
    d.ib2 = ib2;                            /* PC (2026-11-06): (the doubled index stream) */
    d.nVerts = nVerts;
    d.indexCount = indexCount;
    d.firstIndex = firstIndex;
    d.baseVertex = baseVertex;
    d.batch = batch;
    d.head = head;
    d.overlay = overlay;
    d.aMin = aMin;
    d.aMax = aMax;
    d.n2 = n2;
    /* PC (GPU time, 2026-11-03): a batch inside the clipper's planes is drawn
     * without it (vs_vu1_direct), the flare program's discs aside (its
     * vertex stage drops an unlit one past the far plane) */
    d.inside = false;
    d.area = {};
    if (g_runHint.valid) {
        /* PC (2026-11-05): a replay's matrix group inside the planes (RunHint) */
        d.area = g_runHint.rect;
        d.inside = true;
        d.hasArea = true;
        g_clipWhy = 0;
    } else {
        d.hasArea = vu1_area(g_bs, vu, bmin, bmax, d.area, &d.inside);
    }
    const bool direct = d.inside && head.program != VU1P_FLARE && !g_noDirect;
    count_direct(direct);
    if (draw_skipped(true, head.program))
        return;
    if (reflection_skipped(head.program, d.hasArea, d.area))    /* PC: ([Graphics] CarReflections) */
        return;
    MirrorIn in;                            /* PC (2026-11-06): (the lazy mirror's verdict: its lit alpha range, if needed) */
    in.lit = true;
    in.program = head.program;
    in.specExp = head.specExp;
    in.L = L;
    in.aMin = aMin;
    in.aMax = aMax;
    in.n2 = n2;
    submit_prologue(d.hasArea ? &d.area : nullptr, true, d.dateSrv, d.mirrorRange, d.mirror, in, d.noPass);
    sq::run([d] { sub_vu1(d); });
    if (g_bs.wroteTop && g_bs.rt != nullptr)
        g_bs.rt->alphaPos = sq::head();     /* PC: (its alpha bound written here: the submit workers wait for it) */
    if (g_tracing && (g_bs.wroteRgb || g_bs.wroteTop))
        probe_draw(*g_bs.rt);               /* (GT3PC_PGL_PROBE: after the draw) */
}

/* The box of n VU1 vertices (pos and pos2). */
void vu1_box(const gt3vif::Vertex *v, size_t n, float *bmin, float *bmax)
{
    for (int k = 0; k < 3; k++) {
        bmin[k] = 1e30f;
        bmax[k] = -1e30f;
    }
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            bmin[k] = std::min(bmin[k], std::min(v[i].pos[k], v[i].pos2[k]));
            bmax[k] = std::max(bmax[k], std::max(v[i].pos[k], v[i].pos2[k]));
        }
}

/* PC (2026-11-04): the range of n VU1 vertices' alphas (the alpha mirror) */
void vu1_alpha_range(const gt3vif::Vertex *v, size_t n, int &aMin, int &aMax)
{
    aMin = 255;
    aMax = 0;
    for (size_t i = 0; i < n; i++) {
        aMin = std::min(aMin, (int)std::min(v[i].rgba[3], v[i].rgba2[3]));
        aMax = std::max(aMax, (int)std::max(v[i].rgba[3], v[i].rgba2[3]));
    }
    if (n == 0)
        aMin = 0, aMax = 255;
}

std::vector<gt3vif::Vertex> g_vu1Verts;
std::vector<uint32_t> g_vu1Index;
/* PC (2026-11-05): the strips' and tri16's constants' head (unlit) and
 * lighting block (zeros, as their CbVu1 was: the submit stage reads the
 * block where the draw's record points, so a static one) */
const Vu1Head kUnlitHead = {VU1P_DEFAULT, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
const float kZeroLight[16][4] = {};

void strip_indices(uint32_t first, uint32_t len)
{
    for (uint32_t i = 0; i + 2 < len; i++) {
        const uint32_t a = first + i;
        if ((i & 1) == 0) {
            g_vu1Index.push_back(a + 2);
            g_vu1Index.push_back(a);
            g_vu1Index.push_back(a + 1);
        } else {
            g_vu1Index.push_back(a + 2);
            g_vu1Index.push_back(a + 1);
            g_vu1Index.push_back(a);
        }
    }
}

void exec_strip(const CmdStrip &c)
{
    const int n = c.n;
    if (n < 3 || c.xyz == nullptr)
        return;
    g_vu1Verts.assign((size_t)n, gt3vif::Vertex());
    for (int i = 0; i < n; i++) {
        gt3vif::Vertex &v = g_vu1Verts[(size_t)i];
        memset(&v, 0, sizeof(v));
        memcpy(v.pos, c.xyz + 3 * i, 12);
        memcpy(v.pos2, v.pos, 12);
        if (c.st != nullptr) {
            memcpy(v.st, c.st + 2 * i, 8);
            memcpy(v.st2, v.st, 8);
        }
        const uint32_t col = c.rgba != nullptr ? c.rgba[i] : c.constRgba;
        memcpy(v.rgba, &col, 4);
        memcpy(v.rgba2, &col, 4);
    }
    g_vu1Index.clear();
    uint32_t o = 0;
    for (int k = 0; k < c.nStrips && o < (uint32_t)n; k++) {
        uint32_t len = c.lengths[k];
        if (o + len > (uint32_t)n)
            len = (uint32_t)n - o;
        strip_indices(o, len);
        o += len;
    }
    if (g_vu1Index.empty())
        return;
    /* PC: the GS primitives batched so far first: streaming them can restart
     * the vertex stream (DISCARD), and vertices streamed before that are in
     * the buffer's old memory, which the draw no longer reads (vu1_draw drew
     * the batch after these were streamed: the first strip after a restart
     * lost its vertices - a key of the name entry, the OK bar, for a frame,
     * every couple of seconds in the menus: user report) */
    flush_batch();
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    const uint32_t bv = stream_vertices(g_vu1Verts.data(), sizeof(gt3vif::Vertex), (uint32_t)n, &vb);
    const uint32_t bi = stream_indices(g_vu1Index.data(), (uint32_t)g_vu1Index.size(), &ib);
    float bmin[3], bmax[3];
    vu1_box(g_vu1Verts.data(), g_vu1Verts.size(), bmin, bmax);
    int aMin, aMax;
    vu1_alpha_range(g_vu1Verts.data(), g_vu1Verts.size(), aMin, aMax);
    vu1_draw(c.vu, c.prim, kUnlitHead, kZeroLight, vb, ib, (uint32_t)g_vu1Index.size(), bi, (int32_t)bv, bmin, bmax,
             0, ~0u, nullptr, aMin, aMax);
}

void exec_tri16(const CmdTri16 &c)
{
    if (c.nTris <= 0 || c.xyzw == nullptr)
        return;
    const int n = c.nTris * 3;
    g_vu1Verts.assign((size_t)n, gt3vif::Vertex());
    for (int i = 0; i < n; i++) {
        gt3vif::Vertex &v = g_vu1Verts[(size_t)i];
        memset(&v, 0, sizeof(v));
        for (int k = 0; k < 3; k++)
            v.pos[k] = v.pos2[k] = (float)c.xyzw[4 * i + k];
        if (c.st != nullptr) {
            v.st[0] = v.st2[0] = (float)c.st[2 * i] / 4096.0f;
            v.st[1] = v.st2[1] = (float)c.st[2 * i + 1] / 4096.0f;
        }
        const uint32_t col = c.rgba != nullptr ? c.rgba[i] : 0x80808080u;
        memcpy(v.rgba, &col, 4);
        memcpy(v.rgba2, &col, 4);
    }
    g_vu1Index.clear();
    for (int t = 0; t < c.nTris; t++) {
        g_vu1Index.push_back((uint32_t)(3 * t + 2));
        g_vu1Index.push_back((uint32_t)(3 * t));
        g_vu1Index.push_back((uint32_t)(3 * t + 1));
    }
    flush_batch();                          /* PC: before the stream writes (exec_strip) */
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    const uint32_t bv = stream_vertices(g_vu1Verts.data(), sizeof(gt3vif::Vertex), (uint32_t)n, &vb);
    const uint32_t bi = stream_indices(g_vu1Index.data(), (uint32_t)g_vu1Index.size(), &ib);
    float bmin[3], bmax[3];
    vu1_box(g_vu1Verts.data(), g_vu1Verts.size(), bmin, bmax);
    int aMin, aMax;
    vu1_alpha_range(g_vu1Verts.data(), g_vu1Verts.size(), aMin, aMax);
    vu1_draw(c.vu, c.prim, kUnlitHead, kZeroLight, vb, ib, (uint32_t)g_vu1Index.size(), bi, (int32_t)bv, bmin, bmax,
             0, ~0u, nullptr, aMin, aMax);
}

/* GT3PC_FLARE_LOG: the texel of every flare glow / result disc of the frame,
 * in draw order (= query order), logged at the frame's end. */
std::string g_flareGlows;
/* The flare queries' result strip of this frame (exec_flare_query): the glow
 * draws read their texel from it on the GPU (exec_model). */
Target *g_flareStrip = nullptr;

/* PC (2026-11-05): the resolved runs' recording of a part (below) */
void run_rec_part(const CmdModel &c, uint64_t meshSerial, ID3D11Buffer *vb, ID3D11Buffer *ib, ID3D11Buffer *bvb,
                  ID3D11Buffer *ib2, uint32_t nVerts, int32_t program, int32_t specExp, int32_t format, int overlay);
void run_rec_bad(const char *why);

void exec_model(const CmdModel &c)
{
    flush_batch();
    const gt3vif::Mesh *mesh = nullptr;
    ID3D11Buffer *vb = nullptr, *ib = nullptr, *bvb = nullptr, *ib2 = nullptr;
    uint32_t baseV = 0, baseI = 0, firstBatch = 0, endBatch = 0, nVerts = 0;
    uint64_t meshSerial = 0;
    g_profCount[PCOUNT_PARTS]++;
    bool found;
    {
        ProfScope prof(PROF_MESH);
        found = model_mesh(c.part, &mesh, &vb, &ib, &baseV, &baseI, &firstBatch, &endBatch, &bvb, &meshSerial, &ib2,
                           &nVerts);
    }
    if (!found) {
        if (g_runRec)
            run_rec_bad("a part without its mesh");
        return;
    }
    /* (PC, 2026-11-05: the constants' head for the submit stage, which
     * builds the block: Vu1Head) */
    Vu1Head head = {};
    int32_t program, e;
    if (!light_program(c.part.lightProg, program, e)) {
        char key[40];
        snprintf(key, sizeof(key), "pgl-vu1prog-%X", c.part.lightProg);
        log_once(key, "pgl: unknown VU1 lighting program 0x%X: drawn unlit", c.part.lightProg);
    }
    ID3D11ShaderResourceView *flareSrv = nullptr;
    if (program == VU1P_FLARE) {
        /* PC: a result disc's texel from the strip on the GPU (the queries
         * before it in this list wrote it; flare_readback no longer waits
         * for the GPU: docs/renderer.md 8); other flare draws (the unoccluded
         * discs) carry their texel */
        const bool gpu = c.part.flareStripH > 0;
        if (gpu) {
            Target *strip = g_flareStrip;
            const int q = c.part.flareQ;
            if (strip == nullptr || strip->base != c.part.flareStripBlock || q < 0 || (q >> 6) >= strip->gsH)
                return;                     /* (no query wrote it: 0, not drawn) */
            flareSrv = target_srv(*strip);
            if (flareSrv == nullptr)
                return;
            head.flareGpu = 1.0f;
            head.flareX = (float)(q & 0x3F) * strip->sx;
            head.flareY = (float)(q >> 6) * strip->sy;
        }
        /* (the CPU's copy: GT3PC_FLARE_LOG's readback, else stale) */
        const uint32_t o = c.part.flareResults != nullptr ? (c.part.flareResults[c.part.flareIndex] & 0xFF) : 0;
        if (c.part.part == 0 && g_flareGlows.size() < 400 && flare_log(false)) {
            char v[8];
            snprintf(v, sizeof(v), " %02X", o);
            g_flareGlows += v;
        }
        if (!gpu) {
            if (o == 0)
                return;
            head.flareO = (float)o;
        }
    }
    head.program = program;
    head.specExp = e;
    head.format = (int32_t)(mesh->mode >> 5);
    head.morph = c.part.morphWeight;
    head_hide(head, c.part);                /* PC (2026-11-07) */
    /* (PC, 2026-11-05: the lighting block stays in the command, the submit
     * stage reads it there: c.part.light) */
    const int overlay = model_overlay(c.part.model);    /* PC: docs/renderer.md "Overlays" */
    if (g_runRec)                           /* PC: (the resolved runs' recording) */
        run_rec_part(c, meshSerial, vb, ib, bvb, ib2, nVerts, program, e, head.format, overlay);
    g_traceProg = program;
    if (flareSrv != nullptr)
        q::run([flareSrv] { qdc()->VSSetShaderResources(0, 1, &flareSrv); });
    for (uint32_t bi = firstBatch; bi < endBatch && bi < mesh->batches.size(); bi++) {
        const gt3vif::Batch &bt = mesh->batches[bi];     /* PC: the part's batches only (model_mesh) */
        if (bt.part != (uint16_t)c.part.part || bt.indexCount == 0)
            continue;
        if (bt.flags & (gt3vif::BATCH_NO_GIFTAG | gt3vif::BATCH_NOT_TRIANGLES))
            continue;
        const uint32_t prim = ((uint32_t)bt.prim & ~c.part.primMask) | (c.part.primAttr & c.part.primMask);
        g_traceHasVcol = false;
        if (g_tracing && (mesh->attribs & gt3vif::ATTR_COLOR) != 0 && bt.vertexCount != 0 &&
            (size_t)bt.firstVertex + bt.vertexCount <= mesh->vertices.size()) {
            /* (GT3PC_PGL_TRACE) the batch's mean vertex colour */
            double sum[4] = {0, 0, 0, 0};
            for (uint32_t i = 0; i < bt.vertexCount; i++)
                for (int k = 0; k < 4; k++)
                    sum[k] += mesh->vertices[bt.firstVertex + i].rgba[k];
            for (int k = 0; k < 4; k++)
                g_traceVcol[k] = (float)(sum[k] / bt.vertexCount);
            g_traceHasVcol = true;
        }
        vu1_draw(c.vu, prim, head, c.part.light, vb, ib, bt.indexCount, baseI + bt.firstIndex, (int32_t)baseV,
                 bt.hasBounds ? bt.bmin : nullptr, bt.hasBounds ? bt.bmax : nullptr, overlay, bi, bvb,
                 bt.hasBounds ? bt.aMin : 0, bt.hasBounds ? bt.aMax : 255, bt.hasBounds ? bt.nMax2 : 1e30f, ib2,
                 nVerts);
    }
    if (flareSrv != nullptr) {
        /* (unbound: the strip is the next queries' output) */
        q::run([] {
            ID3D11ShaderResourceView *none = nullptr;
            qdc()->VSSetShaderResources(0, 1, &none);
        });
    }
    g_traceProg = -1;
    g_traceHasVcol = false;
}

/* ======================================================================== */
/* Resolved runs (PC, 2026-11-05; docs/renderer.md 4, "Resolved runs")     */
/* ======================================================================== */

/* A run of the list that draws models - a car: its Tex1 uploads, the
 * register writes between its parts and the parts themselves (CMD_MODEL),
 * ~300 commands - is resolved once and replayed at its next frames.  The
 * resolution (RunRec): the draw states the run's draws took (compute_state's
 * results, kept whole), where each draw bound a texture anew, and each
 * draw's batch (its indices, box, alpha range and PRIM, from the mesh).  A
 * replay walks the frame's own list again: the registers are written as
 * ever (the CLUT loads with them, exec_regs / write_reg), the uploads made,
 * and each part's draws submitted as vu1_draw submits them (vu1_submit) -
 * the matrices, lighting blocks and morph weights read from the frame's
 * commands, the textures bound again from the registers (texture_refresh:
 * a lamp's new CLUT, a set moved in VRAM), the state copied from the run
 * instead of worked out from the registers, the batches taken from the run
 * instead of the mesh.  The same draws with the same values: exact.
 *
 * What the resolution depended on is checked before a replay (run_try): the
 * run's structure - the commands with their variable fields masked (the
 * matrices, lighting, morph weights, the sets' VRAM places in TEX0 / TEX2
 * and the uploads' bases: the keys recorded with the commands, pgl_render.h
 * cmd_regs_key / cmd_model_key, mixed with the GS state at its start into
 * the run's key, run_scan_key), the targets of its states (the same
 * objects, sizes, scales, sample counts and alpha mirrors:
 * run_targets_same), the meshes of its models (the same decodes:
 * model_mesh's serial) and the overlays' generation.  Anything else makes
 * a miss: the run is resolved again while it executes as before (the
 * recording hooks in exec_model and vu1_draw), replacing the entry.
 * Not resolved: a run whose draw lacked its mesh or texture, drew into a
 * small target (whose resolution follows what it is drawn from) or whose
 * target took its alpha mirror during the run (its states then differ from
 * the next frame's); nor anything in a traced frame, with GT3PC_FLICKER or
 * GT3PC_PGL_SKIP, or without the alpha mirror (GT3PC_PGL_DATE_STENCIL=0:
 * the copies' bindings are per draw).  GT3PC_PGL_RUNS=0: off (exec_init). */

constexpr uint32_t kRunMinParts = 8;        /* (fewer parts: not worth resolving) */
constexpr uint32_t kRunMaxCmds = 8192;      /* (a run cut there) */
constexpr size_t kRunMaxRuns = 2048;        /* (the least recently replayed go beyond) */
constexpr double kRunExpire = 10.0;         /* seconds unused */

/* TEX0 / TEX2: TBP0 (bits 0..13) and CBP (37..50) - a set's VRAM place
 * (pgl_render.h: the keys recorded with the commands mask them so) */
constexpr uint64_t kTexAddrMask = kCmdTexAddrMask;

/* The GS state at a run's start as the run depends on it (the vertex
 * registers aside: no GS primitive in a run) */
struct GsEntry {
    uint64_t ctx[2][gs::REG_COUNT];
    uint64_t gen[gs::GEN_COUNT];
    uint64_t prmode;
    uint32_t prim;
    uint32_t fullColor;
};

void gs_entry(GsEntry &e)
{
    memcpy(e.ctx, g.ctx, sizeof(e.ctx));
    memcpy(e.gen, g.gen, sizeof(e.gen));
    for (int c = 0; c < 2; c++) {
        e.ctx[c][gs::REG_TEX0] &= kTexAddrMask;
        e.ctx[c][gs::REG_TEX2] &= kTexAddrMask;
    }
    e.prmode = g.prmode;
    e.prim = g.prim;
    e.fullColor = g.fullColor ? 1u : 0u;
}

/* One draw of a part (vu1_draw's call): what it took of the state (event:
 * STATE_*; state - the whole state, by index), its PRIM and its batch;
 * vuGroup - its matrix group (RunVuGroup) */
struct RunStep {
    uint8_t event, hasBounds, aMin, aMax;
    uint16_t state, batch;
    uint16_t vuGroup;
    uint16_t tex;                           /* PC: its texture's record (RunRec::texs; 0xFFFF: resolved at each replay) */
    uint16_t pal[2];                        /* PC: the run's CLUT loads its palette's rows came from (0xFFFF: none) */
    uint32_t indexCount, firstIndex, prim;
    float bmin[3], bmax[3], nMax2;
    /* PC (2026-11-06, step 4): the registers its texture is resolved from, as
     * the walk had them (every value but a TEX0's address is the key's: the
     * replay sets them without the walk, run_regs_at_step) */
    uint64_t tex1, clamp, frame, texa;
};
/* Consecutive draws with one program state (the body's parts share the
 * car's matrix): the box of all their batches - tested against the clipper's
 * planes once a frame for them all (inside: every batch is, by convexity;
 * vu1_submit draws them without the clipper and takes the box's area as
 * theirs) where the frame's draws share the matrix as the recording's did */
struct RunVuGroup {
    float bmin[3], bmax[3];
    bool hasBox;
};
struct RunPart {
    uint32_t cmd;                           /* the command in the run */
    uint32_t firstStep, nSteps;
    uint16_t model;                         /* RunRec::models */
    int16_t overlay;
    int32_t program, specExp, format;
    uint32_t lastPrim;                      /* the PRIM its last batch set (hasPrim), drawn or not: the GS state after */
    bool hasPrim;
};
struct RunModel {
    const void *model;
    uint64_t serial;                        /* model_mesh's */
    ID3D11Buffer *vb, *ib, *bvb;
    uint32_t cmd;                           /* a command whose part is the model's (the check) */
    ID3D11Buffer *ib2;                      /* PC (2026-11-06): its doubled index stream (or null) */
    uint32_t nVerts;
};
/* The targets of a state (one entry per (rt, zt) pair): how they were
 * looked up and what they were */
struct RunTargets {
    TargetArgs args;
    Target *rt, *zt;
    int nw, nh, samples, znw, znh, zsamples;
    float sx, sy;
    bool gsRes, dated;
};
struct RunState {
    BatchState bs;
    uint16_t targets;                       /* RunRec::targets */
    /* PC (2026-11-06): the stage's note of the largest mirror pass (native
     * pixels) the state's draws needed the last time it was walked - the
     * executor's hint for letting the mirror go at the state instead
     * (work_state_fill; g_mirrorLazyOn).  Written by the stage's thread,
     * read by the executor a frame later: a hint, never a value anything
     * exact depends on (an aligned word; a stale read costs a pass or a
     * sync, never a pixel). */
    uint32_t passArea = 0;
};
/* PC (2026-11-05): a step's texture as its state found it, with the identity
 * of what the bind read (pgl_render.h TexIdent): a replay binds it from here
 * while the identity holds ("The textures of a run" below) */
struct RunTex {
    TexIdent id;
    TexBinding out;
    ID3D11SamplerState *samp;
    float texW, texH;
    float texX0, texY0, texInvW, texInvH;
    uint32_t texFlags;                      /* the state's PS flags of the texture (kTexFlags) */
    float ta0, ta1;
};
/* A CLUT load of the run (a TEX0 / TEX2 write with CLD), in order: what it
 * read, and the rows it wrote */
struct RunClut {
    ClutIdent id;
};
WorkPool<RunTex> g_workTexs;                /* (a frame's textures resolved at a replay for the stage's works) */
void run_tex_fill(RunTex &t, const BatchState &b);
/* PC (2026-11-06, step 4 - the register walk from the record): the commands
 * of a run a replay still visits, in order: a register flush or A+D write of
 * TEX0 / TEX2 (their addresses are this frame's: the CLUT loads, the
 * textures) or TEXFLUSH (its effect is the state's), a set's upload, a part.
 * Every other register write is the key's: its end value is the record's
 * (RunRec::end). */
enum : uint16_t { OP_REGS = 1, OP_AD, OP_UPLOAD, OP_SOURCE, OP_PART };
struct RunOp {
    uint32_t cmd;                           /* the command in the run */
    uint16_t kind;                          /* OP_* */
    uint16_t part;                          /* OP_PART: RunRec::parts */
};
struct RunRec {
    uint64_t key = 0;                       /* run_key: the structure's hash */
    uint32_t nCmds = 0;
    uint32_t overlayGen = 0;
    int aniso = 1;                          /* PC (2026-11-10): g_aniso as recorded (its states' samplers, PSF_ANISO) */
    std::vector<RunModel> models;
    std::vector<RunTargets> targets;
    std::vector<RunState> states;
    std::vector<RunPart> parts;
    std::vector<RunStep> steps;
    std::vector<RunVuGroup> vuGroups;
    std::vector<RunTex> texs;               /* PC: the steps' textures (RunStep::tex) */
    std::vector<RunClut> cluts;             /* PC: the run's CLUT loads (RunStep::pal) */
    std::vector<RunOp> ops;                 /* PC (step 4): the commands a replay visits */
    uint64_t opsHash = 0;                   /* PC (2026-11-08): the ops' and parts' bytes as recorded (run_record_sound) */
    std::string damage;                     /* PC (2026-11-08): an op a replay found out of range (run_op_damaged) */
    double recorded = 0.0;                  /* PC (2026-11-08): (g_execSeconds; the damaged record's log line) */
    GsEntry end = {};                       /* PC (step 4): the GS state the run leaves (TEX0 / TEX2 addresses aside) */
    bool workOk = true;                     /* PC: its draws may go to the stage as one record (RunWork) */
    std::atomic<uint32_t> pending{0};       /* PC: works of it the stage has not run yet (texs not rewritten then) */
    uint32_t cmdCounts[32] = {};            /* PC: its commands by kind (the stats line, counted once a replay) */
    bool bad = false;                       /* (recording: not to be kept) */
    const char *why = nullptr;
    double lastUse = 0.0;
    uint32_t hits = 0;
    PglrVu1 lastVu = {};                    /* (recording: the last draw's program state, its group) */
    bool lastVuValid = false;
};

std::unordered_map<uint64_t, std::unique_ptr<RunRec>, KeyHash> g_runs;    /* (by run_key) */
/* PC (2026-11-05): runs replaced within a frame, kept until it ends (the
 * stage may still read a work of theirs: RunWork) */
std::vector<std::unique_ptr<RunRec>> g_runGrave;
bool g_runsOn = true;
uint32_t g_runCmd;                          /* (recording) the command's index in the run */
size_t g_runStart, g_runEnd;                /* (recording) the run's commands in the list */
uint32_t g_runsBad;                         /* runs not kept (the stats line) */
uint32_t g_runsDamaged;                     /* PC (2026-11-08): records found written over (run_record_sound) */
int g_runDamageTest = 0;                    /* PC (2026-11-08): GT3PC_PGL_RUN_DAMAGE=1 an op, 2 a part (run_record_sound's test) */
bool g_runCheckOn = true;                   /* PC (2026-11-08): GT3PC_PGL_RUN_CHECK=0: no check (its cost compared) */
std::unique_ptr<RunRec> g_runRec;           /* the run being recorded (vu1_draw, exec_model) */

bool run_cache_on()
{
    /* (traced frames replay too - the trace shows the replayed draws, with
     * a line per run; GT3PC_FLICKER and GT3PC_PGL_SKIP look at each draw as
     * the normal path makes it) */
    return g_runsOn && g_dateStencil && !flicker_on() && !skip_on();
}

/* PC (2026-11-05): a run's key seen in a recent frame (kRunSeen entries by
 * the key): a run is resolved only when its key comes round again - a
 * run whose structure changes every frame (the course's parts, culled by
 * the camera) is never resolved, and never pays the recording */
struct RunSeen {
    uint64_t key = 0, frame = 0;
};
constexpr uint32_t kRunSeen = 4096;
RunSeen g_runSeen[kRunSeen];

/* ---- The textures of a run (PC, 2026-11-05) ----------------------------
 *
 * A replay resolved its parts' textures as the normal path does: every TEX0
 * through the owner map to its set, the CLUT buffer loaded at every TEX0
 * write with CLD, the palette's key from the buffer's rows, the by-set front
 * probed - CLUT LOAD and TEX LOOKUP 0.86 ms of a 32-car race frame's 2.3 ms
 * (racebench), and the walk around them (texture_refresh, state_texture,
 * the sampler) more.  That is the GS texture emulation the port still does:
 * the registry's, on the executor; the GPU side is native (decoded textures,
 * D3D samplers, a texel rectangle per draw).
 *
 * Now a step's texture is recorded with the identity of what its bind read
 * (pgl_render.h TexIdent: the set and TBP0's place in it, the uploads under
 * the texture; or the target), and the run's CLUT loads with theirs
 * (ClutIdent: the set and CBP's place in it, the fields, the uploads under
 * the CLUT), each step naming the loads that wrote the rows its palette
 * reads (RunStep::pal).  A replay checks the identities against the live
 * registers and the owner map (a few compares a step: texture_ident_check,
 * clut_ident_check) and binds the recorded texture (run_tex_apply); its
 * CLUT loads are deferred (g_clutDefer), each checked and numbered as its
 * write comes (run_clut_deferred).  A step whose identity or palette fails
 * - a set parsed again, a lamp's CLUT changed under the texture, a decoded
 * texture dropped (g_tagEpoch) - resolves its texture as before, after the
 * deferred loads whose rows are still theirs are done in their order
 * (run_clut_catch_up: the rows then are the walk's), and takes its record
 * anew; the other steps go on from theirs.  At the run's end the surviving
 * loads are done the same way, so the CLUT buffer holds what the walk would
 * leave.  Exact: a bind whose identity holds reads the same texels through
 * the same palette, so it is the same decoded texture.
 * GT3PC_PGL_RUN_TEX=0: the textures resolved at every replay as before
 * (exec_init). */
bool g_runTexOn = true;
bool g_clutDefer = false;                   /* a replay: CLUT loads checked against the run's, not done */
int16_t g_rowWriter[32];                    /* the run's load that wrote each CLUT row (-1: before the run) */
bool g_rowDone[32];                         /* (a replay) the row's writer was done for real */
std::vector<uint64_t> g_clutLive;           /* (a replay) the deferred loads' TEX0 as written */
std::vector<uint8_t> g_clutOk;              /* (a replay) each deferred load's identity holds */
uint32_t g_runClutN;                        /* the run's loads so far */
uint32_t g_runClutDone;                     /* (a replay) of them, done for real */
bool g_runClutBad;                          /* (a replay) more loads than recorded: nothing more from the records */
RunRec *g_runReplay = nullptr;              /* the run being replayed */

void run_rows_reset()
{
    for (int i = 0; i < 32; i++) {
        g_rowWriter[i] = -1;
        g_rowDone[i] = true;
    }
    g_runClutN = 0;
    g_runClutDone = 0;
    g_runClutBad = false;
}

/* (recording) the load just done (write_reg, run_clut_load): its identity,
 * the rows it wrote */
void run_rec_clut()
{
    RunRec &r = *g_runRec;
    if (r.bad)
        return;
    if (r.cluts.size() >= 0xFFFE) {
        run_rec_bad("too many CLUT loads");
        return;
    }
    RunClut c;
    clut_last_ident(c.id);
    for (uint32_t rows = c.id.rows; rows != 0; rows &= rows - 1) {
        unsigned long i;
        _BitScanForward(&i, rows);
        g_rowWriter[i] = (int16_t)r.cluts.size();
    }
    r.cluts.push_back(c);
}

void run_clut_catch_up();

/* (a replay) a load's write came: numbered, checked against its record,
 * deferred */
void run_clut_deferred(uint64_t tex0)
{
    if (!clut_load_indexed(tex0))
        return;
    RunRec &r = *g_runReplay;
    const uint32_t k = g_runClutN++;
    if (k >= r.cluts.size()) {
        /* (more loads than the recording had - the key fixes them, so never:
         * done for real from here on, and nothing more from the records) */
        run_clut_catch_up();
        g_clutDefer = false;
        g_runClutBad = true;
        registry_clut_load(tex0);
        return;
    }
    g_clutLive[k] = tex0;
    g_clutOk[k] = clut_ident_check(r.cluts[k].id, tex0) ? 1 : 0;
    for (uint32_t rows = r.cluts[k].id.rows; rows != 0; rows &= rows - 1) {   /* (its rows: a bit each) */
        unsigned long i;
        _BitScanForward(&i, rows);
        g_rowWriter[i] = (int16_t)k;
        g_rowDone[i] = false;
    }
}

/* (a replay) the deferred loads whose rows are still theirs, done for real
 * in their order - the CLUT buffer's rows are then as the walk would have
 * them now (a later load's rows written after an earlier one's, as they
 * were; a load every row of which a later one rewrote is never seen).  A
 * failed identity is taken anew from the real load (it holds next time). */
void run_clut_catch_up()
{
    RunRec &r = *g_runReplay;
    uint16_t todo[32];
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (g_rowDone[i] || g_rowWriter[i] < 0)
            continue;
        const uint16_t w = (uint16_t)g_rowWriter[i];
        int j = 0;
        while (j < n && todo[j] < w)
            j++;
        if (j < n && todo[j] == w)
            continue;
        memmove(todo + j + 1, todo + j, (size_t)(n - j) * sizeof(todo[0]));
        todo[j] = w;
        n++;
    }
    for (int j = 0; j < n; j++) {
        const uint16_t w = todo[j];
        const uint32_t s0 = clut_load_serial();
        registry_clut_load(g_clutLive[w]);
        g_runClutDone++;
        if (!g_clutOk[w] && clut_load_serial() != s0)
            clut_last_ident(r.cluts[w].id);
    }
    for (int i = 0; i < 32; i++)
        g_rowDone[i] = true;
}

/* A step's texture as its state found it (b), with the identity of what the
 * bind read (RunTex) and the run's loads its palette's rows came from
 * (RunStep::pal): recorded, or taken anew after a replay resolved it (the
 * step's record then stands for the next replay).  Not recorded (the step
 * resolves its texture at every replay): a texture not a set's nor a
 * target's (kind 0), the target being drawn or its Z buffer (their copies
 * are the draw's own), a palette with rows loaded before the run, or from
 * three loads. */
void run_rec_tex(RunRec &r, RunStep &s, const BatchState &b)
{
    RunTex t;
    texture_last_ident(t.id);
    bool ok = t.id.kind != 0 && !(b.tex.target != nullptr && (b.tex.target == b.rt || b.tex.target == b.zt));
    uint16_t pal[2] = {0xFFFF, 0xFFFF};
    if (ok && clut_load_indexed(b.tex0)) {
        const gs::Tex0 t0 = gs::decodeTex0(b.tex0);
        const bool t8 = t0.psm == gstex::PSMT8 || t0.psm == gstex::PSMT8H;
        const bool ct16 = t0.cpsm == gstex::PSMCT16 || t0.cpsm == gstex::PSMCT16S;
        const uint32_t rows = t8 ? 16 : 1;  /* (the palette's rows: palette_key's) */
        int n = 0;
        for (uint32_t j = 0; j < rows && ok; j++) {
            int w[2];
            if (ct16) {
                w[0] = w[1] = g_rowWriter[(t0.csa + j) & 31];
            } else {
                w[0] = g_rowWriter[(t0.csa + j) & 15];
                w[1] = g_rowWriter[((t0.csa + j) & 15) + 16];
            }
            for (int k = 0; k < 2 && ok; k++) {
                if (w[k] < 0) {
                    ok = false;             /* (rows loaded before the run) */
                } else if (!(n > 0 && pal[0] == w[k]) && !(n > 1 && pal[1] == w[k])) {
                    if (n == 2)
                        ok = false;         /* (three loads) */
                    else
                        pal[n++] = (uint16_t)w[k];
                }
            }
        }
    }
    if (!ok) {
        if (b.tex.target != nullptr && (b.tex.target == b.rt || b.tex.target == b.zt))
            r.workOk = false;               /* (its copy must come at the draw: not one record for the run, RunWork) */
        /* (the first few, to the log: what kind of texture stays resolved
         * at every replay) */
        static int logged = 0;
        if (logged < 8 && s.tex == 0xFFFF) {
            logged++;
            const gs::Tex0 t0 = gs::decodeTex0(b.tex0);
            pclog::write("pgl runs: a part's texture resolved at every replay: %s (TEX0 psm 0x%X at block 0x%X,"
                         " CBP 0x%X CSA %u; %s)%s",
                         t.id.kind == 0 ? "not a set's nor a target's" :
                         b.tex.target != nullptr && (b.tex.target == b.rt || b.tex.target == b.zt)
                             ? "the target drawn" : "its palette's rows loaded before the run, or from three loads",
                         t0.psm, t0.tbp0, t0.cbp, t0.csa,
                         b.tex.target != nullptr ? "a target" : b.tex.fromSet ? "a set's" : "other",
                         logged == 8 ? " (no more of these lines)" : "");
        }
        s.tex = 0xFFFF;
        s.pal[0] = s.pal[1] = 0xFFFF;
        return;
    }
    run_tex_fill(t, b);
    s.pal[0] = pal[0];
    s.pal[1] = pal[1];
    if (s.tex != 0xFFFF && s.tex < r.texs.size()) {
        r.texs[s.tex] = t;
    } else if (r.texs.size() < 0xFFFF) {
        s.tex = (uint16_t)r.texs.size();
        r.texs.push_back(t);
    } else {
        s.tex = 0xFFFF;
    }
}

/* The texture part of state b (as state_texture left it) into record t */
void run_tex_fill(RunTex &t, const BatchState &b)
{
    t.out = b.tex;
    t.samp = b.samp;
    t.texW = b.texW;
    t.texH = b.texH;
    t.texX0 = b.ps.texX0;
    t.texY0 = b.ps.texY0;
    t.texInvW = b.ps.texInvW;
    t.texInvH = b.ps.texInvH;
    t.texFlags = b.ps.flags & kTexFlags;
    t.ta0 = b.ps.ta0;
    t.ta1 = b.ps.ta1;
}

/* Record t's texture into state b (the state's texture part as
 * texture_refresh sets it); srv: the view to bind instead of the record's
 * (a target's, as of this frame), or null.  (The executor's and the submit
 * stage's: run_tex_apply, run_submit.) */
void tex_apply(const RunTex &t, BatchState &b, ID3D11ShaderResourceView *srv)
{
    b.tex = t.out;
    if (srv != nullptr)
        b.tex.srv = srv;
    b.samp = t.samp;
    b.texW = t.texW;
    b.texH = t.texH;
    b.ps.texX0 = t.texX0;
    b.ps.texY0 = t.texY0;
    b.ps.texInvW = t.texInvW;
    b.ps.texInvH = t.texInvH;
    b.ps.flags = (b.ps.flags & ~kTexFlags) | t.texFlags;
    b.ps.ta0 = t.ta0;
    b.ps.ta1 = t.ta1;
}

/* (a target texture's view as of this frame: a multisampled target's
 * resolve, when it changed - the executor's call, in frame order) */
inline ID3D11ShaderResourceView *run_tex_srv(const RunTex &t)
{
    if (t.id.kind != 2)
        return nullptr;
    if (g_work != nullptr)
        run_work_cut_if(t.id.target);       /* (if this run wrote it: its draws so far before the resolve) */
    return target_srv(*t.id.target);
}

/* (a replay) the step's texture from its record into the executor's state */
void run_tex_apply(const RunTex &t, BatchState &b, uint64_t tex0)
{
    tex_apply(t, b, run_tex_srv(t));
    b.tex0 = tex0;
    g_subPosted = false;                    /* (the submit stage gets the state again) */
    g_fbPending = false;
    g_fbRects.clear();
    g_segStart = 0;
}

/* (a replay for the stage's work, RunWork) the texture the draws from here
 * on bind: the record and a target's view as of this frame */
const RunTex *g_workTex = nullptr;
ID3D11ShaderResourceView *g_workSrv = nullptr;

/* (a run's work) before a texture is resolved after all: the run's books
 * done (the bind reads them), and the work so far posted if the bind may
 * copy or resolve a target the run writes (the live TBP0 lies in the target
 * drawn, or names a Z texture): that copy must come after the run's draws */
void run_work_before_bind(const BatchState &b)
{
    const int ctxt = (g.prim >> 9) & 1;
    const uint64_t tex0 = g.ctx[ctxt][gs::REG_TEX0];
    const uint32_t psm = (uint32_t)(tex0 >> 20) & 0x3F;
    int ox = 0, oy = 0;
    Target *t = gstex::psm_depth(psm) ? b.zt : registry_colour_target_at((uint32_t)tex0 & 0x3FFF, ox, oy);
    if (t != nullptr && (t == b.rt || t == b.zt))
        run_work_cut();
    else if (t != nullptr)
        run_work_cut_if(t);                 /* PC (2026-11-06): (another target the run wrote: the same) */
    else
        run_book_flush();
}

/* PC (2026-11-06, step 3; a replay for the stage's walk) the texture of the
 * step at hand for the stage: a target texture's view noted once per work
 * (WorkView; a note when the work's views are full), a fresh resolve noted
 * for its step (WorkNote) */
void work_view_note(const RunTex &t, ID3D11ShaderResourceView *srv)
{
    RunWork &w = *g_work;
    if (t.id.kind != 2)
        return;
    for (uint32_t i = 0; i < w.nViews; i++)
        if (w.views[i].t == t.id.target)
            return;
    if (w.nViews < 4) {
        w.views[w.nViews++] = {t.id.target, srv};
        return;
    }
    WorkNote &n = w.notes[w.nNotes++];
    n.step = g_runStepCur;
    n.tex = &t;
    n.srv = srv;
    n.skip = g_bs.skip;
}

void work_tex_note(const RunTex *t, bool skip)
{
    RunWork &w = *g_work;
    WorkNote &n = w.notes[w.nNotes++];
    n.step = g_runStepCur;
    n.tex = t;
    n.srv = nullptr;
    n.skip = skip;
}

/* (a replay) a step's texture: from its record while the identities hold,
 * else resolved as before - after the deferred loads are caught up - and
 * recorded anew.  For a run's work the record is noted for the stage
 * (g_workTex) instead of applied to the executor's state. */
void run_replay_tex(RunRec &r, RunStep &s, BatchState &b)
{
    const bool work = g_work != nullptr;
    if (!g_clutDefer && !g_runClutBad) {    /* (GT3PC_PGL_RUN_TEX=0: as before) */
        if (work)
            run_work_before_bind(b);
        texture_refresh(b);
        if (!b.skip && b.tme && b.tex.target != nullptr)
            target_read_by_vu1(b.tex.target, g_traceProg);  /* PC (2026-11-06): ([Graphics] EnvMapDetail; a fresh pointer) */
        if (work) {
            RunTex *o = nullptr;
            if (!b.skip && b.tme) {
                o = g_workTexs.alloc(1);
                o->id.kind = 0;
                run_tex_fill(*o, b);
            }
            g_workTex = o;
            g_workSrv = nullptr;
            work_tex_note(o, b.skip);       /* PC (step 3): (the stage's walk takes it from here) */
        }
        return;
    }
    int why = PCOUNT_RUN_WALK_NOREC;
    if (s.tex != 0xFFFF && !g_runClutBad) {
        const RunTex &t = r.texs[s.tex];
        const int ctxt = (g.prim >> 9) & 1;
        const uint64_t tex0 = g.ctx[ctxt][gs::REG_TEX0];
        bool ok = texture_ident_check(t.id, tex0, g.ctx[ctxt][gs::REG_CLAMP], g.gen[gs::GEN_TEXA]);
        why = PCOUNT_RUN_WALK_IDENT;
        for (int k = 0; k < 2 && ok; k++) {
            const uint16_t p = s.pal[k];
            if (p != 0xFFFF) {
                ok = p < g_runClutN && g_clutOk[p] != 0;
                why = PCOUNT_RUN_WALK_PAL;
            }
        }
        if (ok) {
            if (t.id.kind == 2 && t.id.target != nullptr)
                target_read_by_vu1(t.id.target, g_traceProg);   /* PC (2026-11-06): ([Graphics] EnvMapDetail; the identity
                                                                 * just checked names a live target) */
            if (work) {
                g_workTex = &t;
                g_workSrv = run_tex_srv(t); /* (may cut the work: g_work is the new one after) */
                work_view_note(t, g_workSrv);   /* PC (step 3) */
                /* PC (2026-11-18): and the executor's state the record's too,
                 * as run_tex_apply below.  The state is a copy of the run's
                 * (run_replay_part: RunState::bs), its texture as the state was
                 * recorded, and run_rec_tex renews only the step's record: once
                 * the texture's target had gone (a car's environment map at a
                 * new block, the old target retired) and the record was taken
                 * anew from the new one, the state still named the old target,
                 * freed - submit_prologue's env-pass note (target_read_by_vu1:
                 * envRead = true) wrote into its memory, since taken by other
                 * objects: run records found written over (a command index +
                 * 0x10000 - byte 178 of a 208-byte block, a Target's envRead;
                 * 2026-11-08, 2026-11-18) and the user's crash in D3D11's
                 * Flush (2026-11-18); the executor's other reads of it (the
                 * alpha bounds, the samples) read freed memory.  The stage
                 * draws from the record (WorkState / WorkStep tex): its
                 * pictures as before. */
                tex_apply(t, b, g_workSrv);
                b.tex0 = tex0;
            } else {
                run_tex_apply(t, b, tex0);
            }
            g_profCount[PCOUNT_RUN_TEXFREE]++;
            t_stats->runTexFree++;
            return;
        }
    }
    g_profCount[why]++;
    if (g_clutDefer)
        run_clut_catch_up();
    if (work)
        run_work_before_bind(b);            /* (a bind reads the targets' books, may copy the target drawn) */
    texture_refresh(b);
    if (!b.skip && b.tme && b.tex.target != nullptr)
        target_read_by_vu1(b.tex.target, g_traceProg);  /* PC (2026-11-06): ([Graphics] EnvMapDetail; a fresh pointer) */
    g_profCount[PCOUNT_RUN_WALK]++;
    t_stats->runTexWalk++;
    RunTex *o = nullptr;
    if (!b.skip && b.tme) {
        /* (the record anew - unless a work of the run is still to be read
         * by the stage, which the records' vector must not move under.
         * PC (2026-11-06): nor may it move under this replay's own work,
         * which pending does not count until it is posted - its states and
         * notes hold the records' addresses (WorkState::tex, WorkNote::tex)
         * - so a new record goes in only while the vector has room for it
         * (run_try reserves some before a replay; the step's own record may
         * be rewritten in place: nothing of this replay points at it yet).
         * The user's crash: a worker applied a record whose vector had moved,
         * memory since taken by another run's parts) */
        if (r.pending.load(std::memory_order_acquire) == 0 &&
            (!work || s.tex != 0xFFFF || r.texs.size() < r.texs.capacity()))
            run_rec_tex(r, s, b);
        if (work) {
            o = g_workTexs.alloc(1);
            o->id.kind = 0;
            run_tex_fill(*o, b);
        }
    }
    if (work) {
        g_workTex = o;                      /* (null: untextured, or skipped - the note says) */
        g_workSrv = nullptr;
        work_tex_note(o, b.skip);           /* PC (step 3): (the stage's walk takes it from here) */
    }
}

/* (the executor) a state's note for the stage (WorkState): post_state's views
 * and, PC (2026-11-06, step 3), the verdicts the stage's walk takes as the
 * state's - whether the stage walks it at all (not a state whose first draw
 * syncs the alpha mirror: the sync's place is that draw's, which only a walk
 * of the draws finds), its draws' mirror flags (as submit_prologue finds them
 * for a draw with no sync before it), the reflection skip's limit, the
 * texture in effect and the skip the resolve left (g_workTex, g_bs.skip: the
 * caller sets them again after the state's texture event) */
void work_state_fill(WorkState &ws, uint16_t state)
{
    const BatchState &b = g_bs;
    state_views(b, ws);                     /* (first: draw_dsv may move the mirror) */
    ws.state = state;
    /* ([Graphics] EnvMapDetail: the texture's target is marked as an env pass's
     * by run_replay_tex, once its identity is checked - the state's copy may
     * name a target gone since the recording) */
    const bool dated = g_dateStencil && !b.depthFrame && b.rt != nullptr && b.rt->dated;
    const bool sync = dated && b.dateStencil && !b.rt->mirrorValid;
    ws.walk = g_runWalk && !sync;
    ws.mirrorRange = mirror_on(b);
    ws.mirror = dated && b.rt->mirrorValid;
    /* PC (2026-11-06): the lazy mirror - a state whose draws needed a mirror
     * pass over an eighth of the target or more the last time (the stage's
     * note, RunState::passArea) lets the mirror go (never a DATE state: as
     * submit_prologue) */
    ws.noPass = false;
    if (ws.mirror && g_mirrorLazyOn && !b.dateStencil && mirror_quiet(*b.rt) && g_work != nullptr &&
        state < g_work->r->states.size() && g_work->r->states[state].passArea >= mirror_drop_px(*b.rt)) {
        target_mirror_invalidate(*b.rt);
        g_profCount[PCOUNT_MIRROR_DROP]++;
        ws.mirror = false;
    }
    ws.reflLim = refl_limit(b);
    ws.tex = g_workTex;
    ws.srv = g_workSrv;
    ws.skip = b.skip;
}

/* (a replay for the stage's work) the executor's half of a draw - its area
 * and clipping, whether it is drawn at all, its mirror flags, the
 * bookkeeping of what it writes (as submit_vu1) - noted as a WorkStep */
void submit_vu1_work(RunWork &w, const RunStep &s, uint16_t partIdx, const RunPart &part, const CmdModel &c)
{
    WorkStep st;
    st.flags = 0;
    bool hasArea;
    if (g_runHint.valid) {
        st.area = g_runHint.rect;
        st.flags = WF_HASAREA | WF_INSIDE;
        hasArea = true;
        g_clipWhy = 0;
    } else {
        bool inside = false;
        hasArea = vu1_area(g_bs, c.vu, s.hasBounds ? s.bmin : nullptr, s.hasBounds ? s.bmax : nullptr, st.area,
                           &inside);
        if (!hasArea)
            st.area = {};
        st.flags = (hasArea ? WF_HASAREA : 0) | (inside ? WF_INSIDE : 0);
    }
    const bool direct = (st.flags & WF_INSIDE) != 0 && part.program != VU1P_FLARE && !g_noDirect;
    count_direct(direct);
    if (draw_skipped(true, part.program))
        return;
    if (reflection_skipped(part.program, hasArea, st.area))
        return;
    ID3D11ShaderResourceView *dateSrv = nullptr;
    bool mirrorRange = false, mirror = false, noPass = false;
    MirrorIn in;                            /* PC (2026-11-06): (the lazy mirror's verdict) */
    in.lit = true;
    in.program = part.program;
    in.specExp = part.specExp;
    in.L = c.part.light;
    in.aMin = s.aMin;
    in.aMax = s.aMax;
    in.n2 = s.nMax2;
    submit_prologue(hasArea ? &st.area : nullptr, true, dateSrv, mirrorRange, mirror, in, noPass, false);
    RunWork &wk = *g_work;                  /* (the prologue's mirror sync may have cut the work: the new one) */
    (void)w;
    st.flags |= (mirror ? WF_MIRROR : 0) | (mirrorRange ? WF_MIRROR_RANGE : 0) | (noPass ? WF_NOPASS : 0);
    if (g_bs.wroteTop && g_bs.rt != nullptr) {
        /* (step 2: the targets whose alpha the run writes - their alphaPos after the run's record) */
        bool known = false;
        for (Target *t : wk.topTargets)
            known |= t == g_bs.rt;
        if (!known)
            wk.topTargets.push_back(g_bs.rt);
    }
    st.tex = g_workTex;
    st.srv = g_workSrv;
    st.indexCount = s.indexCount;
    st.firstIndex = s.firstIndex;
    st.step = g_runStepCur;
    st.part = partIdx;
    st.state = (uint16_t)(wk.nStates - 1);
    st.batch = s.batch;
    st.aMin = s.aMin;
    st.aMax = s.aMax;
    st.n2 = s.nMax2;
    wk.steps[wk.nSteps++] = st;
}

/* An A+D write a run may hold: a state register (the context ones, the
 * general ones, PRIM, PRMODE), SIGNAL / FINISH / LABEL - no vertex, no
 * transfer */
bool ad_in_run(uint32_t addr)
{
    addr &= 0xFF;
    if (addr >= 0x80)
        return false;
    if (g_addrReg[addr] >= 0 || g_addrGen[addr] >= 0)
        return true;
    return addr == A_PRIM || addr == A_PRMODE || addr >= 0x60;
}

} // namespace

/* PC (2026-11-06): (the game thread, pglr_ad) the write's key for the run
 * scan, so that the scan mixes one key a command and tests none of the
 * address rules itself (run_scan_key: ~12 % of the executor after step 4,
 * two mixes and the tests for every A+D write of a 32-car frame) */
uint64_t cmd_ad_key(uint32_t addr, uint64_t value)
{
    const uint32_t a = addr & 0xFF;
    if (!ad_in_run(a))
        return 0;
    if (a < 0x80 && g_addrReg[a] >= 0 && (g_addrReg[a] == gs::REG_TEX0 || g_addrReg[a] == gs::REG_TEX2))
        value &= kTexAddrMask;
    return cmd_mix(0x1000 | a, value) | 1;
}

namespace {

/* The run starting at command i: its end (i: none - the command cannot
 * start one), its parts, and its key - the entry state, the commands'
 * structure keys (CmdRegs::key, CmdModel::key: worked out on the game
 * thread as they were recorded; an A+D write's here) and the count, hashed
 * in the same pass.  A Tex1 upload after a part begins the next run (the
 * next car's). */
size_t run_scan_key(const CmdList &list, size_t i, uint32_t &nParts, uint64_t &key)
{
    nParts = 0;
    uint64_t h = 0x2545F4914F6CDD1Dull;
    size_t j = i;
    for (; j < list.size() && j - i < kRunMaxCmds; j++) {
        const CmdType t = list.type(j);
        if (t == CMD_REGS) {
            h = cmd_mix(h, list.key(j));    /* PC: (the key beside the types: CmdList::key) */
            continue;
        }
        if (t == CMD_AD) {
            /* PC (2026-11-06): its key as recorded (cmd_ad_key: 0 - a write a
             * run may not hold) */
            const uint64_t k = list.key(j);
            if (k == 0)
                break;
            h = cmd_mix(h, k);
            continue;
        }
        if (t == CMD_MODEL) {
            const CmdModel &c = *static_cast<const CmdModel *>(list.payload(j));
            if (c.part.preXyz != nullptr || c.part.lightProg == 0x2F7 || c.part.flareStripH > 0 ||
                c.part.flareResults != nullptr)
                break;
            /* (a model the game writes frame after frame - the tyre packets,
             * rebuilt in place - is decoded again each time: its part stays
             * outside, the run ends before it) */
            if (model_volatile(c.part.model)) {
                if (j == i)
                    j++;                    /* (the part itself: no run) */
                break;
            }
            nParts++;
            h = cmd_mix(h, list.key(j));
            continue;
        }
        if (t == CMD_TEX1_UPLOAD) {
            if (nParts != 0)
                break;
            h = cmd_mix(h, 0x2000 ^ (uint64_t)(uintptr_t)static_cast<const CmdTex1 *>(list.payload(j))->img);
            continue;
        }
        if (t == CMD_TEX_SOURCE) {
            const CmdTexSource &c = *static_cast<const CmdTexSource *>(list.payload(j));
            h = cmd_mix(cmd_mix(h, 0x3000 ^ (uint64_t)(uintptr_t)c.img), c.tex0 & kTexAddrMask);
            continue;
        }
        break;
    }
    h = cmd_mix(h, (uint64_t)(j - i));
    if (nParts >= kRunMinParts) {
        /* (the GS state the run starts from: no command of it has run yet) */
        GsEntry entry;
        gs_entry(entry);
        const uint64_t *e = reinterpret_cast<const uint64_t *>(&entry);
        for (size_t w = 0; w < sizeof(entry) / 8; w++)
            h = cmd_mix(h, e[w]);
    }
    key = h;
    return j;
}

/* (The key of a run, a 64-bit hash of its structure: run_scan_key.  No
 * extract kept - a stored copy compared byte for byte cost more in cache
 * misses than the replays saved; a collision of 64-bit hashes between two
 * runs of a frame is not a practical risk.) */

/* ---- recording (the normal execution, hooked) --------------------------- */

void run_rec_bad(const char *why)
{
    if (g_runRec && !g_runRec->bad) {
        g_runRec->bad = true;
        g_runRec->why = why;
    }
}

void run_rec_part(const CmdModel &c, uint64_t meshSerial, ID3D11Buffer *vb, ID3D11Buffer *ib, ID3D11Buffer *bvb,
                  ID3D11Buffer *ib2, uint32_t nVerts, int32_t program, int32_t specExp, int32_t format, int overlay)
{
    RunRec &r = *g_runRec;
    if (r.bad)
        return;
    if (meshSerial == 0 || bvb == nullptr) {
        run_rec_bad("a streamed part");
        return;
    }
    size_t mi = 0;
    for (; mi < r.models.size(); mi++)
        if (r.models[mi].model == c.part.model)
            break;
    if (mi == r.models.size()) {
        if (mi >= 0xFFFF) {
            run_rec_bad("too many models");
            return;
        }
        r.models.push_back(RunModel{c.part.model, meshSerial, vb, ib, bvb, g_runCmd, ib2, nVerts});
    } else if (r.models[mi].serial != meshSerial || r.models[mi].vb != vb || r.models[mi].ib != ib ||
               r.models[mi].bvb != bvb || r.models[mi].ib2 != ib2) {
        run_rec_bad("a model decoded again within the run");
        return;
    }
    RunPart p;
    p.cmd = g_runCmd;
    p.firstStep = (uint32_t)r.steps.size();
    p.nSteps = 0;
    p.model = (uint16_t)mi;
    p.overlay = (int16_t)overlay;
    p.program = program;
    p.specExp = specExp;
    p.format = format;
    p.lastPrim = 0;
    p.hasPrim = false;
    r.parts.push_back(p);
}

/* ---- The registers of a run (PC, 2026-11-06, step 4) ---------------------
 *
 * A replay walked the run's commands for their register writes (exec_regs,
 * write_reg: ~11 % of the executor after step 3, the flushes' payloads
 * streamed again) though every register value along the run is the key's -
 * the entry state and every write are hashed into it, a TEX0 / TEX2's TBP0
 * and CBP aside (a set's VRAM place, kCmdTexAddrMask).  Now a replay visits
 * only the commands that matter (RunRec::ops, recorded as the run executed:
 * run_rec_op): a TEX0 / TEX2 write for its live address - kept in a shadow
 * per context (g_runTex0 / g_runTex2), the CLUT load done with CLD as
 * write_reg did it - a TEXFLUSH (write_reg as before), the sets' uploads,
 * the parts.  A step's texture is resolved from the registers as the walk
 * had them: the live TEX0 and the record's TEX1 / CLAMP / FRAME / TEXA
 * (RunStep: run_regs_at_step).  At the end the registers are the record's
 * end state (gs_entry at the recording's end) with TEX0 / TEX2 as the run
 * wrote them.  The same values at every read, the same loads in the same
 * order: exact.  GT3PC_PGL_RUN_REGS=0: the command walk as before
 * (exec_init). */
bool g_runRegsOn = true;
bool g_runRegs = false;                     /* (a replay) the registers from the record, not the walk */
uint64_t g_runTex0[2], g_runTex2[2];        /* (a replay) each context's TEX0 / TEX2 as the run's writes leave them */
constexpr uint64_t kRunOpRegsMask = (1ull << gs::REG_TEX0) | (1ull << gs::REG_TEX2) | (1ull << (16 + gs::REG_TEX0)) |
                                    (1ull << (16 + gs::REG_TEX2)) | (1ull << (32 + gs::GEN_TEXFLUSH));
static_assert(gs::REG_TEX2 < gs::REG_TEX0, "exec_regs writes a flush's TEX2 before its TEX0");

/* (recording) the command just executed, if a replay must visit it */
void run_rec_op(CmdType t, const void *p)
{
    RunRec &r = *g_runRec;
    if (r.bad)
        return;
    RunOp op = {g_runCmd, 0, 0};
    switch (t) {
    case CMD_REGS:
        if ((static_cast<const CmdRegs *>(p)->mask & kRunOpRegsMask) == 0)
            return;
        op.kind = OP_REGS;
        break;
    case CMD_AD: {
        const uint32_t a = static_cast<const CmdAd *>(p)->addr & 0xFF;
        if (a >= 0x80)
            return;
        const int i = g_addrReg[a];
        if (!(i == gs::REG_TEX0 || i == gs::REG_TEX2 || g_addrGen[a] == gs::GEN_TEXFLUSH))
            return;
        op.kind = OP_AD;
        break;
    }
    case CMD_TEX1_UPLOAD:
        op.kind = OP_UPLOAD;
        break;
    case CMD_TEX_SOURCE:
        op.kind = OP_SOURCE;
        break;
    case CMD_MODEL:
        if (r.parts.empty() || r.parts.back().cmd != g_runCmd)
            return;                         /* (not a part of the run) */
        op.kind = OP_PART;
        op.part = (uint16_t)(r.parts.size() - 1);
        break;
    default:
        return;
    }
    r.ops.push_back(op);
}

/* (a replay) a TEX0 / TEX2 write of the run as write_reg takes it: the live
 * value (its TBP0 / CBP are this frame's) into the context's shadow, the
 * CLUT load with CLD */
inline void run_regs_tex_write(int c, int i, uint64_t v)
{
    if (i == gs::REG_TEX2) {
        const uint64_t mask = (0x3Full << 20) | (~0ull << 37);
        const uint64_t t0 = (g_runTex0[c] & ~mask) | (v & mask);
        g_runTex2[c] = v;
        g_runTex0[c] = t0;
        if ((v >> 61) != 0)
            run_clut_load(t0);
        return;
    }
    if ((v >> 61) != 0)
        run_clut_load(v);
    g_runTex0[c] = v;
}

/* (a replay) a register flush's TEX0 / TEX2 / TEXFLUSH writes, in
 * exec_regs's order: context 1's registers ascending, context 2's, the
 * general ones */
void run_replay_regs(const CmdRegs &c)
{
    const uint64_t m = c.mask;
    for (int ctx = 0; ctx < 2; ctx++) {
        if (m & (1ull << (16 * ctx + gs::REG_TEX2)))
            run_regs_tex_write(ctx, gs::REG_TEX2, c.reg[gs::REG_TEX2]);
        if (m & (1ull << (16 * ctx + gs::REG_TEX0)))
            run_regs_tex_write(ctx, gs::REG_TEX0, c.reg[gs::REG_TEX0]);
    }
    if (m & (1ull << (32 + gs::GEN_TEXFLUSH)))
        write_reg(kGenAddr[gs::GEN_TEXFLUSH], c.gen[gs::GEN_TEXFLUSH]);
}

void run_replay_ad(const CmdAd &c)
{
    const uint32_t a = c.addr & 0xFF;
    const int i = g_addrReg[a];
    if (i == gs::REG_TEX0 || i == gs::REG_TEX2)
        run_regs_tex_write(g_addrCtx[a], i, c.value);
    else
        write_reg(a, c.value);              /* (TEXFLUSH) */
}

/* (a replay) the registers a step's texture is resolved from, as the walk
 * would have them now: the context's live TEX0, the record's others */
inline void run_regs_at_step(const RunStep &s)
{
    const int ctxt = (g.prim >> 9) & 1;
    g.ctx[ctxt][gs::REG_TEX0] = g_runTex0[ctxt];
    g.ctx[ctxt][gs::REG_TEX1] = s.tex1;
    g.ctx[ctxt][gs::REG_CLAMP] = s.clamp;
    g.ctx[ctxt][gs::REG_FRAME] = s.frame;
    g.gen[gs::GEN_TEXA] = s.texa;
}

/* A batch's PRIM set (vu1_draw's set_prim, before anything else): the part's
 * last, so a replay leaves the GS state as the normal path does */
void run_rec_prim(uint32_t prim)
{
    RunRec &r = *g_runRec;
    if (r.bad || r.parts.empty() || r.parts.back().cmd != g_runCmd)
        return;
    r.parts.back().lastPrim = prim;
    r.parts.back().hasPrim = true;
}

/* The state just computed (compute_state) as a run's: its targets' entry */
uint16_t run_rec_state()
{
    RunRec &r = *g_runRec;
    const BatchState &b = g_bs;
    if (g_stateTexFail) {
        run_rec_bad("a draw without its texture");
        return 0;
    }
    if (b.rt != nullptr && b.rt->gsW <= 64 && b.rt->gsH <= 64) {
        run_rec_bad("a small target");
        return 0;
    }
    if (b.dateSrv != nullptr) {
        run_rec_bad("a DATE copy");             /* (the copies' mode: not here) */
        return 0;
    }
    if (b.depthFrame || (b.ps.flags & PSF_BLEND_AD) != 0)
        r.workOk = false;                       /* PC: (a copy per draw, a depth frame: not one record for the run, RunWork) */
    if ((b.rt == nullptr && g_targetArgs.fw != 0) || (b.zt == nullptr && g_targetArgs.hasZ)) {
        run_rec_bad("a draw without its target");   /* (skipped for it: may exist next time) */
        return 0;
    }
    RunTargets t = {};
    t.args = g_targetArgs;
    t.rt = b.rt;
    t.zt = b.zt;
    if (b.rt != nullptr) {
        t.nw = b.rt->nw;
        t.nh = b.rt->nh;
        t.samples = b.rt->samples;
        t.sx = b.rt->sx;
        t.sy = b.rt->sy;
        t.gsRes = b.rt->gsRes;
        t.dated = b.rt->dated;
    }
    if (b.zt != nullptr) {
        t.znw = b.zt->nw;
        t.znh = b.zt->nh;
        t.zsamples = b.zt->samples;
    }
    size_t ti = 0;
    for (; ti < r.targets.size(); ti++) {
        const RunTargets &o = r.targets[ti];
        if (o.rt != t.rt || o.zt != t.zt)
            continue;
        if (o.dated != t.dated) {
            run_rec_bad("the target took its alpha mirror in the run");
            return 0;
        }
        break;
    }
    if (ti == r.targets.size()) {
        if (ti >= 0xFFFF) {
            run_rec_bad("too many targets");
            return 0;
        }
        r.targets.push_back(t);
    }
    if (r.states.size() >= 0xFFFF) {
        run_rec_bad("too many states");
        return 0;
    }
    r.states.push_back(RunState{b, (uint16_t)ti});
    return (uint16_t)(r.states.size() - 1);
}

void run_rec_draw(const PglrVu1 &vu, int event, uint32_t prim, uint32_t indexCount, uint32_t firstIndex,
                  int32_t baseVertex, const float *bmin, const float *bmax, uint32_t batch, ID3D11Buffer *bvb,
                  int aMin, int aMax, float n2)
{
    RunRec &r = *g_runRec;
    if (r.bad)
        return;
    if (r.parts.empty() || r.parts.back().cmd != g_runCmd) {
        run_rec_bad("a draw outside a part");
        return;
    }
    if (baseVertex != 0 || bvb == nullptr || batch > 0xFFFF) {
        run_rec_bad("a streamed draw");
        return;
    }
    RunStep s = {};
    s.event = (uint8_t)event;
    {
        /* PC (step 4): the registers its texture is resolved from (the
         * batch's context: as run_replay_tex takes it from g.prim) */
        const int ctxt = ((prim & 0x7FF) >> 9) & 1;
        s.tex1 = g.ctx[ctxt][gs::REG_TEX1];
        s.clamp = g.ctx[ctxt][gs::REG_CLAMP];
        s.frame = g.ctx[ctxt][gs::REG_FRAME];
        s.texa = g.gen[gs::GEN_TEXA];
    }
    if (event == STATE_WHOLE) {
        s.state = run_rec_state();
        if (r.bad)
            return;
    }
    s.hasBounds = bmin != nullptr && bmax != nullptr;
    if (s.hasBounds) {
        memcpy(s.bmin, bmin, sizeof(s.bmin));
        memcpy(s.bmax, bmax, sizeof(s.bmax));
    }
    /* its matrix group: the last one's while the program state is the same */
    if (!r.lastVuValid || !same_bytes(&vu, &r.lastVu, sizeof(vu)) || r.vuGroups.empty()) {
        if (r.vuGroups.size() >= 0xFFFF) {
            run_rec_bad("too many matrix groups");
            return;
        }
        RunVuGroup gr;
        gr.hasBox = s.hasBounds != 0;
        memcpy(gr.bmin, s.bmin, sizeof(gr.bmin));
        memcpy(gr.bmax, s.bmax, sizeof(gr.bmax));
        r.vuGroups.push_back(gr);
        r.lastVu = vu;
        r.lastVuValid = true;
    } else {
        RunVuGroup &gr = r.vuGroups.back();
        if (!s.hasBounds) {
            gr.hasBox = false;
        } else if (gr.hasBox) {
            for (int k = 0; k < 3; k++) {
                gr.bmin[k] = std::min(gr.bmin[k], s.bmin[k]);
                gr.bmax[k] = std::max(gr.bmax[k], s.bmax[k]);
            }
        }
    }
    s.vuGroup = (uint16_t)(r.vuGroups.size() - 1);
    s.aMin = (uint8_t)aMin;
    s.aMax = (uint8_t)aMax;
    s.nMax2 = n2;
    s.batch = (uint16_t)batch;
    s.indexCount = indexCount;
    s.firstIndex = firstIndex;
    s.prim = prim;
    /* PC (2026-11-05): its texture with the identity of what the bind read
     * ("The textures of a run") */
    s.tex = 0xFFFF;
    s.pal[0] = s.pal[1] = 0xFFFF;
    if (event != STATE_KEPT && !g_bs.skip && g_bs.tme)
        run_rec_tex(r, s, g_bs);
    r.steps.push_back(s);
    r.parts.back().nSteps++;
}

/* PC (2026-11-08): a run's record checked before each replay.  The user's
 * crash (2026-11-08 18:07, a 32-car race): a replay took the payload of
 * command 65,556 of a run of a few hundred (pgl_exec run_replay_regs,
 * reading 0x5C7B3D91) - an op of the record (RunOp::cmd 0x00010014, its
 * kind still OP_REGS) written over after the run was recorded, its memory
 * not this code's at the time (the runs' records are freed only once the
 * frame's works have run; what wrote it is not found yet: the minidump has
 * no heap).  So a replay leaves out an op whose indices point outside the
 * run (run_op_damaged), and a record found so - or, every 32nd replay, one
 * whose parts point outside it or whose ops and parts no longer hash as
 * recorded (run_record_fault) - is not replayed again: logged with what is
 * wrong, dropped and resolved again.  GT3PC_PGL_RUN_CHECK=0: none of it
 * (its cost compared); GT3PC_PGL_RUN_DAMAGE=1 / 2: its test (run_try).
 * (Keep: the log line tells the next such write's place and value.) */
uint64_t run_ops_hash(const RunRec &r)
{
    uint64_t h = 0xCBF29CE484222325ull;
    auto words = [&h](const void *p, size_t bytes) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i + 4 <= bytes; i += 4) {
            uint32_t w;
            memcpy(&w, b + i, 4);
            h = (h ^ w) * 0x100000001B3ull;
        }
    };
    words(r.ops.data(), r.ops.size() * sizeof(RunOp));
    words(r.parts.data(), r.parts.size() * sizeof(RunPart));
    h = (h ^ r.ops.size()) * 0x100000001B3ull;
    return (h ^ r.parts.size()) * 0x100000001B3ull;
}

/* "" when r is whole, else what is wrong with it */
std::string run_record_fault(const RunRec &r)
{
    char buf[256];
    /* (an op a replay found out of range: run_op_damaged) */
    if (!r.damage.empty())
        return r.damage;
    /* (the rest every 32nd replay: the ops' and the parts' indices, and the
     * ops' and parts' hash - a write that left every index in range.  At
     * every replay the ops alone cost 0.04 ms of the benchmark's 1.1, their
     * arrays read cold ahead of the replay: the replay checks each op's
     * indices as it takes it instead, run_op_damaged.  The steps and states
     * not at all: some 400 KB a frame of a 32-car race to read, 0.2 ms) */
    if ((r.hits & 31) != 0)
        return std::string();
    uint32_t wrong = 0;
    {
        const RunOp *o = r.ops.data();
        const uint32_t n = (uint32_t)r.ops.size(), nCmds = r.nCmds, nParts = (uint32_t)r.parts.size();
        for (uint32_t k = 0; k < n; k++)
            wrong |= (uint32_t)(o[k].cmd >= nCmds) | (uint32_t)((uint32_t)(o[k].kind - OP_REGS) > OP_PART - OP_REGS) |
                     ((uint32_t)(o[k].kind == OP_PART) & (uint32_t)(o[k].part >= nParts));
    }
    for (size_t k = 0; wrong != 0 && k < r.ops.size(); k++) {
        const RunOp &op = r.ops[k];
        if (op.cmd >= r.nCmds || op.kind < OP_REGS || op.kind > OP_PART ||
            (op.kind == OP_PART && op.part >= r.parts.size())) {
            snprintf(buf, sizeof(buf), "op %u of %u at %p (+%u of %u bytes): command %u, kind %u, part %u",
                     (unsigned)k, (unsigned)r.ops.size(), (const void *)&op, (unsigned)(k * sizeof(RunOp)),
                     (unsigned)(r.ops.size() * sizeof(RunOp)), op.cmd, op.kind, op.part);
            return buf;
        }
    }
    for (size_t k = 0; k < r.models.size(); k++)
        if (r.models[k].cmd >= r.nCmds) {
            snprintf(buf, sizeof(buf), "model %u of %u: command %u", (unsigned)k, (unsigned)r.models.size(),
                     r.models[k].cmd);
            return buf;
        }
    /* (a part is not checked as the replay reaches it: the stage walks the
     * run's steps from the record, the part left out or not) */
    for (size_t k = 0; k < r.parts.size(); k++) {
        const RunPart &p = r.parts[k];
        if (p.cmd >= r.nCmds || p.model >= r.models.size() || p.firstStep > r.steps.size() ||
            p.nSteps > r.steps.size() - p.firstStep) {
            snprintf(buf, sizeof(buf), "part %u of %u at %p: command %u, model %u of %u, steps %u + %u of %u",
                     (unsigned)k, (unsigned)r.parts.size(), (const void *)&p, p.cmd, p.model,
                     (unsigned)r.models.size(), p.firstStep, p.nSteps, (unsigned)r.steps.size());
            return buf;
        }
    }
    if (run_ops_hash(r) != r.opsHash) {
        snprintf(buf, sizeof(buf), "its ops (%u at %p) or parts (%u at %p) changed, every index still in range",
                 (unsigned)r.ops.size(), (const void *)r.ops.data(), (unsigned)r.parts.size(),
                 (const void *)r.parts.data());
        return buf;
    }
    return std::string();
}

/* (a replay) op k of r out of range - the crash's - noted in the run (its
 * next try drops it) and left out: true.  The replay goes on without it (a
 * register write's texture resolved afresh at its step, a part's draws
 * missing) for this one frame */
bool run_op_damaged(RunRec &r, const RunOp &op)
{
    if (op.cmd < r.nCmds && (op.kind != OP_PART || op.part < r.parts.size()))
        return false;
    if (r.damage.empty()) {
        const size_t k = (size_t)(&op - r.ops.data());
        char buf[256];
        snprintf(buf, sizeof(buf), "op %u of %u at %p (+%u of %u bytes): command %u, kind %u, part %u - found by the"
                 " replay, left out of it", (unsigned)k, (unsigned)r.ops.size(), (const void *)&op,
                 (unsigned)(k * sizeof(RunOp)), (unsigned)(r.ops.size() * sizeof(RunOp)), op.cmd, op.kind, op.part);
        r.damage = buf;
    }
    return true;
}

bool run_record_sound(const RunRec &r)
{
    if (!g_runCheckOn)
        return true;
    const std::string fault = run_record_fault(r);
    if (fault.empty())
        return true;
    g_runsDamaged++;
    if (g_runsDamaged <= 8)
        pclog::write("pgl runs: a run's record damaged since it was recorded - %s (the run: %u commands, %u ops, %u"
                     " parts, %u steps, at %p; recorded %.1f s ago, replayed %u times): not replayed, resolved again%s",
                     fault.c_str(), r.nCmds, (unsigned)r.ops.size(), (unsigned)r.parts.size(),
                     (unsigned)r.steps.size(), (const void *)&r, g_execSeconds - r.recorded, r.hits,
                     g_runsDamaged == 8 ? " (no more of these lines)" : "");
    return false;
}

void run_rec_finish()
{
    std::unique_ptr<RunRec> r = std::move(g_runRec);
    if (!r)
        return;
    texture_ident_capture(false);
    if (r->bad) {
        g_runsBad++;
        static int logged = 0;
        if (logged < 8) {
            logged++;
            pclog::write("pgl runs: a run of %u commands not resolved (%s)%s", r->nCmds, r->why ? r->why : "?",
                         logged == 8 ? " (no more of these lines)" : "");
        }
        return;
    }
    r->lastUse = g_execSeconds;
    r->steps.shrink_to_fit();
    r->states.shrink_to_fit();
    r->ops.shrink_to_fit();
    r->texs.reserve(r->texs.size() + 64);  /* PC (2026-11-06): room for records made at replays (run_replay_tex) */
    gs_entry(r->end);                       /* PC (step 4): the state the run leaves (its last command has run) */
    r->opsHash = run_ops_hash(*r);          /* PC (2026-11-08): (checked before each replay: run_record_sound) */
    r->recorded = g_execSeconds;
    g_runs[r->key] = std::move(r);
}

/* ---- the check and the replay ------------------------------------------- */

bool run_targets_same(const RunRec &r)
{
    for (const RunTargets &t : r.targets) {
        if (t.rt == nullptr) {
            if (t.args.base == 0 && t.args.fw == 0)
                continue;                   /* (a state skipped before its targets) */
            return false;
        }
        Target *rt = t.args.depthFrame ? depth_frame_target(t.args.base, t.args.fbw, t.args.psm, t.args.fw, t.args.fh)
                                       : color_target(t.args.base, t.args.fbw, t.args.psm, t.args.fw, t.args.fh);
        if (rt != t.rt || rt->nw != t.nw || rt->nh != t.nh || rt->samples != t.samples || rt->sx != t.sx ||
            rt->sy != t.sy || rt->gsRes != t.gsRes || rt->dated != t.dated)
            return false;
        if (t.args.hasZ) {
            Target *zt = depth_target(t.args.zbase, t.args.zpsm, *rt);
            if (zt != t.zt || zt->nw != t.znw || zt->nh != t.znh || zt->samples != t.zsamples)
                return false;
        } else if (t.zt != nullptr) {
            return false;
        }
        if (t.dated && !t.args.depthFrame && target_mirror_dsv(*rt, t.zt, false) == nullptr)   /* (a check: claims nothing) */
            return false;
    }
    return true;
}

bool run_models_same(const CmdList &list, size_t first, const RunRec &r)
{
    for (const RunModel &m : r.models) {
        if (m.cmd >= r.nCmds)
            return false;                   /* PC (2026-11-08): (a damaged record: run_record_sound) */
        const CmdModel &c = *static_cast<const CmdModel *>(list.payload(first + m.cmd));
        const gt3vif::Mesh *mesh = nullptr;
        ID3D11Buffer *vb = nullptr, *ib = nullptr, *bvb = nullptr, *ib2 = nullptr;
        uint32_t baseV = 0, baseI = 0, fb = 0, eb = 0, nVerts = 0;
        uint64_t serial = 0;
        bool found;
        {
            ProfScope prof(PROF_MESH);
            found = model_mesh(c.part, &mesh, &vb, &ib, &baseV, &baseI, &fb, &eb, &bvb, &serial, &ib2, &nVerts);
        }
        if (!found || serial != m.serial || vb != m.vb || ib != m.ib || bvb != m.bvb || ib2 != m.ib2 || baseV != 0 ||
            baseI != 0)
            return false;
    }
    return true;
}

/* The replay's matrix group at hand (RunVuGroup): which, whether the frame's
 * draws share the program state as the recording's did (else each batch
 * tests its own box), and the box's test */
struct RunGroupCur {
    int group = -1;
    uint32_t serial = 0;                    /* (the state the box was tested under: its area) */
    bool same = false;
    bool inside = false;
    PglrVu1 vu;
    D3D11_RECT rect;
    int state = -1;                         /* the run's state in effect (RunRec::states; its D3D states kept) */
};
RunGroupCur g_runGroup;

void run_replay_part(const CmdList &list, size_t i, RunRec &r, uint16_t partIdx)
{
    const RunPart &part = r.parts[partIdx];
    const CmdModel &c = *static_cast<const CmdModel *>(list.payload(i));
    const RunModel &m = r.models[part.model];
    g_profCount[PCOUNT_PARTS]++;
    g_profCount[PCOUNT_RUN_PARTS]++;
    Vu1Head head = {part.program, part.specExp, part.format, c.part.morphWeight, 0.0f, 0.0f, 0.0f, 0.0f};
    head_hide(head, c.part);                /* PC (2026-11-07) */
    g_traceProg = part.program;             /* (the F9 trace's lines) */
    for (uint32_t k = 0; k < part.nSteps; k++) {
        RunStep &s = r.steps[part.firstStep + k];   /* (its texture's record may be taken anew: run_replay_tex) */
        g_runStepCur = part.firstStep + k;  /* PC (step 3): (the notes name it; a cut inside the step begins the next work at it) */
        /* (as vu1_draw: the batch's PRIM, then the state - here the run's) */
        g.prim = s.prim & 0x7FF;
        g.fullColor = (s.prim & pr::PRIM_FULL_COLOR) != 0;
        g.count = 0;
        if (s.event == STATE_WHOLE) {
            BatchState &b = g_bs;
            b = r.states[s.state].bs;
            g_subPosted = false;            /* PC: (the submit stage gets it with the first draw) */
            g_runGroup.state = (int)s.state;
            g_stateSerial++;                /* (a new state: the merged draw ends) */
            g_profCount[PCOUNT_STATES]++;
            g_fbPending = false;
            g_fbRects.clear();
            g_segStart = 0;
            if (g_work != nullptr) {
                /* (the stage's views of the state, with its number and its
                 * verdicts: WorkState; PC step 3 - the run's draws as one
                 * record for the stage, or null; a cut begins another) */
                g_runStateCur = s.state;
                g_workTex = nullptr;
                g_workSrv = nullptr;
                g_runBookCur = r.states[s.state].targets;   /* (its books: RunBook) */
                WorkState &ws = g_work->states[g_work->nStates++];
                work_state_fill(ws, s.state);
            }
            if (g_runRegs)
                run_regs_at_step(s);        /* PC (step 4): (the registers as the walk would have them) */
            if (!b.skip && b.tme)
                run_replay_tex(r, s, b);    /* (the texture as the registers have it now: from its record while its identity holds) */
            if (g_work != nullptr) {
                /* PC (step 3): the texture in effect and the skip the resolve
                 * left, into the state's note - the current work's last (a
                 * cut inside the event began a work whose first state is
                 * this one) */
                WorkState &ws = g_work->states[g_work->nStates - 1];
                ws.tex = g_workTex;
                ws.srv = g_workSrv;
                ws.skip = b.skip;
            }
        } else if (s.event == STATE_TEXTURE) {
            g_profCount[PCOUNT_TEXSTATES]++;
            if (g_runRegs)
                run_regs_at_step(s);        /* PC (step 4) */
            run_replay_tex(r, s, g_bs);
        }
        if (g_bs.skip || g_bs.depthFrame)
            continue;
        if (g_work != nullptr && g_work->states[g_work->nStates - 1].walk) {
            /* PC (step 3): the stage walks the draw (run_submit_steps); here
             * only what the commands after the run depend on - the state's
             * books (the scissor for the draw's area: a superset, the same
             * pixels copied) and the alpha position of the target the draw
             * writes (as submit_vu1_work) */
            RunWork &wk = *g_work;
            draw_bookkeep(nullptr, nullptr);
            if (g_bs.wroteTop && g_bs.rt != nullptr) {
                bool known = false;
                for (Target *t : wk.topTargets)
                    known |= t == g_bs.rt;
                if (!known)
                    wk.topTargets.push_back(g_bs.rt);
            }
            continue;
        }
        /* the matrix group: its box tested once (under the first draw's
         * state: the planes are the matrix's alone, the area the box's) */
        RunGroupCur &gc = g_runGroup;
        if (gc.group != (int)s.vuGroup) {
            gc.group = (int)s.vuGroup;
            gc.vu = c.vu;
            gc.same = true;
            gc.serial = ~0u;
        } else if (gc.same && !same_bytes(&c.vu, &gc.vu, sizeof(c.vu))) {
            gc.same = false;                /* (not this frame: each batch its own box) */
        }
        if (gc.same && gc.serial != g_stateSerial) {
            const RunVuGroup &gr = r.vuGroups[s.vuGroup];
            bool in = false;
            gc.inside = gr.hasBox && vu1_area(g_bs, c.vu, gr.bmin, gr.bmax, gc.rect, &in) && in;
            gc.serial = g_stateSerial;
        }
        g_runHint.valid = gc.same && gc.inside;
        g_runHint.rect = gc.rect;
        if (g_work != nullptr)
            submit_vu1_work(*g_work, s, partIdx, part, c);  /* (g_work: a cut inside the step's prologue begins another) */
        else
            submit_vu1(c.vu, head, c.part.light, m.vb, m.ib, s.indexCount, s.firstIndex, 0,
                       s.hasBounds ? s.bmin : nullptr, s.hasBounds ? s.bmax : nullptr, part.overlay, s.batch, m.bvb,
                       s.aMin, s.aMax, s.nMax2, m.ib2, m.nVerts);
        g_runHint.valid = false;
        /* (the blend states of the state's passes are found with it,
         * state_passes, and kept in the run's copy; the submit stage looks
         * up the rasterizer by cull itself, a table of four) */
    }
    if (part.hasPrim) {
        /* (the PRIM of its last batch, drawn or not: as set_prim left it) */
        g.prim = part.lastPrim & 0x7FF;
        g.fullColor = (part.lastPrim & pr::PRIM_FULL_COLOR) != 0;
        g.count = 0;
    }
    g_traceProg = -1;
}

void run_replay(const CmdList &list, size_t first, RunRec &r)
{
    ProfScope prof(PROF_RUN);
    /* PC (2026-11-06): the frame capture (pgl_capture.h): this replay's
     * number (its works carry it to the stage), its time and counts */
    const int32_t capSeq = cap::next_run();
    const int64_t capT0 = capSeq >= 0 ? cap_now() : 0;
    const uint32_t capFree0 = t_stats->runTexFree, capWalk0 = t_stats->runTexWalk;
    const uint32_t capCuts0 = g_profCount[PCOUNT_RUN_CUT], capWorks0 = g_profCount[PCOUNT_RUN_WORK];
    g_capLastRun = capSeq;
    cap::t_run = capSeq;
    flush_batch();
    flare_queries_flush();                  /* PC: (the run's parts are model draws: the queries' batch first) */
    if (g_tracing)
        pclog::write("pgl trace %u: a resolved run replayed: %u commands, %u parts, %u states, %u draws, replayed %u"
                     " times before", g_traceN++, r.nCmds, (unsigned)r.parts.size(), (unsigned)r.states.size(),
                     (unsigned)r.steps.size(), r.hits);
    g_runGroup.group = -1;
    g_runGroup.state = -1;
    /* PC (2026-11-05): its textures from their records while their
     * identities hold, its CLUT loads deferred ("The textures of a run") */
    g_runReplay = &r;
    run_rows_reset();
    if (g_runTexOn) {
        g_clutDefer = true;
        g_clutLive.resize(r.cluts.size());
        g_clutOk.assign(r.cluts.size(), 0);
        texture_ident_capture(true);        /* (a step resolved after all takes its record anew) */
    }
    /* PC (2026-11-05): its draws as one record for the submit stage (RunWork:
     * the file comment at WorkStep) while the frame pipelines, nothing times
     * the GPU's sections, and the run allows it */
    RunWork *w = nullptr;
    if (g_runWorkOn && sq::g_on && r.workOk && !sec_active()) {
        w = g_workRuns.alloc(1);
        w->r = &r;
        w->list = &list;
        w->first = first;
        w->states = g_workStates.alloc(r.states.size() + 1);
        w->steps = g_workSteps.alloc(r.steps.size() + 1);
        w->nStates = w->nSteps = 0;
        w->topTargets.clear();
        w->cap.clear();
        w->alpha.clear();
        w->needB = 0;
        w->done.store(0, std::memory_order_relaxed);
        w->stepFirst = w->stepEnd = 0;      /* PC (step 3) */
        w->cut = false;
        w->notes = g_workNotes.alloc(r.steps.size() + 1);
        w->nNotes = 0;
        w->nViews = 0;
        w->capRun = capSeq;                 /* PC (2026-11-06): (the frame capture) */
        g_workTex = nullptr;
        g_workSrv = nullptr;
        g_runBook.assign(r.targets.size(), RunBook());
        g_runBookCur = 0;
    }
    g_work = w;
    /* PC (2026-11-06, step 3): the stage walks the run's draws itself, the
     * executor noting the states and the textures (the file comment at
     * WorkState); not in a traced frame (its lines are per draw) */
    g_runWalk = w != nullptr && g_runWalkOn && !g_tracing;
    g_runStepCur = 0;
    /* (the run's commands by kind, counted once: RunRec::cmdCounts) */
    for (int c = 0; c < 32; c++)
        t_stats->cmds[c] += r.cmdCounts[c];
    g_profCount[PCOUNT_GSCMDS] += r.cmdCounts[CMD_REGS & 31] + r.cmdCounts[CMD_AD & 31];
    if (g_runRegsOn) {
        /* PC (2026-11-06, step 4): the commands a replay visits alone (RunOp:
         * the file comment at g_runRegsOn), the registers from the record */
        g_runRegs = true;
        for (int c = 0; c < 2; c++) {
            g_runTex0[c] = g.ctx[c][gs::REG_TEX0];
            g_runTex2[c] = g.ctx[c][gs::REG_TEX2];
        }
        /* PC (2026-11-08): an op out of the run's range left out (the user's
         * crash: its payload was no command's; run_op_damaged notes it) */
        const uint32_t opCmds = g_runCheckOn ? r.nCmds : ~0u;
        const uint32_t opParts = g_runCheckOn ? (uint32_t)r.parts.size() : ~0u;
        for (const RunOp &op : r.ops) {
            if ((op.cmd >= opCmds || (op.kind == OP_PART && op.part >= opParts)) && run_op_damaged(r, op))
                continue;
            const void *p = list.payload(first + op.cmd);
            switch (op.kind) {
            case OP_REGS:
                run_replay_regs(*static_cast<const CmdRegs *>(p));
                break;
            case OP_AD:
                run_replay_ad(*static_cast<const CmdAd *>(p));
                break;
            case OP_UPLOAD: {
                const CmdTex1 &c = *static_cast<const CmdTex1 *>(p);
                ProfScope up(PROF_UPLOAD);
                g_profCount[PCOUNT_UPLOADS]++;
                registry_tex1_upload(c.img, c.vramBase, c.copyBytes != 0 ? &c + 1 : nullptr);
                break;
            }
            case OP_SOURCE: {
                const CmdTexSource &c = *static_cast<const CmdTexSource *>(p);
                ProfScope up(PROF_UPLOAD);
                registry_texture_source(c.img, c.vramBase, c.tex0, c.copyBytes != 0 ? &c + 1 : nullptr);
                break;
            }
            case OP_PART:
                run_replay_part(list, first + op.cmd, r, op.part);
                break;
            default:
                break;
            }
        }
        /* the registers as the walk leaves them: the record's end state
         * (every value but a TEX0 / TEX2 address is the key's), TEX0 / TEX2 as
         * the run wrote them */
        memcpy(g.ctx, r.end.ctx, sizeof(g.ctx));
        memcpy(g.gen, r.end.gen, sizeof(g.gen));
        for (int c = 0; c < 2; c++) {
            g.ctx[c][gs::REG_TEX0] = g_runTex0[c];
            g.ctx[c][gs::REG_TEX2] = g_runTex2[c];
        }
        g.prmode = r.end.prmode;
        g.prim = r.end.prim;
        g.fullColor = r.end.fullColor != 0;
        g_runRegs = false;
    } else {
        size_t pi = 0;
        for (uint32_t k = 0; k < r.nCmds; k++) {
            const size_t i = first + k;
            const CmdType t = list.type(i);
            const void *p = list.payload(i);
            switch (t) {
            case CMD_REGS:
                exec_regs(*static_cast<const CmdRegs *>(p));
                break;
            case CMD_AD: {
                const CmdAd &c = *static_cast<const CmdAd *>(p);
                write_reg(c.addr, c.value);
                break;
            }
            case CMD_TEX1_UPLOAD: {
                const CmdTex1 &c = *static_cast<const CmdTex1 *>(p);
                ProfScope up(PROF_UPLOAD);
                g_profCount[PCOUNT_UPLOADS]++;
                registry_tex1_upload(c.img, c.vramBase, c.copyBytes != 0 ? &c + 1 : nullptr);
                break;
            }
            case CMD_TEX_SOURCE: {
                const CmdTexSource &c = *static_cast<const CmdTexSource *>(p);
                ProfScope up(PROF_UPLOAD);
                registry_texture_source(c.img, c.vramBase, c.tex0, c.copyBytes != 0 ? &c + 1 : nullptr);
                break;
            }
            case CMD_MODEL:
                if (pi < r.parts.size() && r.parts[pi].cmd == k) {
                    run_replay_part(list, i, r, (uint16_t)pi);
                    pi++;
                }
                break;
            default:
                break;                      /* (none: run_scan) */
            }
        }
    }
    /* PC (2026-11-05): the CLUT buffer as the walk would leave it - the
     * deferred loads whose rows survive the run, done now */
    if (g_clutDefer) {
        run_clut_catch_up();
        g_profCount[PCOUNT_RUN_CLUTFREE] += g_runClutN - g_runClutDone;
    }
    g_clutDefer = false;
    g_runReplay = nullptr;
    texture_ident_capture(false);
    /* PC (2026-11-05): the run's draws to the stage as one record (its
     * states and textures read there from the run, which stays as it is
     * until the work has run: RunRec::pending); its books done (RunBook) */
    if (g_work != nullptr) {
        g_work->stepEnd = (uint32_t)r.steps.size();     /* PC (step 3): (its steps: to the run's end) */
        run_book_flush();
        run_work_post(g_work);              /* (the work, or the last of a cut run's) */
        g_work = nullptr;
    }
    g_runWalk = false;
    if (capSeq >= 0)                        /* PC (2026-11-06): (the frame capture) */
        cap::run(capSeq, r.key, r.nCmds, (uint32_t)r.parts.size(), (uint32_t)r.states.size(),
                 (uint32_t)r.steps.size(), cap_ns(capT0), t_stats->runTexFree - capFree0,
                 t_stats->runTexWalk - capWalk0, g_profCount[PCOUNT_RUN_CUT] - capCuts0,
                 g_profCount[PCOUNT_RUN_WORK] - capWorks0);
    cap::t_run = -1;
    /* the state is the run's last: the next draw works its own out */
    g_dirty = true;
    g_dirtyTex = false;
    g_bs.valid = false;
}

/* (the executor) A run's work to the stage: one record (the run's states
 * and textures are read there from the run, which stays as it is until the
 * work has run: RunRec::pending).  Nothing for a work without draws. */
void run_work_post(RunWork *w)
{
    if (w->nSteps == 0 && w->stepEnd <= w->stepFirst)   /* PC (step 3): (no notes, no steps to walk) */
        return;
    RunRec &r = *w->r;
    r.pending.fetch_add(1, std::memory_order_acq_rel);
    if (!workers_on()) {
        sq::run([w] { run_submit(*w); });
    } else {
        /* PC (step 2): a submit worker builds the run's submission
         * (work_run), the submit thread splices it in at the run's place
         * (run_publish).  needB: the worker may read the alpha bounds of the
         * targets its textures are (the env map) only once the submit
         * thread has run what last wrote them (Target::alphaPos); the run's
         * own alpha writes get their place after its record.  PC (step 3):
         * the targets its textures are - the views, the states' and the
         * notes' textures, the noted steps' */
        uint64_t need = 0;
        auto bump = [&need](const RunTex *t) {
            if (t != nullptr && t->out.target != nullptr)
                need = std::max(need, t->out.target->alphaPos);
        };
        for (uint32_t i = 0; i < w->nViews; i++)
            need = std::max(need, w->views[i].t->alphaPos);
        for (uint32_t i = 0; i < w->nStates; i++)
            bump(w->states[i].tex);
        for (uint32_t i = 0; i < w->nNotes; i++)
            bump(w->notes[i].tex);
        for (uint32_t i = 0; i < w->nSteps; i++)
            bump(w->steps[i].tex);
        w->needB = need;
        sq::run([w] { run_publish(*w); });
        for (Target *t : w->topTargets)
            t->alphaPos = sq::head();
    }
    /* (published at once: the ring publishes every 256 records, and a frame
     * of works has a few dozen - the stage would get them all at the frame's
     * end, after the executor, instead of alongside it) */
    sq::publish();
    if (workers_on())
        work_push(w);                       /* (after the publish: what it waits for is published) */
    g_profCount[PCOUNT_RUN_WORK]++;
}

/* (the executor) The run's work so far posted, a new one begun for the rest
 * from the state as it is (the stage's state is a work's own): before
 * anything queued mid-run that the GPU must run after the run's draws so far
 * (the file comment at run_work_cut's declaration) */
void run_work_cut()
{
    RunWork *w = g_work;
    if (w == nullptr)
        return;
    run_book_flush();
    RunRec &r = *w->r;
    RunWork *n = g_workRuns.alloc(1);
    n->r = &r;
    n->list = w->list;
    n->first = w->first;
    n->states = g_workStates.alloc(r.states.size() + 1);
    n->steps = g_workSteps.alloc(r.steps.size() + 1);
    n->nStates = n->nSteps = 0;
    n->topTargets.clear();
    n->cap.clear();
    n->alpha.clear();
    n->needB = 0;
    n->done.store(0, std::memory_order_relaxed);
    /* PC (step 3): the steps so far are the work's, the rest the new one's,
     * which begins in the state in effect */
    w->stepEnd = g_runStepCur;
    n->stepFirst = n->stepEnd = g_runStepCur;
    n->cut = true;
    n->notes = g_workNotes.alloc(r.steps.size() + 1);
    n->nNotes = 0;
    n->nViews = 0;
    n->capRun = w->capRun;                  /* PC (2026-11-06): (the frame capture) */
    run_work_post(w);
    WorkState &ws = n->states[n->nStates++];
    work_state_fill(ws, g_runStateCur);     /* (the texture in effect: g_workTex) */
    g_work = n;
    g_profCount[PCOUNT_RUN_CUT]++;
}

void run_work_cut_if(const Target *t)
{
    for (const RunBook &k : g_runBook)
        if (k.any && (k.rt == t || k.zt == t)) {
            run_work_cut();
            return;
        }
}

/* (the submit stage) A run's work: its states and draws built from the run
 * and the executor's notes (RunWork) and submitted as the per-draw records
 * would have been (sub_state_set, sub_vu1) */
/* (the submit stage) a run's draw record (as submit_vu1's) from its step,
 * its part's command and the verdicts - the executor's (WorkStep) or this
 * stage's walk's */
void run_sub_vu1(const RunRec &r, const RunStep &s, const RunPart &part, const CmdModel &c, const D3D11_RECT &area,
                 bool hasArea, bool inside, bool mirrorRange, bool mirror, bool noPass)
{
    const RunModel &m = r.models[part.model];
    SubVu1 d;
    d.vu = &c.vu;
    d.L = c.part.light;
    d.vb = m.vb;
    d.ib = m.ib;
    d.bvb = m.bvb;
    d.ib2 = m.ib2;                          /* PC (2026-11-06): (the doubled index stream) */
    d.nVerts = m.nVerts;
    d.indexCount = s.indexCount;
    d.firstIndex = s.firstIndex;
    d.baseVertex = 0;
    d.batch = s.batch;
    d.head = {part.program, part.specExp, part.format, c.part.morphWeight, 0.0f, 0.0f, 0.0f, 0.0f};
    head_hide(d.head, c.part);              /* PC (2026-11-07) */
    d.overlay = part.overlay;
    d.aMin = s.aMin;
    d.aMax = s.aMax;
    d.n2 = s.nMax2;
    d.area = area;
    d.hasArea = hasArea;
    d.inside = inside;
    d.mirrorRange = mirrorRange;
    d.mirror = mirror;
    d.noPass = noPass;
    d.dateSrv = nullptr;
    sub_vu1(d);
}

/* (the submit stage) a target texture's view as of the work (WorkView; the
 * record's own if the executor noted none - it notes every one) */
ID3D11ShaderResourceView *work_view(const RunWork &w, const RunTex &t)
{
    if (t.id.kind != 2)
        return nullptr;
    for (uint32_t i = 0; i < w.nViews; i++)
        if (w.views[i].t == t.id.target)
            return w.views[i].srv;
    return nullptr;
}

/* PC (2026-11-06, step 3): the stage's walk of a run's steps [stepFirst,
 * stepEnd) - the executor's notes read as it goes (WorkState at each whole
 * state, WorkNote at a texture resolved after all, the record's texture at
 * the other texture events), and for a state the stage walks (WorkState::
 * walk) the executor's half of each draw done here as run_replay_part and
 * submit_vu1_work did it: the matrix group's box tested once a state (the
 * area of all its draws, inside or not), else the draw's own box, the
 * clipper's verdict (count_direct), the reflection skip, the state's mirror
 * flags; for a state the executor walked, its notes of the draws it made
 * (WorkStep).  The steps' parts in order are the run's order of draws. */
void run_submit_steps(const RunWork &w)
{
    RunRec &r = *w.r;
    cap::t_run = w.capRun;                  /* PC (2026-11-06): (the frame capture: the draws' run) */
    uint32_t next = 0;                      /* the next WorkState */
    const WorkState *ws = nullptr;
    const RunTex *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    bool skipping = false;                  /* (a resolve found nothing to sample: until the next texture event) */
    uint32_t ni = 0, oi = 0;                /* the notes' and the executor's draws' cursors */
    RunGroupCur gc;
    uint32_t stateNo = 0;                   /* (the box tested once a state: gc.serial) */
    auto set_state = [&](uint32_t idx) {
        ws = &w.states[idx];
        sub_state_set(r.states[ws->state].bs, *ws);
        /* PC (2026-11-06): the note of the state's largest mirror pass, this
         * walk's (RunState::passArea: the executor's hint next time) */
        t_passState = &r.states[ws->state].passArea;
        *t_passState = 0;
        tex = ws->tex;
        srv = ws->srv;
        skipping = ws->skip;
        if (tex != nullptr)
            tex_apply(*tex, g_sub.bs, srv);
        stateNo++;
    };
    if (w.cut && w.nStates != 0) {
        set_state(0);                       /* (the state in effect as the work begins) */
        next = 1;
    }
    /* (the part of the step at hand: the parts' steps are consecutive) */
    uint32_t pi = 0;
    while (pi < r.parts.size() && w.stepFirst >= r.parts[pi].firstStep + r.parts[pi].nSteps)
        pi++;
    for (uint32_t k = w.stepFirst; k < w.stepEnd; k++) {
        const RunStep &s = r.steps[k];
        while (pi < r.parts.size() && k >= r.parts[pi].firstStep + r.parts[pi].nSteps)
            pi++;
        if (pi >= r.parts.size())
            break;                          /* (never: every step is a part's) */
        if (s.event == STATE_WHOLE && !(w.cut && k == w.stepFirst)) {
            if (next >= w.nStates)
                break;                      /* (never: the executor noted every state) */
            set_state(next++);
        }
        while (ni < w.nNotes && w.notes[ni].step < k)
            ni++;
        if (ni < w.nNotes && w.notes[ni].step == k) {
            const WorkNote &n = w.notes[ni++];
            tex = n.tex;
            srv = n.srv;
            skipping = n.skip;
            if (tex != nullptr)
                tex_apply(*tex, g_sub.bs, srv);
        } else if (s.event == STATE_TEXTURE && s.tex != 0xFFFF) {
            tex = &r.texs[s.tex];           /* (its record held: the executor noted nothing) */
            srv = work_view(w, *tex);
            tex_apply(*tex, g_sub.bs, srv);
        }
        if (ws == nullptr)
            continue;
        const RunPart &part = r.parts[pi];
        const CmdModel &c = *static_cast<const CmdModel *>(w.list->payload(w.first + part.cmd));
        if (!ws->walk) {
            /* the executor walked this state: its note of the draw, if it made one */
            if (oi < w.nSteps && w.steps[oi].step == k) {
                const WorkStep &st = w.steps[oi++];
                if (st.tex != nullptr)
                    tex_apply(*st.tex, g_sub.bs, st.srv);
                cap::t_part = st.part;
                run_sub_vu1(r, s, part, c, st.area, (st.flags & WF_HASAREA) != 0, (st.flags & WF_INSIDE) != 0,
                            (st.flags & WF_MIRROR_RANGE) != 0, (st.flags & WF_MIRROR) != 0,
                            (st.flags & WF_NOPASS) != 0);
            }
            continue;
        }
        const BatchState &b = g_sub.bs;
        if (skipping || b.skip || b.depthFrame)
            continue;
        /* the matrix group: its box tested once (under the first draw's
         * state: the planes are the matrix's alone, the area the box's) */
        if (gc.group != (int)s.vuGroup) {
            gc.group = (int)s.vuGroup;
            gc.vu = c.vu;
            gc.same = true;
            gc.serial = ~0u;
        } else if (gc.same && !same_bytes(&c.vu, &gc.vu, sizeof(c.vu))) {
            gc.same = false;                /* (not this frame: each batch its own box) */
        }
        if (gc.same && gc.serial != stateNo) {
            const RunVuGroup &gr = r.vuGroups[s.vuGroup];
            bool in = false;
            gc.inside = gr.hasBox && vu1_area(b, c.vu, gr.bmin, gr.bmax, gc.rect, &in) && in;
            gc.serial = stateNo;
        }
        D3D11_RECT area;
        bool hasArea, inside;
        if (gc.same && gc.inside) {
            area = gc.rect;
            hasArea = inside = true;
            g_clipWhy = 0;
        } else {
            inside = false;
            hasArea = vu1_area(b, c.vu, s.hasBounds ? s.bmin : nullptr, s.hasBounds ? s.bmax : nullptr, area, &inside);
            if (!hasArea)
                area = {};
        }
        count_direct(inside && part.program != VU1P_FLARE && !g_noDirect);
        if (reflection_skipped_lim(part.program, hasArea, area, ws->reflLim, b.sx, b.sy))
            continue;
        g_profCount[PCOUNT_RUN_WALKED]++;
        cap::t_part = (uint16_t)pi;
        run_sub_vu1(r, s, part, c, area, hasArea, inside, ws->mirrorRange, ws->mirror, ws->noPass);
    }
    t_passState = nullptr;
    cap::t_run = -1;
    cap::t_part = 0xFFFF;
}

void run_submit(const RunWork &w)
{
    run_submit_steps(w);
    w.r->pending.fetch_sub(1, std::memory_order_acq_rel);
}

/* ---- The submit workers (PC, 2026-11-05, ROADMAP item 15 "The parallel
 * executor", step 2) ----------------------------------------------------
 *
 * A replayed run's submission - its states and draw records from the run
 * and the executor's notes (run_submit_steps: sub_vu1, the merged draws,
 * the passes, the binding cache, the D3D thread's calls) - is built on one
 * of a few worker threads instead of the submit thread: the thread's calls
 * are captured into the run's buffer (pgl_queue.h q::t_capture) and the
 * submit thread, reaching the run's record in its ring, waits for the
 * worker and splices the buffer into the D3D thread's ring (run_publish:
 * the calls in the same order as before, the pictures the same).  Each
 * submitting thread has its own binding cache, rings and merged draw
 * (thread_local; pgl_d3d.h Rings); a run starts with its cache forgotten,
 * so its first draw binds everything, and the submit thread forgets its
 * own after a splice.  What a run's submission reads of the others': the
 * alpha bounds of the targets its textures are (Target::alphaLo: the env
 * map, written before the car runs) - the worker waits until the submit
 * thread has run what last wrote them (RunWork::needB, Target::alphaPos);
 * what it writes of them (its own target's bound) goes as deltas the submit
 * thread applies at the publish (AlphaDelta).  A worker whose ring is full
 * waits for the D3D thread to pass the runs before its own (ring_catch_up).
 * GT3PC_PGL_WORKERS=N (0: none - the submit thread builds the runs itself,
 * as step 1); the default is by the machine's cores. */
struct Worker {
    HANDLE thread = nullptr;
    ProfState prof = {};
    Stats stats = {};
};
std::vector<std::unique_ptr<Worker>> g_workers;
SRWLOCK g_workLock = SRWLOCK_INIT;
std::deque<RunWork *> g_workQueue;
std::atomic<uint32_t> g_workPosted{0};     /* (the workers sleep on it) */
std::atomic<bool> g_workStop{false};

bool workers_on()
{
    return !g_workers.empty();
}

void work_push(RunWork *w)
{
    AcquireSRWLockExclusive(&g_workLock);
    g_workQueue.push_back(w);
    ReleaseSRWLockExclusive(&g_workLock);
    g_workPosted.fetch_add(1, std::memory_order_seq_cst);
    WakeByAddressSingle(&g_workPosted);
}

/* (a worker) the next run, waited for; null at the stop */
RunWork *work_pop()
{
    for (;;) {
        uint32_t seen = g_workPosted.load(std::memory_order_seq_cst);
        AcquireSRWLockExclusive(&g_workLock);
        RunWork *w = nullptr;
        if (!g_workQueue.empty()) {
            w = g_workQueue.front();
            g_workQueue.pop_front();
        }
        ReleaseSRWLockExclusive(&g_workLock);
        if (w != nullptr)
            return w;
        if (g_workStop.load(std::memory_order_seq_cst))
            return nullptr;
        for (int i = 0; i < 2000 && g_workPosted.load(std::memory_order_seq_cst) == seen; i++)
            _mm_pause();
        if (g_workPosted.load(std::memory_order_seq_cst) == seen)
            WaitOnAddress(&g_workPosted, &seen, sizeof(seen), 20);
    }
}

/* (a worker) a run's submission built into its capture */
void work_run(RunWork &w)
{
    sq::wait_processed(w.needB);            /* (the bounds it reads are as of its place) */
    q::t_capture = &w.cap;
    t_alphaDeltas = &w.alpha;
    d3d_forget();                           /* (this thread's cache: unknown at a run's start) */
    run_submit_steps(w);
    q::release_held();                      /* (its last draw, into the capture) */
    q::t_capture = nullptr;
    t_alphaDeltas = nullptr;
    w.done.store(1, std::memory_order_release);
    WakeByAddressAll(&w.done);
}

DWORD WINAPI work_thread(void *arg)
{
    Worker &me = *static_cast<Worker *>(arg);
    t_prof = &me.prof;
    g_profCount = me.prof.count;
    t_stats = &me.stats;
    {
        static std::atomic<int> nth{0};     /* PC (2026-11-06): (the frame capture's thread column: 2, 3, ...) */
        cap::t_thread = (uint8_t)std::min(255, 2 + nth.fetch_add(1));
    }
    sample_prof::register_thread("submit worker");  /* PC: GT3PC_SAMPLE */
    d3d_thread_init();                      /* (its rings: pgl_d3d.h Rings) */
    for (;;) {
        RunWork *w = work_pop();
        if (w == nullptr)
            break;
        work_run(*w);
    }
    return 0;
}

/* (the submit stage) a worker's run: waited for, its calls spliced into the
 * D3D thread's ring, its alpha bound writes done, this thread's cache
 * forgotten (the D3D state is the run's last) */
void run_publish(RunWork &w)
{
    q::release_held();
    int spins = 0;
    while (w.done.load(std::memory_order_acquire) == 0) {
        if (spins < 4000) {
            _mm_pause();
            spins++;
            continue;
        }
        uint32_t zero = 0;
        WaitOnAddress(&w.done, &zero, sizeof(zero), 10);
    }
    q::splice(w.cap.buf.data(), w.cap.bytes(), w.cap.count);
    for (const AlphaDelta &d : w.alpha) {
        Target &t = *d.t;
        if (d.full) {
            t.alphaLo = d.lo;
            t.alphaHi = d.hi;
        } else if (t.alphaLo >= 0) {
            t.alphaLo = std::min(t.alphaLo, d.lo);
            t.alphaHi = std::max(t.alphaHi, d.hi);
        }
    }
    d3d_forget();
    w.r->pending.fetch_sub(1, std::memory_order_acq_rel);
}

void workers_stop()
{
    if (g_workers.empty())
        return;
    g_workStop.store(true, std::memory_order_seq_cst);
    g_workPosted.fetch_add(1, std::memory_order_seq_cst);
    WakeByAddressAll(&g_workPosted);
    for (auto &wk : g_workers)
        WaitForSingleObject(wk->thread, 2000);
    g_workers.clear();
}

void workers_start()
{
    const char *e = getenv("GT3PC_PGL_WORKERS");
    int n = e != nullptr ? atoi(e) : -1;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int cpus = (int)si.dwNumberOfProcessors;
    if (n < 0)
        n = cpus >= 8 ? 2 : cpus >= 6 ? 1 : 0;
    n = std::min(n, 8);
    if (n <= 0 || !sq::running()) {
        pclog::write("pgl: no submit workers (%s): the submit thread builds the replayed runs itself",
                     !sq::running() ? "no submit thread" : e != nullptr ? "GT3PC_PGL_WORKERS=0" : "few cores");
        return;
    }
    for (int i = 0; i < n; i++) {
        std::unique_ptr<Worker> wk(new Worker());
        wk->thread = CreateThread(nullptr, 0, work_thread, wk.get(), 0, nullptr);
        if (wk->thread == nullptr)
            break;
        SetThreadDescription(wk->thread, L"pgl submit worker");
        g_workers.push_back(std::move(wk));
    }
    if (!g_workers.empty())
        atexit(&workers_stop);
    pclog::write("pgl: %d submit worker(s) build the replayed runs' submissions, spliced in order by the submit"
                 " thread (GT3PC_PGL_WORKERS=N; 0 off)", (int)g_workers.size());
}

/* The run [i, j) replayed (true), or its recording begun (the commands
 * execute as ever, hooked) */
bool run_try(const CmdList &list, size_t i, size_t j, uint64_t key)
{
    auto it = g_runs.find(key);
    if (it != g_runs.end()) {
        RunRec &r = *it->second;
        /* PC (2026-11-08): the record whole first (run_record_sound: a
         * damaged one is resolved again; run_models_same reads the list by its
         * indices).  GT3PC_PGL_RUN_DAMAGE=1: the user's crash's write (op
         * command 0x00010014) over an op of the first run replayed 100 times,
         * once - the check's test: a log line, the run resolved again, the
         * pictures the same; =2: a padding byte of a part instead (found by
         * the hash at the 128th replay) */
        if (g_runDamageTest != 0 && r.hits == 100 && r.ops.size() >= 4 && !r.parts.empty()) {
            if (g_runDamageTest == 1) {
                r.ops[r.ops.size() - 4].cmd = 0x00010014u;
                pclog::write("pgl runs: GT3PC_PGL_RUN_DAMAGE=1: op %u of a run's %u written over (command 0x00010014)",
                             (unsigned)r.ops.size() - 4, (unsigned)r.ops.size());
            } else {
                /* (=2: a byte nothing reads - the padding after hasPrim -
                 * which only the every-32nd-replay hash sees) */
                static_assert(offsetof(RunPart, hasPrim) + 1 < sizeof(RunPart), "RunPart has no padding to write");
                reinterpret_cast<uint8_t *>(&r.parts.back())[sizeof(RunPart) - 1] ^= 0x5A;
                pclog::write("pgl runs: GT3PC_PGL_RUN_DAMAGE=2: a padding byte of part %u of a run's %u written over",
                             (unsigned)r.parts.size() - 1, (unsigned)r.parts.size());
            }
            g_runDamageTest = 0;
        }
        if (r.nCmds == (uint32_t)(j - i) && r.overlayGen == models_overlay_gen() && r.aniso == g_aniso &&
            run_record_sound(r) && run_targets_same(r) && run_models_same(list, i, r)) {
            r.lastUse = g_execSeconds;
            r.hits++;
            g_profCount[PCOUNT_RUN_HIT]++;
            t_stats->runsHit++;
            /* PC (2026-11-06): room in the texture records for the ones this
             * replay may make, taken now while nothing points into the
             * vector (no work of the run pending, none begun): it must not
             * move under the replay's work (run_replay_tex) */
            if (r.pending.load(std::memory_order_acquire) == 0 && r.texs.capacity() - r.texs.size() < 16)
                r.texs.reserve(r.texs.size() + 64);
            run_replay(list, i, r);
            return true;
        }
        /* (its targets or meshes changed: resolved again; PC: kept until the
         * frame ends - the stage may still read a work of it, RunWork) */
        g_runGrave.push_back(std::move(it->second));
        g_runs.erase(it);
    } else {
        /* (new: resolved at its next frames, once its key has come round -
         * not a run of the course whose parts change with the camera) */
        RunSeen &seen = g_runSeen[(uint32_t)(key >> 20) % kRunSeen];
        const uint64_t frame = frame_number();
        if (seen.key != key || frame - seen.frame > 8 || frame == seen.frame) {
            seen.key = key;
            seen.frame = frame;
            return false;
        }
    }
    g_runRec.reset(new RunRec());
    g_runRec->key = key;
    g_runRec->nCmds = (uint32_t)(j - i);
    g_runRec->overlayGen = models_overlay_gen();
    g_runRec->aniso = g_aniso;              /* PC (2026-11-10) */
    for (size_t k = i; k < j; k++)          /* (its commands by kind: a replay counts them at once) */
        g_runRec->cmdCounts[list.type(k) & 31]++;
    g_runStart = i;
    g_runEnd = j;
    run_rows_reset();                       /* PC: (the run's CLUT loads numbered from here) */
    texture_ident_capture(true);            /* PC: (the binds' identities taken: run_rec_tex) */
    /* (the run's first draw works its whole state out, so the run holds it:
     * a replay starts from whatever state the draws before left - the same
     * registers give the same state, and compute_state is cheap once) */
    g_dirty = true;
    g_profCount[PCOUNT_RUN_MISS]++;
    t_stats->runsMiss++;
    return false;
}

/* Runs unused for kRunExpire seconds dropped; beyond kRunMaxRuns the least
 * recently replayed (exec_end_frame, every 64 frames) */
void runs_housekeep()
{
    const double now = g_execSeconds;
    double oldest = 1e300;
    for (auto it = g_runs.begin(); it != g_runs.end();) {
        if (it->second->lastUse + kRunExpire < now) {
            it = g_runs.erase(it);
        } else {
            oldest = std::min(oldest, it->second->lastUse);
            ++it;
        }
    }
    /* (beyond the most: the oldest half of a second's worth, by their last
     * replay, until under it) */
    while (g_runs.size() > kRunMaxRuns && oldest < 1e300) {
        const double cut = oldest + 0.5;
        oldest = 1e300;
        for (auto it = g_runs.begin(); it != g_runs.end();) {
            if (it->second->lastUse <= cut) {
                it = g_runs.erase(it);
            } else {
                oldest = std::min(oldest, it->second->lastUse);
                ++it;
            }
        }
    }
}

/* ======================================================================== */
/* Effect recipes                                                           */
/* ======================================================================== */

/* PC: a pass over a target, queued for the D3D thread (pgl_queue.h): its
 * fixed state - the textures unbound, rtv the only target over vp, the
 * rasterizer, blend (factor: nullptr for none) and depth states (found by
 * the caller) - and its draw: srv0 / srv1 bound (sampler s if not null), a
 * triangle over the target, the textures unbound again. */
void queue_pass_state(ID3D11RenderTargetView *rtv, const D3D11_VIEWPORT &vp, ID3D11RasterizerState *rs,
                      ID3D11BlendState *bs, const float *factor, ID3D11DepthStencilState *ds,
                      ID3D11DepthStencilView *dsv = nullptr, uint8_t stencilRef = 0)
{
    float f[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const bool hasFactor = factor != nullptr;
    if (hasFactor)
        memcpy(f, factor, sizeof(f));
    q::run([rtv, vp, rs, bs, f, hasFactor, ds, dsv, stencilRef] {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(1, &rtv, dsv);    /* PC: (dsv: the alpha mirror's sync, mirror_sync) */
        qdc()->RSSetViewports(1, &vp);
        qdc()->RSSetState(rs);
        qdc()->OMSetBlendState(bs, hasFactor ? f : nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(ds, stencil_ref_byte(stencilRef));   /* PC (2026-11-06): (the mirror's bit) */
    });
}

void queue_pass_draw(ID3D11ShaderResourceView *srv0, ID3D11ShaderResourceView *srv1, ID3D11SamplerState *s)
{
    uint32_t capId = ~0u;
    if (cap::on()) {
        /* PC (2026-11-06): the frame capture's record (the pass by its shader) */
        cap::Draw d;
        d.kind = cap::K_PASS;
        d.count = 3;
        d.note = ps_name(bound_ps());
        capId = cap::draw_begin(d);
    }
    q::run([srv0, srv1, s, capId] {
        ID3D11ShaderResourceView *srvs[2] = {srv0, srv1};
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, srvs);
        if (s != nullptr)
            qdc()->PSSetSamplers(0, 1, &s);
        qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cap::gpu_begin(capId);
        qdc()->Draw(3, 0);
        cap::gpu_end(capId);
        qdc()->PSSetShaderResources(0, 2, none);
    });
}

void exec_pfx_color(const CmdPfxColor &c)
{
    flush_batch();
    Target *src = find_color_target(c.src);
    Target *dst = color_target(c.dst, c.fbw, 0, c.w, c.h);
    if (src == nullptr || dst == nullptr) {
        log_once("pgl-pfx-color-none", "pgl: colour correction without its frame target: skipped");
        return;
    }
    ID3D11ShaderResourceView *srcSrv = (src == dst) ? target_copy_srv(*src, true) : target_srv(*src);
    uint32_t curve[256];
    for (int i = 0; i < 256; i++)
        curve[i] = (uint32_t)c.curve[3 * i] | ((uint32_t)c.curve[3 * i + 1] << 8) |
                   ((uint32_t)c.curve[3 * i + 2] << 16) | 0xFF000000u;
    Com<ID3D11Texture2D> ctex;
    Com<ID3D11ShaderResourceView> csrv;
    if (!make_texture(256, 1, curve, ctex, csrv))
        return;
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {0, 0, (float)dst->nw, (float)dst->nh, 0, 1};
    queue_pass_state(dst->rtv.get(), vp, raster_state(0, false), blend_plain(gs::WRITE_RGB), nullptr,
                     depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::PfxColor);
    CbPfx cb = {};
    for (int k = 0; k < 3; k++) {
        cb.colR[k] = (float)((c.colR >> (8 * k)) & 0xFF);
        cb.colG[k] = (float)((c.colG >> (8 * k)) & 0xFF);
        cb.colB[k] = (float)((c.colB >> (8 * k)) & 0xFF);
    }
    cb.srcScale[0] = (float)src->nw / (float)dst->nw;
    cb.srcScale[1] = (float)src->nh / (float)dst->nh;
    bind_ps_cb(&cb, sizeof(cb));
    queue_pass_draw(srcSrv, csrv.get(), nullptr);
    d3d_invalidate();
    dst->gen++;
    dst->execAlpha = -1;                    /* PC (2026-11-06): (its alpha no longer known flat) */
    registry_target_written(*dst, true, false);
    if (g_tracing)
        pclog::write("pgl trace %u: colour correction of the frame at 0x%X into 0x%X (%dx%d)", g_traceN++, c.src,
                     c.dst, c.w, c.h);
}

/* pfx_split_rgb_planes (the afterimage's capture, graphics.md 8.6): the
 * frame at c.src (the TEX0 texture: the frame buffer shown last) split into
 * the planes at c.r / c.g / c.b, which drawAfterimage then reads as PSMT8H
 * (registry_split_planes). */
void exec_pfx_split(const CmdPfxSplit &c)
{
    flush_batch();
    Target *src = find_color_target(c.src);
    if (src == nullptr) {
        log_once("pgl-pfx-split-none", "pgl: afterimage capture of block 0x%X, which no draw wrote: skipped", c.src);
        return;
    }
    const uint32_t blocks[3] = {c.r, c.g, c.b};
    registry_split_planes(*src, blocks, c.w, c.h);
    if (g_tracing)
        pclog::write("pgl trace %u: afterimage capture of the frame at 0x%X into the planes at 0x%X / 0x%X / 0x%X"
                     " (%dx%d)", g_traceN++, c.src, c.r, c.g, c.b, c.w, c.h);
}

/* One halving step of the flare reduction on one axis: a texels down to b
 * (pgl_flare.cpp flare_reduce / flare_build_reduce). */
struct FlareStep { int a, b; };

/* The next size of a halving (an odd size goes to the even one below, 1
 * stays 1). */
int flare_half(int n)
{
    int h = n >> 1;
    if (((h << 1) ^ n) != 0)
        h <<= 1;
    return h < 1 ? 1 : h;
}

/* The weight of each copied texel of one axis in texel 0 after the steps:
 * the work buffer (64 texels, clamped) holds `copied' texels of the copy,
 * the rest stale (taken as 0).  Each step writes texels 0..b-1 from a
 * bilinear sample at t = 0.5 + X a / b of the buffer as it was (the sprite's
 * texel mapping, docs/renderer.md 8): in place, so a later step can read a
 * texel an earlier level left (odd sizes reach texel a). */
void flare_axis_weights(int copied, const FlareStep *steps, int nSteps, float *out)
{
    /* PC (CPU time, 2026-11-03): kept by the copied texels and the steps -
     * what they are a function of; the same few box sizes come back every
     * frame, and each query worked the 64 x 64 matrix below twice (~4 % of a
     * race frame's executor time, user capture) */
    struct Memo {
        int copied = -1, nSteps = 0;
        FlareStep steps[16];
        float out[68];
    };
    static Memo memo[64];
    nSteps = std::min(nSteps, 16);
    uint64_t h = (uint64_t)copied << 8 | (uint64_t)nSteps;
    for (int s = 0; s < nSteps; s++)
        h = mul64(h ^ ((uint64_t)(uint32_t)steps[s].a << 32 | (uint32_t)steps[s].b), 0x100000001B3ull);
    Memo &e = memo[(uint32_t)(h ^ (h >> 29)) % 64];
    if (e.copied == copied && e.nSteps == nSteps && memcmp(e.steps, steps, (size_t)nSteps * sizeof(FlareStep)) == 0) {
        memcpy(out, e.out, sizeof(e.out));
        return;
    }
    static float m[64][64], n[64][64];
    for (int k = 0; k < 64; k++)
        for (int c = 0; c < 64; c++)
            m[k][c] = (k == c && k < copied) ? 1.0f : 0.0f;
    for (int s = 0; s < nSteps; s++) {
        const int a = steps[s].a, b = std::min(steps[s].b, 64);
        for (int x = 0; x < b; x++) {
            const float t = 0.5f + (float)x * (float)a / (float)b;
            const int i = (int)std::floor(t);
            const float f = t - (float)i;
            const int i0 = std::min(std::max(i, 0), 63), i1 = std::min(std::max(i + 1, 0), 63);
            for (int c = 0; c < 64; c++)
                n[x][c] = m[i0][c] * (1.0f - f) + m[i1][c] * f;
        }
        for (int x = 0; x < b; x++)
            memcpy(m[x], n[x], sizeof(m[x]));
    }
    for (int c = 0; c < 68; c++)
        out[c] = c < 64 ? m[0][c] : 0.0f;
    e.copied = copied;
    e.nSteps = nSteps;
    memcpy(e.steps, steps, (size_t)nSteps * sizeof(FlareStep));
    memcpy(e.out, out, sizeof(e.out));
}

/* GT3PC_PGL_TRACE: the probe points the next traced frame reads (the
 * centres of the flare queries the shape passed in; F9 traces two frames). */
int g_flareProbeN = 0;
int g_flareProbeX[4], g_flareProbeY[4];
uint32_t g_flareProbePassed[4];

/* GT3PC_PGL_TRACE: what a flare query saw - the screen's depth in the
 * shape's pixels (read back: the traced frame only) against the light's Z,
 * the pixels that passed and the result the weights give.  The queries the
 * shape passed in are remembered as probe points for the next traced frame:
 * its probes show which draws paint those pixels (and whether they write
 * Z). */
void flare_trace_depth(const PglrFlareQuery &c, const CbFlare &cb, Target *zt)
{
    ID3D11Texture2D *ztex = zt != nullptr && zt->tex ? target_tex1(*zt) : nullptr;
    if (ztex == nullptr) {
        pclog::write("pgl trace %u:   (no depth test)", g_traceN++);
        return;
    }
    static Com<ID3D11Texture2D> stage;
    static UINT stageW = 0, stageH = 0;
    D3D11_TEXTURE2D_DESC d = {};
    ztex->GetDesc(&d);
    if (!stage || stageW != d.Width || stageH != d.Height) {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        stage.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, stage.put())))
            return;
        stageW = d.Width;
        stageH = d.Height;
    }
    dc()->CopyResource(stage.get(), ztex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(dc()->Map(stage.get(), 0, D3D11_MAP_READ, 0, &m)))
        return;
    const float zn = z_norm((uint8_t)c.zpsm);
    uint32_t inShape = 0, passed = 0;
    double zmin = 1e30, zmax = -1e30, sum = 0.0;
    for (int y = 0; y < cb.box[3]; y++) {
        for (int x = 0; x < cb.box[2]; x++) {
            const int gx = std::min(std::max(c.box[0] + x, 0), c.frameW - 1);
            const int gy = std::min(std::max(c.box[1] + y, 0), c.frameH - 1);
            bool inside = false;
            for (int r = 0; r < c.nRects; r++)
                inside = inside || (gx >= c.rect[r][0] && gx < c.rect[r][2] && gy >= c.rect[r][1] && gy < c.rect[r][3]);
            if (!inside)
                continue;
            const int fy = c.halfY ? gy / 2 : gy;
            const UINT nx = (UINT)std::min((int)stageW - 1, (int)(((float)gx + 0.5f) * cb.scale[0]));
            const UINT ny = (UINT)std::min((int)stageH - 1, (int)(((float)fy + 0.5f) * cb.scale[1]));
            float sz;
            memcpy(&sz, (const uint8_t *)m.pData + (size_t)ny * m.RowPitch + (size_t)nx * 8, 4);
            const bool ok = c.ztest == 1 || (c.ztest == 2 && cb.z >= sz) || (c.ztest == 3 && cb.z > sz);
            inShape++;
            if (ok) {
                passed++;
                sum += (double)cb.wx[x] * (double)cb.wy[y] * 255.0;
            }
            zmin = std::min(zmin, (double)sz / zn);
            zmax = std::max(zmax, (double)sz / zn);
        }
    }
    dc()->Unmap(stage.get(), 0);
    const uint32_t a = (uint32_t)std::min(255.0, std::floor(sum + 0.5));
    pclog::write("pgl trace %u:   light Z 0x%06X; %u px in the shape, %u passed; scene Z 0x%06X .. 0x%06X;"
                 " A %u, result %u", g_traceN++, c.z, inShape, passed, inShape ? (uint32_t)zmin : 0u,
                 inShape ? (uint32_t)zmax : 0u, a, std::min(255u, (c.cs * a) >> 7));
    if (passed > 0) {
        /* keep the four with the most pixels passing */
        int slot = g_flareProbeN < 4 ? g_flareProbeN++ : -1;
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < 4; i++)
                if (g_flareProbePassed[i] < g_flareProbePassed[slot])
                    slot = i;
            if (g_flareProbePassed[slot] >= passed)
                return;
        }
        g_flareProbeX[slot] = c.box[0] + c.box[2] / 2;
        g_flareProbeY[slot] = c.box[1] + c.box[3] / 2;
        g_flareProbePassed[slot] = passed;
    }
}

/* PC (2026-11-05): the flare queries of a frame drawn as one batch (pgl_d3d.h
 * FlareRec, flare_batch_draw; docs/renderer.md 8).  ps_flare did each query
 * in one pixel, a loop over the box's GS pixels: 37 us of GPU for a 10 x 10
 * lamp query on a GTX 1660 SUPER, serial in one thread, and the next
 * sprite's depth test waited for it - a night race's 150 lamp and course
 * light queries were 5.7 ms of a 15 ms frame (the "effects" GPU section,
 * the five-run test of diag\gpu_ab.cmd).  Now a query joins the frame's
 * batch (its record and the weights set of its box size: g_flareQ) and the
 * batch is drawn - a point per GS pixel of each query, its term added into
 * the query's texel of a float scratch, then a point per query writing the
 * strip's texel - before anything that could change what the queries read
 * or read what they write: a GS draw that is not one of the queries' own
 * sprites (textured, writing depth, into another target or with another
 * depth buffer: flush_batch), a model part (the glows read the strip), a
 * VU1 strip, an upload, a target, a readback, a sub-list, a recipe
 * (flare_queries_check), a replayed run, the frame's end.  The sums are the
 * loop's floats (the points blend in primitive order, the loop's order):
 * measured against ps_flare over 15,000 random queries, every texel equal
 * (the harness: a frame of 150 lamp queries 5.9 ms -> 0.06 ms).
 * GT3PC_PGL_FLARE_BATCH=0: each query its own pass as before (a traced
 * frame too, for its per-query lines). */
struct FlareQueue {
    std::vector<FlareRec> recs;
    std::vector<FlareWeights> weights;
    Target *rt = nullptr, *zt = nullptr, *strip = nullptr;
    FlareBatch b = {};
};
FlareQueue g_flareQ;
bool g_flareBatchOn = true;
constexpr size_t kFlareBatchMax = 4096;     /* (records a batch; the strip holds 64 x 16 at most anyway) */

bool flare_queries_pending()
{
    return !g_flareQ.recs.empty();
}

/* A GS draw the queued queries may wait through: one of their own sprites -
 * untextured, no depth write, into the queries' screen with its depth buffer
 * or none (the box clear, the shape) */
bool flare_sprite_ok()
{
    const BatchState &b = g_bs;
    return !b.tme && !b.wroteZ && !b.depthFrame && b.rt == g_flareQ.rt &&
           (b.zt == nullptr || b.zt == g_flareQ.zt);
}

/* A command the queued queries may wait through (the queries' sprites: GS
 * register writes and vertices; the sections' marks) */
void flare_queries_check(int type)
{
    switch (type) {
    case CMD_REGS: case CMD_AD: case CMD_PRIM: case CMD_GIF: case CMD_FLARE_QUERY: case CMD_GPU_SECTION:
    case CMD_DESIGN: case CMD_LINE_WIDTH:
        return;
    default:
        flare_queries_flush();
    }
}

void flare_queries_flush()
{
    FlareQueue &fq = g_flareQ;
    if (fq.recs.empty())
        return;
    const int prevSec = sec_active() ? sec_want(PGLR_SEC_FLARE_QUERY) : -1;   /* PC: (the GPU sections) */
    ID3D11ShaderResourceView *zsrv = fq.zt != nullptr ? target_srv(*fq.zt) : nullptr;      /* (a resolve first) */
    fq.b.nRecs = (uint32_t)fq.recs.size();
    fq.b.nWeights = (uint32_t)fq.weights.size();
    fq.b.stripW = fq.strip->nw;             /* (at GS resolution: target_gs_resolution) */
    fq.b.stripH = fq.strip->nh;
    d3d_invalidate();
    uint32_t capId = ~0u;
    if (cap::on()) {
        /* PC (2026-11-06): the frame capture's record (the batch as one) */
        cap::Draw d;
        d.kind = cap::K_FLARE;
        d.count = fq.b.nRecs;
        d.target = fq.strip->base;
        d.targetW = (uint16_t)fq.strip->nw;
        d.targetH = (uint16_t)fq.strip->nh;
        d.note = "flare queries' batch";
        capId = cap::draw_begin(d);
    }
    flare_batch_draw(fq.b, fq.recs.data(), fq.weights.data(), zsrv, fq.strip->rtv.get(), capId);
    d3d_invalidate();
    fq.strip->gen++;
    fq.strip->execAlpha = -1;               /* PC (2026-11-06): (its alpha no longer known flat) */
    registry_target_written(*fq.strip, true, true);
    {
        Target *st = fq.strip;              /* PC: (its alpha bound: the texels' alphas are the queries' results) */
        sq::run([st] { st->alphaLo = st->alphaHi = -1; });
        st->alphaPos = sq::head();
    }
    t_stats->flareBatches++;
    fq.recs.clear();
    fq.weights.clear();
    fq.rt = fq.zt = fq.strip = nullptr;
    if (prevSec >= 0)
        sec_want(prevSec);
}

/* A query into the batch (exec_flare_query's values); a batch shares the
 * screen, its depth buffer, the strip and the frame's scale */
void flare_query_queue(const PglrFlareQuery &c, Target *rt, Target *zt, Target *strip, int qx, int qy, int cols,
                       int rows, const float *wx, const float *wy, const float *scale, float z)
{
    FlareQueue &fq = g_flareQ;
    const int32_t halfY = c.halfY ? 1 : 0;
    if (!fq.recs.empty() &&
        (fq.rt != rt || fq.zt != zt || fq.strip != strip || fq.b.scale[0] != scale[0] ||
         fq.b.scale[1] != scale[1] || fq.b.screen[0] != c.frameW || fq.b.screen[1] != c.frameH ||
         fq.b.halfY != halfY || fq.recs.size() >= kFlareBatchMax))
        flare_queries_flush();
    if (fq.recs.empty()) {
        fq.rt = rt;
        fq.zt = zt;
        fq.strip = strip;
        fq.b = FlareBatch{};
        fq.b.scale[0] = scale[0];
        fq.b.scale[1] = scale[1];
        fq.b.screen[0] = c.frameW;
        fq.b.screen[1] = c.frameH;
        fq.b.halfY = halfY;
    }
    /* the weights set of its box size (a few a frame: the lamps' 11 x 11, the sun's) */
    FlareWeights wt;
    memcpy(wt.wx, wx, sizeof(wt.wx));
    memcpy(wt.wy, wy, sizeof(wt.wy));
    int32_t wi = -1;
    for (size_t k = 0; k < fq.weights.size() && wi < 0; k++)
        if (memcmp(&fq.weights[k], &wt, sizeof(wt)) == 0)
            wi = (int32_t)k;
    if (wi < 0) {
        wi = (int32_t)fq.weights.size();
        fq.weights.push_back(wt);
    }
    FlareRec r = {};
    r.box[0] = c.box[0];
    r.box[1] = c.box[1];
    r.box[2] = cols;
    r.box[3] = rows;
    memcpy(r.rect, c.rect, sizeof(r.rect));
    r.nRects = c.nRects;
    r.ztest = c.ztest;
    r.cs = (int32_t)c.cs;
    r.weights = wi;
    r.z = z;
    r.texelX = (float)qx;
    r.texelY = (float)qy;
    fq.recs.push_back(r);
}

/* A lens-flare occlusion query (pgl_flare.cpp flare_occlusion_query,
 * docs/renderer.md 8): the box of the screen tested natively against the
 * screen's depth (ps_flare), the result written into the query's texel of
 * the result strip, which flare_readback reads back for the glows.  This
 * stands in for the GS work after the shape (the copy into the work buffer,
 * the bilinear halvings, the Cs x As store): emulated at the output scale
 * they gave no usable result.  The result is the GS's: the halvings are
 * applied as weights of the copied pixels (flare_axis_weights).  PC
 * (2026-11-05): queued for the frame's batch (above), or - GT3PC_PGL_FLARE_BATCH=0,
 * a traced frame - drawn as its own pass here. */
void exec_flare_query(const PglrFlareQuery &c)
{
    flush_batch();
    /* The screen and its Z buffer: the very targets the shape's draw - the
     * draw just before, into this frame with this Z buffer - used.  (By
     * address alone the screen's block also names other buffers: the car
     * icons' 64 x 32 ones share the race screen's 0x1A60; a lookup that took
     * one of them read an empty Z buffer, every light passed and glows showed
     * through the course.) */
    Target *rt = nullptr, *zt = nullptr;
    if (g_bs.rt != nullptr && !g_bs.depthFrame && g_bs.rt->base == c.frameBlock) {
        rt = g_bs.rt;
        if (g_bs.zt != nullptr && g_bs.zt->base == c.zBlock)
            zt = g_bs.zt;
    }
    if (rt == nullptr)
        rt = color_target(c.frameBlock, c.fbw, c.fpsm, c.frameW, c.frameH);
    /* the strip's size, as the PS2's store set its draw buffer
     * (flare_set_target(occVram, 64, occHeight)): another buffer known at
     * the same block must not stand in for it */
    target_size(c.resultBlock, 64, 0, 64, c.resultH);
    Target *strip = color_target(c.resultBlock, 1, 0, 64, c.resultH);
    if (rt == nullptr || strip == nullptr) {
        log_once("pgl-flare-query-none", "pgl: a flare query without its screen or result strip: skipped");
        return;
    }
    if (c.ztest >= 2 && zt == nullptr) {
        zt = depth_target(c.zBlock, 0x30u | (c.zpsm & 0x0F), *rt);
        if (zt == nullptr || !zt->tex) {
            log_once("pgl-flare-query-z", "pgl: a flare query without the screen's Z buffer: skipped");
            return;
        }
    }
    if (c.ztest < 2)
        zt = nullptr;
    target_gs_resolution(*strip, false, "holds the flare query results");
    g_flareStrip = strip;                   /* (the glow draws read it: exec_model) */
    const int qx = c.q & 0x3F, qy = c.q >> 6;
    if (qy >= strip->gsH)
        return;

    /* the copy (flare_copy_visibility): w + 1 x h + 1 screen pixels from the
     * box's origin, 1:1, at most the 64 x 64 work buffer; the reduction
     * (flare_run_reduce) of w x h: none below 2, the prebuilt square one (w
     * on both axes) below 18, else flare_reduce's (both axes in step) */
    const int w = c.box[2], h = c.box[3];
    const int cols = std::min(w + 1, 64), rows = std::min(h + 1, 64);
    FlareStep sx[16], sy[16];
    int nSteps = 0;
    if (w >= 2) {
        int a = std::min(w, 64), b = w < 18 ? a : std::min(h, 64);
        while ((a >= 2 || b >= 2) && nSteps < 16) {
            const int na = flare_half(a), nb = flare_half(b);
            sx[nSteps] = FlareStep{a, na};
            sy[nSteps] = FlareStep{b, nb};
            nSteps++;
            a = na;
            b = nb;
        }
    }
    float wx[68], wy[68];
    flare_axis_weights(cols, sx, nSteps, wx);
    flare_axis_weights(rows, sy, nSteps, wy);
    const float scale[2] = {zt != nullptr ? (float)zt->nw / (float)rt->gsW : 1.0f,
                            zt != nullptr ? (float)zt->nh / (float)rt->gsH : 1.0f};
    const float z = (float)c.z * z_norm((uint8_t)c.zpsm);
    if (g_flareBatchOn && !g_tracing) {
        flare_query_queue(c, rt, zt, strip, qx, qy, cols, rows, wx, wy, scale, z);
        return;
    }

    /* the query as a pass of its own: ps_flare's loop over the box in one pixel */
    const int prevSec = sec_active() ? sec_want(PGLR_SEC_FLARE_QUERY) : -1;   /* PC: (the GPU sections) */
    static CbFlare cb;
    memset(&cb, 0, sizeof(cb));
    cb.box[0] = c.box[0];
    cb.box[1] = c.box[1];
    cb.box[2] = cols;
    cb.box[3] = rows;
    memcpy(cb.wx, wx, sizeof(cb.wx));
    memcpy(cb.wy, wy, sizeof(cb.wy));
    cb.screen[0] = c.frameW;
    cb.screen[1] = c.frameH;
    memcpy(cb.rect, c.rect, sizeof(cb.rect));
    cb.nRects = c.nRects;
    cb.ztest = c.ztest;
    cb.cs = (int32_t)c.cs;
    cb.halfY = c.halfY;
    cb.scale[0] = scale[0];
    cb.scale[1] = scale[1];
    cb.z = z;

    ID3D11ShaderResourceView *srv = zt != nullptr ? target_srv(*zt) : nullptr;     /* (a resolve first) */
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {(float)qx * strip->sx, (float)qy * strip->sy, strip->sx, strip->sy, 0, 1};
    queue_pass_state(strip->rtv.get(), vp, raster_state(0, false), blend_plain(gs::WRITE_RGB | gs::WRITE_A),
                     nullptr, depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::Flare);
    bind_flare_cb(cb);
    queue_pass_draw(srv, nullptr, nullptr);
    d3d_invalidate();
    strip->gen++;
    strip->execAlpha = -1;                  /* PC (2026-11-06): (its alpha no longer known flat) */
    registry_target_written(*strip, true, true);
    sq::run([strip] { strip->alphaLo = strip->alphaHi = -1; });     /* PC: (its alpha bound: the query's result) */
    strip->alphaPos = sq::head();
    if (g_tracing) {
        pclog::write("pgl trace %u: flare query %d: box %d,%d %dx%d, %d rect(s), ztest %d, Z 0x%06X, Cs 0x%02X;"
                     " screen 0x%X %dx%d (native %dx%d), Z buffer 0x%X (native %dx%d), strip 0x%X %dx%d",
                     g_traceN++, c.q, c.box[0], c.box[1], c.box[2], c.box[3], c.nRects, c.ztest, c.z, c.cs,
                     rt->base, rt->gsW, rt->gsH, rt->nw, rt->nh, zt ? zt->base : 0u, zt ? zt->nw : 0,
                     zt ? zt->nh : 0, strip->base, strip->gsW, strip->gsH);
        flare_trace_depth(c, cb, zt);
    }
    if (prevSec >= 0)
        sec_want(prevSec);
}

/* Draws at half strength (pglr_half_blend; docs/renderer.md, "Fields"): the
 * buffer's contents at the begin, and the buffer then. */
Target *g_halfRt = nullptr;
Com<ID3D11Texture2D> g_halfSnap;
Com<ID3D11ShaderResourceView> g_halfSnapSrv;
Com<ID3D11BlendState> g_halfBlend;

/* The begin keeps a copy of the buffer; the end draws that copy over it with
 * 0.5 / 0.5 blending (RGB: the draws' alpha stays, as on the rows they drew
 * on the PS2), so where the draws changed nothing it stays as it is. */
void exec_half_blend(const CmdHalfBlend &c)
{
    flush_batch();
    if (c.begin != 0) {
        g_halfRt = nullptr;
        Target *rt = color_target(c.base, c.fbw, c.psm, c.w, c.h);
        ID3D11Texture2D *src = rt != nullptr && rt->tex ? target_tex1(*rt) : nullptr;     /* (a resolve) */
        if (src == nullptr)
            return;
        D3D11_TEXTURE2D_DESC d, have = {};
        src->GetDesc(&d);
        if (g_halfSnap)
            g_halfSnap->GetDesc(&have);
        if (!g_halfSnap || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
            g_halfSnapSrv.reset();
            g_halfSnap.reset();
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = 0;
            d.MiscFlags = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(dev()->CreateTexture2D(&d, nullptr, g_halfSnap.put())) ||
                FAILED(dev()->CreateShaderResourceView(g_halfSnap.get(), &sv, g_halfSnapSrv.put()))) {
                g_halfSnap.reset();
                g_halfSnapSrv.reset();
                log_once("pgl-half-blend", "pgl: no copy for draws at half strength: drawn at full strength");
                return;
            }
        }
        ID3D11Texture2D *snap = g_halfSnap.get();
        q::run([snap, src] { qdc()->CopyResource(snap, src); });
        g_halfRt = rt;
        return;
    }
    Target *rt = g_halfRt;
    g_halfRt = nullptr;
    if (rt == nullptr || !rt->rtv)
        return;
    D3D11_TEXTURE2D_DESC have;
    g_halfSnap->GetDesc(&have);
    if ((int)have.Width != rt->nw || (int)have.Height != rt->nh)
        return;                             /* resized meanwhile (target_gs_resolution) */
    if (!g_halfBlend) {
        D3D11_BLEND_DESC bd = {};
        D3D11_RENDER_TARGET_BLEND_DESC &t = bd.RenderTarget[0];
        t.BlendEnable = TRUE;
        t.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        t.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
        t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ZERO;
        t.DestBlendAlpha = D3D11_BLEND_ONE;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                  D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (FAILED(dev()->CreateBlendState(&bd, g_halfBlend.put())))
            return;
    }
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {0, 0, (float)rt->nw, (float)rt->nh, 0, 1};
    const float half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    queue_pass_state(rt->rtv.get(), vp, raster_state(0, false), g_halfBlend.get(), half,
                     depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::Present);
    CbPresent cb = {0, 0, 1, 1};
    bind_ps_cb(&cb, sizeof(cb));
    queue_pass_draw(g_halfSnapSrv.get(), nullptr, sampler_state(false, false, 1, 1));
    d3d_invalidate();
    rt->gen++;
    rt->execAlpha = -1;                     /* PC (2026-11-06): (its alpha no longer known flat) */
    registry_target_written(*rt, true, false);
    if (g_tracing)
        pclog::write("pgl trace %u: the draws since the begin at half strength in 0x%X (%dx%d)", g_traceN++,
                     rt->base, rt->gsW, rt->gsH);
}

/* FXAA over a race view's 3D picture (pglr_post_aa; docs/renderer.md
 * "Anti-aliasing"), when it is the anti-aliasing chosen: the w x h buffer at
 * c.base filtered in place from a copy of it. */
void exec_post_aa(const CmdPostAa &c)
{
    if (settings::anti_aliasing() != settings::AntiAliasing::fxaa)
        return;
    flush_batch();
    Target *t = find_color_target_sized(c.base, c.w, c.h);
    if (t == nullptr || t->gsRes || !t->rtv)
        return;
    ID3D11ShaderResourceView *src = target_copy_srv(*t, true);    /* (all of it: target_copy_srv) */
    if (src == nullptr)
        return;
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {0, 0, (float)t->nw, (float)t->nh, 0, 1};
    queue_pass_state(t->rtv.get(), vp, raster_state(0, false), blend_plain(gs::WRITE_RGB | gs::WRITE_A), nullptr,
                     depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::Fxaa);
    const CbPresent cb = {1.0f / (float)t->nw, 1.0f / (float)t->nh, 0, 0};
    bind_ps_cb(&cb, sizeof(cb));
    queue_pass_draw(src, nullptr, sampler_state(true, true, 1, 1));
    d3d_invalidate();
    t->gen++;
    t->execAlpha = -1;                      /* PC (2026-11-06): (its alpha no longer known flat) */
    registry_target_written(*t, true, false);
    if (g_tracing)
        pclog::write("pgl trace %u: FXAA over the %dx%d picture at 0x%X", g_traceN++, c.w, c.h, c.base);
}

}  /* namespace */

/* ======================================================================== */
/* The execution's CPU profile (pgl_render.h)                               */
/* ======================================================================== */

bool g_profOn;
thread_local uint32_t *g_profCount = g_profA.count;     /* PC (2026-11-05): the calling stage's (ProfState) */

void prof_push(ProfCat c)
{
    ProfState &s = *t_prof;
    if (s.depth < kProfDepth)
        s.stack[s.depth] = ProfOpen{(uint8_t)c, __rdtsc(), 0};
    s.depth++;
}

void prof_pop()
{
    ProfState &s = *t_prof;
    if (--s.depth >= kProfDepth || s.depth < 0) {
        if (s.depth < 0)
            s.depth = 0;
        return;
    }
    const ProfOpen &o = s.stack[s.depth];
    const uint64_t elapsed = __rdtsc() - o.start;
    s.ticks[o.cat] += elapsed > o.child ? elapsed - o.child : 0;
    if (s.depth > 0 && s.depth - 1 < kProfDepth)
        s.stack[s.depth - 1].child += elapsed;
}

namespace {
/* PC (2026-11-05): the submit thread as it starts (sq::g_threadInit): its
 * own profile, counts and stats */
void sub_thread_init()
{
    t_prof = &g_profB;
    g_profCount = g_profB.count;
    t_stats = &g_statsB;
    cap::t_thread = 1;                      /* PC (2026-11-06): (the frame capture's thread column) */
    d3d_thread_init();                      /* PC: (its rings: pgl_d3d.h Rings) */
}
}  /* namespace */

/* ======================================================================== */
/* The list                                                                 */
/* ======================================================================== */

void exec_init()
{
    {
        const char *e = getenv("GT3PC_PGL_CLIP_ALL");
        g_noDirect = e != nullptr && atoi(e) > 0;
        if (g_noDirect)
            pclog::write("pgl: GT3PC_PGL_CLIP_ALL=1: every VU1 draw through the clipper (gs_vu1)");
        e = getenv("GT3PC_PGL_MERGE");
        g_mergeOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_mergeOn)
            pclog::write("pgl: GT3PC_PGL_MERGE=0: each draw its own call (no draw extended by the next)");
        e = getenv("GT3PC_PGL_TEXSTATE");
        g_texStateOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_texStateOn)
            pclog::write("pgl: GT3PC_PGL_TEXSTATE=0: the whole draw state worked out after texture registers too");
        e = getenv("GT3PC_PGL_AD_INST");
        g_adInstOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_adInstOn)
            pclog::write("pgl: GT3PC_PGL_AD_INST=0: a C = Ad form's two passes as two draws, never one of two instances");
        e = getenv("GT3PC_PGL_DATE_PAIR");
        g_datePairOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_datePairOn)
            pclog::write("pgl: GT3PC_PGL_DATE_PAIR=0: a C = Ad draw that flips the alpha mirror's bit as two draws (keep,"
                         " invert), not one of two instances counting the stencil");
        e = getenv("GT3PC_PGL_AD_ONCE");
        g_adOnce = e != nullptr && atoi(e) > 0;
        if (g_adOnce)
            pclog::write("pgl: GT3PC_PGL_AD_ONCE=1: a C = Ad draw of two instances drawn once (a measurement: half the"
                         " reflections)");
        e = getenv("GT3PC_PGL_MIRROR_LAZY");
        g_mirrorLazyOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_mirrorLazyOn)
            pclog::write("pgl: GT3PC_PGL_MIRROR_LAZY=0: the alpha mirror kept through every draw while valid (no pass"
                         " dropped, no stencil-clear sync, not made valid again by a flat write)");
        e = getenv("GT3PC_PGL_MIRROR_DROP_PX");
        g_mirrorDropPx = e != nullptr ? (uint32_t)std::max(0, atoi(e)) : 0u;
        if (g_mirrorDropPx != 0)
            pclog::write("pgl: GT3PC_PGL_MIRROR_DROP_PX=%u: the alpha mirror let go at a mirror pass of that many pixels"
                         " or more (a test: the pictures must be the same)", g_mirrorDropPx);
        e = getenv("GT3PC_PGL_AD_DOUBLE");
        g_adDoubleOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_adDoubleOn)
            pclog::write("pgl: GT3PC_PGL_AD_DOUBLE=0: no doubled index stream - a C = Ad draw as two instances, never"
                         " joined with the next part's");
        e = getenv("GT3PC_PGL_FAST_CLEAR");
        g_fastClearOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_fastClearOn)
            pclog::write("pgl: GT3PC_PGL_FAST_CLEAR=0: the covering flat sprites drawn, not done as clears");
        e = getenv("GT3PC_PGL_MIRROR_SYNC_COPY");
        g_mirrorSyncCopy = e != nullptr && atoi(e) > 0;
        if (g_mirrorSyncCopy)
            pclog::write("pgl: GT3PC_PGL_MIRROR_SYNC_COPY=1: the alpha mirror synced from a copy of the target (a"
                         " measurement)");
        e = getenv("GT3PC_PGL_MIRROR_PASS");
        g_mirrorPassOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_mirrorPassOn)
            pclog::write("pgl: GT3PC_PGL_MIRROR_PASS=0: the alpha mirror's passes not drawn (a measurement: DATE wrong"
                         " where a draw's alpha bit varies)");
    }
    memset(g_addrReg, -1, sizeof(g_addrReg));
    memset(g_addrCtx, 0, sizeof(g_addrCtx));
    memset(g_addrGen, -1, sizeof(g_addrGen));
    for (int i = 0; i < gs::REG_COUNT; i++) {
        g_addrReg[kCtxAddr[i]] = (int8_t)i;
        g_addrCtx[kCtxAddr[i]] = 0;
        g_addrReg[kCtxAddr[i] + 1] = (int8_t)i;
        g_addrCtx[kCtxAddr[i] + 1] = 1;
    }
    for (int i = 0; i < gs::GEN_COUNT; i++)
        g_addrGen[kGenAddr[i]] = (int8_t)i;
    g.gen[gs::GEN_COLCLAMP] = 1;
    g.gen[gs::GEN_PRMODECONT] = 1;
    d3d_init();
    {
        /* PC: the DATE draws' destination alpha (DateMode) */
        const char *e = getenv("GT3PC_PGL_DATE_COPY");
        if (e != nullptr && strcmp(e, "0") == 0) {
            g_dateMode = DATE_NONE;
            pclog::write("pgl: GT3PC_PGL_DATE_COPY=0: DATE draws read their target's old copy (a measurement: the"
                         " picture is wrong after alpha writes)");
        } else if (e != nullptr && strcmp(e, "2") == 0) {
            g_dateMode = DATE_EACH;
            pclog::write("pgl: GT3PC_PGL_DATE_COPY=2: a copy of the target before every DATE draw (a measurement:"
                         " the GS's own semantics)");
        }
        /* PC (2026-11-04): the alpha mirror (g_dateStencil) - not with the
         * copy modes above (they are the measurements against it) */
        const char *s = getenv("GT3PC_PGL_DATE_STENCIL");
        g_dateStencil = g_dateMode == DATE_COPY && !(s != nullptr && strcmp(s, "0") == 0);
        pclog::write(g_dateStencil ? "pgl: DATE draws test the destination alpha's mirror in the stencil (no copies)"
                                   : "pgl: DATE draws read a copy of their target (GT3PC_PGL_DATE_STENCIL=0)");
        /* PC (2026-11-05): the resolved runs (GT3PC_PGL_RUNS=0: off) */
        const char *rr = getenv("GT3PC_PGL_RUNS");
        g_runsOn = !(rr != nullptr && strcmp(rr, "0") == 0);
        /* PC (2026-11-05): their textures from their records while the sets
         * stand (GT3PC_PGL_RUN_TEX=0: resolved at every replay as before) */
        const char *rt = getenv("GT3PC_PGL_RUN_TEX");
        g_runTexOn = !(rt != nullptr && strcmp(rt, "0") == 0);
        /* PC (2026-11-05): a replayed run's draws as one record for the submit
         * stage (RunWork; GT3PC_PGL_RUN_WORK=0: a record per draw as before) */
        const char *rw = getenv("GT3PC_PGL_RUN_WORK");
        g_runWorkOn = !(rw != nullptr && strcmp(rw, "0") == 0);
        if (!g_runWorkOn)
            pclog::write("pgl: a replayed run's draws go to the submit stage one by one (GT3PC_PGL_RUN_WORK=0)");
        /* PC (2026-11-06, step 3): the stage walks a replayed run's draws
         * itself (GT3PC_PGL_RUN_WALK=0: the executor walks them and notes
         * each, as step 1) */
        const char *rk = getenv("GT3PC_PGL_RUN_WALK");
        g_runWalkOn = !(rk != nullptr && strcmp(rk, "0") == 0);
        if (!g_runWalkOn)
            pclog::write("pgl: the executor walks a replayed run's draws itself (GT3PC_PGL_RUN_WALK=0)");
        /* PC (2026-11-06, step 4): a replay's registers from the record
         * (GT3PC_PGL_RUN_REGS=0: the run's commands walked for them as before) */
        const char *rg = getenv("GT3PC_PGL_RUN_REGS");
        g_runRegsOn = !(rg != nullptr && strcmp(rg, "0") == 0);
        if (!g_runRegsOn)
            pclog::write("pgl: a replay walks the run's commands for its registers (GT3PC_PGL_RUN_REGS=0)");
        /* PC (2026-11-08): GT3PC_PGL_RUN_DAMAGE=1 (run_try: run_record_sound's test) */
        const char *dm = getenv("GT3PC_PGL_RUN_DAMAGE");
        g_runDamageTest = dm != nullptr ? std::max(0, std::min(2, atoi(dm))) : 0;
        const char *ck = getenv("GT3PC_PGL_RUN_CHECK");
        g_runCheckOn = !(ck != nullptr && strcmp(ck, "0") == 0);
        if (!g_runCheckOn)
            pclog::write("pgl: GT3PC_PGL_RUN_CHECK=0: the runs' records replayed unchecked (run_record_sound's cost"
                         " compared)");
        pclog::write(g_runsOn && g_dateStencil
                         ? "pgl: runs of model parts resolved once and replayed (their matrices and lighting from"
                           " each frame, their textures %s)"
                         : "pgl: every model part resolved at every frame (%s)",
                     !(g_runsOn && g_dateStencil) ? (g_runsOn ? "no alpha mirror" : "GT3PC_PGL_RUNS=0")
                     : g_runTexOn ? "from their records while the sets stand"
                                  : "resolved at every replay (GT3PC_PGL_RUN_TEX=0)");
    }
    targets_init();
    q::start();                             /* PC: the D3D thread (pgl_queue.h) */
    /* PC (2026-11-05): the submit thread (pgl_queue.h sq; the records above,
     * SubState / SubVu1 / SubGs); the debug aids read their switches now,
     * before two threads could ask at once */
    skip_on();
    flicker_on();
    probe_on();
    sq::g_threadInit = &sub_thread_init;
    sq::start();
    workers_start();                        /* PC (2026-11-05): the submit workers (step 2) */
    {
        /* PC (2026-11-05): GT3PC_PGL_ALPHA_LOG=1 (alpha_log_check) */
        const char *e = getenv("GT3PC_PGL_ALPHA_LOG");
        g_alphaLog = e != nullptr && atoi(e) > 0;
        if (g_alphaLog)
            pclog::write("pgl: GT3PC_PGL_ALPHA_LOG=1: draws that may write a GS alpha above 0x80 into a target are"
                         " counted (ALPHA>80) and logged (pgl alpha: ...)");
        /* PC (2026-11-06): GT3PC_PGL_BOUND_LOG=1 (target_alpha_written, exec_end_frame) */
        const char *bl = getenv("GT3PC_PGL_BOUND_LOG");
        g_boundLog = bl != nullptr && atoi(bl) > 0;
        if (g_boundLog)
            pclog::write("pgl: GT3PC_PGL_BOUND_LOG=1: the alpha writes above 0x80 and the C = Ad draws by their"
                         " target's alpha bound are counted every 100 frames, the targets' bounds logged (pgl bound: ...)");
    }
    {
        /* PC (2026-11-05): the flare queries' batch (flare_queries_flush) */
        const char *e = getenv("GT3PC_PGL_FLARE_BATCH");
        g_flareBatchOn = !(e != nullptr && strcmp(e, "0") == 0);
        if (!g_flareBatchOn)
            pclog::write("pgl: GT3PC_PGL_FLARE_BATCH=0: every flare query drawn as a pass of its own (a comparison:"
                         " the batch gives the same texels)");
    }
    {
        /* PC (2026-11-05): the mirror passes folded into their draws (mirror_fold_ok) */
        const char *e = getenv("GT3PC_PGL_MIRROR_FOLD");
        g_mirrorFoldOn = e != nullptr && atoi(e) > 0;
        if (g_mirrorFoldOn)
            pclog::write("pgl: GT3PC_PGL_MIRROR_FOLD=1: the alpha mirror's passes folded into their draws as one more"
                         " instance (the pictures the same; slower on a GTX 1660 SUPER: the early depth test lost)");
        const char *l = getenv("GT3PC_PGL_MIRROR_LOG");
        g_mirrorLog = l != nullptr && atoi(l) > 0;
        if (g_mirrorLog)
            pclog::write("pgl: GT3PC_PGL_MIRROR_LOG=1: the first 40 draws that need an alpha mirror pass are logged"
                         " with their alpha ranges (pgl mirror: ...)");
        /* PC (2026-11-05): the targets' alpha bounds (target_alpha_written) */
        const char *ab = getenv("GT3PC_PGL_ALPHA_BOUND");
        g_alphaBoundOn = !(ab != nullptr && strcmp(ab, "0") == 0);
        if (!g_alphaBoundOn)
            pclog::write("pgl: GT3PC_PGL_ALPHA_BOUND=0: the targets' alpha not bounded - a draw textured from a"
                         " target may write any alpha, as before (a comparison: the pictures must be the same)");
    }
}

void exec_list(const CmdList &list)
{
    movie_frame();                          /* PC: a movie's textures opened or let go (pgl_movie.cpp) */
    /* PC (2026-11-05): the resolved runs - a run found from command i is
     * replayed, or recorded while its commands execute below (runEnd: the
     * commands up to it belong to a run looked at: not scanned again) */
    size_t runEnd = 0;
    const bool runs = run_cache_on() && g_subDepth == 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (runs && i >= runEnd && !g_runRec) {
            uint32_t nParts = 0;
            uint64_t key = 0;
            const size_t j = run_scan_key(list, i, nParts, key);
            if (j > i) {
                runEnd = j;
                if (nParts >= kRunMinParts) {
                    const int64_t capT0 = cap::on() ? cap_now() : 0;    /* PC (2026-11-06): (the frame capture) */
                    if (run_try(list, i, j, key)) {
                        if (capT0 != 0)
                            cap::cmd((uint32_t)i, 255, cap_ns(capT0), g_capLastRun);
                        i = j - 1;          /* (replayed) */
                        continue;
                    }
                }
            }
        }
        const int64_t capT0 = cap::on() ? cap_now() : 0;    /* PC (2026-11-06): (the frame capture: this command's time) */
        if (g_runRec)
            g_runCmd = (uint32_t)(i - g_runStart);
        const void *p = list.payload(i);
        t_stats->cmds[list.type(i) & 31]++;
        const ProfCat cat = prof_cat(list.type(i));
        ProfScope prof(cat);                        /* (the verbose monitor's profile) */
        if (cat == PROF_GS || cat == PROF_TARGET_SET)
            g_profCount[PCOUNT_GSCMDS]++;
        else if (cat == PROF_UPLOAD)
            g_profCount[PCOUNT_UPLOADS]++;
        if (flare_queries_pending())
            flare_queries_check(list.type(i));  /* PC (2026-11-05): the queries' batch before what they must not wait through */
        switch (list.type(i)) {
        case CMD_REGS:
            exec_regs(*static_cast<const CmdRegs *>(p));
            break;
        case CMD_AD: {
            const CmdAd &c = *static_cast<const CmdAd *>(p);
            write_reg(c.addr, c.value);
            break;
        }
        case CMD_TARGET: {
            const CmdTarget &c = *static_cast<const CmdTarget *>(p);
            if (c.w < 0) {                  /* PC (2026-11-13): the game freed the buffer there (pglr_buffer_freed) */
                target_freed(c.base);
                break;
            }
            if (c.dx > 0.0f) {              /* PC (2026-11-14): a buffer's detail (pglr_target_detail) */
                flush_batch();
                target_detail(c.base, c.w, c.h, c.dx, c.dy);
                g_dirty = true;
                break;
            }
            flush_batch();
            target_size(c.base, c.fbw, c.psm, c.w, c.h);
            g_dirty = true;
            break;
        }
        case CMD_PRIM:
            exec_prim(*static_cast<const CmdPrim *>(p));
            break;
        case CMD_GIF:
            exec_gif(*static_cast<const CmdGif *>(p));
            break;
        case CMD_STRIP:
            exec_strip(*static_cast<const CmdStrip *>(p));
            break;
        case CMD_TRI16:
            exec_tri16(*static_cast<const CmdTri16 *>(p));
            break;
        case CMD_MODEL:
            exec_model(*static_cast<const CmdModel *>(p));
            break;
        case CMD_UPLOAD: {
            const CmdUpload &c = *static_cast<const CmdUpload *>(p);
            flush_batch();
            registry_upload(c.dbp, c.dbw, c.dpsm, c.dx, c.dy, c.w, c.h, c.data,
                            gstex::image_bytes(c.dpsm, (uint32_t)c.w, (uint32_t)c.h));
            g_dirty = true;
            break;
        }
        case CMD_TEX1_UPLOAD: {
            const CmdTex1 &c = *static_cast<const CmdTex1 *>(p);
            flush_batch();
            registry_tex1_upload(c.img, c.vramBase, c.copyBytes != 0 ? &c + 1 : nullptr);
            g_dirty = true;
            break;
        }
        case CMD_TEX_SOURCE: {
            /* PC: a VRAM-less set's texture (ROADMAP 18; the binding found again) */
            const CmdTexSource &c = *static_cast<const CmdTexSource *>(p);
            flush_batch();
            registry_texture_source(c.img, c.vramBase, c.tex0, c.copyBytes != 0 ? &c + 1 : nullptr);
            g_dirty = true;
            break;
        }
        case CMD_READBACK:
            flush_batch();
            readback(*static_cast<const CmdReadback *>(p));
            g_dirty = true;
            break;
        case CMD_SUBCALL: {
            const CmdList *s = sub_list(static_cast<const CmdSubcall *>(p)->key);
            if (s != nullptr && g_subDepth < 4) {
                g_subDepth++;
                exec_list(*s);
                g_subDepth--;
            }
            break;
        }
        case CMD_PFX_COLOR:
            exec_pfx_color(*static_cast<const CmdPfxColor *>(p));
            g_dirty = true;
            break;
        case CMD_PFX_SPLIT:
            exec_pfx_split(*static_cast<const CmdPfxSplit *>(p));
            g_dirty = true;
            break;
        case CMD_FLARE_QUERY:
            exec_flare_query(*static_cast<const PglrFlareQuery *>(p));
            g_dirty = true;
            break;
        case CMD_HALF_BLEND:
            exec_half_blend(*static_cast<const CmdHalfBlend *>(p));
            g_dirty = true;
            break;
        case CMD_DESIGN: {
            const CmdDesign &c = *static_cast<const CmdDesign *>(p);
            g_designX = c.kx;
            g_designY = c.ky;
            break;
        }
        case CMD_LINE_WIDTH: {
            const float px = static_cast<const CmdLineWidth *>(p)->px;
            if (px != g_lineWidth) {
                flush_batch();          /* (the lines so far at their width; the next state's topology) */
                g_lineWidth = px;
                g_dirty = true;
            }
            break;
        }
        case CMD_POST_AA: {
            const int prevSec = sec_active() ? sec_want(PGLR_SEC_POST_AA) : -1;       /* PC: (the GPU sections) */
            exec_post_aa(*static_cast<const CmdPostAa *>(p));
            if (prevSec >= 0)
                sec_want(prevSec);
            g_dirty = true;
            break;
        }
        case CMD_GPU_SECTION:
            if (sec_active()) {
                flush_batch();
                g_secGame = static_cast<const CmdSection *>(p)->id;
                if (sec_active())
                    sec_want(g_secGame);
            }
            break;
        }
        if (capT0 != 0)
            cap::cmd((uint32_t)i, list.type(i), cap_ns(capT0), g_runRec ? -2 : -1);    /* (-2: being recorded) */
        if (g_runRec) {
            run_rec_op(list.type(i), p);    /* PC (step 4): (the commands a replay visits) */
            if (i + 1 >= g_runEnd)
                run_rec_finish();           /* PC: (the run recorded whole) */
        }
    }
    if (g_runRec)
        run_rec_finish();
}

/* ======================================================================== */
/* GPU time by section (PC: GT3PC_GPU_SECTIONS=N)                           */
/* ======================================================================== */

namespace {

/* A frame's timestamps at its section marks (pglr_gpu_section), read back
 * kSecFrames frames later without waiting (a frame not ready by then is
 * dropped); the means of every N frames logged. */
/* PC: (the cars and the target copies mark within the scene; the last place
 * is the frame's end mark's, so that the sections after the limit are not
 * lost - they count to the one marked last, and the limit is logged) */
constexpr int kSecMarks = 4096, kSecFrames = 4;
struct SecFrame {
    Com<ID3D11Query> disjoint;
    Com<ID3D11Query> ts[kSecMarks];
    int id[kSecMarks] = {};
    int lastId = -1;                    /* PC: the section marked last (sec_want) */
    int n = 0;
    bool pending = false;
    uint32_t seq = 0;                   /* the frame monitor's frame (frame_monitor::gpu_sections) */
};
SecFrame g_sec[kSecFrames];
int g_secCur;
int g_secEvery = -1;                    /* -1 not read yet, 0 off */
bool g_secActive;                       /* this frame is timed (sec_begin_frame .. sec_end_frame) */
double g_secSum[PGLR_SEC_COUNT];
uint32_t g_secFrames;
const char *const kSecNames[PGLR_SEC_COUNT] = {"other", "scene", "effects", "copy", "replay blur", "weather",
                                               "smoke", "flare", "post", "cars", "target copies", "flare queries",
                                               "fxaa"};
int g_secGame = PGLR_SEC_OTHER;

/* Timed: GT3PC_GPU_SECTIONS (logged), or the frame monitor's verbose view
 * (shown there: platform/frame_monitor.h) */
bool sec_on()
{
    if (g_secEvery < 0) {
        const char *e = getenv("GT3PC_GPU_SECTIONS");
        g_secEvery = (e != nullptr && atoi(e) > 0) ? atoi(e) : 0;
        if (g_secEvery > 0)
            pclog::write("pgl: GPU time by section, logged every %d frames (GT3PC_GPU_SECTIONS)", g_secEvery);
        frame_monitor::gpu_section_names(kSecNames, PGLR_SEC_COUNT);
    }
    return g_secEvery > 0 || frame_monitor::mode() == frame_monitor::MODE_VERBOSE;
}

bool sec_active()
{
    return g_secActive;
}

void sec_collect(SecFrame &f)
{
    if (!f.pending)
        return;
    /* (PC: not ready yet - the GPU behind by the ring's frames - stays
     * pending: the frame that would reuse the slot is not timed, instead
     * of this one being lost; a GPU-bound run timed no frame at all) */
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    const HRESULT hd = dc()->GetData(f.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hd == S_FALSE)
        return;
    f.pending = false;
    if (hd != S_OK || dj.Disjoint || dj.Frequency == 0)
        return;
    UINT64 t[kSecMarks];
    for (int i = 0; i < f.n; i++) {
        const HRESULT h = dc()->GetData(f.ts[i].get(), &t[i], sizeof(t[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (h == S_FALSE) {
            f.pending = true;
            return;
        }
        if (h != S_OK)
            return;
    }
    float frameMs[PGLR_SEC_COUNT] = {};
    for (int i = 0; i + 1 < f.n; i++)
        if (f.id[i] >= 0 && f.id[i] < PGLR_SEC_COUNT)
            frameMs[f.id[i]] += (float)((double)(t[i + 1] - t[i]) * 1000.0 / (double)dj.Frequency);
    frame_monitor::gpu_sections(f.seq, frameMs, PGLR_SEC_COUNT);
    if (g_secEvery <= 0)
        return;
    for (int s = 0; s < PGLR_SEC_COUNT; s++)
        g_secSum[s] += frameMs[s];
    if (++g_secFrames < (uint32_t)g_secEvery)
        return;
    char line[512];
    int at = snprintf(line, sizeof(line), "pgl gpu sections (ms a frame, mean of %u):", g_secFrames);
    double total = 0.0;
    for (int s = 0; s < PGLR_SEC_COUNT; s++) {
        const double ms = g_secSum[s] / g_secFrames;
        total += ms;
        if (ms >= 0.005 && at < (int)sizeof(line))
            at += snprintf(line + at, sizeof(line) - at, " %s %.2f,", kSecNames[s], ms);
    }
    if (at < (int)sizeof(line))
        snprintf(line + at, sizeof(line) - at, " total %.2f", total);
    pclog::write("%s", line);
    memset(g_secSum, 0, sizeof(g_secSum));
    g_secFrames = 0;
}

void sec_mark(int id)
{
    SecFrame &f = g_sec[g_secCur];
    if (f.n >= (id == PGLR_SEC_COUNT ? kSecMarks : kSecMarks - 1)) {
        log_once("pgl-sec-marks", "pgl: GPU sections: more than %d marks in a frame - the draws after the last one"
                 " count to its section", kSecMarks - 1);
        return;
    }
    if (!f.ts[f.n]) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(dev()->CreateQuery(&d, f.ts[f.n].put())))
            return;
    }
    ID3D11Query *ts = f.ts[f.n].get();
    q::run([ts] { qdc()->End(ts); });
    f.id[f.n] = id;
    f.lastId = id;
    f.n++;
}

int sec_want(int id)
{
    if (!g_secActive)
        return -1;
    SecFrame &f = g_sec[g_secCur];
    const int prev = f.lastId;
    if (id != prev)
        sec_mark(id);
    return prev;
}

int sec_last()
{
    return g_secActive ? g_sec[g_secCur].lastId : -1;
}

void sec_begin_frame()
{
    g_secActive = false;
    if (!sec_on())
        return;
    SecFrame &f = g_sec[g_secCur];
    sec_collect(f);
    if (f.pending)
        return;                             /* PC: (its results still to come: this frame goes untimed) */
    f.n = 0;
    f.lastId = -1;
    g_secGame = PGLR_SEC_OTHER;
    if (!f.disjoint) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(dev()->CreateQuery(&d, f.disjoint.put())))
            return;
    }
    ID3D11Query *dj = f.disjoint.get();
    q::run([dj] { qdc()->Begin(dj); });
    f.seq = frame_monitor::frame_seq();
    g_secActive = true;
    sec_mark(PGLR_SEC_OTHER);
}

void sec_end_frame()
{
    /* (as the frame began: the verbose view may have changed meanwhile) */
    if (!g_secActive)
        return;
    g_secActive = false;
    SecFrame &f = g_sec[g_secCur];
    sec_mark(PGLR_SEC_COUNT);           /* (the end) */
    ID3D11Query *dj = f.disjoint.get();
    q::run([dj] { qdc()->End(dj); });
    f.pending = true;
    g_secCur = (g_secCur + 1) % kSecFrames;
    g_secGame = PGLR_SEC_OTHER;
}

}  /* namespace */

int exec_section_want(int id)
{
    return sec_want(id);
}

double g_execSeconds;

void exec_begin_frame()
{
    d3d_thread_init();                      /* PC (2026-11-05): the executing thread's rings (once; a frame it submits itself) */
    {
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        g_execSeconds = (double)c.QuadPart / (double)f.QuadPart;
    }
    /* the CPU profile: in the frames of the verbose monitor (PC: and in
     * every frame for GT3PC_PGL_PROFILE's log) */
    g_profOn = frame_monitor::mode() == frame_monitor::MODE_VERBOSE || prof_log_every() > 0;
    for (ProfState *s : {&g_profA, &g_profB}) {     /* PC: (both stages'; the submit thread is idle here) */
        s->depth = 0;
        memset(s->ticks, 0, sizeof(s->ticks));
        memset(s->count, 0, sizeof(s->count));
    }
    for (auto &wk : g_workers) {            /* PC (step 2): (the workers': idle here too) */
        wk->prof.depth = 0;
        memset(wk->prof.ticks, 0, sizeof(wk->prof.ticks));
        memset(wk->prof.count, 0, sizeof(wk->prof.count));
    }
    if (g_profOn) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_profQpc0 = q.QuadPart;
        g_profTsc0 = __rdtsc();
    }
    sec_begin_frame();
    d3d_invalidate();
    /* PC (2026-11-05, a measurement): GT3PC_PGL_MIRROR_RESYNC=1 - the alpha
     * mirrors invalid at every frame, as the game's double-buffered frames
     * leave them: each frame's first DATE draw syncs (the benchmark's
     * pictures must stay the exact ones) */
    static int resync = -1;
    if (resync < 0) {
        const char *e = getenv("GT3PC_PGL_MIRROR_RESYNC");
        resync = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (resync)
            pclog::write("pgl: GT3PC_PGL_MIRROR_RESYNC=1: the alpha mirrors synced again at every frame (a measurement)");
    }
    if (resync)
        targets_mirror_invalidate_all();
    /* PC (2026-11-10): [Graphics] AnisotropicFiltering for the frame (aniso_ok;
     * GT3PC_PGL_ANISO=N overrides it: the benchmark's, a measurement) */
    {
        static int envAniso = -1;
        if (envAniso < 0) {
            const char *e = getenv("GT3PC_PGL_ANISO");
            envAniso = e != nullptr ? std::max(1, std::min(16, atoi(e))) : 0;
            if (envAniso > 0)
                pclog::write("pgl: GT3PC_PGL_ANISO=%d: the anisotropic filtering's most, whatever the settings", envAniso);
            const char *z = getenv("GT3PC_PGL_ANISO_ZERO");
            g_anisoZero = z != nullptr ? std::max(0, std::min(2, atoi(z))) : 0;
            if (g_anisoZero == 1)
                pclog::write("pgl: GT3PC_PGL_ANISO_ZERO=1: the anisotropic draws take their centre's colour alone (a"
                             " check: the pictures must equal those without the filtering)");
            else if (g_anisoZero == 2)
                pclog::write("pgl: GT3PC_PGL_ANISO_ZERO=2: the anisotropic taps unweighted by their alpha (a"
                             " comparison: a hardware filter's way, the hidden texels' colours shown)");
        }
        const int a = envAniso > 0 ? envAniso : settings::anisotropic_filtering();
        if (a != g_aniso) {
            g_aniso = a;
            pclog::write("pgl: anisotropic filtering %s%s", a > 1 ? std::to_string(a).c_str() : "off",
                         a > 1 ? "x (3D draws' bilinear textures: up to that many taps along the footprint, each"
                                 " weighted by its alpha)" : "");
        }
    }
    g_dirty = true;
    g_bs.valid = false;
    g_designX = g_designY = 1.0f;
    g_lineWidth = 1.0f;
    g_flareStrip = nullptr;
    if (g_traceOn < 0) {
        /* PC: always armed (it costs nothing until pressed), so a glitch
         * that comes at random can be traced when it shows (user,
         * 2026-10-25); GT3PC_PGL_TRACE=0 turns it off */
        const char *e = getenv("GT3PC_PGL_TRACE");
        g_traceOn = (e == nullptr || atoi(e) > 0) ? 1 : 0;
        if (g_traceOn == 1)
            pclog::write("pgl: the Trace hotkey (F9) logs the draws of the next frame");
    }
    if (g_traceOn == 1) {
        /* F9: the Trace hotkey (gt3pc.ini [Hotkeys]) */
        if (window::take_hotkey(input_map::kHotTrace)) {
            g_tracing = true;
            g_traceN = 0;
            g_traceFrames = 2;
            g_flareProbeN = 0;
            g_probe.last.clear();
            pclog::write("pgl trace: frame %llu begins", (unsigned long long)frame_number());
            probe_on();
        } else if (g_traceNext) {
            /* the second traced frame: unless GT3PC_PGL_PROBE set points,
             * the centres of the flare queries the shape passed in */
            g_traceNext = false;
            g_tracing = true;
            g_traceN = 0;
            g_probe.last.clear();
            pclog::write("pgl trace: frame %llu begins (the second traced frame)", (unsigned long long)frame_number());
            if (!probe_on() && g_flareProbeN > 0) {
                g_probe.n = g_flareProbeN;
                for (int i = 0; i < g_flareProbeN; i++) {
                    g_probe.x[i] = g_flareProbeX[i];
                    g_probe.y[i] = g_flareProbeY[i];
                    pclog::write("pgl trace: probing (%d,%d), the centre of a flare query %u pixels of whose shape"
                                 " passed", g_flareProbeX[i], g_flareProbeY[i], g_flareProbePassed[i]);
                }
                g_probe.state = 1;
                g_probeFromFlares = true;
            }
        }
    }
    /* PC (2026-11-06): the frame capture (pgl_capture.h): F11, or the frame
     * GT3PC_PGL_CAPTURE names - the frame recorded in full, as it is rendered
     * (nothing below changes for it) */
    if (window::take_hotkey(input_map::kHotFrameCapture))
        cap::arm();
    cap::begin_frame(frame_number());
    /* PC (2026-11-05): the submit thread takes this frame's draws (pgl_queue.h
     * sq): not with the debug aids that look at each draw as it is submitted
     * (the F9 trace, GT3PC_FLICKER, GT3PC_PGL_SKIP); the GPU sections' marks
     * are the executor's (submit_prologue) and go along.  The executor's
     * calls go through the thread from here on (q::t_forward, until
     * exec_end_frame drains it) */
    sq::g_on = sq::running() && q::running() && !g_tracing && !flicker_on() && !skip_on();
    q::t_forward = sq::g_on;
    if (sq::g_on)
        d3d_invalidate();                   /* PC: (the submit thread's own binding cache: forgotten with the frame) */
    /* PC (2026-11-05): the runs' works of the frame before are read (it
     * drained at its end): the pools start over */
    g_workRuns.reset();
    g_workStates.reset();
    g_workSteps.reset();
    g_workNotes.reset();
    g_workTexs.reset();
    g_work = nullptr;
}

void exec_end_frame()
{
    flush_batch();
    flare_queries_flush();                  /* PC (2026-11-05): the queries' batch, if a frame left it (flare_queries_flush) */
    sq::run([] { q::release_held(); });     /* PC: (the frame's last draw, if held: draw_passes - the submit stage's) */
    sec_end_frame();
    g_bs.valid = false;
    g_dirty = true;
    g_subPosted = false;
    g_dateRun = DateRun();      /* (targets may go at next_frame) */
    g_halfRt = nullptr;         /* (a half-strength run left open) */
    if (g_tracing) {
        pclog::write("pgl trace: frame %llu ends (%u lines)", (unsigned long long)frame_number(), g_traceN);
        if (g_traceFrames == 2) {
            trace_dump_next(frame_number());    /* (its picture at its present) */
            registry_trace_owners();            /* PC: (who owns VRAM after it) */
        }
        g_tracing = false;
        if (--g_traceFrames > 0) {
            g_traceNext = true;
        } else if (g_probeFromFlares) {
            g_probe.n = 0;
            g_probe.state = 0;
            g_probeFromFlares = false;
        }
    }
    q::run([] {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(0, nullptr, nullptr);
    });
    /* PC (2026-11-05): the submit thread done with the frame - its draws
     * queued for the D3D thread, its counts complete - before anything below
     * reads them or lets a target or texture go; the executor's calls go to
     * the D3D thread's ring themselves again (the submit thread idle until
     * the next frame: one producer at a time) */
    double subMs = 0.0, subWaitMs = 0.0;
    uint32_t subWaits = 0;
    const bool piped = sq::g_on;
    sq::sync();
    q::t_forward = false;
    sq::g_on = false;
    sq::take_stats(&subMs, &subWaitMs, &subWaits);
    g_runGrave.clear();                     /* PC: (the runs replaced this frame: no work of theirs pending now) */
    if (g_boundLog) {
        /* PC (2026-11-06, a measurement: GT3PC_PGL_BOUND_LOG) */
        static uint32_t frames = 0, hiW = 0, d128 = 0, dWide = 0, p128 = 0, pWide = 0;
        hiW += g_boundHiWrites.exchange(0);
        d128 += g_boundAdDate128.exchange(0);
        dWide += g_boundAdDateWide.exchange(0);
        p128 += g_boundAdPlain128.exchange(0);
        pWide += g_boundAdPlainWide.exchange(0);
        if (++frames == 100) {
            pclog::write("pgl bound: over %u frames, a frame: alpha writes above 0x80 %.1f; C = Ad draws into a target"
                         " bounded within 0x80: with DATE %.1f, without %.1f; into one not so bounded: with DATE %.1f,"
                         " without %.1f", frames, hiW / 100.0, d128 / 100.0, p128 / 100.0, dWide / 100.0, pWide / 100.0);
            targets_log_alpha_bounds();
            frames = hiW = d128 = dWide = p128 = pWide = 0;
        }
    }
    /* (the stage's counts and stats into the executor's; PC, step 2: the
     * workers' too - every run published, so they are idle) */
    for (int c = 0; c < PCOUNT_N; c++)
        g_profA.count[c] += g_profB.count[c];
    for (int c = 0; c < PROF_COUNT; c++)
        g_profA.ticks[c] += g_profB.ticks[c];
    {
        const uint32_t *b = reinterpret_cast<const uint32_t *>(&g_statsB);
        uint32_t *a = reinterpret_cast<uint32_t *>(&g_statsA);
        for (size_t k = 0; k < sizeof(Stats) / sizeof(uint32_t); k++)
            a[k] += b[k];
    }
    for (auto &wk : g_workers) {
        for (int c = 0; c < PCOUNT_N; c++)
            g_profA.count[c] += wk->prof.count[c];
        for (int c = 0; c < PROF_COUNT; c++)
            g_profA.ticks[c] += wk->prof.ticks[c];
        const uint32_t *b = reinterpret_cast<const uint32_t *>(&wk->stats);
        uint32_t *a = reinterpret_cast<uint32_t *>(&g_statsA);
        for (size_t k = 0; k < sizeof(Stats) / sizeof(uint32_t); k++)
            a[k] += b[k];
        memset(&wk->stats, 0, sizeof(wk->stats));
    }
    /* the D3D thread's time and the executor's waits for it (all frames:
     * the counts start over) */
    double d3dMs = 0.0, syncMs = 0.0;
    uint32_t syncs = 0;
    q::take_stats(&d3dMs, &syncMs, &syncs);
    if (g_statsEvery < 0) {
        const char *e = getenv("GT3PC_PGL_STATS");
        g_statsEvery = (e != nullptr && atoi(e) > 0) ? atoi(e) : 0;
    }
    if (g_statsEvery > 0 && (frame_number() % (uint64_t)g_statsEvery) == 0) {
        const Stats &s = g_statsA;
        pclog::write("pgl stats frame %llu: regs %u ad %u target %u prim %u gif %u strip %u tri16 %u model %u"
                     " upload %u tex1 %u readback %u sub %u pfx %u/%u flare %u in %u batch(es) | draws %u verts %u |"
                     " skipped: nopass %u target %u scissor %u tex %u reflections %u | target copies whole %u, partial %u (DATE %u,"
                     " C = Ad %u, textures %u, Z %u) | alpha mirror passes %u (%u folded into their draws; the"
                     " texture's alpha varies %u, the vertex alpha %u, both %u, neither %u), syncs %u |"
                     " runs replayed %u (parts' textures from their records %u, looked up %u), resolved %u (%u not"
                     " kept so far, %u kept)",
                     (unsigned long long)frame_number(), s.cmds[CMD_REGS], s.cmds[CMD_AD], s.cmds[CMD_TARGET],
                     s.cmds[CMD_PRIM], s.cmds[CMD_GIF], s.cmds[CMD_STRIP], s.cmds[CMD_TRI16], s.cmds[CMD_MODEL],
                     s.cmds[CMD_UPLOAD], s.cmds[CMD_TEX1_UPLOAD], s.cmds[CMD_READBACK], s.cmds[CMD_SUBCALL],
                     s.cmds[CMD_PFX_COLOR], s.cmds[CMD_PFX_SPLIT], s.cmds[CMD_FLARE_QUERY], s.flareBatches, s.draws,
                     s.verts, s.skipNoPass, s.skipTarget, s.skipScissor, s.skipTex, s.skipRefl, g_copiesWhole, g_copiesPart,
                     g_copiesWhy[COPY_DATE], g_copiesWhy[COPY_AD], g_copiesWhy[COPY_TEXTURE], g_copiesWhy[COPY_ZTEX],
                     s.mirrorPasses, s.mirrorFolds, s.mirrorTex, s.mirrorVtx, s.mirrorBoth, s.mirrorOther,
                     s.mirrorSyncs, s.runsHit, s.runTexFree, s.runTexWalk, s.runsMiss, g_runsBad,
                     (unsigned)g_runs.size());
    }
    targets_env_maps();                     /* PC (2026-11-06): ([Graphics] EnvMapDetail: the env maps' size) */
    if ((frame_number() % 64) == 0)
        runs_housekeep();                   /* PC: (the resolved runs unused for a while) */
    if (!g_profOn)
        cap::end_frame(nullptr, nullptr, 0, nullptr, nullptr, 0, 0.0, subMs, d3dMs);  /* PC (2026-11-06): (the frame capture) */
    if (g_profOn) {
        /* the profile into the frame monitor: ticks as ms by the frame's
         * own TSC / QPC ratio */
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        const uint64_t ticks = __rdtsc() - g_profTsc0;
        const double ms = (double)(q.QuadPart - g_profQpc0) * 1000.0 / (double)f.QuadPart;
        float out[PROF_COUNT];
        for (int c = 0; c < PROF_COUNT; c++)
            out[c] = ticks > 0 ? (float)((double)g_profA.ticks[c] * ms / (double)ticks) : 0.0f;
        g_profCount[PCOUNT_COPIES] = g_copiesWhole + g_copiesPart;
        g_profCount[PCOUNT_SYNCS] = syncs;
        /* PC (2026-11-06): the frame capture's record of the frame's profile */
        cap::end_frame(out, kProfNames, PROF_COUNT, g_profCount, kProfCountNames, PCOUNT_N, ms, subMs, d3dMs);
        if (frame_monitor::mode() == frame_monitor::MODE_VERBOSE) {
            static bool named;
            if (!named) {
                frame_monitor::exec_profile_names(kProfNames, PROF_COUNT, kProfCountNames, PCOUNT_N);
                named = true;
            }
            frame_monitor::exec_profile(out, PROF_COUNT, g_profCount, PCOUNT_N);
            frame_monitor::exec_d3d_thread(q::running() ? (float)d3dMs : -1.0f, (float)syncMs);
            /* PC (2026-11-05): the submit thread's busy time and the executor's
             * waits for it (its ring full, the frame's end) */
            frame_monitor::exec_submit_thread(piped ? (float)subMs : -1.0f, (float)subWaitMs);
        }
        if (prof_log_every() > 0) {
            /* PC: GT3PC_PGL_PROFILE's mean, the kinds by their time */
            for (int c = 0; c < PROF_COUNT; c++)
                g_profLogMs[c] += out[c];
            for (int c = 0; c < PCOUNT_N; c++)
                g_profLogCounts[c] += g_profCount[c];
            g_profLogD3d += d3dMs;
            g_profLogSync += syncMs;
            g_profLogWall += ms;
            g_profLogSub += subMs;
            g_profLogSubWait += subWaitMs;
            if (++g_profLogFrames >= (uint32_t)g_profLogEvery) {
                const double n = (double)g_profLogFrames;
                int order[PROF_COUNT];
                for (int c = 0; c < PROF_COUNT; c++)
                    order[c] = c;
                std::sort(order, order + PROF_COUNT, [](int a, int b) { return g_profLogMs[a] > g_profLogMs[b]; });
                char kinds[640], counts[640];
                size_t at = 0;
                for (int i = 0; i < PROF_COUNT && at < sizeof(kinds); i++)
                    if (g_profLogMs[order[i]] / n >= 0.005)
                        at += (size_t)snprintf(kinds + at, sizeof(kinds) - at, "%s%s %.2f", at ? ", " : "",
                                               kProfNames[order[i]], g_profLogMs[order[i]] / n);
                at = 0;
                counts[0] = '\0';
                for (int c = 0; c < PCOUNT_N && at < sizeof(counts); c++)
                    if (g_profLogCounts[c] > 0.0)
                        at += (size_t)snprintf(counts + at, sizeof(counts) - at, "%s%s %.0f", at ? ", " : "",
                                               kProfCountNames[c], g_profLogCounts[c] / n);
                pclog::write("pgl cpu profile (ms a frame, mean of %u; the executor %.2f, the kinds both stages'): %s"
                             " | the submit thread busy %.2f, the executor's waits for it %.2f | the D3D thread busy"
                             " %.2f, the waits for it %.2f | counts a frame: %s",
                             g_profLogFrames, g_profLogWall / n, kinds, g_profLogSub / n, g_profLogSubWait / n,
                             g_profLogD3d / n, g_profLogSync / n, counts);
                registry_decode_report(g_profLogFrames);    /* PC: (the textures decoded again and again) */
                memset(g_profLogMs, 0, sizeof(g_profLogMs));
                memset(g_profLogCounts, 0, sizeof(g_profLogCounts));
                g_profLogD3d = g_profLogSync = g_profLogWall = g_profLogSub = g_profLogSubWait = 0.0;
                g_profLogFrames = 0;
            }
        }
        g_profOn = false;
    }
    memset(&g_statsA, 0, sizeof(g_statsA));
    memset(&g_statsB, 0, sizeof(g_statsB));
    g_copiesWhole = g_copiesPart = 0;
    memset(g_copiesWhy, 0, sizeof(g_copiesWhy));
    if (!g_flareGlows.empty()) {
        if (flare_log(true))
            pclog::write("pgl flare: frame %llu: glows (draw order), texel R:%s", (unsigned long long)frame_number(),
                         g_flareGlows.c_str());
        g_flareGlows.clear();
    }
    registry_end_frame();
    models_end_frame();
    if (flicker_on())
        flicker_end_frame(frame_number());
    next_frame();
}

}  /* namespace pglr */
