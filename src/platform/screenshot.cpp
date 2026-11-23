/* screenshot.cpp - screenshots (screenshot.h). */
#include "platform/screenshot.h"
#include "platform/log.h"
#include "platform/osd.h"
#include "platform/paths.h"
#include "platform/png_image.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages */
#include "platform/os/fs.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string>
#include <thread>

namespace screenshot {
namespace {

/* At most this many written at once (each holds its picture: up to 256 MB
 * at the largest resolution, in a 32-bit process) */
constexpr int kMaxWriting = 2;

std::atomic<bool> g_asked{false};           /* asked for, not yet taken by the renderer */
std::atomic<int> g_taking{0};               /* taken, its picture not yet handed over */
std::atomic<int> g_writing{0};              /* handed over, being written */
std::mutex g_write;                         /* one written at a time: the names chosen in order */

/* The local time a screenshot was taken at. */
struct Stamp {
    unsigned year, month, day, hour, minute, second;
};

Stamp local_now()
{
#ifdef _WIN32
    SYSTEMTIME at;
    GetLocalTime(&at);
    return {at.wYear, at.wMonth, at.wDay, at.wHour, at.wMinute, at.wSecond};
#else
    const time_t t = time(nullptr);
    struct tm at = {};
    localtime_r(&t, &at);
    return {(unsigned)at.tm_year + 1900u, (unsigned)at.tm_mon + 1u, (unsigned)at.tm_mday, (unsigned)at.tm_hour,
            (unsigned)at.tm_min, (unsigned)at.tm_sec};
#endif
}

/* The calling thread below the game's (it writes in the background). */
void lower_priority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    const id_t tid = (id_t)syscall(SYS_gettid);
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, tid);
    if (errno == 0 && nice < 19)
        setpriority(PRIO_PROCESS, tid, nice + 2);
#endif
}

/* The picture as screenshots\gt3pc_<date>_<time>[_N].png (the time it was
 * taken; _2 .. _99 after one of the same second); a message either way.
 * (The folder's path is native text: the ANSI code page on Windows.) */
void write(const std::vector<uint8_t> &px, int w, int h, const Stamp &at)
{
    std::lock_guard<std::mutex> hold(g_write);
    const std::string &dir = paths::screenshots_dir();
    const std::string udir = fs::native_to_utf8(dir);
    fs::make_dir(udir);                         /* (there already: nothing) */
    char stem[48], file[64] = "";
    snprintf(stem, sizeof(stem), "gt3pc_%04u-%02u-%02u_%02u-%02u-%02u", at.year, at.month, at.day, at.hour,
             at.minute, at.second);
    for (int n = 1; n < 100; n++) {
        if (n == 1)
            snprintf(file, sizeof(file), "%s.png", stem);
        else
            snprintf(file, sizeof(file), "%s_%d.png", stem, n);
        if (!fs::exists(udir + file))
            break;
    }
    const bool ok = png_image::encode_file(fs::widen(udir + file), px.data(), w, h);
    char msg[256];                              /* PC (2026-11-16): 64 before (UTF-8 translations) */
    if (ok) {
        pclog::write("screenshot: %s%s (%d x %d)", dir.c_str(), file, w, h);
        snprintf(msg, sizeof(msg), tr("SCREENSHOT: %s %d X %d"), file, w, h);
    } else {
        pclog::write("screenshot: %s%s (%d x %d) could not be written", dir.c_str(), file, w, h);
        snprintf(msg, sizeof(msg), "%s", tr("SCREENSHOT COULD NOT BE WRITTEN (SEE THE LOG)"));
    }
    osd::show(msg);
}

} // namespace

void request()
{
    if (g_writing.load() >= kMaxWriting) {
        osd::show(tr("SCREENSHOT: STILL WRITING THE LAST ONES"));
        return;
    }
    g_asked = true;
}

bool take_request()
{
    if (!g_asked.exchange(false))
        return false;
    g_taking = 1;
    return true;
}

void failed(const char *why)
{
    pclog::write("screenshot: not taken: %s", why);
    /* (PC 2026-11-16: `why' in English, marked N_ where written; shown
     * translated) */
    char msg[256];
    snprintf(msg, sizeof(msg), tr("NO SCREENSHOT: %s"), tr(why));
    osd::show(msg);
    g_taking = 0;
}

void save(std::vector<uint8_t> &&rgba, int w, int h)
{
    const Stamp at = local_now();
    g_writing++;                            /* (before: pending() never 0 meanwhile) */
    g_taking = 0;
    try {
        std::thread([px = std::move(rgba), w, h, at] {
            lower_priority();               /* (the game first) */
            write(px, w, h, at);
            g_writing--;
        }).detach();
    } catch (...) {
        g_writing--;
        failed(N_("NO THREAD TO WRITE IT"));
    }
}

int pending()
{
    return (g_asked.load() ? 1 : 0) + g_taking.load() + g_writing.load();
}

} // namespace screenshot
