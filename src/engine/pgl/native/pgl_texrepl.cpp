/* pgl_texrepl.cpp - PC (2026-11-13, no original): external texture
 * replacement (pgl_texrepl.h; docs/renderer.md "Texture replacement").
 *
 * Nothing here is freed at exit: the workers may still run while the process
 * ends (the state is on the heap, the locks have no destructors). */
#include "engine/pgl/native/pgl_texrepl.h"
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/pgl/native/pgl_win32.h"
#include <dxgi.h>
#ifdef _WIN32
#include <wincodec.h>
#else
/* off Windows: the files through platform/os/fs.h, a .png by png_image (the
 * game's PNG code), a .jpg by stb_image */
#include "platform/os/fs.h"
#include "platform/png_image.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"
#endif

#include "platform/log.h"
#include "platform/settings.h"

#ifdef _WIN32
#pragma comment(lib, "windowscodecs.lib")
#endif

namespace pglr {
namespace texrepl {

/* ======================================================================== */
/* The names                                                                */
/* ======================================================================== */

namespace {

inline uint32_t rotl32(uint32_t v, int s)
{
    return (v << s) | (v >> (32 - s));
}

inline uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

}  /* namespace */

/* FROZEN (the files' names): four lanes of 32-bit state, a texel a lane in
 * turn (lane += texel x P2, rotated left 13, x P1 - xxHash32's round), the
 * lanes started from the size; then two 32-bit words out of the four lanes,
 * mixed differently, each finished with MurmurHash3's 32-bit finalizer.  All
 * arithmetic modulo 2^32; the texels as 32-bit words R | G << 8 | B << 16 |
 * A << 24. */
uint64_t content_hash(const uint32_t *px, uint32_t w, uint32_t h)
{
    const uint32_t P1 = 0x9E3779B1u, P2 = 0x85EBCA77u, P3 = 0xC2B2AE3Du, P4 = 0x27D4EB2Fu, P5 = 0x165667B1u;
    uint32_t a = P1 ^ w, b = P2 ^ h, c = P3 + w * h, d = P4;
    const size_t n = (size_t)w * h;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a = rotl32(a + px[i] * P2, 13) * P1;
        b = rotl32(b + px[i + 1] * P2, 13) * P1;
        c = rotl32(c + px[i + 2] * P2, 13) * P1;
        d = rotl32(d + px[i + 3] * P2, 13) * P1;
    }
    for (; i < n; i++) {
        a = rotl32(a + px[i] * P2, 13) * P1;
        const uint32_t t = a;
        a = b;
        b = c;
        c = d;
        d = t;
    }
    const uint32_t lo = fmix32(rotl32(a, 1) + rotl32(b, 7) + rotl32(c, 12) + rotl32(d, 18) + (uint32_t)n);
    const uint32_t hi = fmix32((a ^ rotl32(b, 11)) * P3 + (c ^ rotl32(d, 19)) * P4 + (uint32_t)n * P5);
    return (uint64_t)hi << 32 | lo;
}

void hash_name(uint64_t hash, char out[17])
{
    snprintf(out, 17, "%08X%08X", (unsigned)(hash >> 32), (unsigned)hash);
}

namespace {

/* The 16 digits a file's name starts with. */
bool name_hash(const wchar_t *name, uint64_t *hash)
{
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        const wchar_t c = name[i];
        uint32_t d;
        if (c >= L'0' && c <= L'9')
            d = (uint32_t)(c - L'0');
        else if (c >= L'a' && c <= L'f')
            d = (uint32_t)(c - L'a') + 10;
        else if (c >= L'A' && c <= L'F')
            d = (uint32_t)(c - L'A') + 10;
        else
            return false;
        v = v << 4 | d;
    }
    *hash = v;                              /* (anything may follow the 16) */
    return true;
}

/* ======================================================================== */
/* Files                                                                    */
/* ======================================================================== */

#ifdef _WIN32
std::wstring widen(const char *ansi)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi, -1, &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

void make_folders(const std::wstring &path)
{
    for (size_t i = 3; i < path.size(); i++)
        if (path[i] == L'\\')
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

bool read_file(const std::wstring &path, std::vector<uint8_t> &out, size_t most = (size_t)-1)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (512ll << 20);
    if (ok) {
        const size_t n = std::min<size_t>((size_t)size.QuadPart, most);
        out.resize(n);
        size_t at = 0;
        while (ok && at < n) {
            DWORD got = 0;
            ok = ReadFile(f, out.data() + at, (DWORD)std::min<size_t>(n - at, 16u << 20), &got, nullptr) && got != 0;
            at += got;
        }
    }
    CloseHandle(f);
    return ok;
}

/* Whole or not there: written beside the place, then moved onto it. */
bool write_file(const std::wstring &path, const void *head, size_t headBytes, const void *data, size_t bytes)
{
    wchar_t tmp[32];
    swprintf(tmp, 32, L".%lu.tmp", GetCurrentThreadId());
    const std::wstring part = path + tmp;
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    const void *parts[2] = {head, data};
    const size_t sizes[2] = {headBytes, bytes};
    for (int p = 0; p < 2 && ok; p++) {
        size_t at = 0;
        while (ok && at < sizes[p]) {
            DWORD put = 0;
            ok = WriteFile(f, (const uint8_t *)parts[p] + at, (DWORD)std::min<size_t>(sizes[p] - at, 16u << 20), &put,
                           nullptr) && put != 0;
            at += put;
        }
    }
    CloseHandle(f);
    if (ok)
        ok = MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok)
        DeleteFileW(part.c_str());
    return ok;
}

#else /* !_WIN32 */

std::wstring widen(const char *ansi)
{
    return fs::widen(ansi);                 /* (the native text is UTF-8) */
}

std::string narrow(const std::wstring &w)
{
    return fs::narrow(w);
}

/* (the folders of a path ending in '\' or '/') */
void make_folders(const std::wstring &path)
{
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == L'\\' || path[i] == L'/')
            fs::make_dir(fs::narrow(path.substr(0, i)));
}

bool read_file(const std::wstring &path, std::vector<uint8_t> &out, size_t most = (size_t)-1)
{
    fs::File f;
    if (!f.open(fs::narrow(path), fs::kRead, fs::Create::open_existing))
        return false;
    const int64_t size = f.size();
    bool ok = size >= 0 && size < (512ll << 20);
    if (ok) {
        const size_t n = std::min<size_t>((size_t)size, most);
        out.resize(n);
        size_t at = 0;
        while (ok && at < n) {
            const int got = f.read_at(at, out.data() + at, (uint32_t)std::min<size_t>(n - at, 16u << 20));
            ok = got > 0;
            if (ok)
                at += (size_t)got;
        }
    }
    return ok;
}

/* Whole or not there: written beside the place, then moved onto it. */
bool write_file(const std::wstring &path, const void *head, size_t headBytes, const void *data, size_t bytes)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), ".%lu.tmp", (unsigned long)GetCurrentThreadId());
    const std::string to = fs::narrow(path), part = to + tmp;
    bool ok;
    {
        fs::File f;
        if (!f.open(part, fs::kWrite, fs::Create::create_always))
            return false;
        ok = true;
        const void *parts[2] = {head, data};
        const size_t sizes[2] = {headBytes, bytes};
        for (int p = 0; p < 2 && ok; p++) {
            size_t at = 0;
            while (ok && at < sizes[p]) {
                const int put = f.write((const uint8_t *)parts[p] + at, (uint32_t)std::min<size_t>(sizes[p] - at, 16u << 20));
                ok = put > 0;
                if (ok)
                    at += (size_t)put;
            }
        }
    }
    if (ok)
        ok = fs::rename_replace(part, to);
    if (!ok)
        fs::remove_file(part);
    return ok;
}

#endif /* _WIN32 */

double now_seconds()
{
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

/* The bytes the workers hold for the pictures they are making ready: the
 * game is a 32-bit program, and three big pictures decoded at once (64 MB
 * each at 4096 x 4096, as much again for their levels) would take the
 * address space the game needs (platform/memstat.h).  A worker waits with a
 * new picture while the others hold more than kFlight (one always goes). */
constexpr size_t kFlight = 192u << 20;
std::atomic<size_t> g_flight{0};
thread_local size_t t_flight = 0;

void flight_take(size_t bytes)
{
    for (int i = 0; i < 30000; i++) {
        const size_t now = g_flight.load();
        if (now - t_flight == 0 || now + bytes <= kFlight)
            break;
        Sleep(2);
    }
    g_flight += bytes;
    t_flight += bytes;
}

void flight_done()
{
    g_flight -= t_flight;
    t_flight = 0;
}

/* ======================================================================== */
/* Pictures                                                                 */
/* ======================================================================== */

/* R, G, B, A bytes, the usual alpha. */
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
};

constexpr uint32_t kMaxSide = 8192;

#ifdef _WIN32
/* (a worker's: COM is its own) */
IWICImagingFactory *wic()
{
    static thread_local IWICImagingFactory *f = nullptr;
    if (f == nullptr) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    }
    return f;
}

/* A .png / .jpg file (any format WIC reads). */
bool wic_decode(const std::wstring &path, Image &out)
{
    IWICImagingFactory *f = wic();
    if (f == nullptr)
        return false;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h)) && w != 0 && h != 0 && w <= kMaxSide && h <= kMaxSide;
    if (ok) {
        flight_take((size_t)w * h * 10);    /* (its pixels, its levels twice over, its loaded form) */
        out.w = w;
        out.h = h;
        out.px.resize((size_t)w * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()));
    }
    if (conv != nullptr)
        conv->Release();
    if (frame != nullptr)
        frame->Release();
    if (dec != nullptr)
        dec->Release();
    return ok;
}

/* w x h texels (the GS's alpha) as a PNG with the usual alpha: min(2 a,
 * 255). */
bool png_write(const std::wstring &path, const uint32_t *px, uint32_t w, uint32_t h)
{
    IWICImagingFactory *f = wic();
    if (f == nullptr)
        return false;
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint32_t p = px[i];
        const uint32_t a = p >> 24;
        bgra[i * 4 + 0] = (uint8_t)(p >> 16);
        bgra[i * 4 + 1] = (uint8_t)(p >> 8);
        bgra[i * 4 + 2] = (uint8_t)p;
        bgra[i * 4 + 3] = (uint8_t)(a >= 128 ? 255 : a * 2);
    }
    wchar_t tmp[32];
    swprintf(tmp, 32, L".%lu.tmp", GetCurrentThreadId());
    const std::wstring part = path + tmp;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(f->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(part.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
              IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    if (frame != nullptr)
        frame->Release();
    if (enc != nullptr)
        enc->Release();
    if (stream != nullptr)
        stream->Release();
    if (ok)
        ok = MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok)
        DeleteFileW(part.c_str());
    return ok;
}

#else /* !_WIN32 */

/* A .png (png_image) or a .jpg (stb_image) file. */
bool wic_decode(const std::wstring &path, Image &out)
{
    std::vector<uint8_t> file;
    if (!read_file(path, file) || file.empty())
        return false;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (file.size() >= 8 && memcmp(file.data(), kPngSig, 8) == 0) {
        if (!png_image::decode(file.data(), file.size(), &px, &w, &h))
            return false;
    } else {
        int comp = 0;
        if (file.size() > (size_t)INT32_MAX)
            return false;
        uint8_t *p = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &comp, 4);
        if (p == nullptr)
            return false;
        px.assign(p, p + (size_t)w * h * 4);
        stbi_image_free(p);
    }
    if (w <= 0 || h <= 0 || (uint32_t)w > kMaxSide || (uint32_t)h > kMaxSide)
        return false;
    flight_take((size_t)w * h * 10);        /* (its pixels, its levels twice over, its loaded form) */
    out.w = (uint32_t)w;
    out.h = (uint32_t)h;
    out.px.swap(px);
    return true;
}

/* w x h texels (the GS's alpha) as a PNG with the usual alpha: min(2 a,
 * 255). */
bool png_write(const std::wstring &path, const uint32_t *px, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint32_t p = px[i];
        const uint32_t a = p >> 24;
        rgba[i * 4 + 0] = (uint8_t)p;
        rgba[i * 4 + 1] = (uint8_t)(p >> 8);
        rgba[i * 4 + 2] = (uint8_t)(p >> 16);
        rgba[i * 4 + 3] = (uint8_t)(a >= 128 ? 255 : a * 2);
    }
    /* (written beside it and moved into place by png_image) */
    return png_image::encode_file(path, rgba.data(), (int)w, (int)h, 1.0, true);
}

#endif /* _WIN32 */

/* The next mip level: 2 x 2 texels averaged, each channel by itself.  (Not
 * weighted by the alpha, and no colour given to texels of alpha 0: in this
 * game a texture's alpha is often not transparency - a car's paint keeps
 * its reflection mask there, and some passes draw a texture's colours with
 * its alpha ignored - so the colours under alpha 0 are picture.  The first
 * form filled them from their neighbours: a car's windows came out light.) */
void halve(const Image &src, Image &dst)
{
    dst.w = std::max(1u, src.w / 2);
    dst.h = std::max(1u, src.h / 2);
    dst.px.resize((size_t)dst.w * dst.h * 4);
    for (uint32_t y = 0; y < dst.h; y++)
        for (uint32_t x = 0; x < dst.w; x++) {
            uint32_t sum[4] = {2, 2, 2, 2};
            for (uint32_t k = 0; k < 4; k++) {
                const uint32_t sx = std::min(src.w - 1, x * 2 + (k & 1)), sy = std::min(src.h - 1, y * 2 + (k >> 1));
                const uint8_t *p = &src.px[((size_t)sy * src.w + sx) * 4];
                for (int ch = 0; ch < 4; ch++)
                    sum[ch] += p[ch];
            }
            uint8_t *o = &dst.px[((size_t)y * dst.w + x) * 4];
            for (int ch = 0; ch < 4; ch++)
                o[ch] = (uint8_t)(sum[ch] / 4);
        }
}

/* ---- block compression (BC1, BC3) --------------------------------------- */

inline uint16_t to565(const float *c)
{
    const int r = (int)(c[0] * (31.0f / 255.0f) + 0.5f), g = (int)(c[1] * (63.0f / 255.0f) + 0.5f),
              b = (int)(c[2] * (31.0f / 255.0f) + 0.5f);
    return (uint16_t)(r << 11 | g << 5 | b);
}

inline void from565(uint16_t c, int *out)
{
    const int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

/* A BC1 colour block (always its four-colour form) of 16 texels (R, G, B,
 * x): the end colours along the texels' principal axis, then twice the
 * texels' nearest of the four and the end colours that fit those best
 * (least squares). */
void bc1_block(const uint8_t *px, uint8_t *out)
{
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            const int v = px[i * 4 + c];
            mn[c] = std::min(mn[c], v);
            mx[c] = std::max(mx[c], v);
            mean[c] += (float)v;
        }
    float e0[3], e1[3];
    if (mn[0] == mx[0] && mn[1] == mx[1] && mn[2] == mx[2]) {
        for (int c = 0; c < 3; c++)
            e0[c] = e1[c] = (float)mn[c];
    } else {
        float cov[6] = {0, 0, 0, 0, 0, 0};
        for (int c = 0; c < 3; c++)
            mean[c] *= 1.0f / 16.0f;
        for (int i = 0; i < 16; i++) {
            const float d[3] = {px[i * 4] - mean[0], px[i * 4 + 1] - mean[1], px[i * 4 + 2] - mean[2]};
            cov[0] += d[0] * d[0];
            cov[1] += d[0] * d[1];
            cov[2] += d[0] * d[2];
            cov[3] += d[1] * d[1];
            cov[4] += d[1] * d[2];
            cov[5] += d[2] * d[2];
        }
        float v[3] = {(float)(mx[0] - mn[0]), (float)(mx[1] - mn[1]), (float)(mx[2] - mn[2])};
        for (int it = 0; it < 4; it++) {
            const float x = v[0] * cov[0] + v[1] * cov[1] + v[2] * cov[2];
            const float y = v[0] * cov[1] + v[1] * cov[3] + v[2] * cov[4];
            const float z = v[0] * cov[2] + v[1] * cov[4] + v[2] * cov[5];
            const float m = std::max(std::max(fabsf(x), fabsf(y)), fabsf(z));
            if (m < 1e-6f)
                break;
            v[0] = x / m;
            v[1] = y / m;
            v[2] = z / m;
        }
        float lo = 1e30f, hi = -1e30f;
        int ilo = 0, ihi = 0;
        for (int i = 0; i < 16; i++) {
            const float p = px[i * 4] * v[0] + px[i * 4 + 1] * v[1] + px[i * 4 + 2] * v[2];
            if (p < lo) {
                lo = p;
                ilo = i;
            }
            if (p > hi) {
                hi = p;
                ihi = i;
            }
        }
        for (int c = 0; c < 3; c++) {
            e0[c] = (float)px[ihi * 4 + c];
            e1[c] = (float)px[ilo * 4 + c];
        }
    }
    uint16_t c0 = to565(e0), c1 = to565(e1);
    uint32_t idx = 0;
    for (int round = 0; round < 3; round++) {
        if (c0 < c1)
            std::swap(c0, c1);
        if (c0 == c1) {
            idx = 0;
            break;
        }
        int p[4][3];
        from565(c0, p[0]);
        from565(c1, p[1]);
        for (int c = 0; c < 3; c++) {
            p[2][c] = (2 * p[0][c] + p[1][c] + 1) / 3;
            p[3][c] = (p[0][c] + 2 * p[1][c] + 1) / 3;
        }
        static const float wa[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        float aa = 0, bb = 0, ab = 0, ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
        idx = 0;
        for (int i = 0; i < 16; i++) {
            int best = 0, bestD = 1 << 30;
            for (int k = 0; k < 4; k++) {
                const int dr = px[i * 4] - p[k][0], dg = px[i * 4 + 1] - p[k][1], db = px[i * 4 + 2] - p[k][2];
                const int d = dr * dr + dg * dg + db * db;
                if (d < bestD) {
                    bestD = d;
                    best = k;
                }
            }
            idx |= (uint32_t)best << (2 * i);
            const float a = wa[best], b = 1.0f - a;
            aa += a * a;
            bb += b * b;
            ab += a * b;
            for (int c = 0; c < 3; c++) {
                ax[c] += a * px[i * 4 + c];
                bx[c] += b * px[i * 4 + c];
            }
        }
        if (round == 2)
            break;
        const float det = aa * bb - ab * ab;
        if (fabsf(det) < 1e-4f)
            break;
        float n0[3], n1[3];
        for (int c = 0; c < 3; c++) {
            n0[c] = std::min(255.0f, std::max(0.0f, (ax[c] * bb - bx[c] * ab) / det));
            n1[c] = std::min(255.0f, std::max(0.0f, (bx[c] * aa - ax[c] * ab) / det));
        }
        const uint16_t d0 = to565(n0), d1 = to565(n1);
        if ((d0 == c0 && d1 == c1) || d0 == d1)
            break;
        c0 = d0;
        c1 = d1;
    }
    out[0] = (uint8_t)c0;
    out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)c1;
    out[3] = (uint8_t)(c1 >> 8);
    memcpy(out + 4, &idx, 4);
}

/* A BC3 alpha block of 16 texels' alpha (its eight-step form). */
void bc3_alpha_block(const uint8_t *px, uint8_t *out)
{
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) {
        mn = std::min<int>(mn, px[i * 4 + 3]);
        mx = std::max<int>(mx, px[i * 4 + 3]);
    }
    out[0] = (uint8_t)mx;
    out[1] = (uint8_t)mn;
    uint64_t bits = 0;
    if (mx != mn) {
        const int range = mx - mn;
        for (int i = 0; i < 16; i++) {
            const int s = ((mx - px[i * 4 + 3]) * 7 + range / 2) / range;     /* 0: mx .. 7: mn */
            const int code = s == 0 ? 0 : s == 7 ? 1 : s + 1;
            bits |= (uint64_t)code << (3 * i);
        }
    }
    for (int i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(bits >> (8 * i));
}

/* A level as BC1 (8 bytes a block) or BC3 (16): its sides multiples of 4
 * but for the small levels, whose blocks repeat their edge texels. */
void compress_level(const Image &im, bool alpha, std::vector<uint8_t> &out)
{
    const uint32_t bw = (im.w + 3) / 4, bh = (im.h + 3) / 4;
    const size_t at = out.size(), each = alpha ? 16 : 8;
    out.resize(at + (size_t)bw * bh * each);
    uint8_t *o = out.data() + at;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t block[64];
            for (uint32_t k = 0; k < 16; k++) {
                const uint32_t sx = std::min(im.w - 1, bx * 4 + (k & 3)), sy = std::min(im.h - 1, by * 4 + (k >> 2));
                memcpy(block + k * 4, &im.px[((size_t)sy * im.w + sx) * 4], 4);
            }
            if (alpha) {
                bc3_alpha_block(block, o);
                o += 8;
            }
            bc1_block(block, o);
            o += 8;
        }
}

/* ======================================================================== */
/* The loaded form                                                          */
/* ======================================================================== */

/* A texture ready for the device: its levels one after the other in data
 * (or in the file read, a .dds as it is: at). */
struct Blob {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0, mips = 0;
    uint32_t mean = 0;
    uint8_t aMin = 0, aMax = 128;           /* the GS's units */
    std::vector<uint8_t> data;
    size_t at = 0;                          /* the first level's place in data */
};

inline uint32_t block_bytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return 8;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC7_UNORM: return 16;
    default: return 0;
    }
}

/* Level l's bytes and row pitch. */
size_t level_bytes(DXGI_FORMAT f, uint32_t w, uint32_t h, uint32_t l, uint32_t *pitch)
{
    const uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
    const uint32_t bb = block_bytes(f);
    if (bb != 0) {
        *pitch = ((lw + 3) / 4) * bb;
        return (size_t)*pitch * ((lh + 3) / 4);
    }
    *pitch = lw * 4;
    return (size_t)lw * 4 * lh;
}

size_t blob_bytes(const Blob &b)
{
    size_t n = 0;
    uint32_t pitch;
    for (uint32_t l = 0; l < b.mips; l++)
        n += level_bytes(b.format, b.w, b.h, l, &pitch);
    return n;
}

inline uint8_t gs_alpha(uint32_t a)
{
    return (uint8_t)((a + 1) >> 1);
}

/* A decoded picture as the loaded form: its mip levels, compressed when
 * asked (and its sides are multiples of 4: the device wants a
 * block-compressed texture's so). */
void build_blob(Image &im, bool compress, Blob &out)
{
    uint32_t aMin = 255, aMax = 0;
    uint64_t sum[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < (size_t)im.w * im.h; i++) {
        const uint8_t *p = &im.px[i * 4];
        aMin = std::min<uint32_t>(aMin, p[3]);
        aMax = std::max<uint32_t>(aMax, p[3]);
        for (int c = 0; c < 4; c++)
            sum[c] += p[c];
    }
    const uint64_t n = (uint64_t)im.w * im.h;
    out.mean = (uint32_t)(sum[0] / n) | (uint32_t)(sum[1] / n) << 8 | (uint32_t)(sum[2] / n) << 16 |
               (uint32_t)gs_alpha((uint32_t)(sum[3] / n)) << 24;
    out.aMin = gs_alpha(aMin);
    out.aMax = gs_alpha(aMax);
    const bool alpha = aMin != 255;
    const bool bc = compress && (im.w % 4) == 0 && (im.h % 4) == 0;
    out.format = !bc ? DXGI_FORMAT_R8G8B8A8_UNORM : alpha ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_BC1_UNORM;
    out.w = im.w;
    out.h = im.h;
    out.mips = 0;
    out.at = 0;
    out.data.clear();
    Image a, b;
    Image *cur = &im, *next = &a;
    for (;;) {
        if (bc)
            compress_level(*cur, alpha, out.data);
        else
            out.data.insert(out.data.end(), cur->px.begin(), cur->px.end());
        out.mips++;
        if (cur->w == 1 && cur->h == 1)
            break;
        halve(*cur, *next);
        cur = next;
        next = next == &a ? &b : &a;
    }
}

/* ---- .dds ---------------------------------------------------------------- */

inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Bits [at, at + n) of a 16-byte block. */
inline uint32_t bits_of(const uint8_t *b, uint32_t at, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
        v |= (uint32_t)((b[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
    return v;
}

/* The least and greatest alpha a block-compressed level can give (never
 * narrower than the truth: the alpha end values of each block). */
void bc_alpha_range(DXGI_FORMAT f, const uint8_t *data, size_t bytes, uint32_t *lo, uint32_t *hi)
{
    uint32_t mn = 255, mx = 0;
    auto take = [&](uint32_t a) {
        mn = std::min(mn, a);
        mx = std::max(mx, a);
    };
    const uint32_t each = block_bytes(f);
    for (size_t at = 0; at + each <= bytes; at += each) {
        const uint8_t *b = data + at;
        if (f == DXGI_FORMAT_BC1_UNORM) {
            const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
            bool clear = false;
            if (c0 <= c1) {
                const uint32_t idx = rd32(b + 4);
                for (int i = 0; i < 16 && !clear; i++)
                    clear = ((idx >> (2 * i)) & 3) == 3;
            }
            take(255);
            if (clear)
                take(0);
        } else if (f == DXGI_FORMAT_BC2_UNORM) {
            for (int i = 0; i < 16; i++)
                take(((b[i >> 1] >> ((i & 1) * 4)) & 15) * 17);
        } else if (f == DXGI_FORMAT_BC3_UNORM) {
            take(b[0]);
            take(b[1]);
            if (b[0] <= b[1]) {             /* (the six-step form: 0 and 255 are codes of their own) */
                uint64_t bitsv = 0;
                for (int i = 0; i < 6; i++)
                    bitsv |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) {
                    const uint32_t code = (uint32_t)(bitsv >> (3 * i)) & 7;
                    if (code == 6)
                        take(0);
                    if (code == 7)
                        take(255);
                }
            }
        } else {                            /* BC7: by the block's mode */
            uint32_t mode = 0;
            while (mode < 8 && ((b[0] >> mode) & 1) == 0)
                mode++;
            if (mode < 4) {
                take(255);
            } else if (mode == 4 || mode == 5) {
                const uint32_t rot = bits_of(b, mode == 4 ? 5 : 6, 2);
                if (mode == 4) {
                    const uint32_t ch = rot == 0 ? 38 : 8 + (rot - 1) * 10, nb = rot == 0 ? 6 : 5;
                    for (uint32_t e = 0; e < 2; e++) {
                        const uint32_t v = bits_of(b, ch + e * nb, nb);
                        take(nb == 6 ? (v << 2 | v >> 4) : (v << 3 | v >> 2));
                    }
                } else {
                    const uint32_t ch = rot == 0 ? 50 : 8 + (rot - 1) * 14, nb = rot == 0 ? 8 : 7;
                    for (uint32_t e = 0; e < 2; e++) {
                        const uint32_t v = bits_of(b, ch + e * nb, nb);
                        take(nb == 8 ? v : (v << 1 | v >> 6));
                    }
                }
            } else if (mode == 6) {
                take(bits_of(b, 49, 7) << 1 | bits_of(b, 63, 1));
                take(bits_of(b, 56, 7) << 1 | bits_of(b, 64, 1));
            } else if (mode == 7) {
                for (uint32_t e = 0; e < 4; e++) {
                    const uint32_t v = bits_of(b, 74 + e * 5, 5) << 1 | bits_of(b, 94 + e, 1);
                    take(v << 2 | v >> 4);
                }
            } else {
                take(0);
                take(255);
            }
        }
    }
    if (mn > mx) {
        mn = 0;
        mx = 255;
    }
    *lo = mn;
    *hi = mx;
}

/* A .dds file's bytes: out as the file is (its levels at out.at of the
 * same bytes, moved into out.data), or im for one to be built as a decoded
 * picture (uncompressed without mip levels, or 24-bit).  0 no, 1 the blob,
 * 2 the picture. */
int dds_parse(std::vector<uint8_t> &file, Blob &out, Image &im, const char **why)
{
    *why = "not a .dds file";
    if (file.size() < 128 || rd32(file.data()) != 0x20534444u || rd32(file.data() + 4) != 124)
        return 0;
    const uint8_t *hd = file.data() + 4;
    const uint32_t h = rd32(hd + 8), w = rd32(hd + 12);
    uint32_t mips = std::max(1u, rd32(hd + 24));
    const uint8_t *pf = hd + 72;
    const uint32_t pfFlags = rd32(pf + 4), fourCC = rd32(pf + 8), bitCount = rd32(pf + 12);
    const uint32_t rMask = rd32(pf + 16), gMask = rd32(pf + 20), bMask = rd32(pf + 24), aMask = rd32(pf + 28);
    const uint32_t caps2 = rd32(hd + 108);
    size_t at = 128;
    *why = "a cube map or a volume";
    if ((caps2 & 0x200) != 0 || (caps2 & 0x200000) != 0)
        return 0;
    *why = "its size";
    if (w == 0 || h == 0 || w > kMaxSide || h > kMaxSide)
        return 0;
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    bool bgr24 = false;
    *why = "its pixel format";
    if ((pfFlags & 4) != 0) {               /* DDPF_FOURCC */
        switch (fourCC) {
        case 0x31545844: f = DXGI_FORMAT_BC1_UNORM; break;                      /* DXT1 */
        case 0x32545844: case 0x33545844: f = DXGI_FORMAT_BC2_UNORM; break;     /* DXT2, DXT3 */
        case 0x34545844: case 0x35545844: f = DXGI_FORMAT_BC3_UNORM; break;     /* DXT4, DXT5 */
        case 0x30315844: {                                                      /* DX10 */
            if (file.size() < 148)
                return 0;
            const uint32_t dx = rd32(file.data() + 128), dim = rd32(file.data() + 132), arr = rd32(file.data() + 140);
            if (dim != 3 || arr > 1)
                return 0;
            switch (dx) {
            case 71: case 72: f = DXGI_FORMAT_BC1_UNORM; break;
            case 74: case 75: f = DXGI_FORMAT_BC2_UNORM; break;
            case 77: case 78: f = DXGI_FORMAT_BC3_UNORM; break;
            case 98: case 99: f = DXGI_FORMAT_BC7_UNORM; break;
            case 28: case 29: f = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case 87: case 91: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case 88: case 93: f = DXGI_FORMAT_B8G8R8X8_UNORM; break;
            default: return 0;
            }
            at = 148;
            break;
        }
        default: return 0;
        }
    } else if ((pfFlags & 0x40) != 0 && bitCount == 32) {       /* DDPF_RGB */
        const bool hasA = (pfFlags & 1) != 0 && aMask == 0xFF000000u;
        if (rMask == 0x00FF0000u && gMask == 0x0000FF00u && bMask == 0x000000FFu)
            f = hasA ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
        else if (rMask == 0x000000FFu && gMask == 0x0000FF00u && bMask == 0x00FF0000u && hasA)
            f = DXGI_FORMAT_R8G8B8A8_UNORM;
        else
            return 0;
    } else if ((pfFlags & 0x40) != 0 && bitCount == 24 && rMask == 0x00FF0000u && gMask == 0x0000FF00u &&
               bMask == 0x000000FFu) {
        bgr24 = true;
    } else {
        return 0;
    }
    const uint32_t full = 1 + (uint32_t)std::floor(std::log2((double)std::max(w, h)));
    mips = std::min(mips, full);
    const bool bc = block_bytes(f) != 0;
    *why = "a block-compressed file's width and height must be multiples of 4";
    if (bc && ((w % 4) != 0 || (h % 4) != 0))
        return 0;
    *why = "it is cut short";
    if (bgr24 || (!bc && mips == 1 && (w > 1 || h > 1))) {
        /* a picture: its mip levels made here */
        const size_t bpp = bgr24 ? 3 : 4;
        const size_t pitch = bgr24 ? (((size_t)w * 3 + 3) & ~(size_t)3) : (size_t)w * 4;
        const size_t pitchUsed = (size_t)w * bpp;
        if (file.size() < at + (size_t)(h - 1) * (bgr24 ? pitchUsed : pitch) + pitchUsed)
            return 0;
        im.w = w;
        im.h = h;
        im.px.resize((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++) {
            /* (a 24-bit file's rows are not padded in practice: read them packed) */
            const uint8_t *s = file.data() + at + (size_t)y * pitchUsed;
            uint8_t *d = &im.px[(size_t)y * w * 4];
            for (uint32_t x = 0; x < w; x++) {
                if (bgr24) {
                    d[x * 4] = s[x * 3 + 2];
                    d[x * 4 + 1] = s[x * 3 + 1];
                    d[x * 4 + 2] = s[x * 3];
                    d[x * 4 + 3] = 255;
                } else if (f == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    memcpy(d + x * 4, s + x * 4, 4);
                } else {
                    d[x * 4] = s[x * 4 + 2];
                    d[x * 4 + 1] = s[x * 4 + 1];
                    d[x * 4 + 2] = s[x * 4];
                    d[x * 4 + 3] = f == DXGI_FORMAT_B8G8R8X8_UNORM ? 255 : s[x * 4 + 3];
                }
            }
        }
        return 2;
    }
    out.format = f;
    out.w = w;
    out.h = h;
    out.mips = mips;
    out.at = at;
    uint32_t pitch;
    const size_t first = level_bytes(f, w, h, 0, &pitch);
    if (file.size() < at + blob_bytes(out))
        return 0;
    uint32_t lo = 255, hi = 255;
    if (bc) {
        bc_alpha_range(f, file.data() + at, first, &lo, &hi);
    } else if (f != DXGI_FORMAT_B8G8R8X8_UNORM) {
        lo = 255;
        hi = 0;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            const uint32_t a = file[at + i * 4 + 3];
            lo = std::min(lo, a);
            hi = std::max(hi, a);
        }
    }
    out.aMin = gs_alpha(lo);
    out.aMax = gs_alpha(hi);
    out.mean = 0;
    out.data.swap(file);
    return 1;
}

/* ---- the pre-cache -------------------------------------------------------- */

struct CacheHeader {
    char magic[8];                          /* "GT3TEXC" and the form's version (a new one: the files are made again) */
    uint32_t format, w, h, mips, mean;
    uint8_t aMin, aMax, pad[2];
    uint64_t srcSize, srcTime;
    uint32_t dataBytes;
    uint32_t pad2;
};
const char kCacheMagic[8] = {'G', 'T', '3', 'T', 'E', 'X', 'C', 2};   /* (2: the levels plain means, no colours filled in) */

bool cache_read(const std::wstring &path, uint64_t srcSize, uint64_t srcTime, Blob &out)
{
    std::vector<uint8_t> file;
    if (!read_file(path, file) || file.size() < sizeof(CacheHeader))
        return false;
    CacheHeader hd;
    memcpy(&hd, file.data(), sizeof(hd));
    if (memcmp(hd.magic, kCacheMagic, 8) != 0 || hd.srcSize != srcSize || hd.srcTime != srcTime || hd.w == 0 ||
        hd.h == 0 || hd.w > kMaxSide || hd.h > kMaxSide || hd.mips == 0 || hd.mips > 15)
        return false;
    out.format = (DXGI_FORMAT)hd.format;
    if (out.format != DXGI_FORMAT_R8G8B8A8_UNORM && out.format != DXGI_FORMAT_BC1_UNORM &&
        out.format != DXGI_FORMAT_BC3_UNORM)
        return false;
    out.w = hd.w;
    out.h = hd.h;
    out.mips = hd.mips;
    out.mean = hd.mean;
    out.aMin = hd.aMin;
    out.aMax = hd.aMax;
    out.at = sizeof(CacheHeader);
    if (blob_bytes(out) != hd.dataBytes || file.size() != sizeof(CacheHeader) + hd.dataBytes)
        return false;
    out.data.swap(file);
    return true;
}

bool cache_write(const std::wstring &path, uint64_t srcSize, uint64_t srcTime, const Blob &b)
{
    CacheHeader hd = {};
    memcpy(hd.magic, kCacheMagic, 8);
    hd.format = (uint32_t)b.format;
    hd.w = b.w;
    hd.h = b.h;
    hd.mips = b.mips;
    hd.mean = b.mean;
    hd.aMin = b.aMin;
    hd.aMax = b.aMax;
    hd.srcSize = srcSize;
    hd.srcTime = srcTime;
    hd.dataBytes = (uint32_t)blob_bytes(b);
    return write_file(path, &hd, sizeof(hd), b.data.data() + b.at, hd.dataBytes);
}

/* ======================================================================== */
/* The state                                                                */
/* ======================================================================== */

enum Kind : uint8_t { kDds, kPng, kJpg };     /* (the order two files of one hash in a folder are taken in) */

struct Source {
    std::wstring path;
    Kind kind = kPng;
    uint64_t size = 0, time = 0;
};

enum EntryState : uint8_t { kEntIdle, kEntQueued, kEntReady, kEntFailed };   /* (kReady..: the lookup's answers) */

struct Entry {
    Source src;
    EntryState state = kEntIdle;
    bool cached = false;                    /* the pre-cache made ahead looked at it */
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0, mean = 0;
    uint8_t aMin = 0, aMax = 128;
    size_t bytes = 0;
    double asked = 0.0;
    uint32_t version = 0;                   /* moved when its source changes (refresh): a load of the old one is dropped */
};

enum JobKind : uint8_t { kJobIndex, kJobLoad, kJobCache, kJobDump };

struct Job {
    JobKind kind = kJobLoad;
    uint64_t hash = 0;
    Source src;
    bool selected = false;                  /* a dump into dumps_selected */
    bool refresh = false;                   /* the folders listed again (refresh) */
    uint32_t version = 0;                   /* a load: the entry's version when it was asked for */
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> px;
};

struct Family {
    double start = 0.0, mutedUntil = 0.0;
    uint32_t count = 0;
};

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    std::deque<Job> hi, lo;                 /* the loads and dumps; the pre-cache made ahead */
    std::unordered_map<uint64_t, Entry> table;
    std::vector<uint64_t> ready;            /* the table's loaded entries */
    std::vector<uint64_t> loaded;           /* take_loaded's */
    std::unordered_set<uint64_t> dumped, dumpedSelected;
    std::unordered_map<uint64_t, Family> families;
    size_t dumpBytes = 0;
    std::wstring root, cache;
    std::vector<std::wstring> packs;
    bool listed = false;
    uint32_t files = 0, failed = 0, dumpedNow = 0, dumpsWaiting = 0, waiting = 0;
    uint32_t cacheMade = 0, cacheTodo = 0;
    /* refresh: the folders' new list, waiting for the executor (take_changed);
     * the last one's counts */
    bool refreshing = false;
    std::unordered_map<uint64_t, Source> pending;
    uint32_t refreshes = 0, refChanged = 0, refNew = 0, refGone = 0;
    /* Select mode */
    std::vector<Seen> list;
    uint64_t sel = 0;
    uint64_t selPxHash = 0;
    uint32_t selW = 0, selH = 0;
    std::vector<uint32_t> selPx;
};
State *g = nullptr;

std::atomic<bool> g_replace{false}, g_precache{false}, g_compress{true}, g_menuOpen{false};
std::atomic<int> g_dump{0};
std::atomic<uint64_t> g_serial{1};
std::atomic<uint32_t> g_loadedCount{0};
std::atomic<uint64_t> g_selected{0};
std::atomic<bool> g_started{false};
std::atomic<double> g_offSince{-1.0e9};     /* when the replacements were turned off (keep_loaded) */
std::atomic<bool> g_pending{false};         /* State::pending holds a list */
std::atomic<uint32_t> g_refreshSerial{0};   /* the refreshes taken */

/* The seconds the replacements stay loaded after they were turned off: the
 * on / off key is for comparing (keep_loaded). */
constexpr double kKeepOff = 300.0;

struct Hold {
    Hold() { AcquireSRWLockExclusive(&g->lock); }
    ~Hold() { ReleaseSRWLockExclusive(&g->lock); }
};

void take_settings()
{
    const bool was = g_replace.load();
    g_replace = settings::texture_replace();
    if (was && !g_replace.load())
        g_offSince = now_seconds();         /* (keep_loaded) */
    g_precache = settings::texture_precache();
    g_compress = settings::texture_compress();
    g_dump = (int)settings::texture_dump();
}

void update_selected()
{
    g_selected = g_menuOpen.load() && g_dump.load() == (int)settings::TextureDump::select ? g->sel : 0;
}

/* The pre-cache file of a source: named by the texture and by the source's
 * path, size, time and how it is held. */
std::wstring cache_path(uint64_t hash, const Source &s, bool compress)
{
    uint32_t a = 0x9E3779B1u, b = 0x85EBCA77u;
    for (wchar_t c : s.path) {
        const uint32_t v = (uint32_t)towlower(c);
        a = rotl32(a + v * 0x85EBCA77u, 13) * 0x9E3779B1u;
        b = rotl32(b ^ v, 5) * 0xC2B2AE3Du;
    }
    a = fmix32(a ^ (uint32_t)s.size ^ (uint32_t)(s.time >> 32) ^ (compress ? 0x5Cu : 0u));
    b = fmix32(b ^ (uint32_t)s.time ^ (uint32_t)(s.size >> 32));
    char name[17];
    hash_name(hash, name);
    wchar_t file[64];
    swprintf(file, 64, L"%hs_%08X%08X.gt3tex", name, a, b);
    return g->cache + file;
}

/* A source as its loaded form (a worker): from the pre-cache when it is
 * there, else read and built (and kept there when it is on).  direct: a
 * .dds as it is (nothing to keep). */
bool make_blob(uint64_t hash, const Source &s, Blob &out, bool *direct, const char **why)
{
    *direct = false;
    *why = "it could not be read";
    const bool compress = g_compress.load(), precache = g_precache.load();
    const std::wstring cp = cache_path(hash, s, compress);
    if (s.kind != kDds && precache && cache_read(cp, s.size, s.time, out))
        return true;
    Image im;
    if (s.kind == kDds) {
        std::vector<uint8_t> file;
        flight_take((size_t)std::min<uint64_t>(s.size, 512u << 20));
        if (!read_file(s.path, file))
            return false;
        const int r = dds_parse(file, out, im, why);
        if (r == 0)
            return false;
        if (r == 1) {
            *direct = true;
            return true;
        }
        if (precache && cache_read(cp, s.size, s.time, out))
            return true;
    } else if (!wic_decode(s.path, im)) {
        *why = "it could not be decoded (or is over 8192 texels a side)";
        return false;
    }
    build_blob(im, compress, out);
    if (precache) {
        make_folders(g->cache);
        if (!cache_write(cp, s.size, s.time, out))
            log_once("pgl-texrepl-cache", "textures: the pre-cache could not be written (%s)", narrow(cp).c_str());
    }
    return true;
}

bool make_device_texture(const Blob &b, Com<ID3D11Texture2D> &tex, Com<ID3D11ShaderResourceView> &srv)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = b.w;
    d.Height = b.h;
    d.MipLevels = b.mips;
    d.ArraySize = 1;
    d.Format = b.format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init[16] = {};
    size_t at = b.at;
    for (uint32_t l = 0; l < b.mips && l < 16; l++) {
        uint32_t pitch;
        const size_t n = level_bytes(b.format, b.w, b.h, l, &pitch);
        init[l].pSysMem = b.data.data() + at;
        init[l].SysMemPitch = pitch;
        at += n;
    }
    if (FAILED(dev()->CreateTexture2D(&d, init, tex.put())))
        return false;
    return SUCCEEDED(dev()->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
}

/* ---- the folders' list ---------------------------------------------------- */

#ifdef _WIN32
void list_folder(const std::wstring &folder, std::unordered_map<uint64_t, Source> &found, int depth)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::wstring> under;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (depth < 64 && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)    /* (64 deep: as the launcher counts) */
                under.push_back(folder + L"\\" + fd.cFileName);
            continue;
        }
        uint64_t hash;
        if (!name_hash(fd.cFileName, &hash))
            continue;
        const wchar_t *dot = wcsrchr(fd.cFileName, L'.');
        if (dot == nullptr)
            continue;
        Kind kind;
        if (_wcsicmp(dot, L".dds") == 0)
            kind = kDds;
        else if (_wcsicmp(dot, L".png") == 0)
            kind = kPng;
        else if (_wcsicmp(dot, L".jpg") == 0 || _wcsicmp(dot, L".jpeg") == 0)
            kind = kJpg;
        else
            continue;
        Source &s = found[hash];
        if (!s.path.empty() && s.kind <= kind)
            continue;                       /* (a .dds before a .png before a .jpg of the same texture) */
        s.path = folder + L"\\" + fd.cFileName;
        s.kind = kind;
        s.size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        s.time = (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const std::wstring &u : under)
        list_folder(u, found, depth + 1);
}

void list_hashes(const std::wstring &folder, std::unordered_set<uint64_t> &out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        uint64_t hash;
        if (name_hash(fd.cFileName, &hash))
            out.insert(hash);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

#else /* !_WIN32 */

/* The kind of a replacement file by its extension (false: none). */
bool source_kind(const std::wstring &name, Kind *kind)
{
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos)
        return false;
    std::wstring ext = name.substr(dot);
    for (wchar_t &c : ext)
        c = (wchar_t)towlower(c);
    if (ext == L".dds")
        *kind = kDds;
    else if (ext == L".png")
        *kind = kPng;
    else if (ext == L".jpg" || ext == L".jpeg")
        *kind = kJpg;
    else
        return false;
    return true;
}

/* (the entries are found as the system lists them; symbolic links followed,
 * 64 folders deep at most, as on Windows) */
void list_folder(const std::wstring &folder, std::unordered_map<uint64_t, Source> &found, int depth)
{
    std::vector<fs::Entry> entries;
    if (!fs::list(fs::narrow(folder), &entries))
        return;
    std::vector<std::wstring> under;
    for (const fs::Entry &e : entries) {
        const std::wstring name = fs::widen(e.name);
        if (e.st.dir) {
            if (depth < 64)
                under.push_back(folder + L"/" + name);
            continue;
        }
        uint64_t hash;
        if (!name_hash(name.c_str(), &hash))
            continue;
        Kind kind;
        if (!source_kind(name, &kind))
            continue;
        Source &s = found[hash];
        if (!s.path.empty() && s.kind <= kind)
            continue;                       /* (a .dds before a .png before a .jpg of the same texture) */
        s.path = folder + L"/" + name;
        s.kind = kind;
        s.size = e.st.size;
        s.time = (uint64_t)e.st.modify;
    }
    for (const std::wstring &u : under)
        list_folder(u, found, depth + 1);
}

/* The names in folder (ending in a separator) that end in ext, without
 * case. */
std::vector<std::wstring> names_ending(const std::wstring &folder, const wchar_t *ext)
{
    std::vector<std::wstring> out;
    std::vector<fs::Entry> entries;
    if (!fs::list(fs::narrow(folder), &entries))
        return out;
    const size_t n = wcslen(ext);
    for (const fs::Entry &e : entries) {
        if (e.st.dir)
            continue;
        std::wstring name = fs::widen(e.name);
        if (name.size() < n)
            continue;
        bool match = true;
        for (size_t k = 0; k < n && match; k++)
            match = towlower(name[name.size() - n + k]) == towlower(ext[k]);
        if (match)
            out.push_back(std::move(name));
    }
    return out;
}

void list_hashes(const std::wstring &folder, std::unordered_set<uint64_t> &out)
{
    for (const std::wstring &name : names_ending(folder, L".png")) {
        uint64_t hash;
        if (name_hash(name.c_str(), &hash))
            out.insert(hash);
    }
}

#endif /* _WIN32 */

/* The pre-cache made ahead: a job for every listed file not looked at yet
 * (the lock held). */
void queue_cache_jobs()
{
    for (auto &e : g->table) {
        if (e.second.cached)
            continue;
        e.second.cached = true;
        Job j;
        j.kind = kJobCache;
        j.hash = e.first;
        j.src = e.second.src;
        g->lo.push_back(std::move(j));
        g->cacheTodo++;
    }
    WakeAllConditionVariable(&g->wake);
}

/* The folders listed: the first time into the table (nothing is loaded
 * yet); again (refresh) for the executor to take (take_changed: it holds
 * the textures the changed files gave). */
void job_index(bool refresh)
{
    const double t0 = now_seconds();
    std::unordered_map<uint64_t, Source> all;
    for (const std::wstring &pack : g->packs) {
        std::unordered_map<uint64_t, Source> found;
        list_folder(pack, found, 0);
        size_t taken = 0;
        for (auto &f : found)
            if (all.emplace(f.first, std::move(f.second)).second)
                taken++;
        pclog::write("textures: %s: %u replacement files (%u used)", narrow(pack).c_str(), (unsigned)found.size(),
                     (unsigned)taken);
    }
    if (refresh) {
        Hold hold;
        g->pending.swap(all);
        g_pending = true;
        return;
    }
    std::unordered_set<uint64_t> dumped, dumpedSel;
    list_hashes(g->root + L"dumps\\", dumped);
    list_hashes(g->root + L"dumps_selected\\", dumpedSel);
    /* pre-cache files of sources no longer listed (or changed) */
    uint32_t stale = 0;
    if (g_precache.load()) {
        std::unordered_set<std::wstring> want;
        for (const auto &e : all) {
            want.insert(cache_path(e.first, e.second, true));
            want.insert(cache_path(e.first, e.second, false));
        }
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((g->cache + L"*.gt3tex").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring p = g->cache + fd.cFileName;
                if (want.count(p) == 0 && DeleteFileW(p.c_str()))
                    stale++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
#else
        for (const std::wstring &name : names_ending(g->cache, L".gt3tex")) {
            const std::wstring p = g->cache + name;
            if (want.count(p) == 0 && fs::remove_file(fs::narrow(p)))
                stale++;
        }
#endif
    }
    {
        Hold hold;
        for (auto &e : all) {
            Entry &en = g->table[e.first];
            en.src = std::move(e.second);
        }
        g->files = (uint32_t)g->table.size();
        g->dumped.insert(dumped.begin(), dumped.end());
        g->dumpedSelected.swap(dumpedSel);
        g->listed = true;
        if (g_precache.load())
            queue_cache_jobs();
    }
    g_serial++;
    pclog::write("textures: %u folders listed in %.0f ms: %u replacement files; %u dumps there already%s",
                 (unsigned)g->packs.size(), (now_seconds() - t0) * 1000.0, (unsigned)g->files, (unsigned)dumped.size(),
                 stale != 0 ? "; old pre-cache files removed" : "");
}

void job_load(const Job &j)
{
    Blob blob;
    bool direct = false;
    const char *why = "";
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    const double t0 = now_seconds();
    bool ok = make_blob(j.hash, j.src, blob, &direct, &why);
    if (ok && !make_device_texture(blob, tex, srv)) {
        ok = false;
        why = "the device refused it";
    }
    char name[17];
    hash_name(j.hash, name);
    {
        Hold hold;
        auto it = g->table.find(j.hash);
        /* (a file changed or gone since it was asked for - a refresh - is
         * dropped: its entry has been asked for anew, or will be) */
        const bool current = it != g->table.end() && it->second.version == j.version;
        if (!current)
            ok = true;                      /* (nothing to report) */
        if (current) {
            Entry &e = it->second;
            if (ok) {
                e.tex = std::move(tex);
                e.srv = std::move(srv);
                e.w = blob.w;
                e.h = blob.h;
                e.mean = blob.mean;
                e.aMin = blob.aMin;
                e.aMax = blob.aMax;
                e.bytes = blob_bytes(blob);
                e.asked = now_seconds();
                e.state = kEntReady;
                g->ready.push_back(j.hash);
                g->loaded.push_back(j.hash);
                g_loadedCount++;
            } else {
                e.state = kEntFailed;
                g->failed++;
            }
        }
        if (g->waiting > 0)
            g->waiting--;
    }
    flight_done();
    if (!ok)
        pclog::write("textures: %s not used: %s", narrow(j.src.path).c_str(), why);
    else if (settings::debug_file_log())
        pclog::write("textures: %s: %s, %ux%u, %u levels, %.1f ms", name, narrow(j.src.path).c_str(), blob.w, blob.h,
                     blob.mips, (now_seconds() - t0) * 1000.0);
}

void job_cache(const Job &j)
{
    /* (a .dds used as it is has nothing to keep; a file made already is
     * only looked at) */
    const bool compress = g_compress.load();
    bool made = false;
    if (g_precache.load()) {
        const std::wstring cp = cache_path(j.hash, j.src, compress);
#ifdef _WIN32
        bool need = GetFileAttributesW(cp.c_str()) == INVALID_FILE_ATTRIBUTES;
#else
        bool need = !fs::exists(fs::narrow(cp));
#endif
        if (need && j.src.kind == kDds) {
            std::vector<uint8_t> head;
            need = false;
            if (read_file(j.src.path, head, 148) && head.size() >= 128) {
                const uint32_t pfFlags = rd32(head.data() + 80), mips = rd32(head.data() + 28);
                need = (pfFlags & 4) == 0 && mips <= 1;     /* (uncompressed, one level: built as a picture) */
            }
        }
        if (need) {
            Blob blob;
            bool direct = false;
            const char *why = "";
            made = make_blob(j.hash, j.src, blob, &direct, &why) && !direct;
        }
    }
    flight_done();
    Hold hold;
    if (g->cacheTodo > 0)
        g->cacheTodo--;
    if (made)
        g->cacheMade++;
}

void job_dump(Job &j)
{
    char name[17];
    hash_name(j.hash, name);
    const std::wstring folder = g->root + (j.selected ? L"dumps_selected\\" : L"dumps\\");
    make_folders(folder);
    wchar_t file[32];
    swprintf(file, 32, L"%hs.png", name);
    const bool ok = png_write(folder + file, j.px.data(), j.w, j.h);
    if (!ok)
        log_once("pgl-texrepl-dump", "textures: a dump could not be written (%s%s.png)", narrow(folder).c_str(), name);
    Hold hold;
    g->dumpBytes -= j.px.size() * 4;
    if (g->dumpsWaiting > 0)
        g->dumpsWaiting--;
    if (ok)
        g->dumpedNow++;
    else if (!j.selected)
        g->dumped.erase(j.hash);            /* (tried again when it is decoded again) */
    else
        g->dumpedSelected.erase(j.hash);
}

DWORD WINAPI worker(void *arg)
{
    q::release_here();                      /* (its device objects were never in a queued call) */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const bool second = arg != nullptr;
    for (;;) {
        Job j;
        AcquireSRWLockExclusive(&g->lock);
        while (g->hi.empty() && g->lo.empty())
            SleepConditionVariableSRW(&g->wake, &g->lock, INFINITE, 0);
        std::deque<Job> &from = !g->hi.empty() ? g->hi : g->lo;
        j = std::move(from.front());
        from.pop_front();
        ReleaseSRWLockExclusive(&g->lock);
        (void)second;
        switch (j.kind) {
        case kJobIndex: job_index(j.refresh); break;
        case kJobLoad: job_load(j); break;
        case kJobCache: job_cache(j); break;
        case kJobDump: job_dump(j); break;
        }
    }
}

}  /* namespace */

/* ======================================================================== */
/* The registry's side                                                      */
/* ======================================================================== */

void init()
{
    if (g_started.exchange(true))
        return;
    g = new State();
    take_settings();
    char path[1024];
    settings::texture_root(path, sizeof(path));
    g->root = widen(path);
    settings::texture_cache(path, sizeof(path));
    g->cache = widen(path);
    for (int i = 0; i < settings::texture_pack_count(); i++) {
        settings::texture_pack(i, path, sizeof(path));
        if (path[0] != '\0')
            g->packs.push_back(widen(path));
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int workers = si.dwNumberOfProcessors >= 8 ? 3 : si.dwNumberOfProcessors >= 4 ? 2 : 1;
    for (int i = 0; i < workers; i++) {
        HANDLE t = CreateThread(nullptr, 0, worker, (void *)(intptr_t)i, 0, nullptr);
        if (t != nullptr)
            CloseHandle(t);
    }
    Job j;
    j.kind = kJobIndex;
    {
        Hold hold;
        g->hi.push_back(std::move(j));
        WakeConditionVariable(&g->wake);
    }
    /* the interface's side (platform/texture_menu.h) */
    static const texture_menu::Backend backend = {status, menu_open, settings_changed, select_step, dump_selected,
                                                  refresh, refresh_serial};
    texture_menu::set_backend(&backend);
}

bool hooks_on()
{
    return g_started.load(std::memory_order_relaxed) &&
           (g_replace.load(std::memory_order_relaxed) || g_dump.load(std::memory_order_relaxed) != 0);
}

bool replacing()
{
    return g_replace.load(std::memory_order_relaxed);
}

uint64_t serial()
{
    return g_serial.load(std::memory_order_relaxed);
}

bool keep_loaded()
{
    return g_replace.load(std::memory_order_relaxed) || now_seconds() - g_offSince.load() < kKeepOff;
}

void decoded(uint64_t hash, const uint32_t *px, uint32_t w, uint32_t h, uint64_t family)
{
    if (g_dump.load(std::memory_order_relaxed) != (int)settings::TextureDump::all || g == nullptr || w == 0 || h == 0)
        return;
    const double now = now_seconds();
    {
        Hold hold;
        /* (before the dumps folder is listed a file there may be written
         * again: the same picture) */
        if (g->dumped.count(hash) != 0)
            return;
    }
    Job j;
    j.kind = kJobDump;
    j.hash = hash;
    j.w = w;
    j.h = h;
    j.px.assign(px, px + (size_t)w * h);    /* (not under the lock) */
    Hold hold;
    if (g->dumped.count(hash) != 0)
        return;
    if (family != 0) {
        /* (a family's twelfth new picture within a second: animated - left
         * out until it brings none for five.  A car's colours chosen one
         * after the other, or one model in several colours on a grid, bring
         * a few) */
        Family &f = g->families[family];
        if (now < f.mutedUntil) {
            f.mutedUntil = now + 5.0;
            return;
        }
        if (now - f.start > 1.0) {
            f.start = now;
            f.count = 0;
        }
        if (++f.count >= 12) {
            f.mutedUntil = now + 5.0;
            log_once("pgl-texrepl-animated", "textures: a texture whose palette is animated is not dumped at every"
                     " step (Select mode dumps the step shown)");
            return;
        }
        if (g->families.size() > 65536)
            g->families.clear();
    }
    if (g->dumpBytes > (192u << 20)) {
        log_once("pgl-texrepl-dumpfull", "textures: the dumps wait for the disk: some textures are dumped when they"
                 " are decoded again");
        return;
    }
    g->dumped.insert(hash);
    g->dumpBytes += j.px.size() * 4;
    g->dumpsWaiting++;
    g->hi.push_back(std::move(j));
    WakeConditionVariable(&g->wake);
}

static int lookup_locked(uint64_t hash, Found *out)
{
    if (!g->listed)
        return kPending;                    /* (the folders not listed yet: not known) */
    auto it = g->table.find(hash);
    if (it == g->table.end())
        return kNone;
    Entry &e = it->second;
    if (e.state == kEntFailed)
        return kNone;
    if (e.state == kEntReady) {
        e.asked = now_seconds();
        if (out == nullptr)
            return kReady;
        e.tex.get()->AddRef();
        e.srv.get()->AddRef();
        out->tex = e.tex.get();
        out->srv = e.srv.get();
        out->w = e.w;
        out->h = e.h;
        out->mean = e.mean;
        out->aMin = e.aMin;
        out->aMax = e.aMax;
        out->bytes = e.bytes;
        return kReady;
    }
    if (e.state == kEntIdle) {
        e.state = kEntQueued;
        Job j;
        j.kind = kJobLoad;
        j.hash = hash;
        j.src = e.src;
        j.version = e.version;
        g->waiting++;
        g->hi.push_back(std::move(j));
        WakeConditionVariable(&g->wake);
    }
    return kPending;
}

int lookup(uint64_t hash, Found *out)
{
    if (g == nullptr || !g_replace.load(std::memory_order_relaxed))
        return kNone;
    Hold hold;
    return lookup_locked(hash, out);
}

/* The folders' new list against the table (the executor: it lets go of the
 * textures of the files that changed): out, the hashes whose replacement is
 * another now - a file changed (its size or time), new, gone, or refused
 * before (tried again). */
void take_changed(std::vector<uint64_t> &out)
{
    out.clear();
    if (!g_pending.load(std::memory_order_relaxed))
        return;
    uint32_t changed = 0, added = 0, gone = 0;
    {
        Hold hold;
        std::unordered_map<uint64_t, Source> all;
        all.swap(g->pending);
        g_pending = false;
        for (auto it = g->table.begin(); it != g->table.end();) {
            auto n = all.find(it->first);
            if (n == all.end()) {
                out.push_back(it->first);
                gone++;
                it = g->table.erase(it);    /* (its texture let go here, on the executor's thread) */
                continue;
            }
            Entry &e = it->second;
            const bool differs = n->second.path != e.src.path || n->second.size != e.src.size ||
                                 n->second.time != e.src.time;
            if (differs || e.state == kEntFailed) {
                e.src = std::move(n->second);
                e.version++;
                e.tex.reset();
                e.srv.reset();
                e.state = kEntIdle;
                e.cached = false;
                out.push_back(it->first);
                changed += differs;
            }
            all.erase(n);
            ++it;
        }
        for (auto &n : all) {
            g->table[n.first].src = std::move(n.second);
            out.push_back(n.first);
            added++;
        }
        g->files = (uint32_t)g->table.size();
        g->failed = 0;
        g->refreshing = false;
        g->refreshes++;
        g->refChanged = changed;
        g->refNew = added;
        g->refGone = gone;
    }
    g_refreshSerial++;
    pclog::write("textures: refreshed: %u files changed, %u new, %u gone (%u in all)", changed, added, gone,
                 (unsigned)g->files);
}

void ask(uint64_t hash)
{
    lookup(hash, nullptr);
}

void take_loaded(std::vector<uint64_t> &out)
{
    out.clear();
    if (g_loadedCount.load(std::memory_order_relaxed) == 0)
        return;
    Hold hold;
    out.swap(g->loaded);
    g_loadedCount = 0;
}

size_t budget_bytes()
{
    static size_t budget = 0;
    if (budget == 0) {
        uint64_t vram = 0;
        IDXGIDevice *dx = nullptr;
        if (SUCCEEDED(dev()->QueryInterface(IID_PPV_ARGS(&dx)))) {
            IDXGIAdapter *ad = nullptr;
            if (SUCCEEDED(dx->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC d;
                if (SUCCEEDED(ad->GetDesc(&d)))
                    vram = d.DedicatedVideoMemory;
                ad->Release();
            }
            dx->Release();
        }
        /* half the card's memory, 512 MB to 3 GB */
        const uint64_t half = vram / 2;
        budget = (size_t)std::min<uint64_t>(std::max<uint64_t>(half, 512ull << 20), 3072ull << 20);
        pclog::write("textures: up to %u MB of replacements kept loaded", (unsigned)(budget >> 20));
    }
    return budget;
}

void housekeep(double)
{
    if (g == nullptr)
        return;
    const double now = now_seconds();
    Hold hold;
    size_t kept = 0;
    for (size_t i = 0; i < g->ready.size(); i++) {
        auto it = g->table.find(g->ready[i]);
        if (it == g->table.end())
            continue;
        Entry &e = it->second;
        if (e.state != kEntReady)
            continue;                       /* (changed since: refresh) */
        if (now - e.asked > 30.0) {
            /* (the textures bound keep their own references) */
            e.tex.reset();
            e.srv.reset();
            e.state = kEntIdle;
            continue;
        }
        g->ready[kept++] = g->ready[i];
    }
    g->ready.resize(kept);
}

uint64_t selected()
{
    return g_selected.load(std::memory_order_relaxed);
}

void selected_pixels(uint64_t hash, const uint32_t *px, uint32_t w, uint32_t h)
{
    if (g == nullptr)
        return;
    Hold hold;
    if (hash != g->sel)
        return;
    g->selPx.assign(px, px + (size_t)w * h);
    g->selPxHash = hash;
    g->selW = w;
    g->selH = h;
}

bool list_wanted()
{
    return g_menuOpen.load(std::memory_order_relaxed) &&
           g_dump.load(std::memory_order_relaxed) == (int)settings::TextureDump::select;
}

void publish(std::vector<Seen> &list)
{
    if (g == nullptr)
        return;
    Hold hold;
    g->list.swap(list);
}

/* ======================================================================== */
/* The interface's side                                                     */
/* ======================================================================== */

void menu_open(bool open)
{
    if (g == nullptr || g_menuOpen.exchange(open) == open)
        return;
    {
        Hold hold;
        update_selected();
    }
    g_serial++;
}

void settings_changed()
{
    if (g == nullptr)
        return;
    const bool wasCache = g_precache.load();
    take_settings();
    {
        Hold hold;
        update_selected();
        if (!wasCache && g_precache.load() && g->listed)
            queue_cache_jobs();
    }
    g_serial++;
}

void select_step(int dir)
{
    if (g == nullptr)
        return;
    {
        Hold hold;
        const size_t n = g->list.size();
        if (n == 0) {
            g->sel = 0;
        } else {
            size_t i = (size_t)(std::lower_bound(g->list.begin(), g->list.end(), g->sel,
                                                 [](const Seen &s, uint64_t h) { return s.hash < h; }) -
                                g->list.begin());
            const bool there = i < n && g->list[i].hash == g->sel;
            if (g->sel == 0)
                i = dir > 0 ? 0 : n - 1;
            else if (dir > 0)
                i = there ? (i + 1) % n : i % n;
            else
                i = (i + n - 1) % n;
            g->sel = g->list[i].hash;
        }
        update_selected();
    }
    g_serial++;
}

/* The folders listed again and the files that changed loaded anew (the
 * interface's REFRESH, the TextureRefresh hotkey): false with the pre-cache
 * on - a quick loop of edit and look needs none, and its files would be
 * made for every save. */
bool refresh()
{
    if (g == nullptr || g_precache.load())
        return false;
    Hold hold;
    if (g->refreshing || !g->listed)
        return true;                        /* (one runs already) */
    g->refreshing = true;
    Job j;
    j.kind = kJobIndex;
    j.refresh = true;
    g->hi.push_front(std::move(j));
    WakeConditionVariable(&g->wake);
    return true;
}

uint32_t refresh_serial()
{
    return g_refreshSerial.load(std::memory_order_relaxed);
}

bool dump_selected()
{
    if (g == nullptr)
        return false;
    Hold hold;
    if (g->sel == 0 || g->selPxHash != g->sel || g->selPx.empty())
        return false;
    Job j;
    j.kind = kJobDump;
    j.selected = true;
    j.hash = g->sel;
    j.w = g->selW;
    j.h = g->selH;
    j.px = g->selPx;
    g->dumpBytes += j.px.size() * 4;
    g->dumpsWaiting++;
    g->dumpedSelected.insert(j.hash);
    g->hi.push_back(std::move(j));
    WakeConditionVariable(&g->wake);
    return true;
}

void status(Status *out)
{
    *out = Status();
    if (g == nullptr)
        return;
    Hold hold;
    out->listed = g->listed;
    out->folders = (uint32_t)g->packs.size();
    out->files = g->files;
    out->loaded = (uint32_t)g->ready.size();
    out->waiting = g->waiting;
    out->failed = g->failed;
    out->dumped = g->dumpedNow;
    out->dumpsWaiting = g->dumpsWaiting;
    out->cacheMade = g->cacheMade;
    out->cacheTodo = g->cacheTodo;
    out->refreshing = g->refreshing;
    out->refreshes = g->refreshes;
    out->refChanged = g->refChanged;
    out->refNew = g->refNew;
    out->refGone = g->refGone;
    out->seen = (uint32_t)g->list.size();
    out->selHash = g->sel;
    if (g->sel != 0) {
        const auto it = std::lower_bound(g->list.begin(), g->list.end(), g->sel,
                                         [](const Seen &s, uint64_t h) { return s.hash < h; });
        if (it != g->list.end() && it->hash == g->sel) {
            out->selIndex = (int)(it - g->list.begin());
            out->selW = it->w;
            out->selH = it->h;
            out->selReplaced = it->replaced;
        }
        out->selDumped = g->dumpedSelected.count(g->sel) != 0;
        out->selPixels = g->selPxHash == g->sel && !g->selPx.empty();
        if (out->selIndex < 0 && out->selPixels) {
            out->selW = (uint16_t)g->selW;
            out->selH = (uint16_t)g->selH;
        }
    }
}

} /* namespace texrepl */
} /* namespace pglr */
