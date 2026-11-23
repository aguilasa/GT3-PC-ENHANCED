/* pgl_capture.cpp - the frame capture (pgl_capture.h; PC, 2026-11-06).
 *
 * The record of one frame as the game renders it.  The executor notes every
 * command it runs and every resolved run it replays (cmd, run); the submit
 * stage - the submit thread, a worker, or the executor when it submits
 * itself - notes every Direct3D draw, copy, clear and pass as it queues it
 * (draw_begin: what it is, which run and part it came from, why its first
 * part began a draw of its own, the bindings issued for it, its time on the
 * stage); the D3D thread, running the queued call, gives the record its
 * place in the final order and wraps the call in GPU queries - a timestamp
 * either side and the pipeline statistics (gpu_begin / gpu_end).  The frame
 * over, the queries are read on the D3D thread as they come ready (a few
 * frames later: the GPU runs behind) and the executor writes the files:
 * draws.csv (every draw), commands.csv, runs.csv, summary.txt (the tables),
 * and the frame's targets as pictures at its present (pgl_targets.cpp).
 *
 * What the numbers mean.  A draw's pixel shader invocations are its fill:
 * the pixels (samples) it shaded, exact and attributable.  Its GPU
 * microseconds are the time between the timestamps written before and after
 * it - the command processor's time to issue it, which includes the waits
 * when the pipeline behind it is full: in a fill-bound frame that time lands
 * on whichever draws happen to queue behind the heavy ones.  So: pixel
 * counts for what each draw costs the shading units, the microseconds for
 * the front end's cost of a draw where the counts are small, and both summed
 * for the frame.  The capture's own cost (the queries) falls on the captured
 * frame alone. */
#include "engine/pgl/native/pgl_capture.h"
#include "engine/pgl/native/pgl_win32.h"   /* (the Win32 calls off Windows too) */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_render.h"
#include "platform/log.h"

namespace pglr {
namespace cap {

bool g_on = false;
thread_local int32_t t_run = -1;
thread_local uint16_t t_part = 0xFFFF;
thread_local uint8_t t_mergeWhy = 0;
thread_local const char *t_note = nullptr;
thread_local uint8_t t_thread = 0;

namespace {

constexpr uint32_t kMaxDraws = 32768;
constexpr int kMaxPolls = 60;               /* frames to wait for the GPU's results */

struct Cmd {
    uint32_t index;
    uint8_t type;
    int32_t run;
    uint32_t ns;
};
struct Run {
    int32_t seq;
    uint64_t key;
    uint32_t nCmds, parts, states, steps, ns, texFree, texWalk, cuts, works;
};
struct Frame {
    uint64_t frame = 0;
    std::vector<Draw> draws;                /* kMaxDraws places; nDraws used */
    std::atomic<uint32_t> nDraws{0};
    std::vector<Cmd> cmds;
    std::vector<Run> runs;
    int32_t nextRun = 0;
    /* the frame's profile (end_frame) */
    std::vector<float> profMs;
    std::vector<const char *> profNames;
    std::vector<uint32_t> counts;
    std::vector<const char *> countNames;
    double execMs = 0, subMs = 0, d3dMs = 0;
    /* the GPU's results */
    double gpuFirstUs = 0, gpuLastUs = 0;   /* the first draw's start, the last draw's end */
    uint64_t freq = 0;
    bool disjoint = false;
    bool stats = true;                      /* the pipeline statistics were queried (GT3PC_PGL_CAPTURE_STATS) */
    int64_t beginQpc = 0;                   /* (the executor's wall time over the frame) */
    char dir[MAX_PATH] = "";
    bool wantDump = false;
};

bool g_armed = false;
int64_t g_envFrame = -2;                    /* GT3PC_PGL_CAPTURE=N (-2: not read, -1: none) */
bool g_statsOn = true;                      /* GT3PC_PGL_CAPTURE_STATS=0: timestamps alone (the frame nearer its usual time) */
Frame g_cur;                                /* the frame being captured (g_on) */
Frame *g_pend = nullptr;                    /* the frame whose GPU results are awaited (one at a time) */
Frame g_pendStore;
std::atomic<int> g_readState{0};            /* 0 idle, 1 reading queued, 2 ready, 3 not yet (retry) */
int g_polls = 0;
std::atomic<uint32_t> g_order{0};           /* the D3D thread's order of the draws */

/* the queries (the D3D thread's; made as needed, kept) */
Com<ID3D11Query> g_ts0[kMaxDraws], g_ts1[kMaxDraws], g_stats[kMaxDraws];
Com<ID3D11Query> g_disjoint;
std::atomic<uint32_t> g_queried{0};         /* records whose queries were issued */

thread_local uint32_t t_binds = 0;
thread_local int64_t t_lastQpc = 0;

int64_t qpc()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
int64_t qpf()
{
    static int64_t f = 0;
    if (f == 0) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
    }
    return f;
}

const char *const kKindNames[K_COUNT] = {"vu1", "vu1 merged", "gs batch", "mirror pass", "pass", "copy",
                                         "resolve", "clear", "present", "flare batch"};
const char *const kAdNames[AD_COUNT] = {"-", "2 instances", "pair 1st", "pair 2nd", "twice 1st", "twice 2nd",
                                        "shader", "doubled"};
const char *const kProgNames[] = {"default", "diffuse", "spec2", "spec4", "local", "envmtx", "envmap", "const",
                                  "flare"};
const char *const kMergeWhyNames[MW_COUNT] = {"first", "none", "state", "vs/cull", "buffers", "index", "batch",
                                              "record", "ps consts", "slots", "target tex", "C = Ad", "vs consts"};
const char *const kStencilNames[] = {"-", "replace", "date keep", "date invert", "fold replace", "fold date",
                                     "date pair"};
const char *const kCauseNames[] = {"-", "texture", "vertex", "both", "neither"};
const char *const kPrimNames[] = {"point", "line", "line strip", "triangle", "tri strip", "tri fan", "sprite",
                                  "reserved"};
const char *const kBindNames[17] = {"rtv", "dsv", "viewport", "scissor", "raster", "blend", "depth", "ps", "vs",
                                    "topology", "vb", "ib", "srv0", "samp0", "srv1", "ps cb", "vs cb"};

const char *cmd_name(uint8_t t)
{
    switch (t) {
    case CMD_REGS: return "regs";
    case CMD_AD: return "ad";
    case CMD_TARGET: return "target";
    case CMD_PRIM: return "prim";
    case CMD_GIF: return "gif";
    case CMD_STRIP: return "strip";
    case CMD_TRI16: return "tri16";
    case CMD_MODEL: return "model";
    case CMD_UPLOAD: return "upload";
    case CMD_TEX1_UPLOAD: return "tex1 upload";
    case CMD_READBACK: return "readback";
    case CMD_SUBCALL: return "subcall";
    case CMD_PFX_COLOR: return "pfx colour";
    case CMD_PFX_SPLIT: return "pfx split";
    case CMD_FLARE_QUERY: return "flare query";
    case CMD_HALF_BLEND: return "half blend";
    case CMD_DESIGN: return "design";
    case CMD_LINE_WIDTH: return "line width";
    case CMD_POST_AA: return "post aa";
    case CMD_GPU_SECTION: return "gpu section";
    case CMD_TEX_SOURCE: return "tex source";
    case 255: return "run replayed";
    default: return "?";
    }
}
const char *prog_name(uint8_t p)
{
    return p < sizeof(kProgNames) / sizeof(kProgNames[0]) ? kProgNames[p] : "-";
}
const char *name_of(const char *const *names, size_t n, unsigned i)
{
    return i < n ? names[i] : "?";
}
int bind_count(uint32_t m)
{
    int n = 0;
    for (; m != 0; m &= m - 1)
        n++;
    return n;
}

/* The capture's directory: GT3PC_CAPTURE_DIR, else capture\ next to the
 * executable; frame_N inside it (made here) */
bool make_dir(Frame &f)
{
    char root[MAX_PATH] = "";
    const char *e = getenv("GT3PC_CAPTURE_DIR");
    if (e != nullptr && e[0] != 0) {
        snprintf(root, sizeof(root), "%s", e);
    } else {
        GetModuleFileNameA(nullptr, root, MAX_PATH);
        char *slash = strrchr(root, PGL_PATH_SEP[0]);
        if (slash != nullptr)
            *slash = 0;
#ifdef _WIN32
        strncat_s(root, sizeof(root), "\\capture", _TRUNCATE);
#else
        strncat(root, "/capture", sizeof(root) - strlen(root) - 1);
#endif
    }
    CreateDirectoryA(root, nullptr);
    snprintf(f.dir, sizeof(f.dir), "%s" PGL_PATH_SEP "frame_%llu", root, (unsigned long long)f.frame);
    CreateDirectoryA(f.dir, nullptr);
    const DWORD a = GetFileAttributesA(f.dir);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* ---- the D3D thread: the queries read ---------------------------------- */

void read_queries()
{
    Frame &f = *g_pend;
    ID3D11DeviceContext *c = qdc();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    HRESULT h = c->GetData(g_disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (h == S_FALSE) {
        g_readState.store(3, std::memory_order_release);
        return;
    }
    if (h != S_OK) {
        f.freq = 0;
        g_readState.store(2, std::memory_order_release);
        return;
    }
    f.freq = dj.Frequency;
    f.disjoint = dj.Disjoint != 0;
    const uint32_t n = std::min(f.nDraws.load(), g_queried.load());
    uint64_t first = ~0ull, last = 0;
    for (uint32_t i = 0; i < n; i++) {
        Draw &d = f.draws[i];
        if (d.order == ~0u || !g_ts0[i] || !g_ts1[i] || (f.stats && !g_stats[i]))
            continue;
        UINT64 t0 = 0, t1 = 0;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS st = {};
        const HRESULT h0 = c->GetData(g_ts0[i].get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT h1 = c->GetData(g_ts1[i].get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT hs = f.stats ? c->GetData(g_stats[i].get(), &st, sizeof(st), D3D11_ASYNC_GETDATA_DONOTFLUSH)
                                   : S_OK;
        if (h0 == S_FALSE || h1 == S_FALSE || hs == S_FALSE) {
            g_readState.store(3, std::memory_order_release);
            return;
        }
        if (h0 == S_OK && h1 == S_OK && f.freq != 0) {
            d.gpuUs = (double)(t1 - t0) * 1e6 / (double)f.freq;
            first = std::min(first, (uint64_t)t0);
            last = std::max(last, (uint64_t)t1);
        }
        if (hs == S_OK && f.stats) {
            d.iaVerts = st.IAVertices;
            d.iaPrims = st.IAPrimitives;
            d.vsInv = st.VSInvocations;
            d.rastPrims = st.CPrimitives;
            d.psInv = st.PSInvocations;
        }
    }
    if (first != ~0ull && f.freq != 0) {
        f.gpuFirstUs = (double)first * 1e6 / (double)f.freq;
        f.gpuLastUs = (double)last * 1e6 / (double)f.freq;
    }
    g_readState.store(2, std::memory_order_release);
}

/* ---- the files ---------------------------------------------------------- */

struct Agg {
    uint32_t n = 0, parts = 0;
    double gpuUs = 0, cpuUs = 0;
    uint64_t ps = 0, verts = 0, rast = 0;
    uint32_t binds = 0;
    void add(const Draw &d)
    {
        n++;
        parts += d.parts;
        if (d.gpuUs >= 0)
            gpuUs += d.gpuUs;
        cpuUs += d.cpuNs / 1000.0;
        ps += d.psInv;
        verts += d.iaVerts;
        rast += d.rastPrims;
        binds += (uint32_t)bind_count(d.binds);
    }
};

void agg_header(FILE *o, const char *first)
{
    fprintf(o, "%-28s %7s %7s %10s %10s %10s %9s %8s %7s\n", first, "draws", "parts", "GPU us", "PS Mpx", "verts k",
            "rast k", "CPU us", "binds");
}
void agg_row(FILE *o, const char *name, const Agg &a)
{
    if (a.n == 0)
        return;
    fprintf(o, "%-28s %7u %7u %10.0f %10.3f %10.1f %9.1f %8.0f %7.2f\n", name, a.n, a.parts, a.gpuUs,
            a.ps / 1e6, a.verts / 1e3, a.rast / 1e3, a.cpuUs, a.n ? (double)a.binds / a.n : 0.0);
}

void write_draws_csv(const Frame &f, const std::vector<const Draw *> &ord)
{
    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "draws.csv", f.dir);
    FILE *o = fopen(path, "w");
    if (o == nullptr)
        return;
    fprintf(o, "order,id,thread,kind,run,part,parts,instances,count,program,prim,ad_form,blend,write_mask,ztest,"
               "zwrite,zfunc,date,datm,stencil_op,stencil_ref,mirror_pass,fold,flat,slots,merge_why,binds,bind_count,"
               "cpu_ns,target,target_w,target_h,x0,y0,x1,y1,depth_frame,textured,texture_is_target,gpu_us,ia_verts,"
               "ia_prims,vs_inv,rast_prims,ps_inv,atst,aref,agate,note\n");
    for (const Draw *dp : ord) {
        const Draw &d = *dp;
        fprintf(o, "%u,%u,%u,%s,%d,%d,%u,%u,%u,%s,%s,%s,0x%02X,%u,%u,%u,%u,%u,%u,%s,%u,%u,%u,%u,%u,%s,0x%X,%d,%u,0x%X,%u,"
                   "%u,%d,%d,%d,%d,%u,%u,%u,%.2f,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%s\n",
                d.order, d.id, d.thread, kKindNames[d.kind], d.run, d.part == 0xFFFF ? -1 : (int)d.part, d.parts,
                d.instances, d.count, d.program == 255 ? "-" : prog_name(d.program),
                d.prim == 255 ? "-" : name_of(kPrimNames, 8, d.prim), kAdNames[d.adForm], d.blendSel, d.writeMask,
                d.zTest, d.zWrite, d.zFunc, d.date, d.datm, name_of(kStencilNames, 7, d.stencilOp), d.stencilRef,
                d.mirrorPass, d.fold, d.flat, d.slots, name_of(kMergeWhyNames, MW_COUNT, d.mergeWhy), d.binds,
                bind_count(d.binds), d.cpuNs, d.target, d.targetW, d.targetH, d.x0, d.y0, d.x1, d.y1, d.depthFrame,
                d.tme, d.texTarget, d.gpuUs, (unsigned long long)d.iaVerts, (unsigned long long)d.iaPrims,
                (unsigned long long)d.vsInv, (unsigned long long)d.rastPrims, (unsigned long long)d.psInv,
                d.atst, d.aref, d.agate, d.note ? d.note : "");
    }
    fclose(o);
}

void write_cmds_csv(const Frame &f)
{
    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "commands.csv", f.dir);
    FILE *o = fopen(path, "w");
    if (o == nullptr)
        return;
    fprintf(o, "index,type,run,ns\n");
    for (const Cmd &c : f.cmds)
        fprintf(o, "%u,%s,%d,%u\n", c.index, cmd_name(c.type), c.run, c.ns);
    fclose(o);
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "runs.csv", f.dir);
    o = fopen(path, "w");
    if (o == nullptr)
        return;
    fprintf(o, "seq,key,commands,parts,states,steps,replay_ns,tex_from_record,tex_resolved,cuts,works\n");
    for (const Run &r : f.runs)
        fprintf(o, "%d,%016llX,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", r.seq, (unsigned long long)r.key, r.nCmds, r.parts,
                r.states, r.steps, r.ns, r.texFree, r.texWalk, r.cuts, r.works);
    fclose(o);
}

void write_summary(const Frame &f, const std::vector<const Draw *> &ord)
{
    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "summary.txt", f.dir);
    FILE *o = fopen(path, "w");
    if (o == nullptr)
        return;
    const uint32_t n = (uint32_t)ord.size();
    Agg all, drawn;
    for (const Draw *d : ord) {
        all.add(*d);
        if (d->kind == K_VU1 || d->kind == K_VU1_MERGED || d->kind == K_GS)
            drawn.add(*d);
    }
    fprintf(o, "GT3 PC frame capture - frame %llu\n", (unsigned long long)f.frame);
    fprintf(o, "%u Direct3D calls recorded: %u draws of %u parts, %u mirror passes, %u other (passes, copies, clears,"
               " the present); %u commands executed, %u runs replayed\n\n", n, drawn.n, drawn.parts,
            (unsigned)std::count_if(ord.begin(), ord.end(), [](const Draw *d) { return d->kind == K_MIRROR; }),
            n - drawn.n - (unsigned)std::count_if(ord.begin(), ord.end(), [](const Draw *d) { return d->kind == K_MIRROR; }),
            (unsigned)f.cmds.size(), (unsigned)f.runs.size());
    fprintf(o, "CPU stages (ms): executor %.2f (the frame's execution, wall time), submit thread %.2f (busy), D3D thread"
               " %.2f (busy)\n", f.execMs, f.subMs, f.d3dMs);
    if (f.freq != 0) {
        fprintf(o, "GPU: from the first call's start to the last call's end %.2f ms%s; the calls' issue times sum to"
                   " %.2f ms; pixel shader invocations %.1f M, vertices %.1f k, primitives rasterized %.1f k%s\n",
                (f.gpuLastUs - f.gpuFirstUs) / 1000.0, f.disjoint ? " (DISJOINT: the clock changed, times unreliable)" : "",
                all.gpuUs / 1000.0, all.ps / 1e6, all.verts / 1e3, all.rast / 1e3,
                f.stats ? "" : " (GT3PC_PGL_CAPTURE_STATS=0: no pipeline statistics, the counts are 0)");
    } else {
        fprintf(o, "GPU: no results (the queries did not come back)\n");
    }
    fprintf(o, "(How to read the GPU numbers. A call's PS invocations are its fill - the pixels (samples) it shaded -"
               " exact and its own. Its GPU us are the time between the timestamps written before and after it on the"
               " command processor: the front end's cost of issuing it plus any wait for the pipeline behind it, so in"
               " a fill-bound frame the waits land on whatever calls queue behind the heavy ones; and the capture's"
               " own queries slow the captured frame - its total here is well above the frame's usual GPU time (the"
               " F3 view's), the more so with the pipeline statistics on. Use the pixel counts for what a draw costs"
               " the shading units, the issue times for the front end's cost of small draws, and compare totals"
               " between kinds rather than reading one draw's us as its cost. GT3PC_PGL_CAPTURE_STATS=0 captures"
               " timestamps alone, nearer the frame's real timing, without the pixel counts.)\n\n");

    /* the CPU profile */
    if (!f.profMs.empty()) {
        fprintf(o, "Executor CPU profile (ms, this frame):");
        for (size_t i = 0; i < f.profMs.size(); i++)
            if (f.profMs[i] >= 0.005f)
                fprintf(o, " %s %.2f,", f.profNames[i], f.profMs[i]);
        fprintf(o, "\nCounts:");
        for (size_t i = 0; i < f.counts.size(); i++)
            if (f.counts[i] != 0)
                fprintf(o, " %s %u,", f.countNames[i], f.counts[i]);
        fprintf(o, "\n\n");
    }

    /* by kind */
    fprintf(o, "== By kind ==\n");
    agg_header(o, "kind");
    {
        Agg a[K_COUNT];
        for (const Draw *d : ord)
            a[d->kind].add(*d);
        for (int k = 0; k < K_COUNT; k++)
            agg_row(o, kKindNames[k], a[k]);
        agg_row(o, "ALL", all);
    }
    fprintf(o, "\n");

    /* VU1 draws by program and C = Ad form */
    fprintf(o, "== VU1 draws by lighting program and C = Ad form ==\n");
    agg_header(o, "program / form");
    {
        for (unsigned p = 0; p < 9; p++) {
            for (unsigned ad = 0; ad < AD_COUNT; ad++) {
                Agg a;
                for (const Draw *d : ord)
                    if ((d->kind == K_VU1 || d->kind == K_VU1_MERGED) && d->program == p && d->adForm == ad)
                        a.add(*d);
                if (a.n != 0) {
                    char name[64];
                    snprintf(name, sizeof(name), "%s / %s", prog_name((uint8_t)p), kAdNames[ad]);
                    agg_row(o, name, a);
                }
            }
        }
    }
    fprintf(o, "\n");

    /* why the VU1 draws did not join the one before */
    fprintf(o, "== Why a VU1 draw began instead of joining the draw before it ==\n");
    {
        uint32_t why[MW_COUNT] = {};
        uint32_t vu1 = 0;
        for (const Draw *d : ord)
            if (d->kind == K_VU1 || d->kind == K_VU1_MERGED) {
                vu1++;
                why[d->mergeWhy < MW_COUNT ? d->mergeWhy : 0]++;
            }
        for (int w = 0; w < MW_COUNT; w++)
            if (why[w] != 0)
                fprintf(o, "%-28s %7u  (%.1f %%)\n", kMergeWhyNames[w], why[w], vu1 ? 100.0 * why[w] / vu1 : 0.0);
        fprintf(o, "parts a draw:");
        uint32_t h[6] = {};
        for (const Draw *d : ord)
            if (d->kind == K_VU1 || d->kind == K_VU1_MERGED)
                h[d->parts <= 1 ? 0 : d->parts <= 3 ? 1 : d->parts <= 7 ? 2 : d->parts <= 15 ? 3 : d->parts <= 31 ? 4 : 5]++;
        fprintf(o, " 1: %u, 2-3: %u, 4-7: %u, 8-15: %u, 16-31: %u, 32+: %u\n\n", h[0], h[1], h[2], h[3], h[4], h[5]);
    }

    /* mirror passes */
    fprintf(o, "== Alpha mirror passes by cause ==\n");
    {
        Agg a[5];
        for (const Draw *d : ord)
            if (d->kind == K_MIRROR)
                a[d->cause < 5 ? d->cause : 0].add(*d);
        agg_header(o, "cause");
        for (int c = 0; c < 5; c++)
            agg_row(o, kCauseNames[c], a[c]);
        uint32_t folded = 0;
        for (const Draw *d : ord)
            folded += d->fold;
        fprintf(o, "(%u draws with their mirror pass folded in)\n\n", folded);
    }

    /* bindings */
    fprintf(o, "== Bindings issued before a draw (the binding cache's calls) ==\n");
    {
        uint32_t hist[6] = {};
        uint32_t bits[17] = {};
        for (const Draw *d : ord) {
            const int c = bind_count(d->binds);
            hist[c == 0 ? 0 : c == 1 ? 1 : c == 2 ? 2 : c <= 4 ? 3 : c <= 8 ? 4 : 5]++;
            for (int b = 0; b < 17; b++)
                if (d->binds & (1u << b))
                    bits[b]++;
        }
        fprintf(o, "binds a draw: 0: %u, 1: %u, 2: %u, 3-4: %u, 5-8: %u, 9+: %u\n", hist[0], hist[1], hist[2],
                hist[3], hist[4], hist[5]);
        fprintf(o, "which:");
        for (int b = 0; b < 17; b++)
            if (bits[b] != 0)
                fprintf(o, " %s %u,", kBindNames[b], bits[b]);
        fprintf(o, "\n\n");
    }

    /* the heaviest draws */
    auto top = [&](const char *title, bool byPs) {
        std::vector<const Draw *> v(ord);
        std::sort(v.begin(), v.end(), [byPs](const Draw *a, const Draw *b) {
            return byPs ? a->psInv > b->psInv : a->gpuUs > b->gpuUs;
        });
        fprintf(o, "== %s ==\n", title);
        fprintf(o, "%6s %12s %4s %4s %5s %8s %8s %9s %10s %6s %7s %s\n", "order", "kind", "run", "part", "parts",
                "program", "form", "GPU us", "PS Mpx", "verts", "target", "note");
        for (size_t i = 0; i < v.size() && i < 25; i++) {
            const Draw &d = *v[i];
            fprintf(o, "%6u %12s %4d %4d %5u %8s %8s %9.1f %10.3f %6llu 0x%-5X %s\n", d.order, kKindNames[d.kind],
                    d.run, d.part == 0xFFFF ? -1 : (int)d.part, d.parts, d.program == 255 ? "-" : prog_name(d.program),
                    kAdNames[d.adForm], d.gpuUs, d.psInv / 1e6, (unsigned long long)d.iaVerts, d.target,
                    d.note ? d.note : "");
        }
        fprintf(o, "\n");
    };
    top("The 25 draws with the most pixel shader invocations", true);
    top("The 25 draws with the longest issue time", false);

    /* the runs */
    fprintf(o, "== Resolved runs replayed (a car each, mostly) ==\n");
    fprintf(o, "%4s %5s %6s %6s %6s %10s %10s %9s %8s %8s %5s %5s\n", "seq", "parts", "steps", "draws", "mirr",
            "GPU us", "PS Mpx", "replay us", "tex rec", "tex res", "cuts", "works");
    {
        std::vector<Agg> per(f.runs.size());
        std::vector<uint32_t> mirr(f.runs.size(), 0);
        for (const Draw *d : ord)
            if (d->run >= 0 && (size_t)d->run < per.size()) {
                if (d->kind == K_MIRROR)
                    mirr[d->run]++;
                else
                    per[d->run].add(*d);
            }
        for (size_t i = 0; i < f.runs.size(); i++) {
            const Run &r = f.runs[i];
            fprintf(o, "%4d %5u %6u %6u %6u %10.0f %10.3f %9.1f %8u %8u %5u %5u\n", r.seq, r.parts, r.steps, per[i].n,
                    mirr[i], per[i].gpuUs, per[i].ps / 1e6, r.ns / 1000.0, r.texFree, r.texWalk, r.cuts, r.works);
        }
    }
    fprintf(o, "\n");

    /* the targets */
    fprintf(o, "== By target ==\n");
    {
        struct T {
            uint32_t base, w, h;
            Agg draws;
            uint32_t clears = 0, copies = 0, passes = 0;
        };
        std::vector<T> ts;
        for (const Draw *d : ord) {
            size_t i = 0;
            for (; i < ts.size(); i++)
                if (ts[i].base == d->target && ts[i].w == d->targetW && ts[i].h == d->targetH)
                    break;
            if (i == ts.size())
                ts.push_back(T{d->target, d->targetW, d->targetH});
            T &t = ts[i];
            if (d->kind == K_CLEAR)
                t.clears++;
            else if (d->kind == K_COPY || d->kind == K_RESOLVE)
                t.copies++;
            else if (d->kind == K_PASS || d->kind == K_PRESENT || d->kind == K_FLARE)
                t.passes++;
            else
                t.draws.add(*d);
        }
        fprintf(o, "%-10s %-11s %7s %7s %10s %10s %7s %7s %7s\n", "target", "native", "draws", "parts", "GPU us",
                "PS Mpx", "clears", "copies", "passes");
        for (const T &t : ts)
            fprintf(o, "0x%-8X %5ux%-5u %7u %7u %10.0f %10.3f %7u %7u %7u\n", t.base, t.w, t.h, t.draws.n,
                    t.draws.parts, t.draws.gpuUs, t.draws.ps / 1e6, t.clears, t.copies, t.passes);
    }
    fprintf(o, "\n");

    /* the commands */
    fprintf(o, "== Commands executed, by kind (the executor's time) ==\n");
    {
        struct C {
            uint32_t n = 0;
            uint64_t ns = 0;
        } c[256];
        for (const Cmd &k : f.cmds) {
            c[k.type].n++;
            c[k.type].ns += k.ns;
        }
        fprintf(o, "%-16s %8s %10s %8s\n", "kind", "count", "total us", "us each");
        for (int t = 0; t < 256; t++)
            if (c[t].n != 0)
                fprintf(o, "%-16s %8u %10.1f %8.2f\n", cmd_name((uint8_t)t), c[t].n, c[t].ns / 1000.0,
                        c[t].ns / 1000.0 / c[t].n);
    }
    fprintf(o, "\nFiles: draws.csv (every call in the D3D thread's order, one row each: its kind, run and part,"
               " parts, instances, program, blend form, write mask, depth and stencil state, DATE, mirror pass,"
               " merge verdict, the bindings issued for it, the stage's time, its target and area, and the GPU's"
               " timestamps and pipeline statistics), commands.csv (every command the executor ran, its kind, run and"
               " time), runs.csv (every resolved run replayed), frame.bmp and target_<base>_<w>x<h>.bmp (the"
               " targets at the frame's present).\n");
    fclose(o);
}

void write_files(Frame &f)
{
    std::vector<const Draw *> ord;
    const uint32_t n = std::min(f.nDraws.load(), kMaxDraws);
    ord.reserve(n);
    for (uint32_t i = 0; i < n; i++)
        ord.push_back(&f.draws[i]);
    std::sort(ord.begin(), ord.end(), [](const Draw *a, const Draw *b) {
        if (a->order != b->order)
            return a->order < b->order;
        return a->id < b->id;
    });
    write_draws_csv(f, ord);
    write_cmds_csv(f);
    write_summary(f, ord);
    Agg all;
    for (const Draw *d : ord)
        all.add(*d);
    pclog::write("pgl capture: frame %llu written to %s - %u Direct3D calls (%u parts), %u commands, %u runs; GPU"
                 " %.2f ms (the draws' issue times), %.1f M pixel shader invocations, %s",
                 (unsigned long long)f.frame, f.dir, n, all.parts, (unsigned)f.cmds.size(), (unsigned)f.runs.size(),
                 all.gpuUs / 1000.0, all.ps / 1e6, f.freq != 0 ? "see summary.txt" : "no GPU results");
}

}  /* namespace */

/* ---- arming and the frame ---------------------------------------------- */

void arm()
{
    g_armed = true;
}

void begin_frame(uint64_t frame)
{
    if (g_envFrame == -2) {
        const char *e = getenv("GT3PC_PGL_CAPTURE");
        g_envFrame = (e != nullptr && atoll(e) > 0) ? atoll(e) : -1;
        if (g_envFrame > 0)
            pclog::write("pgl: GT3PC_PGL_CAPTURE=%lld: frame %lld will be captured (capture\\frame_%lld\\)",
                         (long long)g_envFrame, (long long)g_envFrame, (long long)g_envFrame);
        const char *s = getenv("GT3PC_PGL_CAPTURE_STATS");
        g_statsOn = !(s != nullptr && strcmp(s, "0") == 0);
        pclog::write("pgl: the FrameCapture hotkey (F11) records the next frame in full (capture\\frame_N\\ next to"
                     " the executable%s)", g_statsOn ? "" : "; GT3PC_PGL_CAPTURE_STATS=0: timestamps alone");
    }
    poll();
    if ((int64_t)frame == g_envFrame)
        g_armed = true;
    g_on = false;
    if (!g_armed)
        return;
    g_armed = false;
    if (g_pend != nullptr) {
        pclog::write("pgl capture: frame %llu not captured - the capture before it is still being read",
                     (unsigned long long)frame);
        return;
    }
    Frame &f = g_cur;
    f.frame = frame;
    if (f.draws.size() != kMaxDraws)
        f.draws.resize(kMaxDraws);
    f.nDraws.store(0);
    f.cmds.clear();
    f.runs.clear();
    f.nextRun = 0;
    f.profMs.clear();
    f.counts.clear();
    f.freq = 0;
    f.disjoint = false;
    f.stats = g_statsOn;
    f.beginQpc = qpc();
    f.gpuFirstUs = f.gpuLastUs = 0;
    f.wantDump = false;
    if (!make_dir(f)) {
        pclog::write("pgl capture: the directory %s could not be made: frame %llu not captured", f.dir,
                     (unsigned long long)frame);
        return;
    }
    g_order.store(0);
    g_queried.store(0);
    q::run([] {
        if (!g_disjoint) {
            D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            dev()->CreateQuery(&d, g_disjoint.put());
        }
        if (g_disjoint)
            qdc()->Begin(g_disjoint.get());
    });
    g_on = true;
    pclog::write("pgl capture: frame %llu begins (%s)", (unsigned long long)frame, f.dir);
}

void end_frame(const float *profMs, const char *const *profNames, int nProf, const uint32_t *counts,
               const char *const *countNames, int nCounts, double execMs, double subMs, double d3dMs)
{
    if (!g_on)
        return;
    g_on = false;
    Frame &f = g_cur;
    f.profMs.assign(profMs, profMs + nProf);
    f.profNames.assign(profNames, profNames + nProf);
    f.counts.assign(counts, counts + nCounts);
    f.countNames.assign(countNames, countNames + nCounts);
    f.execMs = execMs > 0.0 ? execMs : (double)(qpc() - f.beginQpc) * 1000.0 / (double)qpf();
    f.subMs = subMs;
    f.d3dMs = d3dMs;
    q::run([] {
        if (g_disjoint)
            qdc()->End(g_disjoint.get());
    });
    /* (the frame's draws are the pending capture's now: the GPU's results
     * come later; g_cur stands until they have) */
    g_pend = &g_cur;
    g_readState.store(0);
    g_polls = 0;
    f.wantDump = true;
}

const char *dump_dir()
{
    if (g_pend == nullptr || !g_pend->wantDump)
        return nullptr;
    g_pend->wantDump = false;
    return g_pend->dir;
}

void poll()
{
    if (g_pend == nullptr)
        return;
    const int st = g_readState.load(std::memory_order_acquire);
    if (st == 2) {
        write_files(*g_pend);
        g_pend = nullptr;
        return;
    }
    if (st == 1)
        return;                             /* (the D3D thread has the read in hand) */
    if (++g_polls > kMaxPolls) {
        pclog::write("pgl capture: the GPU's results for frame %llu did not come in %d frames: written without them",
                     (unsigned long long)g_pend->frame, kMaxPolls);
        write_files(*g_pend);
        g_pend = nullptr;
        return;
    }
    g_readState.store(1, std::memory_order_release);
    q::run([] { read_queries(); });
}

/* ---- the submit stage ---------------------------------------------------- */

uint32_t draw_begin(const Draw &d)
{
    if (!g_on)
        return ~0u;
    Frame &f = g_cur;
    const uint32_t i = f.nDraws.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxDraws)
        return ~0u;
    Draw &r = f.draws[i];
    r = d;
    r.id = i;
    r.order = ~0u;
    r.thread = t_thread;
    r.binds = take_binds();
    r.cpuNs = take_cpu_ns();
    if (r.run < 0)
        r.run = t_run;
    if (r.part == 0xFFFF)
        r.part = t_part;
    r.gpuUs = -1.0;
    r.iaVerts = r.iaPrims = r.vsInv = r.rastPrims = r.psInv = 0;
    return i;
}

void draw_parts(uint32_t id, uint16_t parts, uint8_t slots)
{
    if (id == ~0u || id >= kMaxDraws)
        return;
    Draw &r = g_cur.draws[id];
    r.parts = parts;
    r.slots = slots;
    if (parts > 1 && r.kind == K_VU1)
        r.kind = K_VU1_MERGED;
}

void draw_area(uint32_t id, long x0, long y0, long x1, long y1)
{
    if (id == ~0u || id >= kMaxDraws)
        return;
    Draw &r = g_cur.draws[id];
    auto c16 = [](long v) { return (int16_t)std::max(-32768L, std::min(32767L, v)); };
    r.x0 = c16(x0);
    r.y0 = c16(y0);
    r.x1 = c16(x1);
    r.y1 = c16(y1);
}

void bind(uint32_t d3db)
{
    t_binds |= d3db;
}

uint32_t take_binds()
{
    const uint32_t b = t_binds;
    t_binds = 0;
    return b;
}

uint32_t take_cpu_ns()
{
    const int64_t now = qpc();
    const int64_t last = t_lastQpc;
    t_lastQpc = now;
    if (last == 0)
        return 0;
    const double ns = (double)(now - last) * 1e9 / (double)qpf();
    return ns < 0 ? 0 : ns > 4e9 ? 0 : (uint32_t)ns;
}

/* ---- the D3D thread ------------------------------------------------------ */

void gpu_begin(uint32_t id)
{
    if (id == ~0u || id >= kMaxDraws)
        return;
    if (!g_ts0[id]) {
        D3D11_QUERY_DESC t = {D3D11_QUERY_TIMESTAMP, 0};
        dev()->CreateQuery(&t, g_ts0[id].put());
        dev()->CreateQuery(&t, g_ts1[id].put());
    }
    if (g_cur.stats && !g_stats[id]) {
        D3D11_QUERY_DESC s = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
        dev()->CreateQuery(&s, g_stats[id].put());
    }
    if (g_ts0[id])
        qdc()->End(g_ts0[id].get());
    if (g_cur.stats && g_stats[id])
        qdc()->Begin(g_stats[id].get());
}

void gpu_end(uint32_t id)
{
    if (id == ~0u || id >= kMaxDraws)
        return;
    if (g_cur.stats && g_stats[id])
        qdc()->End(g_stats[id].get());
    if (g_ts1[id])
        qdc()->End(g_ts1[id].get());
    g_cur.draws[id].order = g_order.fetch_add(1, std::memory_order_relaxed);
    uint32_t q = g_queried.load(std::memory_order_relaxed);
    while (q < id + 1 && !g_queried.compare_exchange_weak(q, id + 1, std::memory_order_relaxed)) {
    }
}

/* ---- the executor -------------------------------------------------------- */

void cmd(uint32_t index, uint8_t type, uint32_t ns, int32_t run)
{
    if (!g_on)
        return;
    g_cur.cmds.push_back(Cmd{index, type, run, ns});
}

void run(int32_t seq, uint64_t key, uint32_t nCmds, uint32_t parts, uint32_t states, uint32_t steps, uint32_t ns,
         uint32_t texFree, uint32_t texWalk, uint32_t cuts, uint32_t works)
{
    if (!g_on)
        return;
    g_cur.runs.push_back(Run{seq, key, nCmds, parts, states, steps, ns, texFree, texWalk, cuts, works});
}

int32_t next_run()
{
    if (!g_on)
        return -1;
    return g_cur.nextRun++;
}

}  /* namespace cap */
}  /* namespace pglr */
