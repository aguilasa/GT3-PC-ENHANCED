/* iso9660.cpp - read-only access to an ISO 9660 disc image (iso9660.h).
 *
 * ECMA-119 layout used here:
 *   volume descriptors from sector 16, one per sector: +0 type (1 primary,
 *   255 terminator), +1 "CD001", +6 version 1.  Primary descriptor: +40
 *   volume identifier (32 characters), +80 volume space size (both-endian
 *   32-bit), +128 logical block size (both-endian 16-bit), +156 the root
 *   directory record (34 bytes).
 *   directory record: +0 length, +2 extent sector (both-endian 32-bit), +10
 *   data length (both-endian 32-bit), +25 flags (0x02 directory, 0x80 more
 *   extents follow in the next record), +26 interleave unit size, +27
 *   interleave gap, +32 name length, +33 name ("\0" is the directory
 *   itself, "\1" its parent; files end in ";1").  Records never cross a
 *   sector boundary: the rest of a sector is zero-filled.
 * Both-endian fields hold the value twice, little-endian then big-endian.
 * The little-endian half is used; some mastering tools write a wrong
 * big-endian half, and the PS2's own driver reads only the little-endian
 * half too. */
#include "iso9660.h"
#include "chd_image.h"         /* PC (2026-11-23): an image stored as a CHD */
#include "util.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */
#include "platform/os/fs.h"

#include <string.h>

namespace iso9660 {
namespace {

constexpr uint32_t kFirstDescriptor = 16;
constexpr uint32_t kMaxDescriptors = 64;      /* give up on a terminator-less image after this */
constexpr uint32_t kMaxDirectorySize = 16u << 20;

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* "GT3.VOL;1" -> "GT3.VOL"; "README." -> "README" (an empty extension). */
std::string plain_name(const char *s, size_t n)
{
    std::string name(s, n);
    size_t semi = name.find(';');
    if (semi != std::string::npos)
        name.resize(semi);
    if (!name.empty() && name.back() == '.')
        name.pop_back();
    return name;
}

bool same_name(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return false;
    }
    return true;
}

/* Fills `e` from a directory record; false when the record is malformed. */
bool parse_record(const uint8_t *r, size_t avail, Entry *e, uint8_t *flags)
{
    if (avail < 34 || r[0] < 34 || r[0] > avail)
        return false;
    uint8_t name_len = r[32];
    if (33u + name_len > r[0])
        return false;
    Extent x;
    x.lba = le32(r + 2);
    x.size = le32(r + 10);
    *flags = r[25];
    e->name = plain_name(reinterpret_cast<const char *>(r + 33), name_len);
    e->is_directory = (r[25] & 0x02) != 0;
    e->extents.assign(1, x);
    e->size = x.size;
    return true;
}

} // namespace

Image::~Image() { close(); }

void Image::close()
{
    delete file_;
    file_ = nullptr;
    delete chd_;                                /* PC (2026-11-23) */
    chd_ = nullptr;
    image_size_ = 0;
    volume_sectors_ = 0;
    volume_id_.clear();
    root_ = Entry();
}

std::string Image::container() const
{
    return chd_ != nullptr ? chd_->describe() : std::string();
}

bool Image::read_raw(uint64_t offset, void *buf, uint32_t size, std::wstring *error) const
{
    if (chd_ != nullptr)                        /* PC (2026-11-23): a CHD's image */
        return chd_->read(offset, buf, size, error);
    /* A positioned read (fs.h read_at), so several threads may read the same
     * Image. */
    uint8_t *p = static_cast<uint8_t *>(buf);
    while (size) {
        const int got = file_ != nullptr ? file_->read_at(offset, p, size) : -1;
        if (got < 0) {
            if (error)
                *error = util::system_error_text(fs::last_error());
            return false;
        }
        if (got == 0) {
            if (error)
                *error = tr(L"it ends too early (is the image complete?)");
            return false;
        }
        p += got;
        offset += got;
        size -= got;
    }
    return true;
}

bool Image::open(const std::wstring &path, std::wstring *error)
{
    close();
    fs::File *f = new fs::File();
    if (!f->open(fs::narrow(path), fs::kRead, fs::Create::open_existing, fs::kShareRead, fs::kSequential)) {
        if (error)
            *error = trf(L"the file could not be opened (%s)", util::system_error_text(fs::last_error()).c_str());
        delete f;
        return false;
    }
    file_ = f;
    path_ = path;
    const int64_t sz = f->size();
    if (sz < 0) {
        if (error)
            *error = trf(L"the file could not be read (%s)", util::system_error_text(fs::last_error()).c_str());
        close();
        return false;
    }
    image_size_ = (uint64_t)sz;

    /* PC (2026-11-23): a CHD (its first bytes say so, whatever its name):
     * read through chd_image from here, as the image it holds */
    {
        uint8_t head[8] = {};
        std::wstring ignored;
        if (image_size_ >= sizeof(head) && read_raw(0, head, sizeof(head), &ignored) &&
            chd_image::is_chd(head, sizeof(head))) {
            delete file_;
            file_ = nullptr;
            chd_ = new chd_image::Reader;
            if (!chd_->open(path, error)) {
                close();
                return false;
            }
            image_size_ = chd_->size();
        }
    }

    const wchar_t *not_iso = N_(L"this file is not a disc image (no ISO 9660 file system)");
    if (image_size_ < (uint64_t)(kFirstDescriptor + 1) * kSectorSize) {
        if (error)
            *error = tr(not_iso);
        close();
        return false;
    }

    /* The primary volume descriptor: the first type-1 descriptor. */
    uint8_t d[kSectorSize];
    bool found = false;
    for (uint32_t s = kFirstDescriptor; s < kFirstDescriptor + kMaxDescriptors; s++) {
        if ((uint64_t)(s + 1) * kSectorSize > image_size_)
            break;
        if (!read_raw((uint64_t)s * kSectorSize, d, kSectorSize, error)) {
            close();
            return false;
        }
        if (memcmp(d + 1, "CD001", 5) != 0)
            break;
        if (d[0] == 255)
            break;
        if (d[0] == 1) {
            found = true;
            break;
        }
    }
    if (!found) {
        if (error)
            *error = tr(not_iso);
        close();
        return false;
    }
    if (le16(d + 128) != kSectorSize) {
        if (error)
            *error = tr(L"this disc image uses an unsupported sector size (not a DVD image)");
        close();
        return false;
    }
    volume_sectors_ = le32(d + 80);
    volume_id_.assign(reinterpret_cast<const char *>(d + 40), 32);
    while (!volume_id_.empty() && (volume_id_.back() == ' ' || volume_id_.back() == '\0'))
        volume_id_.pop_back();

    uint8_t flags = 0;
    if (!parse_record(d + 156, 34, &root_, &flags) || !root_.is_directory) {
        if (error)
            *error = tr(L"the disc image's root directory is damaged");
        close();
        return false;
    }
    root_.name.clear();
    return true;
}

bool Image::read(const Entry &file, uint64_t offset, void *buf, uint32_t size, std::wstring *error) const
{
    if (offset > file.size || size > file.size - offset) {
        if (error)
            *error = trf(L"read past the end of %s", util::widen(file.name).c_str());
        return false;
    }
    uint8_t *p = static_cast<uint8_t *>(buf);
    for (const Extent &x : file.extents) {
        if (size == 0)
            break;
        if (offset >= x.size) {
            offset -= x.size;
            continue;
        }
        uint32_t n = (uint32_t)(x.size - offset) < size ? (uint32_t)(x.size - offset) : size;
        if (!read_raw((uint64_t)x.lba * kSectorSize + offset, p, n, error))
            return false;
        p += n;
        size -= n;
        offset = 0;
    }
    return true;
}

bool Image::read_all(const Entry &file, std::string *out, std::wstring *error) const
{
    if (file.size > (64u << 20)) {
        if (error)
            *error = trf(L"%s is too large to read at once", util::widen(file.name).c_str());
        return false;
    }
    out->assign((size_t)file.size, '\0');
    return file.size == 0 || read(file, 0, &(*out)[0], (uint32_t)file.size, error);
}

bool Image::inside_image(const Entry &file) const
{
    for (const Extent &x : file.extents)
        if ((uint64_t)x.lba * kSectorSize + x.size > image_size_)
            return false;
    return true;
}

bool Image::read_dir(const Entry &dir, std::vector<Entry> *out, std::wstring *error) const
{
    out->clear();
    if (!dir.is_directory || dir.size > kMaxDirectorySize) {
        if (error)
            *error = tr(L"the disc image's directories are damaged");
        return false;
    }
    std::string data;
    if (!read_all(dir, &data, error))
        return false;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(data.data());
    size_t pos = 0, end = data.size();
    bool more_extents = false;   /* the previous record had flag 0x80 */
    while (pos < end) {
        size_t sector_end = (pos / kSectorSize + 1) * kSectorSize;
        if (sector_end > end)
            sector_end = end;
        if (b[pos] == 0) {
            /* Zero padding up to the next sector. */
            pos = sector_end;
            continue;
        }
        Entry e;
        uint8_t flags = 0;
        if (!parse_record(b + pos, sector_end - pos, &e, &flags)) {
            if (error)
                *error = tr(L"the disc image's directories are damaged");
            return false;
        }
        uint8_t name_len = b[pos + 32];
        bool self_or_parent = name_len == 1 && (b[pos + 33] == 0 || b[pos + 33] == 1);
        if (b[pos + 26] || b[pos + 27]) {
            /* Interleaved files (CD-XA audio / video) do not occur on PS2 DVDs. */
            if (error)
                *error = trf(L"the disc image has an interleaved file (%s), which is not supported",
                             util::widen(e.name).c_str());
            return false;
        }
        if (!self_or_parent) {
            if (more_extents && !out->empty() && same_name(out->back().name, e.name)) {
                out->back().extents.push_back(e.extents[0]);
                out->back().size += e.size;
            } else {
                out->push_back(e);
            }
        }
        more_extents = (flags & 0x80) != 0;
        pos += b[pos];
    }
    return true;
}

bool Image::list(const Entry &dir, std::vector<Entry> *out, std::wstring *error) const
{
    return read_dir(dir, out, error);
}

bool Image::find(const std::string &path, Entry *out, std::wstring *error) const
{
    Entry cur = root_;
    size_t pos = 0;
    for (;;) {
        while (pos < path.size() && (path[pos] == '/' || path[pos] == '\\'))
            pos++;
        if (pos >= path.size())
            break;
        size_t next = path.find_first_of("/\\", pos);
        if (next == std::string::npos)
            next = path.size();
        std::string want = plain_name(path.data() + pos, next - pos);
        pos = next;

        if (!cur.is_directory) {
            if (error)
                *error = util::widen(path) + L" was not found on the disc";
            return false;
        }
        std::vector<Entry> entries;
        if (!read_dir(cur, &entries, error))
            return false;
        bool found = false;
        for (Entry &e : entries)
            if (same_name(e.name, want)) {
                cur = std::move(e);
                found = true;
                break;
            }
        if (!found) {
            if (error)
                *error = util::widen(path) + L" was not found on the disc";
            return false;
        }
    }
    *out = cur;
    return true;
}

} // namespace iso9660
