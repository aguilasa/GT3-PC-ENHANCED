/* fs.h - the files and folders of the platform layer, on Windows and POSIX.
 *
 * Paths are UTF-8 strings; '/' and '\' both separate their parts.
 *
 * Windows (fs_win.cpp): the Win32 calls the platform layer made before,
 * through their UTF-16 ("W") forms - a path is turned into UTF-16 at the call
 * and a name read back into UTF-8, nothing else changes (the same access,
 * sharing, creation and flags as before).  The file system there does not
 * tell case apart, nor does this layer.
 *
 * POSIX (fs_posix.cpp): open / pread / pwrite / stat / readdir / mkdir /
 * rename.  '\' is turned into '/'.  A path whose parts exist with another
 * case is resolved to the real ones - asking for race/brake.bin opens
 * RACE/BRAKE.BIN - as the game's data (named in the case of the PS2's disc)
 * needs, for every call: open, probe, list, make, remove and rename.  The
 * exact path is tried first; only a part that is not there is looked for
 * without case (ASCII letters), in the folder's names, which are kept per
 * folder and read again when the folder changes.  Of two names differing
 * only in case the byte-wise smaller wins.
 *
 * The game's own paths (platform/paths.h, host_file.h) are kept in the
 * "native" narrow text: the ANSI code page on Windows (as GetModuleFileNameA,
 * GetPrivateProfileStringA give them), UTF-8 elsewhere; native_to_utf8 and
 * utf8_to_native cross between it and this API. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace fs {

/* The separator the system's own paths use ('\' on Windows, '/' elsewhere):
 * what the paths built for the user, the logs and other libraries use. */
#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

/* ---- text ---------------------------------------------------------------- */

/* The native narrow text (the ANSI code page on Windows) as UTF-8; as it is
 * elsewhere. */
std::string native_to_utf8(const std::string &s);
/* UTF-8 as the native narrow text.  exact (optional): false when a character
 * has none there (then the code page's best fit, as the "A" calls would
 * have).  As it is elsewhere (exact). */
std::string utf8_to_native(const std::string &utf8, bool *exact = nullptr);
/* s is well-formed UTF-8 (no overlong form, no surrogate, nothing past
 * U+10FFFF). */
bool valid_utf8(const std::string &s);
/* s when it is UTF-8, else native_to_utf8(s): a path typed by hand. */
std::string utf8_or_native(const std::string &s);
/* UTF-8 <-> wchar_t (UTF-16 on Windows, UTF-32 elsewhere); a malformed
 * sequence becomes U+FFFD. */
std::wstring widen(const std::string &utf8);
std::string narrow(const std::wstring &w);

/* ---- errors ---------------------------------------------------------------- */

/* The system's error code of the calling thread's last failed call here
 * (GetLastError's, errno's). */
unsigned long last_error();
/* What it means. */
bool error_not_found(unsigned long e);     /* no such file or folder */
bool error_exists(unsigned long e);        /* it is there already */
/* The file is there but another program holds it (Windows: a sharing or
 * lock violation, or access denied; POSIX has no such locks: EBUSY,
 * ETXTBSY). */
bool error_held(unsigned long e);
/* The text of an error code, without a final period or line break. */
std::string error_text(unsigned long e);

/* ---- times ------------------------------------------------------------------- */

/* A file time: 100 ns units since 1601-01-01 UTC (a Windows FILETIME). */
typedef int64_t Time;
Time now();
/* Unix seconds <-> Time. */
constexpr int64_t kUnixEpoch = 116444736000000000LL;
inline Time from_unix(int64_t seconds) { return seconds * 10000000LL + kUnixEpoch; }
inline int64_t to_unix(Time t)              /* (rounded down) */
{
    const int64_t d = t - kUnixEpoch;
    return d >= 0 ? d / 10000000LL : -((-d + 9999999LL) / 10000000LL);
}

/* ---- files -------------------------------------------------------------------- */

enum Access : unsigned {
    kRead = 1,
    kWrite = 2,
};

enum class Create {
    open_existing,      /* it must be there */
    open_always,        /* made when missing */
    create_always,      /* made, or emptied */
    truncate_existing,  /* it must be there; emptied */
};

/* What other opens of the file may do while this one is open (Windows; POSIX
 * has no such locks and ignores it). */
enum Share : unsigned {
    kShareNone = 0,
    kShareRead = 1,
    kShareWrite = 2,
    kShareDelete = 4,
};

/* How the file will be read (a hint to the cache: Windows'
 * FILE_FLAG_RANDOM_ACCESS, FILE_FLAG_SEQUENTIAL_SCAN; posix_fadvise). */
enum Hint : unsigned {
    kNoHint = 0,
    kRandomAccess = 1,
    kSequential = 2,
};

/* An open file.  Positional reads and writes (read_at, write_at) are safe
 * from several threads at once; the sequential ones (read, write, seek) use
 * the file's own position.  Closed when destroyed. */
class File {
public:
    File() = default;
    ~File() { close(); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&o) noexcept : h_(o.h_) { o.h_ = -1; }
    File &operator=(File &&o) noexcept;

    /* False (last_error) when it cannot be opened; a file open before is
     * closed first. */
    bool open(const std::string &path, unsigned access, Create how = Create::open_existing,
              unsigned share = kShareRead, unsigned hint = kNoHint);
    bool is_open() const { return h_ != -1; }
    void close();

    int64_t size() const;                                           /* -1 on an error */
    /* Bytes read (0 at the end), -1 on an error. */
    int read_at(uint64_t offset, void *buf, uint32_t bytes) const;
    /* Bytes written, -1 on an error. */
    int write_at(uint64_t offset, const void *buf, uint32_t bytes) const;
    int read(void *buf, uint32_t bytes) const;
    int write(const void *buf, uint32_t bytes) const;
    /* The position moved from origin (0 the start, 1 the position, 2 the
     * end): the new one, -1 on an error (before the start). */
    int64_t seek(int64_t offset, int origin) const;

private:
    intptr_t h_ = -1;           /* a HANDLE (INVALID_HANDLE_VALUE: -1), a file descriptor */
};

/* A whole file into *out; false when it cannot be read. */
bool read_file(const std::string &path, std::string *out);

/* ---- what is there --------------------------------------------------------------- */

struct Stat {
    bool dir = false;
    bool hidden = false;        /* Windows: hidden or system; POSIX: a name starting with '.' */
    uint64_t size = 0;
    Time create = 0;            /* POSIX: the birth time where the file system keeps one, else modify */
    Time modify = 0;
};

bool stat(const std::string &path, Stat *out);                  /* false: not there */
bool exists(const std::string &path);
bool is_dir(const std::string &path);
bool is_file(const std::string &path);                          /* there, and not a folder */

struct Entry {
    std::string name;
    Stat st;
};

/* The entries of a folder, without "." and ".." (the system's order); false
 * (last_error) when it cannot be listed. */
bool list(const std::string &dir, std::vector<Entry> *out);

/* ---- changes ----------------------------------------------------------------- */

bool make_dir(const std::string &path);                         /* false when it is there already (error_exists) */
/* The folder and its missing parents; true when it is a folder afterwards. */
bool make_dirs(const std::string &path);
bool remove_file(const std::string &path);
bool remove_dir(const std::string &path);                       /* an empty one */
/* Renamed; false when `to` is there already. */
bool rename(const std::string &from, const std::string &to);
/* Renamed over `to` when it is there, in one step: `to` is the old file or
 * the new one, never missing (a file written beside it then put in place). */
bool rename_replace(const std::string &from, const std::string &to);
/* Sets the times given (nullptr: kept).  POSIX cannot set a birth time:
 * create is ignored there. */
bool set_times(const std::string &path, const Time *create, const Time *modify);

/* ---- names ------------------------------------------------------------------- */

/* The running executable's path. */
std::string exe_path();
/* The absolute path of `path` (relative: to the current folder), "." and ".."
 * parts taken away, as the system names it: Windows' GetFullPathName;
 * POSIX: '/' separators and the parts that exist in their real case. */
std::string full_path(const std::string &path);
/* `path` as the system's own calls (and other libraries: SQLite, a decoder)
 * take it: Windows: as it is; POSIX: '/' separators and the parts that exist
 * in their real case. */
std::string system_path(const std::string &path);

} // namespace fs
