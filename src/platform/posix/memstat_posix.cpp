/* memstat_posix.cpp - the process's memory, for the log, off Windows
 * (memstat.h): /proc/self/status.  "virtual" is VmSize of the 32-bit
 * address space (4 GB on a 64-bit kernel, 3 GB on a 32-bit one), "private"
 * the private writable mappings (VmData: data, heaps, stacks, the arena)
 * and "working set" what is resident (VmRSS).  Read by open / read, no
 * allocation, no stdio stream: the crash report calls it from its signal
 * handler.  No GPU part yet. */
#include "platform/memstat.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace memstat {
namespace {

struct Status {
    uint32_t sizeKb, dataKb, rssKb;
    bool ok;
};

/* "<key>:   <n> kB" in the text: n (0 when missing) */
uint32_t field(const char *text, const char *key)
{
    const size_t k = strlen(key);
    for (const char *p = text; (p = strstr(p, key)) != nullptr; p++) {
        if ((p != text && p[-1] != '\n') || p[k] != ':')
            continue;
        p += k + 1;
        while (*p == ' ' || *p == '\t')
            p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (uint32_t)(*p++ - '0');
        return v;
    }
    return 0;
}

Status read_status()
{
    Status s = {};
    char text[4096];
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return s;
    size_t n = 0;
    for (;;) {
        const ssize_t r = ::read(fd, text + n, sizeof(text) - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n == sizeof(text) - 1)
            break;
    }
    ::close(fd);
    text[n] = '\0';
    s.sizeKb = field(text, "VmSize");
    s.dataKb = field(text, "VmData");
    s.rssKb = field(text, "VmRSS");
    s.ok = s.sizeKb != 0;
    return s;
}

/* The address space a 32-bit process has, MB. */
uint32_t total_mb()
{
    struct utsname u;
    if (uname(&u) == 0 && strcmp(u.machine, "x86_64") == 0)
        return 4096;
    return 3072;
}

/* text appended to out (n bytes, `at' used so far); the new length */
size_t put(char *out, size_t n, size_t at, const char *fmt, ...)
{
    if (at >= n)
        return at;
    va_list a;
    va_start(a, fmt);
    const int w = vsnprintf(out + at, n - at, fmt, a);
    va_end(a);
    if (w < 0)
        return at;
    return at + (size_t)w >= n ? n - 1 : at + (size_t)w;
}

} // namespace

uint32_t virtual_mb()
{
    const Status s = read_status();
    return s.ok ? s.sizeKb >> 10 : 0;
}

void line(char *out, size_t n, bool withGpu)
{
    (void)withGpu;                          /* (no GPU counters here yet) */
    if (n == 0)
        return;
    out[0] = '\0';
    const Status s = read_status();
    if (!s.ok) {
        put(out, n, 0, "not known");
        return;
    }
    put(out, n, 0, "virtual %u MB of %u, private %u MB, working set %u MB", (unsigned)(s.sizeKb >> 10),
        (unsigned)total_mb(), (unsigned)(s.dataKb >> 10), (unsigned)(s.rssKb >> 10));
}

} // namespace memstat
