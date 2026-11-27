/* chd_image.h - PC (2026-11-23, user: "CHD support for direct disc loading"):
 * a disc image stored as a CHD (MAME's "compressed hunks of data", what
 * chdman writes and what PCSX2 plays), read as the plain image it holds.
 *
 * iso9660::Image opens one where it opens an .iso (it looks at the file's
 * first bytes, not its name), so everything that reads a disc image reads a
 * CHD the same way: the installer, the Play tab's Disc sub-tab (the game
 * then reads the volume's files out of the CHD in place), the song manager,
 * the image packs.
 *
 * The reading is libchdr's (third_party, cmake/chd.cmake): CHD versions 1 to
 * 5, the hunks packed with zlib, LZMA, zstd, Huffman or FLAC and their CD
 * forms.  Two kinds of file hold a PlayStation 2 disc:
 *   a DVD's (chdman createdvd; also createhd / createraw: any CHD whose
 *     units are not CD frames) - the image's bytes as they are;
 *   a CD's (chdman createcd, which older guides used for DVD images too) -
 *     frames of 2448 bytes, a sector's 2048 data bytes at the frame's start
 *     (a cooked track) or after a raw sector's 16 or 24 byte header: found
 *     by where the ISO 9660 descriptor of sector 16 lies.
 * A CHD that needs a parent CHD is refused.
 *
 * Hunks are unpacked as they are read and a few megabytes of them kept, so a
 * file read front to back unpacks each hunk once.  Any thread may read (one
 * at a time). */
#pragma once

#include <cstdint>
#include <string>

namespace chd_image {

/* The first bytes of a file are a CHD's ("MComprHD"). */
bool is_chd(const void *head, size_t size);

class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;

    /* Opens `path` read-only; false with a message in `error' (the file is
     * not a CHD libchdr reads, needs a parent, or holds no disc image this
     * finds 2048-byte sectors in). */
    bool open(const std::wstring &path, std::wstring *error);
    void close();
    bool is_open() const { return impl_ != nullptr; }

    /* The bytes of the disc image it holds (its sectors x 2048). */
    uint64_t size() const;
    /* `size' bytes of the image at `offset'. */
    bool read(uint64_t offset, void *buf, uint32_t size, std::wstring *error) const;
    /* For the log: "CHD v5, 4096-byte hunks (zstd, zlib, huff, flac)". */
    std::string describe() const;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace chd_image
