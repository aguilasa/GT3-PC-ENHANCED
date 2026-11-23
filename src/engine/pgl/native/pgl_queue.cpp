/* pgl_queue.cpp - the renderer's D3D thread and submit thread (pgl_queue.h).
 *
 * One producer at a time per ring (the game thread executing a frame; game
 * threads run one at a time, platform/scheduler.h; PC 2026-11-05: the submit
 * thread for the D3D thread's ring while a frame pipelines) and one
 * consumer: positions in the ring only grow (an index is the position modulo
 * its size).  The producer publishes its records in batches (head), the
 * consumer runs them and publishes how far it got after each (tail; the
 * producer reads it only when the ring seems full or at a sync).  Idle, the
 * consumer polls a little, then sleeps on head (WaitOnAddress); a sync or a
 * full ring waits likewise on tail. */
#include "engine/pgl/native/pgl_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "engine/pgl/native/pgl_intrin.h"
#include "engine/pgl/native/pgl_win32.h"
#include <unknwn.h>

#include "platform/crash.h"
#include "platform/gpu.h"
#include "platform/log.h"
#include "platform/sample_prof.h"          /* PC: GT3PC_SAMPLE */
#include "platform/settings.h"

#pragma comment(lib, "Synchronization.lib")

namespace pglr {

namespace {

constexpr int kSpin = 4000;                 /* polls before sleeping */

int64_t qpc()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double g_qpcFreq;

/* PC (2026-11-05): a ring and its consumer thread (the D3D thread's, the
 * submit thread's: the same mechanism) */
struct Ring {
    q::Producer *prod;
    uint64_t size;
    const char *what;                           /* (the log's name) */
    alignas(64) std::atomic<uint64_t> head{0};  /* published by the producer */
    alignas(64) std::atomic<uint64_t> tail{0};  /* run by the consumer */
    alignas(64) std::atomic<int> sleeping{0};   /* the consumer sleeps on head */
    std::atomic<int> waiting{0};                /* threads asleep on tail (the producer; PC: the submit workers) */
    std::atomic<bool> stop{false};
    std::atomic<int64_t> busy{0};               /* QPC ticks the consumer ran calls */
    HANDLE thread = nullptr;
    uint32_t waits = 0;                         /* (the producer's waits for the consumer) */
    int64_t waitTicks = 0;
};

void publish(Ring &r)
{
    r.prod->pending = 0;
    r.head.store(r.prod->head, std::memory_order_seq_cst);
    if (r.sleeping.load(std::memory_order_seq_cst) != 0)
        WakeByAddressSingle((void *)&r.head);
}

/* The producer waits until the consumer has run up to `want'. */
void wait_tail(Ring &r, uint64_t want)
{
    publish(r);
    int spins = 0;
    for (;;) {
        uint64_t t = r.tail.load(std::memory_order_acquire);
        if (t >= want) {
            r.prod->tailSeen = t;
            return;
        }
        if (spins < kSpin) {
            _mm_pause();
            spins++;
            continue;
        }
        r.waiting.fetch_add(1, std::memory_order_seq_cst);
        t = r.tail.load(std::memory_order_seq_cst);
        if (t < want)
            WaitOnAddress((volatile void *)&r.tail, &t, sizeof(t), 50);
        r.waiting.fetch_sub(1, std::memory_order_seq_cst);
    }
}

/* PC (2026-11-05): another thread (a submit worker) waits until the consumer
 * has run up to `want' (nothing published here: not the producer) */
void wait_processed(Ring &r, uint64_t want)
{
    int spins = 0;
    for (;;) {
        uint64_t t = r.tail.load(std::memory_order_acquire);
        if (t >= want)
            return;
        if (spins < kSpin) {
            _mm_pause();
            spins++;
            continue;
        }
        r.waiting.fetch_add(1, std::memory_order_seq_cst);
        t = r.tail.load(std::memory_order_seq_cst);
        if (t < want)
            WaitOnAddress((volatile void *)&r.tail, &t, sizeof(t), 50);
        r.waiting.fetch_sub(1, std::memory_order_seq_cst);
    }
}

void *reserve_slow(Ring &r, uint64_t need)
{
    q::Producer &p = *r.prod;
    if (need > r.size / 4)
        pclog::fatal("pgl: a queued call of %llu bytes (the %s's queue holds %llu)", (unsigned long long)need,
                     r.what, (unsigned long long)r.size);
    uint64_t idx = p.head & (r.size - 1);
    const uint64_t pad = idx + need > r.size ? r.size - idx : 0;
    if (p.head + pad + need - p.tailSeen > r.size) {
        /* full as last seen: how far the consumer is now, or wait */
        p.tailSeen = r.tail.load(std::memory_order_acquire);
        if (p.head + pad + need - p.tailSeen > r.size) {
            r.waits++;
            const int64_t t0 = qpc();
            wait_tail(r, p.head + pad + need - r.size);
            r.waitTicks += qpc() - t0;
        }
    }
    if (pad != 0) {
        /* the ring wraps: the rest of it skipped */
        q::Rec *w = reinterpret_cast<q::Rec *>(p.buf + idx);
        w->fn = nullptr;
        w->size = (uint32_t)pad;
        w->data = 0;
        p.head += pad;
        idx = 0;
    }
    q::Rec *rec = reinterpret_cast<q::Rec *>(p.buf + idx);
    rec->size = (uint32_t)need;
    rec->data = 0;
    return rec;
}

/* The consumer's loop (its thread): the records run in order, the tail
 * published after each; idle, a short poll then a sleep on the head. */
void consume(Ring &r)
{
    uint64_t tail = 0;
    int64_t busyFrom = 0;
    for (;;) {
        uint64_t head = r.head.load(std::memory_order_acquire);
        if (head == tail) {
            if (busyFrom != 0) {
                r.busy.fetch_add(qpc() - busyFrom, std::memory_order_relaxed);
                busyFrom = 0;
            }
            if (r.stop.load(std::memory_order_acquire))
                break;
            for (int i = 0; i < kSpin && (head = r.head.load(std::memory_order_acquire)) == tail; i++)
                _mm_pause();
            if (head == tail) {
                r.sleeping.store(1, std::memory_order_seq_cst);
                head = r.head.load(std::memory_order_seq_cst);
                if (head == tail && !r.stop.load(std::memory_order_seq_cst))
                    WaitOnAddress((volatile void *)&r.head, &tail, sizeof(tail), 100);
                r.sleeping.store(0, std::memory_order_seq_cst);
                continue;
            }
        }
        if (busyFrom == 0)
            busyFrom = qpc();
        while (tail != head) {
            q::Rec *rec = reinterpret_cast<q::Rec *>(r.prod->buf + (tail & (r.size - 1)));
            const uint32_t size = rec->size;
            if (rec->fn != nullptr)
                rec->fn(rec);
            tail += size;
            r.tail.store(tail, std::memory_order_release);
            /* PC (2026-11-05): a thread asleep on the tail woken as it moves,
             * not only when the ring runs empty - a submit worker waiting for
             * a position (sq::wait_processed) slept to its timeout while the
             * consumer went on with a ring that never emptied mid-frame, and
             * the submit thread waited for the worker: a stall per run (user
             * report: stutters, gone with the verbose view's timing) */
            if (r.waiting.load(std::memory_order_seq_cst) != 0)
                WakeByAddressAll((void *)&r.tail);
        }
        if (r.waiting.load(std::memory_order_seq_cst) != 0)
            WakeByAddressAll((void *)&r.tail);
    }
}

/* The ring's memory and thread (false: neither; the reason logged) */
bool ring_start(Ring &r, LPTHREAD_START_ROUTINE main, const wchar_t *name, const char *fallback)
{
    r.prod->buf = static_cast<uint8_t *>(VirtualAlloc(nullptr, (SIZE_T)r.size, MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_READWRITE));
    if (r.prod->buf == nullptr) {
        pclog::write("pgl: no memory for the %s's queue: %s", r.what, fallback);
        return false;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;
    r.thread = CreateThread(nullptr, 0, main, nullptr, 0, nullptr);
    if (r.thread == nullptr) {
        pclog::write("pgl: the %s could not start: %s", r.what, fallback);
        return false;
    }
    SetThreadPriority(r.thread, GetThreadPriority(GetCurrentThread()));
    SetThreadDescription(r.thread, name);
    r.prod->running = true;
    return true;
}

void ring_stop(Ring &r)
{
    r.stop.store(true, std::memory_order_seq_cst);
    WakeByAddressSingle((void *)&r.head);
    WaitForSingleObject(r.thread, 2000);
    r.prod->running = false;
}

void ring_take_stats(Ring &r, double *busyMs, double *waitMs, uint32_t *waits)
{
    const int64_t ticks = r.busy.exchange(0, std::memory_order_relaxed);
    *busyMs = g_qpcFreq > 0.0 ? (double)ticks * 1000.0 / g_qpcFreq : 0.0;
    *waitMs = g_qpcFreq > 0.0 ? (double)r.waitTicks * 1000.0 / g_qpcFreq : 0.0;
    *waits = r.waits;
    r.waits = 0;
    r.waitTicks = 0;
}

}  /* namespace */

/* ======================================================================== */
/* The D3D thread                                                           */
/* ======================================================================== */

namespace q {

thread_local bool t_d3dThread = false;
thread_local bool t_forward = false;
Producer g_prod;
thread_local void (*g_held)() = nullptr;
thread_local bool t_holder = false;
thread_local Capture *t_capture = nullptr;  /* PC (2026-11-05): (the submit workers) */

void *capture_reserve(size_t bytes)
{
    Capture &c = *t_capture;
    const size_t blocks = ((bytes + 15) & ~(size_t)15) / 16;
    const size_t at = c.buf.size();
    c.buf.resize(at + blocks);
    Rec *r = reinterpret_cast<Rec *>(&c.buf[at]);
    r->size = (uint32_t)(blocks * 16);
    r->data = 0;
    return r;
}

namespace {

Ring g_ring = {&g_prod, kSize, "D3D thread"};
int g_syncLog = -1;

/* GT3PC_SYNC_LOG=1 (or the launcher's Debug tab, [Debug] SyncLog): who made
 * the executor wait for the D3D thread, each call chain once (at most 64). */
void log_sync_caller()
{
    if (g_syncLog < 0) {
        const char *e = getenv("GT3PC_SYNC_LOG");
        g_syncLog = (e != nullptr && atoi(e) > 0) || settings::debug_sync_log() ? 1 : 0;
        if (g_syncLog)
            pclog::write("pgl: logging the waits for the D3D thread (pgl sync: ..., each caller once)");
    }
    if (g_syncLog == 0)
        return;
    static std::unordered_set<uint64_t> seen;
    void *frames[8] = {};
    const USHORT n = CaptureStackBackTrace(2, 8, frames, nullptr);
    uint64_t h = 0xCBF29CE484222325ull;
    for (USHORT i = 0; i < n; i++)
        h = (h ^ (uint64_t)(uintptr_t)frames[i]) * 0x100000001B3ull;
    if (seen.size() >= 64 || !seen.insert(h).second)
        return;
    char line[1024];
    size_t used = (size_t)snprintf(line, sizeof(line), "pgl sync:");
    for (USHORT i = 0; i < n && i < 6 && used < sizeof(line) - 8; i++) {
        char name[160];
        crash::describe_address(frames[i], name, sizeof(name));
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s %s", i ? " <" : "", name);
    }
    pclog::write("%s", line);
}

DWORD WINAPI d3d_thread(void *)
{
    t_d3dThread = true;
    sample_prof::register_thread("D3D");    /* PC: GT3PC_SAMPLE (platform/sample_prof.h) */
    consume(g_ring);
    return 0;
}

}  /* namespace */

void start()
{
    if (g_prod.running)
        return;
    const char *e = getenv("GT3PC_D3D_THREAD");
    if (e != nullptr && atoi(e) == 0) {
        pclog::write("pgl: the D3D thread off (GT3PC_D3D_THREAD=0): the executor makes its Direct3D calls itself");
        return;
    }
    if (!ring_start(g_ring, d3d_thread, L"pgl D3D", "the executor makes its Direct3D calls itself"))
        return;
    gpu::set_context_guard(&sync);
    atexit(&stop);
    pclog::write("pgl: the D3D thread runs the Direct3D calls (a %u MB queue)", (unsigned)(kSize >> 20));
}

void stop()
{
    if (!g_prod.running || t_d3dThread)
        return;
    sync();
    ring_stop(g_ring);
}

void publish()
{
    pglr::publish(g_ring);
}

void *reserve_slow(uint64_t need)
{
    return pglr::reserve_slow(g_ring, need);
}

void sync()
{
    release_held();                         /* PC: (a held draw goes before the context's other use) */
    if (!g_prod.running || t_d3dThread)
        return;
    /* PC (2026-11-05): the submit thread first - the executor's calls go
     * through it while the frame pipelines, and it holds the last draw */
    if (!sq::t_thread)
        sq::sync();
    if (g_prod.pending != 0)
        publish();
    const uint64_t t = g_ring.tail.load(std::memory_order_acquire);
    g_prod.tailSeen = t;
    if (t == g_prod.head)
        return;
    g_ring.waits++;
    log_sync_caller();
    const int64_t t0 = qpc();
    wait_tail(g_ring, g_prod.head);
    g_ring.waitTicks += qpc() - t0;
}

void take_stats(double *busyMs, double *syncMs, uint32_t *syncs)
{
    ring_take_stats(g_ring, busyMs, syncMs, syncs);
}

/* PC (2026-11-05, the submit workers): captured records appended to the ring
 * as they are - in pieces of up to 1 MB at records' edges (a reservation
 * must fit the ring's quarter), each a reservation as any record's */
void splice(const void *recs, size_t bytes, uint32_t count)
{
    (void)count;
    if (bytes == 0)
        return;
    release_held();
    const uint8_t *p = static_cast<const uint8_t *>(recs);
    if (!running() || on_d3d_thread()) {
        for (size_t at = 0; at < bytes;) {  /* (no D3D thread: run here, in order) */
            Rec *r = reinterpret_cast<Rec *>(const_cast<uint8_t *>(p) + at);
            const uint32_t size = r->size;
            if (r->fn != nullptr)
                r->fn(r);
            at += size;
        }
        return;
    }
    size_t at = 0;
    while (at < bytes) {
        size_t end = at;
        uint32_t n = 0;
        while (end < bytes) {
            const Rec *r = reinterpret_cast<const Rec *>(p + end);
            if (end > at && end - at + r->size > (1u << 20))
                break;
            end += r->size;
            n++;
        }
        void *dst = reserve(end - at);
        memcpy(dst, p + at, end - at);
        g_prod.head += end - at;
        g_prod.pending += n;
        if (g_prod.pending >= kBatch)
            publish();
        at = end;
    }
}

namespace {
thread_local bool t_releaseHere;            /* (release_here) */
}  /* namespace */

void release_here()
{
    t_releaseHere = true;
}

bool release_queued(IUnknown *p)
{
    if (!g_prod.running || t_d3dThread || t_releaseHere)
        return false;
    run([p] { p->Release(); });             /* (PC: through the submit thread when forwarding, in order) */
    return true;
}

}  /* namespace q */

/* ======================================================================== */
/* The submit thread (PC, 2026-11-05)                                       */
/* ======================================================================== */

namespace sq {

q::Producer g_prod;
thread_local bool t_thread = false;
bool g_on = false;
void (*g_threadInit)() = nullptr;

namespace {

Ring g_ring = {&g_prod, kSize, "submit thread"};

DWORD WINAPI sub_thread(void *)
{
    t_thread = true;
    sample_prof::register_thread("submit");     /* PC: GT3PC_SAMPLE */
    if (g_threadInit != nullptr)
        g_threadInit();
    consume(g_ring);
    return 0;
}

}  /* namespace */

void start()
{
    if (g_prod.running)
        return;
    const char *e = getenv("GT3PC_PGL_SUBMIT_THREAD");
    if (e != nullptr && atoi(e) == 0) {
        pclog::write("pgl: the submit thread off (GT3PC_PGL_SUBMIT_THREAD=0): the executor submits the draws itself");
        return;
    }
    if (!q::running()) {
        pclog::write("pgl: no submit thread without the D3D thread: the executor submits the draws itself");
        return;
    }
    if (!ring_start(g_ring, sub_thread, L"pgl submit", "the executor submits the draws itself"))
        return;
    atexit(&stop);
    pclog::write("pgl: the submit thread does the draws' submission (a %u MB queue): the executor, it and the D3D"
                 " thread run side by side", (unsigned)(kSize >> 20));
}

void stop()
{
    if (!g_prod.running || t_thread)
        return;
    sync();
    ring_stop(g_ring);
}

void publish()
{
    pglr::publish(g_ring);
}

void *reserve_slow(uint64_t need)
{
    return pglr::reserve_slow(g_ring, need);
}

void sync()
{
    if (!g_on || t_thread || !g_prod.running)
        return;
    run([] { q::release_held(); });         /* (the held draw first, as q::sync's word is) */
    publish();
    const uint64_t t = g_ring.tail.load(std::memory_order_acquire);
    g_prod.tailSeen = t;
    if (t == g_prod.head)
        return;
    g_ring.waits++;
    const int64_t t0 = qpc();
    wait_tail(g_ring, g_prod.head);
    g_ring.waitTicks += qpc() - t0;
}

void take_stats(double *busyMs, double *waitMs, uint32_t *waits)
{
    ring_take_stats(g_ring, busyMs, waitMs, waits);
}

uint64_t processed()
{
    return g_ring.tail.load(std::memory_order_acquire);
}

void wait_processed(uint64_t want)
{
    pglr::wait_processed(g_ring, want);
}

}  /* namespace sq */
}  /* namespace pglr */
