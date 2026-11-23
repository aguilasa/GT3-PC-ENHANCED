/* fs_posix.cpp - the files and folders of the platform layer on POSIX (fs.h):
 * open / pread / pwrite / stat / readdir, '\' read as '/', and the parts of a
 * path resolved without case. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                         /* renameat2, statx */
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64                /* (i686: files past 2 GB, a disc image) */
#endif

#include "platform/os/fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <unordered_map>

namespace fs {
namespace {

thread_local unsigned long t_error;

void keep_error()
{
    t_error = (unsigned long)errno;
}

/* ---- the case of a path's parts ----
 * A folder's names by their lower case (ASCII), kept with the folder's
 * identity and change time: read again when the folder has changed, or when
 * a call here changed it. */
struct Folder {
    dev_t dev = 0;
    ino_t ino = 0;
    struct timespec mtime = {};
    std::unordered_map<std::string, std::string> names;
};

std::mutex g_foldersMutex;
std::unordered_map<std::string, Folder> g_folders;
constexpr size_t kMaxFolders = 4096;

char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = lower_ascii(c);
    return out;
}

std::string slashes(const std::string &path)
{
    std::string p = path;
    for (char &c : p)
        if (c == '\\')
            c = '/';
    return p;
}

std::string join(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    if (dir.back() == '/')
        return dir + name;
    return dir + "/" + name;
}

std::string parent_of(const std::string &p)
{
    const size_t s = p.find_last_of('/');
    if (s == std::string::npos)
        return ".";
    return s == 0 ? "/" : p.substr(0, s);
}

std::string base_of(const std::string &p)
{
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

/* The name in `dir` (a real path) whose lower case is `name`'s; false when
 * there is none. */
bool find_without_case(const std::string &dir, const std::string &name, std::string *real)
{
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    std::lock_guard<std::mutex> lock(g_foldersMutex);
    auto it = g_folders.find(dir);
    if (it == g_folders.end() || it->second.dev != st.st_dev || it->second.ino != st.st_ino ||
        it->second.mtime.tv_sec != st.st_mtim.tv_sec || it->second.mtime.tv_nsec != st.st_mtim.tv_nsec) {
        DIR *d = opendir(dir.c_str());
        if (d == nullptr)
            return false;
        if (g_folders.size() >= kMaxFolders)
            g_folders.clear();
        Folder &f = g_folders[dir];
        f.dev = st.st_dev;
        f.ino = st.st_ino;
        f.mtime = st.st_mtim;
        f.names.clear();
        while (struct dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..")
                continue;
            auto ins = f.names.emplace(lower(n), n);
            if (!ins.second && n < ins.first->second)
                ins.first->second = n;  /* (two names differing in case: the byte-wise smaller) */
        }
        closedir(d);
        it = g_folders.find(dir);
    }
    auto n = it->second.names.find(lower(name));
    if (n == it->second.names.end())
        return false;
    *real = n->second;
    return true;
}

/* A call here changed the folder holding `path`. */
void changed(const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_foldersMutex);
    g_folders.erase(parent_of(path));
}

/* `path` ('\' as '/') with each part that is not there in its case in the
 * case it has (fs.h); from the first part found in no case on, the rest as
 * given. */
std::string resolve(const std::string &path)
{
    const std::string p = slashes(path);
    struct stat st;
    if (p.empty() || lstat(p.c_str(), &st) == 0)
        return p;
    std::string out;
    size_t at = 0;
    if (p[0] == '/') {
        out = "/";
        at = 1;
    }
    while (at <= p.size()) {
        size_t next = p.find('/', at);
        if (next == std::string::npos)
            next = p.size();
        const std::string part = p.substr(at, next - at);
        const bool last = next >= p.size();
        if (!part.empty()) {
            std::string cand = join(out, part);
            if (part != "." && part != ".." && lstat(cand.c_str(), &st) != 0) {
                std::string real;
                if (!find_without_case(out.empty() ? "." : out, part, &real)) {
                    /* (not there in any case: the rest as given) */
                    out = cand;
                    if (!last)
                        out += p.substr(next);
                    return out;
                }
                cand = join(out, real);
            }
            out = cand;
        }
        if (last) {
            if (!p.empty() && p.back() == '/' && (out.empty() || out.back() != '/'))
                out += '/';
            break;
        }
        at = next + 1;
    }
    return out;
}

Time to_time(const struct timespec &t)
{
    return from_unix((int64_t)t.tv_sec) + (Time)(t.tv_nsec / 100);
}

struct timespec from_time(Time t)
{
    struct timespec ts;
    const int64_t s = to_unix(t);
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((t - from_unix(s)) * 100);
    return ts;
}

/* The stat of a real path (follows a link; a broken link: the link's own). */
bool stat_real(const std::string &real, const std::string &name, Stat *out)
{
    Stat s;
    s.hidden = !name.empty() && name[0] == '.';
#if defined(STATX_BTIME)
    struct statx x;
    if (statx(AT_FDCWD, real.c_str(), 0, STATX_BASIC_STATS | STATX_BTIME, &x) == 0 ||
        statx(AT_FDCWD, real.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &x) == 0) {
        s.dir = S_ISDIR(x.stx_mode);
        s.size = s.dir ? 0 : (uint64_t)x.stx_size;
        struct timespec m = {(time_t)x.stx_mtime.tv_sec, (long)x.stx_mtime.tv_nsec};
        s.modify = to_time(m);
        if (x.stx_mask & STATX_BTIME) {
            struct timespec b = {(time_t)x.stx_btime.tv_sec, (long)x.stx_btime.tv_nsec};
            s.create = to_time(b);
        } else {
            s.create = s.modify;
        }
        *out = s;
        return true;
    }
#else
    struct stat st;
    if (::stat(real.c_str(), &st) == 0 || lstat(real.c_str(), &st) == 0) {
        s.dir = S_ISDIR(st.st_mode);
        s.size = s.dir ? 0 : (uint64_t)st.st_size;
        s.modify = to_time(st.st_mtim);
        s.create = s.modify;
        *out = s;
        return true;
    }
#endif
    keep_error();
    return false;
}

int code_point(const unsigned char *p, size_t n, size_t *len)
{
    /* the length of a well-formed sequence at p, its value; -1 (len 1) when
     * malformed */
    const unsigned c = p[0];
    *len = 1;
    if (c < 0x80)
        return (int)c;
    size_t k;
    unsigned v, min;
    if (c >= 0xC2 && c <= 0xDF) {
        k = 2;
        v = c & 0x1F;
        min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        k = 3;
        v = c & 0x0F;
        min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        k = 4;
        v = c & 0x07;
        min = 0x10000;
    } else {
        return -1;
    }
    if (n < k)
        return -1;
    for (size_t i = 1; i < k; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return -1;
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return -1;
    *len = k;
    return (int)v;
}

} // namespace

/* ---- text ---- */

std::string native_to_utf8(const std::string &s)
{
    return s;
}

std::string utf8_to_native(const std::string &utf8, bool *exact)
{
    if (exact != nullptr)
        *exact = true;
    return utf8;
}

bool valid_utf8(const std::string &s)
{
    const unsigned char *p = (const unsigned char *)s.data();
    size_t i = 0;
    while (i < s.size()) {
        size_t len;
        if (code_point(p + i, s.size() - i, &len) < 0)
            return false;
        i += len;
    }
    return true;
}

std::string utf8_or_native(const std::string &s)
{
    return s;
}

std::wstring widen(const std::string &utf8)
{
    std::wstring w;
    w.reserve(utf8.size());
    const unsigned char *p = (const unsigned char *)utf8.data();
    size_t i = 0;
    while (i < utf8.size()) {
        size_t len;
        const int c = code_point(p + i, utf8.size() - i, &len);
        w.push_back(c < 0 ? (wchar_t)0xFFFD : (wchar_t)c);
        i += len;
    }
    return w;
}

std::string narrow(const std::wstring &w)
{
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        uint32_t c = (uint32_t)wc;
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            c = 0xFFFD;
        if (c < 0x80) {
            s += (char)c;
        } else if (c < 0x800) {
            s += (char)(0xC0 | (c >> 6));
            s += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += (char)(0xE0 | (c >> 12));
            s += (char)(0x80 | ((c >> 6) & 0x3F));
            s += (char)(0x80 | (c & 0x3F));
        } else {
            s += (char)(0xF0 | (c >> 18));
            s += (char)(0x80 | ((c >> 12) & 0x3F));
            s += (char)(0x80 | ((c >> 6) & 0x3F));
            s += (char)(0x80 | (c & 0x3F));
        }
    }
    return s;
}

/* ---- errors ---- */

unsigned long last_error()
{
    return t_error;
}

bool error_not_found(unsigned long e)
{
    return e == ENOENT || e == ENOTDIR;
}

bool error_exists(unsigned long e)
{
    return e == EEXIST;
}

bool error_held(unsigned long e)
{
    return e == EBUSY || e == ETXTBSY;
}

std::string error_text(unsigned long e)
{
    char buf[256];
    buf[0] = 0;
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char *t = strerror_r((int)e, buf, sizeof(buf));
#else
    const char *t = strerror_r((int)e, buf, sizeof(buf)) == 0 ? buf : "";
#endif
    std::string s = t != nullptr ? t : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '.'))
        s.pop_back();
    return s.empty() ? "error " + std::to_string(e) : s;
}

/* ---- times ---- */

Time now()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return to_time(ts);
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
    (void)share;
    close();
    int flags = O_CLOEXEC;
    if ((access & kRead) && (access & kWrite))
        flags |= O_RDWR;
    else if (access & kWrite)
        flags |= O_WRONLY;
    else
        flags |= O_RDONLY;
    switch (how) {
    case Create::open_existing: break;
    case Create::open_always: flags |= O_CREAT; break;
    case Create::create_always: flags |= O_CREAT | O_TRUNC; break;
    case Create::truncate_existing: flags |= O_TRUNC; break;
    }
    const std::string real = resolve(path);
    struct stat st;
    if (::stat(real.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EISDIR;                     /* (a folder is not a file to open, as on Windows) */
        keep_error();
        return false;
    }
    const bool made = (flags & O_CREAT) && lstat(real.c_str(), &st) != 0;
    int fd;
    do {
        fd = ::open(real.c_str(), flags, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        keep_error();
        return false;
    }
    if (made)
        changed(real);
#if defined(POSIX_FADV_RANDOM)
    if (hint & kRandomAccess)
        posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    if (hint & kSequential)
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    (void)hint;
#endif
    h_ = fd;
    return true;
}

void File::close()
{
    if (h_ != -1)
        ::close((int)h_);
    h_ = -1;
}

int64_t File::size() const
{
    struct stat st;
    if (h_ == -1 || fstat((int)h_, &st) != 0) {
        if (h_ == -1)
            errno = EBADF;
        keep_error();
        return -1;
    }
    return (int64_t)st.st_size;
}

int File::read_at(uint64_t offset, void *buf, uint32_t bytes) const
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < bytes) {
        const ssize_t n = pread((int)h_, p + done, bytes - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            keep_error();
            return done != 0 ? (int)done : -1;
        }
        if (n == 0)
            break;                          /* the end */
        done += (uint32_t)n;
    }
    return (int)done;
}

int File::write_at(uint64_t offset, const void *buf, uint32_t bytes) const
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < bytes) {
        const ssize_t n = pwrite((int)h_, p + done, bytes - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            keep_error();
            return -1;
        }
        done += (uint32_t)n;
    }
    return (int)done;
}

int File::read(void *buf, uint32_t bytes) const
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::read((int)h_, p + done, bytes - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            keep_error();
            return done != 0 ? (int)done : -1;
        }
        if (n == 0)
            break;
        done += (uint32_t)n;
    }
    return (int)done;
}

int File::write(const void *buf, uint32_t bytes) const
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::write((int)h_, p + done, bytes - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            keep_error();
            return -1;
        }
        done += (uint32_t)n;
    }
    return (int)done;
}

int64_t File::seek(int64_t offset, int origin) const
{
    const int how = origin == 2 ? SEEK_END : origin == 1 ? SEEK_CUR : SEEK_SET;
    const off_t pos = lseek((int)h_, (off_t)offset, how);
    if (pos < 0) {
        keep_error();
        return -1;
    }
    return (int64_t)pos;
}

bool read_file(const std::string &path, std::string *out)
{
    out->clear();
    File f;
    if (!f.open(path, kRead))
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
    const std::string real = resolve(path);
    return stat_real(real, base_of(real), out);
}

bool exists(const std::string &path)
{
    struct stat st;
    if (::stat(resolve(path).c_str(), &st) != 0) {
        keep_error();
        return false;
    }
    return true;
}

bool is_dir(const std::string &path)
{
    struct stat st;
    if (::stat(resolve(path).c_str(), &st) != 0) {
        keep_error();
        return false;
    }
    return S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path)
{
    struct stat st;
    if (::stat(resolve(path).c_str(), &st) != 0) {
        keep_error();
        return false;
    }
    return !S_ISDIR(st.st_mode);
}

bool list(const std::string &dir, std::vector<Entry> *out)
{
    out->clear();
    const std::string real = resolve(dir);
    DIR *d = opendir(real.empty() ? "." : real.c_str());
    if (d == nullptr) {
        keep_error();
        return false;
    }
    while (struct dirent *e = readdir(d)) {
        Entry entry;
        entry.name = e->d_name;
        if (entry.name == "." || entry.name == "..")
            continue;
        if (!stat_real(join(real, entry.name), entry.name, &entry.st))
            continue;                       /* (gone since) */
        out->push_back(std::move(entry));
    }
    closedir(d);
    return true;
}

/* ---- changes ---- */

bool make_dir(const std::string &path)
{
    std::string real = resolve(path);
    while (real.size() > 1 && real.back() == '/')
        real.pop_back();
    if (mkdir(real.c_str(), 0777) != 0) {
        keep_error();
        return false;
    }
    changed(real);
    return true;
}

bool make_dirs(const std::string &path)
{
    std::string p = slashes(path);
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    if (p.empty())
        return false;
    if (is_dir(p))
        return true;
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
        make_dirs(p.substr(0, slash));
    return make_dir(p) || (error_exists(t_error) && is_dir(p));
}

bool remove_file(const std::string &path)
{
    const std::string real = resolve(path);
    struct stat st;
    if (lstat(real.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        keep_error();
        return false;
    }
    if (unlink(real.c_str()) != 0) {
        keep_error();
        return false;
    }
    changed(real);
    return true;
}

bool remove_dir(const std::string &path)
{
    std::string real = resolve(path);
    while (real.size() > 1 && real.back() == '/')
        real.pop_back();
    if (rmdir(real.c_str()) != 0) {
        keep_error();
        return false;
    }
    changed(real);
    return true;
}

bool rename(const std::string &from, const std::string &to)
{
    const std::string src = resolve(from);
    std::string dst = resolve(to);
    struct stat a, b;
    if (lstat(src.c_str(), &a) != 0) {
        keep_error();
        return false;
    }
    if (lstat(dst.c_str(), &b) == 0) {
        if (a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
            errno = EEXIST;                 /* (as MoveFile: never over another file) */
            keep_error();
            return false;
        }
        /* the same file in another case: a new case for its name */
        dst = join(parent_of(dst), base_of(slashes(to)));
        if (dst == src)
            return true;
        if (::rename(src.c_str(), dst.c_str()) != 0) {
            keep_error();
            return false;
        }
        changed(src);
        return true;
    }
    int r = renameat2(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(), RENAME_NOREPLACE);
    if (r != 0 && (errno == EINVAL || errno == ENOSYS)) {
        /* (a file system without RENAME_NOREPLACE: checked just before) */
        r = lstat(dst.c_str(), &b) == 0 ? (errno = EEXIST, -1) : ::rename(src.c_str(), dst.c_str());
    }
    if (r != 0) {
        keep_error();
        return false;
    }
    changed(src);
    changed(dst);
    return true;
}

bool rename_replace(const std::string &from, const std::string &to)
{
    const std::string src = resolve(from), dst = resolve(to);
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        keep_error();
        return false;
    }
    changed(src);
    changed(dst);
    return true;
}

bool set_times(const std::string &path, const Time *create, const Time *modify)
{
    (void)create;                           /* (no birth time can be set) */
    if (modify == nullptr)
        return exists(path);
    struct timespec t[2];
    t[0].tv_sec = 0;
    t[0].tv_nsec = UTIME_OMIT;
    t[1] = from_time(*modify);
    if (utimensat(AT_FDCWD, resolve(path).c_str(), t, 0) != 0) {
        keep_error();
        return false;
    }
    return true;
}

/* ---- names ---- */

std::string exe_path()
{
    std::string buf(PATH_MAX, '\0');
    for (;;) {
        const ssize_t n = readlink("/proc/self/exe", &buf[0], buf.size());
        if (n < 0) {
            keep_error();
            return std::string();
        }
        if ((size_t)n < buf.size()) {
            buf.resize((size_t)n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::string full_path(const std::string &path)
{
    std::string p = slashes(path);
    if (p.empty() || p[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) {
            keep_error();
            return resolve(p);
        }
        p = join(cwd, p);
    }
    /* "." and ".." taken away by their text, as GetFullPathName does */
    std::vector<std::string> parts;
    size_t at = 1;
    while (at <= p.size()) {
        size_t next = p.find('/', at);
        if (next == std::string::npos)
            next = p.size();
        const std::string part = p.substr(at, next - at);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        at = next + 1;
    }
    std::string out;
    for (const std::string &part : parts)
        out += "/" + part;
    if (out.empty())
        out = "/";
    else if (p.back() == '/')
        out += '/';
    return resolve(out);
}

std::string system_path(const std::string &path)
{
    return resolve(path);
}

} // namespace fs
