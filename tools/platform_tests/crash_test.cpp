/* crash_test - the crash report on POSIX (src/platform/posix/crash_posix.cpp):
 *   1. a child that writes through a null pointer in probe::guilty_write
 *      dies of SIGSEGV, and its log has the report: the signal, the address
 *      written, "crash: in probe::guilty_write" and its caller,
 *      probe::caller_of_guilty, on the stack;
 *   2. the control: the same child without crash::install() dies the same
 *      way and its log names neither (what check 1 sees is the report's);
 *   3. crash::log_stack of another thread (a pthread parked in
 *      probe::parked_function) names that function.
 * Exit code 0 when everything passes.  The children are this program run
 * again (crash_test segv|nosig|stack <log>). */
#include "platform/crash.h"
#include "platform/log.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>

/* ---- what crash_posix.cpp links against (the game scheduler's thread id) */

namespace sched {
int current_thread()
{
    return 0;
}
} // namespace sched

/* ---- the functions the report must name ---------------------------------- */

namespace probe {

__attribute__((noinline)) void guilty_write(volatile int *p)
{
    *p = 42;
}

__attribute__((noinline)) void caller_of_guilty()
{
    guilty_write(nullptr);
    __asm__ __volatile__("" ::: "memory");      /* (the call is not a tail call) */
}

std::atomic<bool> g_parked{false};
sem_t g_never;

__attribute__((noinline)) void parked_function()
{
    g_parked = true;
    while (sem_wait(&g_never) != 0) {
    }
    __asm__ __volatile__("" ::: "memory");
}

void *parked_thread(void *)
{
    parked_function();
    return nullptr;
}

} // namespace probe

/* ---- the children ----------------------------------------------------------- */

static int child(const char *mode, const char *log)
{
    struct rlimit none = {0, 0};
    setrlimit(RLIMIT_CORE, &none);              /* (no core files from the test) */
    pclog::open(log);
    if (strcmp(mode, "nosig") != 0)
        crash::install();
    if (strcmp(mode, "stack") == 0) {
        sem_init(&probe::g_never, 0, 0);
        pthread_t t;
        pthread_create(&t, nullptr, probe::parked_thread, nullptr);
        while (!probe::g_parked)
            usleep(1000);
        usleep(20000);                          /* (into its wait) */
        crash::log_stack((void *)(uintptr_t)t, "stack: ");
        pclog::close();
        return 0;
    }
    probe::caller_of_guilty();
    return 0;
}

/* ---- the checks ------------------------------------------------------------- */

static int g_failures;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_failures++;
}

static std::string slurp(const std::string &path)
{
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/* the lines of `text' holding `what' */
static void show(const std::string &text, const char *what)
{
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (line.find(what) != std::string::npos)
            printf("    | %s\n", line.c_str());
}

static int run_child(const char *self, const char *mode, const std::string &log)
{
    const pid_t pid = fork();
    if (pid == 0) {
        execl(self, self, mode, log.c_str(), (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

int main(int argc, char **argv)
{
    if (argc == 3)
        return child(argv[1], argv[2]);

    char dir[] = "/tmp/crash_test_XXXXXX";
    if (mkdtemp(dir) == nullptr) {
        perror("mkdtemp");
        return 2;
    }
    const std::string self = "/proc/self/exe";
    char exe[1024];
    const ssize_t n = readlink(self.c_str(), exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = '\0';

    /* 1. the report */
    const std::string segvLog = std::string(dir) + "/segv.log";
    int st = run_child(exe, "segv", segvLog);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "the child died of SIGSEGV (the default action after the report)");
    std::string text = slurp(segvLog);
    show(text, "crash:");
    check(text.find("crash: SIGSEGV") != std::string::npos, "the log names the signal");
    check(text.find("crash: writing address 0x00000000") != std::string::npos, "the log names the address written");
    check(text.find("crash: in probe::guilty_write(") != std::string::npos, "the fault is in probe::guilty_write");
    check(text.find("] probe::caller_of_guilty(") != std::string::npos,
          "the stack scan finds its caller, probe::caller_of_guilty");
    check(text.find("#1 probe::caller_of_guilty(") != std::string::npos,
          "the unwound stack has probe::caller_of_guilty as frame #1");

    /* 2. the control */
    const std::string nosigLog = std::string(dir) + "/nosig.log";
    st = run_child(exe, "nosig", nosigLog);
    text = slurp(nosigLog);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "control: without the handler the child dies of SIGSEGV too");
    check(text.find("probe::guilty_write") == std::string::npos && text.find("crash:") == std::string::npos,
          "control: and its log has no report");

    /* 3. another thread's stack */
    const std::string stackLog = std::string(dir) + "/stack.log";
    st = run_child(exe, "stack", stackLog);
    text = slurp(stackLog);
    show(text, "stack: ");
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "log_stack's child ended normally");
    check(text.find("stack: [esp+") != std::string::npos && text.find("probe::parked_function(") != std::string::npos,
          "log_stack names probe::parked_function on the other thread's stack");

    unlink(segvLog.c_str());
    unlink(nosigLog.c_str());
    unlink(stackLog.c_str());
    rmdir(dir);
    printf("%s\n", g_failures == 0 ? "crash_test: all passed" : "crash_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
