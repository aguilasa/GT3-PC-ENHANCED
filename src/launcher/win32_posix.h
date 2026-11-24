/* win32_posix.h - the Win32 file calls of the launcher's installer
 * (install.cpp, disc_files.cpp and the conversions: music, gameconf,
 * databases, stills, image_packs) off Windows, with the Windows names, types
 * and meanings, over the platform layer's files (platform/os/fs.h:
 * '\' read as '/', the parts of a path matched without case).  Windows
 * includes <windows.h> in its place; nothing here is built there.
 *
 * What differs from Windows:
 *   - GetLastError is the platform layer's last error (fs::last_error: an
 *     errno value), which util::system_error_text turns into text; the
 *     ERROR_ names the callers compare with are the errno values of
 *     fs::error_held (POSIX has no sharing locks: a file another program
 *     holds open does not stop a rename or a delete);
 *   - the find calls list the folder whole and give the names matching the
 *     pattern ('*' and '?', without case) in NTFS' order (by their upper
 *     case), never "." or ".."; a name starting with '.' is hidden
 *     (FILE_ATTRIBUTE_HIDDEN);
 *   - SetFileAttributesW changes nothing (no read-only attribute here);
 *   - the narrow text of CP_ACP is UTF-8, as the platform layer's native
 *     text is (fs.h). */
#pragma once

#ifndef _WIN32

#include "platform/os/win_compat.h"

/* (what <windows.h> brings in with it and the callers use) */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* ---- types ---- */
typedef intptr_t LONG_PTR;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

/* (the offset of a positional ReadFile; the read is synchronous) */
typedef struct _OVERLAPPED {
    uintptr_t Internal;
    uintptr_t InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED;

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    wchar_t cFileName[MAX_PATH];
    wchar_t cAlternateFileName[14];
} WIN32_FIND_DATAW;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

typedef enum _FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic } FINDEX_INFO_LEVELS;
typedef enum _FINDEX_SEARCH_OPS {
    FindExSearchNameMatch,
    FindExSearchLimitToDirectories,
    FindExSearchLimitToDevices
} FINDEX_SEARCH_OPS;
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;

/* ---- values ---- */
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

#define FILE_ATTRIBUTE_READONLY      0x00000001u
#define FILE_ATTRIBUTE_HIDDEN        0x00000002u
#define FILE_ATTRIBUTE_SYSTEM        0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY     0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE       0x00000020u
#define FILE_ATTRIBUTE_NORMAL        0x00000080u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u

#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u

#define FILE_SHARE_READ   0x00000001u
#define FILE_SHARE_WRITE  0x00000002u
#define FILE_SHARE_DELETE 0x00000004u

#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5

#define FILE_FLAG_RANDOM_ACCESS   0x10000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u

#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#define MOVEFILE_WRITE_THROUGH    0x00000008u

#define FIND_FIRST_EX_LARGE_FETCH 0x00000002u

#define WC_NO_BEST_FIT_CHARS 0x00000400u
#define MB_ERR_INVALID_CHARS 0x00000008u

/* (errno values: what fs::error_held takes for a file another program
 * holds, and the end of a file) */
#define ERROR_SHARING_VIOLATION EBUSY
#define ERROR_ACCESS_DENIED     ETXTBSY
#define ERROR_HANDLE_EOF        ENODATA

/* ---- strings ---- */
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp

/* ---- the calls (win32_posix.cpp) ---- */
DWORD GetLastError();

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags,
                   HANDLE templ);
BOOL CloseHandle(HANDLE h);
/* overlapped: read at its offset (the file's position unchanged), else at
 * the position; *got 0 at the end, with TRUE */
BOOL ReadFile(HANDLE h, void *buf, DWORD bytes, DWORD *got, OVERLAPPED *overlapped);
BOOL WriteFile(HANDLE h, const void *buf, DWORD bytes, DWORD *put, OVERLAPPED *overlapped);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd);
HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, void *fd, FINDEX_SEARCH_OPS op, void *filter,
                        DWORD flags);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL FindClose(HANDLE h);

DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void *data);
BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes);

BOOL DeleteFileW(LPCWSTR path);
BOOL RemoveDirectoryW(LPCWSTR path);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR buf, LPWSTR *file_part);
BOOL GetDiskFreeSpaceExW(LPCWSTR path, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes);

/* CP_UTF8 and CP_ACP (both UTF-8 here) */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR out, int out_n);

#endif /* !_WIN32 */
