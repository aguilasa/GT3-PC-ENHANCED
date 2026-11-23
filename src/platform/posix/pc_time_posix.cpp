/* pc_time_posix.cpp - the clock as the game saw it on the PS2, off Windows
 * (pc_time.h): clock_gettime, timegm / gmtime_r for the zone-less
 * conversions and localtime_r for the PC's time zone (daylight saving
 * included), as the Windows file's FILETIME / SYSTEMTIME calls.  A 64-bit
 * time_t here (glibc 2.34 and later; nothing of it crosses this file): the
 * PS2's clock runs to 2099, a 32-bit one ends in 2038. */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _TIME_BITS
#define _TIME_BITS 64
#endif
#include "platform/pc_time.h"

#include <time.h>

namespace pctime {
namespace {

constexpr int64_t kJstOffset = 9 * 3600;

/* Unix seconds of 2000-01-01 00:00 UTC. */
constexpr int64_t kEpoch2000 = 946684800LL;

int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

int64_t utc_now_seconds_since_2000()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec - kEpoch2000;
}

bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* BCD clock <-> seconds since 2000 in the clock's own zone.  A date that
 * does not exist gives 0, as SystemTimeToFileTime's failure does. */
int64_t bcd_to_seconds(const uint8_t *c)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int year = 2000 + bcd(c[7]), month = bcd(c[6]), day = bcd(c[5]);
    const int hour = bcd(c[3]), minute = bcd(c[2]), second = bcd(c[1]);
    if (month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59)
        return 0;
    if (day > kDays[month - 1] + (month == 2 && leap(year) ? 1 : 0))
        return 0;
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    return (int64_t)timegm(&t) - kEpoch2000;
}

void tm_to_bcd(const struct tm &t, uint8_t *c)
{
    c[1] = to_bcd(t.tm_sec > 59 ? 59 : t.tm_sec);
    c[2] = to_bcd(t.tm_min);
    c[3] = to_bcd(t.tm_hour);
    c[4] = 0;
    c[5] = to_bcd(t.tm_mday);
    c[6] = to_bcd(t.tm_mon + 1);
    c[7] = to_bcd((t.tm_year + 1900) % 100);
}

void seconds_to_bcd(int64_t s, uint8_t *c)
{
    const time_t u = (time_t)(s + kEpoch2000);
    struct tm t;
    if (gmtime_r(&u, &t) != nullptr)
        tm_to_bcd(t, c);
}

} // namespace

uint32_t rtc_seconds_since_2000_jst()
{
    return (uint32_t)(utc_now_seconds_since_2000() + kJstOffset);
}

void bcd_jst_to_gmt(uint8_t *c)
{
    seconds_to_bcd(bcd_to_seconds(c) - kJstOffset, c);
}

void bcd_jst_to_local(uint8_t *c)
{
    const int64_t utc = bcd_to_seconds(c) - kJstOffset;
    /* UTC -> local with the PC's time zone rules (daylight saving included). */
    const time_t u = (time_t)(utc + kEpoch2000);
    struct tm t;
    if (localtime_r(&u, &t) != nullptr)
        tm_to_bcd(t, c);
    else
        seconds_to_bcd(utc, c);
}

} // namespace pctime
