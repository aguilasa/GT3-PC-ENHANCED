/* user_dir.h - the folder the programs write their files into off Windows.
 *
 * On Windows the game and the launcher keep their files in the folder of
 * the executable: gt3pc.ini, gt3pc_engine.ini, the log, the save folders,
 * the screenshots, the caches and the installed game files (data_US ...).
 * Off Windows that folder may be read-only (an AppImage, mounted from a
 * read-only image; a system install).  Then the files go to a folder of the
 * user's instead, $XDG_DATA_HOME/gt3pc (~/.local/share/gt3pc).  An
 * executable's folder that can be written to keeps them, as on Windows.
 * $GT3PC_HOME, when it is set, is the folder either way: the AppImage's
 * AppRun sets it, since an AppImage run without FUSE is extracted into a
 * temporary folder that can be written to and is removed at the exit.
 *
 * Header only: the game (src/main/main_posix.cpp, platform/paths.cpp) and
 * the launcher (src/launcher/main_posix.cpp) both use it. */
#pragma once

#ifndef _WIN32

#include "platform/os/fs.h"

#include <stdlib.h>
#include <unistd.h>

#include <string>

namespace pcrun {

/* The folder to write into, with a trailing '/', for an executable in
 * `exeDir` (with its trailing '/'): $GT3PC_HOME when it is set, else
 * `exeDir` when it can be written to, else the user's folder (made when it
 * is not there yet).  An empty `exeDir` (the executable's path not known)
 * is kept as it is. */
inline std::string user_dir(const std::string &exeDir)
{
    std::string dir;
    const char *own = getenv("GT3PC_HOME");
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (own != nullptr && own[0] != 0)
        dir = own;
    else if (exeDir.empty() || access(exeDir.c_str(), W_OK) == 0)
        return exeDir;
    else if (xdg != nullptr && xdg[0] == '/')     /* (the specification: a relative one is ignored) */
        dir = std::string(xdg) + "/gt3pc";
    else if (home != nullptr && home[0] != 0)
        dir = std::string(home) + "/.local/share/gt3pc";
    else
        return exeDir;
    dir = fs::full_path(dir);
    fs::make_dirs(dir);
    if (dir.empty() || dir.back() != '/')
        dir += '/';
    return dir;
}

} // namespace pcrun

#endif
