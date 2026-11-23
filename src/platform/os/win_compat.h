/* win_compat.h - the few Win32 names the platform layer uses for settings,
 * key codes, ticks and string compares.
 *
 * Windows: <windows.h> (and <realtimeapiset.h>), nothing else; the build there
 * is unchanged.
 *
 * Linux: the same names with the Windows types and values, implemented on
 * POSIX.  The private-profile (INI) functions come from ini_posix.h, with the
 * semantics of the Windows ones.
 *
 * Never declare here the names of the EE kernel of include/eekernel.h (its
 * thread and semaphore calls share their names with Win32 ones but not their
 * signatures); threads and events belong to the scheduler's own layer. */
#ifndef PLATFORM_OS_WIN_COMPAT_H
#define PLATFORM_OS_WIN_COMPAT_H

#ifdef _WIN32

#include <windows.h>
#include <realtimeapiset.h>                 /* QueryUnbiasedInterruptTime */

#else /* !_WIN32 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>

/* ---- types: the 32-bit widths of Windows, spelled as DXVK-native's
 * windows_base.h spells them (DWORD uint32_t, LONG int32_t), so that this
 * header and the Direct3D headers (d3d11.h, dxgi.h) go in one file in
 * either order ---- */
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef const wchar_t *LPCWSTR;
typedef wchar_t *LPWSTR;
typedef void *HANDLE;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef WINAPI
#define WINAPI
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

/* ---- strings ---- */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strtoui64 strtoull
#define _strtoi64 strtoll

/* ---- time ---- */
static inline ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ull + (ULONGLONG)ts.tv_nsec / 1000000ull;
}

static inline DWORD GetTickCount(void)
{
    return (DWORD)GetTickCount64();
}

/* 100 ns units, without the time the machine slept (CLOCK_MONOTONIC stops
 * during suspend on Linux, as the unbiased interrupt time does on Windows). */
static inline BOOL QueryUnbiasedInterruptTime(ULONGLONG *t)
{
    struct timespec ts;
    if (t == NULL || clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return FALSE;
    *t = (ULONGLONG)ts.tv_sec * 10000000ull + (ULONGLONG)ts.tv_nsec / 100ull;
    return TRUE;
}

static inline void Sleep(DWORD ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* ---- keyboard ----
 * The virtual-key codes keep their Windows values: they are what gt3pc.ini
 * stores for keyboard bindings. */
#define VK_LBUTTON    0x01
#define VK_RBUTTON    0x02
#define VK_CANCEL     0x03
#define VK_MBUTTON    0x04
#define VK_XBUTTON1   0x05
#define VK_XBUTTON2   0x06
#define VK_BACK       0x08
#define VK_TAB        0x09
#define VK_CLEAR      0x0C
#define VK_RETURN     0x0D
#define VK_SHIFT      0x10
#define VK_CONTROL    0x11
#define VK_MENU       0x12
#define VK_PAUSE      0x13
#define VK_CAPITAL    0x14
#define VK_ESCAPE     0x1B
#define VK_SPACE      0x20
#define VK_PRIOR      0x21
#define VK_NEXT       0x22
#define VK_END        0x23
#define VK_HOME       0x24
#define VK_LEFT       0x25
#define VK_UP         0x26
#define VK_RIGHT      0x27
#define VK_DOWN       0x28
#define VK_SELECT     0x29
#define VK_PRINT      0x2A
#define VK_EXECUTE    0x2B
#define VK_SNAPSHOT   0x2C
#define VK_INSERT     0x2D
#define VK_DELETE     0x2E
#define VK_HELP       0x2F
#define VK_LWIN       0x5B
#define VK_RWIN       0x5C
#define VK_APPS       0x5D
#define VK_SLEEP      0x5F
#define VK_NUMPAD0    0x60
#define VK_NUMPAD1    0x61
#define VK_NUMPAD2    0x62
#define VK_NUMPAD3    0x63
#define VK_NUMPAD4    0x64
#define VK_NUMPAD5    0x65
#define VK_NUMPAD6    0x66
#define VK_NUMPAD7    0x67
#define VK_NUMPAD8    0x68
#define VK_NUMPAD9    0x69
#define VK_MULTIPLY   0x6A
#define VK_ADD        0x6B
#define VK_SEPARATOR  0x6C
#define VK_SUBTRACT   0x6D
#define VK_DECIMAL    0x6E
#define VK_DIVIDE     0x6F
#define VK_F1         0x70
#define VK_F2         0x71
#define VK_F3         0x72
#define VK_F4         0x73
#define VK_F5         0x74
#define VK_F6         0x75
#define VK_F7         0x76
#define VK_F8         0x77
#define VK_F9         0x78
#define VK_F10        0x79
#define VK_F11        0x7A
#define VK_F12        0x7B
#define VK_F13        0x7C
#define VK_F14        0x7D
#define VK_F15        0x7E
#define VK_F16        0x7F
#define VK_F17        0x80
#define VK_F18        0x81
#define VK_F19        0x82
#define VK_F20        0x83
#define VK_F21        0x84
#define VK_F22        0x85
#define VK_F23        0x86
#define VK_F24        0x87
#define VK_NUMLOCK    0x90
#define VK_SCROLL     0x91
#define VK_LSHIFT     0xA0
#define VK_RSHIFT     0xA1
#define VK_LCONTROL   0xA2
#define VK_RCONTROL   0xA3
#define VK_LMENU      0xA4
#define VK_RMENU      0xA5
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE
#define VK_OEM_8      0xDF
#define VK_OEM_102    0xE2

/* No keyboard layout to ask on Linux: no scan code and no layout name, so
 * the callers fall back to their own key names. */
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2

static inline UINT MapVirtualKeyW(UINT code, UINT type)
{
    (void)code;
    (void)type;
    return 0;
}

static inline int GetKeyNameTextW(LONG lparam, LPWSTR buf, int n)
{
    (void)lparam;
    if (buf != NULL && n > 0)
        buf[0] = L'\0';
    return 0;
}

/* ---- text: CP_UTF8 only (wchar_t is UTF-32 here) ---- */
#define CP_ACP  0
#define CP_UTF8 65001

static inline int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int wn, LPSTR out, int n,
                                      LPCSTR def, BOOL *used_def)
{
    (void)flags;
    (void)def;
    if (used_def != NULL)
        *used_def = FALSE;
    if (cp != CP_UTF8 || w == NULL || n < 0)
        return 0;
    const int count = wn < 0 ? (int)wcslen(w) + 1 : wn;
    int len = 0;
    for (int i = 0; i < count; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c > 0x10FFFFu || (c >= 0xD800u && c <= 0xDFFFu))
            c = 0xFFFDu;
        unsigned char b[4];
        int k;
        if (c < 0x80u) {
            b[0] = (unsigned char)c;
            k = 1;
        } else if (c < 0x800u) {
            b[0] = (unsigned char)(0xC0u | (c >> 6));
            b[1] = (unsigned char)(0x80u | (c & 0x3Fu));
            k = 2;
        } else if (c < 0x10000u) {
            b[0] = (unsigned char)(0xE0u | (c >> 12));
            b[1] = (unsigned char)(0x80u | ((c >> 6) & 0x3Fu));
            b[2] = (unsigned char)(0x80u | (c & 0x3Fu));
            k = 3;
        } else {
            b[0] = (unsigned char)(0xF0u | (c >> 18));
            b[1] = (unsigned char)(0x80u | ((c >> 12) & 0x3Fu));
            b[2] = (unsigned char)(0x80u | ((c >> 6) & 0x3Fu));
            b[3] = (unsigned char)(0x80u | (c & 0x3Fu));
            k = 4;
        }
        if (n != 0) {
            if (len + k > n)
                return 0;                   /* ERROR_INSUFFICIENT_BUFFER */
            for (int j = 0; j < k; j++)
                out[len + j] = (char)b[j];
        }
        len += k;
    }
    return len;
}

/* ---- settings files ---- */
#include "platform/os/ini_posix.h"

#endif /* !_WIN32 */

#endif /* PLATFORM_OS_WIN_COMPAT_H */
