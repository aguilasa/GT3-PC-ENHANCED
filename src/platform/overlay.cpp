/* overlay.cpp - drawing over the picture for GT3 PC's own panels
 * (overlay.h).  The frame time monitor's drawing, shared. */
#include "platform/overlay.h"
#include "platform/overlay_font.h"                /* PC (2026-11-16): the font's tables */
#include "platform/gpu.h"
#include "platform/log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <algorithm>
#endif

#include <mutex>
#include <string.h>
#include <unordered_map>

namespace overlay {
namespace {

/* PC (2026-11-07): 65536 rectangles a frame (was 16384: the verbose frame
 * monitor's three panels - some 2,300 characters of up to ten rectangles
 * each and the graphs' 600 bars - ran out of them, and its last counts were
 * not drawn: user report).  A rectangle past the budget is dropped. */
constexpr int kMaxVertices = 6 * 65536;

bool g_ready, g_failed;
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11InputLayout *g_layout;
ID3D11Buffer *g_vb;
ID3D11BlendState *g_blend;
ID3D11RasterizerState *g_raster;
ID3D11DepthStencilState *g_depth;

struct Vertex {
    float x, y;                                 /* clip space */
    uint32_t color;
};
Vertex *g_v;                                    /* the mapped buffer while building */
int g_n;
int g_bw, g_bh;
float g_toX, g_toY;                             /* pixels to clip space */

const char kShader[] = R"(
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; };
VsOut vs_main(float2 pos : POSITION, float4 color : COLOR)
{
    VsOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.color = color;
    return o;
}
float4 ps_main(VsOut i) : SV_Target { return i.color; }
)";

bool init()
{
    ID3D11Device *dev = gpu::device();
    ID3DBlob *code = nullptr;
    g_vs = gpu::compile_vs("overlay", kShader, "vs_main", &code);
    g_ps = gpu::compile_ps("overlay", kShader, "ps_main");
    if (g_vs == nullptr || g_ps == nullptr || code == nullptr)
        return false;
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(il, 2, code->GetBufferPointer(), code->GetBufferSize(), &g_layout);
    code->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(sizeof(Vertex) * kMaxVertices);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g_vb);

    D3D11_BLEND_DESC b = {};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&b, &g_blend);

    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&r, &g_raster);

    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev->CreateDepthStencilState(&d, &g_depth);

    return g_layout && g_vb && g_blend && g_raster && g_depth;
}

/* ---- the font: platform/overlay_font.h (PC 2026-11-16: its tables moved there) */

using overlay_font::Glyph;
using overlay_font::narrow_glyph;
using overlay_font::upper;

/* A wide glyph: MS Gothic's 12 x 12 bitmap (bit 11 the left column), its
 * advance 6 or 12 font pixels; 0: the font has none (a blank cell).  Off
 * Windows (no MS Gothic, no GDI) the bitmaps are a public-domain 12 px font's,
 * built in: overlay_cjk.inc (tools/overlay_cjk/gen_overlay_cjk.py). */
struct Wide {
    uint16_t rows[12];
    int advance;
    bool drawn;
};
#ifdef _WIN32
std::mutex g_wideLock;
std::unordered_map<uint32_t, Wide> g_wide;
HDC g_wideDc;
HBITMAP g_wideBmp;
HFONT g_wideFont;
uint32_t *g_wideBits;                           /* 24 x 12, top down */
bool g_wideFailed;

const Wide &wide_glyph(uint32_t c)
{
    std::lock_guard<std::mutex> l(g_wideLock);
    auto it = g_wide.find(c);
    if (it != g_wide.end())
        return it->second;
    Wide w = {};
    w.advance = 6;
    if (!g_wideFailed && g_wideDc == nullptr) {
        g_wideDc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 24;
        bi.bmiHeader.biHeight = -12;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        g_wideBmp = g_wideDc ? CreateDIBSection(g_wideDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        g_wideBits = (uint32_t *)bits;
        g_wideFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"MS Gothic");
        if (g_wideDc == nullptr || g_wideBmp == nullptr || g_wideFont == nullptr) {
            g_wideFailed = true;
            pclog::write("overlay: no MS Gothic 12 px font (Japanese text in the panels is not drawn)");
        } else {
            SelectObject(g_wideDc, g_wideBmp);
            SelectObject(g_wideDc, g_wideFont);
            SetTextColor(g_wideDc, RGB(255, 255, 255));
            SetBkColor(g_wideDc, RGB(0, 0, 0));
            SetBkMode(g_wideDc, OPAQUE);
            SetTextAlign(g_wideDc, TA_TOP | TA_LEFT);
        }
    }
    if (!g_wideFailed && c <= 0xFFFF) {
        const wchar_t ch = (wchar_t)c;
        WORD index = 0xFFFF;
        if (GetGlyphIndicesW(g_wideDc, &ch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == 1 && index != 0xFFFF) {
            SIZE sz = {};
            GetTextExtentPoint32W(g_wideDc, &ch, 1, &sz);
            w.advance = sz.cx > 6 ? 12 : 6;
            GdiFlush();
            memset(g_wideBits, 0, 24 * 12 * 4);
            const RECT box = {0, 0, 24, 12};
            ExtTextOutW(g_wideDc, 0, 0, ETO_OPAQUE, &box, &ch, 1, nullptr);
            GdiFlush();
            for (int y = 0; y < 12; y++)
                for (int x = 0; x < 12; x++)
                    if ((g_wideBits[y * 24 + x] & 0xFFFFFF) != 0)
                        w.rows[y] |= (uint16_t)(0x800 >> x);
            w.drawn = true;
        }
    }
    return g_wide.emplace(c, w).first->second;
}

#else /* !_WIN32 */

std::mutex g_wideLock;
std::unordered_map<uint32_t, Wide> g_wide;

#include "platform/overlay_cjk.inc"

const Wide &wide_glyph(uint32_t c)
{
    std::lock_guard<std::mutex> l(g_wideLock);
    auto it = g_wide.find(c);
    if (it != g_wide.end())
        return it->second;
    Wide w = {};
    w.advance = 6;
    const auto *end = kCjkGlyphs + sizeof(kCjkGlyphs) / sizeof(kCjkGlyphs[0]);
    const auto *g = std::lower_bound(kCjkGlyphs, end, c,
                                     [](const uint16_t (&e)[13], uint32_t code) { return e[0] < code; });
    if (g != end && (*g)[0] == c) {
        for (int y = 0; y < 12; y++)
            w.rows[y] = (*g)[1 + y];
        w.advance = 12;                         /* (the font's cells are all full width) */
        w.drawn = true;
    }
    return g_wide.emplace(c, w).first->second;
}

#endif /* _WIN32 */

/* The next character of a UTF-8 string (a byte that starts no valid sequence:
 * itself, as Latin-1). */
uint32_t next_char(const char *&s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c = p[0];
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n > 0) {
        uint32_t v = c & (0x3F >> n);
        int i = 1;
        for (; i <= n && (p[i] & 0xC0) == 0x80; i++)
            v = (v << 6) | (p[i] & 0x3F);
        if (i > n) {
            s += n + 1;
            return v;
        }
    }
    s += 1;
    return c;
}

/* A character's advance in font pixels (6, or a wide one's 12). */
int advance(uint32_t c)
{
    Glyph g;
    if (c < 0x80 || narrow_glyph(c, &g))
        return 6;
    return wide_glyph(c).advance;
}

void draw_rows(float x, float y, int s, const uint16_t *rows, int nrows, int ncols, uint32_t c)
{
    for (int row = 0; row < nrows; row++) {
        const uint16_t bits = rows[row];
        for (int col = 0; col < ncols;) {
            const uint16_t m = (uint16_t)(1u << (ncols - 1 - col));
            if (!(bits & m)) {
                col++;
                continue;
            }
            int end = col;
            while (end < ncols && (bits & (1u << (ncols - 1 - end))))
                end++;
            rect(x + (float)(col * s), y + (float)(row * s), (float)((end - col) * s), (float)s, c);
            col = end;
        }
    }
}

} // namespace

bool begin()
{
    int w = 0, h = 0;
    gpu::backbuffer_size(&w, &h);
    return begin_sized(w, h);
}

/* PC (2026-11-08): a panel on a target of this size (another screen's back
 * buffer: the cockpit camera's calibration panel on player 2's monitor). */
bool begin_sized(int w, int h)
{
    if (g_failed)
        return false;
    if (!g_ready) {
        if (!init()) {
            g_failed = true;
            pclog::write("overlay: its D3D objects could not be made (no frame monitor or settings menu)");
            return false;
        }
        g_ready = true;
    }
    g_bw = w;
    g_bh = h;
    if (g_bw <= 0 || g_bh <= 0)
        return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gpu::context()->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;
    g_v = (Vertex *)m.pData;
    g_n = 0;
    g_toX = 2.0f / (float)g_bw;
    g_toY = 2.0f / (float)g_bh;
    return true;
}

void size(int *w, int *h)
{
    *w = g_bw;
    *h = g_bh;
}

void rect(float x, float y, float w, float h, uint32_t c)
{
    if (g_v == nullptr || w <= 0.0f || h <= 0.0f || g_n + 6 > kMaxVertices)
        return;
    const float x0 = x * g_toX - 1.0f, x1 = (x + w) * g_toX - 1.0f;
    const float y0 = 1.0f - y * g_toY, y1 = 1.0f - (y + h) * g_toY;
    Vertex *v = g_v + g_n;
    v[0] = {x0, y0, c};
    v[1] = {x1, y0, c};
    v[2] = {x0, y1, c};
    v[3] = {x0, y1, c};
    v[4] = {x1, y0, c};
    v[5] = {x1, y1, c};
    g_n += 6;
}

float text_width(const char *str, int s)
{
    return (float)(text_cells(str) * 6 * s);
}

/* PC (2026-11-16): UTF-8 - a wide (Japanese) character two cells */
int text_cells(const char *str)
{
    int px = 0;
    while (*str != '\0')
        px += advance(upper(next_char(str)));
    return (px + 5) / 6;
}

/* each row's runs of lit pixels as one rectangle */
float text(float x, float y, int s, const char *str, uint32_t c)
{
    while (*str != '\0') {
        const uint32_t ch = upper(next_char(str));
        Glyph g;
        if (narrow_glyph(ch, &g)) {
            uint16_t rows[12];
            for (int i = 0; i < 12; i++)
                rows[i] = g.rows[i];
            draw_rows(x, y - 3.0f * s, s, rows, 12, 5, c);
            x += 6.0f * s;
            continue;
        }
        if (ch < 0x80) {
            x += 6.0f * s;              /* a space, or a character without a glyph */
            continue;
        }
        const Wide &w = wide_glyph(ch);
        if (w.drawn)
            draw_rows(x, y - 3.0f * s, s, w.rows, 12, 12, c);
        x += (float)(w.advance * s);
    }
    return x;
}

void end(ID3D11RenderTargetView *bb)
{
    ID3D11DeviceContext *dc = gpu::context();
    if (g_v == nullptr)
        return;
    dc->Unmap(g_vb, 0);
    g_v = nullptr;
    if (bb == nullptr || g_n == 0)
        return;
    const D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)g_bw, (float)g_bh, 0.0f, 1.0f};
    dc->OMSetRenderTargets(1, &bb, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(g_raster);
    dc->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(g_depth, 0);
    dc->IASetInputLayout(g_layout);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(Vertex), offset = 0;
    dc->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    dc->VSSetShader(g_vs, nullptr, 0);
    dc->GSSetShader(nullptr, nullptr, 0);
    dc->PSSetShader(g_ps, nullptr, 0);
    dc->Draw((UINT)g_n, 0);
}

} // namespace overlay
