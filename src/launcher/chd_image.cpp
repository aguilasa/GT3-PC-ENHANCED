/* chd_image.cpp - a disc image stored as a CHD (chd_image.h), read with
 * libchdr. */
#include "chd_image.h"
#include "util.h"
#include "platform/ui_text.h"

#include <libchdr/chd.h>

#ifdef _WIN32
#include <windows.h>
#else
#include "platform/os/fs.h"
#endif

#include <string.h>

#include <mutex>
#include <vector>

namespace chd_image {
namespace {

constexpr uint32_t kSector = 2048;
constexpr uint32_t kCdFrame = 2448;             /* a CD frame: the 2352-byte sector and its 96 subcode bytes */
/* The unpacked hunks kept, in bytes (4 hunks at the least, 256 at the most:
 * a DVD's are 4096 bytes by chdman's default, a CD's 19,584). */
constexpr uint32_t kKeepBytes = 2u << 20;
/* libchdr's own read-ahead of the packed file (chd_set_cache_budget): one
 * read of the file for many hunks read in a row. */
constexpr size_t kReadAhead = 1u << 20;

#ifdef _WIN32
/* The file as libchdr reads it: a Win32 handle (a path of any length and
 * characters, offsets past 4 GB) and where its next read is. */
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t pos = 0;
};

uint64_t file_size(void *p)
{
    LARGE_INTEGER sz;
    return GetFileSizeEx(((File *)p)->h, &sz) ? (uint64_t)sz.QuadPart : (uint64_t)-1;
}

size_t file_read(void *buf, size_t size, size_t count, void *p)
{
    File *f = (File *)p;
    if (size == 0 || count == 0)
        return 0;
    uint64_t left = (uint64_t)size * count;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (left != 0) {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)f->pos;
        ov.OffsetHigh = (DWORD)(f->pos >> 32);
        DWORD got = 0;
        const DWORD want = left > (64u << 20) ? (64u << 20) : (DWORD)left;
        if (!ReadFile(f->h, out, want, &got, &ov) || got == 0)
            break;
        out += got;
        f->pos += got;
        done += got;
        left -= got;
    }
    return (size_t)(done / size);
}

int file_close(void *p)
{
    File *f = (File *)p;
    if (f->h != INVALID_HANDLE_VALUE)
        CloseHandle(f->h);
    delete f;
    return 0;
}

#else
/* The file as libchdr reads it: an fs::File (positioned reads) and where its
 * next read is. */
struct File {
    fs::File f;
    uint64_t pos = 0;
};

uint64_t file_size(void *p)
{
    const int64_t sz = ((File *)p)->f.size();
    return sz >= 0 ? (uint64_t)sz : (uint64_t)-1;
}

size_t file_read(void *buf, size_t size, size_t count, void *p)
{
    File *f = (File *)p;
    if (size == 0 || count == 0)
        return 0;
    uint64_t left = (uint64_t)size * count;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0;
    while (left != 0) {
        const uint32_t want = left > (64u << 20) ? (64u << 20) : (uint32_t)left;
        const int got = f->f.read_at(f->pos, out, want);
        if (got <= 0)
            break;
        out += got;
        f->pos += (uint64_t)got;
        done += (uint64_t)got;
        left -= (uint64_t)got;
    }
    return (size_t)(done / size);
}

int file_close(void *p)
{
    delete (File *)p;                           /* (fs::File closes itself) */
    return 0;
}
#endif

int file_seek(void *p, int64_t offset, int whence)
{
    File *f = (File *)p;
    int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = (int64_t)f->pos;
    } else if (whence == SEEK_END) {
        const uint64_t size = file_size(p);
        if (size == (uint64_t)-1)
            return -1;
        base = (int64_t)size;
    } else if (whence != SEEK_SET) {
        return -1;
    }
    if (base + offset < 0)
        return -1;
    f->pos = (uint64_t)(base + offset);
    return 0;
}

const core_file_callbacks kFile = {file_size, file_read, file_close, file_seek};

std::string codec_name(uint32_t tag)
{
    if (tag == 0)
        return "none";
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = (char)(tag >> shift);
        s += c >= 0x20 && c < 0x7F ? c : '?';
    }
    return s;
}

std::wstring chd_error_text(chd_error err)
{
    if (err == CHDERR_REQUIRES_PARENT)
        return tr(L"this CHD file needs a parent CHD file, which is not supported");
    return trf(L"the CHD file could not be read (%s)", util::widen(chd_error_string(err)).c_str());
}

} // namespace

bool is_chd(const void *head, size_t size)
{
    return size >= 8 && memcmp(head, "MComprHD", 8) == 0;
}

struct Reader::Impl {
    chd_file *chd = nullptr;
    const chd_header *header = nullptr;
    uint32_t hunkBytes = 0;
    bool cd = false;                            /* frames of kCdFrame bytes */
    uint32_t cdData = 0;                        /* a sector's data in its frame */
    uint32_t cdFirst = 0;                       /* the frame of sector 0 */
    uint64_t size = 0;                          /* the image's bytes */

    struct Kept {
        uint32_t hunk = ~0u;
        uint64_t used = 0;
        std::vector<uint8_t> bytes;
    };
    std::mutex lock;
    std::vector<Kept> kept;
    uint64_t clock = 0;

    /* hunk `n' unpacked (kept); nullptr with *err on a failure */
    const uint8_t *hunk(uint32_t n, chd_error *err)
    {
        Kept *oldest = &kept[0];
        for (Kept &k : kept) {
            if (k.hunk == n) {
                k.used = ++clock;
                return k.bytes.data();
            }
            if (k.used < oldest->used)
                oldest = &k;
        }
        if (oldest->bytes.size() != hunkBytes)
            oldest->bytes.assign(hunkBytes, 0);
        oldest->hunk = ~0u;
        *err = chd_read(chd, n, oldest->bytes.data());
        if (*err != CHDERR_NONE)
            return nullptr;
        oldest->hunk = n;
        oldest->used = ++clock;
        return oldest->bytes.data();
    }

    /* `bytes' of the CHD's own data at `offset' (its hunks end to end) */
    bool raw(uint64_t offset, uint8_t *out, uint32_t bytes, chd_error *err)
    {
        while (bytes != 0) {
            const uint64_t n = offset / hunkBytes;
            const uint32_t at = (uint32_t)(offset % hunkBytes);
            if (n >= header->totalhunks) {
                *err = CHDERR_HUNK_OUT_OF_RANGE;
                return false;
            }
            /* a whole hunk straight into the caller's memory (libchdr wants
             * it 2-byte aligned) */
            if (at == 0 && bytes >= hunkBytes && ((uintptr_t)out & 1) == 0) {
                *err = chd_read(chd, (uint32_t)n, out);
                if (*err != CHDERR_NONE)
                    return false;
                out += hunkBytes;
                offset += hunkBytes;
                bytes -= hunkBytes;
                continue;
            }
            const uint8_t *h = hunk((uint32_t)n, err);
            if (h == nullptr)
                return false;
            const uint32_t take = hunkBytes - at < bytes ? hunkBytes - at : bytes;
            memcpy(out, h + at, take);
            out += take;
            offset += take;
            bytes -= take;
        }
        return true;
    }
};

Reader::~Reader() { close(); }

void Reader::close()
{
    if (impl_ == nullptr)
        return;
    if (impl_->chd != nullptr)
        chd_close(impl_->chd);                  /* (closes the file too: file_close) */
    delete impl_;
    impl_ = nullptr;
}

bool Reader::open(const std::wstring &path, std::wstring *error)
{
    close();
    File *file = new File;
#ifdef _WIN32
    file->h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file->h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = trf(L"the file could not be opened (%s)", util::system_error_text(GetLastError()).c_str());
        delete file;
        return false;
    }
#else
    if (!file->f.open(fs::narrow(path), fs::kRead, fs::Create::open_existing, fs::kShareRead, fs::kRandomAccess)) {
        if (error)
            *error = trf(L"the file could not be opened (%s)", util::system_error_text(fs::last_error()).c_str());
        delete file;
        return false;
    }
#endif
    chd_file *chd = nullptr;
    /* (the file is libchdr's from here: closed by it, opened or not) */
    const chd_error err = chd_open_core_file_callbacks(&kFile, file, CHD_OPEN_READ, nullptr, &chd);
    if (err != CHDERR_NONE || chd == nullptr) {
        if (error)
            *error = chd_error_text(err != CHDERR_NONE ? err : CHDERR_INVALID_FILE);
        return false;
    }
    Impl *im = new Impl;
    im->chd = chd;
    im->header = chd_get_header(chd);
    im->hunkBytes = im->header->hunkbytes;
    impl_ = im;
    const wchar_t *not_disc = N_(L"this CHD file holds no disc image (no ISO 9660 file system)");
    if (im->hunkBytes == 0 || im->header->totalhunks == 0) {
        if (error)
            *error = tr(not_disc);
        close();
        return false;
    }
    chd_set_cache_budget(chd, kReadAhead);      /* (refused: read hunk by hunk) */
    uint32_t keep = kKeepBytes / im->hunkBytes;
    keep = keep < 4 ? 4 : keep > 256 ? 256 : keep;
    im->kept.resize(keep);

    im->cd = im->header->unitbytes == kCdFrame && im->hunkBytes % kCdFrame == 0;
    if (!im->cd) {
        im->size = im->header->logicalbytes;
        return true;
    }
    /* a CD's frames: where sector 16's "CD001" lies says where a sector's
     * data is in its frame (0 a cooked track, 16 a raw mode 1 sector, 24 a
     * raw mode 2 form 1 one) and whether 150 frames of pregap come first */
    const uint64_t frames = im->header->logicalbytes / kCdFrame;
    static const uint32_t kData[] = {0, 16, 24}, kFirst[] = {0, 150};
    for (uint32_t first : kFirst)
        for (uint32_t data : kData) {
            uint8_t d[8];
            chd_error e = CHDERR_NONE;
            if (frames > first + 16 && im->raw((uint64_t)(first + 16) * kCdFrame + data, d, 6, &e) &&
                memcmp(d + 1, "CD001", 5) == 0) {
                im->cdFirst = first;
                im->cdData = data;
                im->size = (frames - first) * kSector;
                return true;
            }
        }
    if (error)
        *error = tr(not_disc);
    close();
    return false;
}

uint64_t Reader::size() const
{
    return impl_ != nullptr ? impl_->size : 0;
}

bool Reader::read(uint64_t offset, void *buf, uint32_t size, std::wstring *error) const
{
    Impl *im = impl_;
    if (im == nullptr || offset > im->size || size > im->size - offset) {
        if (error)
            *error = tr(L"it ends too early (is the image complete?)");
        return false;
    }
    std::lock_guard<std::mutex> hold(im->lock);
    chd_error err = CHDERR_NONE;
    uint8_t *out = (uint8_t *)buf;
    bool ok = true;
    if (!im->cd) {
        ok = im->raw(offset, out, size, &err);
    } else {
        while (size != 0 && ok) {
            const uint64_t sector = offset / kSector;
            const uint32_t at = (uint32_t)(offset % kSector);
            const uint32_t take = kSector - at < size ? kSector - at : size;
            ok = im->raw((sector + im->cdFirst) * kCdFrame + im->cdData + at, out, take, &err);
            out += take;
            offset += take;
            size -= take;
        }
    }
    if (!ok && error)
        *error = chd_error_text(err);
    return ok;
}

std::string Reader::describe() const
{
    if (impl_ == nullptr)
        return std::string();
    const chd_header *h = impl_->header;
    std::string s = "CHD v" + std::to_string(h->version) + ", " + std::to_string(h->hunkbytes) + "-byte hunks (";
    bool any = false;
    for (uint32_t c : h->compression)
        if (c != 0) {
            s += (any ? ", " : "") + codec_name(c);
            any = true;
        }
    s += any ? ")" : "not packed)";
    if (impl_->cd)
        s += ", CD frames, the sectors' data at +" + std::to_string(impl_->cdData);
    return s;
}

} // namespace chd_image
