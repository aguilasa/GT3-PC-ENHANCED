/* sched_test - the game-thread scheduler and the FPU mode on POSIX
 * (src/platform/scheduler.cpp, src/platform/fpu.h):
 *   1. three game threads of one priority pass a token round by
 *      WaitSema / SignalSema, in order, one at a time;
 *   2. SignalSema switches to a higher-priority waiter at once and
 *      RotateThreadReadyQueue lets equal priorities run;
 *   3. a game thread runs in the EE's FPU mode (MXCSR RC=11, FTZ and DAZ
 *      on, x87 RC toward zero) and DefaultFpuMode switches to the default
 *      mode and back; a native thread keeps the default mode;
 *   4. the main game thread has the 16 MB stack it asked for;
 *   5. the vblank interrupt comes every period, within 1 ms, over 600
 *      samples (the mean and the largest deviation are printed).
 * Exit code 0 when everything passes.
 *
 * SCHED_TEST_BREAK=order|fpu|timer breaks one check on purpose (a thread
 * skips its wait, the game thread drops the FPU mode, the timer tolerance
 * becomes 1 us), to see that the check fails. */
#include "eekernel.h"
#include "platform/fpu.h"
#include "platform/log.h"
#include "platform/scheduler.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

/* ---- what the scheduler links against (the game's log and crash report) */

namespace pclog {
void vwrite(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
void write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(fmt, ap);
    va_end(ap);
}
void raw(const char *text, int len)
{
    fwrite(text, 1, (size_t)len, stdout);
}
void close() {}
[[noreturn]] void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(fmt, ap);
    va_end(ap);
    _exit(3);
}
} // namespace pclog

namespace crash {
void log_stack(void *, const char *) {}
void prepare_thread() {}
void describe_address(const void *addr, char *out, unsigned outSize)
{
    snprintf(out, outSize, "%p", addr);
}
} // namespace crash

/* ---- checks -------------------------------------------------------------- */

static std::string g_break;
static int g_failures;
static std::atomic<int> g_onCpu{0};
static std::atomic<bool> g_done{false};
static std::vector<std::string> g_trace;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok)
        g_failures++;
}

/* A game thread's step: two at once would be a scheduler bug. */
static void trace(const std::string &s)
{
    if (g_onCpu.fetch_add(1) != 0) {
        printf("FAIL two game threads ran at once (%s)\n", s.c_str());
        g_failures++;
    }
    g_trace.push_back(s);
    for (volatile int i = 0; i < 20000; i++) {       /* widen the window */
    }
    g_onCpu.fetch_sub(1);
}

static std::string joined(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &x : v)
        s += (s.empty() ? "" : " ") + x;
    return s;
}

static void expect(const char *what, const std::vector<std::string> &want)
{
    const bool ok = g_trace == want;
    check(ok, "%s", what);
    if (!ok)
        printf("   got:  %s\n   want: %s\n", joined(g_trace).c_str(), joined(want).c_str());
    g_trace.clear();
}

static int make_sema(int init, int max = 1)
{
    SemaParam p = {};
    p.initCount = init;
    p.maxCount = max;
    return CreateSema(&p);
}

static int make_thread(void (*fn)(void *), int prio)
{
    ThreadParam p = {};
    p.entry = fn;
    p.initPriority = prio;
    p.stackSize = 0x10000;
    return CreateThread(&p);
}

/* ---- 1: a token round of three threads ---------------------------------- */

constexpr int kRounds = 50;
static int s_ring[3], s_ringDone;

static void ring_thread(void *arg)
{
    const int me = (int)(intptr_t)arg;
    for (int r = 0; r < kRounds; r++) {
        if (!(g_break == "order" && me == 1 && r == 3))
            WaitSema(s_ring[me]);
        trace(std::string(1, (char)('A' + me)));
        if (me == 2 && r == kRounds - 1)
            SignalSema(s_ringDone);
        else
            SignalSema(s_ring[(me + 1) % 3]);
    }
}

static void test_ring()
{
    for (int i = 0; i < 3; i++)
        s_ring[i] = make_sema(0);
    s_ringDone = make_sema(0);
    for (int i = 0; i < 3; i++)
        StartThread(make_thread(ring_thread, 20), (void *)(intptr_t)i);   /* below us: wait */
    SignalSema(s_ring[0]);
    WaitSema(s_ringDone);                   /* the CPU goes to the ring */
    std::vector<std::string> want;
    for (int r = 0; r < kRounds; r++)
        for (const char *n : {"A", "B", "C"})
            want.push_back(n);
    const size_t got = g_trace.size();
    char what[160];
    snprintf(what, sizeof(what), "3 game threads alternate by WaitSema / SignalSema (%d rounds, %zu steps)",
             kRounds, got);
    if (g_trace != want && got > 12)
        g_trace.resize(12), want.resize(12);
    expect(what, want);
}

/* ---- 2: preemption by priority, rotation -------------------------------- */

static int s_high;
static void high_waiter(void *)
{
    trace("high-wait");
    WaitSema(s_high);
    trace("high-run");
}

static void named(void *arg)
{
    trace((const char *)arg);
}

static void test_priority()
{
    s_high = make_sema(0);
    StartThread(make_thread(high_waiter, 5), nullptr);      /* beats us (10): runs to its wait */
    trace("main-signal");
    SignalSema(s_high);
    trace("main-after");
    expect("SignalSema switches to a higher-priority waiter at once",
           {"high-wait", "main-signal", "high-run", "main-after"});

    StartThread(make_thread(named, 10), (void *)"X");
    StartThread(make_thread(named, 10), (void *)"Y");       /* same priority: not yet */
    trace("main");
    RotateThreadReadyQueue(10);
    trace("main-back");
    expect("equal priorities are FIFO; rotation lets them run", {"main", "X", "Y", "main-back"});
}

/* ---- 3: the FPU mode ----------------------------------------------------- */

static unsigned int mxcsr()
{
    return fpu::read_mxcsr();
}

static unsigned int x87_rc()
{
    fpu_control_t cw;
    _FPU_GETCW(cw);
    return (unsigned int)cw & _FPU_RC_ZERO;
}

/* a / b on the SSE unit (the i686 build computes floats on the x87) */
__attribute__((target("sse"), noinline)) static float sse_div(float a, float b)
{
    __asm__ __volatile__("movss %1, %%xmm0\n\tdivss %2, %%xmm0\n\tmovss %%xmm0, %0"
                         : "=m"(a) : "m"(a), "m"(b) : "xmm0");
    return a;
}

static void *native_fpu(void *out)
{
    *(unsigned int *)out = mxcsr();
    return nullptr;
}

static void test_fpu()
{
    if (g_break == "fpu")
        fpu::set_mode(fpu::kDefaultMode);
    const unsigned int csr = mxcsr();
    check((csr & 0x6000u) == 0x6000u && (csr & 0x8000u) && (csr & 0x0040u),
          "game thread: MXCSR 0x%04X (RC=%u%u, FTZ=%u, DAZ=%u)", csr, (csr >> 14) & 1, (csr >> 13) & 1,
          (csr >> 15) & 1, (csr >> 6) & 1);
    check(x87_rc() == _FPU_RC_ZERO, "game thread: x87 RC 0x%03X (toward zero)", x87_rc());

    /* the mode at work: 1/3 rounded toward zero, a denormal flushed */
    volatile float one = 1.0f, three = 3.0f, tiny = 2e-38f, ten = 10.0f;
    const float third = sse_div(one, three);
    uint32_t bits;
    memcpy(&bits, &third, 4);
    check(bits == 0x3EAAAAAAu, "game thread: SSE 1/3 = 0x%08X (toward zero: 0x3EAAAAAA)", bits);
    const float denorm = sse_div(tiny, ten);
    check(denorm == 0.0f, "game thread: SSE 2e-38/10 = %g (flushed to 0)", (double)denorm);

    {
        fpu::DefaultFpuMode d;
        const unsigned int in = mxcsr();
        check((in & fpu::kSseBits) == 0 && x87_rc() == _FPU_RC_NEAREST,
              "DefaultFpuMode: MXCSR 0x%04X, x87 RC 0x%03X (nearest, denormals kept)", in, x87_rc());
        {
            fpu::ScopedFpuMode g(d.saved());
            check((mxcsr() & fpu::kSseBits) == (csr & fpu::kSseBits) && x87_rc() == _FPU_RC_ZERO,
                  "ScopedFpuMode(saved()): the game's mode again inside");
        }
        check((mxcsr() & fpu::kSseBits) == 0, "ScopedFpuMode restores the default mode");
    }
    check((mxcsr() & fpu::kSseBits) == (csr & fpu::kSseBits) && x87_rc() == _FPU_RC_ZERO,
          "DefaultFpuMode restores the game's mode (MXCSR 0x%04X)", mxcsr());

    /* not checked: a pthread inherits its creator's mode (Windows threads
     * start in the default one) */
    unsigned int native = 0;
    pthread_t th;
    pthread_create(&th, nullptr, native_fpu, &native);
    pthread_join(th, nullptr);
    printf("     a pthread created by a game thread starts with MXCSR 0x%04X\n", native);
}

/* The interrupt handlers (the timer thread) run in the default mode. */
static std::atomic<unsigned int> s_intrCsr{0xFFFFFFFFu};
static int s_intrSema;
static int fpu_handler(int)
{
    unsigned int expected = 0xFFFFFFFFu;
    if (s_intrCsr.compare_exchange_strong(expected, mxcsr()))
        iSignalSema(s_intrSema);
    return 0;
}

static void test_intr_fpu()
{
    s_intrSema = make_sema(0);
    const int id = AddIntcHandler(INTC_VBLANK_S, fpu_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    WaitSema(s_intrSema);
    DisableIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, id);
    const unsigned int c = s_intrCsr.load();
    check((c & fpu::kSseBits) == 0, "interrupt handler: MXCSR 0x%04X (the default mode)", c);
}

/* ---- 4: the main game thread's stack -------------------------------------- */

static void test_stack()
{
    pthread_attr_t attr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstacksize(&attr, &size);
        pthread_attr_destroy(&attr);
    }
    check(size >= 16u * 1024 * 1024, "main game thread stack: %zu bytes (16 MB asked)", size);
}

/* ---- 5: the vblank timer -------------------------------------------------- */

constexpr int kSamples = 600;
static double s_vbAt[kSamples + 1];
static std::atomic<int> s_vbN{0};
static double s_vbLate[kSamples + 1];
static int s_vbSema;

static int vblank_handler(int)
{
    const double now = sched::now_seconds();
    const int n = s_vbN.load();
    if (n <= kSamples) {
        s_vbAt[n] = now;
        s_vbLate[n] = now - sched::vblank_time();
        s_vbN.store(n + 1);
        if (n == kSamples)
            iSignalSema(s_vbSema);
    }
    return 0;
}

static void test_timer()
{
    s_vbSema = make_sema(0);
    const int id = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    WaitSema(s_vbSema);
    DisableIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, id);

    const double period = sched::vblank_period();
    double sum = 0, worst = 0, lateSum = 0, lateMax = 0;
    for (int i = 1; i <= kSamples; i++) {
        const double dev = std::fabs(s_vbAt[i] - s_vbAt[i - 1] - period);
        sum += dev;
        if (dev > worst)
            worst = dev;
        lateSum += s_vbLate[i];
        if (s_vbLate[i] > lateMax)
            lateMax = s_vbLate[i];
    }
    const double tol = g_break == "timer" ? 1e-6 : 1e-3;
    printf("     vblank period %.4f ms; interval deviation over %d samples: mean %.4f ms, max %.4f ms;"
           " lateness after the due time: mean %.4f ms, max %.4f ms\n",
           period * 1e3, kSamples, sum / kSamples * 1e3, worst * 1e3, lateSum / kSamples * 1e3, lateMax * 1e3);
    check(worst < tol, "vblank interval within %.4f ms of the period (%d samples, max %.4f ms)", tol * 1e3,
          kSamples, worst * 1e3);
}

/* -------------------------------------------------------------------------- */

static void game_main(void *)
{
    ChangeThreadPriority(GetThreadId(), 10);
    test_fpu();
    test_intr_fpu();
    test_stack();
    test_ring();
    test_priority();
    test_timer();
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    fflush(stdout);
    g_done = true;
    SleepThread();
}

int main()
{
    if (const char *b = getenv("SCHED_TEST_BREAK"))
        g_break = b;
    sched::init();
    sched::start_main_thread(game_main, nullptr, 1, 16 * 1024 * 1024);
    for (int i = 0; i < 600 && !g_done; i++)
        usleep(100000);
    if (!g_done) {
        printf("FAIL the game thread did not finish in 60 s\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
