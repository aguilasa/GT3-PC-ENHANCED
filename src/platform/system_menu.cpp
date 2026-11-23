/* system_menu.cpp - the Esc menu of GT3 PC (system_menu.h). */
#include "platform/system_menu.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/memcard.h"
#include "platform/overlay.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages */
#include "platform/window.h"

#include "platform/os/win_compat.h"
#ifndef _WIN32
#include <SDL3/SDL_events.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace system_menu {
namespace {

using overlay::rgba;

enum Action { ACT_NONE, ACT_BACK, ACT_RESTART, ACT_RESTART_NEW_GAME, ACT_EXIT };

struct Item {
    const char *label;
    const char *help;                           /* under the list while it is chosen */
    Action action;
};

/* (help: any number of lines, '\n' between; both translated where drawn) */
const Item kItems[] = {
    {N_("BACK"), N_("CLOSES THIS MENU."), ACT_BACK},
    {N_("RESTART"), N_("THE GAME STARTS AGAIN FROM THE BEGINNING.\nWHAT IS NOT SAVED IS LOST."), ACT_RESTART},
    {N_("RESTART AS NEW GAME"),
     N_("THE GAME STARTS AGAIN WITHOUT LOADING A SAVE.\nYOUR SAVES STAY ON THE MEMORY CARD.\nWHAT IS NOT SAVED IS LOST."),
     ACT_RESTART_NEW_GAME},
    {N_("EXIT"), N_("CLOSES THE GAME.\nWHAT IS NOT SAVED IS LOST."), ACT_EXIT},
};
constexpr int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

std::atomic<bool> g_shown{false};
std::atomic<bool> g_restart{false};
std::atomic<bool> g_restartNewGame{false};
bool g_newGameRun;                              /* set once, before the game threads */
std::mutex g_lock;                             /* the selection and the pending action (window / render threads) */
int g_sel;
/* Restart / Exit chosen while a memory card was being written: done at the
 * first present after it (present) */
Action g_pending = ACT_NONE;

void close_window()
{
#ifdef _WIN32
    PostMessageW(window::handle(), WM_CLOSE, 0, 0);
#else
    /* (the window's loop ends as when it is closed) */
    SDL_Event quit = {};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
#endif
}

/* Restart / Exit now (the window closes; main.cpp restarts) */
void act(Action a)
{
    if (a == ACT_RESTART) {
        pclog::write("system menu: restart");
        g_restart = true;
    } else if (a == ACT_RESTART_NEW_GAME) {
        pclog::write("system menu: restart as a new game");
        g_restartNewGame = true;
        g_restart = true;
    } else {
        pclog::write("system menu: exit");
    }
    close_window();
}

void choose()
{
    const Action a = kItems[g_sel].action;
    if (a == ACT_BACK) {
        g_shown = false;
        return;
    }
    if (memcard::busy()) {
        g_pending = a;                          /* the menu stays, saying why (build) */
        return;
    }
    act(a);
}

void move(int d)
{
    g_sel = (g_sel + d + kItemCount) % kItemCount;
}

void close_menu()
{
    g_shown = false;
    g_pending = ACT_NONE;
}

/* ---- the game controllers (read at each present) --------------------- */

enum : uint16_t {
    PAD_UP = 1u << 4, PAD_DOWN = 1u << 6, PAD_CIRCLE = 1u << 13, PAD_CROSS = 1u << 14
};
uint16_t g_padPrev[2];
bool g_padSync = true;                          /* the next read only takes the buttons down (just opened) */

void poll_pads()
{
    for (int port = 0; port < 2; port++) {
        gamepad::State st;
        const uint16_t b = gamepad::read(port, &st) ? st.buttons : 0;
        const uint16_t pressed = g_padSync ? 0 : (uint16_t)(b & ~g_padPrev[port]);
        g_padPrev[port] = b;
        if (pressed & PAD_CIRCLE) {
            close_menu();
            return;
        }
        if (pressed & PAD_UP) move(-1);
        if (pressed & PAD_DOWN) move(1);
        if (pressed & PAD_CROSS) choose();
    }
    g_padSync = false;
}

/* ---- drawing (as the settings menu's) -------------------------------- */

const uint32_t kPanel = rgba(10, 12, 16, 215);
const uint32_t kFrame = rgba(120, 190, 255, 160);
const uint32_t kTitle = rgba(255, 255, 255, 255);
const uint32_t kLabel = rgba(170, 170, 170, 255);
const uint32_t kLabelSel = rgba(255, 255, 255, 255);
const uint32_t kHighlight = rgba(255, 255, 255, 30);
const uint32_t kHelp = rgba(200, 200, 150, 255);
const uint32_t kHint = rgba(130, 130, 130, 255);
const uint32_t kNotice = rgba(255, 200, 80, 255);

const char kWaiting[] = N_("WAITING FOR THE MEMORY CARD:\nTHE SAVE GOES ON. ESC: CANCEL.");
const char kHint1[] = N_("UP/DOWN CHOOSE   ENTER SELECT   ESC CLOSE");
/* (%s: the controller's names of its south and east buttons - CROSS,
 * CIRCLE; A, B... - gamepad::button_names) */
const char kHint2[] = N_("PAD: D-PAD, %s SELECT, %s CLOSE");

/* The lines of text and the longest (PC 2026-11-16: in cells - a Japanese
 * character two -, the text translated). */
void text_extent(const char *t, int *rows, int *cols)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", t);
    int n = 0;
    for (char *line = buf; line != nullptr; n++) {
        char *next = strchr(line, '\n');
        if (next != nullptr)
            *next++ = '\0';
        *cols = std::max(*cols, overlay::text_cells(line));
        line = next;
    }
    *rows = std::max(*rows, n);
}

/* Each line of `t' from y down. */
void draw_lines(float x, float y, int s, float lineH, const char *t, uint32_t color)
{
    char buf[1024];                             /* PC (2026-11-16): 256 before (UTF-8 translations) */
    snprintf(buf, sizeof(buf), "%s", t);
    for (char *line = buf; line != nullptr; y += lineH) {
        char *next = strchr(line, '\n');
        if (next != nullptr)
            *next++ = '\0';
        overlay::text(x, y, s, line, color);
        line = next;
    }
}

void build(int bw, int bh)
{
    /* PC (2026-11-16): the text in the interface's language (platform/ui_text.h),
     * its widths in cells (overlay::text_cells), the buffers for UTF-8 */
    const char *const hint1 = tr(kHint1);
    int helpRows = 1, cols = overlay::text_cells(hint1);
    for (const Item &it : kItems) {
        text_extent(tr(it.help), &helpRows, &cols);
        cols = std::max(cols, overlay::text_cells(tr(it.label)));     /* (a translation may be the widest) */
    }
    text_extent(tr(kWaiting), &helpRows, &cols);
    char hint2[256];                            /* PC (2026-11-07): the controller's button names */
    const gamepad::ButtonNames names = gamepad::button_names();
    snprintf(hint2, sizeof(hint2), tr(kHint2), tr(names.south), tr(names.east));
    cols = std::max(cols, overlay::text_cells(hint2));
    /* title, a gap, the items, a gap, the help, a gap, two hints */
    const int rows = kItemCount + helpRows + 6;
    const int s = std::max(1, std::min({bh / 360, (bw - 16) / (cols * 6 + 20), (bh - 16) / (rows * 12 + 20)}));
    const float cw = 6.0f * s, lineH = 12.0f * s, pad = 10.0f * s;
    const float pw = cols * cw + 2.0f * pad;
    const float ph = pad + lineH * rows + pad;
    const float px = std::floor(((float)bw - pw) * 0.5f), py = std::floor(((float)bh - ph) * 0.5f);
    const float b = (float)std::max(1, s / 2);
    overlay::rect(px, py, pw, ph, kPanel);
    overlay::rect(px, py, pw, b, kFrame);
    overlay::rect(px, py + ph - b, pw, b, kFrame);
    overlay::rect(px, py, b, ph, kFrame);
    overlay::rect(px + pw - b, py, b, ph, kFrame);

    const float x0 = px + pad, x1 = px + pw - pad;
    float y = py + pad;
    overlay::text(x0, y, s, "GT3 PC", kTitle);
    char key[32];
    input_map::hotkey_text(input_map::kHotSystemMenu, key, sizeof(key));
    overlay::text(x1 - overlay::text_width(key, s), y, s, key, kHint);
    y += 2.0f * lineH;

    for (int i = 0; i < kItemCount; i++) {
        const bool sel = i == g_sel;
        if (sel)
            overlay::rect(x0 - 4.0f * s, y - 3.0f * s, x1 - x0 + 8.0f * s, lineH, kHighlight);
        overlay::text(x0, y, s, tr(kItems[i].label), sel ? kLabelSel : kLabel);
        y += lineH;
    }
    y += lineH;
    if (g_pending != ACT_NONE)
        draw_lines(x0, y, s, lineH, tr(kWaiting), kNotice);
    else
        draw_lines(x0, y, s, lineH, tr(kItems[g_sel].help), kHelp);
    y += (float)(helpRows + 1) * lineH;
    overlay::text(x0, y, s, hint1, kHint);
    y += lineH;
    overlay::text(x0, y, s, hint2, kHint);
}

} // namespace

void toggle()
{
    std::lock_guard<std::mutex> l(g_lock);
    if (g_shown.load()) {
        close_menu();
        return;
    }
    g_sel = 0;                                  /* Back: an Enter right after Esc does no harm */
    g_pending = ACT_NONE;
    g_padSync = true;
    g_shown = true;
}

void hide()
{
    std::lock_guard<std::mutex> l(g_lock);
    close_menu();
}

bool shown()
{
    return g_shown.load(std::memory_order_relaxed);
}

void key(int vk, bool repeat)
{
    std::lock_guard<std::mutex> l(g_lock);
    switch (vk) {
    case VK_UP: move(-1); break;
    case VK_DOWN: move(1); break;
    case VK_RETURN:
    case VK_SPACE:
        if (!repeat)
            choose();
        break;
    case VK_ESCAPE:
    case VK_BACK:
        if (!repeat)
            close_menu();
        break;
    }
}

void present(ID3D11RenderTargetView *bb)
{
    if (!g_shown.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_pending != ACT_NONE && !memcard::busy()) {
        const Action a = g_pending;
        g_pending = ACT_NONE;
        act(a);
    }
    poll_pads();
    if (bb == nullptr || !g_shown.load() || !overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    build(bw, bh);
    overlay::end(bb);
}

bool restart_requested()
{
    return g_restart.load();
}

bool restart_as_new_game()
{
    return g_restartNewGame.load();
}

void set_new_game_run()
{
    g_newGameRun = true;
}

bool new_game_run()
{
    return g_newGameRun;
}

} // namespace system_menu
