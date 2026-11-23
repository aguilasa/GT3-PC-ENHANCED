/* png_test.cpp - checks the PNG files off Windows (src/platform/png_image.cpp
 * over LodePNG): a picture written and read back has its pixels, and its
 * pixels' shape (the pHYs chunk, as WIC writes it: x / y pixels per metre
 * for 96 / aspect and 96 dots per inch); opaque and with alpha; a file
 * without pHYs reads as square pixels; too big and broken files are
 * refused; the file is put in place whole (no .part left).  Works in a fresh
 * folder under $TMPDIR (or /tmp), removed at the end; exits 0 when every
 * check holds. */
#include "platform/png_image.h"
#include "platform/os/fs.h"

#include <lodepng.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_failures++;
}

std::vector<uint8_t> picture(int w, int h)
{
    std::vector<uint8_t> p((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *q = &p[((size_t)y * w + x) * 4];
            q[0] = (uint8_t)(x * 7);
            q[1] = (uint8_t)(y * 13);
            q[2] = (uint8_t)(x ^ y);
            q[3] = (uint8_t)(x + y * 3);
        }
    return p;
}

/* The pHYs chunk of a PNG file as it is: x, y, unit; false: none. */
bool phys_of(const std::string &png, unsigned *x, unsigned *y, int *unit)
{
    for (size_t i = 8; i + 12 <= png.size();) {
        const unsigned char *c = (const unsigned char *)png.data() + i;
        const size_t len = (size_t)c[0] << 24 | (size_t)c[1] << 16 | (size_t)c[2] << 8 | c[3];
        if (memcmp(c + 4, "pHYs", 4) == 0 && len == 9) {
            *x = (unsigned)c[8] << 24 | (unsigned)c[9] << 16 | (unsigned)c[10] << 8 | c[11];
            *y = (unsigned)c[12] << 24 | (unsigned)c[13] << 16 | (unsigned)c[14] << 8 | c[15];
            *unit = c[16];
            return true;
        }
        i += 12 + len;
    }
    return false;
}

/* One picture written with `aspect` and read back. */
void round_trip(const std::string &dir, const char *name, int w, int h, double aspect, bool alpha)
{
    const std::vector<uint8_t> src = picture(w, h);
    const std::string path = dir + "/" + name;
    char what[160];
    snprintf(what, sizeof(what), "%s: %dx%d, aspect %.6f, %s: written", name, w, h, aspect, alpha ? "RGBA" : "RGB");
    check(png_image::encode_file(fs::widen(path), src.data(), w, h, aspect, alpha), what);
    check(!fs::exists(path + ".part"), "  no .part left behind");
    std::string file;
    if (!fs::read_file(path, &file)) {
        check(false, "  read back");
        return;
    }
    unsigned px = 0, py = 0;
    int unit = -1;
    const bool phys = phys_of(file, &px, &py, &unit);
    printf("      pHYs: %s x=%u y=%u unit=%d\n", phys ? "present" : "MISSING", px, py, unit);
    check(phys && unit == 1 && py == 3780, "  pHYs in pixels per metre, y = 96 dpi (3780)");
    std::vector<uint8_t> got;
    int gw = 0, gh = 0;
    double ga = 0.0;
    check(png_image::decode(file.data(), file.size(), &got, &gw, &gh, &ga), "  decoded");
    printf("      read back: %dx%d, aspect %.6f\n", gw, gh, ga);
    check(gw == w && gh == h, "  the same size");
    check(fabs(ga - aspect) < 1e-3 * aspect, "  the same aspect ratio");
    bool same = got.size() == src.size();
    for (size_t i = 0; same && i < src.size(); i++)
        same = got[i] == ((i & 3) == 3 && !alpha ? 255 : src[i]);
    check(same, alpha ? "  the same pixels, alpha included" : "  the same pixels, alpha 255 (opaque)");
}

} // namespace

int main()
{
    const char *tmp = getenv("TMPDIR");
    std::string base = std::string(tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp") + "/png_test_XXXXXX";
    std::vector<char> b(base.begin(), base.end());
    b.push_back('\0');
    if (mkdtemp(b.data()) == nullptr) {
        perror("mkdtemp");
        return 2;
    }
    const std::string dir = b.data();

    round_trip(dir, "square.png", 64, 48, 1.0, false);
    round_trip(dir, "wide.png", 640, 448, 4.0 / 3.0 * 448.0 / 640.0, false);   /* a 4:3 PS2 frame */
    round_trip(dir, "anamorphic.png", 33, 17, 16.0 / 9.0, true);
    round_trip(dir, "tall.png", 20, 30, 0.5, true);
    /* over an older file: replaced */
    round_trip(dir, "square.png", 16, 16, 1.25, false);

    /* control: a PNG without pHYs reads as square pixels */
    {
        const std::vector<uint8_t> src = picture(8, 8);
        std::vector<unsigned char> png;
        check(lodepng::encode(png, src, 8, 8) == 0, "control: a PNG without pHYs made by LodePNG itself");
        std::string s((const char *)png.data(), png.size());
        unsigned x, y;
        int u;
        check(!phys_of(s, &x, &y, &u), "  it has no pHYs");
        std::vector<uint8_t> got;
        int w = 0, h = 0;
        double a = 0.0;
        check(png_image::decode(png.data(), png.size(), &got, &w, &h, &a) && a == 1.0, "  aspect 1");
    }
    /* refused */
    {
        std::vector<uint8_t> got;
        int w = 0, h = 0;
        check(!png_image::decode("not a png", 9, &got, &w, &h), "not a PNG: refused");
        std::vector<uint8_t> big((size_t)4 * 4, 0);
        std::vector<unsigned char> png;
        lodepng::encode(png, big, 2, 2);
        /* the header's width made 20000 (past kMaxSide), its CRC made again
         * (a well-formed file, only too big) */
        png[16] = 0, png[17] = 0, png[18] = 0x4E, png[19] = 0x20;
        lodepng_chunk_generate_crc(png.data() + 8);
        unsigned iw = 0, ih = 0;
        lodepng::State st;
        check(lodepng_inspect(&iw, &ih, &st, png.data(), png.size()) == 0 && iw == 20000,
              "  (LodePNG reads its header: 20000 wide)");
        check(!png_image::decode(png.data(), png.size(), &got, &w, &h), "a side past kMaxSide: refused");
        check(!png_image::encode_file(fs::widen(dir + "/no/such/folder.png"), big.data(), 2, 2),
              "a file that cannot be made: false");
        check(!fs::exists(dir + "/no/such/folder.png.part"), "  nothing left behind");
    }
    /* Session: the same calls */
    {
        png_image::Session s;
        const std::vector<uint8_t> src = picture(10, 10);
        const std::string path = dir + "/session.png";
        check(s.encode_file(fs::widen(path), src.data(), 10, 10, 2.0, true), "Session: written");
        std::string file;
        std::vector<uint8_t> got;
        int w = 0, h = 0;
        double a = 0.0;
        check(fs::read_file(path, &file) && s.decode(file.data(), file.size(), &got, &w, &h, &a) && got == src &&
                  fabs(a - 2.0) < 2e-3,
              "Session: read back, aspect 2");
    }

    std::vector<fs::Entry> entries;
    fs::list(dir, &entries);
    for (const fs::Entry &e : entries)
        fs::remove_file(dir + "/" + e.name);
    fs::remove_dir(dir);

    printf("%s\n", g_failures == 0 ? "png_test: all checks passed" : "png_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
