/* pgl_render.cpp - the bridge of pgl to the native renderer
 * (pgl_bridge.h): the command lists, the frame flow, the sub-lists, the
 * immediate transfers and the presentation entry points.
 *
 * Frame flow (docs/renderer.md): gfx_begin_packet starts a list, every
 * recording appends to it, gfx_end_packet completes it, gfx_kick_frame_lists
 * executes it (as the PS2's DMA ran the packet: the pointers recorded are
 * read now), gfx_set_display_fb presents the target at the displayed
 * address.  What is recorded outside an open frame is dropped at the next
 * begin (the PS2 never sent it). */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#include <cstdlib>

#include "engine/pgl/native/pgl_intrin.h"
#include "engine/pgl/native/pgl_win32.h"

#include "platform/fpu.h"
#include "platform/frame_monitor.h"
#include "platform/gpu.h"
#include "platform/log.h"
#include "platform/sample_prof.h"          /* PC: GT3PC_SAMPLE */

namespace pglr {

/* ======================================================================== */
/* Command lists                                                            */
/* ======================================================================== */

void *CmdList::add(CmdType t, size_t bytes)
{
    size_t at = data_.size();
    size_t total = 16 + ((bytes + 15) & ~(size_t)15);

    data_.resize(at + total);
    data_[at] = (uint8_t)t;
    offs_.push_back((uint32_t)at);
    keys_.push_back(0);                     /* PC: (set_last_key: the resolved runs' structure keys) */
    return &data_[at + 16];
}

namespace {

/* Frame timing (logged every GT3PC_PGL_STATS frames, default 600):
 * recording (gfx_begin_packet .. gfx_end_packet: the game's render code with
 * pgl's recording), execution (gfx_kick_frame_lists), presentation
 * (gfx_set_display_fb), the frame period (kick to kick) and the GPU time of
 * the execution (timestamp queries, read a few frames later). */
struct Timing {
    double freq = 0;
    int64_t begin = 0;
    int64_t lastKick = 0;
    double rec = 0, exec = 0, present = 0, period = 0, gpu = 0;
    double recMax = 0, execMax = 0, presentMax = 0, periodMax = 0, gpuMax = 0;
    int n = 0, gpuN = 0;
    int every = -1;
    /* GPU queries */
    struct Q {
        Com<ID3D11Query> disjoint, t0, t1;
        bool pending = false;
        uint32_t seq = 0;                   /* the frame monitor's frame (frame_monitor::gpu_time) */
    } q[4];
    int qi = 0;
};
Timing g_t;

int64_t qpc()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double ms_since(int64_t t0)
{
    return (double)(qpc() - t0) * 1000.0 / g_t.freq;
}

/* The calling thread's CPU cycles so far (QueryThreadCycleTime: the time
 * it ran, not the time it was blocked). */
uint64_t thread_cycles()
{
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return c;
}

/* Thread cycles as ms: they count at the TSC's rate, measured against QPC
 * over the run (-1 for its first 200 ms: not known yet). */
double cycles_ms(uint64_t cycles)
{
    static int64_t q0;
    static uint64_t r0;
    const int64_t q = qpc();
    const uint64_t r = __rdtsc();
    if (q0 == 0) {
        q0 = q;
        r0 = r;
        return -1.0;
    }
    const double ms = (double)(q - q0) * 1000.0 / g_t.freq;
    if (ms < 200.0 || r <= r0)
        return -1.0;
    return (double)cycles * ms / (double)(r - r0);
}

CmdList g_frame;                    /* being recorded */
CmdList g_ready;                    /* ended, waiting for the kick */
bool    g_readyValid = false;
/* The pictures' aspects of those two frames (pglr_set_picture_aspect; 0:
 * the 4:3 picture) */
float   g_frameAspect = 0.0f, g_readyAspect = 0.0f;
bool    g_frameMulti = false, g_readyMulti = false;    /* (pglr_set_picture_multi) */
float   g_menuAspect = 0.0f;                /* (pglr_set_menu_aspect: a 4:3 frame's when > 0) */
/* A readback the game reads was recorded in the frame (pglr_readback: the
 * frame executes at the kick, not on the executor thread) */
bool    g_frameReadback = false, g_readyReadback = false;
/* The game thread's waits for the executor thread since the frame's
 * recording began (the game's DMA wait lies inside it, gfx_dma_sync_frame:
 * RECORD leaves it to EXECUTE) */
double  g_recWaitMs = 0.0;
CmdList *g_rec = &g_frame;          /* where recordings go */
std::vector<CmdList *> g_recStack;
std::unordered_map<const void *, std::unique_ptr<CmdList>> g_subs;
bool    g_inited = false;

}  /* namespace */

double g_readbackWait;

const CmdList *sub_list(const void *key)
{
    auto it = g_subs.find(key);
    return it == g_subs.end() ? nullptr : it->second.get();
}

/* ======================================================================== */
/* Logging                                                                  */
/* ======================================================================== */

void log_once(const char *key, const char *fmt, ...)
{
    /* by the key's hash: no allocation on the (frequent) repeat; locked (the
     * D3D thread's calls log too) */
    static std::set<uint64_t> seen;
    static std::mutex lock;
    uint64_t h = 0xCBF29CE484222325ull;
    for (const char *p = key; *p; p++)
        h = (h ^ (uint8_t)*p) * 0x100000001B3ull;
    {
        std::lock_guard<std::mutex> hold(lock);
        if (!seen.insert(h).second)
            return;
    }
    va_list ap;
    va_start(ap, fmt);
    pclog::vwrite(fmt, ap);
    va_end(ap);
}

/* (Crashes are reported by platform/crash.cpp: the stack in the log and a
 * minidump.) */

}  /* namespace pglr */

using namespace pglr;

extern "C" {

/* (the executor thread, below) */
static void exec_wait(void);
static void exec_start(void);
static void context_guard(void);

/* ======================================================================== */
/* Driver                                                                   */
/* ======================================================================== */

void pglr_init(void)
{
    if (g_inited)
        return;
    fpu::DefaultFpuMode fpuMode;
    exec_init();
    registry_init();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_t.freq = (double)f.QuadPart;
    exec_start();                           /* PC: the executor thread */
    pre::start();                           /* PC: the worker decoding what is loaded (pgl_prewarm.cpp) */
    gpu::set_context_guard(&context_guard);  /* (a use of the context waits for both threads) */
    g_inited = true;
    pclog::write("pgl: native renderer up (output scale %.3f)", output_scale());
}

uint64_t pglr_load64(const volatile uint64_t *p)
{
    return (uint64_t)_InterlockedCompareExchange64((volatile long long *)p, 0, 0);
}

void pglr_store64(volatile uint64_t *p, uint64_t v)
{
    long long old = _InterlockedCompareExchange64((volatile long long *)p, 0, 0);
    for (;;) {
        long long seen = _InterlockedCompareExchange64((volatile long long *)p, (long long)v, old);
        if (seen == old)
            return;
        old = seen;
    }
}

void pglr_add64(volatile uint64_t *p, uint64_t v)
{
    long long old = _InterlockedCompareExchange64((volatile long long *)p, 0, 0);
    for (;;) {
        long long seen = _InterlockedCompareExchange64((volatile long long *)p, old + (long long)v, old);
        if (seen == old)
            return;
        old = seen;
    }
}

uint64_t pglr_cycles(void)
{
    return __rdtsc();
}

/* ======================================================================== */
/* Frame                                                                    */
/* ======================================================================== */

void pglr_begin_frame(void)
{
    g_t.begin = qpc();
    g_frameReadback = false;
    g_recWaitMs = 0.0;
    g_frame.clear();
    g_rec = &g_frame;
    g_recStack.clear();
    g_frameAspect = 0.0f;                   /* the 4:3 picture unless a race view says otherwise */
    g_frameMulti = false;
}

void pglr_set_picture_aspect(float aspect)
{
    g_frameAspect = aspect > 0.0f ? aspect : 0.0f;
}

void pglr_set_picture_multi(int multi)
{
    g_frameMulti = multi != 0;
}

void pglr_set_menu_aspect(float aspect)
{
    g_menuAspect = aspect > 0.0f ? aspect : 0.0f;
}

/* PC (2026-11-13): the blocks of the buffers the game freed and has not
 * allocated again (pglr_buffer_freed / _alloc; the recording thread's). */
static std::vector<uint32_t> g_freedBlocks;

void pglr_buffer_freed(uint32_t baseBlock)
{
    for (uint32_t b : g_freedBlocks)
        if (b == baseBlock)
            return;
    if (g_freedBlocks.size() < 1024)
        g_freedBlocks.push_back(baseBlock);
}

void pglr_buffer_alloc(uint32_t baseBlock)
{
    for (size_t i = 0; i < g_freedBlocks.size(); i++) {
        if (g_freedBlocks[i] == baseBlock) {
            g_freedBlocks[i] = g_freedBlocks.back();
            g_freedBlocks.pop_back();
            return;
        }
    }
}

void pglr_end_frame(void)
{
    /* PC (2026-11-13): the buffers freed in the frame (and still free), after
     * everything that drew into or read them */
    for (uint32_t b : g_freedBlocks) {
        CmdTarget *c = (CmdTarget *)g_frame.add(CMD_TARGET, sizeof(CmdTarget));
        c->base = b;
        c->fbw = 0;
        c->psm = 0;
        c->w = -1;
        c->h = -1;
        c->dx = 0.0f;
        c->dy = 0.0f;
    }
    g_freedBlocks.clear();
    /* PC: a frame without a race view of its own: the menus' stretch, if on */
    if (g_frameAspect <= 0.0f && g_menuAspect > 0.0f)
        g_frameAspect = g_menuAspect;
    if (g_t.begin != 0) {
        const double ms = std::max(0.0, ms_since(g_t.begin) - g_recWaitMs);
        g_t.rec += ms;
        g_t.recMax = std::max(g_t.recMax, ms);
        frame_monitor::add(frame_monitor::PART_RECORD, ms);
    }
    std::swap(g_ready, g_frame);
    g_frame.clear();
    g_readyAspect = g_frameAspect;
    g_readyMulti = g_frameMulti;
    g_readyReadback = g_frameReadback;
    g_readyValid = true;
    g_rec = &g_frame;
    g_recStack.clear();
}

/* ======================================================================== */
/* The executor thread                                                      */
/* ======================================================================== */

/* PC (docs/renderer.md 3, "The executor thread"): a kicked frame's list runs
 * on a thread of its own - pglr_kick hands it over and returns - while the
 * game thread runs the next ticks and records the next frame, as the PS2's
 * DMA ran it while the EE went on.  The game waits for it where it waited
 * for the DMA (gfx_dma_sync_frame, gfx_sync_pipeline: pglr_sync), and so
 * does every entry point here that touches the renderer's state (the
 * presentation, the immediate uploads, a Tex1 set loaded, a sub-list
 * recorded again, a use of the context: exec_wait).  A frame with a
 * readback the game reads (not the flare strip's, read on the GPU) runs at
 * the kick, as before.  GT3PC_EXEC_THREAD=0: no thread. */
static CmdList g_execList;                  /* the frame being executed */
static float g_execAspect;
static bool g_execMulti;
static HANDLE g_execThread, g_execStart;
static bool g_execOn;
static std::atomic<uint32_t> g_execBusy{0};
static thread_local bool t_execThread;
/* The last execution, for its report on the game thread (exec_report) */
static double g_execWallMs, g_execBlockedMs;
static bool g_execReport;

static void execute_frame(void);

static DWORD WINAPI exec_thread_main(void *)
{
    t_execThread = true;
    sample_prof::register_thread("executor");  /* PC: GT3PC_SAMPLE (platform/sample_prof.h) */
    /* Debug aid: GT3PC_TEX_STRESS thread - each list starts 0 .. 8 ms late,
     * the game thread running on meanwhile: whatever the list reads of the
     * game's memory has that much longer to change before it is read (a
     * texture uploaded from bytes the game had moved on from) */
    const bool jitter = stress_on("thread");
    if (jitter)
        pclog::write("pgl: GT3PC_TEX_STRESS; thread: each frame's list starts 0 .. 8 ms late");
    uint32_t seed = 0x2545F491u;
    for (;;) {
        WaitForSingleObject(g_execStart, INFINITE);
        if (jitter) {
            seed = seed * 1664525u + 1013904223u;
            Sleep((seed >> 16) % 9);
        }
        {
            fpu::DefaultFpuMode fpuMode;
            execute_frame();
        }
        g_execBusy.store(0, std::memory_order_release);
        WakeByAddressAll((void *)&g_execBusy);
    }
}

/* The last execution into the frame monitor and the stats (the game
 * thread): on the thread, EXECUTE is the game thread's wait for it (what
 * the frame waited; its own time is EXEC THREAD), else its time. */
static void exec_report(double waitedMs, bool threaded)
{
    if (!g_execReport)
        return;
    g_execReport = false;
    g_t.exec += g_execWallMs;
    g_t.execMax = std::max(g_t.execMax, g_execWallMs);
    g_t.n++;
    if (threaded) {
        frame_monitor::add(frame_monitor::PART_EXECUTE, waitedMs);
        frame_monitor::add(frame_monitor::PART_WAIT, std::min(waitedMs, g_execBlockedMs));
        frame_monitor::exec_thread((float)g_execWallMs);
    } else {
        frame_monitor::add(frame_monitor::PART_EXECUTE, g_execWallMs);
        frame_monitor::add(frame_monitor::PART_WAIT, g_execBlockedMs);
    }
    if (g_t.every < 0) {
        const char *e = getenv("GT3PC_PGL_STATS");
        g_t.every = (e != nullptr && atoi(e) > 0) ? atoi(e) : 600;
    }
    if (g_t.n >= g_t.every) {
        const double n = (double)g_t.n;
        pclog::write("pgl time (ms, mean / max over %d frames): frame %.2f / %.2f, record %.2f / %.2f,"
                     " execute %.2f / %.2f, present %.2f / %.2f, GPU %.2f / %.2f", g_t.n, g_t.period / n,
                     g_t.periodMax, g_t.rec / n, g_t.recMax, g_t.exec / n, g_t.execMax, g_t.present / n,
                     g_t.presentMax, g_t.gpuN ? g_t.gpu / g_t.gpuN : 0.0, g_t.gpuMax);
        g_t.rec = g_t.exec = g_t.present = g_t.period = g_t.gpu = 0;
        g_t.recMax = g_t.execMax = g_t.presentMax = g_t.periodMax = g_t.gpuMax = 0;
        g_t.n = g_t.gpuN = 0;
    }
}

/* The game thread waits until the executor has run what it was handed
 * (nothing on the executor itself, or without it). */
static void exec_wait(void)
{
    if (!g_execOn || t_execThread)
        return;
    double waited = 0.0;
    if (g_execBusy.load(std::memory_order_acquire) != 0) {
        const int64_t w0 = qpc();
        for (int spins = 0;; spins++) {
            uint32_t v = g_execBusy.load(std::memory_order_acquire);
            if (v == 0)
                break;
            if (spins < 2000) {
                _mm_pause();
                continue;
            }
            WaitOnAddress((volatile void *)&g_execBusy, &v, sizeof(v), 50);
        }
        waited = ms_since(w0);
        g_recWaitMs += waited;
    }
    exec_report(waited, true);
}

/* gpu.h's context guard: the executor, then the D3D thread, done (the
 * executor's own uses only the latter). */
static void context_guard(void)
{
    exec_wait();
    pglr::q::sync();
}

static void exec_start(void)
{
    const char *e = getenv("GT3PC_EXEC_THREAD");
    if (e != nullptr && atoi(e) == 0) {
        pclog::write("pgl: no executor thread (GT3PC_EXEC_THREAD=0): a frame executes at the kick");
        return;
    }
    g_execStart = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_execStart != nullptr)
        g_execThread = CreateThread(nullptr, 0, exec_thread_main, nullptr, 0, nullptr);
    if (g_execThread == nullptr) {
        pclog::write("pgl: the executor thread could not start: a frame executes at the kick");
        return;
    }
    SetThreadPriority(g_execThread, THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(g_execThread, L"pgl executor");
    g_execOn = true;
    pclog::write("pgl: the executor thread runs each frame's list while the game thread goes on");
}

void pglr_kick(void)
{
    static thread_local bool sampled = false;   /* PC: the recording thread (GT3PC_SAMPLE) */
    if (!sampled) {
        sampled = true;
        sample_prof::register_thread("game (recording)");
    }
    exec_wait();                            /* (the frame before done) */
    if (!g_readyValid)
        return;
    fpu::DefaultFpuMode fpuMode;
    set_game_fpu_mode(fpuMode.saved());
    const int64_t t0 = qpc();
    if (g_t.lastKick != 0) {
        const double period = (double)(t0 - g_t.lastKick) * 1000.0 / g_t.freq;
        g_t.period += period;
        g_t.periodMax = std::max(g_t.periodMax, period);
    }
    g_t.lastKick = t0;
    std::swap(g_execList, g_ready);
    g_ready.clear();
    g_readyValid = false;
    g_execAspect = g_readyAspect;
    g_execMulti = g_readyMulti;
    if (g_execOn && !g_readyReadback) {
        g_execBusy.store(1, std::memory_order_release);
        SetEvent(g_execStart);
        return;
    }
    execute_frame();
    exec_report(0.0, false);
}

/* A frame's list executed (the executor thread, or the kick's). */
static void execute_frame(void)
{
    const int64_t t0 = qpc();
    const uint64_t cyc0 = thread_cycles();
    g_readbackWait = 0.0;

    /* GPU time: the oldest query set first (non-blocking) */
    Timing::Q &q = g_t.q[g_t.qi];
    if (q.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, b = 0;
        if (dc()->GetData(q.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            dc()->GetData(q.t0.get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            dc()->GetData(q.t1.get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (!dj.Disjoint && dj.Frequency != 0) {
                const double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
                g_t.gpu += ms;
                g_t.gpuMax = std::max(g_t.gpuMax, ms);
                g_t.gpuN++;
                frame_monitor::gpu_time(q.seq, ms);
            }
        }
        q.pending = false;
    }
    if (!q.disjoint) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev()->CreateQuery(&d, q.disjoint.put());
        d.Query = D3D11_QUERY_TIMESTAMP;
        dev()->CreateQuery(&d, q.t0.put());
        dev()->CreateQuery(&d, q.t1.put());
    }
    const bool timed = q.disjoint && q.t0 && q.t1;

    targets_frame_settings();               /* the anti-aliasing chosen (MSAA: the targets' samples) */
    targets_frame_aspect(g_execAspect, g_execMulti);    /* the targets for the frame's picture */
    exec_begin_frame();                     /* (the GPU sections' results read first: before anything is queued) */
    if (timed) {
        ID3D11Query *dj0 = q.disjoint.get(), *t0q = q.t0.get();
        pglr::q::run([dj0, t0q] {
            qdc()->Begin(dj0);
            qdc()->End(t0q);
        });
    }
    exec_list(g_execList);
    exec_end_frame();
    g_execList.clear();

    /* (queued: the D3D thread goes on with the frame while the game thread
     * goes on with the next ticks; the next use of the context waits for
     * it, pgl_queue.h) */
    ID3D11Query *t1 = timed ? q.t1.get() : nullptr, *dj = timed ? q.disjoint.get() : nullptr;
    if (timed) {
        q.pending = true;
        q.seq = frame_monitor::frame_seq();
        g_t.qi = (g_t.qi + 1) & 3;
    }
    /* hand the frame to the GPU now (it would otherwise wait in the
     * driver's buffer until the next Present, a frame later) */
    pglr::q::run([t1, dj] {
        if (t1 != nullptr) {
            qdc()->End(t1);
            qdc()->End(dj);
        }
        qdc()->Flush();
    });
    /* the execution's time, and the part of it the thread did not run -
     * blocked: the driver holding a call while the GPU is behind (with vsync
     * off Present does not wait: the next frame's calls do), a readback (its
     * wait measured, if longer) - reported on the game thread (exec_report) */
    const double ms = ms_since(t0);
    const double cpu = cycles_ms(thread_cycles() - cyc0);
    double wait = cpu >= 0.0 ? ms - cpu : 0.0;
    wait = std::min(ms, std::max({wait, g_readbackWait, 0.0}));
    g_execWallMs = ms;
    g_execBlockedMs = wait;
    g_execReport = true;
}

void pglr_sync(void)
{
    exec_wait();                            /* PC: the executor thread (the PS2's DMA) done */
}

void pglr_target_stats(int *alive, unsigned *created, unsigned *reused, unsigned *retired)
{
    exec_wait();
    target_stats(alive, created, reused, retired);
}

void pglr_display(uint32_t fbBlock, uint32_t psm, uint32_t fbw, int dx, int dy, int dw, int dh)
{
    exec_wait();
    fpu::DefaultFpuMode fpuMode;
    const int64_t t0 = qpc();
    frame_monitor::output_begin();          /* (OUTPUT: to Present, gpu::present) */
    present_target(fbBlock, psm, fbw, dx, dy, dw, dh);
    const double ms = ms_since(t0);
    g_t.present += ms;
    g_t.presentMax = std::max(g_t.presentMax, ms);
}

void pglr_display_enable(int on)
{
    exec_wait();
    fpu::DefaultFpuMode fpuMode;
    if (on)
        present_last();
    else
        present_black();
}

void pglr_present_again(void)
{
    exec_wait();
    fpu::DefaultFpuMode fpuMode;
    present_again();
}

/* ======================================================================== */
/* Recording                                                                */
/* ======================================================================== */

void pglr_regs(uint64_t mask, const uint64_t *reg, const uint64_t *gen)
{
    CmdRegs *c = (CmdRegs *)g_rec->add(CMD_REGS, sizeof(CmdRegs));
    c->mask = mask;
    memcpy(c->reg, reg, sizeof(c->reg));
    memcpy(c->gen, gen, sizeof(c->gen));
    c->key = cmd_regs_key(mask, reg, gen);  /* PC: (the resolved runs' key, worked out here) */
    g_rec->set_last_key(c->key);
}

void pglr_ad(uint32_t gsReg, uint64_t value)
{
    CmdAd *c = (CmdAd *)g_rec->add(CMD_AD, sizeof(CmdAd));
    c->addr = gsReg;
    c->value = value;
    g_rec->set_last_key(cmd_ad_key(gsReg, value));    /* PC (2026-11-06): (the resolved runs' key, worked out here) */
}

void pglr_target(uint32_t baseBlock, uint32_t fbw, uint32_t psm, int width, int height)
{
    CmdTarget *c = (CmdTarget *)g_rec->add(CMD_TARGET, sizeof(CmdTarget));
    c->base = baseBlock;
    c->fbw = fbw;
    c->psm = psm;
    c->w = width;
    c->h = height;
    c->dx = 0.0f;
    c->dy = 0.0f;
}

/* PC (2026-11-14): pgl_bridge.h */
void pglr_target_detail(uint32_t baseBlock, int width, int height, float dx, float dy)
{
    CmdTarget *c = (CmdTarget *)g_rec->add(CMD_TARGET, sizeof(CmdTarget));
    c->base = baseBlock;
    c->fbw = 0;
    c->psm = 0;
    c->w = width;
    c->h = height;
    c->dx = dx > 0.0f ? dx : 1.0f;
    c->dy = dy > 0.0f ? dy : 1.0f;
}

void pglr_prim(uint32_t prim, const pgl::prim::GsVertex *v, int n, uint32_t flags)
{
    if (n <= 0)
        return;
    CmdPrim *c = (CmdPrim *)g_rec->add(CMD_PRIM, sizeof(CmdPrim) + sizeof(pgl::prim::GsVertex) * n);
    c->prim = prim;
    c->flags = flags;
    c->n = n;
    memcpy(c + 1, v, sizeof(pgl::prim::GsVertex) * n);
}

void pglr_gif(const void *data, uint32_t qwc, uint32_t flags)
{
    CmdGif *c = (CmdGif *)g_rec->add(CMD_GIF, sizeof(CmdGif));
    c->data = data;
    c->qwc = qwc;
    c->flags = flags;
}

void pglr_strip(const PglrVu1 *vu, uint32_t prim, const float *xyz, const float *st, const uint32_t *rgba,
                uint32_t constRgba, int n, const uint8_t *lengths, int nStrips, uint32_t flags)
{
    if (n < 3 || xyz == nullptr)
        return;
    CmdStrip *c = (CmdStrip *)g_rec->add(CMD_STRIP, sizeof(CmdStrip));
    c->vu = *vu;
    c->prim = prim;
    c->xyz = xyz;
    c->st = st;
    c->rgba = rgba;
    c->constRgba = constRgba;
    c->n = n;
    c->flags = flags;
    if (lengths != nullptr && nStrips > 0) {
        if (nStrips > 64)
            nStrips = 64;
        c->nStrips = nStrips;
        memcpy(c->lengths, lengths, nStrips);
    } else {
        c->nStrips = 1;
        c->lengths[0] = (uint8_t)n;
    }
}

void pglr_tri16(const PglrVu1 *vu, uint32_t prim, const int16_t *xyzw, const int16_t *st, const uint32_t *rgba,
                int nTris, uint32_t flags)
{
    CmdTri16 *c = (CmdTri16 *)g_rec->add(CMD_TRI16, sizeof(CmdTri16));
    c->vu = *vu;
    c->prim = prim;
    c->xyzw = xyzw;
    c->st = st;
    c->rgba = rgba;
    c->nTris = nTris;
    c->flags = flags;
}

void pglr_model_part(const PglrVu1 *vu, const PglrModelPart *part, uint32_t flags)
{
    CmdModel *c = (CmdModel *)g_rec->add(CMD_MODEL, sizeof(CmdModel));
    c->vu = *vu;
    c->part = *part;
    c->flags = flags;
    c->pad = 0;
    c->key = cmd_model_key(*vu, *part, flags);  /* PC: (the resolved runs' key, worked out here) */
    g_rec->set_last_key(c->key);
}

void pglr_upload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, int dx, int dy, int w, int h, const void *data,
                 int immediate)
{
    if (immediate) {
        exec_wait();                        /* PC: (the registry: the executor thread's) */
        fpu::DefaultFpuMode fpuMode;
        registry_upload(dbp, dbw, dpsm, dx, dy, w, h, data,
                        gstex::image_bytes(dpsm, (uint32_t)w, (uint32_t)h));
        return;
    }
    CmdUpload *c = (CmdUpload *)g_rec->add(CMD_UPLOAD, sizeof(CmdUpload));
    c->dbp = dbp;
    c->dbw = dbw;
    c->dpsm = dpsm;
    c->dx = dx;
    c->dy = dy;
    c->w = w;
    c->h = h;
    c->data = data;
}

/* tex1_upload_packet wrote each transfer's BITBLTBUF (DBP) into the packet
 * when it was called and only referenced the pixels; the set can be linked
 * for another block (tex1_set_vram_base) before the list executes - the
 * VRAM allocator evicts and reloads sets within a frame, TexSet::load moves
 * a wheel face for every wheel - so the base is taken now. */
void pglr_tex1_upload(const void *tex1Image, int immediate)
{
    const uint32_t base = registry_tex1_base(tex1Image);
    if (immediate) {
        exec_wait();                        /* PC: (the registry: the executor thread's) */
        fpu::DefaultFpuMode fpuMode;
        registry_tex1_upload(tex1Image, base, nullptr);
        return;
    }
    /* PC: the set's header and tables as they are now, with the base (the
     * executor reads them a frame later; the game relinks them in place) */
    const size_t n = registry_tex1_copy_size(tex1Image);
    CmdTex1 *c = (CmdTex1 *)g_rec->add(CMD_TEX1_UPLOAD, sizeof(CmdTex1) + n);
    c->img = tex1Image;
    c->vramBase = base;
    c->copyBytes = (uint32_t)n;
    registry_tex1_copy(tex1Image, c + 1);
}

void pglr_texture_source(const void *tex1Image, uint32_t vramBase, uint64_t tex0)
{
    const size_t n = registry_tex1_copy_size(tex1Image);   /* (as pglr_tex1_upload's) */
    CmdTexSource *c = (CmdTexSource *)g_rec->add(CMD_TEX_SOURCE, sizeof(CmdTexSource) + n);
    c->img = tex1Image;
    c->vramBase = vramBase;
    c->tex0 = tex0;
    c->copyBytes = (uint32_t)n;
    registry_tex1_copy(tex1Image, c + 1);
}

void pglr_tex1_loaded(void *tex1Image)
{
    exec_wait();                            /* PC: (the registry: the executor thread's) */
    registry_tex1_loaded(tex1Image);
}

void pglr_model_written(const void *model)
{
    models_written(model);
}

void pglr_model_loaded(const void *model)
{
    models_loaded(model);
}

void pglr_modelset_loaded(const void *const *packets, int n)
{
    models_set_loaded(packets, n);
}

static bool g_nextReadbackFlare;            /* pglr_readback_flare */

void pglr_readback_flare(void)
{
    g_nextReadbackFlare = true;
}

void pglr_readback(uint32_t sbp, uint32_t sbw, uint32_t spsm, int sx, int sy, int w, int h, void *dst,
                   PglrReadbackFunc callback, uint32_t madr, uint32_t arg)
{
    CmdReadback *c = (CmdReadback *)g_rec->add(CMD_READBACK, sizeof(CmdReadback));
    c->flags = g_nextReadbackFlare ? READBACK_FLARE : 0;
    g_nextReadbackFlare = false;
    /* PC: a readback the game reads (or calls back for) executes the frame
     * at the kick (the flare strip's is read on the GPU, unless logged) */
    if (!(c->flags & READBACK_FLARE) || callback != nullptr || flare_log(false))
        g_frameReadback = true;
    c->sbp = sbp;
    c->sbw = sbw;
    c->spsm = spsm;
    c->sx = sx;
    c->sy = sy;
    c->w = w;
    c->h = h;
    c->dst = dst;
    c->cb = callback;
    c->madr = madr;
    c->arg = arg;
}

void pglr_sub_begin(const void *header)
{
    exec_wait();                            /* PC: (the sub-lists: the executor thread may call this one) */
    std::unique_ptr<CmdList> &l = g_subs[header];
    if (!l)
        l.reset(new CmdList());
    l->clear();
    g_recStack.push_back(g_rec);
    g_rec = l.get();
}

void pglr_sub_end(void)
{
    if (g_recStack.empty())
        return;
    g_rec = g_recStack.back();
    g_recStack.pop_back();
}

void pglr_sub_call(const void *header)
{
    CmdSubcall *c = (CmdSubcall *)g_rec->add(CMD_SUBCALL, sizeof(CmdSubcall));
    c->key = header;
}

void pglr_pfx_color_correction(uint32_t srcBlock, uint32_t dstBlock, uint32_t fbw, int w, int h,
                               const uint8_t *curve, uint32_t colR, uint32_t colG, uint32_t colB)
{
    CmdPfxColor *c = (CmdPfxColor *)g_rec->add(CMD_PFX_COLOR, sizeof(CmdPfxColor));
    c->src = srcBlock;
    c->dst = dstBlock;
    c->fbw = fbw;
    c->w = w;
    c->h = h;
    c->colR = colR;
    c->colG = colG;
    c->colB = colB;
    memcpy(c->curve, curve, sizeof(c->curve));
}

void pglr_pfx_split_rgb(uint32_t srcBlock, uint32_t blockR, uint32_t blockG, uint32_t blockB, int w, int h)
{
    CmdPfxSplit *c = (CmdPfxSplit *)g_rec->add(CMD_PFX_SPLIT, sizeof(CmdPfxSplit));
    c->src = srcBlock;
    c->r = blockR;
    c->g = blockG;
    c->b = blockB;
    c->w = w;
    c->h = h;
}

void pglr_flare_query(const PglrFlareQuery *query)
{
    PglrFlareQuery *c = (PglrFlareQuery *)g_rec->add(CMD_FLARE_QUERY, sizeof(PglrFlareQuery));
    *c = *query;
}

void pglr_set_design_scale(float kx, float ky)
{
    CmdDesign *c = (CmdDesign *)g_rec->add(CMD_DESIGN, sizeof(CmdDesign));
    c->kx = kx > 0.0f ? kx : 1.0f;
    c->ky = ky > 0.0f ? ky : 1.0f;
}

void pglr_set_line_width(float px)
{
    CmdLineWidth *c = (CmdLineWidth *)g_rec->add(CMD_LINE_WIDTH, sizeof(CmdLineWidth));
    c->px = px > 1.0f ? px : 1.0f;
}

void pglr_post_aa(uint32_t block, int w, int h)
{
    CmdPostAa *c = (CmdPostAa *)g_rec->add(CMD_POST_AA, sizeof(CmdPostAa));
    c->base = block;
    c->w = w;
    c->h = h;
}

void pglr_half_blend(int begin, uint32_t block, uint32_t fbw, uint32_t psm, int w, int h)
{
    CmdHalfBlend *c = (CmdHalfBlend *)g_rec->add(CMD_HALF_BLEND, sizeof(CmdHalfBlend));
    c->begin = begin;
    c->base = block;
    c->fbw = fbw;
    c->psm = psm;
    c->w = w;
    c->h = h;
}

void pglr_gpu_section(int id)
{
    CmdSection *c = (CmdSection *)g_rec->add(CMD_GPU_SECTION, sizeof(CmdSection));
    c->id = id;
}

void pglr_log_once(const char *key, const char *message)
{
    log_once(key, "%s", message);
}

}  /* extern "C" */
