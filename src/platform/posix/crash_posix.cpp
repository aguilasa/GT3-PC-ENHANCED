/* crash_posix.cpp - the crash report of GT3 PC off Windows (crash.h).
 *
 * A fatal signal's handler (sigaction, on an alternate signal stack: a
 * stack overflow still gets its report) writes what the Windows filter
 * does: the signal and the faulting address, the memory, the registers,
 * the function the fault is in, the return addresses found on the stack
 * (a scan, as on Windows: the optimised code keeps no frame-pointer chain)
 * and the frames backtrace() unwinds (from the unwind tables).  Then the
 * signal's default action runs, so the system writes its core dump as it
 * would without the handler (ulimit -c, systemd-coredump).
 *
 * The functions are named from the program's own symbol table (.symtab,
 * else .dynsym; read once by install(), C++ names demangled), other
 * modules' by dladdr.  No line numbers: the report names the image's load
 * address and its addr2line command.  The stack scan copies the stack
 * through a pipe (write() fails with EFAULT on memory that is not there,
 * where a read would fault again).
 *
 * log_stack asks the thread itself (pthread_kill with a signal of its own)
 * to copy its instruction pointer and its stack, from the stack pointer to
 * the top its pthread attributes give, then describes the copy. */
#include "platform/crash.h"
#include "platform/log.h"
#include "platform/memstat.h"
#include "platform/scheduler.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace crash {
namespace {

std::atomic<bool> g_inCrash{false};

/* ---- the program's image and its functions ---------------------------- */

struct Function {
    uintptr_t addr;                 /* in memory (the load bias added) */
    uint32_t size;
    uint32_t name;                  /* offset in g_names */
};

std::once_flag g_imageOnce;
uintptr_t g_bias;                   /* the program's load bias (PIE; 0 otherwise) */
uintptr_t g_codeLo, g_codeHi;       /* its executable segments in memory */
char g_exe[1024];                   /* its path */
const char *g_exeName = "gt3pc";    /* its file name (in g_exe) */
Function *g_functions;
size_t g_functionCount;
char *g_names;

/* the demangler's output (one at a time; busy: the mangled name is shown) */
std::atomic<bool> g_demangleBusy{false};
char *g_demangled;
size_t g_demangledSize;

int find_program(struct dl_phdr_info *info, size_t, void *)
{
    /* (the first object is the program itself) */
    g_bias = (uintptr_t)info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0)
            continue;
        const uintptr_t lo = g_bias + ph.p_vaddr, hi = lo + ph.p_memsz;
        if (g_codeHi == 0 || lo < g_codeLo)
            g_codeLo = lo;
        if (hi > g_codeHi)
            g_codeHi = hi;
    }
    return 1;
}

bool read_at(int fd, void *out, size_t n, off_t at)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t r = pread(fd, (char *)out + done, n - done, at + (off_t)done);
        if (r <= 0)
            return false;
        done += (size_t)r;
    }
    return true;
}

/* The function symbols of the program's file. */
void load_functions()
{
    const int fd = open(g_exe, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    Elf32_Ehdr eh;
    Elf32_Shdr *sh = nullptr;
    Elf32_Sym *syms = nullptr;
    if (!read_at(fd, &eh, sizeof(eh), 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_shentsize != sizeof(Elf32_Shdr) || eh.e_shnum == 0)
        goto done;
    sh = (Elf32_Shdr *)malloc(sizeof(Elf32_Shdr) * eh.e_shnum);
    if (sh == nullptr || !read_at(fd, sh, sizeof(Elf32_Shdr) * eh.e_shnum, eh.e_shoff))
        goto done;
    for (int pass = 0; pass < 2 && g_functions == nullptr; pass++) {
        const Elf32_Word want = pass == 0 ? SHT_SYMTAB : SHT_DYNSYM;
        for (unsigned i = 0; i < eh.e_shnum; i++) {
            if (sh[i].sh_type != want || sh[i].sh_link >= eh.e_shnum || sh[i].sh_entsize != sizeof(Elf32_Sym))
                continue;
            const Elf32_Shdr &strs = sh[sh[i].sh_link];
            const size_t count = sh[i].sh_size / sizeof(Elf32_Sym);
            syms = (Elf32_Sym *)malloc(sh[i].sh_size);
            g_names = (char *)malloc(strs.sh_size + 1);
            if (syms == nullptr || g_names == nullptr || !read_at(fd, syms, sh[i].sh_size, sh[i].sh_offset) ||
                !read_at(fd, g_names, strs.sh_size, strs.sh_offset)) {
                free(g_names);
                g_names = nullptr;
                break;
            }
            g_names[strs.sh_size] = '\0';
            size_t n = 0;
            for (size_t k = 0; k < count; k++)
                if (ELF32_ST_TYPE(syms[k].st_info) == STT_FUNC && syms[k].st_value != 0 &&
                    syms[k].st_shndx != SHN_UNDEF && syms[k].st_name < strs.sh_size)
                    n++;
            g_functions = (Function *)malloc(sizeof(Function) * (n ? n : 1));
            if (g_functions == nullptr)
                break;
            for (size_t k = 0; k < count; k++)
                if (ELF32_ST_TYPE(syms[k].st_info) == STT_FUNC && syms[k].st_value != 0 &&
                    syms[k].st_shndx != SHN_UNDEF && syms[k].st_name < strs.sh_size)
                    g_functions[g_functionCount++] = {g_bias + syms[k].st_value, syms[k].st_size, syms[k].st_name};
            std::sort(g_functions, g_functions + g_functionCount,
                      [](const Function &a, const Function &b) { return a.addr < b.addr; });
            break;
        }
        free(syms);
        syms = nullptr;
    }
done:
    free(syms);
    free(sh);
    close(fd);
}

void load_image()
{
    const ssize_t n = readlink("/proc/self/exe", g_exe, sizeof(g_exe) - 1);
    g_exe[n > 0 ? n : 0] = '\0';
    if (const char *slash = strrchr(g_exe, '/'))
        g_exeName = slash + 1;
    dl_iterate_phdr(find_program, nullptr);
    load_functions();
    g_demangledSize = 4096;
    g_demangled = (char *)malloc(g_demangledSize);
}

void ensure_image()
{
    std::call_once(g_imageOnce, load_image);
}

bool in_image(uintptr_t a)
{
    return a >= g_codeLo && a < g_codeHi;
}

const Function *function_at(uintptr_t a)
{
    if (g_functionCount == 0)
        return nullptr;
    const Function *begin = g_functions, *end = g_functions + g_functionCount;
    const Function *f = std::upper_bound(begin, end, a,
                                         [](uintptr_t v, const Function &x) { return v < x.addr; });
    if (f == g_functions)
        return nullptr;
    --f;
    /* (a size of 0: an assembly label, taken up to the next symbol) */
    if (f->size != 0 && a >= f->addr + f->size)
        return nullptr;
    return f;
}

/* A symbol's name, demangled when it can be (into out). */
void put_name(const char *mangled, unsigned disp, char *out, size_t outSize)
{
    if (mangled[0] == '_' && mangled[1] == 'Z' && g_demangled != nullptr && !g_demangleBusy.exchange(true)) {
        int status = -1;
        size_t size = g_demangledSize;
        char *d = abi::__cxa_demangle(mangled, g_demangled, &size, &status);
        if (d != nullptr) {
            g_demangled = d;
            g_demangledSize = std::max(g_demangledSize, size);
        }
        if (status == 0 && d != nullptr) {
            snprintf(out, outSize, "%s+0x%X", d, disp);
            g_demangleBusy = false;
            return;
        }
        g_demangleBusy = false;
    }
    snprintf(out, outSize, "%s+0x%X", mangled, disp);
}

/* "function+0xNN" for a code address (the program's; else "module+0xNN"). */
void describe(uintptr_t addr, char *out, size_t outSize)
{
    if (in_image(addr)) {
        if (const Function *f = function_at(addr)) {
            put_name(g_names + f->name, (unsigned)(addr - f->addr), out, outSize);
            return;
        }
        snprintf(out, outSize, "%s+0x%X", g_exeName, (unsigned)(addr - g_bias));
        return;
    }
    Dl_info info;
    if (addr != 0 && dladdr((const void *)addr, &info) != 0 && info.dli_fname != nullptr) {
        const char *slash = strrchr(info.dli_fname, '/');
        const char *module = slash ? slash + 1 : info.dli_fname;
        if (info.dli_sname != nullptr && info.dli_saddr != nullptr) {
            char name[384];
            put_name(info.dli_sname, (unsigned)(addr - (uintptr_t)info.dli_saddr), name, sizeof(name));
            snprintf(out, outSize, "%s (%s)", name, module);
        } else {
            snprintf(out, outSize, "%s+0x%X", module, (unsigned)(addr - (uintptr_t)info.dli_fbase));
        }
        return;
    }
    snprintf(out, outSize, "0x%08X", (unsigned)addr);
}

/* A word on the stack that points just after a call instruction of the
 * program: a return address (as on Windows). */
bool is_return_address(uintptr_t a)
{
    if (!in_image(a) || a < g_codeLo + 8)
        return false;
    const uint8_t *p = (const uint8_t *)a;
    if (p[-5] == 0xE8)                                  /* call rel32 */
        return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)        /* call reg / [reg] */
        return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)        /* call [reg+d8] */
        return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)        /* call [reg+d32] / [abs] */
        return true;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)        /* call [sib+d32] */
        return true;
    return false;
}

/* ---- memory that may not be there ------------------------------------- */

int g_probe[2] = {-1, -1};          /* a pipe: write() checks the memory, read() takes it back */

bool safe_copy(void *out, const void *from, size_t n)
{
    if (g_probe[1] < 0)
        return false;
    const ssize_t w = write(g_probe[1], from, n);
    if (w != (ssize_t)n) {
        if (w > 0) {
            char sink[256];
            for (ssize_t left = w; left > 0;) {
                const ssize_t r = read(g_probe[0], sink, (size_t)std::min<ssize_t>(left, sizeof(sink)));
                if (r <= 0)
                    break;
                left -= r;
            }
        }
        return false;
    }
    return read(g_probe[0], out, n) == (ssize_t)n;
}

/* ---- the alternate signal stack ------------------------------------------ */

struct AltStack {
    void *base = nullptr;
    size_t size = 0;
    ~AltStack()
    {
        if (base == nullptr)
            return;
        stack_t off = {};
        off.ss_flags = SS_DISABLE;
        sigaltstack(&off, nullptr);
        munmap(base, size);
    }
};
thread_local AltStack t_altStack;

/* ---- the report ----------------------------------------------------------- */

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

constexpr int kFatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

void on_crash(int sig, siginfo_t *si, void *ctx)
{
    if (g_inCrash.exchange(true)) {
        /* (another thread's crash is being reported, or the report itself
         * crashed: the default action, at once) */
        signal(sig, SIG_DFL);
        if (si == nullptr || si->si_code <= 0)
            raise(sig);
        return;
    }
    const ucontext_t *uc = (const ucontext_t *)ctx;
    const greg_t *r = uc->uc_mcontext.gregs;
    const uintptr_t eip = (uintptr_t)r[REG_EIP], esp = (uintptr_t)r[REG_ESP];
    char where[512];

    const int gameThread = sched::current_thread();
    char who[48];
    if (gameThread != 0)
        snprintf(who, sizeof(who), "game thread %d", gameThread);
    else
        snprintf(who, sizeof(who), "a native thread");
    pclog::write("crash: %s (signal %d, code %d) at 0x%08X, %s (Linux thread %d)", signal_name(sig), sig,
                 si ? si->si_code : 0, (unsigned)eip, who, (int)syscall(SYS_gettid));
    if ((sig == SIGSEGV || sig == SIGBUS) && si != nullptr) {
        /* (the page fault's error code: bit 1 a write, bit 4 an instruction fetch) */
        const unsigned err = (unsigned)r[REG_ERR];
        const bool pageFault = r[REG_TRAPNO] == 14;
        pclog::write("crash: %s address 0x%08X%s",
                     !pageFault ? "at" : (err & 16) ? "executing" : (err & 2) ? "writing" : "reading",
                     (unsigned)(uintptr_t)si->si_addr,
                     sig == SIGSEGV ? (si->si_code == SEGV_MAPERR ? " (not mapped)" :
                                       si->si_code == SEGV_ACCERR ? " (not allowed)" : "") : "");
    }
    if (sig == SIGABRT)
        pclog::write("crash: abort() - a failed check, or std::terminate (a C++ exception nobody caught:"
                     " std::bad_alloc from new when the memory is out)");
    char mem[256];
    memstat::line(mem, sizeof(mem), false);
    pclog::write("crash: memory: %s", mem);
    pclog::write("crash: eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X eip %08X",
                 (unsigned)r[REG_EAX], (unsigned)r[REG_EBX], (unsigned)r[REG_ECX], (unsigned)r[REG_EDX],
                 (unsigned)r[REG_ESI], (unsigned)r[REG_EDI], (unsigned)r[REG_EBP], (unsigned)esp, (unsigned)eip);
    describe(eip, where, sizeof(where));
    pclog::write("crash: in %s", where);
    pclog::write("crash: image %s at 0x%08X (lines: addr2line -f -C -e %s <address - 0x%08X>)", g_exeName,
                 (unsigned)g_bias, g_exe, (unsigned)g_bias);

    /* The return addresses on the stack, nearest first (copied a page at a
     * time, up to where the memory ends). */
    int found = 0;
    uintptr_t at = esp & ~(uintptr_t)3;
    for (int i = 0; i < 4096 && found < 24;) {
        uintptr_t words[256];
        const uintptr_t pageEnd = (at | 0xFFF) + 1;
        size_t n = std::min<size_t>((pageEnd - at) / sizeof(uintptr_t), 256);
        if (n == 0 || !safe_copy(words, (const void *)at, n * sizeof(uintptr_t)))
            break;
        for (size_t k = 0; k < n && i < 4096 && found < 24; k++, i++) {
            if (!is_return_address(words[k]))
                continue;
            describe(words[k], where, sizeof(where));
            pclog::write("crash:   [esp+0x%03X] %s", (unsigned)(i * 4), where);
            found++;
        }
        at += n * sizeof(uintptr_t);
    }

    struct rlimit core;
    if (getrlimit(RLIMIT_CORE, &core) == 0 && core.rlim_cur == 0)
        pclog::write("crash: no core dump (its size limit is 0: ulimit -c)");
    else
        pclog::write("crash: the system's core dump follows (ulimit -c, systemd-coredump: coredumpctl)");

    /* The frames the unwind tables give (last: an unwinder on a broken
     * stack may fault, and the lines above are written already). */
    void *frames[32];
    const int count = backtrace(frames, 32);
    int first = 0;
    for (int i = 0; i < count; i++)
        if ((uintptr_t)frames[i] == eip) {
            first = i;
            break;
        }
    for (int i = first; i < count; i++) {
        /* (a return address: its call is the byte before) */
        const uintptr_t a = (uintptr_t)frames[i];
        describe(i == first ? a : a - 1, where, sizeof(where));
        pclog::write("crash:   #%d %s", i - first, where);
    }
    pclog::close();
    /* SA_RESETHAND gave the default action back: a fault happens again on
     * the return; a signal sent (abort, kill) is raised again. */
    if (si == nullptr || si->si_code <= 0)
        raise(sig);
}

/* ---- another thread's stack ------------------------------------------------ */

enum { kMaxWords = 64 * 1024, kMaxFound = 40 };
enum { kIdle, kWaiting, kCopying, kDone };

struct Request {
    std::atomic<int> state{kIdle};
    pthread_t target;
    uintptr_t stackHi;
    uintptr_t eip, esp;
    size_t count;
    sem_t done;
};
Request g_req;
uintptr_t g_words[kMaxWords];       /* (one report at a time: g_logStackMutex) */
std::mutex g_logStackMutex;
std::once_flag g_sampleOnce;
int g_sampleSignal;

void on_sample(int, siginfo_t *, void *ctx)
{
    const int savedErrno = errno;
    int expect = kWaiting;
    if (pthread_equal(pthread_self(), g_req.target) && g_req.state.compare_exchange_strong(expect, kCopying)) {
        const ucontext_t *uc = (const ucontext_t *)ctx;
        g_req.eip = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
        g_req.esp = (uintptr_t)uc->uc_mcontext.gregs[REG_ESP];
        size_t n = 0;
        if (g_req.stackHi > g_req.esp)
            n = std::min<size_t>((g_req.stackHi - g_req.esp) / sizeof(uintptr_t), kMaxWords);
        memcpy(g_words, (const void *)g_req.esp, n * sizeof(uintptr_t));
        g_req.count = n;
        g_req.state = kDone;
        sem_post(&g_req.done);
    }
    errno = savedErrno;
}

void install_sample_handler()
{
    sem_init(&g_req.done, 0, 0);
    /* (a real-time signal above the C library's own) */
    g_sampleSignal = SIGRTMIN + 3;
    struct sigaction sa = {};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(g_sampleSignal, &sa, nullptr);
}

} // namespace

void log_stack(void *thread, const char *prefix)
{
    ensure_image();
    std::call_once(g_sampleOnce, install_sample_handler);
    std::lock_guard<std::mutex> one(g_logStackMutex);
    const pthread_t t = (pthread_t)(uintptr_t)thread;
    if (pthread_equal(t, pthread_self())) {
        pclog::write("%sthe calling thread (not reported)", prefix);
        return;
    }
    pthread_attr_t attr;
    void *lo = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(t, &attr) != 0) {
        pclog::write("%sno stack bounds for the thread", prefix);
        return;
    }
    pthread_attr_getstack(&attr, &lo, &size);
    pthread_attr_destroy(&attr);

    g_req.target = t;
    g_req.stackHi = (uintptr_t)lo + size;
    g_req.count = 0;
    g_req.state = kWaiting;
    const int err = pthread_kill(t, g_sampleSignal);
    if (err != 0) {
        g_req.state = kIdle;
        pclog::write("%scould not signal the thread (error %d)", prefix, err);
        return;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 1;
    int w;
    while ((w = sem_timedwait(&g_req.done, &until)) != 0 && errno == EINTR) {
    }
    if (w != 0) {
        int expect = kWaiting;
        if (g_req.state.compare_exchange_strong(expect, kIdle)) {
            pclog::write("%sthe thread did not answer", prefix);
            return;
        }
        while (sem_wait(&g_req.done) != 0 && errno == EINTR) {    /* (copying: almost done) */
        }
    }
    const uintptr_t eip = g_req.eip;
    const size_t count = g_req.count;
    g_req.state = kIdle;

    char where[512];
    if (in_image(eip)) {
        describe(eip, where, sizeof(where));
        pclog::write("%sat %s", prefix, where);
    }
    int found = 0;
    for (size_t i = 0; i < count && found < kMaxFound; i++) {
        if (!is_return_address(g_words[i]))
            continue;
        describe(g_words[i], where, sizeof(where));
        pclog::write("%s[esp+0x%05X] %s", prefix, (unsigned)(i * 4), where);
        found++;
    }
}

void describe_address(const void *addr, char *out, unsigned outSize)
{
    ensure_image();
    describe((uintptr_t)addr, out, outSize);
}

void prepare_thread()
{
    if (t_altStack.base != nullptr)
        return;
    stack_t old = {};
    if (sigaltstack(nullptr, &old) == 0 && (old.ss_flags & SS_DISABLE) == 0)
        return;                             /* (one of its own already) */
    long min = sysconf(_SC_SIGSTKSZ);
    const size_t size = std::max<size_t>(64 * 1024, min > 0 ? (size_t)min : 0);
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return;
    stack_t ss = {};
    ss.ss_sp = p;
    ss.ss_size = size;
    if (sigaltstack(&ss, nullptr) != 0) {
        munmap(p, size);
        return;
    }
    t_altStack.base = p;
    t_altStack.size = size;
}

void install()
{
    ensure_image();
    std::call_once(g_sampleOnce, install_sample_handler);
    /* (backtrace loads the unwinder on its first call: not in the handler) */
    void *warm[2];
    backtrace(warm, 2);
    if (pipe2(g_probe, O_CLOEXEC | O_NONBLOCK) != 0)
        g_probe[0] = g_probe[1] = -1;
    prepare_thread();
    struct sigaction sa = {};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : kFatal)
        sigaction(sig, &sa, nullptr);
    pclog::write("crash: report installed (%u functions named from %s)", (unsigned)g_functionCount, g_exeName);
}

} // namespace crash
