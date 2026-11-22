/* pc_physics_threads.cpp - the cars' own part of the frame on native worker
 * threads (pc_physics_threads.h).  No original: new PC code. */
#include "game/race/dynamics/pc_physics_threads.h"
#include "libc.h"                               /* printf */
#include "platform/settings.h"                  /* settings::physics_threads */

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#include <float.h>                              /* __control87_2 */
#else
#include <fpu_control.h>                        /* _FPU_GETCW, _FPU_SETCW */
#endif
#include <stdlib.h>                             /* getenv, atoi */
#include <emmintrin.h>                          /* _mm_pause, _mm_getcsr, _mm_setcsr */

namespace {

enum { kMaxWorkers = 16 };

/* The x87 control word bits a job takes from its caller: the exception
 * masks, the rounding and the precision.  (The SSE unit's MXCSR is copied
 * whole: its rounding, flush-to-zero and denormals-are-zero.) */
#ifdef _MSC_VER
const unsigned int kX87Mask = _MCW_EM | _MCW_RC | _MCW_PC;
#else
/* GCC / Clang: the hardware control word itself.  MSVC's __control87_2
 * reads and writes the x87 word in its own _MCW_* encoding; here the same
 * three fields are taken in the FPU's layout: the exception masks (bits
 * 0-5), the precision control (bits 8-9) and the rounding control (bits
 * 10-11). */
const unsigned int kX87Mask = 0x003Fu | _FPU_EXTENDED | _FPU_RC_ZERO;
#endif

/* The pool's job (one at a time).  The plain fields are written under mtx
 * while no worker is inside a job; a worker copies them under mtx. */
struct Pool {
    std::mutex              mtx;
    std::condition_variable wake;
    unsigned                gen = 0;        /* +1 for every job */
    bool                    open = false;   /* workers may still join the job */
    PcCarJob                job = nullptr;
    void                   *ctx = nullptr;
    int                     last = 0;       /* the end of the cars */
    unsigned int            x87 = 0;        /* the caller's x87 control word (__control87_2's form) */
    unsigned int            csr = 0;        /* its MXCSR, the exception flags cleared */
    std::atomic<int>        next{0};        /* the next car to take */
    std::atomic<int>        left{0};        /* the cars not done */
    std::atomic<int>        inside{0};      /* the workers inside the job */
};

/* Never freed: the workers wait on it until the process ends (a static
 * object's destructor would run under them at exit). */
Pool *g_pool;
int   g_workers = -1;                       /* -1: GT3PC_PHYSICS_THREADS not read yet */

/* This thread in the FPU mode of the job's caller. */
void use_fpu_mode(unsigned int x87, unsigned int csr)
{
    unsigned int cur = 0;

#ifdef _MSC_VER
    __control87_2(x87, kX87Mask, &cur, nullptr);
#else
    /* What __control87_2(x87, kX87Mask, ...) does on MSVC: the masked
     * fields of the x87 word (exception masks, precision, rounding) set
     * to the caller's, the others kept. */
    fpu_control_t cw;

    _FPU_GETCW(cw);
    cur = cw;
    cw = (fpu_control_t)((cur & ~kX87Mask) | (x87 & kX87Mask));
    _FPU_SETCW(cw);
#endif
    _mm_setcsr(csr);
}

/* Takes the job's cars until none is left. */
void take_cars(Pool *p, PcCarJob job, void *ctx, int last)
{
    for (;;) {
        int i = p->next.fetch_add(1, std::memory_order_relaxed);

        if (i >= last)
            break;
        job(ctx, i);
        p->left.fetch_sub(1, std::memory_order_release);
    }
}

/* Until *n is 0: spinning a while (a job is short), then giving the OS
 * thread's time slice away - never the game's scheduler. */
void wait_zero(std::atomic<int> *n)
{
    int spins = 0;

    while (n->load(std::memory_order_acquire) != 0) {
        if (++spins < 4000)
            _mm_pause();
        else
            std::this_thread::yield();
    }
}

void worker_main(Pool *p)
{
    unsigned seen = 0;

    for (;;) {
        PcCarJob job;
        void *ctx;
        int last;
        unsigned int x87, csr;

        {
            std::unique_lock<std::mutex> lock(p->mtx);

            p->wake.wait(lock, [&] { return p->gen != seen; });
            seen = p->gen;
            if (!p->open)
                continue;                   /* done before this worker woke */
            job = p->job;
            ctx = p->ctx;
            last = p->last;
            x87 = p->x87;
            csr = p->csr;
            p->inside.fetch_add(1, std::memory_order_relaxed);
        }
        use_fpu_mode(x87, csr);
        take_cars(p, job, ctx, last);
        p->inside.fetch_sub(1, std::memory_order_release);
    }
}

}  // namespace

int pc_physics_threads(void)
{
    if (g_workers < 0) {
        /* GT3PC_PHYSICS_THREADS, else [Debug] PhysicsThreads (the launcher's
         * Debug tab) */
        const char *e = getenv("GT3PC_PHYSICS_THREADS");
        int n = e != NULL ? atoi(e) : settings::physics_threads();
        const bool automatic = n == settings::kPhysicsThreadsAuto;
        if (automatic) {
            /* auto: every logical core but the one of the game thread,
             * which takes cars too */
            const int cores = (int)std::thread::hardware_concurrency();
            n = cores > 1 ? cores - 1 : 0;
        }

        if (n < 0)
            n = 0;
        if (n > kMaxWorkers)
            n = kMaxWorkers;
        g_workers = n;
        if (n > 0) {
            g_pool = new Pool;
            for (int i = 0; i < n; i++)
                std::thread(worker_main, g_pool).detach();
            printf("physics: %s: %d worker threads for the cars' own steps of a frame%s\n",
                   e != NULL ? "GT3PC_PHYSICS_THREADS" : "[Debug] PhysicsThreads", n,
                   automatic ? " (auto: the logical cores less one, at most 16)" : "");
        }
    }
    return g_workers;
}

void pc_physics_run(PcCarJob job, void *ctx, int first, int last)
{
    Pool *p = g_pool;
    unsigned int x87 = 0;

    if (p == nullptr || last - first < 2) {
        for (int i = first; i < last; i++)
            job(ctx, i);
        return;
    }
#ifdef _MSC_VER
    __control87_2(0, 0, &x87, nullptr);
#else
    {
        /* MSVC's __control87_2(0, 0, &x87, ...): only reads the x87
         * control word. */
        fpu_control_t cw;

        _FPU_GETCW(cw);
        x87 = cw;
    }
#endif
    {
        std::lock_guard<std::mutex> lock(p->mtx);

        p->job = job;
        p->ctx = ctx;
        p->last = last;
        p->x87 = x87 & kX87Mask;
        p->csr = _mm_getcsr() & ~0x3Fu;     /* (the sticky exception flags not passed on) */
        p->next.store(first, std::memory_order_relaxed);
        p->left.store(last - first, std::memory_order_relaxed);
        p->open = true;
        p->gen++;
    }
    p->wake.notify_all();
    take_cars(p, job, ctx, last);
    wait_zero(&p->left);
    {
        std::lock_guard<std::mutex> lock(p->mtx);

        p->open = false;                    /* no worker joins any more */
    }
    wait_zero(&p->inside);                  /* and none is still taking cars */
}
