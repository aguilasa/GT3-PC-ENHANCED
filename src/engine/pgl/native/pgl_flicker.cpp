/* pgl_flicker.cpp - GT3PC_FLICKER: a debug aid for pictures that are wrong
 * for a single frame (docs/renderer.md "Debug aids").
 *
 * GT3PC_FLICKER=1 (or the number of pictures to keep, 30 .. 600; 1 means
 * 180, three seconds of the menus) keeps the last pictures presented - each
 * drawn into a 640 x 448 slot on the GPU as the window shows it, without
 * the overlays - and the draws of the frames they show (FlickerDraw, from
 * pgl_exec.cpp).  None is kept while the tick step pauses (F5).  F8 (the
 * FlickerCapture hotkey, gt3pc.ini [Hotkeys]) reads
 * them back, oldest first, and looks for pictures that differ from both
 * neighbours where the neighbours agree (a pixel counts when a channel
 * differs by more than kDiff).  For the kMaxFound with the most such pixels
 * the three pictures (before, it, after) go to GT3PC_FLICKER_DIR (default:
 * "flicker" next to the executable) as BMPs, and the log gets where the
 * pixels are, the draws of its frame that differ from both neighbours' at
 * the same index where those two agree, and the draws of its frame into the
 * displayed buffer that touch the pixels.  A text file per F8 lists every
 * picture kept (its frame, the pixels changed since the one before, the
 * pixels that differ from both neighbours). */
#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_win32.h"   /* (the Win32 calls off Windows too) */

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "platform/frame_step.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/window.h"

namespace pglr {

namespace {

constexpr int kW = 640, kH = 448;
constexpr int kDiff = 40;               /* a channel's difference that counts */
constexpr int kMinPixels = 12;          /* a flicker smaller than that is not reported */
constexpr int kMaxFound = 6;
constexpr size_t kMaxDraws = 20000;     /* kept a frame */
/* GT3PC_FLICKER_RECT=x0,y0,x1,y1 (pixels of the 640 x 448 pictures,
 * inclusive): only the pixels inside count - a small flicker (a car lamp's
 * triangle, user report 2026-10-25) among bigger changes elsewhere (blinking
 * texts, lamps switched by the game) that otherwise take the kMaxFound */
int g_rect[4] = {0, 0, kW - 1, kH - 1};
int g_maxFound = kMaxFound;             /* (24 with a rectangle: its flickers are small, and few) */

struct Slot {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11RenderTargetView> rtv;
    uint64_t frame = 0;                 /* the frame whose execution it shows */
    uint32_t disp = ~0u;                /* the displayed buffer (~0: black) */
    float uv[4] = {0, 0, 1, 1};         /* its part of that buffer */
    int gsW = kW, gsH = kH;             /* the buffer's GS size */
};

struct FrameDraws {
    uint64_t frame = 0;
    std::vector<FlickerDraw> draws;
    std::vector<std::string> events;    /* "draw N: ..." (flicker_event) */
};
constexpr size_t kMaxEvents = 200;      /* kept a frame */

int g_state = -1;                       /* -1 not read, 0 off, else the slots */
std::vector<Slot> g_slots;
uint64_t g_kept = 0;                    /* pictures kept so far */
std::vector<FrameDraws> g_frames;       /* by frame number modulo their count */
std::vector<FlickerDraw> g_cur;         /* the frame executing */
std::vector<std::string> g_curEvents;
char g_dir[MAX_PATH];
int g_dumps = 0;

/* The pipeline statistics queries of the last kQueryFrames frames (a set
 * per frame, reused): each around one draw (its index in the frame), read
 * when its set comes round again, into that frame's record. */
constexpr int kQueryFrames = 4;
struct QuerySet {
    uint64_t frame = ~0ull;
    std::vector<Com<ID3D11Query>> q;
    std::vector<Com<ID3D11Query>> occ;  /* PC: the same draws' occlusion queries (FlickerDraw::gpuPassed) */
    std::vector<uint32_t> draw;
    size_t used = 0;
};
QuerySet g_q[kQueryFrames];
uint64_t g_qFrame = 0;                  /* the frame whose set is filling */
bool g_qFailed = false;

void resolve_set(QuerySet &s)
{
    FrameDraws *f = nullptr;
    if (s.frame != ~0ull && !g_frames.empty()) {
        FrameDraws &c = g_frames[s.frame % g_frames.size()];
        if (c.frame == s.frame)
            f = &c;
    }
    static std::vector<uint32_t> bad;
    bad.assign(s.used, 0);
    d3d_verify_resolve((int)(&s - g_q), (uint32_t)s.used, bad.data());
    for (size_t i = 0; i < s.used; i++) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS st = {};
        HRESULT hr;
        int spins = 0;
        while ((hr = dc()->GetData(s.q[i].get(), &st, sizeof(st), 0)) == S_FALSE && spins++ < 100000)
            ;
        if (hr != S_OK || f == nullptr || s.draw[i] >= f->draws.size())
            continue;
        FlickerDraw &d = f->draws[s.draw[i]];
        d.gpuVerts = (uint32_t)st.IAVertices;
        d.gpuPrims = (uint32_t)st.CPrimitives;
        d.gpuPixels = (uint32_t)st.PSInvocations;
        d.gpuBad = bad[i];
        UINT64 passed = 0;
        spins = 0;
        if (i < s.occ.size() && s.occ[i]) {
            while ((hr = dc()->GetData(s.occ[i].get(), &passed, sizeof(passed), 0)) == S_FALSE && spins++ < 100000)
                ;
            if (hr == S_OK)
                d.gpuPassed = (uint32_t)passed;
        }
    }
    s.used = 0;
    s.frame = ~0ull;
}

void resolve_all()
{
    for (QuerySet &s : g_q)
        if (s.frame != ~0ull)
            resolve_set(s);
}

bool differs(uint32_t a, uint32_t b)
{
    for (int c = 0; c < 24; c += 8) {
        const int d = (int)((a >> c) & 0xFF) - (int)((b >> c) & 0xFF);
        if (d > kDiff || d < -kDiff)
            return true;
    }
    return false;
}

const FrameDraws *frame_draws(uint64_t frame)
{
    if (g_frames.empty())
        return nullptr;
    const FrameDraws &f = g_frames[frame % g_frames.size()];
    return f.frame == frame ? &f : nullptr;
}

void describe(const FlickerDraw &d, char *out, size_t n)
{
    static const char *kinds[] = {"gs", "vu1", "skipped gs", "skipped vu1"};
    static const char *whys[] = {"", " (nothing passes)", " (no target)", " (no texture)", " (scissor)", " (other)"};
    char tex[96] = "untextured";
    if (d.tex != 0)
        snprintf(tex, sizeof(tex), "tex 0x%X %s mean %02X %02X %02X a %02X", d.tex & 0x3FFF,
                 gstex::psm_name((d.tex >> 16) & 0x3F), d.texMean & 0xFF, (d.texMean >> 8) & 0xFF,
                 (d.texMean >> 16) & 0xFF, d.texMean >> 24);
    char rt[16] = "none";
    if (d.rt != ~0u)
        snprintf(rt, sizeof(rt), "0x%X", d.rt);
    char vtx[64] = "";
    if (d.kind == 0)
        snprintf(vtx, sizeof(vtx), " | v0 rgba %08X uv %.2f %.2f", d.rgba, d.u, d.v);
    char bound[24] = "";
    if (d.bound != 0)
        snprintf(bound, sizeof(bound), " | BOUND WRONG %05X", d.bound);
    char gpu[160] = "";
    char passed[24] = "";
    if (d.gpuPassed != ~0u)
        snprintf(passed, sizeof(passed), ", %u passed", d.gpuPassed);
    if (d.kind < 2 && d.gpuVerts != ~0u)
        snprintf(gpu, sizeof(gpu), " | GPU %u verts, %u prims, %u pixels%s%s%s%s%s%s", d.gpuVerts, d.gpuPrims,
                 d.gpuPixels, passed, d.gpuBad ? " | GPU HELD OTHER:" : "", (d.gpuBad & 1) ? " VS constants" : "",
                 (d.gpuBad & 2) ? " PS constants" : "", (d.gpuBad & 4) ? " vertices" : "",
                 (d.gpuBad & 8) ? " indices" : "");
    else if (d.kind < 2)
        snprintf(gpu, sizeof(gpu), " | GPU not read");
    snprintf(out, n, "%s %u%s | FRAME %s (view %08X) | PRIM %03X | %s | view %08X | state %08X | ps %08X | data"
             " %08X%s | GS (%d,%d)-(%d,%d)%s%s", kinds[d.kind & 3], d.count, whys[d.why <= 5 ? d.why : 0], rt, d.rtv,
             d.prim, tex, d.srv, d.state, d.ps, d.data, vtx, d.x0, d.y0, d.x1, d.y1, gpu, bound);
}

/* The same draw, whichever frame buffer (the two alternate) */
bool same(const FlickerDraw &a, const FlickerDraw &b)
{
    FlickerDraw x = a, y = b;
    x.rt = y.rt = 0;
    x.rtv = y.rtv = 0;
    return memcmp(&x, &y, sizeof(x)) == 0;
}

/* Slot k of the n kept, oldest first. */
Slot &slot_at(size_t n, size_t k)
{
    return g_slots[(size_t)((g_kept - n + k) % g_slots.size())];
}

bool read_slot(Slot &s, ID3D11Texture2D *staging, std::vector<uint32_t> &px)
{
    px.assign((size_t)kW * kH, 0);
    if (!s.tex)
        return false;
    dc()->CopyResource(staging, s.tex.get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dc()->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        return false;
    for (int y = 0; y < kH; y++)
        memcpy(&px[(size_t)y * kW], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, kW * 4);
    dc()->Unmap(staging, 0);
    return true;
}

void write_bmp(const char *path, const std::vector<uint32_t> &px)
{
    FILE *f = fopen(path, "wb");
    if (f == nullptr)
        return;
    const uint32_t row = kW * 3;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
    put32(2, 54 + row * kH);
    put32(10, 54);
    put32(14, 40);
    put32(18, kW);
    put32(22, kH);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * kH);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row);
    for (int y = kH - 1; y >= 0; y--) {
        for (int x = 0; x < kW; x++) {
            const uint32_t p = px[(size_t)y * kW + x];
            line[3 * x + 0] = (uint8_t)(p >> 16);
            line[3 * x + 1] = (uint8_t)(p >> 8);
            line[3 * x + 2] = (uint8_t)p;
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

struct Found {
    size_t k;                           /* the slot (oldest first) */
    int pixels;
    int x0, y0, x1, y1;                 /* picture pixels, inclusive */
};

/* The events of the frame shown in slot k, its draws that differ from both
 * neighbours' (same index) where those two agree, then its draws into the
 * displayed buffer touching f's pixels (with the frame before's at the same
 * index where that one differs). */
void log_draws(size_t n, const Found &f)
{
    const Slot &a = slot_at(n, f.k - 1), &b = slot_at(n, f.k), &c = slot_at(n, f.k + 1);
    const FrameDraws *da = frame_draws(a.frame), *db = frame_draws(b.frame), *dc_ = frame_draws(c.frame);
    char s0[512], s1[512], s2[512];
    if (db == nullptr) {
        pclog::write("pgl flicker:   the draws of frame %llu are no longer kept", (unsigned long long)b.frame);
        return;
    }
    pclog::write("pgl flicker:   frame %llu: %u draws, %u events; before (frame %llu) %d, %d; after (frame %llu)"
                 " %d, %d", (unsigned long long)b.frame, (unsigned)db->draws.size(), (unsigned)db->events.size(),
                 (unsigned long long)a.frame, da ? (int)da->draws.size() : -1, da ? (int)da->events.size() : -1,
                 (unsigned long long)c.frame, dc_ ? (int)dc_->draws.size() : -1, dc_ ? (int)dc_->events.size() : -1);
    for (size_t i = 0; i < db->events.size() && i < 40; i++)
        pclog::write("pgl flicker:   event: %s", db->events[i].c_str());
    if (db->events.size() > 40)
        pclog::write("pgl flicker:   (%u more events)", (unsigned)db->events.size() - 40);
    /* the draws the GPU did not get as written (or with a binding wrong) */
    auto wrong = [](const FrameDraws *fd) {
        int n = 0;
        if (fd != nullptr)
            for (const FlickerDraw &d : fd->draws)
                n += (d.gpuBad != 0 || d.bound != 0) ? 1 : 0;
        return n;
    };
    pclog::write("pgl flicker:   draws the GPU did not get as written: %d (before %d, after %d)", wrong(db), wrong(da),
                 wrong(dc_));
    int listed = 0;
    for (size_t i = 0; i < db->draws.size() && listed < 30; i++) {
        const FlickerDraw &d = db->draws[i];
        if (d.gpuBad == 0 && d.bound == 0)
            continue;
        listed++;
        describe(d, s1, sizeof(s1));
        pclog::write("pgl flicker:   not as written: draw %u: %s", (unsigned)i, s1);
    }
    if (da != nullptr && dc_ != nullptr && a.frame != b.frame && c.frame != b.frame) {
        int shown = 0, more = 0;
        const size_t m = std::max(db->draws.size(), std::max(da->draws.size(), dc_->draws.size()));
        for (size_t i = 0; i < m; i++) {
            const bool ha = i < da->draws.size(), hb = i < db->draws.size(), hc = i < dc_->draws.size();
            const bool agree = ha == hc && (!ha || same(da->draws[i], dc_->draws[i]));
            if (!agree)
                continue;
            if (ha == hb && (!hb || same(db->draws[i], da->draws[i])))
                continue;
            if (shown++ >= 12) {
                more++;
                continue;
            }
            if (hb)
                describe(db->draws[i], s1, sizeof(s1));
            else
                snprintf(s1, sizeof(s1), "(none)");
            if (ha)
                describe(da->draws[i], s0, sizeof(s0));
            else
                snprintf(s0, sizeof(s0), "(none)");
            pclog::write("pgl flicker:   draw %u: %s", (unsigned)i, s1);
            pclog::write("pgl flicker:     the neighbours': %s", s0);
        }
        if (shown == 0)
            pclog::write("pgl flicker:   no draw differs from both neighbours' where they agree");
        else if (more > 0)
            pclog::write("pgl flicker:   (%d more such draws)", more);
    }
    /* PC: the draws whose GPU counts are the odd ones out - primitives, shader
     * runs or passing samples off from the same draw's in both neighbours,
     * those two close: a moving camera changes every draw's constants and
     * data each frame but its counts only a little, a triangle lost (culled,
     * clipped, failing a test) changes them in its frame alone (user report
     * 2026-10-25: a car lamp's triangle) */
    /* (the same draw in a neighbour: by kind, size, PRIM, texture and target
     * near the same index - a frame with a draw more or less shifts every
     * later index; frame 1683 of the lamp report had 1274 draws, the one
     * before 1275; a draw into the displayed buffer is into the other one in
     * a neighbour: the two alternate) */
    auto match = [&b](const FrameDraws *fd, uint32_t fdDisp, size_t i, const FlickerDraw &d) -> const FlickerDraw * {
        for (int off = 0; off <= 12; off++)
            for (int sgn = 1; sgn >= -1; sgn -= 2) {
                if (off == 0 && sgn < 0)
                    continue;
                const long long j = (long long)i + sgn * off;
                if (j < 0 || j >= (long long)fd->draws.size())
                    continue;
                const FlickerDraw &e = fd->draws[(size_t)j];
                if (e.kind == d.kind && e.count == d.count && e.prim == d.prim && e.tex == d.tex &&
                    (e.rt == d.rt || (e.rt == fdDisp && d.rt == b.disp)))
                    return &e;
            }
        return nullptr;
    };
    if (da != nullptr && dc_ != nullptr && a.frame != b.frame && c.frame != b.frame) {
        auto odd = [](uint32_t x, uint32_t y, uint32_t z, bool exact) {
            if (x == ~0u || y == ~0u || z == ~0u)
                return false;
            const uint32_t lo = std::min(x, z), hi = std::max(x, z);
            if (exact)
                return lo == hi && y != lo;
            const uint32_t dev = y < lo ? lo - y : y > hi ? y - hi : 0;
            return dev >= 4 && dev > 2 * (hi - lo) && (uint64_t)dev * 50 > lo;
        };
        int shown = 0;
        for (size_t i = 0; i < db->draws.size(); i++) {
            const FlickerDraw &y = db->draws[i];
            if (y.kind >= 2)
                continue;
            const FlickerDraw *px = match(da, a.disp, i, y), *pz = match(dc_, c.disp, i, y);
            if (px == nullptr || pz == nullptr)
                continue;                   /* (not the same draw in the three frames) */
            const FlickerDraw &x = *px, &z = *pz;
            const bool p = odd(x.gpuPrims, y.gpuPrims, z.gpuPrims, true);
            const bool r = odd(x.gpuPixels, y.gpuPixels, z.gpuPixels, false);
            const bool s = odd(x.gpuPassed, y.gpuPassed, z.gpuPassed, false);
            if (!p && !r && !s)
                continue;
            if (shown++ >= 20)
                continue;
            describe(y, s1, sizeof(s1));
            pclog::write("pgl flicker:   GPU counts off (%s%s%s): draw %u: prims %u (neighbours %u / %u), pixels %u"
                         " (%u / %u), passed %d (%d / %d): %s", p ? "prims " : "", r ? "pixels " : "",
                         s ? "passed" : "", (unsigned)i, y.gpuPrims, x.gpuPrims, z.gpuPrims, y.gpuPixels, x.gpuPixels,
                         z.gpuPixels, (int)y.gpuPassed, (int)x.gpuPassed, (int)z.gpuPassed, s1);
        }
        if (shown == 0)
            pclog::write("pgl flicker:   no draw's GPU counts are off from both neighbours'");
        else if (shown > 20)
            pclog::write("pgl flicker:   (%d more such draws)", shown - 20);
        /* with GT3PC_FLICKER_RECT: every draw of a 640 x 448 target (a race
         * frame: the pictures' own pixels) touching the rectangle, with the
         * neighbours' counts, odd or not */
        if (g_rect[0] != 0 || g_rect[1] != 0 || g_rect[2] != kW - 1 || g_rect[3] != kH - 1) {
            int listed = 0;
            for (size_t i = 0; i < db->draws.size(); i++) {
                const FlickerDraw &y = db->draws[i];
                if (y.kind >= 2 || y.rt == b.disp || y.x1 < g_rect[0] || y.x0 > g_rect[2] || y.y1 < g_rect[1] ||
                    y.y0 > g_rect[3] || y.x1 - y.x0 >= kW - 1)
                    continue;               /* (full-screen passes left out) */
                if (listed++ >= 80)
                    continue;
                const FlickerDraw *px = match(da, a.disp, i, y), *pz = match(dc_, c.disp, i, y);
                describe(y, s1, sizeof(s1));
                pclog::write("pgl flicker:   in the rectangle: draw %u: prims %u (%d / %d), pixels %u (%d / %d), passed"
                             " %d (%d / %d): %s", (unsigned)i, y.gpuPrims, px ? (int)px->gpuPrims : -1,
                             pz ? (int)pz->gpuPrims : -1, y.gpuPixels, px ? (int)px->gpuPixels : -1,
                             pz ? (int)pz->gpuPixels : -1, (int)y.gpuPassed, px ? (int)px->gpuPassed : -1,
                             pz ? (int)pz->gpuPassed : -1, s1);
            }
            if (listed > 80)
                pclog::write("pgl flicker:   (%d more in the rectangle)", listed - 80);
        }
    }
    /* the pixels in the displayed buffer's GS pixels */
    const float du = b.uv[2] - b.uv[0], dv = b.uv[3] - b.uv[1];
    const int gx0 = (int)((b.uv[0] + du * (float)f.x0 / kW) * (float)b.gsW);
    const int gx1 = (int)((b.uv[0] + du * (float)(f.x1 + 1) / kW) * (float)b.gsW);
    const int gy0 = (int)((b.uv[1] + dv * (float)f.y0 / kH) * (float)b.gsH);
    const int gy1 = (int)((b.uv[1] + dv * (float)(f.y1 + 1) / kH) * (float)b.gsH);
    int touching = 0;
    for (size_t i = 0; i < db->draws.size(); i++) {
        const FlickerDraw &d = db->draws[i];
        if (d.rt != b.disp || d.x1 < gx0 || d.x0 > gx1 || d.y1 < gy0 || d.y0 > gy1)
            continue;
        if (touching++ >= 40)
            continue;
        describe(d, s2, sizeof(s2));
        pclog::write("pgl flicker:   touches it (GS (%d,%d)-(%d,%d) of 0x%X): draw %u: %s", gx0, gy0, gx1, gy1,
                     b.disp, (unsigned)i, s2);
        if (da != nullptr && i < da->draws.size() && !same(da->draws[i], d)) {
            describe(da->draws[i], s0, sizeof(s0));
            pclog::write("pgl flicker:     the frame before's: %s", s0);
        }
    }
    if (touching > 40)
        pclog::write("pgl flicker:   (%d more draws touch it)", touching - 40);
}

void dump()
{
    const size_t n = (size_t)std::min<uint64_t>(g_kept, g_slots.size());
    if (n < 3) {
        pclog::write("pgl flicker: F8: fewer than 3 pictures kept");
        return;
    }
    g_dumps++;
    CreateDirectoryA(g_dir, nullptr);
    resolve_all();                      /* (the GPU counts of the last frames) */
    {
        /* how often the GPU held other data than written, over the frames kept */
        uint64_t draws = 0, bad = 0, frames = 0, badFrames = 0;
        uint32_t bits = 0;
        for (const FrameDraws &f : g_frames) {
            if (f.frame == 0 || f.draws.empty())
                continue;
            frames++;
            bool any = false;
            for (const FlickerDraw &d : f.draws) {
                if (d.kind >= 2 || d.gpuVerts == ~0u)
                    continue;
                draws++;
                if (d.gpuBad != 0) {
                    bad++;
                    bits |= d.gpuBad;
                    any = true;
                }
            }
            badFrames += any ? 1 : 0;
        }
        pclog::write("pgl flicker: GPU checks over %llu frames: %llu of %llu draws held other data than written"
                     " (bits %X), in %llu frames", (unsigned long long)frames, (unsigned long long)bad,
                     (unsigned long long)draws, bits, (unsigned long long)badFrames);
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kW;
    d.Height = kH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    if (FAILED(dev()->CreateTexture2D(&d, nullptr, staging.put())))
        return;

    /* one pass, three pictures at a time: k - 2, k - 1, k */
    std::vector<uint32_t> p0, p1, p2;
    std::vector<int> changed(n, 0), flick(n, 0);
    std::vector<Found> found;
    for (size_t k = 0; k < n; k++) {
        read_slot(slot_at(n, k), staging.get(), p2);
        if (k >= 1) {
            int c = 0;
            for (size_t i = 0; i < p2.size(); i++)
                c += differs(p1[i], p2[i]) ? 1 : 0;
            changed[k] = c;
        }
        if (k >= 2) {
            Found f = {k - 1, 0, kW, kH, -1, -1};
            for (int y = g_rect[1]; y <= g_rect[3]; y++)
                for (int x = g_rect[0]; x <= g_rect[2]; x++) {
                    const size_t i = (size_t)y * kW + x;
                    if (differs(p0[i], p1[i]) && differs(p1[i], p2[i]) && !differs(p0[i], p2[i])) {
                        f.pixels++;
                        f.x0 = std::min(f.x0, x);
                        f.y0 = std::min(f.y0, y);
                        f.x1 = std::max(f.x1, x);
                        f.y1 = std::max(f.y1, y);
                    }
                }
            flick[k - 1] = f.pixels;
            if (f.pixels >= kMinPixels)
                found.push_back(f);
        }
        p0.swap(p1);
        p1.swap(p2);
    }

    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "flicker_%d.txt", g_dir, g_dumps);
    if (FILE *t = fopen(path, "w")) {
        fprintf(t, "picture  frame  buffer  changed  flicker (pixels of 640 x 448; oldest first)\n");
        for (size_t k = 0; k < n; k++) {
            const Slot &s = slot_at(n, k);
            fprintf(t, "%5u %8llu  %6X %8d %8d\n", (unsigned)k, (unsigned long long)s.frame, s.disp, changed[k],
                    flick[k]);
        }
        fclose(t);
    }

    /* the most pixels first, then at most kMaxFound, two pictures apart */
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.pixels > b.pixels; });
    std::vector<Found> keep;
    for (const Found &f : found) {
        bool tooClose = false;          /* (`near' is a Windows macro) */
        for (const Found &k : keep)
            tooClose = tooClose || (f.k + 2 > k.k && k.k + 2 > f.k);
        if (!tooClose)
            keep.push_back(f);
        if ((int)keep.size() >= g_maxFound)
            break;
    }
    std::sort(keep.begin(), keep.end(), [](const Found &a, const Found &b) { return a.k < b.k; });
    pclog::write("pgl flicker: F8 (%d): %u pictures kept; %u differ from both neighbours where they agree"
                 " (%u reported), %s" PGL_PATH_SEP "flicker_%d.txt", g_dumps, (unsigned)n, (unsigned)found.size(),
                 (unsigned)keep.size(), g_dir, g_dumps);
    int idx = 0;
    for (const Found &f : keep) {
        idx++;
        const Slot &b = slot_at(n, f.k);
        pclog::write("pgl flicker: picture %u of %u (frame %llu, buffer 0x%X): %d pixels in (%d,%d)-(%d,%d) of"
                     " 640x448; flicker_%d_%d_a/b/c.bmp (before, it, after)", (unsigned)f.k, (unsigned)n,
                     (unsigned long long)b.frame, b.disp, f.pixels, f.x0, f.y0, f.x1, f.y1, g_dumps, idx);
        const char *tags[3] = {"a", "b", "c"};
        for (int j = 0; j < 3; j++) {
            read_slot(slot_at(n, f.k - 1 + j), staging.get(), p0);
            snprintf(path, sizeof(path), "%s" PGL_PATH_SEP "flicker_%d_%d_%s.bmp", g_dir, g_dumps, idx, tags[j]);
            write_bmp(path, p0);
        }
        log_draws(n, f);
    }
}

}  /* namespace */

bool flicker_on()
{
    if (g_state < 0) {
        g_state = 0;
        const char *e = getenv("GT3PC_FLICKER");
        const int v = e != nullptr ? atoi(e) : 0;
        if (v > 0) {
            g_state = v == 1 ? 180 : std::min(600, std::max(30, v));
            g_slots.resize((size_t)g_state);
            g_frames.resize((size_t)g_state + 8);
            const char *dir = getenv("GT3PC_FLICKER_DIR");
            if (dir != nullptr && dir[0] != 0) {
                snprintf(g_dir, sizeof(g_dir), "%s", dir);
            } else {
                char exe[MAX_PATH] = "";
                GetModuleFileNameA(nullptr, exe, MAX_PATH);
                char *slash = strrchr(exe, PGL_PATH_SEP[0]);
                if (slash != nullptr)
                    *slash = 0;
                snprintf(g_dir, sizeof(g_dir), "%s" PGL_PATH_SEP "flicker", exe);
            }
            const size_t len = strlen(g_dir);
            if (len > 0 && (g_dir[len - 1] == '\\' || g_dir[len - 1] == '/'))
                g_dir[len - 1] = 0;
            pclog::write("pgl: GT3PC_FLICKER: the last %d pictures kept; F8 writes the ones that differ from both"
                         " neighbours to %s", g_state, g_dir);
            const char *rect = getenv("GT3PC_FLICKER_RECT");
            int r[4];
            if (rect != nullptr && sscanf(rect, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4) {
                g_rect[0] = std::max(0, std::min(r[0], r[2]));
                g_rect[1] = std::max(0, std::min(r[1], r[3]));
                g_rect[2] = std::min(kW - 1, std::max(r[0], r[2]));
                g_rect[3] = std::min(kH - 1, std::max(r[1], r[3]));
                g_maxFound = 24;
                pclog::write("pgl: GT3PC_FLICKER_RECT: only the pixels in (%d,%d)-(%d,%d) count; up to %d"
                             " reported", g_rect[0], g_rect[1], g_rect[2], g_rect[3], g_maxFound);
            }
        }
    }
    return g_state > 0;
}

void flicker_draw(const FlickerDraw &d)
{
    if (g_cur.size() < kMaxDraws)
        g_cur.push_back(d);
}

void flicker_end_frame(uint64_t frame)
{
    FrameDraws &f = g_frames[frame % g_frames.size()];
    f.frame = frame;
    f.draws.swap(g_cur);
    g_cur.clear();
    f.events.swap(g_curEvents);
    g_curEvents.clear();
    /* the queries: this frame's set is its own now; the oldest set is read
     * (kQueryFrames - 1 frames on: done) and fills next */
    g_q[g_qFrame % kQueryFrames].frame = frame;
    g_qFrame++;
    QuerySet &next = g_q[g_qFrame % kQueryFrames];
    if (next.frame != ~0ull)
        resolve_set(next);
}

ID3D11Query *flicker_query_begin()
{
    if (!flicker_on() || g_qFailed)
        return nullptr;
    QuerySet &s = g_q[g_qFrame % kQueryFrames];
    if (s.used >= 4096)
        return nullptr;
    if (s.used == s.q.size()) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
        Com<ID3D11Query> q;
        if (FAILED(dev()->CreateQuery(&d, q.put()))) {
            g_qFailed = true;
            pclog::write("pgl: GT3PC_FLICKER: no pipeline statistics queries (the GPU counts are not kept)");
            return nullptr;
        }
        s.q.push_back(std::move(q));
        D3D11_QUERY_DESC od = {D3D11_QUERY_OCCLUSION, 0};
        Com<ID3D11Query> o;
        if (FAILED(dev()->CreateQuery(&od, o.put())))
            o.reset();                      /* (the passing counts not kept) */
        s.occ.push_back(std::move(o));
        s.draw.push_back(0);
    }
    ID3D11Query *q = s.q[s.used].get();
    s.draw[s.used] = (uint32_t)g_cur.size();
    if (s.occ[s.used])
        dc()->Begin(s.occ[s.used].get());
    s.used++;
    dc()->Begin(q);
    return q;
}

void flicker_query_end(ID3D11Query *q, bool indexed, uint32_t first, uint32_t count, int32_t baseVertex)
{
    dc()->End(q);
    const int set = (int)(g_qFrame % kQueryFrames);
    const QuerySet &s = g_q[set];
    if (s.used > 0 && s.occ[s.used - 1])
        dc()->End(s.occ[s.used - 1].get());
    if (s.used > 0)
        d3d_verify_copy(set, (uint32_t)(s.used - 1), indexed, first, count, baseVertex);
}

void flicker_event(const char *fmt, ...)
{
    if (!flicker_on() || g_curEvents.size() >= kMaxEvents)
        return;
    char text[400];
    const int at = snprintf(text, sizeof(text), "draw %u: ", (unsigned)g_cur.size());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + at, sizeof(text) - (size_t)at, fmt, ap);
    va_end(ap);
    g_curEvents.emplace_back(text);
}

void flicker_capture(ID3D11ShaderResourceView *picture, const float uv[4], int gsW, int gsH, uint32_t disp)
{
    if (!flicker_on() || frame_step::paused())
        return;
    Slot &s = g_slots[(size_t)(g_kept % g_slots.size())];
    if (!s.tex) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = kW;
        d.Height = kH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, s.tex.put())) ||
            FAILED(dev()->CreateRenderTargetView(s.tex.get(), nullptr, s.rtv.put()))) {
            s.tex.reset();
            s.rtv.reset();
            log_once("pgl-flicker-slot", "pgl: GT3PC_FLICKER: could not create a picture slot");
            return;
        }
    }
    ID3D11RenderTargetView *rtv = s.rtv.get();
    if (picture == nullptr) {
        const float black[4] = {0, 0, 0, 1};
        dc()->ClearRenderTargetView(rtv, black);
    } else {
        dc()->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)kW, (float)kH, 0, 1};
        dc()->RSSetViewports(1, &vp);
        dc()->RSSetState(raster_state(0, false));
        dc()->OMSetBlendState(blend_plain(15), nullptr, 0xFFFFFFFFu);
        dc()->OMSetDepthStencilState(depth_state(false, pgl::gs::Compare::Always, false), 0);
        bind_vs(Vs::Full);
        bind_ps(Ps::Present);
        const CbPresent cb = {uv[0], uv[1], uv[2], uv[3]};
        bind_ps_cb(&cb, sizeof(cb));
        dc()->PSSetShaderResources(0, 1, &picture);
        ID3D11SamplerState *smp = sampler_state(true, true, 1, 1);
        dc()->PSSetSamplers(0, 1, &smp);
        dc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc()->Draw(3, 0);
        ID3D11ShaderResourceView *none = nullptr;
        dc()->PSSetShaderResources(0, 1, &none);
        dc()->OMSetRenderTargets(0, nullptr, nullptr);
    }
    const uint64_t f = frame_number();
    s.frame = f > 0 ? f - 1 : 0;        /* (next_frame came after its execution) */
    s.disp = picture != nullptr ? disp : ~0u;
    memcpy(s.uv, uv, sizeof(s.uv));
    s.gsW = std::max(1, gsW);
    s.gsH = std::max(1, gsH);
    g_kept++;
}

void flicker_poll()
{
    if (flicker_on() && window::take_hotkey(input_map::kHotFlickerCapture))
        dump();
}

}  /* namespace pglr */
