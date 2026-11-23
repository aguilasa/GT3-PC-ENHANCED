/* song_osd.cpp - the race's song changer's panel (song_osd.h). */
#include "platform/song_osd.h"
#include "platform/hotkey_hint.h"               /* PC (2026-11-08): control_text */
#include "platform/input_map.h"
#include "platform/overlay.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages */

#ifdef _WIN32
#include <windows.h>
#else
#include "platform/os/fs.h"                     /* the native text as UTF-8, widened */
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>

namespace song_osd {
namespace {

using Clock = std::chrono::steady_clock;

/* A live race's frame this recent: the race runs (a paused one, or none,
 * leaves it older). */
constexpr auto kLive = std::chrono::milliseconds(250);
/* Steps asked for and not taken yet, at most. */
constexpr int kMaxSteps = 4;

std::atomic<bool> g_open{false};
std::atomic<long long> g_lastFrame{0};      /* Clock's ticks; 0: none */
std::atomic<int> g_steps{0};

std::mutex g_lock;                          /* the song's text */
std::string g_title, g_artist;              /* folded (fold); title "": none */
int g_number = 0, g_count = 0;

bool live()
{
    const long long last = g_lastFrame.load();
    return last != 0 && Clock::now().time_since_epoch().count() - last <
                            std::chrono::duration_cast<Clock::duration>(kLive).count();
}

#ifndef _WIN32
/* The base letter of a precomposed Latin letter with an accent (U+00C0 ..
 * U+017F; what Unicode's canonical decomposition leaves of it, as
 * FoldStringW's MAP_COMPOSITE on Windows), 0 for none. */
wchar_t base_letter(wchar_t c)
{
    static const char kLatin1[] =               /* U+00C0 .. U+00FF */
        "AAAAAA\0CEEEEIIII\0NOOOOO\0\0UUUUY\0\0"
        "aaaaaa\0ceeeeiiii\0nooooo\0\0uuuuy\0y";
    static const char kExtendedA[] =            /* U+0100 .. U+017F */
        "AaAaAaCcCcCcCcDd\0\0EeEeEeEeEeGgGgGgGgHh\0\0IiIiIiIiI\0\0\0Jj"
        "Kk\0LlLlLl\0\0\0\0NnNnNn\0\0\0OoOoOo\0\0RrRrRrSsSsSsSsTtTt\0\0"
        "UuUuUuUuUuUuWwYyYZzZzZz\0";
    static_assert(sizeof(kLatin1) == 0x40 + 1, "U+00C0 .. U+00FF");
    static_assert(sizeof(kExtendedA) == 0x80 + 1, "U+0100 .. U+017F");
    if (c >= 0xC0 && c <= 0xFF)
        return (wchar_t)(unsigned char)kLatin1[c - 0xC0];
    if (c >= 0x100 && c <= 0x17F)
        return (wchar_t)(unsigned char)kExtendedA[c - 0x100];
    return 0;
}
#endif

/* The game's text (the ANSI code page on Windows, UTF-8 elsewhere) in the
 * panel font's characters: the accents left off (Unicode's decomposition: e
 * from e-acute), a few letters and signs spelled out, anything else '?'. */
std::string fold(const char *text)
{
    std::string out;
    if (text == nullptr)
        return out;
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (n <= 1)
        return out;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &w[0], n);
    const int m = FoldStringW(MAP_COMPOSITE | MAP_FOLDCZONE, w.c_str(), -1, nullptr, 0);
    if (m > 1) {
        std::wstring d((size_t)m, L'\0');
        FoldStringW(MAP_COMPOSITE | MAP_FOLDCZONE, w.c_str(), -1, &d[0], m);
        w.swap(d);
    }
#else
    std::wstring w = fs::widen(fs::native_to_utf8(text));
    for (wchar_t &c : w) {
        const wchar_t b = base_letter(c);
        if (b != 0)
            c = b;
    }
#endif
    for (wchar_t c : w) {
        if (c == 0)
            break;
        if (c >= 0x300 && c <= 0x36F)
            continue;                       /* (a combining accent) */
        if (c >= 0x20 && c < 0x7F) {
            out += (char)c;
            continue;
        }
        switch (c) {
        case 0xC6: out += "AE"; break;
        case 0xE6: out += "ae"; break;
        case 0x152: out += "OE"; break;
        case 0x153: out += "oe"; break;
        case 0xDF: out += "ss"; break;
        case 0xD8: case 0xF8: out += 'O'; break;
        case 0x110: case 0x111: case 0xD0: case 0xF0: out += 'D'; break;
        case 0x141: case 0x142: out += 'L'; break;
        case 0xDE: case 0xFE: out += "TH"; break;
        case 0x2018: case 0x2019: case 0xB4: out += '\''; break;
        case 0x201C: case 0x201D: out += '"'; break;
        case 0x2013: case 0x2014: out += '-'; break;
        case 0x2026: out += "..."; break;
        case 0xD7: out += 'X'; break;
        case 0xA0: case 0x3000: out += ' '; break;
        default: out += '?'; break;
        }
    }
    return out;
}

/* "UP (,) PREVIOUS": hotkey h's device and key, then what it does (PC
 * 2026-11-08: platform/hotkey_hint.h, shared with the cockpit camera's
 * calibration panel). */
std::string control_text(int h, const char *what)
{
    return hotkey_hint::control_text(h, what);
}

/* text cut to `width' pixels at scale s ("..." at its end; PC 2026-11-16:
 * whole UTF-8 characters, counted in cells - a Japanese one two) */
std::string fit(const std::string &text, float width, int s)
{
    if (overlay::text_width(text.c_str(), s) <= width)
        return text;
    const int cells = (int)(width / (6.0f * (float)s)) - 3;
    std::string out;
    int used = 0;
    for (size_t i = 0, n; i < text.size(); i += n) {
        for (n = 1; i + n < text.size() && ((unsigned char)text[i + n] & 0xC0) == 0x80; n++) {
        }
        const std::string ch = text.substr(i, n);
        used += overlay::text_cells(ch.c_str());
        if (used > cells)
            break;
        out += ch;
    }
    return out + "...";
}

} // namespace

void toggle()
{
    if (!live())
        return;
    g_open = !g_open.load();
    g_steps = 0;
}

void step(int dir)
{
    if (!g_open.load() || !live())
        return;
    const int s = g_steps.load();
    g_steps = std::clamp(s + (dir > 0 ? 1 : -1), -kMaxSteps, kMaxSteps);
}

int race_frame()
{
    g_lastFrame = Clock::now().time_since_epoch().count();
    if (!g_open.load()) {
        g_steps = 0;
        return 0;
    }
    int s = g_steps.load();
    while (s != 0 && !g_steps.compare_exchange_weak(s, s > 0 ? s - 1 : s + 1)) {
    }
    return s > 0 ? 1 : s < 0 ? -1 : 0;
}

void race_end()
{
    g_open = false;
    g_steps = 0;
    g_lastFrame = 0;
}

void set_song(const char *title, const char *artist, int number, int count)
{
    std::string t = fold(title), a = fold(artist);
    std::lock_guard<std::mutex> hold(g_lock);
    g_title = title != nullptr ? (t.empty() ? std::string("?") : t) : std::string();
    g_artist = a;
    g_number = number;
    g_count = count;
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (bb == nullptr || !g_open.load() || !live() || !overlay::begin())
        return;
    std::string title, artist;
    int number, count;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        title = g_title;
        artist = g_artist;
        number = g_number;
        count = g_count;
    }
    /* (the hotkeys are read once, at the game's start; PC 2026-11-16: the
     * text made at each frame, in the interface's language - it may change) */
    std::string hint;
    {
        const std::string parts[] = {control_text(input_map::kHotPreviousSong, tr("PREVIOUS")),
                                     control_text(input_map::kHotNextSong, tr("NEXT")),
                                     control_text(input_map::kHotSongMenu, tr("CLOSE"))};
        for (const std::string &p : parts)
            if (!p.empty())
                hint += (hint.empty() ? "" : "   ") + p;
    }
    int bw, bh;
    overlay::size(&bw, &bh);
    /* the title half again as large as the panels' text (osd.cpp's unit) */
    const int unit = std::max(1, bh / 540), big = std::max(1, (bh * 3 + 540) / 1080), small = unit;
    const float pad = 6.0f * (float)unit, gap = 4.0f * (float)unit;
    const float maxText = (float)bw * 0.45f - 2.0f * pad;
    char head[256];                             /* PC (2026-11-16): 48 before (UTF-8 translations) */
    if (count > 0)
        snprintf(head, sizeof(head), tr("RACE SONGS  %d / %d"), number, count);
    else
        snprintf(head, sizeof(head), "%s", tr("RACE SONGS"));
    const std::string lines[4] = {fit(head, maxText, small),
                                  fit(title.empty() ? std::string(tr("NO SONG PLAYING")) : title, maxText, big),
                                  fit(artist, maxText, small), fit(hint, maxText, small)};
    const int scale[4] = {small, big, small, small};
    float textW = 0.0f, h = 2.0f * pad;
    for (int i = 0; i < 4; i++) {
        textW = std::max(textW, overlay::text_width(lines[i].c_str(), scale[i]));
        h += 7.0f * (float)scale[i] + (i < 3 ? gap : 0.0f);
    }
    h += gap;                               /* (the controls a little apart) */
    const float w = textW + 2.0f * pad;
    const float x = 8.0f * (float)unit, y = (float)bh - h - 8.0f * (float)unit;
    const uint32_t amber = rgba(255, 200, 80, 255);
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 190));
    overlay::rect(x, y, 2.0f * (float)unit, h, amber);
    const uint32_t colour[4] = {rgba(255, 200, 80, 200), title.empty() ? rgba(150, 150, 150, 255) : amber,
                                rgba(230, 230, 230, 255), rgba(150, 150, 150, 255)};
    float ty = y + pad;
    for (int i = 0; i < 4; i++) {
        if (i == 3)
            ty += gap;
        overlay::text(x + pad, ty, scale[i], lines[i].c_str(), colour[i]);
        ty += 7.0f * (float)scale[i] + gap;
    }
    overlay::end(bb);
}

} // namespace song_osd
