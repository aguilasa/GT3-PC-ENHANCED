/* disc_image.cpp - the game files read straight from a disc image
 * (disc_image.h).  The image and its volume are read with the launcher's own
 * readers (src/launcher/iso9660.*, volume.*, inflate.*: what the installer
 * unpacks them with), built into the game too. */
#include "platform/disc_image.h"
#include "platform/log.h"

#include "launcher/inflate.h"
#include "launcher/iso9660.h"
#include "launcher/volume.h"
#include "platform/os/fs.h"

#include <ctype.h>

#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>

namespace disc_image {
namespace {

/* The unpacked files kept (the newest first): a file opened again soon (a
 * probe, then the read; a menu shown again) is not inflated again.  Small:
 * the game is a 32-bit program, and what it loads it keeps itself.  The
 * largest packed file of the discs is 13.6 MB unpacked (a course). */
constexpr size_t kKeepBytes = 16u << 20;

iso9660::Image g_image;
iso9660::Entry g_volume;
std::string g_path;
bool g_mounted = false;
std::vector<File> g_files;
std::unordered_map<std::string, size_t> g_byName;      /* the lower-case rel -> g_files */

struct Kept {
    const File *file;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};
std::mutex g_keptMutex;
std::list<Kept> g_kept;
size_t g_keptBytes = 0;

/* (the marker's UTF-8; else typed by hand in the ANSI code page) */
std::wstring widen_utf8(const std::string &s)
{
    return fs::widen(fs::utf8_or_native(s));
}

std::string narrow_utf8(const std::wstring &w)
{
    return fs::narrow(w);
}

} // namespace

bool mount(const std::string &image, const std::string &volume, const std::string &language, std::string *error)
{
    std::wstring err;
    g_mounted = false;
    g_files.clear();
    g_byName.clear();
    g_path = image;
    if (!g_image.open(widen_utf8(image), &err)) {
        *error = narrow_utf8(err);
        return false;
    }
    const std::string name = volume.empty() ? std::string("GT3.VOL") : volume;
    if (!g_image.find(name, &g_volume, &err) || g_volume.is_directory) {
        *error = "it has no " + name + " (another disc image is at this path now?)";
        g_image.close();
        return false;
    }
    if (!g_image.inside_image(g_volume)) {
        *error = "it ends before " + name + " does (the image is incomplete)";
        g_image.close();
        return false;
    }
    std::vector<volume::File> files;
    const volume::Reader reader = [](uint64_t offset, void *buf, uint32_t size) {
        std::wstring e;
        return g_image.read(g_volume, offset, buf, size, &e);
    };
    if (!volume::list_files(reader, g_volume.size, &files, &err)) {
        *error = name + ": " + narrow_utf8(err);
        g_image.close();
        return false;
    }
    /* as an install lays them out: the unified structure */
    const bool flat = volume::flat_courses(files);
    const std::string region = volume::course_region(language);
    g_files.reserve(files.size());
    uint32_t packed = 0;
    for (const volume::File &v : files) {
        File f;
        f.rel = volume::unified_path(v.path, flat, region);
        std::replace(f.rel.begin(), f.rel.end(), '/', '\\');
        f.offset = (uint64_t)v.lsn * volume::kSectorSize;
        f.size = v.size;
        f.stored = v.stored;
        f.packed = v.packed;
        packed += v.packed ? 1 : 0;
        std::string key = f.rel;
        for (char &c : key)
            c = (char)tolower((unsigned char)c);
        if (g_byName.emplace(std::move(key), g_files.size()).second)
            g_files.push_back(std::move(f));
    }
    g_mounted = true;
    pclog::write("disc image: %s", image.c_str());
    if (!g_image.container().empty())           /* PC (2026-11-23): a CHD's (launcher/chd_image.h) */
        pclog::write("disc image: a %s", g_image.container().c_str());
    pclog::write("disc image: %s, %llu bytes at sector %u: %u files, %u of them packed%s", name.c_str(),
                 (unsigned long long)g_volume.size, g_volume.extents.empty() ? 0u : g_volume.extents[0].lba,
                 (unsigned)g_files.size(), packed, flat ? (", its courses in courses\\" + region).c_str() : "");
    return true;
}

bool mounted()
{
    return g_mounted;
}

const std::string &image_path()
{
    return g_path;
}

const File *find(const std::string &rel)
{
    if (!g_mounted)
        return nullptr;
    const auto it = g_byName.find(rel);
    return it != g_byName.end() ? &g_files[it->second] : nullptr;
}

const std::vector<File> &files()
{
    return g_files;
}

bool read(const File &f, uint64_t offset, void *buf, uint32_t bytes)
{
    if (!g_mounted || offset > f.stored || bytes > f.stored - offset)
        return false;
    if (bytes == 0)
        return true;
    std::wstring err;
    if (!g_image.read(g_volume, f.offset + offset, buf, bytes, &err)) {
        pclog::write("disc image: %s: read of %u bytes at %llu failed (%s)", f.rel.c_str(), bytes,
                     (unsigned long long)offset, narrow_utf8(err).c_str());
        return false;
    }
    return true;
}

std::shared_ptr<const std::vector<uint8_t>> unpacked(const File &f)
{
    {
        std::lock_guard<std::mutex> lock(g_keptMutex);
        for (auto it = g_kept.begin(); it != g_kept.end(); ++it)
            if (it->file == &f) {
                g_kept.splice(g_kept.begin(), g_kept, it);
                return g_kept.front().bytes;
            }
    }
    std::vector<uint8_t> stored(f.stored);
    auto out = std::make_shared<std::vector<uint8_t>>();
    std::wstring err;
    if (!read(f, 0, stored.data(), f.stored))
        return nullptr;
    if (!inflate::gunzip(stored.data(), stored.size(), out.get(), &err) || out->size() != f.size) {
        pclog::write("disc image: %s could not be unpacked (%s): the image is damaged", f.rel.c_str(),
                     err.empty() ? "wrong size" : narrow_utf8(err).c_str());
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_keptMutex);
    g_kept.push_front(Kept{&f, out});
    g_keptBytes += out->size();
    while (g_kept.size() > 1 && g_keptBytes > kKeepBytes) {
        g_keptBytes -= g_kept.back().bytes->size();
        g_kept.pop_back();
    }
    return out;
}

} // namespace disc_image
