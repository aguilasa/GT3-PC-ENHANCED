/* run_posix.cpp - the process side of the program's entry off Windows
 * (run.h). */
#include "platform/posix/run.h"
#include "platform/os/fs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace pcrun {
namespace {

constexpr char kRestartAfter[] = "--restart-after";
constexpr char kNewGame[] = "--new-game";

} // namespace

std::string exe_dir()
{
    std::string exe = fs::exe_path();
    const size_t slash = exe.find_last_of('/');
    if (slash == std::string::npos)
        return std::string();
    exe.resize(slash + 1);
    return exe;
}

std::string rotate_log(const std::string &dir)
{
    const std::string log = dir + "gt3pc.log";
    /* (rename replaces the old copy in one step; no log yet: nothing kept) */
    if (::rename(log.c_str(), (dir + "gt3pc.prev.log").c_str()) != 0 && errno != ENOENT)
        fprintf(stderr, "gt3pc: the last log could not be kept: %s\n", strerror(errno));
    return log;
}

void wait_for_previous_run(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], kRestartAfter) != 0)
            continue;
        const pid_t pid = (pid_t)strtol(argv[i + 1], nullptr, 10);
        if (pid <= 0 || pid == getpid())
            return;
        /* (no handle to wait on another process by: its id polled) */
        for (int ms = 0; ms < 15000 && (kill(pid, 0) == 0 || errno == EPERM); ms += 20) {
            struct timespec d = {0, 20 * 1000 * 1000};
            nanosleep(&d, nullptr);
        }
        return;
    }
}

bool new_game_run(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], kNewGame) == 0)
            return true;
    return false;
}

std::vector<std::string> restart_args(const std::string &exe, int argc, char **argv, bool newGame)
{
    std::vector<std::string> args{exe};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], kRestartAfter) == 0) {
            i++;                                /* (the last run's) */
            continue;
        }
        if (strcmp(argv[i], kNewGame) == 0)
            continue;                           /* (the last run's: a Restart from it loads as usual) */
        args.push_back(argv[i]);
    }
    if (newGame)
        args.push_back(kNewGame);
    return args;
}

void restart(int argc, char **argv, bool newGame)
{
    const std::string exe = fs::exe_path();
    if (exe.empty()) {
        fprintf(stderr, "gt3pc: restart: the program's path is not known\n");
        return;
    }
    const std::vector<std::string> args = restart_args(exe, argc, argv, newGame);
    std::vector<char *> v;
    for (const std::string &a : args)
        v.push_back(const_cast<char *>(a.c_str()));
    v.push_back(nullptr);
    execv(exe.c_str(), v.data());
    fprintf(stderr, "gt3pc: restart: %s could not be started: %s\n", exe.c_str(), strerror(errno));
}

} // namespace pcrun
