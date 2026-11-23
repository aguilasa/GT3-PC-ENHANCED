/* pc_data_file.cpp - PC: a database of the data folder read whole (see
 * pc_data_file.h). */
#include "libc.h"
#include "game/app/pc_data_file.h"
#include "eekernel.h"                           /* DelayThread */
#include "platform/file_alert.h"
#include "platform/host_file.h"
#include "platform/sqlite_file.h"

#include <string>
#include <vector>

extern int g_00350B18;                          /* T002: the render thread (-1: none yet) */

namespace {

constexpr int kMaxBytes = 256 << 20;
constexpr int kRetryUs = 250000;

/* (host_file_present, never file_locate: file_resolve's io_getstat finds a file
 * by opening it, so a held file is "not found" there) */
std::string host_path(const std::string &path)
{
    return "host:data/" + path;
}

/* <name>.sqlite3: read through SQLite (platform/sqlite_file.h). */
bool is_database(const std::string &path)
{
    static const char kExt[] = ".sqlite3";
    const size_t n = sizeof(kExt) - 1;
    return path.size() > n && _stricmp(path.c_str() + path.size() - n, kExt) == 0;
}

/* The path of the file read (a mod's, else the data folder's): the alert
 * names it. */
std::string full_path(const std::string &host)
{
    char full[1024];
    return host_file_full_path(host.c_str(), full, sizeof(full)) == 0 ? std::string(full) : host;
}

/* A wait's start: the log, the alert over the game (before the first frame:
 * out of exclusive fullscreen, where a box stays hidden; the swap chain is
 * free then). */
void wait_begin(const std::string &path, const std::string &host, const char *why)
{
    printf("PC: %s %s: waiting for it\n", path.c_str(), why);
    if (g_00350B18 < 0)
        file_alert::leave_fullscreen();
    file_alert::show(std::vector<std::string>{full_path(host)});
}

void wait_end(const std::string &path)
{
    printf("PC: %s is free again\n", path.c_str());
    file_alert::show(std::vector<std::string>());
}

/* An SQLite database, through SQLite's own connection and locks: an editor
 * connected to it does not hold it up; a transaction of its under way (a
 * commit, a change it has not finished) is waited for. */
bool read_database(const std::string &path, const std::string &host, std::string *out)
{
    char full[4096];                            /* (UTF-8: SQLite's, which spells any file's name) */
    if (host_file_full_path_utf8(host.c_str(), full, sizeof(full)) != 0)
        return false;
    bool waited = false;
    for (;;) {
        std::string error;
        const sqlite_file::Result r = sqlite_file::read(full, out, &error);
        if (r == sqlite_file::Result::ok)
            break;
        if (r != sqlite_file::Result::busy) {
            printf("PC: %s cannot be read as an SQLite database: %s\n", path.c_str(), error.c_str());
            if (waited)
                wait_end(path);
            return false;
        }
        if (!waited)
            wait_begin(path, host, "is locked by another program's transaction");
        waited = true;
        DelayThread(kRetryUs);
    }
    if (waited)
        wait_end(path);
    return true;
}

/* Any other file: its bytes, once no other program holds it for writing. */
bool read_plain(const std::string &path, const std::string &host, std::string *out)
{
    bool waited = false;
    int fd;
    host_file_quiet(1);
    for (;;) {
        fd = host_file_open(host.c_str());
        const unsigned held = host_file_held();
        if (held == 0)
            break;
        /* (a mod's held: not the data folder's copy in its place) */
        if (fd >= 0)
            host_file_close(fd);
        if (!waited) {
            char why[80];
#ifdef _WIN32
            snprintf(why, sizeof(why), "is held by another program (Windows error %u)", held);
#else
            snprintf(why, sizeof(why), "is held by another program (error %u)", held);
#endif
            wait_begin(path, host, why);
        }
        waited = true;
        DelayThread(kRetryUs);
    }
    if (waited)
        wait_end(path);
    host_file_quiet(0);
    if (fd < 0)
        return false;
    const int64_t n = host_file_size(fd);
    out->clear();
    bool ok = n >= 0 && n < kMaxBytes;
    if (ok && n > 0) {
        out->resize((size_t)n);
        ok = host_file_read_at(fd, 0, &(*out)[0], (uint32_t)n) == (int)n;
    }
    host_file_close(fd);
    return ok;
}

} // namespace

bool pc_data_file_read(const std::string &path, std::string *out)
{
    const std::string host = host_path(path);
    if (!host_file_present(host.c_str()))
        return false;
    return is_database(path) ? read_database(path, host, out) : read_plain(path, host, out);
}
