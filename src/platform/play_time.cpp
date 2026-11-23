/* play_time.cpp - the time played (play_time.h). */
#include "platform/play_time.h"
#include "platform/log.h"
#include "platform/paths.h"

#include "platform/os/win_compat.h"         /* QueryUnbiasedInterruptTime too */

#include <stdio.h>
#include <stdlib.h>

#include <mutex>
#include <string>
#ifndef _WIN32
#include <chrono>
#include <condition_variable>
#include <thread>
#endif

namespace play_time {
namespace {

constexpr const char *kSection = "Stats";
constexpr const char *kKey = "PlayTime";
constexpr DWORD kWriteMs = 60 * 1000;

uint64_t g_base;                            /* the seconds of the runs before this one */
ULONGLONG g_start;                          /* QueryUnbiasedInterruptTime at start (100 ns; no sleep) */
#ifdef _WIN32
HANDLE g_thread;
HANDLE g_stop;
#else
/* No Win32 threads off Windows (CreateThread is also the EE kernel's name,
 * eekernel.h): the writer is a std::thread woken by a condition variable. */
std::thread g_thread;
std::mutex g_stopMutex;
std::condition_variable g_stopCv;
bool g_stopping;
#endif
std::mutex g_mutex;

std::string ini_path()
{
    return paths::ini_path();               /* PC (2026-11-14): the shared one with --shared (paths.h) */
}

void write()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = g_start;
    QueryUnbiasedInterruptTime(&now);
    char v[32];
    snprintf(v, sizeof(v), "%llu", (unsigned long long)(g_base + (now - g_start) / 10000000ull));
    WritePrivateProfileStringA(kSection, kKey, v, ini_path().c_str());
}

#ifdef _WIN32
DWORD WINAPI writer(void *)
{
    while (WaitForSingleObject(g_stop, kWriteMs) == WAIT_TIMEOUT)
        write();
    return 0;
}
#else
void writer()
{
    std::unique_lock<std::mutex> lock(g_stopMutex);
    while (!g_stopCv.wait_for(lock, std::chrono::milliseconds(kWriteMs), [] { return g_stopping; })) {
        lock.unlock();
        write();
        lock.lock();
    }
}
#endif

} // namespace

void start()
{
    char v[32] = "";
    GetPrivateProfileStringA(kSection, kKey, "0", v, sizeof(v), ini_path().c_str());
    g_base = _strtoui64(v, nullptr, 10);
    QueryUnbiasedInterruptTime(&g_start);
#ifdef _WIN32
    g_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop != nullptr ? CreateThread(nullptr, 0, writer, nullptr, 0, nullptr) : nullptr;
#else
    g_stopping = false;
    g_thread = std::thread(writer);
#endif
    pclog::write("time played: %.1f hours so far", (double)g_base / 3600.0);
}

void stop()
{
#ifdef _WIN32
    if (g_thread != nullptr) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
#else
    if (g_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(g_stopMutex);
            g_stopping = true;
        }
        g_stopCv.notify_all();
        g_thread.join();
    }
#endif
    write();
}

} // namespace play_time
