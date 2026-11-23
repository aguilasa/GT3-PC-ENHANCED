/* fs_win.cpp - the files and folders of the platform layer on Windows (fs.h):
 * the Win32 calls, through their UTF-16 forms. */
#include "platform/os/fs.h"

#include <windows.h>

namespace fs {
namespace {

thread_local unsigned long t_error;

/* The calling thread's error, kept for last_error (a later call - a log line -
 * may change GetLastError's). */
void keep_error()
{
    t_error = GetLastError();
}

Time file_time(const FILETIME &ft)
{
    return (Time)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

FILETIME to_filetime(Time t)
{
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(uint64_t)t;
    ft.dwHighDateTime = (DWORD)((uint64_t)t >> 32);
    return ft;
}

std::wstring from_code_page(UINT cp, DWORD flags, const std::string &s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_code_page(UINT cp, const std::wstring &w)
{
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool attributes(const std::string &path, DWORD *a)
{
    *a = GetFileAttributesW(widen(path).c_str());
    if (*a == INVALID_FILE_ATTRIBUTES) {
        keep_error();
        return false;
    }
    return true;
}

Stat stat_of(DWORD attrs, const FILETIME &create, const FILETIME &modify, DWORD sizeHigh, DWORD sizeLow)
{
    Stat s;
    s.dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    s.hidden = (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    s.size = ((uint64_t)sizeHigh << 32) | sizeLow;
    s.create = file_time(create);
    s.modify = file_time(modify);
    return s;
}

HANDLE handle(intptr_t h)
{
    return (HANDLE)h;
}

} // namespace

/* ---- text ---- */

std::string native_to_utf8(const std::string &s)
{
    return narrow(from_code_page(CP_ACP, 0, s));
}

std::string utf8_to_native(const std::string &utf8, bool *exact)
{
    const std::wstring w = widen(utf8);
    if (exact != nullptr) {
        /* (WC_NO_BEST_FIT_CHARS: a character the code page has only a look-alike for is not exact) */
        BOOL lossy = FALSE;
        const int n = w.empty() ? 1
                                : WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), nullptr,
                                                      0, nullptr, &lossy);
        *exact = n > 0 && !lossy;
    }
    return to_code_page(CP_ACP, w);
}

bool valid_utf8(const std::string &s)
{
    return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0;
}

std::string utf8_or_native(const std::string &s)
{
    return valid_utf8(s) ? s : native_to_utf8(s);
}

std::wstring widen(const std::string &utf8)
{
    return from_code_page(CP_UTF8, 0, utf8);
}

std::string narrow(const std::wstring &w)
{
    return to_code_page(CP_UTF8, w);
}

/* ---- errors ---- */

unsigned long last_error()
{
    return t_error;
}

bool error_not_found(unsigned long e)
{
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

bool error_exists(unsigned long e)
{
    return e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS;
}

bool error_held(unsigned long e)
{
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED;
}

std::string error_text(unsigned long e)
{
    wchar_t *text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, (DWORD)e, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
    std::wstring w;
    if (n != 0 && text != nullptr)
        w.assign(text, n);
    if (text != nullptr)
        LocalFree(text);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.'))
        w.pop_back();
    return w.empty() ? "error " + std::to_string(e) : narrow(w);
}

/* ---- times ---- */

Time now()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return file_time(ft);
}

/* ---- files ---- */

File &File::operator=(File &&o) noexcept
{
    if (this != &o) {
        close();
        h_ = o.h_;
        o.h_ = -1;
    }
    return *this;
}

bool File::open(const std::string &path, unsigned access, Create how, unsigned share, unsigned hint)
{
    close();
    DWORD acc = 0;
    if (access & kRead)
        acc |= GENERIC_READ;
    if (access & kWrite)
        acc |= GENERIC_WRITE;
    DWORD sh = 0;
    if (share & kShareRead)
        sh |= FILE_SHARE_READ;
    if (share & kShareWrite)
        sh |= FILE_SHARE_WRITE;
    if (share & kShareDelete)
        sh |= FILE_SHARE_DELETE;
    DWORD disp = OPEN_EXISTING;
    switch (how) {
    case Create::open_existing: disp = OPEN_EXISTING; break;
    case Create::open_always: disp = OPEN_ALWAYS; break;
    case Create::create_always: disp = CREATE_ALWAYS; break;
    case Create::truncate_existing: disp = TRUNCATE_EXISTING; break;
    }
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (hint & kRandomAccess)
        flags |= FILE_FLAG_RANDOM_ACCESS;
    if (hint & kSequential)
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    const HANDLE h = CreateFileW(widen(path).c_str(), acc, sh, nullptr, disp, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        keep_error();
        return false;
    }
    h_ = (intptr_t)h;
    return true;
}

void File::close()
{
    if (h_ != -1)
        CloseHandle(handle(h_));
    h_ = -1;
}

int64_t File::size() const
{
    LARGE_INTEGER sz;
    if (h_ == -1 || !GetFileSizeEx(handle(h_), &sz)) {
        keep_error();
        return -1;
    }
    return sz.QuadPart;
}

int File::read_at(uint64_t offset, void *buf, uint32_t bytes) const
{
    /* (a positioned read on a synchronous handle: the OVERLAPPED only carries
     * the offset, so several threads may read the same file) */
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(handle(h_), buf, bytes, &got, &ov)) {
        keep_error();
        return t_error == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (int)got;
}

int File::write_at(uint64_t offset, const void *buf, uint32_t bytes) const
{
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD put = 0;
    if (!WriteFile(handle(h_), buf, bytes, &put, &ov)) {
        keep_error();
        return -1;
    }
    return (int)put;
}

int File::read(void *buf, uint32_t bytes) const
{
    DWORD got = 0;
    if (!ReadFile(handle(h_), buf, bytes, &got, nullptr)) {
        keep_error();
        return -1;
    }
    return (int)got;
}

int File::write(const void *buf, uint32_t bytes) const
{
    DWORD put = 0;
    if (!WriteFile(handle(h_), buf, bytes, &put, nullptr)) {
        keep_error();
        return -1;
    }
    return (int)put;
}

int64_t File::seek(int64_t offset, int origin) const
{
    LARGE_INTEGER d, pos;
    d.QuadPart = offset;
    const DWORD how = origin == 2 ? FILE_END : origin == 1 ? FILE_CURRENT : FILE_BEGIN;
    if (!SetFilePointerEx(handle(h_), d, &pos, how)) {
        keep_error();
        return -1;
    }
    return pos.QuadPart;
}

bool read_file(const std::string &path, std::string *out)
{
    out->clear();
    File f;
    if (!f.open(path, kRead, Create::open_existing, kShareRead | kShareWrite))
        return false;
    const int64_t n = f.size();
    if (n < 0 || n > 0x7FFFFFFF)
        return false;
    out->resize((size_t)n);
    return n == 0 || f.read_at(0, &(*out)[0], (uint32_t)n) == (int)n;
}

/* ---- what is there ---- */

bool stat(const std::string &path, Stat *out)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) {
        keep_error();
        return false;
    }
    *out = stat_of(d.dwFileAttributes, d.ftCreationTime, d.ftLastWriteTime, d.nFileSizeHigh, d.nFileSizeLow);
    return true;
}

bool exists(const std::string &path)
{
    DWORD a;
    return attributes(path, &a);
}

bool is_dir(const std::string &path)
{
    DWORD a;
    return attributes(path, &a) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_file(const std::string &path)
{
    DWORD a;
    return attributes(path, &a) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool list(const std::string &dir, std::vector<Entry> *out)
{
    out->clear();
    std::wstring pattern = widen(dir);
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    const HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                      FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        keep_error();
        return false;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        Entry e;
        e.name = narrow(fd.cFileName);
        e.st = stat_of(fd.dwFileAttributes, fd.ftCreationTime, fd.ftLastWriteTime, fd.nFileSizeHigh,
                       fd.nFileSizeLow);
        out->push_back(std::move(e));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return true;
}

/* ---- changes ---- */

bool make_dir(const std::string &path)
{
    if (!CreateDirectoryW(widen(path).c_str(), nullptr)) {
        keep_error();
        return false;
    }
    return true;
}

bool make_dirs(const std::string &path)
{
    if (path.empty())
        return false;
    DWORD a;
    if (attributes(path, &a))
        return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    /* the parent first, then this folder */
    std::string p = path;
    while (!p.empty() && (p.back() == '\\' || p.back() == '/'))
        p.pop_back();
    const size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos && slash > 0 && p[slash - 1] != ':' && p[slash - 1] != '\\')
        make_dirs(p.substr(0, slash));
    return make_dir(p) || error_exists(t_error);
}

bool remove_file(const std::string &path)
{
    if (!DeleteFileW(widen(path).c_str())) {
        keep_error();
        return false;
    }
    return true;
}

bool remove_dir(const std::string &path)
{
    if (!RemoveDirectoryW(widen(path).c_str())) {
        keep_error();
        return false;
    }
    return true;
}

bool rename(const std::string &from, const std::string &to)
{
    if (!MoveFileW(widen(from).c_str(), widen(to).c_str())) {
        keep_error();
        return false;
    }
    return true;
}

bool rename_replace(const std::string &from, const std::string &to)
{
    if (!MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        keep_error();
        return false;
    }
    return true;
}

bool set_times(const std::string &path, const Time *create, const Time *modify)
{
    /* (FILE_FLAG_BACKUP_SEMANTICS: a folder's too) */
    const HANDLE h = CreateFileW(widen(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        keep_error();
        return false;
    }
    FILETIME c, m;
    if (create != nullptr)
        c = to_filetime(*create);
    if (modify != nullptr)
        m = to_filetime(*modify);
    const BOOL ok = SetFileTime(h, create != nullptr ? &c : nullptr, nullptr, modify != nullptr ? &m : nullptr);
    if (!ok)
        keep_error();
    CloseHandle(h);
    return ok != FALSE;
}

/* ---- names ---- */

std::string exe_path()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &path[0], (DWORD)path.size());
        if (n == 0) {
            keep_error();
            return std::string();
        }
        if (n < path.size()) {
            path.resize(n);
            return narrow(path);
        }
        path.resize(path.size() * 2);
    }
}

std::string full_path(const std::string &path)
{
    const std::wstring w = widen(path);
    const DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) {
        keep_error();
        return path;
    }
    std::wstring out((size_t)n, L'\0');
    const DWORD got = GetFullPathNameW(w.c_str(), n, &out[0], nullptr);
    if (got == 0 || got >= n) {
        keep_error();
        return path;
    }
    out.resize(got);
    return narrow(out);
}

std::string system_path(const std::string &path)
{
    return path;
}

} // namespace fs
