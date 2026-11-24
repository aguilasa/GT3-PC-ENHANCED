/* install.cpp - installs the game files from the user's disc image (install.h).
 *
 * The unpack reads the volume's files in disc order (one sequential pass over
 * the image, the cache manager reading ahead) and writes each to its loose
 * file; gzip-packed files are inflated in memory and checked against their
 * CRC-32 and size before they are written. */
#include "install.h"
#include "gameconf.h"
#include "databases.h"
#include "inflate.h"
#include "music.h"
#include "stills.h"
#include "image_packs.h"                    /* PC (2026-11-08) */
#include "util.h"
#include "volume.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"

/* the demo replays built in (cmake/demos.cmake) */
extern "C" const unsigned char *gt3pc_demo_file(size_t index, const char **name, size_t *size);
#endif

#include <string.h>

#include <algorithm>
#include <vector>

namespace install {
namespace {

constexpr const wchar_t *kPartSuffix = L".part";     /* data.part while unpacking */
constexpr const wchar_t *kOldSuffix = L".old";       /* the replaced data folder, until deleted */

/* The Gran Turismo 3 engine releases known by name, by their boot file;
 * any other disc with CORE.GT3 is installed too (release_name). */
struct Release {
    const char *boot;
    const wchar_t *name;
};
constexpr Release kReleases[] = {
    {"SCUS_971.02", L"Gran Turismo 3 A-Spec (USA, SCUS-97102)"},
    {"PBPX_955.03", L"Gran Turismo 3 A-Spec (USA, PBPX-95503)"},
    {"SCES_502.94", L"Gran Turismo 3 A-Spec (Europe, SCES-50294)"},
    {"SCPS_150.09", L"Gran Turismo 3 A-Spec (Japan, SCPS-15009)"},
    {"SCPS_150.10", L"Gran Turismo Concept 2001 Tokyo (Japan, SCPS-15010)"},
    {"SCES_508.58", L"Gran Turismo Concept 2002 Tokyo-Geneva (Europe, SCES-50858)"},
    {"SCPS_560.05", L"Gran Turismo Concept 2002 Tokyo-Seoul (Korea, SCPS-56005)"},
};
/* What an earlier launcher installed (the only disc it took). */
constexpr const wchar_t *kEarlierRelease = L"Gran Turismo 3 A-Spec (USA, SCUS-97102)";

bool same_text(const std::string &a, const char *b)
{
    return _stricmp(a.c_str(), b) == 0;
}

/* The playable version of a boot file (the bundle has the US disc's files),
 * else data only. */
Version version_of(const std::string &boot)
{
    if (same_text(boot, "SCUS_971.02") || same_text(boot, "PBPX_955.03"))
        return Version::us;
    if (same_text(boot, "SCES_502.94"))
        return Version::eu;
    if (same_text(boot, "SCPS_150.09"))
        return Version::jp;
    return Version::data_only;
}

/* A disc's data folder: its version's, else data_<boot file> (a demo
 * disc's game, or a disc without SYSTEM.CNF: data_<its folder or volume
 * name>), with the characters a folder name cannot have replaced. */
std::wstring data_folder_of(Version v, const std::string &boot, const std::string &folder, const std::string &volume_id)
{
    if (v != Version::data_only)
        return version_folder(v);
    std::string what = !folder.empty() ? folder : !boot.empty() ? boot : volume_id.empty() ? "disc" : volume_id;
    for (char &c : what)
        if (c < 0x20 || strchr("<>:\"/\\|?* ", c) != nullptr)
            c = '_';
    return std::wstring(kDataFolder) + L"_" + util::widen(what);
}

/* (PC 2026-11-14: the unified structure's course_region, flat_courses and
 * unified_path are volume.h's now: the game reads a disc image's volume in
 * the same structure) */

/* What the game needs to know of a release, from its CORE.GT3: the car
 * database files its code reads, and its own language (install.h). */
struct CoreFacts {
    std::string database, strings, unistrings;  /* "database/paramdb_eu.db", ... ("" unknown) */
    std::string language;                       /* "US", "BR", "JP" */
};

/* "SCES_502.94" -> "SCES-50294". */
std::string serial(const std::string &boot)
{
    std::string s;
    for (char c : boot)
        if (c == '_')
            s += '-';
        else if (c != '.')
            s += c;
    return s;
}

/* The release's name: a known boot file's, else what the disc says of it
 * (its volume name, the game's folder, the boot file). */
std::wstring release_name(const std::string &boot, const std::string &folder, const std::string &volume_id)
{
    if (folder.empty())
        for (const Release &r : kReleases)
            if (same_text(boot, r.boot))
                return r.name;
    std::string what = volume_id.empty() ? std::string("a disc") : volume_id;
    if (!folder.empty())
        what += "\\" + folder;
    else if (!boot.empty())
        what += ", " + serial(boot);
    return L"(" + util::widen(what) + L")";
}

/* The release's own language, by its car database (each release's code
 * reads its region's: the US build _us, the PAL build _eu, the Japanese one
 * none; GT Concept _bs (Japan), _gtc_eu (Europe), _kr (Korea: English
 * here)), else by its boot file's prefix. */
std::string language_of(const std::string &database, const std::string &boot)
{
    static const char kBase[] = "database/paramdb";
    const size_t nb = sizeof(kBase) - 1;
    if (database.size() >= nb + 3) {
        const std::string suffix = database.substr(nb, database.size() - nb - 3);
        auto ends = [&suffix](const char *e) {
            const size_t n = strlen(e);
            return suffix.size() >= n && _stricmp(suffix.c_str() + suffix.size() - n, e) == 0;
        };
        if (suffix.empty() || ends("_bs"))
            return "JP";
        if (ends("_us") || ends("_kr") || ends("_tw") || ends("_cn"))
            return "US";
        if (ends("_eu"))
            return "BR";
    }
    const std::string p = boot.substr(0, 4);
    if (same_text(p, "SCES") || same_text(p, "SLES") || same_text(p, "SCED"))
        return "BR";
    if (same_text(p, "SCPS") || same_text(p, "SLPS") || same_text(p, "SLPM") || same_text(p, "SCAJ") ||
        same_text(p, "PCPX") || same_text(p, "PAPX"))
        return "JP";
    return "US";
}

/* The NUL-terminated string of `body` at the first `prefix` that ends in
 * `suffix` ("database/paramdb" .. ".db"), "" if none. */
std::string find_string(const std::vector<uint8_t> &body, const char *prefix, const char *suffix)
{
    const size_t np = strlen(prefix), ns = strlen(suffix);
    for (size_t i = 0; i + np < body.size(); i++) {
        if (memcmp(&body[i], prefix, np) != 0)
            continue;
        size_t e = i + np;
        while (e < body.size() && e - i < 128 && body[e] >= 0x20 && body[e] < 0x7F)
            e++;
        if (e < body.size() && body[e] == 0 && e - i >= np + ns && memcmp(&body[e - ns], suffix, ns) == 0)
            return std::string((const char *)&body[i], e - i);
    }
    return std::string();
}

/* The facts of a CORE.GT3 (its bytes: two load-source bytes, the u32
 * unpacked size, raw deflate data; tools/gt3/gt3pack.py); the language by
 * the boot file alone when it cannot be unpacked. */
CoreFacts core_facts(const std::vector<uint8_t> &core, const std::string &boot)
{
    CoreFacts f;
    std::vector<uint8_t> body;
    std::wstring err;
    if (core.size() > 6) {
        uint32_t raw = 0;
        memcpy(&raw, core.data() + 2, 4);
        if (inflate::inflate_raw(core.data() + 6, core.size() - 6, raw, &body, &err)) {
            f.database = find_string(body, "database/paramdb", ".db");
            f.strings = find_string(body, "database/paramstr", ".db");
            f.unistrings = find_string(body, "database/paramunistr", ".db");
        }
    }
    f.language = language_of(f.database, boot);
    return f;
}

/* The marker's lines before the counts (install.h); how: where the files are
 * from (a disc image's folder, PC 2026-11-14: "read from its disc image"). */
std::string marker_head(const std::wstring &release, const std::string &boot, const CoreFacts &f,
                        const char *how = "unpacked from its volume")
{
    const std::string name = util::narrow(release);
    std::string m = name + ": the game files, " + how + "\r\n";
    m += "release " + name + "\r\n";
    if (!boot.empty())
        m += "boot " + boot + "\r\n";
    m += "language " + f.language + "\r\n";
    if (!f.database.empty())
        m += "database " + f.database + "\r\n";
    if (!f.strings.empty())
        m += "strings " + f.strings + "\r\n";
    if (!f.unistrings.empty())
        m += "unistrings " + f.unistrings + "\r\n";
    return m;
}

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

/* The value of `key` in SYSTEM.CNF ("BOOT2 = cdrom0:\SCUS_971.02;1"). */
bool cnf_value(const std::string &cnf, const char *key, std::string *value)
{
    size_t pos = 0;
    while (pos < cnf.size()) {
        size_t end = cnf.find('\n', pos);
        if (end == std::string::npos)
            end = cnf.size();
        std::string line = cnf.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (same_text(trim(line.substr(0, eq)), key)) {
            *value = trim(line.substr(eq + 1));
            return true;
        }
    }
    return false;
}

/* "cdrom0:\SCUS_971.02;1" -> "SCUS_971.02". */
std::string boot_file(const std::string &boot2)
{
    size_t start = boot2.find_last_of("\\/:");
    std::string name = start == std::string::npos ? boot2 : boot2.substr(start + 1);
    size_t semi = name.find(';');
    if (semi != std::string::npos)
        name.resize(semi);
    return trim(name);
}

/* The RoFS table of contents at the start of a volume (volume.h): magic
 * "RoFS", or its bitwise inverse when the TOC body is stored inverted; word 3
 * = header size, word 4 = body size. */
bool rofs_header_ok(const uint8_t *hdr, uint64_t file_size)
{
    static const uint8_t magic[4] = {'R', 'o', 'F', 'S'};
    static const uint8_t inverted[4] = {0xAD, 0x90, 0xB9, 0xAC};
    if (memcmp(hdr, magic, 4) != 0 && memcmp(hdr, inverted, 4) != 0)
        return false;
    uint32_t w3, w4;
    memcpy(&w3, hdr + 12, 4);
    memcpy(&w4, hdr + 16, 4);
    return w3 >= 0x20 && (uint64_t)w3 + w4 <= file_size;
}

bool is_folder(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_file(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Deletes a folder and everything in it. */
void delete_tree(const std::wstring &folder)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((util::with_slash(folder) + L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring p = util::with_slash(folder) + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(p);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(folder.c_str());
}

bool write_file(const std::wstring &path, const uint8_t *p, size_t size, DWORD *err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    while (size) {
        DWORD n = size > (64u << 20) ? (64u << 20) : (DWORD)size, put = 0;
        if (!WriteFile(h, p, n, &put, nullptr) || put == 0) {
            *err = GetLastError();
            CloseHandle(h);
            return false;
        }
        p += put;
        size -= put;
    }
    CloseHandle(h);
    return true;
}

std::wstring full_folder(const std::wstring &folder_in)
{
    wchar_t full[MAX_PATH * 4];
    DWORD fn = GetFullPathNameW(folder_in.c_str(), MAX_PATH * 4, full, nullptr);
    return util::with_slash(fn && fn < MAX_PATH * 4 ? std::wstring(full, fn) : folder_in);
}

/* The demo replays of the licence tests and the arcade time trials (this
 * tree's demo\: resources of this type named as the files;
 * src/launcher/CMakeLists.txt.  Off Windows: byte arrays of a generated
 * source, cmake/demos.cmake). */
struct Demo {
    std::wstring name;
    const void *data;                       /* (null: missing) */
    DWORD size;
};

#ifdef _WIN32
constexpr const wchar_t *kDemoResource = L"GT3DEMO";

BOOL CALLBACK list_demo(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR names)
{
    reinterpret_cast<std::vector<std::wstring> *>(names)->push_back(name);
    return TRUE;
}
#endif

std::vector<Demo> launcher_demos()
{
    std::vector<Demo> demos;
#ifdef _WIN32
    HMODULE inst = GetModuleHandleW(nullptr);
    std::vector<std::wstring> names;
    EnumResourceNamesW(inst, kDemoResource, list_demo, reinterpret_cast<LONG_PTR>(&names));
    for (const std::wstring &name : names) {
        HRSRC r = FindResourceW(inst, name.c_str(), kDemoResource);
        HGLOBAL res = r ? LoadResource(inst, r) : nullptr;
        const void *p = res ? LockResource(res) : nullptr;
        const DWORD size = r ? SizeofResource(inst, r) : 0;
        demos.push_back({name, p, size});
    }
#else
    const char *name = nullptr;
    size_t size = 0;
    for (size_t i = 0; const unsigned char *p = gt3pc_demo_file(i, &name, &size); i++)
        demos.push_back({util::widen(name), p, (DWORD)size});
#endif
    return demos;
}

/* Reads the whole file `path` into `out` (false when it cannot be, or is
 * larger than `max`).  Below (read_small_file). */
bool read_small_file(const std::wstring &path, size_t max, std::vector<uint8_t> *out);

/* Writes the demos over the disc's in the data folder `data`.  The engine is
 * the same for every region and opens demo\<name> in all of them
 * (load_demo_setting); the European disc has its PAL copies beside those,
 * <name>_p (and L0S0001_uk, L0S0001_uk_p), which its own code read: each file
 * goes to its plain name and over any such copy.  `changed`: only the files
 * whose bytes differ from the launcher's (an installed version refreshed at
 * the start, refresh_demos; a file already the same is left untouched).
 * *count: the demos written (every one of the set, or with `changed` those
 * that had a file to write); false with the reason in *message when one
 * cannot be written.  discFiles (PC 2026-11-14, a disc image's folder:
 * prepare_disc): the names of the disc's own demo files, which are not in the
 * folder - its copies of a demo (<name>_*) are written there too, so the game
 * reads the launcher's in their place as in an install. */
bool write_demos(const std::wstring &data, bool changed, int *count, std::wstring *message,
                 const std::vector<std::wstring> *discFiles = nullptr)
{
    const std::vector<Demo> demos = launcher_demos();
    const std::wstring folder = util::with_slash(data) + L"demo";
    if (!demos.empty() && !util::make_folders(folder)) {
        *message = trf(L"The folder %s could not be created: %s.", folder.c_str(),
                       util::system_error_text(GetLastError()).c_str());
        return false;
    }
    int written = 0;
    for (const Demo &demo : demos) {
        const std::wstring &name = demo.name;
        const void *p = demo.data;
        const DWORD size = demo.size;
        if (p == nullptr || size == 0) {
            *message = trf(L"The demo replay %s is missing from the launcher.", name.c_str());
            return false;
        }
        std::vector<std::wstring> files{name};
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((folder + L"\\" + name + L"_*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                files.push_back(fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        if (discFiles != nullptr)
            for (const std::wstring &d : *discFiles) {
                if (d.size() <= name.size() + 1 || d[name.size()] != L'_' ||
                    _wcsnicmp(d.c_str(), name.c_str(), name.size()) != 0)
                    continue;
                bool have = false;
                for (const std::wstring &file : files)
                    have = have || _wcsicmp(file.c_str(), d.c_str()) == 0;
                if (!have)
                    files.push_back(d);
            }
        bool any = false;
        for (const std::wstring &file : files) {
            const std::wstring out = folder + L"\\" + file;
            if (changed) {
                std::vector<uint8_t> have;
                if (read_small_file(out, size, &have) && have.size() == size &&
                    memcmp(have.data(), p, size) == 0)
                    continue;
            }
            DWORD err = 0;
            if (!write_file(out, static_cast<const uint8_t *>(p), size, &err)) {
                *message = trf(L"Writing %s failed: %s.", out.c_str(), util::system_error_text(err).c_str());
                return false;
            }
            any = true;
        }
        if (any)
            written++;
    }
    *count = changed ? written : (int)demos.size();
    return true;
}

/* How long a folder's rename waits for the other programs holding its files
 * open (move_folder). */
constexpr DWORD kMoveWaitMs = 120000;

/* Renames the folder `from` to `to`.  A folder with a file open in another
 * program cannot be renamed (access denied, or a sharing violation), and
 * right after the unpack an antivirus or the search indexer is often still
 * reading the new files (user report: the install failed at its end,
 * access denied, the files whole in data.part): tried again every 250 ms
 * for up to kMoveWaitMs, the phase saying so.  *err: the last error. */
bool move_folder(const std::wstring &from, const std::wstring &to, Listener &listener, DWORD *err)
{
    const DWORD start = GetTickCount();
    bool told = false;
    for (;;) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
            return true;
        *err = GetLastError();
        if ((*err != ERROR_ACCESS_DENIED && *err != ERROR_SHARING_VIOLATION) || GetTickCount() - start > kMoveWaitMs ||
            listener.cancel_requested())
            return false;
        if (!told) {
            listener.phase(tr(L"Putting the game files in place: waiting for another program to let go of them "
                              L"(the game, an antivirus scan?)..."));
            told = true;
        }
        Sleep(250);
    }
}

/* Unpacks every file of the volume `read` gives (`volume_size` bytes) into
 * folder\<dataName>.part, in the unified structure (`language`: the disc's,
 * for its courses' region folder), then puts it in place as
 * folder\<dataName>; the marker starts with `head` (marker_head).  convert:
 * the music converted for the PC (music.h) before the marker. */
Result unpack(const volume::Reader &read, uint64_t volume_size, const std::wstring &folder_in,
              const std::wstring &dataName, const std::wstring &source, const std::string &head,
              const std::string &language, bool convert, Listener &listener, std::wstring *message)
{
    std::wstring err;
    const std::wstring folder = full_folder(folder_in);
    if (!util::make_folders(folder)) {
        *message = trf(L"The folder %s could not be created: %s.", folder.c_str(),
                       util::system_error_text(GetLastError()).c_str());
        return Result::failed;
    }
    const std::wstring data = folder + dataName, part = data + kPartSuffix, old = data + kOldSuffix;

    listener.phase(tr(L"Reading the list of game files..."));
    std::vector<volume::File> files;
    if (!volume::list_files(read, volume_size, &files, &err)) {
        *message = trf(L"The game files could not be listed (%s). Make a new image of the disc.", err.c_str());
        return Result::failed;
    }
    /* the unified structure: courses in courses/ itself and none in a
     * folder of it (the Japanese disc) go into the region's folder */
    const bool flatCourses = volume::flat_courses(files);
    const std::string region = volume::course_region(language);
    /* in disc order: one pass over the image */
    std::sort(files.begin(), files.end(), [](const volume::File &a, const volume::File &b) { return a.lsn < b.lsn; });
    uint64_t total = 0, adsBytes = 0;
    for (const volume::File &f : files) {
        total += f.size;
        if (f.path.size() > 8 && _strnicmp(f.path.c_str(), "bgm/", 4) == 0 &&
            _stricmp(f.path.c_str() + f.path.size() - 4, ".ads") == 0)
            adsBytes += f.size;
    }

    /* Room for the files (an installed data folder stays until the swap),
     * the music's .wav files too */
    ULARGE_INTEGER avail = {};
    if (GetDiskFreeSpaceExW(folder.c_str(), &avail, nullptr, nullptr)) {
        uint64_t need = total + (64u << 20) + (convert ? music::growth(adsBytes) : 0);
        if (avail.QuadPart < need) {
            *message = trf(L"There is not enough free disk space in %s: the game files need %s, and %s is free.",
                           folder.c_str(), util::format_size(need).c_str(), util::format_size(avail.QuadPart).c_str());
            return Result::failed;
        }
    }

    /* Left over by an install that was killed. */
    if (is_folder(part))
        delete_tree(part);
    if (is_folder(old))
        delete_tree(old);
    if (!util::make_folders(part)) {
        *message = trf(L"The folder %s could not be created: %s.", part.c_str(),
                       util::system_error_text(GetLastError()).c_str());
        return Result::failed;
    }

    listener.phase(tr(L"Unpacking the game files..."));
    std::vector<uint8_t> stored, unpacked;
    uint64_t done = 0;
    std::wstring lastFolder;
    listener.progress(0, total);
    for (const volume::File &f : files) {
        if (listener.cancel_requested()) {
            delete_tree(part);
            *message = tr(L"The installation was cancelled.");
            return Result::cancelled;
        }
        std::wstring rel = util::widen(volume::unified_path(f.path, flatCourses, region));
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const std::wstring out = util::with_slash(part) + rel;
        const std::wstring outFolder = out.substr(0, out.find_last_of(L'\\'));
        if (outFolder != lastFolder) {
            if (!util::make_folders(outFolder)) {
                *message = trf(L"The folder %s could not be created: %s.", outFolder.c_str(),
                               util::system_error_text(GetLastError()).c_str());
                delete_tree(part);
                return Result::failed;
            }
            lastFolder = outFolder;
        }
        stored.resize(f.stored);
        if (f.stored && !read((uint64_t)f.lsn * volume::kSectorSize, stored.data(), f.stored)) {
            *message = trf(L"The disc image could not be read (%s).", util::widen(f.path).c_str());
            delete_tree(part);
            return Result::failed;
        }
        const uint8_t *p = stored.data();
        size_t n = stored.size();
        if (f.packed) {
            if (!inflate::gunzip(stored.data(), stored.size(), &unpacked, &err) || unpacked.size() != f.size) {
                *message = trf(L"The game file %s could not be unpacked (%s). Make a new image of the disc.",
                               util::widen(f.path).c_str(), err.empty() ? tr(L"wrong size") : err.c_str());
                delete_tree(part);
                return Result::failed;
            }
            p = unpacked.data();
            n = unpacked.size();
        }
        DWORD werr = 0;
        if (!write_file(out, p, n, &werr)) {
            *message = trf(L"Writing %s failed: %s.", out.c_str(), util::system_error_text(werr).c_str());
            delete_tree(part);
            return Result::failed;
        }
        done += f.size;
        listener.progress(done, total);
    }

    /* The music as the PC plays it (music.h); what could not be converted
     * stays as it was (the game plays the .ads) */
    std::wstring musicNote;
    if (convert) {
        listener.phase(tr(L"Converting the music to WAV..."));
        if (music::convert(part, listener, &musicNote) == Result::cancelled) {
            delete_tree(part);
            *message = tr(L"The installation was cancelled.");
            return Result::cancelled;
        }
    }
    /* The game configs as SQLite databases (gameconf.h); the .gcf files go
     * once their databases are checked */
    std::wstring confNote;
    if (convert) {
        listener.phase(tr(L"Writing the game configs as databases..."));
        gameconf::convert(part, &confNote);
    }
    /* The database folder as SQLite databases (databases.h); its .db files
     * go once their databases are checked */
    std::wstring dbNote;
    if (convert) {
        listener.phase(tr(L"Writing the database folder as databases..."));
        databases::convert(part, &dbNote);
    }
    /* The arcade menus' stills as PNG files (stills.h); course.ipic goes once
     * they are checked */
    std::wstring stillNote;
    if (convert) {
        listener.phase(tr(L"Writing the arcade menus' stills as PNG files..."));
        stills::convert(part, &stillNote);
    }
    /* PC (2026-11-08): the image packs as folders of PNG files
     * (image_packs.h); each .imgs goes once its folder is checked */
    std::wstring packNote;
    if (convert) {
        listener.phase(tr(L"Writing the image packs as PNG files..."));
        image_packs::convert(part, &packNote);
    }
    /* The demo replays (the licence tests', the arcade time trials') over the
     * disc's */
    int demos = 0;
    if (convert) {
        listener.phase(tr(L"Writing the demo replays..."));
        if (!write_demos(part, false, &demos, message)) {
            delete_tree(part);
            return Result::failed;
        }
    }

    /* The marker, last: a data folder without it is incomplete. */
    std::string marker = head;
    marker += "files " + std::to_string(files.size()) + "\r\nbytes " + std::to_string(total) + "\r\nsource " +
              util::narrow(source) + "\r\n";
    DWORD werr = 0;
    if (!write_file(util::with_slash(part) + kMarkerFile, (const uint8_t *)marker.data(), marker.size(), &werr)) {
        *message = trf(L"Writing the install marker failed: %s.", util::system_error_text(werr).c_str());
        delete_tree(part);
        return Result::failed;
    }

    /* Into place. */
    listener.phase(tr(L"Putting the game files in place..."));
    DWORD e = 0;
    if (is_folder(data) && !move_folder(data, old, listener, &e)) {
        *message = trf(L"The installed game files in %s could not be replaced: %s.", data.c_str(),
                       util::system_error_text(e).c_str());
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
            *message += std::wstring(L"\n\n") + tr(L"If the game is running, close it and install again.");
        delete_tree(part);
        return Result::failed;
    }
    if (!move_folder(part, data, listener, &e)) {
        if (is_folder(old))
            MoveFileExW(old.c_str(), data.c_str(), MOVEFILE_WRITE_THROUGH);
        *message = trf(L"The game files could not be put in place as %s: %s.", data.c_str(),
                       util::system_error_text(e).c_str());
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
            *message += L"\n\n" + trf(L"Another program kept a file of %s open (an antivirus scan of the new files, "
                                      L"the search indexer, an Explorer window in it). Install again.",
                                      part.c_str());
        return Result::failed;
    }
    if (is_folder(old))
        delete_tree(old);
    if (install_state(folder, dataName) != InstallState::ok) {
        *message = trf(L"The installed game files in %s could not be checked. Install again.", data.c_str());
        return Result::failed;
    }
    *message = trf(L"Installed %d game files (%s) in %s.", (int)files.size(), util::format_size(total).c_str(),
                   data.c_str());
    if (flatCourses)
        *message += L" " + trf(L"Its courses are in courses\\%s (the unified structure).", util::widen(region).c_str());
    if (!musicNote.empty())
        *message += L" " + musicNote;
    if (!confNote.empty())
        *message += L" " + confNote;
    if (!dbNote.empty())
        *message += L" " + dbNote;
    if (!stillNote.empty())
        *message += L" " + stillNote;
    if (!packNote.empty())
        *message += L" " + packNote;
    if (demos != 0)
        *message += L" " + trf(L"%d demo replays (the licence tests', the arcade time trials') written over the "
                               L"disc's.",
                               demos);
    return Result::ok;
}

} // namespace

bool refresh_demos(const std::wstring &data, int *written, std::wstring *message)
{
    return write_demos(data, true, written, message);
}

const wchar_t *version_name(Version v)
{
    switch (v) {
    case Version::us:
        return L"Gran Turismo 3 A-Spec (USA)";
    case Version::eu:
        return L"Gran Turismo 3 A-Spec (Europe)";
    case Version::jp:
        return L"Gran Turismo 3 A-Spec (Japan)";
    default:
        return L"data only";
    }
}

std::wstring version_folder(Version v)
{
    const char *c = version_code(v);
    return c[0] != 0 ? std::wstring(kDataFolder) + L"_" + util::widen(c) : std::wstring();
}

const char *version_code(Version v)
{
    switch (v) {
    case Version::us:
        return "US";
    case Version::eu:
        return "EU";
    case Version::jp:
        return "JP";
    default:
        return "";
    }
}

InstallState install_state(const std::wstring &folder, const std::wstring &data_name)
{
    const std::wstring data = util::with_slash(folder) + data_name;
    if (data_name.empty() || !is_folder(data))
        return InstallState::missing;
    return is_file(util::with_slash(data) + kMarkerFile) ? InstallState::ok : InstallState::damaged;
}

bool has_volume(const std::wstring &folder)
{
    HANDLE h = CreateFileW((util::with_slash(folder) + kVolumeFile).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    uint8_t hdr[0x40] = {};
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &size) && ReadFile(h, hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) &&
              rofs_header_ok(hdr, (uint64_t)size.QuadPart);
    CloseHandle(h);
    return ok;
}

namespace {

/* The marker's lines (folder\<data>\kMarkerFile), empty without one. */
std::vector<std::string> marker_lines(const std::wstring &folder, const std::wstring &data)
{
    std::vector<std::string> lines;
    HANDLE h = CreateFileW((util::with_slash(folder) + data + L"\\" + kMarkerFile).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return lines;
    char buf[8192];
    DWORD got = 0;
    std::string text;
    if (ReadFile(h, buf, sizeof(buf), &got, nullptr))
        text.assign(buf, got);
    CloseHandle(h);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        lines.push_back(trim(text.substr(pos, end - pos)));
        pos = end + 1;
    }
    return lines;
}

/* The value of the marker's line "<key> <value>", "" if none. */
std::string marker_value(const std::vector<std::string> &lines, const char *key)
{
    const size_t n = strlen(key);
    for (const std::string &l : lines)
        if (l.size() > n && l.compare(0, n, key) == 0 && l[n] == ' ')
            return trim(l.substr(n + 1));
    return std::string();
}

/* A RoFS volume, whole in the image. */
bool volume_ok(const iso9660::Image &image, const iso9660::Entry &v)
{
    uint8_t hdr[0x40];
    std::wstring err;
    return !v.is_directory && v.size >= sizeof(hdr) && image.read(v, 0, hdr, sizeof(hdr), &err) &&
           rofs_header_ok(hdr, v.size);
}

/* The file `path` into `out`; false if missing or larger than `max`. */
bool read_small_file(const std::wstring &path, size_t max, std::vector<uint8_t> *out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && (uint64_t)size.QuadPart <= max;
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    return ok;
}

} // namespace

std::wstring installed_release(const std::wstring &folder, const std::wstring &data)
{
    if (install_state(folder, data) == InstallState::missing)
        return std::wstring();
    const std::vector<std::string> lines = marker_lines(folder, data);
    std::string r = marker_value(lines, "release");
    if (r.empty() && !lines.empty()) {
        /* an earlier launcher's: "<release>: the game files, ..." */
        const size_t colon = lines[0].find(": the game files");
        r = colon != std::string::npos ? lines[0].substr(0, colon) : util::narrow(kEarlierRelease);
    }
    return util::widen(r);
}

std::string installed_language(const std::wstring &folder, const std::wstring &data)
{
    const std::string l = marker_value(marker_lines(folder, data), "language");
    return l.empty() ? std::string("US") : l;
}

/* PC (2026-11-15) */
std::string marker_info(const std::wstring &data, const char *key)
{
    std::wstring d = data;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
        d.pop_back();
    return marker_value(marker_lines(std::wstring(), d), key);
}

bool language_installed(const std::wstring &folder, const std::wstring &data, const char *code)
{
    return is_file(util::with_slash(folder) + data + L"\\menu\\" + util::widen(code) + L"\\gt_mode301.pmb");
}

std::vector<Extraction> extractions(const std::wstring &folder)
{
    std::vector<Extraction> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((util::with_slash(folder) + kDataFolder + L"_*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const std::wstring name = fd.cFileName;
        bool version = false;
        for (int v = 0; v < kVersions; v++)
            version = version || _wcsicmp(name.c_str(), version_folder((Version)v).c_str()) == 0;
        if (version || install_state(folder, name) != InstallState::ok)
            continue;                       /* (the versions, a .part / .old, an unfinished one) */
        out.push_back({name, installed_release(folder, name)});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const Extraction &a, const Extraction &b) { return a.data < b.data; });
    return out;
}

std::wstring migrate_earlier_install(const std::wstring &folder)
{
    if (install_state(folder, kDataFolder) != InstallState::ok)
        return std::wstring();
    const std::string boot = marker_value(marker_lines(folder, kDataFolder), "boot");
    /* an earlier launcher's marker without a boot file: the US disc */
    const Version v = boot.empty() ? Version::us : version_of(boot);
    const std::wstring target = data_folder_of(v, boot, std::string(), std::string());
    const std::wstring from = util::with_slash(folder) + kDataFolder, to = util::with_slash(folder) + target;
    if (is_folder(to) || !MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
        return std::wstring();
    return trf(L"The game files in data were moved to %s (a data folder per version).", target.c_str());
}

bool check_disc(const iso9660::Image &image, DiscInfo *info, std::wstring *message, bool *wrong_disc)
{
    *wrong_disc = false;
    std::wstring err;

    /* CORE.GT3, the engine's main program: at the root, else in a folder of
     * the root (a demo disc's game) */
    info->folder.clear();
    bool found = image.find("CORE.GT3", &info->core, &err) && !info->core.is_directory;
    if (!found) {
        std::vector<iso9660::Entry> root;
        if (!image.list(image.root(), &root, &err)) {
            *message = trf(L"The disc image could not be read: %s.", err.c_str());
            return false;
        }
        for (const iso9660::Entry &d : root)
            if (d.is_directory && image.find(d.name + "/CORE.GT3", &info->core, &err) && !info->core.is_directory) {
                info->folder = d.name;
                found = true;
                break;
            }
    }
    if (!found) {
        *wrong_disc = true;
        *message = tr(L"This disc image is not a Gran Turismo 3 engine disc: it has no CORE.GT3 (the engine's main "
                      L"program).");
        return false;
    }

    /* SYSTEM.CNF's boot file names a known release (on a demo disc, the
     * disc's menu: the game is named by its folder) */
    info->boot.clear();
    iso9660::Entry cnf_entry;
    std::string cnf, boot2;
    if (image.find("SYSTEM.CNF", &cnf_entry, &err) && image.read_all(cnf_entry, &cnf, &err) &&
        cnf_value(cnf, "BOOT2", &boot2))
        info->boot = boot_file(boot2);
    info->release = release_name(info->boot, info->folder, image.volume_id());
    /* the main versions are played, the rest extracted as data only */
    info->version = info->folder.empty() ? version_of(info->boot) : Version::data_only;
    info->data = data_folder_of(info->version, info->folder.empty() ? info->boot : std::string(), info->folder,
                                image.volume_id());

    /* the volume beside it: GT3.VOL, else another RoFS volume (*.VOL); whole
     * in the image, and starting with its table of contents */
    const std::string dir = info->folder.empty() ? std::string() : info->folder + "/";
    bool vol = image.find(dir + "GT3.VOL", &info->volume, &err) && !info->volume.is_directory;
    if (!vol) {
        iso9660::Entry d;
        std::vector<iso9660::Entry> entries;
        if ((info->folder.empty() ? (d = image.root(), true) : image.find(info->folder, &d, &err)) &&
            image.list(d, &entries, &err))
            for (const iso9660::Entry &e : entries)
                if (e.name.size() > 4 && same_text(e.name.substr(e.name.size() - 4), ".VOL") && volume_ok(image, e)) {
                    info->volume = e;
                    vol = true;
                    break;
                }
    }
    if (!vol) {
        *message = tr(L"The disc image has CORE.GT3 but no game volume (GT3.VOL) beside it: the image is damaged. "
                      L"Make a new image of the disc.");
        return false;
    }
    const std::wstring name = util::widen(info->volume.name);
    if (!image.inside_image(info->volume)) {
        *message = trf(L"The disc image is incomplete: it ends before %s does. Make a new image of the disc.",
                       name.c_str());
        return false;
    }
    if (!volume_ok(image, info->volume)) {
        *message = trf(L"%s on this disc image is damaged (it has no RoFS table of contents). "
                       L"Make a new image of the disc.",
                       name.c_str());
        return false;
    }
    return true;
}

Result install_from_iso(const std::wstring &iso_path, const std::wstring &folder, Listener &listener,
                        std::wstring *message)
{
    listener.phase(tr(L"Checking the disc image..."));
    iso9660::Image image;
    std::wstring err;
    if (!image.open(iso_path, &err)) {
        *message = trf(L"The disc image could not be used: %s.", err.c_str());
        return Result::failed;
    }
    DiscInfo disc;
    bool wrong_disc = false;
    if (!check_disc(image, &disc, message, &wrong_disc))
        return wrong_disc ? Result::wrong_disc : Result::failed;
    /* what its code reads (the databases) and its language */
    std::string core;
    std::vector<uint8_t> coreBytes;
    if (image.read_all(disc.core, &core, &err))
        coreBytes.assign(core.begin(), core.end());
    const CoreFacts facts = core_facts(coreBytes, disc.folder.empty() ? disc.boot : std::string());
    listener.phase(disc.version == Version::data_only ? trf(L"Extracting %s...", disc.release.c_str())
                                                      : trf(L"Installing %s...", disc.release.c_str()));
    const iso9660::Entry &vol = disc.volume;
    volume::Reader read = [&image, &vol](uint64_t offset, void *buf, uint32_t size) {
        std::wstring e;
        return image.read(vol, offset, buf, size, &e);
    };
    return unpack(read, vol.size, folder, disc.data, iso_path, marker_head(disc.release, disc.boot, facts),
                  facts.language, disc.version != Version::data_only, listener, message);
}

Result install_from_volume(const std::wstring &volume_path, const std::wstring &folder, Listener &listener,
                           std::wstring *message)
{
    listener.phase(tr(L"Checking gt3.vol..."));
    HANDLE h = CreateFileW(volume_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *message = trf(L"The file %s could not be opened: %s.", volume_path.c_str(),
                       util::system_error_text(GetLastError()).c_str());
        return Result::failed;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    /* the release: a CORE.GT3 and SYSTEM.CNF beside the volume (the
     * development tree's extracted disc), else what an earlier launcher
     * installed (the US disc) */
    const std::wstring dir = volume_path.substr(0, volume_path.find_last_of(L"\\/") + 1);
    std::vector<uint8_t> core, cnf;
    std::string boot, boot2;
    if (read_small_file(dir + L"SYSTEM.CNF", 4096, &cnf) &&
        cnf_value(std::string(cnf.begin(), cnf.end()), "BOOT2", &boot2))
        boot = boot_file(boot2);
    std::wstring release = kEarlierRelease;
    CoreFacts facts;
    facts.language = "US";
    Version version = Version::us;
    if (read_small_file(dir + L"CORE.GT3", 64u << 20, &core)) {
        facts = core_facts(core, boot);
        release = release_name(boot, std::string(), std::string());
        version = version_of(boot);
    }
    const std::wstring data = data_folder_of(version, boot, std::string(), std::string());
    volume::Reader read = [h](uint64_t offset, void *buf, uint32_t n) {
        uint8_t *p = static_cast<uint8_t *>(buf);
        while (n) {
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)offset;
            ov.OffsetHigh = (DWORD)(offset >> 32);
            DWORD got = 0;
            if (!ReadFile(h, p, n, &got, &ov) || got == 0)
                return false;
            p += got;
            offset += got;
            n -= got;
        }
        return true;
    };
    Result r = unpack(read, (uint64_t)size.QuadPart, folder, data, volume_path, marker_head(release, boot, facts),
                      facts.language, version != Version::data_only, listener, message);
    CloseHandle(h);
    return r;
}

/* PC (2026-11-14): a disc image's own data folder (install.h). */
Result prepare_disc(const std::wstring &iso_path, const std::wstring &folder_in, DiscFolder *out, std::wstring *message)
{
    /* (the path the game opens: whole, whatever folder it starts in) */
    wchar_t whole[2048];
    const DWORD wn = GetFullPathNameW(iso_path.c_str(), 2048, whole, nullptr);
    const std::wstring path = wn != 0 && wn < 2048 ? std::wstring(whole, wn) : iso_path;
    iso9660::Image image;
    std::wstring err;
    if (!image.open(path, &err)) {
        *message = trf(L"The disc image %s could not be used: %s.", path.c_str(), err.c_str());
        return Result::failed;
    }
    DiscInfo disc;
    bool wrong_disc = false;
    if (!check_disc(image, &disc, message, &wrong_disc))
        return wrong_disc ? Result::wrong_disc : Result::failed;
    /* A variant of a version - its number under other letters: a prototype's
     * SCEF_502.94 for Europe's SCES_502.94 - is played as that version: its
     * marker has the version's boot file, which the game names the saves by
     * (T108 mcGetNamePrefix); its folder is named by its own. */
    Version version = disc.version;
    std::string versionBoot = disc.boot;
    if (version == Version::data_only && disc.folder.empty()) {
        static const char *const kBoots[kVersions] = {"SCUS_971.02", "SCES_502.94", "SCPS_150.09"};
        for (int v = 0; v < kVersions; v++)
            if (disc.boot.size() == strlen(kBoots[v]) && _stricmp(disc.boot.c_str() + 4, kBoots[v] + 4) == 0) {
                version = (Version)v;
                versionBoot = kBoots[v];
            }
    }
    if (version == Version::data_only) {
        *message = trf(L"%s is not a version the game plays (USA, Europe, Japan). \"Install game files...\" "
                       L"extracts its files as data only.",
                       disc.release.c_str());
        return Result::wrong_disc;
    }
    /* (a variant's name: its version's with its own serial, "Gran Turismo 3
     * A-Spec (Europe, SCEF-50294)") */
    std::wstring release = disc.release;
    if (versionBoot != disc.boot) {
        release = version_name(version);
        if (!release.empty() && release.back() == L')')
            release.pop_back();
        release += L", " + util::widen(serial(disc.boot)) + L")";
    }
    /* what its code reads (the databases) and its language, as an install's marker says them */
    std::string core;
    std::vector<uint8_t> coreBytes;
    if (image.read_all(disc.core, &core, &err))
        coreBytes.assign(core.begin(), core.end());
    const CoreFacts facts = core_facts(coreBytes, disc.boot);
    /* its folder: the boot file and the CRC-32 of the volume's table of contents */
    uint8_t hdr[0x40];
    uint32_t headerSize = 0, bodySize = 0;
    std::vector<uint8_t> toc;
    bool tocOk = image.read(disc.volume, 0, hdr, sizeof(hdr), &err);
    if (tocOk) {
        memcpy(&headerSize, hdr + 12, 4);
        memcpy(&bodySize, hdr + 16, 4);
        const uint64_t tocSize = (uint64_t)headerSize + bodySize;
        tocOk = tocSize <= disc.volume.size && tocSize <= (64u << 20);
        if (tocOk) {
            toc.resize((size_t)tocSize);
            tocOk = image.read(disc.volume, 0, toc.data(), (uint32_t)tocSize, &err);
        }
    }
    if (!tocOk) {
        *message = err.empty()
                       ? std::wstring(tr(L"The disc image's game volume could not be read. Make a new image of the "
                                         L"disc."))
                       : trf(L"The disc image's game volume could not be read (%s). Make a new image of the disc.",
                             err.c_str());
        return Result::failed;
    }
    std::string boot = disc.boot;
    for (char &c : boot)
        if (c < 0x20 || strchr("<>:\"/\\|?* ", c) != nullptr)
            c = '_';
    wchar_t id[16];
    swprintf(id, 16, L"%08X", (unsigned)inflate::crc32(0, toc.data(), toc.size()));
    out->name = util::widen(boot) + L"_" + id;
    out->release = release;
    out->version = version;

    const std::wstring data = full_folder(folder_in) + kDiscsFolder + L"\\" + out->name;
    if (!util::make_folders(data)) {
        *message = trf(L"The folder %s could not be created: %s.", data.c_str(),
                       util::system_error_text(GetLastError()).c_str());
        return Result::failed;
    }
    /* the demo replays (the licence tests', the arcade time trials'), which
     * the game reads in place of the disc's - the disc's copies of them too
     * (<name>_p: the volume's demo files tell); then the marker, last */
    std::vector<std::wstring> discDemos;
    {
        std::vector<volume::File> files;
        const iso9660::Entry &vol = disc.volume;
        const volume::Reader read = [&image, &vol](uint64_t offset, void *buf, uint32_t size) {
            std::wstring e;
            return image.read(vol, offset, buf, size, &e);
        };
        if (!volume::list_files(read, vol.size, &files, &err)) {
            *message = trf(L"The disc image's game files could not be listed (%s). Make a new image of the disc.",
                           err.c_str());
            return Result::failed;
        }
        for (const volume::File &f : files)
            if (f.path.size() > 5 && _strnicmp(f.path.c_str(), "demo/", 5) == 0 && f.path.find('/', 5) == std::string::npos)
                discDemos.push_back(util::widen(f.path.substr(5)));
    }
    int demos = 0;
    if (!write_demos(data, true, &demos, message, &discDemos))
        return Result::failed;
    std::string marker = marker_head(release, versionBoot, facts, "read from its disc image");
    if (versionBoot != disc.boot)
        marker += "disc " + disc.boot + "\r\n";         /* (its own boot file, for the reader) */
    marker += "image " + util::narrow(path) + "\r\n";
    marker += "volume " + (disc.folder.empty() ? std::string() : disc.folder + "/") + disc.volume.name + "\r\n";
    const std::wstring markerPath = util::with_slash(data) + kMarkerFile;
    std::vector<uint8_t> have;
    if (read_small_file(markerPath, 1u << 20, &have) && have.size() == marker.size() &&
        memcmp(have.data(), marker.data(), marker.size()) == 0)
        return Result::ok;                  /* (as it is: the game may be reading it) */
    DWORD werr = 0;
    if (!write_file(markerPath, (const uint8_t *)marker.data(), marker.size(), &werr)) {
        *message = trf(L"Writing %s failed: %s.", markerPath.c_str(), util::system_error_text(werr).c_str());
        if (werr == ERROR_SHARING_VIOLATION || werr == ERROR_ACCESS_DENIED)
            *message += std::wstring(L"\n\n") + tr(L"If the game is running from this disc image, close it first.");
        return Result::failed;
    }
    return Result::ok;
}

} // namespace install
