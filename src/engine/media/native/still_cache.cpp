/* still_cache.cpp - the arcade menus' stills in memory (still_cache.h). */
#include "engine/media/native/still_cache.h"

#include "engine/media/native/mp4_movie.h"

#include "platform/host_file.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/png_image.h"

#ifdef _WIN32
#include <windows.h>
#else
/* off Windows the files through platform/os/fs.h ('\\' and '/' both
 * separate there) */
#include "platform/os/fs.h"
#include "platform/os/win_compat.h"
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gt3media {
namespace {

constexpr const char *kCacheFolders[] = {"cache\\", "cache\\movie_frames\\"};
constexpr const char *kMovies = "host:data/stream";
constexpr const char *kStills = "host:data/stream/course";
constexpr const char *kMovieEnd = "_full.mp4";
constexpr const char *kStillEnd = ".png";
/* What memory holds at most, past it a still is read when it is shown (the
 * 42 of the disc are 10 MB): the PNG files of a mod's 4K stills are big. */
constexpr uint64_t kMaxBytes = 512u << 20;
constexpr DWORD kMaxFile = 256u << 20;
constexpr unsigned kMaxThreads = 8;

struct Still {
    std::vector<uint8_t> png;
    bool movie = false;
};
/* By the lower-case name.  Filled before the game starts, only read after. */
std::unordered_map<std::string, Still> g_stills;
uint64_t g_bytes;

struct Movie {
    std::string name;                       /* the still's: "midfield" */
    std::string path;                       /* the .mp4's Windows path */
    std::string base;                       /* its files in the cache start so */
    std::string cached;                     /* its picture's file there */
};

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

#ifdef _WIN32
std::wstring widen(const std::string &s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

#else
std::wstring widen(const std::string &s)
{
    return fs::widen(s);                    /* (the native text is UTF-8) */
}

#endif

bool ends_with(const char *s, size_t n, const char *end, size_t endN)
{
    return n > endN && _stricmp(s + n - endN, end) == 0;
}

#ifdef _WIN32
bool read_file(const std::string &path, std::vector<uint8_t> *out)
{
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (LONGLONG)kMaxFile;
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out->data(), (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart;
    }
    CloseHandle(h);
    return ok;
}

/* base + the file's size and date: its picture's name; "" without the file. */
std::string cache_file(const std::string &base, const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
        return std::string();
    char key[64];
    snprintf(key, sizeof(key), "%llx-%08lx%08lx.png",
             ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow, (unsigned long)a.ftLastWriteTime.dwHighDateTime,
             (unsigned long)a.ftLastWriteTime.dwLowDateTime);
    return base + key;
}

#else
bool read_file(const std::string &path, std::vector<uint8_t> *out)
{
    fs::File f;
    if (!f.open(path, fs::kRead, fs::Create::open_existing, fs::kShareRead | fs::kShareDelete, fs::kSequential))
        return false;
    const int64_t size = f.size();
    bool ok = size > 0 && size <= (int64_t)kMaxFile;
    if (ok) {
        out->resize((size_t)size);
        ok = f.read_at(0, out->data(), (uint32_t)size) == (int)size;
    }
    return ok;
}

/* base + the file's size and date: its picture's name; "" without the file. */
std::string cache_file(const std::string &base, const std::string &path)
{
    fs::Stat st;
    if (!fs::stat(path, &st))
        return std::string();
    const uint64_t t = (uint64_t)st.modify;
    char key[64];
    snprintf(key, sizeof(key), "%llx-%08lx%08lx.png", (unsigned long long)st.size, (unsigned long)(t >> 32),
             (unsigned long)(t & 0xFFFFFFFFu));
    return base + key;
}

#endif

/* host_file_list: an .mp4 preview movie that is one (else its .pss plays). */
void add_movie(const char *file, void *arg)
{
    const size_t n = strlen(file), endN = strlen(kMovieEnd);
    if (!ends_with(file, n, kMovieEnd, endN))
        return;
    char full[1024];
    const std::string host = std::string(kMovies) + "/" + file;
    if (host_file_full_path(host.c_str(), full, sizeof(full)) != 0 || mp4_duration(full) < 0)
        return;
    Movie m;
    m.name = lower(std::string(file, n - endN));
    m.path = full;
    m.base = paths::game_dir() + kCacheFolders[1] + lower(std::string(file, n - 4)) + ".";
    m.cached = cache_file(m.base, m.path);
    if (!m.cached.empty())
        static_cast<std::vector<Movie> *>(arg)->push_back(std::move(m));
}

/* The other files of the movie's name: another movie's picture, or a write the
 * game was closed in. */
#ifdef _WIN32
void delete_older(const Movie &m)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((m.base + "*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    const std::string folder = m.base.substr(0, m.base.find_last_of('\\') + 1);
    do {
        const std::string p = folder + fd.cFileName;
        if (_stricmp(p.c_str(), m.cached.c_str()) != 0)
            DeleteFileA(p.c_str());
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

#else
void delete_older(const Movie &m)
{
    const size_t cut = m.base.find_last_of("/\\") + 1;
    const std::string folder = m.base.substr(0, cut), stem = m.base.substr(cut);
    std::vector<fs::Entry> entries;
    if (!fs::list(folder, &entries))
        return;
    for (const fs::Entry &e : entries) {
        if (e.st.dir || _strnicmp(e.name.c_str(), stem.c_str(), stem.size()) != 0)
            continue;
        const std::string p = folder + e.name;
        if (_stricmp(p.c_str(), m.cached.c_str()) != 0)
            fs::remove_file(p);
    }
}

#endif

/* The movie's first picture saved as its file in the cache. */
void take_picture(const Movie &m)
{
    MfScope mf;
    Mp4VideoReader reader;
    int64_t time = 0;
    const uint8_t *bgrx = nullptr;
    if (!mf.ok() || !reader.open(m.path.c_str()) || !reader.next(&time, &bgrx))
        return;
    const int w = reader.width(), h = reader.height();
    const size_t pixels = (size_t)w * h;
    std::vector<uint8_t> rgba(pixels * 4);
    uint8_t *d = rgba.data();
    for (size_t i = 0; i < pixels; i++, d += 4, bgrx += 4) {
        d[0] = bgrx[2];
        d[1] = bgrx[1];
        d[2] = bgrx[0];
        d[3] = 0xFF;
    }
    if (!png_image::encode_file(widen(m.cached), rgba.data(), w, h, reader.pixel_aspect())) {
        pclog::write("stills: %s could not be written", m.cached.c_str());
        return;
    }
    delete_older(m);
    pclog::write("stills: first picture of %s saved as %s", m.path.c_str(), m.cached.c_str());
}

/* All of them, each big picture's compression on a core of its own.  (Their
 * own threads: Media Foundation's COM is theirs.) */
void take_pictures(const std::vector<const Movie *> &movies)
{
    for (const char *folder : kCacheFolders)
#ifdef _WIN32
        CreateDirectoryA((paths::game_dir() + folder).c_str(), nullptr);
#else
        fs::make_dir(paths::game_dir() + folder);
#endif
    std::atomic<size_t> next{0};
    const auto work = [&movies, &next] {
        for (size_t i = next++; i < movies.size(); i = next++)
            take_picture(*movies[i]);
    };
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const size_t count = std::min<size_t>(movies.size(), std::min(cores, kMaxThreads));
    std::vector<std::thread> threads;
    for (size_t i = 0; i < count; i++)
        threads.emplace_back(work);
    for (std::thread &t : threads)
        t.join();
}

void keep(const std::string &name, std::vector<uint8_t> &&png, bool movie)
{
    if (g_bytes + png.size() > kMaxBytes) {
        pclog::write("stills: %s stays on the disk: memory holds %u MB of them already", name.c_str(),
                     (unsigned)(g_bytes >> 20));
        return;
    }
    g_bytes += png.size();
    Still &s = g_stills[name];
    s.png = std::move(png);
    s.movie = movie;
}

/* The movie's file in the cache, whole: false when it is not there or damaged,
 * and it has to be taken. */
bool keep_picture(const Movie &m)
{
    std::vector<uint8_t> png, rgba;
    int w = 0, h = 0;
    if (!read_file(m.cached, &png) || !png_image::decode(png.data(), png.size(), &rgba, &w, &h))
        return false;
    keep(m.name, std::move(png), true);
    return true;
}

/* host_file_list: a still, unless its movie's picture took its place. */
void add_still(const char *file, void *)
{
    const size_t n = strlen(file), endN = strlen(kStillEnd);
    if (!ends_with(file, n, kStillEnd, endN))
        return;
    const std::string name = lower(std::string(file, n - endN));
    if (g_stills.count(name) != 0)
        return;
    char full[1024];
    std::vector<uint8_t> png;
    const std::string host = std::string(kStills) + "/" + file;
    if (host_file_full_path(host.c_str(), full, sizeof(full)) == 0 && read_file(full, &png))
        keep(name, std::move(png), false);
}

} // namespace

void still_cache_prepare()
{
    const DWORD t0 = GetTickCount();
    std::vector<Movie> movies;
    host_file_list(kMovies, add_movie, &movies);
    std::vector<const Movie *> missing;
    for (const Movie &m : movies)
        if (!keep_picture(m))
            missing.push_back(&m);
    if (!missing.empty())
        take_pictures(missing);
    for (const Movie *m : missing)
        keep_picture(*m);
    const size_t pictures = g_stills.size();
    host_file_list(kStills, add_still, nullptr);
    /* The decoder's code in memory, and kept there: the first still shown does
     * not read it from the disk either. */
#ifdef _WIN32
    LoadLibraryW(L"windowscodecs.dll");
#endif
    if (!g_stills.empty()) {
        const Still &s = g_stills.begin()->second;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        png_image::decode(s.png.data(), s.png.size(), &rgba, &w, &h);
    }
    pclog::write("stills: %u in memory (%.1f MB), %u of them MP4 first pictures, %u saved now, in %lu ms",
                 (unsigned)g_stills.size(), g_bytes / 1048576.0, (unsigned)pictures, (unsigned)missing.size(),
                 (unsigned long)(GetTickCount() - t0));
}

bool still_cache_find(const char *name, StillFile *out)
{
    const auto it = g_stills.find(lower(name));
    if (it == g_stills.end())
        return false;
    out->data = it->second.png.data();
    out->size = it->second.png.size();
    out->movie = it->second.movie;
    return true;
}

} // namespace gt3media
