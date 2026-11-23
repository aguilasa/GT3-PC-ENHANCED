/* png_image.h - PNG files through the Windows Imaging Component, for the game
 * and the launcher (docs/movies.md "Stills").  Any thread: each call holds COM
 * for itself.  Elsewhere through LodePNG (cmake/lodepng.cmake), with the same
 * physical size (pHYs) written and read.  No game headers: game code can
 * include it. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace png_image {

/* A picture too big for the game: a side past this (Direct3D's limit), or more
 * bytes than kMaxBytes. */
constexpr int kMaxSide = 16384;
constexpr uint64_t kMaxBytes = 512u << 20;

/* The PNG file `size` bytes at `data` as w x h rows of R, G, B, A bytes, top
 * down.  aspect (if given): the pixels' shape, x / y, from the file's physical
 * size (1 without one).  False when it is no PNG or too big. */
bool decode(const void *data, size_t size, std::vector<uint8_t> *rgba, int *w, int *h, double *aspect = nullptr);

/* w x h rows of R, G, B, A bytes as a PNG at `path`, whose pixels are
 * `aspect` wide for their height: an opaque picture (the alpha not written),
 * or with alpha (PC 2026-11-08: 32-bit RGBA, the image packs' pictures:
 * launcher/image_packs.h).  The file is whole or not there: written beside
 * it, then moved into place, over one that is there.  False (nothing left
 * behind) when it cannot be. */
bool encode_file(const std::wstring &path, const uint8_t *rgba, int w, int h, double aspect = 1.0, bool alpha = false);

/* PC (2026-11-08): the same for many pictures on one thread: COM and the
 * imaging factory taken once (the calls above take them for each picture,
 * some 4 ms; the image packs have thousands of small pictures).  One thread
 * only: a session is used by the thread that made it. */
class Session {
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    bool decode(const void *data, size_t size, std::vector<uint8_t> *rgba, int *w, int *h, double *aspect = nullptr);
    bool encode_file(const std::wstring &path, const uint8_t *rgba, int w, int h, double aspect = 1.0,
                     bool alpha = false);

private:
    bool com_;
    void *wic_;                             /* IWICImagingFactory (no Windows headers here); null elsewhere */
};

} // namespace png_image
