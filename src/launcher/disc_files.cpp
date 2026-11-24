/* disc_files.cpp - the launcher's view of a disc image's data folder (disc_files.h). */
#include "disc_files.h"
#include "inflate.h"
#include "install.h"
#include "iso9660.h"
#include "util.h"
#include "volume.h"

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif

#include <algorithm>
#include <map>
#include <mutex>

namespace disc_files {
namespace {

std::mutex g_mutex;
/* by the data folder's full path, lower case; a null entry: not a disc image's
 * folder (kept: a folder does not change between the two) */
std::map<std::wstring, std::shared_ptr<const Disc>> g_discs;

std::string lower_rel(std::string s)
{
    for (char &c : s) {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    while (!s.empty() && s.back() == '\\')
        s.pop_back();
    return s;
}

std::wstring key_of(const std::wstring &data)
{
    wchar_t full[2048];
    const DWORD n = GetFullPathNameW(data.c_str(), 2048, full, nullptr);
    std::wstring k = n != 0 && n < 2048 ? std::wstring(full, n) : data;
    while (k.size() > 3 && (k.back() == L'\\' || k.back() == L'/'))
        k.pop_back();
    for (wchar_t &c : k)
        c = (wchar_t)towlower(c);
    return k;
}

} // namespace

struct Disc::Impl {
    iso9660::Image image;
    iso9660::Entry volume;
};

Disc::Disc() : impl_(new Impl) {}
Disc::~Disc() = default;

bool Disc::open(const std::wstring &image, const std::string &volumeName, const std::string &language)
{
    image_ = image;
    std::wstring err;
    if (!impl_->image.open(image, &err))
        return false;
    const std::string name = volumeName.empty() ? std::string("GT3.VOL") : volumeName;
    if (!impl_->image.find(name, &impl_->volume, &err) || impl_->volume.is_directory ||
        !impl_->image.inside_image(impl_->volume))
        return false;
    const iso9660::Image &img = impl_->image;
    const iso9660::Entry &vol = impl_->volume;
    std::vector<volume::File> list;
    const volume::Reader reader = [&img, &vol](uint64_t offset, void *buf, uint32_t size) {
        std::wstring e;
        return img.read(vol, offset, buf, size, &e);
    };
    if (!volume::list_files(reader, vol.size, &list, &err))
        return false;
    /* as an install lays them out (the game's disc_image does the same) */
    const bool flat = volume::flat_courses(list);
    const std::string region = volume::course_region(language);
    files_.reserve(list.size());
    for (const volume::File &v : list) {
        File f;
        f.rel = volume::unified_path(v.path, flat, region);
        std::replace(f.rel.begin(), f.rel.end(), '/', '\\');
        f.offset = (uint64_t)v.lsn * volume::kSectorSize;
        f.size = v.size;
        f.stored = v.stored;
        f.packed = v.packed;
        files_.push_back(std::move(f));
    }
    return true;
}

const File *Disc::find(const std::string &rel) const
{
    const std::string want = lower_rel(rel);
    for (const File &f : files_)
        if (f.rel.size() == want.size() && lower_rel(f.rel) == want)
            return &f;
    return nullptr;
}

std::vector<const File *> Disc::folder(const std::string &rel) const
{
    const std::string prefix = lower_rel(rel) + "\\";
    std::vector<const File *> out;
    for (const File &f : files_) {
        if (f.rel.size() <= prefix.size())
            continue;
        const std::string r = lower_rel(f.rel);
        if (r.compare(0, prefix.size(), prefix) == 0 && r.find('\\', prefix.size()) == std::string::npos)
            out.push_back(&f);
    }
    std::sort(out.begin(), out.end(), [](const File *a, const File *b) { return _stricmp(a->rel.c_str(), b->rel.c_str()) < 0; });
    return out;
}

bool Disc::read(const File &f, uint64_t offset, void *buf, uint32_t bytes) const
{
    if (offset > f.size || bytes > f.size - offset)
        return false;
    if (bytes == 0)
        return true;
    if (f.packed) {
        std::vector<uint8_t> all;
        if (!whole(f, &all))
            return false;
        memcpy(buf, all.data() + (size_t)offset, bytes);
        return true;
    }
    std::wstring err;
    return impl_->image.read(impl_->volume, f.offset + offset, buf, bytes, &err);
}

bool Disc::whole(const File &f, std::vector<uint8_t> *out) const
{
    std::wstring err;
    if (!f.packed) {
        out->resize(f.size);
        return f.size == 0 || impl_->image.read(impl_->volume, f.offset, out->data(), f.size, &err);
    }
    std::vector<uint8_t> stored(f.stored);
    if (f.stored != 0 && !impl_->image.read(impl_->volume, f.offset, stored.data(), f.stored, &err))
        return false;
    return inflate::gunzip(stored.data(), stored.size(), out, &err) && out->size() == f.size;
}

std::shared_ptr<const Disc> of(const std::wstring &data)
{
    const std::wstring key = key_of(data);
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_discs.find(key);
    if (it != g_discs.end())
        return it->second;
    std::shared_ptr<const Disc> out;
    const std::string image = install::marker_info(data, "image");
    if (!image.empty()) {
        auto d = std::make_shared<Disc>();
        if (d->open(util::widen(image), install::marker_info(data, "volume"), install::marker_info(data, "language")))
            out = d;
        else
            return nullptr;                 /* (not kept: the image may come back) */
    }
    g_discs[key] = out;
    return out;
}

void release_all()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_discs.clear();
}

} // namespace disc_files
