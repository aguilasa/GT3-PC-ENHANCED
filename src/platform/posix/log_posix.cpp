/* log_posix.cpp - the GT3 PC log off Windows (log.h): gt3pc.log next to the
 * program and the standard error (a terminal, the desktop session's
 * journal).  There is no console window to open: the standard error is
 * already the console. */
#include "platform/log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mutex>

#ifndef GT3PC_LOG_NO_SDL
#include <SDL3/SDL_messagebox.h>
#endif

namespace pclog {
namespace {

std::mutex g_mutex;
FILE *g_file = nullptr;

void emit(const char *text, int len)
{
    if (len <= 0)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fwrite(text, 1, len, g_file);
        fflush(g_file);
    }
    /* (unbuffered: a line is there before a crash) */
    fwrite(text, 1, len, stderr);
}

} // namespace

void open(const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        fclose(g_file);
    /* (close-on-exec: the restart's new image does not inherit it) */
    g_file = fopen(path, "wbe");
}

void open_console()
{
    /* the standard error is the console already */
}

void close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        fclose(g_file);
    g_file = nullptr;
}

void vwrite(const char *fmt, va_list ap)
{
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    if (n == 0 || buf[n - 1] != '\n')
        buf[n++] = '\n';
    emit(buf, n);
}

void write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(fmt, ap);
    va_end(ap);
}

void raw(const char *text, int len)
{
    emit(text, len);
}

void fatal(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write("FATAL: %s", buf);
#ifndef GT3PC_LOG_NO_SDL
    /* (callable before SDL_Init; without a display it fails, and the log has it) */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gran Turismo 3", buf, nullptr);
#endif
    close();
    /* as ExitProcess: no static destructors (the game threads still run) */
    _exit(1);
}

} // namespace pclog
