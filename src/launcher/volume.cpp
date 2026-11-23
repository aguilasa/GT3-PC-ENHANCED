/* volume.cpp - the files of Gran Turismo 3's GT3.VOL (volume.h). */
#include "volume.h"

#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#include <string.h>
#ifndef _WIN32
#include "platform/os/win_compat.h"         /* _stricmp */
#endif

namespace volume {
namespace {

struct Toc {
    std::vector<uint8_t> data;

    bool word(uint32_t off, uint32_t *v) const
    {
        if ((uint64_t)off + 4 > data.size())
            return false;
        memcpy(v, &data[off], 4);
        return true;
    }
    bool name(uint32_t rec, std::string *out) const
    {
        uint32_t w;
        if (!word(rec, &w))
            return false;
        uint32_t off = w & 0xFFFFFF;
        if (off >= data.size())
            return false;
        const void *end = memchr(&data[off], 0, data.size() - off);
        if (!end)
            return false;
        out->assign((const char *)&data[off], (const char *)end);
        return true;
    }
};

bool walk(const Toc &toc, uint32_t rec, const std::string &prefix, int depth, std::vector<File> *files)
{
    if (depth > 32)
        return false;
    uint32_t count;
    if (!toc.word(rec + 4, &count) || count == 0)
        return false;
    for (uint32_t i = 0; i + 1 < count; i++) {
        uint32_t ent;
        std::string name;
        if (!toc.word(rec + 0xC + 4 * i, &ent) || (uint64_t)ent + 0x10 > toc.data.size() || !toc.name(ent, &name) ||
            name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name == "." || name == "..")
            return false;
        uint8_t type = toc.data[ent + 3];
        if (type == 1) {
            if (!walk(toc, ent, prefix + name + "/", depth + 1, files))
                return false;
            continue;
        }
        File f;
        f.path = prefix + name;
        f.packed = type == 2;
        uint32_t packed = 0;
        if (!toc.word(ent + 4, &f.lsn) || !toc.word(ent + 8, &f.size) || !toc.word(ent + 0xC, &packed))
            return false;
        f.stored = f.packed ? packed : f.size;
        files->push_back(f);
    }
    return true;
}

} // namespace

bool list_files(const Reader &read, uint64_t volume_size, std::vector<File> *files, std::wstring *error)
{
    files->clear();
    uint8_t hdr[0x40];
    if (volume_size < sizeof(hdr) || !read(0, hdr, sizeof(hdr))) {
        *error = tr(L"the volume could not be read");
        return false;
    }
    static const uint8_t magic[4] = {'R', 'o', 'F', 'S'};
    static const uint8_t inverted[4] = {0xAD, 0x90, 0xB9, 0xAC};
    const bool inv = memcmp(hdr, inverted, 4) == 0;
    if (!inv && memcmp(hdr, magic, 4) != 0) {
        *error = tr(L"the volume has no RoFS table of contents");
        return false;
    }
    uint32_t headerSize, bodySize;
    memcpy(&headerSize, hdr + 12, 4);
    memcpy(&bodySize, hdr + 16, 4);
    const uint64_t tocSize = (uint64_t)headerSize + bodySize;
    if (headerSize < 0x20 || tocSize > volume_size || tocSize > (64u << 20)) {
        *error = tr(L"the volume's table of contents does not fit the volume");
        return false;
    }
    Toc toc;
    toc.data.resize((size_t)tocSize);
    if (!read(0, toc.data.data(), (uint32_t)tocSize)) {
        *error = tr(L"the volume's table of contents could not be read");
        return false;
    }
    if (inv)
        for (uint64_t i = headerSize; i < tocSize; i++)
            toc.data[(size_t)i] ^= 0xFF;
    if (!walk(toc, 0x14, std::string(), 0, files)) {
        *error = tr(L"the volume's table of contents is damaged");
        return false;
    }
    for (const File &f : *files)
        if ((uint64_t)f.lsn * kSectorSize + f.stored > volume_size) {
            *error = tr(L"a file of the volume lies past its end");
            return false;
        }
    return true;
}

std::string course_region(const std::string &language)
{
    if (_stricmp(language.c_str(), "JP") == 0)
        return "JP";
    if (_stricmp(language.c_str(), "US") == 0)
        return "US";
    if (_stricmp(language.c_str(), "BR") == 0)
        return "UK";
    return "EU";
}

bool flat_courses(const std::vector<File> &files)
{
    bool flatFiles = false, courseFolders = false;
    for (const File &f : files)
        if (f.path.size() > 8 && _strnicmp(f.path.c_str(), "courses/", 8) == 0) {
            if (f.path.find('/', 8) != std::string::npos)
                courseFolders = true;
            else if (_stricmp(f.path.c_str() + 8, ".list") != 0)
                flatFiles = true;
        }
    return flatFiles && !courseFolders;
}

std::string unified_path(const std::string &path, bool flatCourses, const std::string &region)
{
    if (flatCourses && path.size() > 8 && _strnicmp(path.c_str(), "courses/", 8) == 0 &&
        path.find('/', 8) == std::string::npos && _stricmp(path.c_str() + 8, ".list") != 0)
        return path.substr(0, 8) + region + "/" + path.substr(8);
    return path;
}

} // namespace volume
