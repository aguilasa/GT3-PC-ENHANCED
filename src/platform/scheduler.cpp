/* scheduler.cpp - the game threads of GT3 PC (scheduler.h).
 *
 * One mutex guards everything.  g_running is the game thread that holds the
 * CPU; every other game thread waits on its own condition variable until
 * the scheduler hands it the CPU.  Ready queues are per priority, FIFO; a
 * thread preempted by a higher-priority one goes back to the head of its
 * queue (it did not use up its turn), a thread that blocks and wakes again
 * goes to the tail.
 *
 * The timer thread is the interrupt source: vblank start / end at
 * 60000/1001 Hz, alarms, and the ends of DelayThread waits.  Handlers run
 * under the interrupt lock, which a game thread holds while it is inside a
 * DI section; the lock follows the EE thread, not the native one: it is let
 * go when such a thread is switched out and taken back when it resumes
 * (on the EE the interrupt enable is part of each thread's context). */
#include "platform/scheduler.h"
#include "platform/crash.h"                 /* log_stack (log_threads) */
#include "platform/log.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <timeapi.h>
#include <float.h>
#else
#include "platform/fpu.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace sched {
namespace {

constexpr int kPriorities = 128;
constexpr int kMaxThreads = 256;
constexpr int kMaxSemas   = 1024;
constexpr int kMinStack   = 1024 * 1024;      /* game code keeps big arrays on the stack */
constexpr double kVblankPeriod = 1001.0 / 60000.0;
/* PC: VBLANK_END is raised right after VBLANK_START's handlers, not ~22
 * lines (1.4 ms) later.  Its one user, the music thread (priority 6: the
 * sequencer and snd_flush), must run between the vsync and the frame's
 * sound update (RaceEntryCar::updateSound in the render thread's object
 * update), as on the PS2, where the interrupt preempted the render thread
 * (7) at once and the EE took far longer than 1.4 ms to reach the update.
 * Here a thread made ready switches in only at the running one's next
 * switch point, and the render thread runs the frame's D3D work (priority 1)
 * for 0.2 .. 10 ms first: with the delay the flush fell before or after
 * the update by turns, two updates shared one flush (a sound stopped and
 * restarted inside it: the kerb rumble's stutter).  Raised with VBLANK_START
 * the flush runs as soon as the render thread drops to 7, before the
 * update, every frame the render thread waited for the vsync. */
constexpr double kVblankEndDelay = 0.0;
constexpr double kHsyncRate = 15734.264;

struct Thread {
    bool used = false;
    int  id = 0;
    int  status = ST_DORMANT;
    int  prio = 0, initPrio = 0;
    int  waitType = WAIT_NONE, waitId = 0, wakeupCount = 0;
    int  waitResult = 0;
    bool suspended = false;
    void (*entry)(void *) = nullptr;
    void *arg = nullptr;
    int  stackSize = 0;
    int  generation = 0;          /* bumped at each start / termination */
    int  diCount = 0;             /* DI depth of the thread */
    double delayUntil = 0;
#ifdef _WIN32
    HANDLE native = nullptr;      /* its last native thread (log_threads) */
#else
    pthread_t native{};           /* its last native thread (log_threads) */
    bool hasNative = false;
#endif
    std::condition_variable cv;
};

struct Sema {
    bool used = false;
    int  count = 0, max = 0, init = 0;
    std::deque<int> waiters;
};

struct Handler {
    int  id;
    int  cause;
    int  (*fn)(int, void *, void *);
    int  (*plain)(int);
    void *arg;
};

struct Alarm {
    int  id;
    double when;
    uint16_t hsyncs;
    void (*cb)(int, uint16_t, void *);
    void *arg;
};

/* An owner-free lock: DI on one native thread may be released by the
 * scheduler on its behalf when that EE thread is switched out. */
class IntrLock {
public:
    void lock()
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !held_; });
        held_ = true;
    }
    void unlock()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            held_ = false;
        }
        cv_.notify_all();
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    bool held_ = false;
};

std::mutex g_mtx;
#ifdef _WIN32
Thread g_threads[kMaxThreads];
#else
/* Never destroyed (nor g_intr): glibc's pthread_cond_destroy waits for the
 * threads still blocked on the condition, so exit() would hang on the game
 * threads parked in their waits. */
Thread (&g_threads)[kMaxThreads] = *new Thread[1][kMaxThreads];
#endif
Sema g_semas[kMaxSemas];
std::deque<int> g_ready[kPriorities];
int  g_running = 0;
bool g_needResched = false;

#ifdef _WIN32
IntrLock g_intr;
#else
IntrLock &g_intr = *new IntrLock;
#endif
std::mutex g_hmtx;                          /* handlers and alarms */
std::vector<Handler> g_handlers;
bool g_intcEnabled[16];
int  g_nextHandlerId = 1;
std::vector<Alarm> g_alarms;
int  g_nextAlarmId = 1;
#ifdef _WIN32
HANDLE g_timerWake = nullptr;               /* nudges the timer thread */

LARGE_INTEGER g_qpcFreq, g_qpcStart;
#else
/* The timer thread's wake-up: a new alarm or delay sets the flag and
 * signals the condition (CLOCK_MONOTONIC); like the auto-reset event of the
 * Windows build, a nudge given while the timer is not waiting is kept for
 * its next wait. */
pthread_mutex_t g_wakeMtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wakeCv;
bool g_wakePending = false;

int64_t g_startNs;                          /* CLOCK_MONOTONIC at init() */
#endif
std::atomic<uint64_t> g_vblanks{0};
std::atomic<double> g_vblankTime{0.0};     /* now_seconds() of the last vblank */
/* The times of the last 32 vblanks, by their count (vblanks_at). */
std::atomic<double> g_vblankTimes[32];

thread_local Thread *tl_self = nullptr;
thread_local int tl_gen = 0;
/* Set on the timer thread while it runs interrupt handlers / alarms / the
 * ends of DelayThread waits: the threads they make ready are dispatched
 * only when the whole interrupt is done, the highest-priority one first,
 * as the EE kernel does on the return from an interrupt.  (Dispatching the
 * first one woken let the main thread run its frame before the render
 * thread at a vblank: controls read twice or missed.) */
thread_local bool tl_inInterrupt = false;

/* ---- the host ------------------------------------------------------------- */

/* Ends the calling native thread without returning to its caller.  glibc's
 * pthread_exit unwinds the stack (destructors run, catch (...) sees it and
 * must rethrow); the callers hold no lock by then. */
[[noreturn]] void end_native_thread()
{
#ifdef _WIN32
    _endthreadex(0);
#else
    pthread_exit(nullptr);
#endif
    for (;;) {
    }
}

/* Wakes the timer thread to look at the next event again. */
void nudge_timer()
{
#ifdef _WIN32
    SetEvent(g_timerWake);
#else
    pthread_mutex_lock(&g_wakeMtx);
    g_wakePending = true;
    pthread_cond_signal(&g_wakeCv);
    pthread_mutex_unlock(&g_wakeMtx);
#endif
}

/* ---- ready queues -------------------------------------------------------- */

int peek_best()
{
    for (int p = 0; p < kPriorities; p++)
        if (!g_ready[p].empty())
            return g_ready[p].front();
    return 0;
}

int pop_best()
{
    for (int p = 0; p < kPriorities; p++)
        if (!g_ready[p].empty()) {
            int id = g_ready[p].front();
            g_ready[p].pop_front();
            return id;
        }
    return 0;
}

void remove_ready(Thread &t)
{
    auto &q = g_ready[t.prio];
    for (auto it = q.begin(); it != q.end(); ++it)
        if (*it == t.id) {
            q.erase(it);
            return;
        }
}

void dispatch(int id)
{
    g_running = id;
    if (id) {
        Thread &t = g_threads[id];
        t.status = ST_RUN;
        t.cv.notify_all();
    }
}

/* t stops waiting; it runs now if nothing runs, else at the running
 * thread's next switch point (or at once from a game-thread caller, which
 * reschedules after this). */
void make_ready(Thread &t)
{
    t.waitType = WAIT_NONE;
    if (t.suspended) {
        t.status = ST_SUSPEND;
        return;
    }
    t.status = ST_READY;
    g_ready[t.prio].push_back(t.id);
    if (g_running == 0) {
        if (!tl_inInterrupt)
            dispatch(pop_best());
    } else if (t.prio < g_threads[g_running].prio) {
        g_needResched = true;
    }
}

/* The end of an interrupt: the CPU goes to the best ready thread if no
 * game thread holds it (one that does switches at its next switch point,
 * g_needResched). */
void dispatch_after_interrupt()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_running == 0)
        dispatch(pop_best());
}

/* Blocks the calling native thread until its EE thread holds the CPU.  A
 * thread terminated meanwhile ends its native thread here, without
 * unwinding (as the EE abandons a terminated thread's stack). */
void wait_for_cpu(std::unique_lock<std::mutex> &lk, Thread *t, int gen)
{
    t->cv.wait(lk, [&] { return t->generation != gen || g_running == t->id; });
    if (t->generation != gen) {
        lk.unlock();
        end_native_thread();
    }
}

/* The running thread `self' gives up the CPU if it must (its status is no
 * longer ST_RUN) or should (a higher-priority thread is ready), and returns
 * when it holds it again. */
void reschedule(std::unique_lock<std::mutex> &lk)
{
    Thread *self = tl_self;
    if (!self || g_running != self->id)
        return;
    g_needResched = false;
    if (self->status == ST_RUN) {
        int best = peek_best();
        if (best == 0 || g_threads[best].prio >= self->prio)
            return;
        self->status = ST_READY;
        g_ready[self->prio].push_front(self->id);
    }
    dispatch(pop_best());
    if (g_running == self->id)
        return;
    int gen = tl_gen;
    bool hadDI = self->diCount > 0;
    if (hadDI)
        g_intr.unlock();
    wait_for_cpu(lk, self, gen);
    if (hadDI) {
        lk.unlock();
        g_intr.lock();
        lk.lock();
    }
}

Thread *thread_of(int tid)
{
    if (tid == 0)
        return tl_self;
    if (tid < 1 || tid >= kMaxThreads || !g_threads[tid].used)
        return nullptr;
    return &g_threads[tid];
}

Sema *sema_of(int sid)
{
    if (sid < 0 || sid >= kMaxSemas || !g_semas[sid].used)
        return nullptr;
    return &g_semas[sid];
}

/* ---- native threads ------------------------------------------------------ */

/* The EE's FPU rounds toward zero and has no denormals: game threads run
 * with the same rounding and flushing (x87 and SSE control words), which
 * keeps the physics and replays closest to the PS2's results.  Native
 * calls that need the default mode (the graphics driver) restore it
 * around themselves (platform/fpu.h). */
void set_game_fpu_mode()
{
#ifdef _WIN32
    unsigned int cw;
    _controlfp_s(&cw, _RC_CHOP | _DN_FLUSH, _MCW_RC | _MCW_DN);
#else
    fpu::set_mode(fpu::kGameMode);
#endif
}

struct StartBox {
    Thread *t;
    int gen;
};

#ifdef _WIN32
unsigned __stdcall native_entry(void *p)
#else
void *native_entry(void *p)
#endif
{
    StartBox box = *(StartBox *)p;
    delete (StartBox *)p;
    set_game_fpu_mode();
    Thread *t = box.t;
    tl_self = t;
    tl_gen = box.gen;
    void (*entry)(void *);
    void *arg;
    {
        std::unique_lock<std::mutex> lk(g_mtx);
        wait_for_cpu(lk, t, box.gen);
        entry = t->entry;
        arg = t->arg;
    }
    entry(arg);
    exit_thread(false);
    return 0;
}

void spawn_native(Thread &t)
{
    StartBox *box = new StartBox{&t, t.generation};
    unsigned stack = t.stackSize > kMinStack ? (unsigned)t.stackSize : (unsigned)kMinStack;
#ifdef _WIN32
    uintptr_t h = _beginthreadex(nullptr, stack, native_entry, box, 0, nullptr);
    if (h == 0)
        pclog::fatal("could not create a game thread (errno %d)", errno);
    if (t.native)
        CloseHandle(t.native);
    t.native = (HANDLE)h;
#else
    /* Detached: a game thread ends by pthread_exit and nobody joins it.  The
     * stack size is the one asked (16 MB for the main thread, main.cpp), at
     * least kMinStack, rounded up to 64 KB. */
    stack = (stack + 0xFFFFu) & ~0xFFFFu;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_attr_setstacksize(&attr, stack);
    pthread_t h;
    if (err == 0)
        err = pthread_create(&h, &attr, native_entry, box);
    pthread_attr_destroy(&attr);
    if (err != 0)
        pclog::fatal("could not create a game thread (error %d)", err);
    t.native = h;
    t.hasNative = true;
#endif
}

/* ---- interrupts ---------------------------------------------------------- */

void run_handlers(int cause)
{
    std::vector<Handler> list;
    {
        std::lock_guard<std::mutex> lk(g_hmtx);
        if (!g_intcEnabled[cause])
            return;
        for (const Handler &h : g_handlers)
            if (h.cause == cause)
                list.push_back(h);
    }
    if (list.empty())
        return;
    g_intr.lock();
    tl_inInterrupt = true;
    for (const Handler &h : list) {
        if (h.fn)
            h.fn(cause, h.arg, nullptr);
        else if (h.plain)
            h.plain(cause);
    }
    tl_inInterrupt = false;
    g_intr.unlock();
    dispatch_after_interrupt();
}

void wake_delayed(double now)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    tl_inInterrupt = true;
    for (int i = 1; i < kMaxThreads; i++) {
        Thread &t = g_threads[i];
        if (t.used && (t.status & ST_WAIT) && t.waitType == WAIT_DELAY && t.delayUntil <= now) {
            if (t.status == ST_WAITSUSPEND) {
                t.waitType = WAIT_NONE;
                t.status = ST_SUSPEND;
            } else {
                make_ready(t);
            }
        }
    }
    tl_inInterrupt = false;
    if (g_running == 0)
        dispatch(pop_best());
}

double next_delay_time()
{
    double best = 1e30;
    std::lock_guard<std::mutex> lk(g_mtx);
    for (int i = 1; i < kMaxThreads; i++) {
        const Thread &t = g_threads[i];
        if (t.used && (t.status & ST_WAIT) && t.waitType == WAIT_DELAY && t.delayUntil < best)
            best = t.delayUntil;
    }
    return best;
}

void run_alarms(double now)
{
    std::vector<Alarm> due;
    {
        std::lock_guard<std::mutex> lk(g_hmtx);
        for (size_t i = 0; i < g_alarms.size();) {
            if (g_alarms[i].when <= now) {
                due.push_back(g_alarms[i]);
                g_alarms.erase(g_alarms.begin() + i);
            } else {
                i++;
            }
        }
    }
    if (due.empty())
        return;
    g_intr.lock();
    tl_inInterrupt = true;
    for (const Alarm &a : due)
        a.cb(a.id, a.hsyncs, a.arg);
    tl_inInterrupt = false;
    g_intr.unlock();
    dispatch_after_interrupt();
}

double next_alarm_time()
{
    double best = 1e30;
    std::lock_guard<std::mutex> lk(g_hmtx);
    for (const Alarm &a : g_alarms)
        if (a.when < best)
            best = a.when;
    return best;
}

#ifndef _WIN32
/* The last part of a timer wait, slept without the wake-up: a nudge that
 * comes inside it is seen at the event that ends it, at most this late. */
constexpr double kTimerFineWait = 0.0005;

timespec monotonic_at(double t)
{
    const int64_t ns = g_startNs + (int64_t)(t * 1e9);
    timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    return ts;
}

/* The timer thread sleeps until now_seconds() reaches `next' or a nudge
 * comes: on the condition until shortly before `next', then the rest on
 * clock_nanosleep at the absolute time, whose wake-up is the precise one. */
void timer_sleep_until(double next)
{
    pthread_mutex_lock(&g_wakeMtx);
    const double coarse = next - kTimerFineWait;
    while (!g_wakePending && now_seconds() < coarse) {
        const timespec ts = monotonic_at(coarse);
        pthread_cond_timedwait(&g_wakeCv, &g_wakeMtx, &ts);
    }
    const bool woken = g_wakePending;
    g_wakePending = false;
    pthread_mutex_unlock(&g_wakeMtx);
    if (woken)
        return;
    const timespec ts = monotonic_at(next);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

/* The timer thread's priority.  The Windows build makes it time-critical;
 * here it is the hook for SDL_SetCurrentThreadPriority
 * (SDL_THREAD_PRIORITY_TIME_CRITICAL) once SDL is linked in, and the
 * default priority until then. */
void raise_timer_priority()
{
}
#endif

/* The interrupt source.  Sleeps with a high-resolution timer until the
 * next event (or until a new alarm / delay nudges it), so vblanks are
 * within a fraction of a millisecond. */
#ifdef _WIN32
unsigned __stdcall timer_thread(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
#else
void *timer_thread(void *)
{
    raise_timer_priority();
    /* A new pthread inherits its creator's FPU mode (a Windows thread starts
     * in the default one): handlers run in the default mode, as there. */
    fpu::set_mode(fpu::kDefaultMode);
#endif
    double nextStart = now_seconds() + kVblankPeriod;
    double nextEnd = 1e30;
    for (;;) {
        double now = now_seconds();
        double next = nextStart;
        if (nextEnd < next) next = nextEnd;
        double d = next_delay_time();
        if (d < next) next = d;
        double a = next_alarm_time();
        if (a < next) next = a;
        if (next > now) {
#ifdef _WIN32
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((next - now) * 1e7);
            if (due.QuadPart > -1) due.QuadPart = -1;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE hs[2] = {timer, g_timerWake};
            WaitForMultipleObjects(2, hs, FALSE, INFINITE);
#else
            timer_sleep_until(next);
#endif
            now = now_seconds();
        }
        if (now >= nextStart) {
            g_vblankTime = nextStart;
            g_vblankTimes[(g_vblanks.load() + 1) & 31] = nextStart;
            g_vblanks.fetch_add(1);
            run_handlers(INTC_VBLANK_START);
            nextEnd = nextStart + kVblankEndDelay;
            nextStart += kVblankPeriod;
            if (nextStart < now)                    /* fell far behind (debugger): resync */
                nextStart = now + kVblankPeriod;
        }
        if (now >= nextEnd) {                       /* (at once: kVblankEndDelay) */
            nextEnd = 1e30;
            run_handlers(INTC_VBLANK_END);
        }
        wake_delayed(now);
        run_alarms(now);
    }
}

} // namespace

/* ======================================================================== */

double now_seconds()
{
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpcStart.QuadPart) / (double)g_qpcFreq.QuadPart;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec - g_startNs) * 1e-9;
#endif
}

uint64_t vblank_count()
{
    return g_vblanks;
}

double vblank_time()
{
    return g_vblankTime;
}

double vblank_period()
{
    return kVblankPeriod;
}

uint64_t vblanks_at(double t, double *time)
{
    const uint64_t n = g_vblanks.load();
    for (uint64_t k = n; k > 0 && k + 31 > n; k--) {
        const double vt = g_vblankTimes[k & 31].load();
        if (vt <= t) {
            *time = vt;
            return k;
        }
    }
    /* t before the vblanks kept: the oldest of them */
    const uint64_t k = n > 31 ? n - 31 : 0;
    *time = k > 0 ? g_vblankTimes[k & 31].load() : 0.0;
    return k;
}

void init()
{
#ifdef _WIN32
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    timeBeginPeriod(1);
    g_timerWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uintptr_t h = _beginthreadex(nullptr, 256 * 1024, timer_thread, nullptr, 0, nullptr);
    if (h == 0)
        pclog::fatal("could not start the timer thread");
    CloseHandle((HANDLE)h);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_startNs = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g_wakeCv, &ca);
    pthread_condattr_destroy(&ca);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t h;
    const int err = pthread_create(&h, &attr, timer_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (err != 0)
        pclog::fatal("could not start the timer thread (error %d)", err);
#endif
}

void start_main_thread(void (*entry)(void *), void *arg, int priority, int stackSize)
{
    int tid = create_thread(entry, priority, stackSize);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        Thread &t = g_threads[tid];
        t.arg = arg;
        t.generation++;
        t.prio = t.initPrio;
        t.status = ST_READY;
        g_ready[t.prio].push_back(t.id);
        spawn_native(t);
        if (g_running == 0)
            dispatch(pop_best());
    }
}

/* ---- threads ------------------------------------------------------------- */

int create_thread(void (*entry)(void *), int priority, int stackSize)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (priority < 0 || priority >= kPriorities)
        return -1;
    for (int i = 1; i < kMaxThreads; i++) {
        Thread &t = g_threads[i];
        if (t.used)
            continue;
        t.used = true;
        t.id = i;
        t.status = ST_DORMANT;
        t.prio = t.initPrio = priority;
        t.waitType = WAIT_NONE;
        t.waitId = t.wakeupCount = t.waitResult = 0;
        t.suspended = false;
        t.entry = entry;
        t.arg = nullptr;
        t.stackSize = stackSize;
        t.diCount = 0;
        return i;
    }
    return -1;
}

int delete_thread(int tid)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || t == tl_self || t->status != ST_DORMANT)
        return -1;
    t->used = false;
    return tid;
}

int start_thread(int tid, void *arg)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || t->status != ST_DORMANT)
        return -1;
    t->arg = arg;
    t->generation++;
    t->prio = t->initPrio;
    t->wakeupCount = 0;
    t->diCount = 0;
    t->suspended = false;
    spawn_native(*t);
    make_ready(*t);
    reschedule(lk);
    return tid;
}

void exit_thread(bool deleteIt)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *self = tl_self;
    if (!self) {
        lk.unlock();
        end_native_thread();
    }
    if (self->diCount > 0) {
        self->diCount = 0;
        g_intr.unlock();
    }
    self->status = ST_DORMANT;
    self->generation++;
    if (deleteIt)
        self->used = false;
    if (g_running == self->id)
        dispatch(pop_best());
    lk.unlock();
    end_native_thread();
}

int terminate_thread(int tid)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || t == tl_self)
        return -1;
    if (t->status == ST_READY)
        remove_ready(*t);
    if ((t->status & ST_WAIT) && t->waitType == WAIT_SEMA) {
        Sema *s = sema_of(t->waitId);
        if (s)
            for (auto it = s->waiters.begin(); it != s->waiters.end(); ++it)
                if (*it == t->id) {
                    s->waiters.erase(it);
                    break;
                }
    }
    t->status = ST_DORMANT;
    t->waitType = WAIT_NONE;
    t->generation++;                /* its native thread ends when it wakes */
    t->cv.notify_all();
    return tid;
}

int change_priority(int tid, int priority, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || priority < 0 || priority >= kPriorities)
        return -1;
    int old = t->prio;
    if (t->status == ST_READY) {
        remove_ready(*t);
        t->prio = priority;
        g_ready[priority].push_back(t->id);
        if (g_running && priority < g_threads[g_running].prio)
            g_needResched = true;
    } else {
        t->prio = priority;
    }
    if (t == tl_self && g_running == t->id) {
        int best = peek_best();
        if (best && g_threads[best].prio < priority) {
            t->status = ST_READY;
            g_ready[priority].push_back(t->id);
        }
    }
    if (dispatch)
        reschedule(lk);
    return old;
}

int rotate_ready_queue(int priority, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    if (priority < 0 || priority >= kPriorities)
        return -1;
    Thread *self = tl_self;
    if (self && g_running == self->id && self->prio == priority) {
        if (!g_ready[priority].empty()) {
            self->status = ST_READY;
            g_ready[priority].push_back(self->id);
            if (dispatch)
                reschedule(lk);
        }
    } else if (g_ready[priority].size() > 1) {
        int id = g_ready[priority].front();
        g_ready[priority].pop_front();
        g_ready[priority].push_back(id);
    }
    return priority;
}

int release_wait(int tid, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || !(t->status & ST_WAIT))
        return -1;
    if (t->waitType == WAIT_SEMA) {
        Sema *s = sema_of(t->waitId);
        if (s)
            for (auto it = s->waiters.begin(); it != s->waiters.end(); ++it)
                if (*it == t->id) {
                    s->waiters.erase(it);
                    break;
                }
    }
    t->waitResult = -1;
    if (t->status == ST_WAITSUSPEND) {
        t->waitType = WAIT_NONE;
        t->status = ST_SUSPEND;
    } else {
        make_ready(*t);
    }
    if (dispatch)
        reschedule(lk);
    return tid;
}

int current_thread()
{
    return tl_self ? tl_self->id : 0;
}

int refer_thread(int tid, ThreadInfo *out)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t)
        return -1;
    out->status = t->status;
    out->initPriority = t->initPrio;
    out->currentPriority = t->prio;
    out->waitType = t->waitType == WAIT_DELAY ? WAIT_SLEEP : t->waitType;
    out->waitId = t->waitId;
    out->wakeupCount = t->wakeupCount;
    out->entry = t->entry;
    return t->status;
}

int sleep_thread()
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *self = tl_self;
    if (!self)
        return -1;
    if (self->wakeupCount > 0) {
        self->wakeupCount--;
        reschedule(lk);
        return self->id;
    }
    self->status = ST_WAIT;
    self->waitType = WAIT_SLEEP;
    self->waitResult = self->id;
    reschedule(lk);
    return self->waitResult;
}

int wakeup_thread(int tid, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || t == tl_self)
        return -1;
    if ((t->status & ST_WAIT) && t->waitType == WAIT_SLEEP) {
        if (t->status == ST_WAITSUSPEND) {
            t->waitType = WAIT_NONE;
            t->status = ST_SUSPEND;
        } else {
            make_ready(*t);
        }
    } else {
        t->wakeupCount++;
    }
    if (dispatch)
        reschedule(lk);
    return tid;
}

int cancel_wakeup(int tid)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t)
        return -1;
    int n = t->wakeupCount;
    t->wakeupCount = 0;
    return n;
}

int suspend_thread(int tid, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || t->status == ST_DORMANT)
        return -1;
    t->suspended = true;
    if (t->status == ST_READY) {
        remove_ready(*t);
        t->status = ST_SUSPEND;
    } else if (t->status == ST_WAIT) {
        t->status = ST_WAITSUSPEND;
    } else if (t->status == ST_RUN) {
        t->status = ST_SUSPEND;
        reschedule(lk);
    }
    return tid;
}

int resume_thread(int tid, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *t = thread_of(tid);
    if (!t || !t->suspended)
        return -1;
    t->suspended = false;
    if (t->status == ST_SUSPEND)
        make_ready(*t);
    else if (t->status == ST_WAITSUSPEND)
        t->status = ST_WAIT;
    if (dispatch)
        reschedule(lk);
    return tid;
}

void run_outside(void (*fn)(void *), void *arg)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *self = tl_self;
    if (!self || g_running != self->id) {
        lk.unlock();
        fn(arg);
        return;
    }
    /* leave the CPU as a blocking kernel call would */
    self->status = ST_WAIT;
    self->waitType = WAIT_NATIVE;
    const int gen = tl_gen;
    const bool hadDI = self->diCount > 0;
    if (hadDI)
        g_intr.unlock();
    dispatch(pop_best());
    lk.unlock();
    fn(arg);
    lk.lock();
    /* back in the ready queue; the CPU at the scheduler's turn */
    make_ready(*self);
    wait_for_cpu(lk, self, gen);
    if (hadDI) {
        lk.unlock();
        g_intr.lock();
        lk.lock();
    }
}

void wait_until(double t)
{
    double now;
    while ((now = now_seconds()) < t - 0.0008)
        delay_thread((int)((t - 0.0008 - now) * 1e6) + 1);
    while (now_seconds() < t)
#ifdef _WIN32
        YieldProcessor();
#else
        __asm__ __volatile__("pause");
#endif
}

int delay_thread(int usec)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Thread *self = tl_self;
    if (!self)
        return -1;
    self->status = ST_WAIT;
    self->waitType = WAIT_DELAY;
    self->delayUntil = now_seconds() + usec * 1e-6;
    nudge_timer();
    reschedule(lk);
    return 0;
}

/* ---- semaphores ------------------------------------------------------------ */

int create_sema(int initCount, int maxCount)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    for (int i = 1; i < kMaxSemas; i++) {
        Sema &s = g_semas[i];
        if (s.used)
            continue;
        s.used = true;
        s.count = s.init = initCount;
        s.max = maxCount;
        s.waiters.clear();
        return i;
    }
    return -1;
}

int delete_sema(int sid)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Sema *s = sema_of(sid);
    if (!s)
        return -1;
    while (!s->waiters.empty()) {
        Thread &t = g_threads[s->waiters.front()];
        s->waiters.pop_front();
        t.waitResult = -1;
        make_ready(t);
    }
    s->used = false;
    reschedule(lk);
    return sid;
}

int signal_sema(int sid, bool dispatch)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Sema *s = sema_of(sid);
    if (!s)
        return -1;
    if (!s->waiters.empty()) {
        Thread &t = g_threads[s->waiters.front()];
        s->waiters.pop_front();
        t.waitResult = sid;
        if (t.status == ST_WAITSUSPEND) {
            t.waitType = WAIT_NONE;
            t.status = ST_SUSPEND;
        } else {
            make_ready(t);
        }
    } else if (s->max > 0 && s->count >= s->max) {
        return -1;
    } else {
        s->count++;
    }
    if (dispatch)
        reschedule(lk);
    return sid;
}

int wait_sema(int sid)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Sema *s = sema_of(sid);
    if (!s)
        return -1;
    if (s->count > 0) {
        s->count--;
        reschedule(lk);
        return sid;
    }
    Thread *self = tl_self;
    if (!self) {
        pclog::write("sched: WaitSema(%d) outside a game thread would block; ignored", sid);
        return -1;
    }
    s->waiters.push_back(self->id);
    self->status = ST_WAIT;
    self->waitType = WAIT_SEMA;
    self->waitId = sid;
    self->waitResult = sid;
    reschedule(lk);
    return self->waitResult;
}

int poll_sema(int sid)
{
    std::unique_lock<std::mutex> lk(g_mtx);
    Sema *s = sema_of(sid);
    if (!s || s->count <= 0)
        return -1;
    s->count--;
    return sid;
}

int refer_sema(int sid, SemaInfo *out)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    Sema *s = sema_of(sid);
    if (!s)
        return -1;
    out->currentCount = s->count;
    out->maxCount = s->max;
    out->initCount = s->init;
    out->numWaitThreads = (int)s->waiters.size();
    return sid;
}

/* ---- interrupts ------------------------------------------------------------ */

int add_intc_handler(int cause, int (*handler)(int, void *, void *), void *arg,
                     int (*plain)(int), int first)
{
    if (cause < 0 || cause >= 16)
        return -1;
    std::lock_guard<std::mutex> lk(g_hmtx);
    Handler h{g_nextHandlerId++, cause, handler, plain, arg};
    if (first == 0)
        g_handlers.insert(g_handlers.begin(), h);
    else
        g_handlers.push_back(h);
    return h.id;
}

int remove_intc_handler(int cause, int id)
{
    std::lock_guard<std::mutex> lk(g_hmtx);
    for (auto it = g_handlers.begin(); it != g_handlers.end(); ++it)
        if (it->id == id && it->cause == cause) {
            g_handlers.erase(it);
            return 0;
        }
    return -1;
}

int enable_intc(int cause, bool on)
{
    if (cause < 0 || cause >= 16)
        return -1;
    std::lock_guard<std::mutex> lk(g_hmtx);
    bool was = g_intcEnabled[cause];
    g_intcEnabled[cause] = on;
    return was ? 0 : 1;
}

int di()
{
    Thread *self = tl_self;
    if (!self)
        return 0;
    if (self->diCount++ == 0) {
        g_intr.lock();
        return 1;
    }
    return 0;
}

void ei()
{
    Thread *self = tl_self;
    if (!self || self->diCount == 0)
        return;
    if (--self->diCount == 0) {
        g_intr.unlock();
        /* An interrupt (or an i-call) that made a higher-priority thread
         * ready during the section takes effect here, as on the EE when
         * interrupts come back on. */
        preempt_point();
    }
}

int set_alarm(uint16_t hsyncs, void (*cb)(int, uint16_t, void *), void *arg)
{
    int id;
    {
        std::lock_guard<std::mutex> lk(g_hmtx);
        id = g_nextAlarmId++;
        g_alarms.push_back(Alarm{id, now_seconds() + hsyncs / kHsyncRate, hsyncs, cb, arg});
    }
    nudge_timer();
    return id;
}

int release_alarm(int id)
{
    std::lock_guard<std::mutex> lk(g_hmtx);
    for (auto it = g_alarms.begin(); it != g_alarms.end(); ++it)
        if (it->id == id) {
            g_alarms.erase(it);
            return 0;
        }
    return -1;
}

void preempt_point()
{
    /* not inside a DI section: no interrupt preempts there on the EE */
    if (!g_needResched || !tl_self || tl_self->diCount != 0)
        return;
    std::unique_lock<std::mutex> lk(g_mtx);
    reschedule(lk);
}

void log_threads(const char *why)
{
    struct Snap {
        int id, status, prio, initPrio, waitType, waitId, diCount;
        int semaCount, semaWaiters;
        double delayLeft;
        void (*entry)(void *);
#ifdef _WIN32
        HANDLE native;
#else
        int generation;
#endif
    };
    static Snap snaps[kMaxThreads];
    int n = 0, running;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        running = g_running;
        const double now = now_seconds();
        for (int i = 1; i < kMaxThreads; i++) {
            const Thread &t = g_threads[i];
            if (!t.used || t.status == ST_DORMANT)
                continue;
            Snap &s = snaps[n++];
            s.id = t.id;
            s.status = t.status;
            s.prio = t.prio;
            s.initPrio = t.initPrio;
            s.waitType = t.waitType;
            s.waitId = t.waitId;
            s.diCount = t.diCount;
            s.semaCount = s.semaWaiters = 0;
            if ((t.status & ST_WAIT) && t.waitType == WAIT_SEMA) {
                if (const Sema *sm = sema_of(t.waitId)) {
                    s.semaCount = sm->count;
                    s.semaWaiters = (int)sm->waiters.size();
                }
            }
            s.delayLeft = (t.delayUntil - now) * 1000.0;
            s.entry = t.entry;
#ifdef _WIN32
            s.native = nullptr;
            if (t.native)
                DuplicateHandle(GetCurrentProcess(), t.native, GetCurrentProcess(), &s.native, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
#else
            s.generation = t.generation;
#endif
        }
    }

    pclog::write("sched: the game threads (%s): %d, thread %d on the CPU", why, n, running);
    for (int i = 0; i < n; i++) {
        const Snap &s = snaps[i];
        char state[96], entry[256];
        switch (s.status) {
        case ST_RUN:
            snprintf(state, sizeof(state), "running");
            break;
        case ST_READY:
            snprintf(state, sizeof(state), "ready");
            break;
        case ST_SUSPEND:
            snprintf(state, sizeof(state), "suspended");
            break;
        default:
            switch (s.waitType) {
            case WAIT_SEMA:
                snprintf(state, sizeof(state), "waiting for semaphore %d (count %d, %d waiting)", s.waitId,
                         s.semaCount, s.semaWaiters);
                break;
            case WAIT_SLEEP:
                snprintf(state, sizeof(state), "sleeping");
                break;
            case WAIT_DELAY:
                snprintf(state, sizeof(state), "delayed (%.1f ms left)", s.delayLeft);
                break;
            case WAIT_NATIVE:
                snprintf(state, sizeof(state), "in a native call");
                break;
            default:
                snprintf(state, sizeof(state), "status 0x%X", s.status);
                break;
            }
            if (s.status == ST_WAITSUSPEND)
                strncat(state, ", suspended", sizeof(state) - strlen(state) - 1);
            break;
        }
        crash::describe_address((const void *)s.entry, entry, sizeof(entry));
        pclog::write("sched: thread %d, priority %d (%d at start)%s: %s; %s", s.id, s.prio, s.initPrio,
                     s.diCount > 0 ? ", interrupts off" : "", state, entry);
#ifdef _WIN32
        if (s.native) {
            if (GetThreadId(s.native) != GetCurrentThreadId())
                crash::log_stack(s.native, "sched:     ");
            CloseHandle(s.native);
        }
#else
        /* No handle keeps a detached pthread alive: its stack is read under
         * the lock, which a game thread takes before it ends, and only if it
         * is still the native thread seen above. */
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            const Thread &t = g_threads[s.id];
            if (t.used && t.status != ST_DORMANT && t.generation == s.generation && t.hasNative &&
                !pthread_equal(t.native, pthread_self()))
                crash::log_stack((void *)(uintptr_t)t.native, "sched:     ");
        }
#endif
    }
}

} // namespace sched
