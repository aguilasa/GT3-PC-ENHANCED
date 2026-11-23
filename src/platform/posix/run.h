/* run.h - the process side of the program's entry off Windows
 * (src/main/main_posix.cpp): the program's folder, the log's rotation and
 * the Esc menu's Restart, as WinMain has them.
 *
 * Restart: Windows starts a new process with this one's arguments and
 * --restart-after <its id>, which waits for this one to end.  Here the
 * process replaces itself (execv of the program's own file): the same
 * process id, the files opened close-on-exec closed, the other threads
 * gone - nothing to wait for.  A --restart-after given anyway (a launcher's)
 * is still honoured for another process id. */
#pragma once

#include <string>
#include <vector>

namespace pcrun {

/* The program's folder (from /proc/self/exe), with a trailing '/'; empty
 * when it cannot be read. */
std::string exe_dir();

/* The log's path in `dir' (gt3pc.log), the last run's kept as
 * gt3pc.prev.log (a crash report survives a restart). */
std::string rotate_log(const std::string &dir);

/* --restart-after <pid> in the arguments: waits (up to 15 s) for that
 * process to end, unless it is this one. */
void wait_for_previous_run(int argc, char **argv);

/* --new-game in the arguments (Restart as new game: no autoload, for one
 * run). */
bool new_game_run(int argc, char **argv);

/* The new run's arguments: the program's path, then this run's arguments
 * without --restart-after <pid> and --new-game, then --new-game when
 * `newGame'. */
std::vector<std::string> restart_args(const std::string &exe, int argc, char **argv, bool newGame);

/* Replaces the process with a new run of the program (restart_args); only
 * returns when that fails (the reason on the standard error: the log is
 * closed by then). */
void restart(int argc, char **argv, bool newGame);

} // namespace pcrun
