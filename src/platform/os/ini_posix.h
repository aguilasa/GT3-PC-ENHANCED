/* ini_posix.h - the Win32 private-profile (INI) calls on POSIX (ini_posix.cpp),
 * with the Windows semantics the settings code relies on:
 *
 *   - section and key names compare without case (ASCII);
 *   - a missing file, section or key gives the default (trailing blanks of
 *     the default dropped, as Windows does);
 *   - values are trimmed and lose one pair of matching surrounding quotes;
 *   - lists (all sections, all keys of a section, a whole section) are
 *     NUL-separated strings ending with a second NUL; a list cut by the
 *     buffer returns size - 2, a cut value size - 1;
 *   - writing edits the file in place: comments, blank lines and the order of
 *     the lines are kept; a NULL value deletes the key, a NULL key the whole
 *     section;
 *   - every call reads the file again, so a change made by another process
 *     (gt3pc.ini and gt3pc_engine.ini are shared) is seen at once.
 *
 * Bytes are passed through as they are (the "A" calls: no code page). */
#ifndef PLATFORM_OS_INI_POSIX_H
#define PLATFORM_OS_INI_POSIX_H

#ifndef _WIN32

#include "platform/os/win_compat.h"

DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file);
UINT GetPrivateProfileIntA(LPCSTR section, LPCSTR key, int def, LPCSTR file);
DWORD GetPrivateProfileSectionA(LPCSTR section, LPSTR out, DWORD size, LPCSTR file);
DWORD GetPrivateProfileSectionNamesA(LPSTR out, DWORD size, LPCSTR file);
BOOL WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file);
BOOL WritePrivateProfileSectionA(LPCSTR section, LPCSTR pairs, LPCSTR file);

#endif /* !_WIN32 */

#endif /* PLATFORM_OS_INI_POSIX_H */
