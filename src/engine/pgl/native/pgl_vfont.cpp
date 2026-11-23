/* pgl_vfont.cpp - PC (ROADMAP 36, no original): vector fonts (pgl_vfont.h;
 * docs/fonts.md).
 *
 * A vector face (VFace) stands beside each face of a loaded font set, keyed
 * by the set's path and the face's name (a set loaded again finds its faces'
 * again).  Its font (a FontFile: DirectWrite's face of the file; off Windows
 * stb_truetype's) is scaled so
 * that the reference character - "H", else "0", else "1" - is as tall as the
 * original's white fill, measured from the original's palette #1 (its
 * coverage, decoded with gstex), on the same baseline.
 *
 * A glyph is laid out when first asked for: its outline (DirectWrite's, in
 * ems, flattened), its cell (the outline's bounds at the face's scale and the
 * outline of one GS pixel and one transparent pixel around), a place on a
 * shelf of one of the face's pages.  It is rasterized into the page's
 * palette pictures that are in use (a palette is made when first selected):
 * the coverage of the outline (signed area accumulation, non-zero), for #0
 * the black outline around it (the distance to the fill, a 2-pass exact
 * Euclidean distance transform, its edge one pixel soft), white composited
 * over it.  Pictures are at the output scale in quarter steps; a new scale
 * rasterizes every glyph again.
 *
 * The digits and the time display's dash (0x7F where the original has a
 * glyph: its copy of the en dash, a digit wide) are tab glyphs: the face's
 * widest digit is their cell, each in its middle, never kerned; drawn, the
 * cell is widened to whole output pixels (pgl_vfont_advance).
 *
 * The game's structures (G001_flare.h FontSet, PglFont, FontGlyph) are read
 * through mirrors of their 32-bit layout. */
#include "engine/pgl/native/pgl_vfont.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite_1.h>
#else
/* the fonts read by stb_truetype (third_party_src/stb), its code in this file
 * only; the JIS codes through EUC-JP (code page 20932's table) */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"
#include "platform/os/eucjp.h"
#include "platform/os/fs.h"
#endif

#include "engine/pgl/native/gs_tex1.h"
#include "engine/pgl/native/gs_tex_space.h"
#include "engine/pgl/native/pgl_bridge.h"
#include "platform/host_file.h"
#include "platform/log.h"

namespace {

/* ======================================================================== */
/* The game's structures                                                    */
/* ======================================================================== */

static_assert(sizeof(void *) == 4, "the game's structures are 32-bit");

struct OrigGlyph {                  /* FontGlyph */
    int8_t   bearingX, bearingY;
    uint8_t  w, h, advance, u;
    uint16_t v;
    const int8_t *left, *right;
};
struct OrigFace {                   /* PglFont */
    const char      *name;
    const OrigGlyph *glyphs;
    uint16_t first, last;
    uint8_t  width, lineHeight;
    int8_t   ascent;
    uint8_t  kernRows;
    uint8_t  kanjiW, kanjiH;
    int8_t   kanjiX, kanjiY;
    uint8_t  unk14[8];
    const void *tex;                /* Tex1Image */
};
struct OrigSet {                    /* FontSet */
    uint32_t magic;
    const void *self, *unk08;
    uint32_t unk0C;
    uint16_t count, unk12;
    const OrigFace *const *fonts;
    const void *const *tex;
};
static_assert(sizeof(OrigGlyph) == 0x10 && sizeof(OrigFace) == 0x20 && sizeof(OrigSet) == 0x1C,
              "G001_flare.h's layouts");

/* ======================================================================== */
/* State                                                                    */
/* ======================================================================== */

constexpr int kPageW = 256, kPageTw = 8;    /* GS texels */
constexpr int kPageH = 1024, kPageTh = 10;
/* rows a cell may reach: UV's 14 bits end at 1023 + 15/16 texels, and a
 * sprite's far corner is the cell's end + 1/2 */
constexpr int kPageRows = 1022;
constexpr int kRowStep = 32;                /* the pictures grow by this many GS rows */
constexpr float kOutline = 1.0f;            /* GS pixels: the originals' black outline (palette #0) */
constexpr float kFlatTol = 1.0f / 2048.0f;  /* ems: the flattening's tolerance */

struct Outline {
    std::vector<float> e;           /* edges x0 y0 x1 y1, ems, y down from the baseline */
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty = true;
};

#ifndef _WIN32
/* DWRITE_FONT_METRICS' fields used here, in design units */
struct FontMetrics {
    uint16_t designUnitsPerEm = 0;
    uint16_t ascent = 0, descent = 0;
    int16_t capHeight = 0;
};
#endif

struct FontFile {
    std::string path;               /* the host path (the log) */
#ifdef _WIN32
    IDWriteFontFace *face = nullptr;
    IDWriteFontFace1 *face1 = nullptr;
    bool kerns = false;
    DWRITE_FONT_METRICS m = {};
#else
    std::vector<unsigned char> data;        /* the file (stb_truetype reads it in place) */
    stbtt_fontinfo info = {};
    bool kerns = false;
    FontMetrics m;
#endif
    std::unordered_map<uint16_t, Outline> outlines;
    std::unordered_map<uint32_t, float> kern;   /* (a << 16 | b) -> ems */
};

struct Page {
    PglrHostImage img[2];
    std::vector<uint32_t> px[2];
    uint64_t tex0[2] = {0, 0};
    bool on[2] = {false, false};
    int shelfX = 0, shelfY = 0, shelfH = 0;
    int rows = 0;                   /* GS rows the pictures hold */
};

struct VGlyph {
    FontFile *font = nullptr;
    uint16_t index = 0;
    float emPx = 0;                 /* GS pixels per em */
    float ox = 0, oy = 0;           /* the outline's origin from the pen (x, the baseline) */
    int cx0 = 0, cy0 = 0, cw = 0, ch = 0;   /* the cell from the pen; cw 0: nothing drawn */
    float advance = 0;
    int page = -1, u = 0, v = 0;
    bool tab = false;               /* a digit or the time's dash: advance the face's tabW */
};

struct VFace {
    std::string key, name;
    FontFile *latin = nullptr;
    float emPx = 0, baseY = 0;      /* the font's scale and baseline (latin) */
    float tabW = 0;                 /* the digits' cell: the font's widest digit (0: it has none) */
    bool timeDash = false;          /* the original has a glyph at kTimeDash */
    int kx = 0, ky = 0, kw = 0, kh = 0;     /* the kanji cell */
    std::vector<std::unique_ptr<Page>> pages;
    std::vector<VGlyph> glyphs;     /* id - 1 */
    std::unordered_map<uint32_t, uint32_t> byCode;  /* code (kanji: kKanjiKey | JIS) -> id, 0: none */
    int palette = 0;
    bool paletteOn[2] = {false, false};
};
constexpr uint32_t kKanjiKey = 0x10000u;
constexpr int kTimeDash = 0x7F;             /* race_format_time's "-'--.---" */

#ifdef _WIN32
IDWriteFactory *g_dw;
bool g_dwFailed;
#endif
std::unordered_map<std::string, std::unique_ptr<FontFile>> g_files;     /* host path (lower) -> font, null: unusable */
std::unordered_map<std::string, std::unique_ptr<VFace>> g_faces;        /* "path|face" */
std::unordered_map<const void *, VFace *> g_byFont;                     /* PglFont -> its vector side */
const void *g_lastFont;
VFace *g_lastFace;
FontFile *g_kanji;                  /* the kanji font file's replacement */
int g_scaleQ;                       /* the pictures' scale, quarter native pixels per GS pixel (0: not yet) */

float page_scale() { return (float)g_scaleQ * 0.25f; }

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return s;
}

/* "font/system.fnt" -> "font/system/" */
std::string folder_of(const char *path)
{
    std::string p = path;
    const size_t slash = p.find_last_of("/\\");
    const size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p.resize(dot);
    return p + "/";
}

#ifdef _WIN32
bool dw_init()
{
    if (g_dw != nullptr)
        return true;
    if (g_dwFailed)
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&g_dw)))) {
        g_dwFailed = true;
        pclog::write("PC: vector fonts: DirectWrite is not available: the bitmap fonts are drawn");
        return false;
    }
    return true;
}

/* ======================================================================== */
/* Font files                                                               */
/* ======================================================================== */

FontFile *load_font(const std::string &hostPath)
{
    const std::string key = lower(hostPath);
    auto it = g_files.find(key);
    if (it != g_files.end())
        return it->second.get();
    g_files[key] = nullptr;
    if (!dw_init())
        return nullptr;
    char full[1024];
    wchar_t wide[1024];
    if (host_file_full_path(hostPath.c_str(), full, sizeof(full)) != 0 ||
        MultiByteToWideChar(CP_ACP, 0, full, -1, wide, 1024) <= 0)
        return nullptr;
    IDWriteFontFile *file = nullptr;
    IDWriteFontFace *face = nullptr;
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType;
    DWRITE_FONT_FACE_TYPE faceType;
    UINT32 faces = 0;
    if (FAILED(g_dw->CreateFontFileReference(wide, nullptr, &file)) ||
        FAILED(file->Analyze(&supported, &fileType, &faceType, &faces)) || !supported ||
        FAILED(g_dw->CreateFontFace(faceType, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &face))) {
        if (file != nullptr)
            file->Release();
        pclog::write("PC: vector font: %s is not a font DirectWrite reads: not used", hostPath.c_str());
        return nullptr;
    }
    file->Release();
    auto f = std::make_unique<FontFile>();
    f->path = hostPath;
    f->face = face;
    face->GetMetrics(&f->m);
    if (SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace1), reinterpret_cast<void **>(&f->face1))))
        f->kerns = f->face1->HasKerningPairs() != FALSE;
    if (f->m.designUnitsPerEm == 0) {
        pclog::write("PC: vector font: %s has no em: not used", hostPath.c_str());
        return nullptr;
    }
    pclog::write("PC: vector font: %s (%u units per em%s)", hostPath.c_str(), f->m.designUnitsPerEm,
                 f->kerns ? ", kerning pairs" : "");
    FontFile *r = f.get();
    g_files[key] = std::move(f);
    return r;
}

#else /* !_WIN32 */

/* ======================================================================== */
/* Font files                                                               */
/* ======================================================================== */

/* OS/2's sCapHeight (version 2 and later), else 0. */
int16_t os2_cap_height(const stbtt_fontinfo &info)
{
    const stbtt_uint32 os2 = stbtt__find_table(info.data, (stbtt_uint32)info.fontstart, "OS/2");
    if (os2 == 0 || ttUSHORT(info.data + os2) < 2)
        return 0;
    return ttSHORT(info.data + os2 + 88);
}

FontFile *load_font(const std::string &hostPath)
{
    const std::string key = lower(hostPath);
    auto it = g_files.find(key);
    if (it != g_files.end())
        return it->second.get();
    g_files[key] = nullptr;
    char full[1024];
    std::string bytes;
    if (host_file_full_path(hostPath.c_str(), full, sizeof(full)) != 0 || !fs::read_file(full, &bytes))
        return nullptr;
    auto f = std::make_unique<FontFile>();
    f->path = hostPath;
    f->data.assign(bytes.begin(), bytes.end());
    const int offset = stbtt_GetFontOffsetForIndex(f->data.data(), 0);   /* (a .ttc: its first face) */
    if (offset < 0 || !stbtt_InitFont(&f->info, f->data.data(), offset)) {
        pclog::write("PC: vector font: %s is not a font stb_truetype reads: not used", hostPath.c_str());
        return nullptr;
    }
    const float perUnit = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
    f->m.designUnitsPerEm = perUnit > 0.0f ? (uint16_t)std::lround(1.0f / perUnit) : 0;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    f->m.ascent = (uint16_t)std::max(0, ascent);
    f->m.descent = (uint16_t)std::max(0, -descent);
    f->m.capHeight = os2_cap_height(f->info);
    f->kerns = f->info.kern != 0;           /* (the 'kern' table's pairs, as DirectWrite's) */
    if (f->m.designUnitsPerEm == 0) {
        pclog::write("PC: vector font: %s has no em: not used", hostPath.c_str());
        return nullptr;
    }
    pclog::write("PC: vector font: %s (%u units per em%s)", hostPath.c_str(), f->m.designUnitsPerEm,
                 f->kerns ? ", kerning pairs" : "");
    FontFile *r = f.get();
    g_files[key] = std::move(f);
    return r;
}

#endif /* _WIN32 */

/* <folder><name>.ttf / .otf / .ttc, the first present */
FontFile *find_font(const std::string &folder, const char *name)
{
    static const char *const kExt[] = {".ttf", ".otf", ".ttc"};
    for (const char *ext : kExt) {
        const std::string host = "host:data/" + folder + name + ext;
        if (host_file_present(host.c_str()))
            return load_font(host);
    }
    return nullptr;
}

#ifdef _WIN32
uint16_t glyph_index(FontFile *f, uint32_t ucs)
{
    UINT16 index = 0;
    const UINT32 cp = ucs;
    if (ucs == 0 || FAILED(f->face->GetGlyphIndices(&cp, 1, &index)))
        return 0;
    return index;
}

float glyph_advance(FontFile *f, uint16_t index)
{
    DWRITE_GLYPH_METRICS gm = {};
    if (FAILED(f->face->GetDesignGlyphMetrics(&index, 1, &gm, FALSE)))
        return 0.0f;
    return (float)gm.advanceWidth / (float)f->m.designUnitsPerEm;
}

/* A glyph's outline as edges, its curves flattened. */
class Flatten : public ID2D1SimplifiedGeometrySink {
public:
    explicit Flatten(Outline &o) : o_(o) {}
    STDMETHOD(QueryInterface)(REFIID iid, void **pp) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID2D1SimplifiedGeometrySink)) {
            *pp = this;
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE) override {}
    STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT) override {}
    STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override
    {
        start_ = cur_ = p;
        open_ = true;
    }
    STDMETHOD_(void, AddLines)(const D2D1_POINT_2F *pts, UINT32 n) override
    {
        for (UINT32 i = 0; i < n; i++)
            line(pts[i]);
    }
    STDMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT *b, UINT32 n) override
    {
        for (UINT32 i = 0; i < n; i++) {
            const D2D1_POINT_2F p0 = cur_, p1 = b[i].point1, p2 = b[i].point2, p3 = b[i].point3;
            const float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
            const float bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
            const float dd = 6.0f * std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));
            const int steps = std::max(1, std::min(64, (int)std::ceil(std::sqrt(dd / (8.0f * kFlatTol)))));
            for (int s = 1; s <= steps; s++) {
                const float t = (float)s / (float)steps, u = 1.0f - t;
                const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                line(D2D1::Point2F(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                                   w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y));
            }
        }
    }
    STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END) override
    {
        if (open_)
            line(start_);                   /* (filled: closed either way) */
        open_ = false;
    }
    STDMETHOD(Close)() override { return S_OK; }

private:
    void line(D2D1_POINT_2F p)
    {
        if (p.x != cur_.x || p.y != cur_.y) {
            const float e[4] = {cur_.x, cur_.y, p.x, p.y};
            o_.e.insert(o_.e.end(), e, e + 4);
            for (int i = 0; i < 4; i += 2) {
                if (o_.empty) {
                    o_.x0 = o_.x1 = e[i];
                    o_.y0 = o_.y1 = e[i + 1];
                    o_.empty = false;
                }
                o_.x0 = std::min(o_.x0, e[i]);
                o_.x1 = std::max(o_.x1, e[i]);
                o_.y0 = std::min(o_.y0, e[i + 1]);
                o_.y1 = std::max(o_.y1, e[i + 1]);
            }
        }
        cur_ = p;
    }
    Outline &o_;
    D2D1_POINT_2F start_ = {}, cur_ = {};
    bool open_ = false;
};

const Outline &glyph_outline(FontFile *f, uint16_t index)
{
    auto it = f->outlines.find(index);
    if (it != f->outlines.end())
        return it->second;
    Outline &o = f->outlines[index];
    Flatten sink(o);
    if (FAILED(f->face->GetGlyphRunOutline(1.0f, &index, nullptr, nullptr, 1, FALSE, FALSE, &sink))) {
        o.e.clear();
        o.empty = true;
    }
    return o;
}
#else /* !_WIN32 */

uint16_t glyph_index(FontFile *f, uint32_t ucs)
{
    if (ucs == 0)
        return 0;
    return (uint16_t)stbtt_FindGlyphIndex(&f->info, (int)ucs);
}

float glyph_advance(FontFile *f, uint16_t index)
{
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&f->info, index, &advance, &bearing);
    return (float)advance / (float)f->m.designUnitsPerEm;
}

/* A glyph's outline as edges, its curves flattened (the font's units to ems,
 * y down, as DirectWrite's outline at an em of 1). */
class Flatten {
public:
    explicit Flatten(Outline &o) : o_(o) {}
    void begin(float x, float y)
    {
        end();
        start_ = cur_ = {x, y};
        open_ = true;
    }
    void quad(float cx, float cy, float x, float y)
    {
        const Pt p0 = cur_, p1 = {cx, cy}, p2 = {x, y};
        const float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
        const float dd = 2.0f * std::sqrt(ax * ax + ay * ay);
        const int steps = std::max(1, std::min(64, (int)std::ceil(std::sqrt(dd / (8.0f * kFlatTol)))));
        for (int s = 1; s <= steps; s++) {
            const float t = (float)s / (float)steps, u = 1.0f - t;
            line({u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y});
        }
    }
    void cubic(float c1x, float c1y, float c2x, float c2y, float x, float y)
    {
        const Pt p0 = cur_, p1 = {c1x, c1y}, p2 = {c2x, c2y}, p3 = {x, y};
        const float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
        const float bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
        const float dd = 6.0f * std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));
        const int steps = std::max(1, std::min(64, (int)std::ceil(std::sqrt(dd / (8.0f * kFlatTol)))));
        for (int s = 1; s <= steps; s++) {
            const float t = (float)s / (float)steps, u = 1.0f - t;
            const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
            line({w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x, w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y});
        }
    }
    void to(float x, float y) { line({x, y}); }
    void end()
    {
        if (open_)
            line(start_);                   /* (filled: closed either way) */
        open_ = false;
    }

private:
    struct Pt {
        float x, y;
    };
    void line(Pt p)
    {
        if (p.x != cur_.x || p.y != cur_.y) {
            const float e[4] = {cur_.x, cur_.y, p.x, p.y};
            o_.e.insert(o_.e.end(), e, e + 4);
            for (int i = 0; i < 4; i += 2) {
                if (o_.empty) {
                    o_.x0 = o_.x1 = e[i];
                    o_.y0 = o_.y1 = e[i + 1];
                    o_.empty = false;
                }
                o_.x0 = std::min(o_.x0, e[i]);
                o_.x1 = std::max(o_.x1, e[i]);
                o_.y0 = std::min(o_.y0, e[i + 1]);
                o_.y1 = std::max(o_.y1, e[i + 1]);
            }
        }
        cur_ = p;
    }
    Outline &o_;
    Pt start_ = {0, 0}, cur_ = {0, 0};
    bool open_ = false;
};

const Outline &glyph_outline(FontFile *f, uint16_t index)
{
    auto it = f->outlines.find(index);
    if (it != f->outlines.end())
        return it->second;
    Outline &o = f->outlines[index];
    stbtt_vertex *v = nullptr;
    const int n = stbtt_GetGlyphShape(&f->info, index, &v);
    const float k = 1.0f / (float)f->m.designUnitsPerEm;
    Flatten sink(o);
    for (int i = 0; i < n; i++) {
        const float x = v[i].x * k, y = -v[i].y * k;
        switch (v[i].type) {
        case STBTT_vmove:
            sink.begin(x, y);
            break;
        case STBTT_vline:
            sink.to(x, y);
            break;
        case STBTT_vcurve:
            sink.quad(v[i].cx * k, -v[i].cy * k, x, y);
            break;
        case STBTT_vcubic:
            sink.cubic(v[i].cx * k, -v[i].cy * k, v[i].cx1 * k, -v[i].cy1 * k, x, y);
            break;
        }
    }
    sink.end();
    stbtt_FreeShape(&f->info, v);
    return o;
}
#endif /* _WIN32 */

/* ======================================================================== */
/* Rasterizing                                                              */
/* ======================================================================== */

/* One edge into the area accumulation buffer (w x h, + 2): the signed area
 * it covers in each pixel, the coverage then the running sum along the rows. */
void raster_line(float *acc, int w, int h, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    float dir = 1.0f;
    if (y0 > y1) {
        dir = -1.0f;
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    const float xmax = (float)w - 1.001f;
    x0 = std::max(0.0f, std::min(xmax, x0));
    x1 = std::max(0.0f, std::min(xmax, x1));
    const float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0.0f)
        x -= y0 * dxdy;
    const int yEnd = std::min(h, (int)std::ceil(y1));
    for (int y = std::max(0, (int)y0); y < yEnd; y++) {
        const int row = y * w;
        const float dy = std::min((float)(y + 1), y1) - std::max((float)y, y0);
        const float xnext = x + dxdy * dy;
        const float d = dy * dir;
        float xa = x, xb = xnext;
        if (xa > xb)
            std::swap(xa, xb);
        const float xaf = std::floor(xa);
        const int xai = (int)xaf;
        const float xbc = std::ceil(xb);
        const int xbi = (int)xbc;
        if (xbi <= xai + 1) {
            const float xmf = 0.5f * (x + xnext) - xaf;
            acc[row + xai] += d - d * xmf;
            acc[row + xai + 1] += d * xmf;
        } else {
            const float s = 1.0f / (xb - xa);
            const float x0f = xa - xaf;
            const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
            const float x1f = xb - xbc + 1.0f;
            const float am = 0.5f * s * x1f * x1f;
            acc[row + xai] += d * a0;
            if (xbi == xai + 2) {
                acc[row + xai + 1] += d * (1.0f - a0 - am);
            } else {
                const float a1 = s * (1.5f - x0f);
                acc[row + xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++)
                    acc[row + xi] += d * s;
                const float a2 = a1 + (float)(xbi - xai - 3) * s;
                acc[row + xbi - 1] += d * (1.0f - a2 - am);
            }
            acc[row + xbi] += d * am;
        }
        x = xnext;
    }
}

/* The squared distances of f (0 inside, kFar outside) along one line
 * (Felzenszwalb & Huttenlocher's lower envelope of parabolas), n samples at
 * stride. */
constexpr float kFar = 1e20f;
std::vector<float> g_edtF, g_edtZ;
std::vector<int> g_edtV;
void edt_line(float *f, int n, int stride)
{
    const float inf = HUGE_VALF;
    g_edtF.resize((size_t)n);
    g_edtV.resize((size_t)n);
    g_edtZ.resize((size_t)n + 1);
    float *t = g_edtF.data(), *z = g_edtZ.data();
    int *v = g_edtV.data();
    for (int i = 0; i < n; i++)
        t[i] = f[i * stride];
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; q++) {
        float s;
        for (;;) {
            s = ((t[q] + (float)q * q) - (t[v[k]] + (float)v[k] * v[k])) / (float)(2 * q - 2 * v[k]);
            if (s > z[k])
                break;
            k--;                            /* (z[0] is -inf: k stays >= 0) */
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (float)q)
            k++;
        const float d = (float)(q - v[k]);
        f[q * stride] = d * d + t[v[k]];
    }
}

std::vector<float> g_cover, g_ring;         /* (raster_glyph's) */

/* Writes glyph g of face vf into its page's pictures of the palettes in
 * mask (bit p: palette p, those that are on). */
void raster_glyph(VFace &vf, const VGlyph &g, int mask)
{
    if (g.cw <= 0 || g.page < 0 || g.font == nullptr)
        return;
    Page &pg = *vf.pages[(size_t)g.page];
    for (int p = 0; p < 2; p++)
        if (!pg.on[p])
            mask &= ~(1 << p);
    if (mask == 0)
        return;
    const float s = page_scale();
    const int n0x = (int)std::ceil((float)g.u * s - 0.5f), n1x = (int)std::ceil((float)(g.u + g.cw) * s - 0.5f);
    const int n0y = (int)std::ceil((float)g.v * s - 0.5f), n1y = (int)std::ceil((float)(g.v + g.ch) * s - 0.5f);
    const int w = n1x - n0x, h = n1y - n0y;
    if (w <= 0 || h <= 0)
        return;
    const Outline &ol = glyph_outline(g.font, g.index);
    std::vector<float> &acc = g_cover, &out = g_ring;
    acc.assign((size_t)w * h + 2, 0.0f);
    const float bx = ((float)(g.u - g.cx0) + g.ox) * s - (float)n0x;
    const float by = ((float)(g.v - g.cy0) + g.oy) * s - (float)n0y;
    const float k = g.emPx * s;
    for (size_t i = 0; i + 3 < ol.e.size(); i += 4)
        raster_line(acc.data(), w, h, bx + ol.e[i] * k, by + ol.e[i + 1] * k, bx + ol.e[i + 2] * k,
                    by + ol.e[i + 3] * k);
    float sum = 0.0f;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        sum += acc[i];
        acc[i] = std::min(1.0f, std::fabs(sum));
    }
    /* palette #0's outline: the distance to the fill */
    if (mask & 1) {
        out.resize((size_t)w * h);
        for (size_t i = 0; i < out.size(); i++)
            out[i] = acc[i] >= 0.5f ? 0.0f : kFar;
        for (int x = 0; x < w; x++)
            edt_line(&out[(size_t)x], h, w);
        for (int y = 0; y < h; y++)
            edt_line(&out[(size_t)y * w], w, 1);
        const float r = kOutline * s;
        for (size_t i = 0; i < out.size(); i++)
            out[i] = out[i] <= 0.0f ? 1.0f : std::max(0.0f, std::min(1.0f, r + 1.0f - std::sqrt(out[i])));
    }
    for (int p = 0; p < 2; p++) {
        if (!(mask & (1 << p)))
            continue;
        PglrHostImage &im = pg.img[p];
        pglr_host_image_lock(&im);
        const int y0 = std::max(0, n0y), y1 = std::min(im.h, n1y);
        const int x0 = std::max(0, n0x), x1 = std::min(im.w, n1x);
        for (int y = y0; y < y1; y++) {
            uint32_t *row = &pg.px[p][(size_t)y * im.w];
            const size_t at = (size_t)(y - n0y) * w;
            if (p == 1) {
                for (int x = x0; x < x1; x++) {
                    const float c = acc[at + (size_t)(x - n0x)];
                    row[x] = ((uint32_t)std::lround(c * 128.0f) << 24) | 0xFFFFFFu;
                }
            } else {
                for (int x = x0; x < x1; x++) {
                    const float c = acc[at + (size_t)(x - n0x)], o = out[at + (size_t)(x - n0x)];
                    const float a = c + o * (1.0f - c);
                    const uint32_t v = a > 0.0f ? (uint32_t)std::lround(255.0f * c / a) : 0u;
                    row[x] = ((uint32_t)std::lround(a * 128.0f) << 24) | v | (v << 8) | (v << 16);
                }
            }
        }
        if (y1 > y0) {
            if (im.dirtyY1 > im.dirtyY0) {
                im.dirtyY0 = std::min(im.dirtyY0, y0);
                im.dirtyY1 = std::max(im.dirtyY1, y1);
            } else {
                im.dirtyY0 = y0;
                im.dirtyY1 = y1;
            }
        }
        pglr_host_image_unlock(&im);
    }
}

/* ======================================================================== */
/* Pages                                                                    */
/* ======================================================================== */

uint32_t clear_pixel(int palette) { return palette == 1 ? 0x00FFFFFFu : 0u; }

/* The picture of palette p sized for the page's rows at the scale (all of
 * it new, cleared). */
void page_picture(Page &pg, int p)
{
    const float s = page_scale();
    PglrHostImage &im = pg.img[p];
    pglr_host_image_lock(&im);
    im.gsW = kPageW;
    im.gsH = pg.rows;
    im.w = (int)std::lround((float)kPageW * s);
    im.h = (int)std::lround((float)pg.rows * s);
    pg.px[p].assign((size_t)im.w * im.h, clear_pixel(p));
    im.pixels = pg.px[p].data();
    im.generation++;
    im.dirtyY0 = im.dirtyY1 = 0;
    pglr_host_image_unlock(&im);
}

/* Palette p of the page in use (its host image registered). */
bool page_palette_on(Page &pg, int p)
{
    if (pg.on[p])
        return true;
    PglrHostImage &im = pg.img[p];
    memset(&im, 0, sizeof(im));
    im.id = -1;
    pg.tex0[p] = pglr_host_image_add(&im, kPageTw, kPageTh);
    if (pg.tex0[p] == 0)
        return false;
    pg.on[p] = true;
    page_picture(pg, p);
    return true;
}

/* The page's pictures hold `rows' GS rows at least (kRowStep steps; what
 * they hold is kept). */
void page_grow(Page &pg, int rows)
{
    const int want = std::min(kPageH, (rows + kRowStep - 1) / kRowStep * kRowStep);
    if (want <= pg.rows)
        return;
    pg.rows = want;
    const float s = page_scale();
    for (int p = 0; p < 2; p++) {
        if (!pg.on[p])
            continue;
        PglrHostImage &im = pg.img[p];
        pglr_host_image_lock(&im);
        im.gsH = pg.rows;
        im.h = (int)std::lround((float)pg.rows * s);
        pg.px[p].resize((size_t)im.w * im.h, clear_pixel(p));
        im.pixels = pg.px[p].data();
        im.generation++;
        im.dirtyY0 = im.dirtyY1 = 0;
        pglr_host_image_unlock(&im);
    }
}

/* A place for g's cell on a shelf of the face's last page, else a new page;
 * false when it is larger than a page. */
bool place(VFace &vf, VGlyph &g)
{
    if (g.cw > kPageW || g.ch > kPageRows)
        return false;
    Page *pg = vf.pages.empty() ? nullptr : vf.pages.back().get();
    if (pg != nullptr && pg->shelfX + g.cw > kPageW) {
        pg->shelfY += pg->shelfH;
        pg->shelfX = 0;
        pg->shelfH = 0;
    }
    if (pg == nullptr || pg->shelfY + g.ch > kPageRows) {
        vf.pages.push_back(std::make_unique<Page>());
        pg = vf.pages.back().get();
        for (int p = 0; p < 2; p++)
            if (vf.paletteOn[p])
                page_palette_on(*pg, p);
    }
    g.page = (int)vf.pages.size() - 1;
    g.u = pg->shelfX;
    g.v = pg->shelfY;
    pg->shelfX += g.cw;
    pg->shelfH = std::max(pg->shelfH, g.ch);
    page_grow(*pg, pg->shelfY + pg->shelfH);
    return true;
}

/* The pictures at the output scale (quarter steps, 1 .. 16): a new scale
 * makes every face's pictures again and rasterizes their glyphs. */
void check_scale()
{
    const float s = pglr_text_scale();
    const int q = std::max(4, std::min(64, (int)std::ceil(s * 4.0f - 0.01f)));
    if (q == g_scaleQ)
        return;
    g_scaleQ = q;
    size_t glyphs = 0;
    for (auto &kv : g_faces) {
        VFace &vf = *kv.second;
        for (auto &pg : vf.pages)
            for (int p = 0; p < 2; p++)
                if (pg->on[p])
                    page_picture(*pg, p);
        for (const VGlyph &g : vf.glyphs) {
            raster_glyph(vf, g, 3);
            glyphs++;
        }
    }
    pclog::write("PC: vector fonts: rasterized at %.2f pixels per GS pixel (%u glyphs)", page_scale(),
                 (unsigned)glyphs);
}

/* ======================================================================== */
/* Faces and glyphs                                                         */
/* ======================================================================== */

/* The original's reference glyph: the top and bottom of its white fill (GS
 * pixels from the pen), from palette #1's coverage. */
bool reference_fill(const OrigFace &f, int code, float &top, float &bottom)
{
    const OrigGlyph &g = f.glyphs[code - f.first];
    gstex::Tex1Set set;
    if (f.tex == nullptr || !gstex::tex1_from_memory(f.tex, set) || set.tex.size() < 2)
        return false;
    gstex::TexSpace space;
    gstex::tex1_layout(set, space);
    gstex::DecodedTexture d;
    gstex::Texa texa;
    if (!gstex::tex1_decode(set, space, 1, 0, texa, d, true))
        return false;
    int firstRow = -1, lastRow = -1;
    float firstCov = 0, lastCov = 0;
    for (int y = 0; y < g.h; y++) {
        uint32_t m = 0;
        for (int x = 0; x < g.w; x++) {
            const int px = g.u + x - (int)d.rect.x, py = g.v + y - (int)d.rect.y;
            if (px < 0 || py < 0 || px >= (int)d.rect.w || py >= (int)d.rect.h)
                continue;
            m = std::max(m, d.rgba[(size_t)py * d.rect.w + px] >> 24);
        }
        if (m > 0) {
            if (firstRow < 0) {
                firstRow = y;
                firstCov = std::min(1.0f, (float)m / 128.0f);
            }
            lastRow = y;
            lastCov = std::min(1.0f, (float)m / 128.0f);
        }
    }
    if (firstRow < 0 || lastRow <= firstRow)
        return false;
    top = (float)(g.bearingY + firstRow + 1) - firstCov;
    bottom = (float)(g.bearingY + lastRow) + lastCov;
    return true;
}

/* The face's font scaled to the original: emPx and baseY. */
void setup_latin(VFace &vf, const OrigFace &f)
{
    static const char kRefs[] = {'H', '0', '1'};
    int ref = 0;
    for (char c : kRefs)
        if (c >= f.first && c <= f.last && f.glyphs[c - f.first].w != 0) {
            ref = c;
            break;
        }
    float top = 0, bottom = 0;
    bool measured = ref != 0 && reference_fill(f, ref, top, bottom);
    if (!measured) {
        /* (the cell less the outline, else the ascent) */
        if (ref != 0) {
            const OrigGlyph &g = f.glyphs[ref - f.first];
            top = (float)g.bearingY + kOutline;
            bottom = (float)(g.bearingY + g.h) - kOutline;
        } else {
            top = -(float)f.ascent + kOutline;
            bottom = 0.0f;
        }
    }
    FontFile *font = vf.latin;
    const float upem = (float)font->m.designUnitsPerEm;
    float t = -(float)font->m.capHeight / upem, b = 0.0f;
    const uint16_t index = ref != 0 ? glyph_index(font, (uint32_t)ref) : 0;
    if (index != 0) {
        const Outline &o = glyph_outline(font, index);
        if (!o.empty && o.y1 > o.y0) {
            t = o.y0;
            b = o.y1;
        }
    }
    if (!(b > t))
        t = -0.7f, b = 0.0f;
    vf.emPx = (bottom - top) / (b - t);
    vf.baseY = bottom - b * vf.emPx;
    /* the digits' cell: the widest digit of the font */
    vf.tabW = 0.0f;
    for (uint32_t c = '0'; c <= '9'; c++) {
        const uint16_t d = glyph_index(font, c);
        if (d != 0)
            vf.tabW = std::max(vf.tabW, glyph_advance(font, d) * vf.emPx);
    }
    pclog::write("PC: vector font: face %s from %s: its \"%c\" %.2f GS pixels tall%s (an em %.2f, the baseline at %+.2f, "
                 "the digits %.2f wide)",
                 vf.key.c_str(), font->path.c_str(), ref != 0 ? ref : 'H', bottom - top,
                 measured ? "" : " (not measured: the cell)", vf.emPx, vf.baseY, vf.tabW);
}

/* A glyph of font laid out (and rasterized into the palettes on): its id,
 * 0 when the font lacks the character.  tab: in the middle of the digits'
 * cell, its advance. */
uint32_t make_glyph(VFace &vf, FontFile *font, uint32_t ucs, float emPx, float ox, float oy, float advanceAdd,
                    bool tab)
{
    const uint16_t index = glyph_index(font, ucs);
    if (index == 0)
        return 0;
    VGlyph g;
    g.font = font;
    g.index = index;
    g.emPx = emPx;
    g.advance = glyph_advance(font, index) * emPx + advanceAdd;
    if (tab) {
        ox += 0.5f * (vf.tabW - g.advance);
        g.advance = vf.tabW;
        g.tab = true;
    }
    g.ox = ox;
    g.oy = oy;
    const Outline &o = glyph_outline(font, index);
    if (!o.empty) {
        const float pad = kOutline + 1.0f;
        g.cx0 = (int)std::floor(ox + o.x0 * emPx - pad);
        g.cy0 = (int)std::floor(oy + o.y0 * emPx - pad);
        g.cw = (int)std::ceil(ox + o.x1 * emPx + pad) - g.cx0;
        g.ch = (int)std::ceil(oy + o.y1 * emPx + pad) - g.cy0;
        if (!place(vf, g))
            g.cw = g.ch = 0;                /* (larger than a page: not drawn) */
    }
    vf.glyphs.push_back(g);
    raster_glyph(vf, vf.glyphs.back(), 3);
    return (uint32_t)vf.glyphs.size();
}

/* Fills out from glyph id (the palette selected made on). */
int glyph_out(VFace &vf, uint32_t id, PglVGlyph *out)
{
    if (id == 0 || id > vf.glyphs.size())
        return 0;
    const VGlyph &g = vf.glyphs[id - 1];
    out->x0 = (float)g.cx0;
    out->y0 = (float)g.cy0;
    out->w = g.cw;
    out->h = g.ch;
    out->advance = g.advance;
    out->u = g.u;
    out->v = g.v;
    out->tex0 = 0;
    out->id = id;
    out->tab = g.tab ? 1 : 0;
    if (g.cw > 0) {
        Page &pg = *vf.pages[(size_t)g.page];
        if (!pg.on[vf.palette]) {
            if (!page_palette_on(pg, vf.palette))
                return 0;
            for (const VGlyph &o : vf.glyphs)
                if (o.page == g.page)
                    raster_glyph(vf, o, 1 << vf.palette);
        }
        out->tex0 = pg.tex0[vf.palette];
    }
    return 1;
}

#ifdef _WIN32
/* JIS row / cell -> Unicode (through Shift_JIS, code page 932) */
uint32_t jis_unicode(int jis)
{
    const int j1 = (jis >> 8) & 0x7F, j2 = jis & 0x7F;
    if (j1 < 0x21 || j1 > 0x7E || j2 < 0x21 || j2 > 0x7E)
        return 0;
    char sj[2];
    sj[0] = (char)(((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0));
    sj[1] = (char)(j2 + ((j1 & 1) ? (j2 >= 0x60 ? 0x20 : 0x1F) : 0x7E));
    wchar_t w = 0;
    if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sj, 2, &w, 1) != 1)
        return 0;
    return (uint32_t)w;
}

/* font_putc's code -> Unicode: 0x80..0x9F are Windows-1252's (the originals'
 * glyphs there: the euro, the quotes, the dashes, ...) */
uint32_t code_unicode(int code)
{
    if (code >= 0x80 && code <= 0x9F) {
        const char b = (char)code;
        wchar_t w = 0;
        if (MultiByteToWideChar(1252, MB_ERR_INVALID_CHARS, &b, 1, &w, 1) != 1 || w == (wchar_t)code)
            return 0;
        return (uint32_t)w;
    }
    return code > 0 && code < 0x110000 ? (uint32_t)code : 0;
}
#else /* !_WIN32 */

/* JIS row / cell -> Unicode: as code page 932 maps it (Shift_JIS, the
 * Windows build's), through EUC-JP (code page 20932's JIS X 0208) and the
 * symbols the two map differently (932's fullwidth forms).  (932's NEC and IBM
 * rows are not there.) */
uint32_t jis_unicode(int jis)
{
    const int j1 = (jis >> 8) & 0x7F, j2 = jis & 0x7F;
    if (j1 < 0x21 || j1 > 0x7E || j2 < 0x21 || j2 > 0x7E)
        return 0;
    static const struct {
        uint16_t jis;
        uint32_t ucs;
    } k932[] = {
        {0x2141, 0xFF5E}, {0x2142, 0x2225}, {0x215D, 0xFF0D}, {0x2171, 0xFFE0}, {0x2172, 0xFFE1},
        {0x224C, 0xFFE2}, {0x213D, 0x2015}, {0x2140, 0xFF3C},
    };
    for (const auto &d : k932)
        if (d.jis == ((j1 << 8) | j2))
            return d.ucs;
    const char euc[2] = {(char)(j1 | 0x80), (char)(j2 | 0x80)};
    std::u32string w;
    if (!eucjp::decode(euc, 2, &w) || w.size() != 1)
        return 0;
    return (uint32_t)w[0];
}

/* font_putc's code -> Unicode: 0x80..0x9F are Windows-1252's (the originals'
 * glyphs there: the euro, the quotes, the dashes, ...) */
uint32_t code_unicode(int code)
{
    if (code >= 0x80 && code <= 0x9F) {
        /* (0: the bytes 1252 leaves as they are) */
        static const uint16_t k1252[32] = {
            0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
            0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
        };
        return k1252[code - 0x80];
    }
    return code > 0 && code < 0x110000 ? (uint32_t)code : 0;
}
#endif /* _WIN32 */

/* The time display's dash (kTimeDash): the font's en dash, as the originals'
 * (their 0x7F is a copy of 0x96, a digit wide), else its figure dash, else
 * its hyphen; 0: none. */
uint32_t time_dash(FontFile *f)
{
    static const uint32_t kDashes[] = {0x2013, 0x2012, 0x002D};
    for (uint32_t d : kDashes)
        if (glyph_index(f, d) != 0)
            return d;
    return 0;
}

}  /* namespace */

/* ======================================================================== */
/* The interface                                                            */
/* ======================================================================== */

extern "C" void pgl_vfont_set_loaded(const char *path, const void *fontSet)
{
    const OrigSet *fs = static_cast<const OrigSet *>(fontSet);
    if (path == nullptr || fs == nullptr)
        return;
    g_lastFont = nullptr;
    const std::string folder = folder_of(path);
    for (int i = 0; i < fs->count; i++) {
        const OrigFace *f = fs->fonts[i];
        if (f == nullptr || f->name == nullptr)
            continue;
        const std::string key = std::string(path) + "|" + f->name;
        std::unique_ptr<VFace> &slot = g_faces[key];
        if (!slot) {
            slot = std::make_unique<VFace>();
            VFace &vf = *slot;
            vf.key = key;
            vf.name = f->name;
            vf.latin = find_font(folder, f->name);
            if (vf.latin == nullptr)
                vf.latin = find_font(folder, "default");
            if (vf.latin != nullptr)
                setup_latin(vf, *f);
        }
        slot->kx = f->kanjiX;
        slot->ky = f->kanjiY;
        slot->kw = f->kanjiW;
        slot->kh = f->kanjiH;
        slot->timeDash = kTimeDash >= f->first && kTimeDash <= f->last && f->glyphs[kTimeDash - f->first].w != 0;
        g_byFont[f] = slot.get();
    }
}

extern "C" void pgl_vfont_set_released(const void *fontSet)
{
    const OrigSet *fs = static_cast<const OrigSet *>(fontSet);
    if (fs == nullptr)
        return;
    g_lastFont = nullptr;
    for (int i = 0; i < fs->count; i++)
        if (fs->fonts[i] != nullptr)
            g_byFont.erase(fs->fonts[i]);
}

extern "C" void pgl_vfont_kanji_loaded(const char *path)
{
    FontFile *font = path != nullptr ? find_font(folder_of(path), "kanji") : nullptr;
    if (font == g_kanji)
        return;
    g_kanji = font;
    g_lastFont = nullptr;
    /* the kanji laid out from the previous font go (their cells stay) */
    for (auto &kv : g_faces) {
        auto &m = kv.second->byCode;
        for (auto it = m.begin(); it != m.end();)
            it = (it->first & kKanjiKey) ? m.erase(it) : std::next(it);
    }
    if (font != nullptr)
        pclog::write("PC: vector font: the kanji of %s from %s", path, font->path.c_str());
}

extern "C" void *pgl_vfont_face(const void *pglFont)
{
    if (pglFont == g_lastFont && pglFont != nullptr)
        return g_lastFace;
    VFace *vf = nullptr;
    auto it = g_byFont.find(pglFont);
    if (it != g_byFont.end() && (it->second->latin != nullptr || g_kanji != nullptr))
        vf = it->second;
    g_lastFont = pglFont;
    g_lastFace = vf;
    return vf;
}

extern "C" void pgl_vfont_palette(void *vfp, int palette)
{
    VFace *vf = static_cast<VFace *>(vfp);
    if (vf == nullptr)
        return;
    check_scale();
    const int p = palette > 0 ? 1 : 0;
    vf->palette = p;
    if (vf->paletteOn[p])
        return;
    vf->paletteOn[p] = true;
    for (auto &pg : vf->pages)
        page_palette_on(*pg, p);
    for (const VGlyph &g : vf->glyphs)
        raster_glyph(*vf, g, 1 << p);
}

extern "C" int pgl_vfont_glyph(void *vfp, int code, PglVGlyph *out)
{
    VFace *vf = static_cast<VFace *>(vfp);
    if (vf == nullptr || vf->latin == nullptr || out == nullptr)
        return 0;
    if (g_scaleQ == 0)
        check_scale();
    auto it = vf->byCode.find((uint32_t)code);
    uint32_t id;
    if (it != vf->byCode.end()) {
        id = it->second;
    } else {
        uint32_t ucs = code_unicode(code);
        bool tab = code >= '0' && code <= '9';
        if (code == kTimeDash && vf->timeDash) {
            ucs = time_dash(vf->latin);
            tab = true;
        }
        id = ucs != 0 ? make_glyph(*vf, vf->latin, ucs, vf->emPx, 0.0f, vf->baseY, 0.0f, tab && vf->tabW > 0.0f) : 0;
        vf->byCode[(uint32_t)code] = id;
    }
    return glyph_out(*vf, id, out);
}

extern "C" int pgl_vfont_kanji(void *vfp, int jis, PglVGlyph *out)
{
    VFace *vf = static_cast<VFace *>(vfp);
    if (vf == nullptr || out == nullptr)
        return 0;
    if (g_scaleQ == 0)
        check_scale();
    const uint32_t key = kKanjiKey | ((uint32_t)jis & 0x7F7F);
    auto it = vf->byCode.find(key);
    uint32_t id = 0;
    if (it != vf->byCode.end()) {
        id = it->second;
    } else {
        const uint32_t ucs = jis_unicode(jis);
        if (ucs != 0 && g_kanji != nullptr && vf->kh > 2) {
            /* in the face's kanji cell: the em the cell less its outline, the
             * baseline at the font's ascent share of it */
            const auto &m = g_kanji->m;
            const float em = (float)vf->kh - 2.0f * kOutline;
            const float asc = m.ascent + m.descent > 0 ? (float)m.ascent / (float)(m.ascent + m.descent) : 0.88f;
            id = make_glyph(*vf, g_kanji, ucs, em, (float)vf->kx + kOutline, (float)vf->ky + kOutline + em * asc,
                            (float)vf->kx + 2.0f * kOutline, false);
        }
        if (id == 0 && ucs != 0 && vf->latin != nullptr)
            id = make_glyph(*vf, vf->latin, ucs, vf->emPx, 0.0f, vf->baseY, 0.0f, false);
        vf->byCode[key] = id;
    }
    return glyph_out(*vf, id, out);
}

extern "C" float pgl_vfont_kern(void *vfp, uint32_t prev, uint32_t next)
{
    VFace *vf = static_cast<VFace *>(vfp);
    if (vf == nullptr || prev == 0 || next == 0 || prev > vf->glyphs.size() || next > vf->glyphs.size())
        return 0.0f;
    const VGlyph &a = vf->glyphs[prev - 1], &b = vf->glyphs[next - 1];
    FontFile *f = a.font;
    if (f == nullptr || f != b.font || a.emPx != b.emPx || !f->kerns || a.tab || b.tab)
        return 0.0f;
    const uint32_t key = (uint32_t)a.index << 16 | b.index;
    auto it = f->kern.find(key);
    float em;
    if (it != f->kern.end()) {
        em = it->second;
    } else {
#ifdef _WIN32
        const UINT16 pair[2] = {a.index, b.index};
        INT32 adj[2] = {0, 0};
        em = SUCCEEDED(f->face1->GetKerningPairAdjustments(2, pair, adj))
                 ? (float)adj[0] / (float)f->m.designUnitsPerEm
                 : 0.0f;
#else
        em = (float)stbtt__GetGlyphKernInfoAdvance(&f->info, a.index, b.index) / (float)f->m.designUnitsPerEm;
#endif
        f->kern[key] = em;
    }
    return em * a.emPx;
}

extern "C" float pgl_vfont_advance(const PglVGlyph *g, float xScale, float spacingX, float *shift)
{
    *shift = 0.0f;
    if (!g->tab)
        return g->advance;
    const float sx = pglr_text_scale_x(), k = xScale * sx;     /* output pixels per GS pixel of the glyph */
    if (!(k > 0.01f))
        return g->advance;
    /* the pen's move in whole output pixels, the cell's at least */
    const float move = std::ceil((g->advance * xScale + spacingX) * sx - 0.01f);
    const float advance = (move / sx - spacingX) / xScale;
    *shift = std::round(0.5f * (advance - g->advance) * k) / k;
    return advance;
}
