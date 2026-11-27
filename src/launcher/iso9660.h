/* iso9660.h - read-only access to an ISO 9660 disc image.
 *
 * PlayStation 2 DVDs carry an ISO 9660 file system (with a UDF bridge that
 * describes the same files; the PS2's own CD/DVD driver reads only the ISO
 * 9660 side, and so does this reader).  The reader finds the primary volume
 * descriptor at sector 16, walks directories from the root and reads file
 * data; files may be larger than 2 GB (Gran Turismo 3's GT3.VOL is
 * 3,876,222,976 bytes in one extent), and multi-extent files are joined.
 *
 * The image is opened for reading only and is never written.  Nothing here
 * knows about Gran Turismo: install.cpp checks the disc, and a later
 * loose-file installer can walk the tree with list() and read().
 *
 * PC (2026-11-23, user: "CHD support for direct disc loading"): the image
 * file may be a CHD (chd_image.h) - told by its first bytes, not its name -
 * and is then read through it as the image it holds; everything below is the
 * same for both. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs {
class File;
}

namespace chd_image {
class Reader;
}

namespace iso9660 {

constexpr uint32_t kSectorSize = 2048;  /* logical block size of a DVD image */

/* A run of file data: `size` bytes from sector `lba` of the image. */
struct Extent {
    uint32_t lba = 0;
    uint32_t size = 0;
};

/* A file or directory found in the image. */
struct Entry {
    std::string name;            /* as recorded, without the ";1" version (e.g. "GT3.VOL") */
    bool is_directory = false;
    uint64_t size = 0;           /* bytes: the sum of the extents */
    std::vector<Extent> extents; /* one, or several for a multi-extent file */
};

class Image {
public:
    Image() = default;
    ~Image();
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    /* Opens `path` read-only and reads the primary volume descriptor; false
     * (with a message in `error`) when the file is not an ISO 9660 image. */
    bool open(const std::wstring &path, std::wstring *error);
    void close();
    bool is_open() const { return file_ != nullptr || chd_ != nullptr; }
    /* PC (2026-11-23): what the file is when it is not a plain image - a
     * CHD's "CHD v5, 4096-byte hunks (zstd, zlib, huff, flac)" - else "". */
    std::string container() const;

    uint64_t image_size() const { return image_size_; }        /* bytes of the image (a CHD: of the image it holds) */
    uint32_t volume_sectors() const { return volume_sectors_; } /* volume space size (PVD) */
    const std::string &volume_id() const { return volume_id_; } /* e.g. "GRANTURISMO3" */
    const Entry &root() const { return root_; }

    /* Looks up "SYSTEM.CNF", "/DIR/FILE.EXT" or "dir\\file.ext": ASCII case
     * is ignored, a ";1" version is optional.  False when not found (then
     * `error` says why: no such file, or a damaged directory). */
    bool find(const std::string &path, Entry *out, std::wstring *error) const;

    /* The entries of a directory (without "." and ".."). */
    bool list(const Entry &dir, std::vector<Entry> *out, std::wstring *error) const;

    /* Reads `size` bytes at `offset` of a file's data.  False past the end
     * of the file or on a read error (a short image file is a read error). */
    bool read(const Entry &file, uint64_t offset, void *buf, uint32_t size, std::wstring *error) const;

    /* A whole small file (SYSTEM.CNF and the like) into `out`. */
    bool read_all(const Entry &file, std::string *out, std::wstring *error) const;

    /* True when every extent of `file` lies inside the image file: a
     * truncated image fails this before a long copy finds out. */
    bool inside_image(const Entry &file) const;

    /* Raw bytes of the image at a byte offset. */
    bool read_raw(uint64_t offset, void *buf, uint32_t size, std::wstring *error) const;

private:
    bool read_dir(const Entry &dir, std::vector<Entry> *out, std::wstring *error) const;

    fs::File *file_ = nullptr;  /* (platform/os/fs.h; a plain image) */
    chd_image::Reader *chd_ = nullptr; /* PC (2026-11-23): the CHD it is read through instead */
    std::wstring path_;
    uint64_t image_size_ = 0;
    uint32_t volume_sectors_ = 0;
    std::string volume_id_;
    Entry root_;
};

} // namespace iso9660
