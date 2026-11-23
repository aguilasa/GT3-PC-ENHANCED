/* pgl_prewarm.cpp - PC: what the game loads, decoded ahead of its first
 * draw on a worker thread (docs/renderer.md 7, "Decoding ahead").
 *
 * A model (vif_model_relocate) or a Tex1 set (tex1_relocate) the game loads
 * is copied on the game thread (models_loaded, registry_tex1_loaded) and
 * handed here as a job.  The worker decodes the copy - never the game's
 * memory, which the game goes on changing (a set is linked again for every
 * VRAM block it gets) and may free - and makes its native resources (the
 * device is free-threaded).  What it made waits in the models' and the
 * textures' ready lists until the executor takes it at a draw: a model's
 * mesh when the model's header, part table and data hash as they did when
 * it was copied, a texture by its cache key, the one texture_bind works out
 * at the draw (the set's image and load stamp, the texture's place in it,
 * its palette's content...).  Anything that does not match is decoded at
 * the draw as before.  The worker's objects were never used by a queued
 * call: it releases them at once (q::release_here).
 *
 * One worker below normal priority, the jobs in load order.  At the end of
 * each run of jobs it logs what it decoded and what the draws took since
 * the last line.  GT3PC_PREWARM=0: no worker (everything decoded at the
 * draw). */
#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_render.h"   /* PC (2026-11-07): registry_cache_bytes, models_cache_bytes */

#include <atomic>
#include <cstdlib>
#include <deque>
#include <exception>
#include <new>

#include "engine/pgl/native/pgl_win32.h"

#include "platform/log.h"
#include "platform/memstat.h"               /* PC (2026-11-07): the memory line */

namespace pglr {
namespace pre {

namespace {

/* (Win32 locks, no destructors: the worker may still run while the process
 * exits and the statics go) */
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_wake = CONDITION_VARIABLE_INIT;
std::deque<Job *> *g_jobs;
bool g_on;
double g_freq;

/* the counts since the last line */
std::atomic<uint32_t> g_made[2], g_taken[2], g_late[2];
std::atomic<uint64_t> g_madeBytes[2];
double g_madeMs;

/* (a line once a load of some size is done - a course, a car - not for
 * every menu picture) */
void report(double ms)
{
    g_madeMs += ms;
    if (g_made[MODEL].load() < 32 && g_made[TEXTURE].load() < 64)
        return;
    ms = g_madeMs;
    g_madeMs = 0.0;
    const uint32_t m = g_made[MODEL].exchange(0), t = g_made[TEXTURE].exchange(0);
    const uint64_t mb = g_madeBytes[MODEL].exchange(0), tb = g_madeBytes[TEXTURE].exchange(0);
    const uint32_t mt = g_taken[MODEL].exchange(0), tt = g_taken[TEXTURE].exchange(0);
    const uint32_t ml = g_late[MODEL].exchange(0), tl = g_late[TEXTURE].exchange(0);
    pclog::write("pgl: decoded ahead %u models (%.1f MB) and %u textures (%.1f MB) in %.0f ms; since the last"
                 " line the draws took %u models and %u textures decoded ahead, decoded %u models and %u Tex1"
                 " textures themselves", m, (double)mb / 1048576.0, t, (double)tb / 1048576.0, ms, mt, tt, ml, tl);
    /* PC (2026-11-07): the process's memory after a load (platform/
     * memstat.h: the address space ran out in a long session) and the
     * caches' decoded bytes */
    char mem[256];
    memstat::line(mem, sizeof(mem), true);
    pclog::write("pgl: memory: %s; the caches: textures %u MB, meshes %u MB", mem,
                 (unsigned)(registry_cache_bytes() >> 20), (unsigned)(models_cache_bytes() >> 20));
}

/* PC (2026-11-07): a job's run.  Out of memory (std::bad_alloc from a
 * decode's buffers: the crash of 2026-11-07, 4071 MB of the 4096 in use) or
 * any other exception, the job is dropped - what it would have made is
 * decoded at the draw as before - and logged once each. */
void run_job(Job *job)
{
    try {
        job->run();
    } catch (const std::bad_alloc &) {
        static bool said;
        if (!said) {
            said = true;
            char mem[256];
            memstat::line(mem, sizeof(mem), true);
            pclog::write("pgl: the decoding worker ran out of memory (%s): the job dropped, its models or textures"
                         " decoded at the draw; the next ones quietly", mem);
        }
    } catch (const std::exception &e) {
        static bool said;
        if (!said) {
            said = true;
            pclog::write("pgl: the decoding worker's job failed (%s): dropped; the next ones quietly", e.what());
        }
    }
}

DWORD WINAPI worker(void *)
{
    q::release_here();
    double burst = -1.0;                    /* (when this run of jobs began; < 0: idle) */
    for (;;) {
        AcquireSRWLockExclusive(&g_lock);
        while (g_jobs->empty()) {
            if (burst >= 0.0) {
                ReleaseSRWLockExclusive(&g_lock);
                report((seconds() - burst) * 1000.0);
                burst = -1.0;
                AcquireSRWLockExclusive(&g_lock);
                continue;
            }
            SleepConditionVariableSRW(&g_wake, &g_lock, INFINITE, 0);
        }
        Job *job = g_jobs->front();
        g_jobs->pop_front();
        ReleaseSRWLockExclusive(&g_lock);
        if (burst < 0.0)
            burst = seconds();
        run_job(job);
        delete job;
    }
}

}  /* namespace */

void start()
{
    const char *e = getenv("GT3PC_PREWARM");
    if (e != nullptr && atoi(e) == 0) {
        pclog::write("pgl: nothing decoded ahead (GT3PC_PREWARM=0): models and textures decoded at their first"
                     " draw");
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;
    g_jobs = new std::deque<Job *>();
    HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (t == nullptr) {
        pclog::write("pgl: the decoding worker could not start: models and textures decoded at their first draw");
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(t, L"pgl decode ahead");
    CloseHandle(t);
    g_on = true;
    pclog::write("pgl: models and textures the game loads are decoded ahead on a worker thread");
}

bool on()
{
    return g_on;
}

void post(Job *job)
{
    if (!g_on) {
        delete job;
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_jobs->push_back(job);
    ReleaseSRWLockExclusive(&g_lock);
    WakeConditionVariable(&g_wake);
}

void made(Kind k, uint32_t n, size_t bytes)
{
    g_made[k] += n;
    g_madeBytes[k] += bytes;
}

void taken(Kind k)
{
    g_taken[k].fetch_add(1, std::memory_order_relaxed);
}

void late(Kind k)
{
    g_late[k].fetch_add(1, std::memory_order_relaxed);
}

double seconds()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (g_freq == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)c.QuadPart / (double)f.QuadPart;
    }
    return (double)c.QuadPart / g_freq;
}

}  /* namespace pre */
}  /* namespace pglr */
