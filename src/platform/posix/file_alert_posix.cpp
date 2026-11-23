/* file_alert_posix.cpp - files in use by another program, off Windows
 * (file_alert.h): no message box, a notice over the picture instead
 * (platform/osd.h, renewed while the files stay in use) and the whole text
 * in the log.  The game goes on waiting behind it as on Windows; the notice
 * goes when the files are free (an empty list). */
#include "platform/file_alert.h"
#include "platform/fpu.h"
#include "platform/log.h"
#include "platform/osd.h"

#include <stdio.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace file_alert {
namespace {

/* osd::show lasts 2 s: renewed sooner. */
constexpr auto kRenew = std::chrono::milliseconds(1500);

std::mutex g_mutex;
std::condition_variable g_wake;
bool g_started;                             /* the renewing thread */
std::string g_notice;                       /* what is shown (empty: nothing) */
std::vector<std::string> g_named;           /* the files of the last call */

std::string base_name(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string notice_of(const std::vector<std::string> &files)
{
    char t[256];
    if (files.size() == 1)
        snprintf(t, sizeof(t), "%s is in use by another program - waiting for it", base_name(files[0]).c_str());
    else
        snprintf(t, sizeof(t), "%u game files are in use by another program - waiting for them",
                 (unsigned)files.size());
    return t;
}

void renewer()
{
    /* (a native thread a game thread started: the default FPU mode, not the
     * game's it inherits) */
    fpu::set_mode(fpu::kDefaultMode);
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        if (!g_notice.empty())
            osd::show(g_notice.c_str());
        g_wake.wait_for(lock, kRenew);
    }
}

} // namespace

void show(const std::vector<std::string> &files)
{
    if (files == g_named)
        return;
    if (files.empty()) {
        pclog::write("file alert: the files are free again");
    } else {
        std::string all;
        for (const std::string &f : files)
            all += "\n    " + f;
        pclog::write("file alert: %s:%s", files.size() == 1 ? "this game file is in use by another program"
                                                            : "these game files are in use by another program",
                     all.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_notice = files.empty() ? std::string() : notice_of(files);
        if (!g_started && !files.empty()) {
            std::thread(renewer).detach();  /* (it sleeps while nothing is shown; the process ends it) */
            g_started = true;
        }
    }
    g_wake.notify_one();
    g_named = files;
}

void leave_fullscreen()
{
    /* a notice drawn in the picture needs no leaving of fullscreen */
}

} // namespace file_alert
