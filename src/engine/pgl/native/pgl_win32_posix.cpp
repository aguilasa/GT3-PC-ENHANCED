/* pgl_win32_posix.cpp - the renderer's Win32 calls on POSIX (pgl_win32.h). */
#include "engine/pgl/native/pgl_win32.h"

#include "platform/crash.h"
#include "platform/fpu.h"
#include "platform/os/fs.h"

#include <errno.h>
#include <execinfo.h>
#include <linux/futex.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

int64_t mono_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct timespec in_ms(DWORD ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    return ts;
}

enum class Kind { thread, event };

/* A HANDLE of this file: a thread or an event. */
struct Handle {
    Kind kind;
    /* the thread */
    pthread_t thread{};
    LPTHREAD_START_ROUTINE start = nullptr;
    LPVOID arg = nullptr;
    pid_t tid = 0;
    bool joined = false;
    /* the thread's end, an event's state */
    std::mutex m;
    std::condition_variable cv;
    bool set = false;
    bool manual = false;
};

/* GetCurrentThread's pseudo handle (the calling thread, as on Windows). */
Handle *const kCurrentThread = reinterpret_cast<Handle *>(static_cast<intptr_t>(-2));

void *thread_main(void *p)
{
    Handle *h = static_cast<Handle *>(p);
    {
        std::lock_guard<std::mutex> l(h->m);
        h->tid = (pid_t)syscall(SYS_gettid);
    }
    /* (a Windows thread starts in the default FPU mode; a pthread inherits its creator's) */
    fpu::set_mode(fpu::kDefaultMode);
    crash::prepare_thread();
    h->start(h->arg);
    {
        std::lock_guard<std::mutex> l(h->m);
        h->set = true;
    }
    h->cv.notify_all();
    return nullptr;
}

pid_t tid_of(HANDLE thread)
{
    Handle *h = static_cast<Handle *>(thread);
    if (h == kCurrentThread)
        return (pid_t)syscall(SYS_gettid);
    if (h == nullptr || h->kind != Kind::thread)
        return 0;
    std::lock_guard<std::mutex> l(h->m);
    return h->tid;
}

/* the futex word holding the address */
uint32_t *futex_word(const volatile void *address)
{
    return reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(address) & ~(uintptr_t)3);
}

long futex(uint32_t *word, int op, uint32_t value, const struct timespec *timeout)
{
    return syscall(SYS_futex, word, op, value, timeout, nullptr, 0);
}

} // namespace

BOOL QueryPerformanceCounter(LARGE_INTEGER *count)
{
    count->QuadPart = mono_ns();
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq)
{
    freq->QuadPart = 1000000000LL;
    return TRUE;
}

BOOL QueryThreadCycleTime(HANDLE thread, ULONG64 *cycles)
{
    if (thread != kCurrentThread)
        return FALSE;
    /* the TSC's rate, once: 2 ms of the clock against it */
    static const double tscPerNs = [] {
        const int64_t t0 = mono_ns();
        const uint64_t r0 = __rdtsc();
        int64_t t1;
        do {
            t1 = mono_ns();
        } while (t1 - t0 < 2000000);
        return (double)(__rdtsc() - r0) / (double)(t1 - t0);
    }();
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return FALSE;
    *cycles = (ULONG64)(((double)ts.tv_sec * 1e9 + (double)ts.tv_nsec) * tscPerNs);
    return TRUE;
}

BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *lock, DWORD ms, ULONG)
{
    if (ms == INFINITE)
        return pthread_cond_wait(&cv->c, &lock->m) == 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&cv->c, &lock->m, &ts) == 0;
}

BOOL WaitOnAddress(volatile void *address, void *compare, SIZE_T size, DWORD ms)
{
    if (std::memcmp(const_cast<void *>(address), compare, size) != 0)
        return TRUE;
    uint32_t *word = futex_word(address);
    uint32_t seen;
    if (size >= 4) {
        std::memcpy(&seen, compare, 4);         /* (the low word of an 8-byte value) */
    } else {
        /* the word as it is with the value compared in place */
        seen = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        std::memcpy(reinterpret_cast<uint8_t *>(&seen) + (reinterpret_cast<uintptr_t>(address) & 3), compare,
                    size);
    }
    struct timespec ts = in_ms(ms);
    const long r = futex(word, FUTEX_WAIT_PRIVATE, seen, ms == INFINITE ? nullptr : &ts);
    if (r != 0 && errno == ETIMEDOUT)
        return FALSE;
    return TRUE;
}

void WakeByAddressSingle(void *address)
{
    futex(futex_word(address), FUTEX_WAKE_PRIVATE, 1, nullptr);
}

void WakeByAddressAll(void *address)
{
    futex(futex_word(address), FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, nullptr);
}

HANDLE CreateThread(void *, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID arg, DWORD, DWORD *id)
{
    Handle *h = new Handle;
    h->kind = Kind::thread;
    h->start = start;
    h->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack != 0)
        pthread_attr_setstacksize(&attr, std::max<size_t>(stack, 64 * 1024));
    const int err = pthread_create(&h->thread, &attr, thread_main, h);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        delete h;
        return nullptr;
    }
    if (id != nullptr)
        *id = 0;
    return h;
}

HANDLE CreateEventW(void *, BOOL manualReset, BOOL initialState, LPCWSTR)
{
    Handle *h = new Handle;
    h->kind = Kind::event;
    h->manual = manualReset != FALSE;
    h->set = initialState != FALSE;
    return h;
}

BOOL SetEvent(HANDLE event)
{
    Handle *h = static_cast<Handle *>(event);
    if (h == nullptr || h == kCurrentThread || h->kind != Kind::event)
        return FALSE;
    {
        std::lock_guard<std::mutex> l(h->m);
        h->set = true;
    }
    if (h->manual)
        h->cv.notify_all();
    else
        h->cv.notify_one();
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD ms)
{
    Handle *h = static_cast<Handle *>(handle);
    if (h == nullptr || h == kCurrentThread)
        return WAIT_FAILED;
    std::unique_lock<std::mutex> l(h->m);
    if (ms == INFINITE)
        h->cv.wait(l, [h] { return h->set; });
    else if (!h->cv.wait_for(l, std::chrono::milliseconds(ms), [h] { return h->set; }))
        return WAIT_TIMEOUT;
    if (h->kind == Kind::event && !h->manual)
        h->set = false;
    if (h->kind == Kind::thread && !h->joined) {
        h->joined = true;
        l.unlock();
        pthread_join(h->thread, nullptr);
    }
    return WAIT_OBJECT_0;
}

BOOL CloseHandle(HANDLE handle)
{
    Handle *h = static_cast<Handle *>(handle);
    if (h == nullptr || h == kCurrentThread)
        return FALSE;
    if (h->kind == Kind::thread) {
        bool joined;
        {
            std::lock_guard<std::mutex> l(h->m);
            joined = h->joined;
            h->joined = true;
        }
        if (!joined) {
            /* (a thread still running keeps its Handle: it is never freed) */
            pthread_detach(h->thread);
            return TRUE;
        }
    }
    delete h;
    return TRUE;
}

HANDLE GetCurrentThread(void)
{
    return kCurrentThread;
}

DWORD GetCurrentThreadId(void)
{
    return (DWORD)syscall(SYS_gettid);
}

BOOL SetThreadPriority(HANDLE thread, int priority)
{
    if (priority >= THREAD_PRIORITY_NORMAL)
        return TRUE;
    const pid_t tid = tid_of(thread);
    if (tid == 0)
        return FALSE;
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno != 0)
        return FALSE;
    const int step = priority <= THREAD_PRIORITY_IDLE ? 19 : -priority * 2;
    return setpriority(PRIO_PROCESS, (id_t)tid, std::min(19, nice + step)) == 0;
}

int GetThreadPriority(HANDLE)
{
    return THREAD_PRIORITY_NORMAL;
}

HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name)
{
    Handle *h = static_cast<Handle *>(thread);
    if (name == nullptr || h == nullptr)
        return E_INVALIDARG;
    char narrow[16];
    size_t n = 0;
    for (; name[n] != 0 && n < sizeof(narrow) - 1; n++)
        narrow[n] = name[n] < 0x80 ? (char)name[n] : '?';
    narrow[n] = 0;
    const pthread_t t = h == kCurrentThread ? pthread_self() : h->thread;
    return pthread_setname_np(t, narrow) == 0 ? S_OK : E_FAIL;
}

BOOL SwitchToThread(void)
{
    return sched_yield() == 0;
}

void GetSystemInfo(SYSTEM_INFO *si)
{
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD, DWORD)
{
    void *p = mmap(address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD)
{
    /* (MEM_RELEASE's size 0 means the whole block: the caller here gives it) */
    return size != 0 && munmap(address, size) == 0;
}

USHORT CaptureStackBackTrace(ULONG skip, ULONG count, void **frames, ULONG *hash)
{
    void *all[64];
    const int n = backtrace(all, (int)std::min<ULONG>(64, skip + 1 + count));
    int k = 0;
    for (int i = (int)skip + 1; i < n && k < (int)count; i++)    /* (+1: this function) */
        frames[k++] = all[i];
    if (hash != nullptr) {
        ULONG s = 0;
        for (int i = 0; i < k; i++)
            s += (ULONG)(uintptr_t)frames[i];
        *hash = s;
    }
    return (USHORT)k;
}

DWORD GetModuleFileNameA(HMODULE, char *path, DWORD size)
{
    if (size == 0)
        return 0;
    const std::string exe = fs::exe_path();
    const size_t n = std::min<size_t>(exe.size(), size - 1);
    std::memcpy(path, exe.data(), n);
    path[n] = 0;
    return (DWORD)n;
}

BOOL CreateDirectoryA(const char *path, void *)
{
    return fs::make_dir(path) ? TRUE : FALSE;
}

DWORD GetFileAttributesA(const char *path)
{
    fs::Stat st;
    if (!fs::stat(path, &st))
        return INVALID_FILE_ATTRIBUTES;
    return st.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
