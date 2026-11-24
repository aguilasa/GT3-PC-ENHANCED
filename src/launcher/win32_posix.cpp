/* win32_posix.cpp - the Win32 file calls of the launcher's installer off
 * Windows (win32_posix.h), over the platform layer's files (platform/os/fs.h). */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64                /* (i686: the free space of a large disk) */
#endif

#include "win32_posix.h"

#include "platform/os/fs.h"

#include <string.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::string path_of(LPCWSTR w)
{
    return w != nullptr ? fs::narrow(std::wstring(w)) : std::string();
}

void put_time(fs::Time t, FILETIME *ft)
{
    ft->dwLowDateTime = (DWORD)(uint64_t)t;
    ft->dwHighDateTime = (DWORD)((uint64_t)t >> 32);
}

DWORD attributes_of(const std::string &name, const fs::Stat &st)
{
    DWORD a = st.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (st.hidden || (!name.empty() && name[0] == '.'))
        a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}

/* ---- the open files ---- */

fs::File *file_of(HANDLE h)
{
    return h != nullptr && h != INVALID_HANDLE_VALUE ? static_cast<fs::File *>(h) : nullptr;
}

/* ---- the find calls ---- */

wchar_t upper(wchar_t c)
{
    return c >= L'a' && c <= L'z' ? (wchar_t)(c - L'a' + L'A') : c;
}

/* `name` matches the pattern `mask` ('*' any run, '?' one character),
 * without case (ASCII letters). */
bool matches(const wchar_t *mask, const wchar_t *name)
{
    const wchar_t *star = nullptr, *after = nullptr;
    while (*name) {
        if (*mask == L'*') {
            star = mask++;
            after = name;
        } else if (*mask == L'?' || (*mask && upper(*mask) == upper(*name))) {
            mask++;
            name++;
        } else if (star != nullptr) {
            mask = star + 1;
            name = ++after;
        } else {
            return false;
        }
    }
    while (*mask == L'*')
        mask++;
    return *mask == 0;
}

/* (NTFS' order of a folder's names: by their upper case) */
bool before(const std::wstring &a, const std::wstring &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const wchar_t x = upper(a[i]), y = upper(b[i]);
        if (x != y)
            return (uint32_t)x < (uint32_t)y;
    }
    return a.size() < b.size();
}

struct Found {
    std::wstring name;
    fs::Stat st;
};

struct Search {
    std::vector<Found> found;
    size_t next = 0;
};

void fill(const Found &f, WIN32_FIND_DATAW *fd)
{
    *fd = WIN32_FIND_DATAW{};
    fd->dwFileAttributes = attributes_of(fs::narrow(f.name), f.st);
    put_time(f.st.create, &fd->ftCreationTime);
    put_time(f.st.modify, &fd->ftLastAccessTime);
    put_time(f.st.modify, &fd->ftLastWriteTime);
    fd->nFileSizeHigh = (DWORD)(f.st.size >> 32);
    fd->nFileSizeLow = (DWORD)f.st.size;
    const size_t n = std::min(f.name.size(), (size_t)MAX_PATH - 1);
    std::copy(f.name.begin(), f.name.begin() + (ptrdiff_t)n, fd->cFileName);
    fd->cFileName[n] = 0;
}

} // namespace

DWORD GetLastError()
{
    return (DWORD)fs::last_error();
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void *, DWORD disposition, DWORD flags, HANDLE)
{
    const std::string p = path_of(path);
    unsigned a = 0;
    if (access & GENERIC_READ)
        a |= fs::kRead;
    if (access & GENERIC_WRITE)
        a |= fs::kWrite;
    fs::Create how = fs::Create::open_existing;
    switch (disposition) {
    case CREATE_NEW:
        if (fs::exists(p)) {
            /* never over a file that is there, as Windows; the error (EEXIST)
             * from a folder asked for in its place */
            fs::make_dir(p);
            return INVALID_HANDLE_VALUE;
        }
        how = fs::Create::open_always;
        break;
    case CREATE_ALWAYS: how = fs::Create::create_always; break;
    case OPEN_ALWAYS: how = fs::Create::open_always; break;
    case TRUNCATE_EXISTING: how = fs::Create::truncate_existing; break;
    default: break;
    }
    unsigned s = 0;
    if (share & FILE_SHARE_READ)
        s |= fs::kShareRead;
    if (share & FILE_SHARE_WRITE)
        s |= fs::kShareWrite;
    if (share & FILE_SHARE_DELETE)
        s |= fs::kShareDelete;
    unsigned hint = fs::kNoHint;
    if (flags & FILE_FLAG_SEQUENTIAL_SCAN)
        hint |= fs::kSequential;
    if (flags & FILE_FLAG_RANDOM_ACCESS)
        hint |= fs::kRandomAccess;
    fs::File *f = new fs::File;
    if (!f->open(p, a, how, s, hint)) {
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

BOOL CloseHandle(HANDLE h)
{
    fs::File *f = file_of(h);
    if (f == nullptr)
        return FALSE;
    delete f;
    return TRUE;
}

BOOL ReadFile(HANDLE h, void *buf, DWORD bytes, DWORD *got, OVERLAPPED *overlapped)
{
    const fs::File *f = file_of(h);
    if (got != nullptr)
        *got = 0;
    if (f == nullptr)
        return FALSE;
    uint8_t *p = static_cast<uint8_t *>(buf);
    uint64_t at = overlapped != nullptr ? ((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset) : 0;
    DWORD done = 0;
    while (done < bytes) {
        /* (fs reads at most what an int counts) */
        const uint32_t n = std::min<uint32_t>(bytes - done, 1u << 30);
        const int r = overlapped != nullptr ? f->read_at(at + done, p + done, n) : f->read(p + done, n);
        if (r < 0)
            return FALSE;
        done += (DWORD)r;
        if ((uint32_t)r < n)
            break;                          /* the end */
    }
    if (got != nullptr)
        *got = done;
    return TRUE;
}

BOOL WriteFile(HANDLE h, const void *buf, DWORD bytes, DWORD *put, OVERLAPPED *overlapped)
{
    const fs::File *f = file_of(h);
    if (put != nullptr)
        *put = 0;
    if (f == nullptr)
        return FALSE;
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    uint64_t at = overlapped != nullptr ? ((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset) : 0;
    DWORD done = 0;
    while (done < bytes) {
        const uint32_t n = std::min<uint32_t>(bytes - done, 1u << 30);
        const int r = overlapped != nullptr ? f->write_at(at + done, p + done, n) : f->write(p + done, n);
        if (r <= 0)
            return FALSE;
        done += (DWORD)r;
    }
    if (put != nullptr)
        *put = done;
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size)
{
    const fs::File *f = file_of(h);
    const int64_t n = f != nullptr ? f->size() : -1;
    if (n < 0)
        return FALSE;
    size->QuadPart = n;
    return TRUE;
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd)
{
    const std::wstring p = pattern != nullptr ? pattern : L"";
    const size_t slash = p.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash + 1);
    const std::wstring mask = slash == std::wstring::npos ? p : p.substr(slash + 1);
    std::vector<fs::Entry> entries;
    if (mask.empty() || !fs::list(fs::narrow(dir), &entries))
        return INVALID_HANDLE_VALUE;
    Search *s = new Search;
    for (const fs::Entry &e : entries) {
        std::wstring name = fs::widen(e.name);
        if (matches(mask.c_str(), name.c_str()))
            s->found.push_back({std::move(name), e.st});
    }
    if (s->found.empty()) {
        delete s;
        fs::Stat none;
        fs::stat(fs::narrow(p), &none);     /* (the error of a name not there) */
        return INVALID_HANDLE_VALUE;
    }
    std::sort(s->found.begin(), s->found.end(), [](const Found &a, const Found &b) { return before(a.name, b.name); });
    fill(s->found[s->next++], fd);
    return s;
}

HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS, void *fd, FINDEX_SEARCH_OPS, void *, DWORD)
{
    return FindFirstFileW(pattern, static_cast<WIN32_FIND_DATAW *>(fd));
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd)
{
    Search *s = h != nullptr && h != INVALID_HANDLE_VALUE ? static_cast<Search *>(h) : nullptr;
    if (s == nullptr || s->next >= s->found.size())
        return FALSE;
    fill(s->found[s->next++], fd);
    return TRUE;
}

BOOL FindClose(HANDLE h)
{
    Search *s = h != nullptr && h != INVALID_HANDLE_VALUE ? static_cast<Search *>(h) : nullptr;
    if (s == nullptr)
        return FALSE;
    delete s;
    return TRUE;
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    const std::string p = path_of(path);
    fs::Stat st;
    if (!fs::stat(p, &st))
        return INVALID_FILE_ATTRIBUTES;
    const size_t slash = p.find_last_of("\\/");
    return attributes_of(slash == std::string::npos ? p : p.substr(slash + 1), st);
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, void *data)
{
    const std::string p = path_of(path);
    fs::Stat st;
    if (!fs::stat(p, &st))
        return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *a = static_cast<WIN32_FILE_ATTRIBUTE_DATA *>(data);
    const size_t slash = p.find_last_of("\\/");
    a->dwFileAttributes = attributes_of(slash == std::string::npos ? p : p.substr(slash + 1), st);
    put_time(st.create, &a->ftCreationTime);
    put_time(st.modify, &a->ftLastAccessTime);
    put_time(st.modify, &a->ftLastWriteTime);
    a->nFileSizeHigh = (DWORD)(st.size >> 32);
    a->nFileSizeLow = (DWORD)st.size;
    return TRUE;
}

BOOL SetFileAttributesW(LPCWSTR path, DWORD)
{
    return fs::exists(path_of(path)) ? TRUE : FALSE;
}

BOOL DeleteFileW(LPCWSTR path)
{
    return fs::remove_file(path_of(path)) ? TRUE : FALSE;
}

BOOL RemoveDirectoryW(LPCWSTR path)
{
    return fs::remove_dir(path_of(path)) ? TRUE : FALSE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    const std::string a = path_of(from), b = path_of(to);
    return ((flags & MOVEFILE_REPLACE_EXISTING) ? fs::rename_replace(a, b) : fs::rename(a, b)) ? TRUE : FALSE;
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    std::string bytes;
    if (!fs::read_file(path_of(from), &bytes))
        return FALSE;
    HANDLE h = CreateFileW(to, GENERIC_WRITE, 0, nullptr, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD put = 0;
    const BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size();
    CloseHandle(h);
    if (!ok)
        fs::remove_file(path_of(to));
    return ok;
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR buf, LPWSTR *file_part)
{
    std::wstring full = fs::widen(fs::full_path(path_of(path)));
    if (file_part != nullptr)
        *file_part = nullptr;
    if (full.size() + 1 > size)
        return (DWORD)full.size() + 1;
    std::copy(full.begin(), full.end(), buf);
    buf[full.size()] = 0;
    if (file_part != nullptr) {
        const size_t slash = full.find_last_of(L'/');
        if (slash != std::wstring::npos && slash + 1 < full.size())
            *file_part = buf + slash + 1;
    }
    return (DWORD)full.size();
}

BOOL GetDiskFreeSpaceExW(LPCWSTR path, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_bytes)
{
    struct statvfs v;
    if (statvfs(fs::system_path(path_of(path)).c_str(), &v) != 0)
        return FALSE;
    const ULONGLONG unit = v.f_frsize != 0 ? (ULONGLONG)v.f_frsize : (ULONGLONG)v.f_bsize;
    if (avail != nullptr)
        avail->QuadPart = (ULONGLONG)v.f_bavail * unit;
    if (total != nullptr)
        total->QuadPart = (ULONGLONG)v.f_blocks * unit;
    if (free_bytes != nullptr)
        free_bytes->QuadPart = (ULONGLONG)v.f_bfree * unit;
    return TRUE;
}

int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR out, int out_n)
{
    if ((cp != CP_UTF8 && cp != CP_ACP) || s == nullptr || out_n < 0)
        return 0;
    std::string text = n < 0 ? std::string(s, strlen(s) + 1) : std::string(s, (size_t)n);
    if ((flags & MB_ERR_INVALID_CHARS) && !fs::valid_utf8(text))
        return 0;                           /* ERROR_NO_UNICODE_TRANSLATION */
    const std::wstring w = fs::widen(text);
    if (out_n == 0)
        return (int)w.size();
    if ((int)w.size() > out_n)
        return 0;                           /* ERROR_INSUFFICIENT_BUFFER */
    std::copy(w.begin(), w.end(), out);
    return (int)w.size();
}
