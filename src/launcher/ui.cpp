/* ui.cpp - the launcher window (ui.h).
 *
 * A small fixed-size window over the launcher's art (launcher_bg.png, a
 * resource decoded by WIC and scaled to the window: draw_background): seven
 * tabs - Play, a row per version (USA, Europe, Japan: install.h) with its
 * state and a Play button, and the data-only extractions; Display,
 * Graphics, Audio, Game, every setting of gt3pc.ini the game's own settings
 * menu (F2) has, and the launcher's display ones; Controls, the control
 * mappings (controls.h); Debug, the debug console - on a darkened panel,
 * each tab a page that scrolls when it holds more than fits (panel.h), a
 * status line, a progress bar shown while installing, and the buttons
 * Install game files... / Exit.  PC (2026-11-14): the Play tab has two
 * sub-tabs, Installed (the versions' rows; Install game files... is there
 * now) and Disc (the disc images played straight from their files); the mod
 * and texture managers' buttons are right of the panel.
 * Plain Win32 with the system's controls and message font, per-monitor DPI
 * aware (v2, from the manifest): every size below is in 96-DPI units and is
 * scaled to the window's DPI.  The install runs on a worker thread and
 * reports through posted messages, so the window stays responsive.  So does
 * the conversion of an installed version's music (music.h), started with
 * the window when a version needs it. */
#include "ui.h"
#include "controls.h"
#include "display.h"
#include "gameconf.h"
#include "databases.h"
#include "disc_files.h"                         /* PC (2026-11-15): a disc image's songs */
#include "disc_list.h"                          /* PC (2026-11-14): the Play tab's Disc sub-tab */
#include "engines.h"                            /* PC (2026-11-14): an engine's own settings file */
#include "install.h"
#include "mod_manager.h"
#include "music.h"
#include "panel.h"
#include "song_manager.h"
#include "stills.h"
#include "image_packs.h"
#include "resource.h"
#include "texture_manager.h"                    /* PC (2026-11-13): the Play tab's Textures... */
#include "save_manager.h"                       /* PC (2026-11-15): Saves..., under Textures... */
#include "platform/save_folders.h"
#include "util.h"
#include "platform/hud_colors.h"
#include "platform/input_map.h"                 /* PC (2026-11-09): the clutch mode's popup */
#include "platform/ui_text.h"                   /* PC (2026-11-16): the interface's languages */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {
namespace {

const wchar_t kClassName[] = L"GT3LauncherWindow";
const wchar_t kTitle[] = N_(L"Gran Turismo 3 A-Spec - Launcher");
const wchar_t kAppName[] = L"Gran Turismo 3 A-Spec";   /* message box captions */
const wchar_t kGameExe[] = L"gt3pc.exe";

/* Layout, in 96-DPI units. */
constexpr int kClientW = 640, kClientH = 360;
constexpr int kMargin = 16;
constexpr int kButtonH = 28, kButtonW = 100, kInstallW = 170, kButtonGap = 8;
constexpr int kStatusH = 36;    /* two lines of the message font */
constexpr int kStatusW = 440;   /* (left of the art's logo, bottom right) */
constexpr int kProgressH = 14;
/* The tabs: a row of headers over the settings' panel. */
constexpr int kTabY = 12, kTabW = 86, kTabH = 30;
constexpr int kTabs = 7;
/* The settings: a label and a drop-down list (or a slider and its value) per
 * row, on the panel (each tab's page: more rows than kViewRows scroll). */
constexpr int kRowH = 30, kLabelW = 150, kComboW = 300, kComboListH = 240;
constexpr int kSliderH = 26, kValueW = 140, kValueGap = 6;
constexpr int kPanelPad = 12;
constexpr int kViewRows = 6;
constexpr int kPanelY = kTabY + kTabH;
constexpr int kPanelW = kPanelPad + kLabelW + kComboW + kPanelPad;
constexpr int kPanelH = kPanelPad + kViewRows * kRowH + kPanelPad / 2;
/* The panel darkens the art behind the tabs and the settings, for the text. */
constexpr int kPanelShade = 150;    /* of 256: the art kept */

constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

const wchar_t *const kTabNames[kTabs] = {N_(L"Play"), N_(L"Display"), N_(L"Graphics"), N_(L"Audio"),
                                         N_(L"Game"), N_(L"Controls"), N_(L"Debug")};
enum { TAB_PLAY, TAB_DISPLAY, TAB_GRAPHICS, TAB_AUDIO, TAB_GAME, TAB_CONTROLS, TAB_DEBUG };

/* The Play tab: a version a row (its name and state, a Play button at the
 * right), the mod manager's button and what it has on (mod_manager.h), then
 * the data-only extractions.  PC (2026-11-13): beside the mod manager's
 * button the texture manager's (texture_manager.h), what each has on in two
 * lines right of them (the buttons 100 wide, were 150: the lines' longest,
 * "No texture folders are installed", is 201 of the 215 left at 96 DPI). */
constexpr int kVersionRowH = 48, kPlayW = 90;
/* PC (2026-11-14, user request: "Play tab gets sub tabs: Installed tab, and
 * Disc tab ... Mods... and Textures... button need to be moved outside of the
 * list and into the open space on the right, the buttons and their infos
 * vertically layed out"): the Play tab has two sub-tabs over its page -
 * Installed, the versions' rows and the extractions (the tab as it was), and
 * Disc, the disc images played straight from their files (disc_list.h,
 * install.h prepare_disc), a row each: its release, its path under it, Play
 * and Remove (a small X with a tooltip: the two as wide as a version's Play,
 * so the longest release, "Gran Turismo 3 A-Spec (Europe, SCES-50294)", 322
 * px at 96 DPI, fits beside them, a scroll bar too) at the right.  Each
 * sub-tab's own button is at the right of the
 * sub-tabs' line: Install game files... (was at the window's bottom left) and
 * Play from disc....  The two managers' buttons are no longer on the page:
 * right of the panel, in the art's open space there (kSideGap from the panel
 * to the window's margin), one under the other, what each has on under its
 * button. */
constexpr int kDiscPlayW = 58, kDiscRemoveW = 28, kDiscGap = 4;
const wchar_t kDiscRemoveTip[] = N_(L"Remove from this list (the file stays)");
constexpr int kSideGap = 10;
const wchar_t *const kPlaySubNames[2] = {N_(L"Installed"), N_(L"Disc")};
enum { PLAY_INSTALLED, PLAY_DISC };
/* (PC 2026-11-15, user request: "only add the disc without automatically
 * starting it. The play button in the disc tab's list item should be the only
 * way to start it": the sub-tab's button is Add disc..., which only adds) */
const wchar_t kDiscNoneText[] =
    N_(L"No disc image has been added yet.\n\n\"Add disc...\" adds a Gran Turismo 3 disc image (USA, Europe, "
       L"Japan) straight from its file, without installing it, and keeps it in this list.");
/* The Audio tab: two sub-tabs over its page - Settings (its rows), Songs (the
 * music's notices of this run, the race songs of each version, the song
 * manager's button: song_manager.h).  The Game tab: six, General, HUD,
 * Cameras (PC 2026-11-08: the camera settings, as the F2 menu's CAMERAS;
 * user request), Extras, Surfaces and Challenges (PC 2026-11-14: the
 * challenge modes, as the F2 menu's CHALLENGE MODES; user request), rows
 * each (Row::sub). */
/* (a sub-tab is as wide as its name plus kSubTabPad each side, kSubTabMinW at
 * least: sub_tab_width; PC 2026-11-08 - five Game sub-tabs of one fixed width
 * ran off the page, user report.  PC 2026-11-14: the Game tab's six at their
 * names' widths did too - 503 of 457 pixels at 96 DPI - so its padding is
 * narrower, as far as kSubTabMinPad, until they fit: game_sub_tab_pad) */
constexpr int kSubTabMinW = 56, kSubTabPad = 14, kSubTabMinPad = 6, kSubTabH = 28, kSongsLineH = 18,
              kSongsOpenW = 170;
const wchar_t *const kAudioSubNames[2] = {N_(L"Settings"), N_(L"Songs")};
enum { GAME_GENERAL, GAME_HUD, GAME_CAMERAS, GAME_EXTRAS, GAME_SURFACES, GAME_CHALLENGES, kGameSubs };
const wchar_t *const kGameSubNames[kGameSubs] = {N_(L"General"), N_(L"HUD"), N_(L"Cameras"), N_(L"Extras"),
                                                 N_(L"Surfaces"), N_(L"Challenges")};

enum : int {
    IDC_INSTALL = 1001,
    IDC_CANCEL,
    IDC_EXIT,
    IDC_STATUS,
    IDC_PROGRESS,
    IDC_MODE,
    IDC_RES,
    IDC_RATE,
    IDC_SPAN,
    IDC_SPLIT,
    IDC_PLAY_TIME,          /* the time played (the bottom row's middle) */
    IDC_MODS_OPEN,          /* the Play tab's mod manager button and its text */
    IDC_MODS_TEXT,
    IDC_TEXTURES_OPEN,      /* PC (2026-11-13): the texture manager's button and its text */
    IDC_TEXTURES_TEXT,
    IDC_SAVES_OPEN,         /* PC (2026-11-15): the save manager's button and its text */
    IDC_SAVES_TEXT,
    IDC_CAP_FPS,            /* the frame cap's two fields (add_cap_row): the rate, the time, */
    IDC_CAP_MS,
    IDC_CAP_FPS_UD,         /* their up / down arrows and their labels */
    IDC_CAP_MS_UD,
    IDC_CAP_FPS_LABEL,
    IDC_CAP_MS_LABEL,
    IDC_TAB0 = 1100,        /* .. + kTabs - 1 */
    IDC_PAGE0 = 1110,       /* the tabs' pages */
    IDC_VERSION_PLAY0 = 1180,   /* the Play tab's buttons, names and states, by version */
    IDC_VERSION_NAME0 = 1184,
    IDC_VERSION_STATE0 = 1188,
    IDC_GAME_SUB0 = 1170,   /* .. + kGameSubs - 1: the Game tab's sub-tabs: General, HUD, Cameras, Extras, Surfaces,
                               Challenges */
    IDC_EXTRACTIONS = 1192,
    IDC_AUDIO_SUB0 = 1194,  /* the Audio tab's sub-tabs: Settings, Songs */
    IDC_SONGS_TEXT = 1196,  /* the Songs sub-tab's text and button */
    IDC_SONGS_OPEN = 1197,
    IDC_OPTION0 = 1200,    /* the options' lists (or sliders), by index */
                            /* 1400..1699: the Controls tab's (controls.h) */
    IDC_VALUE0 = 1700,      /* the sliders' values, the colours' Default buttons, by option index */
    IDC_LABEL0 = 2000,      /* the rows' labels */
    IDC_DEFAULT0 = 2400,    /* the sliders' Default buttons, by option index */
    IDC_PLAY_SUB0 = 3000,   /* PC (2026-11-14): the Play tab's sub-tabs: Installed, Disc */
    IDC_DISC_OPEN = 3002,   /* the Disc sub-tab's Play from disc..., its text with no disc image */
    IDC_DISC_NONE = 3003,
    IDC_DISC_PLAY0 = 3100,  /* its rows' buttons and texts, by the image's place in the list */
    IDC_DISC_REMOVE0 = 3200,
    IDC_DISC_NAME0 = 3300,
    IDC_DISC_PATH0 = 3400,
};

enum : UINT {
    WM_APP_PHASE = WM_APP + 1,   /* lParam: std::wstring* (the receiver deletes it) */
    WM_APP_PROGRESS,             /* g.done / g.total changed */
    WM_APP_DONE,                 /* the worker finished: join it, then read g.result */
    WM_APP_DISC_REMOVE,          /* PC (2026-11-14): wParam: the disc image to take out of the list (posted by its
                                    Remove button: the row's controls are destroyed) */
};

/* An entry of an option's list: what it shows, the ini value it writes. */
struct Choice {
    std::wstring text, value;
};

/* A setting of gt3pc.ini written as one key (the game reads it at start; its
 * settings menu writes the same keys): its tab, label, key, default (the
 * game's, for a missing key) and the values offered.  A value in the file
 * that none of them stands for gets an entry of its own, so it is kept.
 * A slider instead (step > 0, no choices): min .. max by step, written with
 * `decimals', shown as text(v) beside it; a value in the file between its
 * steps or out of its range is shown as it is (the slider at the nearest)
 * until the slider moves; resettable: a Default button too, that removes its
 * key.  A colour instead (color): a button of the colour that opens the
 * colour dialog and a Default button, its key written as RRGGBB and removed
 * when it is the game's own (platform/hud_colors.h). */
struct Option {
    int tab;
    int sub = 0;                            /* the tab's sub-tab (the Game tab's: GAME_GENERAL ..) */
    const wchar_t *label, *section, *key, *def;
    std::vector<Choice> choices;
    HWND combo = nullptr;                   /* the list, or the slider */
    double min = 0, max = 0, step = 0;
    int decimals = 0;
    std::wstring (*text)(double v) = nullptr;
    HWND value = nullptr;                   /* the slider's value */
    double cur = 0;                         /* the slider's value in the file */
    bool noZero = false;                    /* a slider at 0 stands for the next step (the course scale: 5 %) */
    bool resettable = false;                /* a slider with a Default button */
    bool color = false;                     /* a colour: combo its button, value its Default button */
    uint32_t rgb = 0, gameRgb = 0;          /* a colour's 0xRRGGBB, the game's own */
    /* PC (2026-11-14): a whole number typed into a field (combo: the field),
     * min .. max, written when the field is left or Enter is pressed (the
     * custom starting credits) */
    bool number = false;
    /* PC (2026-11-09): shown only while the key whenKey of its section is
     * whenValue (the H-shifter with the clutch mode); nullptr: always */
    const wchar_t *whenKey = nullptr, *whenValue = nullptr;
};

/* A row of a tab: its label and its list (or slider and value, or colour and
 * Default button: button); reset: a slider's Default button; sub: the tab's
 * sub-tab it shows in (Option::sub). */
struct Row {
    int tab;
    HWND label, combo;
    HWND value = nullptr;
    HWND reset = nullptr;
    int sub = 0;
    bool cap = false;                       /* the frame cap's row (add_cap_row): combo and value its two fields, */
    std::vector<HWND> extra;                /* extra their labels and up / down arrows */
    bool button = false;
    bool number = false;                    /* PC (2026-11-14): combo is a number's field (Option::number) */
    int option = -1;                        /* PC (2026-11-09): its option (g.options; -1 none) */
};

/* PC (2026-11-14): a disc image's row in the Play tab's Disc sub-tab. */
struct DiscRow {
    HWND name = nullptr, path = nullptr, play = nullptr, remove = nullptr;
};

struct Window {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr;
    HWND install = nullptr, cancel = nullptr, exit = nullptr, status = nullptr, progress = nullptr;
    int statusX = 0, statusBottom = 0;      /* PC (2026-11-16): where the status line ends (place_status) */
    HWND tabs[kTabs] = {};
    HWND pages[kTabs] = {};                 /* each tab's controls (panels: panel.h) */
    int tab = TAB_PLAY;
    /* The Play tab, by version (install::Version) */
    HWND versionPlay[install::kVersions] = {}, versionName[install::kVersions] = {},
         versionState[install::kVersions] = {};
    HWND extractionsLabel = nullptr;
    HWND modsOpen = nullptr, modsText = nullptr;
    HWND texturesOpen = nullptr, texturesText = nullptr;    /* PC (2026-11-13): the texture manager's */
    HWND savesOpen = nullptr, savesText = nullptr;          /* PC (2026-11-15): the save manager's */
    /* PC (2026-11-14): the Play tab's sub-tabs (Installed, Disc); the Disc
     * sub-tab: its button, its text with no disc image, the images of
     * gt3pc.ini [Discs] (the last played first), whether each file is there
     * (as of the last refresh) and their rows */
    int playSub = PLAY_INSTALLED;
    HWND playSubTab[2] = {};
    HWND discOpen = nullptr, discNone = nullptr;
    HWND tip = nullptr;                     /* the rows' Remove buttons' tooltip */
    std::vector<disc_list::Disc> discs;
    std::vector<char> discThere;
    std::vector<DiscRow> discRows;
    HWND playTime = nullptr;                /* gt3pc.ini [Stats] PlayTime, in hours */
    int lastVersion = 0;                    /* gt3pc.ini [Game] Version: the last played */
    std::vector<Row> rows;                  /* in tab order (the creation order: the Tab key's) */
    std::vector<Option> options;
    /* The display settings (gt3pc.ini [Display], display.h): their lists,
     * what each list entry stands for. */
    HWND mode = nullptr, res = nullptr, rate = nullptr, span = nullptr, split = nullptr;
    HWND capFps = nullptr, capMs = nullptr;  /* the frame cap's fields (add_cap_row) */
    display::Settings disp;
    std::vector<display::Size> resSizes;    /* by entry ({0, 0}: the desktop's) */
    std::vector<int> rateValues;            /* by entry (0: the default) */
    std::vector<std::wstring> spanValues;   /* by entry: SpanMonitors (empty: off) */
    std::vector<std::wstring> splitValues;  /* by entry: SplitMonitor (empty: auto) */
    HFONT font = nullptr, tabFont = nullptr;
    HICON icon_big = nullptr, icon_small = nullptr;
    /* The background: the art at the window's size with the panel darkened
     * (bg, selected into bgDC), and a brush of it for the labels. */
    HBITMAP bg = nullptr;
    HDC bgDC = nullptr;
    HGDIOBJ bgOld = nullptr;
    HBRUSH bgBrush = nullptr;
    UINT dpi = 96;
    int default_id = 0;      /* the button Enter presses (DM_GETDEFID) */
    HWND last_focus = nullptr;
    std::wstring folder;   /* the launcher's folder: gt3pc.exe and the data folder live here */
    /* PC (2026-11-14, engines.h): the folder of the gt3pc.ini read and
     * written - the launcher's own, or (an engine started by another
     * launcher's selector) that launcher's: the settings are one file for
     * every engine.  What is an engine's own is in gt3pc_engine.ini of its
     * folder (engine_ini_path): the mod folders, the disc images played, the
     * version or disc image played last. */
    std::wstring shared;

    /* The Audio tab's sub-tabs and Songs page; the music's notices of this
     * run (the conversion, new songs, files not added), unread until the
     * Songs page is shown (a red dot on the tab and the sub-tab). */
    int audioSub = 0;
    HWND audioSubTab[2] = {};
    HWND songsText = nullptr, songsOpen = nullptr;
    /* The Game tab's sub-tabs (General, HUD, Extras, Surfaces). */
    int gameSub = 0;
    HWND gameSubTab[kGameSubs] = {};
    std::vector<std::wstring> musicNotes;
    bool musicUnread = false;

    /* The install (or, converting: the music's conversion, music.h). */
    bool installing = false;
    bool converting = false;
    bool close_when_done = false;
    std::thread worker;
    std::atomic<bool> cancel_requested{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::wstring phase;
    bool copying = false;
    install::Result result = install::Result::ok;
    std::wstring result_message;
} g;

int px(int dip) { return MulDiv(dip, (int)g.dpi, 96); }

/* ---- the background ------------------------------------------------------ */

/* The art (the resource IDR_BACKGROUND) decoded and scaled to w x h into
 * top-down 32-bit pixels; false when it could not be (the window stays
 * black). */
bool load_art(int w, int h, uint32_t *pixels)
{
    HRSRC r = FindResourceW(g.inst, MAKEINTRESOURCEW(IDR_BACKGROUND), RT_RCDATA);
    HGLOBAL res = r ? LoadResource(g.inst, r) : nullptr;
    const void *data = res ? LockResource(res) : nullptr;
    const DWORD size = r ? SizeofResource(g.inst, r) : 0;
    if (data == nullptr || size == 0)
        return false;
    IWICImagingFactory *wic = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory((BYTE *)data, size)) &&
              SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
              SUCCEEDED(scaler->Initialize(conv, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
              SUCCEEDED(scaler->CopyPixels(nullptr, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)pixels));
    for (IUnknown *u : {(IUnknown *)scaler, (IUnknown *)conv, (IUnknown *)frame, (IUnknown *)decoder,
                        (IUnknown *)stream, (IUnknown *)wic})
        if (u)
            u->Release();
    return ok;
}

/* The background at the window's size: the art, the panel behind the tabs
 * and the settings darkened (kPanelShade).  Made again at each DPI. */
void build_background()
{
    const int w = px(kClientW), h = px(kClientH);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;             /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr)
        return;
    uint32_t *p = (uint32_t *)bits;
    if (!load_art(w, h, p))
        memset(p, 0, (size_t)w * h * 4);
    const int x0 = px(kMargin), y0 = px(kTabY), x1 = x0 + px(kPanelW), y1 = px(kPanelY) + px(kPanelH);
    for (int y = std::max(0, y0); y < std::min(h, y1); y++) {
        for (int x = std::max(0, x0); x < std::min(w, x1); x++) {
            const uint32_t c = p[(size_t)y * w + x];
            const uint32_t r = ((c >> 16) & 0xFF) * kPanelShade >> 8, gr = ((c >> 8) & 0xFF) * kPanelShade >> 8,
                           b = (c & 0xFF) * kPanelShade >> 8;
            p[(size_t)y * w + x] = 0xFF000000u | (r << 16) | (gr << 8) | b;
        }
    }
    if (g.bgDC == nullptr)
        g.bgDC = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(g.bgDC, bmp);
    if (g.bg != nullptr)
        DeleteObject(g.bg);
    else
        g.bgOld = old;
    g.bg = bmp;
    if (g.bgBrush != nullptr)
        DeleteObject(g.bgBrush);
    g.bgBrush = CreatePatternBrush(bmp);
}

/* Draws the window's background into `dc` over `client`. */
void draw_background(HDC dc, const RECT &client)
{
    if (g.bgDC == nullptr) {
        FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }
    BitBlt(dc, client.left, client.top, client.right - client.left, client.bottom - client.top, g.bgDC, client.left,
           client.top, SRCCOPY);
}

/* A child's place in the client area (the background under it). */
POINT child_origin(HWND c)
{
    POINT o = {0, 0};
    MapWindowPoints(c, g.wnd, &o, 1);
    return o;
}

/* A label or the status line: its text drawn over the background. */
bool is_text(HWND c)
{
    if (c == g.status || c == g.extractionsLabel || c == g.songsText || c == g.modsText || c == g.texturesText ||
        c == g.savesText || c == g.playTime || c == g.discNone || controls::is_text(c))
        return true;
    for (const DiscRow &r : g.discRows)
        if (c == r.name || c == r.path)
            return true;
    for (int v = 0; v < install::kVersions; v++)
        if (c == g.versionName[v] || c == g.versionState[v])
            return true;
    /* (a slider takes the same brush for its background) */
    return std::any_of(g.rows.begin(), g.rows.end(), [&](const Row &r) {
        return r.label == c || (r.value != nullptr && (r.value == c || r.combo == c));
    });
}

/* A tab's (or sub-tab's) header: its name over the background, the chosen
 * one bright and underlined in red; `dot': a red dot after the name (the
 * music's notices unread). */
void draw_header(const DRAWITEMSTRUCT *d, const wchar_t *name, bool chosen, bool dot)
{
    const RECT &r = d->rcItem;
    const POINT o = child_origin(d->hwndItem);
    if (g.bgDC != nullptr)
        BitBlt(d->hDC, 0, 0, r.right - r.left, r.bottom - r.top, g.bgDC, o.x, o.y, SRCCOPY);
    HBRUSH red = CreateSolidBrush(RGB(214, 22, 22));
    if (chosen) {
        RECT bar = {r.left + px(10), r.bottom - px(4), r.right - px(10), r.bottom - px(1)};
        FillRect(d->hDC, &bar, red);
    }
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, chosen ? RGB(255, 255, 255) : RGB(168, 168, 168));
    HGDIOBJ old = SelectObject(d->hDC, g.tabFont ? g.tabFont : g.font);
    RECT t = r;
    t.bottom -= px(3);
    DrawTextW(d->hDC, name, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (dot) {
        RECT m = t;
        DrawTextW(d->hDC, name, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int cx = (t.left + t.right + (m.right - m.left)) / 2 + px(6), cy = (t.top + t.bottom) / 2 - px(5);
        HGDIOBJ ob = SelectObject(d->hDC, red);
        HGDIOBJ op = SelectObject(d->hDC, GetStockObject(NULL_PEN));
        Ellipse(d->hDC, cx - px(3), cy - px(3), cx + px(4), cy + px(4));
        SelectObject(d->hDC, op);
        SelectObject(d->hDC, ob);
    }
    SelectObject(d->hDC, old);
    DeleteObject(red);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -px(3), -px(3));
        DrawFocusRect(d->hDC, &f);
    }
}

void draw_tab(const DRAWITEMSTRUCT *d)
{
    const int i = (int)d->CtlID - IDC_TAB0;
    draw_header(d, tr(kTabNames[i]), i == g.tab, i == TAB_AUDIO && g.musicUnread);
}

/* A sub-tab's width on `page': its name in the headers' font with kSubTabPad
 * each side (the underline's inset and some air), kSubTabMinW at least. */
int text_width(HWND page, const wchar_t *text, HFONT font = nullptr)
{
    HDC dc = GetDC(page);
    HGDIOBJ old = SelectObject(dc, font ? font : g.font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
    SelectObject(dc, old);
    ReleaseDC(page, dc);
    return (int)size.cx;
}

int sub_tab_width(HWND page, const wchar_t *name, int pad = -1)
{
    if (pad < 0)
        pad = px(kSubTabPad);
    return std::max(px(kSubTabMinW), text_width(page, name, g.tabFont ? g.tabFont : g.font) + 2 * pad);
}

/* PC (2026-11-14): the Game tab's sub-tabs' padding: kSubTabPad, or less
 * (kSubTabMinPad at least) for them all to fit in the page beside a scroll
 * bar (always the same, whether the sub-tab shown scrolls or not) */
int game_sub_tab_pad()
{
    const int room = px(kPanelW) - GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi) - 2 * px(kPanelPad);
    int pad = px(kSubTabPad);
    for (; pad > px(kSubTabMinPad); pad--) {
        int total = 0;
        for (int s = 0; s < kGameSubs; s++)
            total += sub_tab_width(g.pages[TAB_GAME], tr(kGameSubNames[s]), pad);
        if (total <= room)
            break;
    }
    return pad;
}

/* ---- state ------------------------------------------------------------------ */

install::InstallState version_state(int v)
{
    return install::install_state(g.folder, install::version_folder((install::Version)v));
}

/* The status line, two lines at most: what to do. */
std::wstring state_text()
{
    /* PC (2026-11-14): the Disc sub-tab's */
    if (g.playSub == PLAY_DISC)
        return g.discs.empty() ? tr(L"No disc image has been added yet.\n"
                                    L"Choose \"Add disc...\" and pick a Gran Turismo 3 disc image.")
                               : tr(L"Ready.\n"
                                    L"\"Add disc...\" adds another disc image; its Play plays it without installing it.");
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok)
            return tr(L"Ready.\n"
                      L"\"Install game files...\" installs another version from its disc image.");
    if (install::has_volume(g.folder))
        return tr(L"The game files need unpacking from gt3.vol (installed by an earlier version).\n"
                  L"Choose \"Install game files...\" to unpack them.");
    return tr(L"No version is installed.\n"
              L"Choose \"Install game files...\" and pick a Gran Turismo 3 disc image.");
}

/* A version's state in the Play tab. */
std::wstring version_text(int v)
{
    const std::wstring data = install::version_folder((install::Version)v);
    switch (install::install_state(g.folder, data)) {
    case install::InstallState::ok:
        return trf(L"Installed from %s.", install::installed_release(g.folder, data).c_str());
    case install::InstallState::damaged:
        return trf(L"Incomplete (%s): install it again from its disc image.", data.c_str());
    default:
        return tr(L"Not installed: \"Install game files...\" with its disc image.");
    }
}

/* The data-only extractions, as the Play tab lists them ("" with none). */
std::wstring extractions_text(int *lines)
{
    std::wstring t;
    *lines = 0;
    for (const install::Extraction &e : install::extractions(g.folder)) {
        t += (t.empty() ? tr(L"Data only, not playable:") : L"") + std::wstring(L"\n  ") + e.release + L" (" + e.data + L")";
        *lines += *lines == 0 ? 2 : 1;
    }
    return t;
}

std::wstring ini_path();
std::wstring engine_ini_path();

/* The mod folders of the engine's own gt3pc_engine.ini [Mods] (PC 2026-11-14:
 * were gt3pc.ini's): how many are on. */
std::wstring mods_text()
{
    const std::vector<mod_manager::Mod> mods = mod_manager::load(engine_ini_path());
    const long on = (long)std::count_if(mods.begin(), mods.end(), [](const mod_manager::Mod &m) { return m.on; });
    if (mods.empty())
        return tr(L"No mod folders are installed");
    if (on == 0)
        return tr(L"No mod folders enabled");
    return on == 1 ? std::wstring(tr(L"1 mod folder")) : trf(L"%ld mod folders", on);
}

/* PC (2026-11-13): the texture folders of gt3pc.ini [Textures]: how many are
 * on (as the mod folders'). */
std::wstring textures_text()
{
    const std::vector<texture_manager::Pack> packs = texture_manager::load(ini_path());
    const long on =
        (long)std::count_if(packs.begin(), packs.end(), [](const texture_manager::Pack &p) { return p.on; });
    if (packs.empty())
        return tr(L"No texture folders are installed");
    if (on == 0)
        return tr(L"No texture folders enabled");
    return on == 1 ? std::wstring(tr(L"1 texture folder")) : trf(L"%ld texture folders", on);
}

/* PC (2026-11-15): the engine's save folders (platform/save_folders.h):
 * saves\<engine>\ beside the gt3pc.ini in use, and what each memory card slot
 * holds (gt3pc_engine.ini [Saves]). */
std::wstring saves_dir();
std::wstring saves_text()
{
    return save_manager::summary(saves_dir(), engine_ini_path());
}

/* gt3pc.ini [Stats] PlayTime (seconds; platform/play_time.h) in hours. */
std::wstring play_time_text()
{
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"Stats", L"PlayTime", L"0", v, 32, ini_path().c_str());
    return trf(L"Time played: %.1f hours", (double)_wcstoui64(v, nullptr, 10) / 3600.0);
}

/* The chosen tab's page shown, the others hidden (the focus, if it was in
 * one of them, to the tab's header); the Controls tab told (its devices are
 * read while it is shown). */
void show_tab(int t)
{
    g.tab = t;
    HWND f = GetFocus();
    bool lost = false;
    for (int i = 0; i < kTabs; i++) {
        if (i != t && f != nullptr && IsChild(g.pages[i], f))
            lost = true;
        ShowWindow(g.pages[i], i == t ? SW_SHOW : SW_HIDE);
    }
    controls::show(t == TAB_CONTROLS);
    if (t == TAB_AUDIO && g.audioSub == 1)
        g.musicUnread = false;              /* (the notices seen) */
    for (HWND h : g.tabs)
        InvalidateRect(h, nullptr, FALSE);
    for (HWND h : g.audioSubTab)
        InvalidateRect(h, nullptr, FALSE);
    for (HWND h : g.gameSubTab)
        InvalidateRect(h, nullptr, FALSE);
    for (HWND h : g.playSubTab)
        InvalidateRect(h, nullptr, FALSE);
    /* PC (2026-11-08, user report): the focus goes to the tab's header on
     * every change - it stayed on a Play button out of sight, and Enter on
     * another tab started the game (the default button goes with the tab
     * too: DM_GETDEFID) */
    (void)lost;
    SetFocus(g.tabs[t]);
}

/* PC (2026-11-15, user request: "If the user only links a disc with no
 * installation, the song manager needs to still work"): the disc images of
 * the list whose data folder is readied (install::prepare_disc: it has its
 * marker) and whose file is there - their songs are the song manager's and
 * the Songs page's too, as an installed version's (music.h reads what the
 * folder does not have from the image). */
bool disc_there(const disc_list::Disc &d);      /* (below) */

struct SongFolder {
    std::wstring name, data;
};

/* "Disc image: Gran Turismo 3 A-Spec (USA, SCUS-97102)" */
std::wstring disc_song_name(const std::wstring &release)
{
    return trf(L"Disc image: %s", release.c_str());
}

std::vector<SongFolder> disc_song_folders()
{
    std::vector<SongFolder> out;
    for (const disc_list::Disc &d : g.discs) {
        if (d.folder.empty() || !disc_there(d))
            continue;
        const std::wstring data = g.folder + install::kDiscsFolder + L"\\" + d.folder;
        if (install::marker_info(data, "image").empty())
            continue;
        out.push_back({disc_song_name(!d.name.empty() ? d.name : d.folder), data});
    }
    return out;
}

/* A disc image's race songs: its folder's ads.txt (read each time, as an
 * installed version's), else the image's list - read once and kept: it does
 * not change until the folder has an ads.txt. */
size_t disc_race_songs(const std::wstring &data)
{
    static std::map<std::wstring, size_t> s_fromImage;
    const DWORD a = GetFileAttributesW((util::with_slash(data) + L"bgm\\ads.txt").c_str());
    const bool own = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    if (!own) {
        const auto it = s_fromImage.find(data);
        if (it != s_fromImage.end())
            return it->second;
    }
    spu2::AdsList list;
    spu2::ads_list_parse(music::list_text(data), &list, nullptr);
    const size_t n = list.group[spu2::ADS_GROUP_RACE_SONGS].size();
    if (!own) {
        s_fromImage[data] = n;
        disc_files::release_all();          /* (the image let go of: it may be moved meanwhile) */
    }
    return n;
}

/* The Songs page's text: each installed version's race songs, then the
 * music's notices of this run. */
std::wstring songs_text()
{
    std::wstring t;
    for (int v = 0; v < install::kVersions; v++) {
        if (version_state(v) != install::InstallState::ok)
            continue;
        spu2::AdsList list;
        spu2::ads_list_parse(music::list_text(g.folder + install::version_folder((install::Version)v)), &list, nullptr);
        const size_t n = list.group[spu2::ADS_GROUP_RACE_SONGS].size();
        const wchar_t *name = install::version_name((install::Version)v);
        t += (t.empty() ? L"" : L"\n") +
             (n == 1 ? trf(L"%s: 1 race song", name) : trf(L"%s: %zu race songs", name, n));
    }
    /* PC (2026-11-15): the disc images added */
    for (const SongFolder &d : disc_song_folders()) {
        const size_t n = disc_race_songs(d.data);
        t += (t.empty() ? L"" : L"\n") +
             (n == 1 ? trf(L"%s: 1 race song", d.name.c_str()) : trf(L"%s: %zu race songs", d.name.c_str(), n));
    }
    if (t.empty())
        t = tr(L"No version is installed and no disc image is added.");
    t += g.musicNotes.empty() ? std::wstring(L"\n\n") + tr(L"No new songs. Add .wav / .mp3 / .ads files into the bgm folder and restart the launcher to add more") : std::wstring(L"\n");
    for (const std::wstring &n : g.musicNotes)
        t += L"\n" + n;
    return t;
}

/* PC (2026-11-09): row r is shown by its option's condition (Option::
 * whenKey: the H-shifter with the clutch mode). */
bool row_shown(const Row &r)
{
    if (r.option < 0 || r.option >= (int)g.options.size() || g.options[r.option].whenKey == nullptr)
        return true;
    const Option &o = g.options[r.option];
    /* PC (2026-11-20, user report: "sometimes users can turn on realistic +
     * clutch mode and the H-shifter UI doesn't appear" - a screenshot with
     * Realistic + Clutch in the list and no H-shifter row under it): by the
     * list the key's own row shows, as the user sees it, not gt3pc.ini read
     * again - the two can differ (a write that did not land, the file changed
     * by another program meanwhile) and the row followed the file.  The
     * file's value only for a key without a list here. */
    for (const Option &w : g.options)
        if (w.combo != nullptr && w.step <= 0 && !w.color && !w.number && _wcsicmp(w.section, o.section) == 0 &&
            _wcsicmp(w.key, o.whenKey) == 0) {
            const int i = (int)SendMessageW(w.combo, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)w.choices.size())
                return _wcsicmp(w.choices[i].value.c_str(), o.whenValue) == 0;
        }
    wchar_t v[64];
    GetPrivateProfileStringW(o.section, o.whenKey, L"", v, 64, ini_path().c_str());
    return _wcsicmp(v, o.whenValue) == 0;
}

/* PC (2026-11-14): a text's height wrapped into `width' px, in its control's
 * font (one line at least). */
int wrapped_height(HWND text, int width)
{
    wchar_t t[256] = L"";
    GetWindowTextW(text, t, 256);
    RECT r = {0, 0, width, 0};
    HDC dc = GetDC(text);
    HFONT font = (HFONT)SendMessageW(text, WM_GETFONT, 0, 0);
    HGDIOBJ old = font != nullptr ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (old != nullptr)
        SelectObject(dc, old);
    ReleaseDC(text, dc);
    return std::max((int)r.bottom, (int)tm.tmHeight);
}

/* PC (2026-11-14, user request): the mod and texture managers' buttons right
 * of the panel, in the art's open space there, one under the other; what each
 * has on under its button, wrapped (again when a manager changes its text). */
void side_layout()
{
    const int x = px(kMargin) + px(kPanelW) + px(kSideGap), w = px(kClientW) - px(kMargin) - x;
    int y = px(kPanelY) + px(kPanelPad) / 2;
    /* (PC 2026-11-15: the save manager's under the texture manager's) */
    const HWND pairs[3][2] = {{g.modsOpen, g.modsText}, {g.texturesOpen, g.texturesText}, {g.savesOpen, g.savesText}};
    /* PC (2026-11-20, user report: "adding a mod folder can glitch the
     * background behind the buttons' text underneath them" - the Textures...
     * and Saves... buttons' labels left over the texts under them): moved
     * without their old pixels (a move copies them, and these texts' art
     * depends on their place: WM_CTLCOLORSTATIC), and the column repainted
     * with its children - the window clips its children, so invalidating it
     * alone left the moved buttons and texts as copied.  Seen in the launcher
     * harness when the mods text went from one line to two and back: 3,076
     * pixels unlike a full repaint, now none. */
    for (const auto &p : pairs) {
        SetWindowPos(p[0], nullptr, x, y, w, px(kButtonH), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        y += px(kButtonH) + px(4);
        const int th = wrapped_height(p[1], w);
        SetWindowPos(p[1], nullptr, x, y, w, th, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        y += th + px(12);
    }
    RECT side = {x, px(kPanelY), x + w, px(kPanelY) + px(kPanelH)};
    RedrawWindow(g.wnd, &side, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* PC (2026-11-16): the interface's languages run longer than English.  A
 * tab's row labels' column: its widest label (and some air), kLabelW at
 * least, kLabelMaxW at most - a label wider still wraps to two lines in its
 * row (place_label). */
constexpr int kLabelMaxW = 220;

int label_column(int tab)
{
    int widest = 0;
    for (const Row &r : g.rows) {
        if (r.tab != tab || r.label == nullptr)
            continue;
        wchar_t t[256] = L"";
        GetWindowTextW(r.label, t, 256);
        widest = std::max(widest, text_width(g.pages[tab], t));
    }
    return std::max(px(kLabelW), std::min(px(kLabelMaxW), widest + px(10)));
}

void place_label(HWND page, HWND label, int x, int y, int lw)
{
    wchar_t t[256] = L"";
    GetWindowTextW(label, t, 256);
    if (text_width(page, t) + px(4) <= lw) {
        panel::place(page, label, x, y + px(4), lw, px(20));
        return;
    }
    /* two lines: as tall as they are (the font's lines at 150 % dpi are
     * more than half a row), centred on the row */
    HDC dc = GetDC(page);
    HGDIOBJ old = SelectObject(dc, g.font);
    RECT r = {0, 0, lw - px(6), 0};
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(page, dc);
    const int h = (int)r.bottom + px(1);
    panel::place(page, label, x, y + (px(kRowH) - h) / 2, lw - px(6), h);
}

/* PC (2026-11-16): the status line's height - its text's lines at its
 * width, two (kStatusH) to three - its bottom where layout() put it (a
 * translation of a two-line message can take three). */
void place_status()
{
    if (g.status == nullptr || g.statusBottom <= 0)
        return;
    wchar_t t[2048] = L"";
    GetWindowTextW(g.status, t, 2048);
    HDC dc = GetDC(g.status);
    HFONT f = (HFONT)SendMessageW(g.status, WM_GETFONT, 0, 0);
    HGDIOBJ old = SelectObject(dc, f != nullptr ? (HGDIOBJ)f : (HGDIOBJ)g.font);
    RECT r = {0, 0, px(kStatusW), 0};
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g.status, dc);
    const int h = std::max(px(kStatusH), std::min(px(kStatusH) * 3 / 2, (int)r.bottom + px(1)));
    RECT cur;
    GetWindowRect(g.status, &cur);
    const bool resized = cur.bottom - cur.top != h;
    /* (not its old pixels moved: the static's text is laid out again whole) */
    SetWindowPos(g.status, nullptr, g.statusX, g.statusBottom - h, px(kStatusW), h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (resized) {
        InvalidateRect(g.wnd, nullptr, TRUE);
        InvalidateRect(g.status, nullptr, TRUE);
    }
}

void layout()
{
    int w = px(kClientW), h = px(kClientH), m = px(kMargin);
    (void)w;
    for (int i = 0; i < kTabs; i++)
        SetWindowPos(g.tabs[i], nullptr, m + i * px(kTabW), px(kTabY), px(kTabW), px(kTabH),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    /* the pages over the panel below the headers; each tab's rows from its
     * page's top, the lists narrower by the scroll bar when there is one */
    const int pageY = px(kPanelY) + px(kPanelPad) / 2;
    for (HWND p : g.pages)
        SetWindowPos(p, nullptr, m, pageY, px(kPanelW), px(kPanelY) + px(kPanelH) - pageY, SWP_NOZORDER | SWP_NOACTIVATE);
    /* (the rows of the sub-tab shown) */
    int count[kTabs] = {};
    for (const Row &r : g.rows)
        if (r.sub == (r.tab == TAB_GAME ? g.gameSub : 0) && row_shown(r))
            count[r.tab]++;
    /* the Audio tab: its sub-tabs' bar, then the Settings rows or the Songs
     * page (its text, wrapped lines counted, and the song manager's button);
     * the Game tab: its bar, then the rows of the sub-tab shown */
    const int audioTop = px(kSubTabH) + px(6);
    const std::wstring songs = songs_text();
    SetWindowTextW(g.songsText, songs.c_str());
    /* its height as it wraps (the width less a scroll bar's) */
    int textH = px(kSongsLineH);
    {
        RECT box = {0, 0, px(kPanelW) - 2 * px(kPanelPad) - GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi), 0};
        HDC dc = GetDC(g.songsText);
        HGDIOBJ old = SelectObject(dc, g.font);
        DrawTextW(dc, songs.c_str(), -1, &box, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(g.songsText, dc);
        textH = std::max(textH, (int)box.bottom + px(2));
    }
    const int songsH = textH + px(8) + px(kButtonH);
    int cw[kTabs] = {}, index[kTabs][kGameSubs] = {};
    for (int i = 0; i < kTabs; i++)
        if (i != TAB_CONTROLS && i != TAB_PLAY) {
            int h = px(kPanelPad) / 2 + count[i] * px(kRowH);
            if (i == TAB_AUDIO)
                h = audioTop + (g.audioSub == 0 ? h : px(kPanelPad) / 2 + songsH + px(kPanelPad) / 2);
            if (i == TAB_GAME)
                h += audioTop;
            cw[i] = panel::begin(g.pages[i], h, px(kRowH));
        }
    {
        HWND p = g.pages[TAB_AUDIO];
        for (int s = 0, tx = px(kPanelPad); s < 2; s++) {
            const int w = sub_tab_width(p, tr(kAudioSubNames[s]));
            panel::place(p, g.audioSubTab[s], tx, px(2), w, px(kSubTabH));
            tx += w;
        }
        const int y = audioTop + px(kPanelPad) / 2, x = px(kPanelPad);
        panel::place(p, g.songsText, x, y, cw[TAB_AUDIO] - 2 * x, textH);
        panel::place(p, g.songsOpen, x, y + textH + px(8), px(kSongsOpenW), px(kButtonH));
    }
    const int gamePad = game_sub_tab_pad();     /* PC (2026-11-14) */
    for (int s = 0, tx = px(kPanelPad); s < kGameSubs; s++) {
        const int w = sub_tab_width(g.pages[TAB_GAME], tr(kGameSubNames[s]), gamePad);
        panel::place(g.pages[TAB_GAME], g.gameSubTab[s], tx, px(2), w, px(kSubTabH));
        tx += w;
    }
    {
        /* the Play tab (PC 2026-11-14): its sub-tabs' bar - Installed, Disc -
         * with the sub-tab's own button at its right (Install game files...,
         * Play from disc...); then Installed: a version a row - its name, its
         * state under it, the Play button at the right - and the
         * extractions; Disc: a disc image a row - its release, its path under
         * it, Play and Remove at the right - or its text with none.  (The mod
         * and texture managers' buttons: side_layout) */
        HWND p = g.pages[TAB_PLAY];
        int lines = 0;
        extractions_text(&lines);
        const int top = px(kPanelPad) / 2, lineH = px(18), rowH = px(kVersionRowH);
        const int y0 = audioTop + top, noneH = 4 * lineH;
        const int extraY = y0 + install::kVersions * rowH;
        const int height = g.playSub == PLAY_INSTALLED
                               ? extraY + lines * lineH + top
                               : y0 + (g.discRows.empty() ? noneH : (int)g.discRows.size() * rowH) + top;
        const int pw = panel::begin(p, height, px(kRowH));
        const int x = px(kPanelPad), bw = px(kPlayW);
        for (int s = 0, tx = x; s < 2; s++) {
            const int sw = sub_tab_width(p, tr(kPlaySubNames[s]));
            panel::place(p, g.playSubTab[s], tx, px(2), sw, px(kSubTabH));
            tx += sw;
        }
        for (HWND b : {g.install, g.discOpen})
            panel::place(p, b, pw - px(kPanelPad) - px(kInstallW), px(2), px(kInstallW), px(kButtonH));
        const int tw = pw - x - bw - px(kPanelPad) - px(8);
        for (int v = 0; v < install::kVersions; v++) {
            const int y = y0 + v * rowH;
            panel::place(p, g.versionName[v], x, y + px(2), tw, px(22));
            panel::place(p, g.versionState[v], x, y + px(24), tw, px(20));
            panel::place(p, g.versionPlay[v], pw - px(kPanelPad) - bw, y + px(8), bw, px(kButtonH));
        }
        panel::place(p, g.extractionsLabel, x, extraY, pw - 2 * x, std::max(1, lines) * lineH);
        panel::place(p, g.discNone, x, y0, pw - 2 * x, noneH);
        const int pb = px(kDiscPlayW), rb = px(kDiscRemoveW);
        const int dw = pw - x - pb - px(kDiscGap) - rb - px(kPanelPad) - px(8);
        for (size_t i = 0; i < g.discRows.size(); i++) {
            const int y = y0 + (int)i * rowH;
            const DiscRow &r = g.discRows[i];
            panel::place(p, r.name, x, y + px(2), dw, px(22));
            panel::place(p, r.path, x, y + px(24), dw, px(20));
            panel::place(p, r.play, pw - px(kPanelPad) - rb - px(kDiscGap) - pb, y + px(8), pb, px(kButtonH));
            panel::place(p, r.remove, pw - px(kPanelPad) - rb, y + px(8), rb, px(kButtonH));
        }
    }
    side_layout();
    int labelW[kTabs] = {};
    for (int t = 0; t < kTabs; t++)
        labelW[t] = label_column(t);        /* PC (2026-11-16) */
    for (const Row &r : g.rows) {
        if (!row_shown(r))
            continue;                       /* PC (2026-11-09): hidden (show_game_rows), no place */
        int y = px(kPanelPad) / 2 + index[r.tab][r.sub]++ * px(kRowH) +
                (r.tab == TAB_AUDIO || r.tab == TAB_GAME ? audioTop : 0);
        int x = px(kPanelPad);
        const int kw = labelW[r.tab];       /* (the label column: kLabelW before 2026-11-16) */
        const int w = std::min(px(kComboW), cw[r.tab] - x - kw - px(kPanelPad) / 2);
        place_label(g.pages[r.tab], r.label, x, y, kw);
        if (r.cap) {
            /* "FPS" [ 7 digits ][^v]   "ms" [ 7 digits ][^v] */
            const int lw = px(30), ew = text_width(g.pages[r.tab], L"0000000") + px(12), uw = px(16),
                      gap = px(12);
            int cx = x + kw;
            panel::place(g.pages[r.tab], r.extra[0], cx, y + px(4), lw, px(20));
            panel::place(g.pages[r.tab], r.combo, cx + lw, y, ew, px(kSliderH));
            panel::place(g.pages[r.tab], r.extra[1], cx + lw + ew, y, uw, px(kSliderH));
            cx += lw + ew + uw + gap;
            panel::place(g.pages[r.tab], r.extra[2], cx, y + px(4), lw, px(20));
            panel::place(g.pages[r.tab], r.value, cx + lw, y, ew, px(kSliderH));
            panel::place(g.pages[r.tab], r.extra[3], cx + lw + ew, y, uw, px(kSliderH));
        } else if (r.number) {
            /* PC (2026-11-14): a number's field, nine digits and their commas wide */
            panel::place(g.pages[r.tab], r.combo, x + kw, y,
                         text_width(g.pages[r.tab], L"000,000,000") + px(16), px(kSliderH));
        } else if (r.value != nullptr) {
            /* the slider, its value right of it (a colour's Default button),
             * a slider's Default button in the value's right half */
            int vw = std::min(px(kValueW), w / 2);
            int dw = 0;
            if (r.reset != nullptr) {
                /* PC (2026-11-16): the Default button as wide as its text (a
                 * translation runs longer: "Predeterminado"), the value's part
                 * 48 px at least; the slider gives the room */
                wchar_t t[64] = L"";
                GetWindowTextW(r.reset, t, 64);
                dw = std::max(vw / 2, text_width(g.pages[r.tab], t) + px(16));
                vw = std::max(vw, dw + px(kValueGap) + px(48));
            }
            panel::place(g.pages[r.tab], r.combo, x + kw, y, w - vw - px(kValueGap), px(kSliderH));
            panel::place(g.pages[r.tab], r.value, x + kw + w - vw, r.button ? y : y + px(4),
                         r.reset != nullptr ? vw - dw - px(kValueGap) : vw, r.button ? px(kSliderH) : px(20));
            if (r.reset != nullptr)
                panel::place(g.pages[r.tab], r.reset, x + kw + w - dw, y, dw, px(kSliderH));
        } else {
            panel::place(g.pages[r.tab], r.combo, x + kw, y, w, px(kComboListH));
        }
    }
    controls::layout(g.dpi);
    int by = h - m - px(kButtonH);
    int bh = px(kButtonH);
    /* (PC 2026-11-14: Install game files... is on the Play tab's Installed
     * sub-tab; Cancel, shown while installing, stays here) */
    SetWindowPos(g.cancel, nullptr, m, by, px(kButtonW), bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.exit, nullptr, px(kClientW) - m - px(kButtonW), by, px(kButtonW), bh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    /* the time played: centred on the window, between the buttons */
    const int tw = px(kClientW) - 2 * (m + px(kInstallW) + px(kButtonGap));
    SetWindowPos(g.playTime, nullptr, (px(kClientW) - tw) / 2, by + (bh - px(20)) / 2, tw, px(20),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    /* While installing the progress bar sits above the buttons and the
     * status line above it; otherwise the status line comes down.  Both left
     * of the art's logo. */
    int py = by - px(12) - px(kProgressH);
    SetWindowPos(g.progress, nullptr, m, py, px(kStatusW), px(kProgressH), SWP_NOZORDER | SWP_NOACTIVATE);
    g.statusX = m;
    g.statusBottom = g.installing ? py - px(6) : by - px(12);
    place_status();
    InvalidateRect(g.wnd, nullptr, TRUE);
}

void set_status(const std::wstring &text)
{
    SetWindowTextW(g.status, text.c_str());
    place_status();                         /* PC (2026-11-16) */
}

void show_progress_text()
{
    std::wstring s = g.phase;
    uint64_t done = g.done, total = g.total;
    if (g.copying && total) {
        wchar_t pct[16];
        swprintf(pct, 16, L"%u%%", (unsigned)(done * 100 / total));
        s += L"  " + trf(L"%s of %s (%s)", util::format_size(done).c_str(), util::format_size(total).c_str(), pct);
        SendMessageW(g.progress, PBM_SETPOS, (WPARAM)(done * 1000 / total), 0);
    }
    set_status(s);
}

/* The version Enter plays: the last played when it is installed, else the
 * first installed; -1 with none. */
int default_version()
{
    int def = -1;
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok && (def < 0 || v == g.lastVersion))
            def = v;
    return def;
}

/* PC (2026-11-14): the image is where the list says. */
bool disc_there(const disc_list::Disc &d)
{
    const DWORD a = GetFileAttributesW(d.path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The disc image Enter plays on the Disc sub-tab: the first of the list that
 * is there (the last played is the first); -1 with none. */
int default_disc()
{
    for (size_t i = 0; i < g.discThere.size() && i < g.discRows.size(); i++)
        if (g.discThere[i])
            return (int)i;
    return -1;
}

HWND default_button()
{
    if (g.installing)
        return g.cancel;
    if (g.playSub == PLAY_DISC) {           /* PC (2026-11-14) */
        const int d = default_disc();
        return d >= 0 ? g.discRows[d].play : g.discOpen;
    }
    const int dv = default_version();
    return dv >= 0 ? g.versionPlay[dv] : g.install;
}

/* Updates the controls for the installed state, or for a running install. */
void refresh(const std::wstring &note = std::wstring())
{
    for (int v = 0; v < install::kVersions; v++) {
        SetWindowTextW(g.versionState[v], version_text(v).c_str());
        EnableWindow(g.versionPlay[v], !g.installing && version_state(v) == install::InstallState::ok);
    }
    int lines = 0;
    SetWindowTextW(g.extractionsLabel, extractions_text(&lines).c_str());
    SetWindowTextW(g.modsText, mods_text().c_str());
    SetWindowTextW(g.texturesText, textures_text().c_str());
    SetWindowTextW(g.savesText, saves_text().c_str());     /* PC (2026-11-15) */
    SetWindowTextW(g.playTime, play_time_text().c_str());
    /* (PC 2026-11-14: Install game files... is the Installed sub-tab's, shown
     * with it: show_play_sub) */
    EnableWindow(g.install, !g.installing);
    ShowWindow(g.cancel, g.installing ? SW_SHOW : SW_HIDE);
    EnableWindow(g.cancel, g.installing);
    ShowWindow(g.progress, g.installing ? SW_SHOW : SW_HIDE);
    /* PC (2026-11-14): the Disc sub-tab's rows: an image's path, or that it
     * is not there (its Play off) */
    g.discThere.assign(g.discs.size(), 0);
    for (size_t i = 0; i < g.discRows.size() && i < g.discs.size(); i++) {
        g.discThere[i] = disc_there(g.discs[i]) ? 1 : 0;
        SetWindowTextW(g.discRows[i].path,
                       (g.discThere[i] ? g.discs[i].path : trf(L"Not found: %s", g.discs[i].path.c_str())).c_str());
        EnableWindow(g.discRows[i].play, !g.installing && g.discThere[i]);
        EnableWindow(g.discRows[i].remove, !g.installing);
    }
    EnableWindow(g.discOpen, !g.installing);
    /* Enter plays the default version when one is installed, else installs
     * (the Disc sub-tab: the first disc image there, else Play from disc...). */
    HWND def = default_button();
    g.default_id = GetDlgCtrlID(def);
    std::vector<HWND> buttons = {g.install, g.cancel, g.exit, g.discOpen};
    buttons.insert(buttons.end(), std::begin(g.versionPlay), std::end(g.versionPlay));
    for (const DiscRow &r : g.discRows)
        buttons.push_back(r.play);
    for (HWND b : buttons)
        SendMessageW(b, BM_SETSTYLE, b == def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    if (!g.installing)
        set_status(note.empty() ? state_text() : note + L"\n" + state_text().substr(0, state_text().find(L'\n')));
    layout();
}

/* ---- the Play tab's sub-tabs (PC 2026-11-14) ------------------------------------------ */

/* Sub-tab s of the Play tab shown: Installed (the versions' rows, the
 * extractions, Install game files...) or Disc (the disc images' rows, or the
 * text with none, Play from disc...). */
void show_play_sub(int s)
{
    g.playSub = s;
    const int inst = s == PLAY_INSTALLED ? SW_SHOW : SW_HIDE, disc = s == PLAY_DISC ? SW_SHOW : SW_HIDE;
    for (int v = 0; v < install::kVersions; v++)
        for (HWND c : {g.versionName[v], g.versionState[v], g.versionPlay[v]})
            ShowWindow(c, inst);
    ShowWindow(g.extractionsLabel, inst);
    ShowWindow(g.install, inst);
    ShowWindow(g.discOpen, disc);
    ShowWindow(g.discNone, s == PLAY_DISC && g.discRows.empty() ? SW_SHOW : SW_HIDE);
    for (const DiscRow &r : g.discRows)
        for (HWND c : {r.name, r.path, r.play, r.remove})
            ShowWindow(c, disc);
    for (HWND h : g.playSubTab)
        InvalidateRect(h, nullptr, FALSE);
    panel::scroll_to(g.pages[TAB_PLAY], 0);
    refresh();
}

/* ---- the Audio tab's sub-tabs, the music's notices ----------------------------------- */

/* Sub-tab s of the Audio tab shown: its rows (Settings) or the Songs page. */
void show_audio_sub(int s)
{
    g.audioSub = s;
    for (const Row &r : g.rows)
        if (r.tab == TAB_AUDIO)
            for (HWND c : {r.label, r.combo, r.value, r.reset})
                if (c != nullptr)
                    ShowWindow(c, s == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g.songsText, s == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g.songsOpen, s == 1 ? SW_SHOW : SW_HIDE);
    if (s == 1 && g.tab == TAB_AUDIO)
        g.musicUnread = false;
    for (HWND h : g.audioSubTab)
        InvalidateRect(h, nullptr, FALSE);
    InvalidateRect(g.tabs[TAB_AUDIO], nullptr, FALSE);
    layout();
}

/* The Game tab's rows of sub-tab g.gameSub shown, the others hidden. */
void show_game_rows()
{
    for (const Row &r : g.rows)
        if (r.tab == TAB_GAME)
            for (HWND c : {r.label, r.combo, r.value, r.reset})
                if (c != nullptr)
                    ShowWindow(c, r.sub == g.gameSub && row_shown(r) ? SW_SHOW : SW_HIDE);
}

/* Sub-tab s of the Game tab shown: General, HUD, Extras or Surfaces. */
void show_game_sub(int s)
{
    g.gameSub = s;
    show_game_rows();
    for (HWND h : g.gameSubTab)
        InvalidateRect(h, nullptr, FALSE);
    panel::scroll_to(g.pages[TAB_GAME], 0);
    layout();
}

/* A notice of the music (the conversion, new songs, files not added): on
 * the Songs page, unread (the red dot) until it is shown. */
void music_note(const std::wstring &text)
{
    g.musicNotes.push_back(text);
    g.musicUnread = !(g.tab == TAB_AUDIO && g.audioSub == 1);
    for (HWND h : g.audioSubTab)
        InvalidateRect(h, nullptr, FALSE);
    InvalidateRect(g.tabs[TAB_AUDIO], nullptr, FALSE);
    layout();
}

/* The song manager on the installed versions (the last played first). */
void open_song_manager()
{
    std::vector<song_manager::Version> versions;
    int first = 0;
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok) {
            if (v == g.lastVersion)
                first = (int)versions.size();
            versions.push_back({install::version_name((install::Version)v),
                                g.folder + install::version_folder((install::Version)v)});
        }
    /* PC (2026-11-15): the disc images added (the last played first, when it is one of them) */
    wchar_t lastDisc[128] = L"";
    GetPrivateProfileStringW(L"Game", L"Disc", L"", lastDisc, 128, engine_ini_path().c_str());
    for (const SongFolder &d : disc_song_folders()) {
        const std::wstring name = d.data.substr(d.data.find_last_of(L'\\') + 1);
        if (lastDisc[0] != 0 && _wcsicmp(name.c_str(), lastDisc) == 0)
            first = (int)versions.size();
        versions.push_back({d.name, d.data});
    }
    if (versions.empty()) {
        MessageBoxW(g.wnd, tr(L"No version is installed and no disc image is added: install one, or add a disc image (Play "
                              L"tab)."),
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    song_manager::open(g.wnd, versions, first);
}

/* The mod manager; its template's folders are the installed versions'. */
void open_mod_manager()
{
    std::vector<std::wstring> data;
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok)
            data.push_back(g.folder + install::version_folder((install::Version)v));
    mod_manager::open(g.wnd, engine_ini_path(), data, [] {
        SetWindowTextW(g.modsText, mods_text().c_str());
        side_layout();                      /* PC (2026-11-14): its text wraps under its button */
    });
}

/* PC (2026-11-13): the texture manager. */
void open_texture_manager()
{
    texture_manager::open(g.wnd, ini_path(), [] {
        SetWindowTextW(g.texturesText, textures_text().c_str());
        side_layout();                      /* PC (2026-11-14) */
    });
}

/* PC (2026-11-15): the save manager, over this engine's save folders. */
void open_save_manager()
{
    const bool shared = _wcsicmp(util::with_slash(g.shared).c_str(), util::with_slash(g.folder).c_str()) != 0;
    save_manager::open(g.wnd, saves_dir(), engine_ini_path(), save_folders::engine_name(g.folder, shared), [] {
        SetWindowTextW(g.savesText, saves_text().c_str());
        side_layout();
    });
}

/* ---- DPI ------------------------------------------------------------------------ */

/* Widens a list's drop-down to its longest entry in its font (the monitor
 * lists' entries can be longer than the list is wide). */
void fit_list(HWND c)
{
    HDC dc = GetDC(c);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(c, WM_GETFONT, 0, 0));
    int w = 0;
    int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        std::wstring s((size_t)SendMessageW(c, CB_GETLBTEXTLEN, i, 0) + 1, L'\0');
        int len = (int)SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)&s[0]);
        SIZE size = {};
        if (len > 0 && GetTextExtentPoint32W(dc, s.c_str(), len, &size) && size.cx > w)
            w = size.cx;
    }
    if (old)
        SelectObject(dc, old);
    ReleaseDC(c, dc);
    /* (never narrower than the list itself) */
    SendMessageW(c, CB_SETDROPPEDWIDTH, w + px(12) + GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi), 0);
}

void apply_dpi(UINT dpi, const RECT *suggested)
{
    g.dpi = dpi;

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT font = nullptr, tabFont = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfWeight = FW_SEMIBOLD;
        lf.lfHeight = lf.lfHeight * 6 / 5;
        tabFont = CreateFontIndirectW(&lf);
    }
    std::vector<HWND> all = {g.install, g.cancel, g.exit, g.status, g.progress, g.extractionsLabel, g.songsText,
                             g.songsOpen, g.discOpen, g.discNone};
    for (const DiscRow &r : g.discRows)     /* PC (2026-11-14) */
        for (HWND c : {r.path, r.play, r.remove})
            all.push_back(c);
    for (int v = 0; v < install::kVersions; v++) {
        all.push_back(g.versionPlay[v]);
        all.push_back(g.versionState[v]);
    }
    for (const Row &r : g.rows) {
        all.push_back(r.label);
        all.push_back(r.combo);
        if (r.value != nullptr)
            all.push_back(r.value);
        if (r.reset != nullptr)
            all.push_back(r.reset);
        for (HWND c : r.extra)
            all.push_back(c);
    }
    for (HWND c : all)
        SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    if (g.font)
        DeleteObject(g.font);
    if (g.tabFont)
        DeleteObject(g.tabFont);
    g.font = font;
    g.tabFont = tabFont;
    for (HWND c : g.versionName)
        SendMessageW(c, WM_SETFONT, (WPARAM)(tabFont ? tabFont : font), FALSE);
    for (const DiscRow &r : g.discRows)
        SendMessageW(r.name, WM_SETFONT, (WPARAM)(tabFont ? tabFont : font), FALSE);
    controls::set_font(font, tabFont);
    for (const Row &r : g.rows)
        if (r.value == nullptr)
            fit_list(r.combo);

    HICON icon_big = nullptr, icon_small = nullptr;
    int cb = GetSystemMetricsForDpi(SM_CXICON, dpi), cs = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    LoadIconWithScaleDown(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER), cb, cb, &icon_big);
    LoadIconWithScaleDown(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER), cs, cs, &icon_small);
    SendMessageW(g.wnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
    SendMessageW(g.wnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);
    if (g.icon_big)
        DestroyIcon(g.icon_big);
    if (g.icon_small)
        DestroyIcon(g.icon_small);
    g.icon_big = icon_big;
    g.icon_small = icon_small;

    build_background();

    /* The fixed client size at this DPI. */
    RECT r = {0, 0, px(kClientW), px(kClientH)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (suggested) {
        SetWindowPos(g.wnd, nullptr, suggested->left, suggested->top, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        /* First showing: centred in the work area of its monitor. */
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        int x = (mi.rcWork.left + mi.rcWork.right - ww) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - wh) / 2;
        SetWindowPos(g.wnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout();
    RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- actions --------------------------------------------------------------- */

void message(const std::wstring &text, UINT icon) { MessageBoxW(g.wnd, text.c_str(), kAppName, MB_OK | icon); }

/* ---- the settings (gt3pc.ini) ---------------------------------------------- */

std::wstring ini_path() { return (g.shared.empty() ? g.folder : g.shared) + L"gt3pc.ini"; }
/* PC (2026-11-14): the engine's own settings file, in its folder */
std::wstring engine_ini_path() { return g.folder + engines::kEngineIni; }
/* PC (2026-11-15): saves\<engine>\ (platform/save_folders.h), beside the
 * gt3pc.ini in use */
std::wstring saves_dir() { return save_folders::saves_dir(g.shared.empty() ? g.folder : g.shared, g.folder); }
/* PC (2026-11-15, user request: "The launcher needs to create 2 default
 * folders if there's nothing: (default1, default2)"): at the start and at
 * Play, the engine without a save folder gets default1 and default2, in
 * memory card slots 1 and 2 (save_folders::make_defaults) */
void ensure_save_folders()
{
    if (save_folders::make_defaults(saves_dir(), engine_ini_path()) && g.savesText != nullptr) {
        SetWindowTextW(g.savesText, saves_text().c_str());
        side_layout();
    }
}
/* " --shared <folder>" for the game of an engine that reads another
 * launcher's gt3pc.ini (platform/paths.h), else nothing. */
std::wstring shared_arg()
{
    if (g.shared.empty() || _wcsicmp(g.shared.c_str(), g.folder.c_str()) == 0)
        return std::wstring();
    std::wstring f = g.shared;
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/'))
        f.pop_back();
    return L" --shared \"" + f + L"\"";
}
std::wstring size_text(int w, int h) { return std::to_wstring(w) + L" x " + std::to_wstring(h); }
void combo_reset(HWND c) { SendMessageW(c, CB_RESETCONTENT, 0, 0); }
void combo_add(HWND c, const std::wstring &s) { SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
int combo_sel(HWND c) { return (int)SendMessageW(c, CB_GETCURSEL, 0, 0); }

/* The refresh rates of the fullscreen resolution chosen (the default: the
 * nearest to 60 Hz, the game's rate); none in the other modes. */
void fill_rates()
{
    combo_reset(g.rate);
    g.rateValues.clear();
    if (g.disp.mode != display::Mode::fullscreen) {
        combo_add(g.rate, g.disp.mode == display::Mode::borderless ? tr(L"The desktop's") : L"-");
        SendMessageW(g.rate, CB_SETCURSEL, 0, 0);
        EnableWindow(g.rate, FALSE);
        return;
    }
    display::Size s = g.disp.width > 0 ? display::Size{g.disp.width, g.disp.height} : display::desktop_size();
    combo_add(g.rate, tr(L"Default (60 Hz)"));
    g.rateValues.push_back(0);
    int sel = 0;
    for (int r : display::refresh_rates(s.w, s.h)) {
        if (r == g.disp.refresh)
            sel = (int)g.rateValues.size();
        combo_add(g.rate, std::to_wstring(r) + L" Hz");
        g.rateValues.push_back(r);
    }
    g.disp.refresh = g.rateValues[sel];
    SendMessageW(g.rate, CB_SETCURSEL, sel, 0);
    EnableWindow(g.rate, TRUE);
}

/* The resolutions of the mode chosen: fullscreen, the monitor's (the
 * desktop's first); windowed, 4:3 window sizes that fit the screen (and the
 * window's own size when it is none of them); borderless, the desktop's. */
void fill_resolutions()
{
    combo_reset(g.res);
    g.resSizes.clear();
    const display::Size desk = display::desktop_size();
    int sel = 0;
    if (g.disp.mode == display::Mode::fullscreen) {
        combo_add(g.res, trf(L"Desktop (%s)", size_text(desk.w, desk.h).c_str()));
        g.resSizes.push_back(display::Size{0, 0});
        for (const display::Size &s : display::fullscreen_sizes()) {
            if (s.w == g.disp.width && s.h == g.disp.height)
                sel = (int)g.resSizes.size();
            combo_add(g.res, size_text(s.w, s.h));
            g.resSizes.push_back(s);
        }
        if (sel == 0)
            g.disp.width = g.disp.height = 0;
    } else if (g.disp.mode == display::Mode::windowed) {
        bool found = false;
        for (const display::Size &s : display::window_sizes()) {
            if (s.w == g.disp.windowW && s.h == g.disp.windowH) {
                sel = (int)g.resSizes.size();
                found = true;
            }
            combo_add(g.res, size_text(s.w, s.h));
            g.resSizes.push_back(s);
        }
        if (!found) {
            sel = (int)g.resSizes.size();
            combo_add(g.res, trf(L"%s (the window's size)", size_text(g.disp.windowW, g.disp.windowH).c_str()));
            g.resSizes.push_back(display::Size{g.disp.windowW, g.disp.windowH});
        }
    } else {
        combo_add(g.res, trf(L"Desktop (%s)", size_text(desk.w, desk.h).c_str()));
        g.resSizes.push_back(display::Size{0, 0});
    }
    SendMessageW(g.res, CB_SETCURSEL, sel, 0);
    EnableWindow(g.res, g.disp.mode != display::Mode::borderless);
    fill_rates();
}

/* "DISPLAY2" of the device name \\.\DISPLAY2. */
std::wstring device_text(const std::wstring &device)
{
    size_t i = device.rfind(L'\\');
    return i == std::wstring::npos ? device : device.substr(i + 1);
}

/* The monitor lists: the runs of monitors side by side that one picture can
 * span (borderless and fullscreen only; display::spans), and the split
 * screen's second monitor.  The runs name their monitors by their friendly
 * names when every monitor has one of its own (two monitors of a kind would
 * read the same), else DISPLAYn.  A saved choice none of the entries stands
 * for gets an entry of its own, marked, so it is not lost. */
void fill_monitors()
{
    const std::vector<display::Monitor> mons = display::monitors();
    bool named = true;
    for (size_t i = 0; i < mons.size(); i++) {
        named = named && !mons[i].name.empty();
        for (size_t j = 0; j < i; j++)
            named = named && _wcsicmp(mons[i].name.c_str(), mons[j].name.c_str()) != 0;
    }
    auto connected = [&](const std::wstring &device) {
        return std::any_of(mons.begin(), mons.end(),
                           [&](const display::Monitor &m) { return _wcsicmp(m.device.c_str(), device.c_str()) == 0; });
    };

    combo_reset(g.span);
    g.spanValues.assign(1, std::wstring());
    combo_add(g.span, tr(L"Off"));
    int sel = 0;
    for (const display::Span &s : display::spans(mons)) {
        std::wstring text;
        for (int i : s.monitors)
            text += (text.empty() ? L"" : L" + ") + (named ? mons[i].name : device_text(mons[i].device));
        if (display::same_devices(s.value, g.disp.spanMonitors))
            sel = (int)g.spanValues.size();
        combo_add(g.span, text + L" (" + size_text(s.w, s.h) + L")");
        g.spanValues.push_back(s.value);
    }
    const std::vector<std::wstring> saved = display::device_list(g.disp.spanMonitors);
    if (sel == 0 && !saved.empty()) {
        /* (connected but no longer side by side, or one monitor only) */
        std::wstring text;
        bool all = true;
        for (const std::wstring &d : saved) {
            text += (text.empty() ? L"" : L" + ") + device_text(d);
            all = all && connected(d);
        }
        sel = (int)g.spanValues.size();
        combo_add(g.span, all ? trf(L"%s (not side by side)", text.c_str()) : trf(L"%s (not connected)", text.c_str()));
        g.spanValues.push_back(g.disp.spanMonitors);
    }
    SendMessageW(g.span, CB_SETCURSEL, sel, 0);
    EnableWindow(g.span, g.disp.mode != display::Mode::windowed);

    combo_reset(g.split);
    g.splitValues.assign(1, std::wstring());
    combo_add(g.split, tr(L"Automatic"));
    sel = 0;
    for (const display::Monitor &m : mons) {
        if (_wcsicmp(m.device.c_str(), g.disp.splitMonitor.c_str()) == 0)
            sel = (int)g.splitValues.size();
        combo_add(g.split, device_text(m.device) + (m.name.empty() ? L"" : L": " + m.name) + L" (" +
                               size_text(m.w, m.h) + L")");
        g.splitValues.push_back(m.device);
    }
    if (sel == 0 && !g.disp.splitMonitor.empty()) {
        sel = (int)g.splitValues.size();
        combo_add(g.split, trf(L"%s (not connected)", device_text(g.disp.splitMonitor).c_str()));
        g.splitValues.push_back(g.disp.splitMonitor);
    }
    SendMessageW(g.split, CB_SETCURSEL, sel, 0);
}

/* ---- the game's other settings (gt3pc.ini [Graphics], [Audio],
 * HudMaxAspect; the settings menu's, src/platform/settings.h) ---------------- */

std::wstring fmt(const wchar_t *f, double v)
{
    wchar_t s[64];
    swprintf(s, 64, f, v);
    return s;
}

/* The sliders' values shown. */
std::wstring percent_text(double v) { return fmt(L"%g%%", v); }
std::wstring scale_text(double v) { return v > 100.0 ? trf(L"%s (supersampling)", percent_text(v).c_str()) : percent_text(v); }
std::wstring safe_zone_text(double v) { return percent_text(v); }
std::wstring course_scale_text(double v)
{
    return v < 0 ? trf(L"%s (horizontal, mirrored)", percent_text(v).c_str())
                 : trf(L"%s (horizontal)", percent_text(v).c_str());
}
std::wstring course_height_text(double v) { return trf(L"%s (height)", percent_text(v).c_str()); }
std::wstring ms_text(double v) { return fmt(L"%.1f ms", v); }
std::wstring raw_text(double v) { return fmt(L"%.2f", v); }
std::wstring count_text(double v) { return fmt(L"%.0f", v); }
std::wstring angle_text(double v) { return trf(L"%.0f degrees", v); }
/* PC (2026-11-12): MenuWheelsSpeed (0: the menu wheels at every speed) */
std::wstring wheel_speed_text(double v) { return v < 0.5 ? std::wstring(tr(L"Always menu wheels")) : fmt(L"%.0f km/h", v); }

/* The languages (src/platform/language.h): their country codes (the menus'
 * folders), names and ini values. */
struct Language {
    const char *code;
    const wchar_t *name, *value;
};
const Language kLanguages[] = {
    {"JP", N_(L"Japanese"), L"japanese"}, {"US", N_(L"English (US)"), L"english_us"},
    {"BR", N_(L"English (UK)"), L"english_uk"}, {"FR", N_(L"French"), L"french"},
    {"GR", N_(L"German"), L"german"}, {"IT", N_(L"Italian"), L"italian"},
    {"SP", N_(L"Spanish"), L"spanish"},
};

/* The language's list: automatic (each version's own) and the languages
 * whose menus an installed version has (every language without an install;
 * a version without the chosen one plays its own: platform/language.h).
 * Built again after an install (install_done). */
std::vector<Choice> language_choices()
{
    bool any = false, has[sizeof(kLanguages) / sizeof(kLanguages[0])] = {};
    for (int v = 0; v < install::kVersions; v++) {
        const std::wstring data = install::version_folder((install::Version)v);
        if (install::install_state(g.folder, data) != install::InstallState::ok)
            continue;
        any = true;
        for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); i++)
            has[i] = has[i] || install::language_installed(g.folder, data, kLanguages[i].code);
    }
    std::vector<Choice> c = {{tr(L"Default"), L"auto"}};
    for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); i++)
        if (!any || has[i])
            c.push_back({tr(kLanguages[i].name), kLanguages[i].value});
    return c;
}

/* PC (2026-11-16): GT3 PC's own text's languages (platform/ui_text.h), each in
 * its own words; Automatic with the one it stands for. */
std::vector<Choice> interface_language_choices()
{
    std::vector<Choice> c = {
        {trf(L"Automatic (%s)", ui_text::native_name_w(ui_text::windows_language())), L"auto"}};
    for (int i = 0; i < ui_text::kCount; i++)
        c.push_back({ui_text::native_name_w(i), util::widen(ui_text::value(i))});
    return c;
}

/* The options, in their tabs' order (their rows are placed among the
 * display ones: wnd_proc's WM_CREATE). */
void build_options()
{
    auto add = [](int tab, const wchar_t *label, const wchar_t *section, const wchar_t *key, const wchar_t *def,
                  std::vector<Choice> choices) {
        Option o;
        o.tab = tab;
        o.label = label;
        o.section = section;
        o.key = key;
        o.def = def;
        o.choices = std::move(choices);
        g.options.push_back(std::move(o));
    };
    auto add_slider = [](int tab, const wchar_t *label, const wchar_t *section, const wchar_t *key, const wchar_t *def,
                         double min, double max, double step, int decimals, std::wstring (*text)(double)) {
        Option o;
        o.tab = tab;
        o.label = label;
        o.section = section;
        o.key = key;
        o.def = def;
        o.min = min;
        o.max = max;
        o.step = step;
        o.decimals = decimals;
        o.text = text;
        g.options.push_back(std::move(o));
    };
    /* PC (2026-11-14): a whole number in a field */
    auto add_number = [](int tab, const wchar_t *label, const wchar_t *section, const wchar_t *key, const wchar_t *def,
                         double min, double max) {
        Option o;
        o.tab = tab;
        o.label = label;
        o.section = section;
        o.key = key;
        o.def = def;
        o.min = min;
        o.max = max;
        o.number = true;
        g.options.push_back(std::move(o));
    };
    /* Display */
    add(TAB_DISPLAY, tr(L"Vertical sync"), L"Graphics", L"VSync", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"1 frame"), L"1"}, {tr(L"2 frames"), L"2"}, {tr(L"3 frames"), L"3"}});
    /* PC (2026-11-19, user request): settings.h's GameClockSync (the F2
     * menu's GAME CLOCK SYNC) */
    add(TAB_DISPLAY, tr(L"Game clock sync"), L"Display", L"GameClockSync", L"variable",
        {{tr(L"Variable"), L"variable"}, {tr(L"Static NTSC"), L"ntsc"}});
    /* (the frame cap: its own row of two fields, add_cap_row - PC 2026-11-08,
     * user request; a list of rates before) */
    /* (PC 2026-11-15, user request: "Menu aspect: 16:9 option") */
    add(TAB_DISPLAY, tr(L"Menu aspect"), L"Display", L"MenuAspect", L"original",
        {{tr(L"Original (4:3)"), L"original"}, {L"16:9", L"16:9"}, {tr(L"Stretch"), L"stretch"}});
    add(TAB_DISPLAY, tr(L"Movie aspect"), L"Display", L"MovieAspect", L"original",
        {{tr(L"Original"), L"original"}, {tr(L"Stretch"), L"stretch"}});
    /* Graphics */
    add(TAB_GRAPHICS, tr(L"Anti-aliasing"), L"Graphics", L"AntiAliasing", L"off",
        {{tr(L"Off"), L"off"}, {L"FXAA", L"fxaa"}, {L"MSAA 2x", L"msaa2"}, {L"MSAA 4x", L"msaa4"}, {L"MSAA 8x", L"msaa8"},
         /* PC (2026-11-15, user request): settings.h's AGSS */
         {L"AGSS 2x", L"agss2"}, {L"AGSS 4x", L"agss4"}, {L"AGSS 8x", L"agss8"}});
    /* PC (2026-11-10): settings.h's AnisotropicFiltering (the F2 menu's
     * ANISOTROPIC FILTERING) */
    add(TAB_GRAPHICS, tr(L"Anisotropic filtering"), L"Graphics", L"AnisotropicFiltering", L"off",
        {{tr(L"Off"), L"off"}, {L"2x", L"2"}, {L"4x", L"4"}, {L"8x", L"8"}, {L"16x", L"16"}});
    /* (the sliders' ranges: settings.h's kRenderScaleMin .. kRenderScaleMax,
     * kRenderDistanceMin .., kFovMin .., kHudSafeZoneMin .., kAudioBufferMin ..) */
    add_slider(TAB_GRAPHICS, tr(L"Resolution scale"), L"Graphics", L"RenderScale", L"100", 10, 400, 5, 0, scale_text);
    /* (the labels and the values' names as the F2 menu's, settings_menu.cpp:
     * user request, 2026-11-08) */
    add(TAB_GRAPHICS, tr(L"Levels of detail"), L"Graphics", L"Lod", L"on",
        {{tr(L"Normal"), L"on"}, {tr(L"Always best"), L"off"}});
    add(TAB_GRAPHICS, tr(L"Road reflection"), L"Graphics", L"ReflectionDetail", L"original",
        {{tr(L"Normal"), L"original"}, {tr(L"High"), L"full"}});
    /* PC (2026-11-14): settings.h's ReflectionResolution (the F2 menu's ROAD
     * REFLECTION RESOLUTION) */
    add(TAB_GRAPHICS, tr(L"Road reflection resolution"), L"Graphics", L"ReflectionResolution", L"normal",
        {{tr(L"Normal"), L"normal"}, {tr(L"Half"), L"half"}, {tr(L"Full"), L"full"}});
    add(TAB_GRAPHICS, tr(L"Car reflections"), L"Graphics", L"CarReflections", L"all",
        {{tr(L"Normal"), L"all"}, {tr(L"Low"), L"large"}, {tr(L"Off"), L"none"}});
    add(TAB_GRAPHICS, tr(L"Env map detail"), L"Graphics", L"EnvMapDetail", L"scaled",
        {{tr(L"High"), L"scaled"}, {tr(L"Normal"), L"original"}});
    add(TAB_GRAPHICS, tr(L"Replay depth of field"), L"Graphics", L"ReplayDepthOfField", L"original",
        {{tr(L"Normal"), L"original"}, {tr(L"Reduced"), L"reduced"}, {tr(L"Off"), L"off"}});
    add_slider(TAB_GRAPHICS, tr(L"Render distance"), L"Graphics", L"RenderDistance", L"100", 100, 400, 5, 0,
               percent_text);
    /* ("%%": its percent sign - a format, so a translation keeps it as one) */
    add(TAB_GRAPHICS, tr(L"Smoke opacity"), L"Graphics", L"SmokeOpacity", L"normal",
        {{tr(L"Normal"), L"normal"}, {trf(L"Reduced (75%%)"), L"reduced"}, {tr(L"Half"), L"half"}});
    /* PC (2026-11-12): the menu (showroom) wheel models on the race cars (the
     * F2 menu's MENU WHEELS, RACE WHEELS FROM) */
    add(TAB_GRAPHICS, tr(L"Menu wheels"), L"Graphics", L"MenuWheels", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add_slider(TAB_GRAPHICS, tr(L"Race wheels from"), L"Graphics", L"MenuWheelsSpeed", L"20", 0, 400, 1, 0,
               wheel_speed_text);
    /* PC (2026-11-13): the menu (showroom) tyres on the race cars (the F2
     * menu's MENU TIRES) */
    add(TAB_GRAPHICS, tr(L"Menu tires"), L"Graphics", L"MenuTires", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* PC (2026-11-15: as the F2 menu's, user request "Move Rear view settings
     * into graphics category"; were Game > Cameras' rows) */
    add(TAB_GRAPHICS, tr(L"Rear view: bumper cam"), L"Graphics", L"RearViewBumper", L"on", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_GRAPHICS, tr(L"Rear view: chase cam"), L"Graphics", L"RearViewChase", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* Audio (PC 2026-11-15, user request: "Volume goes above the output
     * settings") - the volumes: settings.h's [Audio] MenuBgm .. ReplaySe, 0 .. 100 */
    add_slider(TAB_AUDIO, tr(L"Menu music"), L"Audio", L"MenuBgm", L"100", 0, 100, 1, 0, percent_text);
    add_slider(TAB_AUDIO, tr(L"Race music"), L"Audio", L"RaceBgm", L"50", 0, 100, 1, 0, percent_text);
    add_slider(TAB_AUDIO, tr(L"Replay music"), L"Audio", L"ReplayBgm", L"50", 0, 100, 1, 0, percent_text);
    add_slider(TAB_AUDIO, tr(L"Menu sound effects"), L"Audio", L"MenuSe", L"100", 0, 100, 1, 0, percent_text);
    add_slider(TAB_AUDIO, tr(L"Race sound effects"), L"Audio", L"RaceSe", L"100", 0, 100, 1, 0, percent_text);
    add_slider(TAB_AUDIO, tr(L"Replay sound effects"), L"Audio", L"ReplaySe", L"100", 0, 100, 1, 0, percent_text);
    /* (PC 2026-11-15, user request: "Audio Output: Windows Wasapi is the
     * default"; was Auto) */
    add(TAB_AUDIO, tr(L"Audio output"), L"Audio", L"Output", L"wasapi",
        {{tr(L"Auto"), L"auto"}, {tr(L"Low latency kernel streaming"), L"ks"}, {L"Windows WASAPI", L"wasapi"}});
    add_slider(TAB_AUDIO, tr(L"Audio buffer"), L"Audio", L"Buffer", L"2", 1.0, 20.0, 0.1, 1, ms_text);
    /* Game */
    add(TAB_GAME, tr(L"Language"), L"Game", L"Language", L"auto", language_choices());
    /* PC (2026-11-16): the launcher's and the game's PC panels' language
     * (the F2 menu's INTERFACE LANGUAGE): the launcher starts again in it */
    add(TAB_GAME, tr(L"Interface language"), L"Interface", L"Language", L"auto", interface_language_choices());
    add(TAB_GAME, tr(L"Speed unit"), L"Game", L"SpeedUnit", L"auto",
        {{tr(L"Auto"), L"auto"},
         {L"km/h, km", L"kmh"},
         {tr(L"mph, miles"), L"mph"}});
    add(TAB_GAME, tr(L"Power unit"), L"Game", L"PowerUnit", L"auto",
        {{tr(L"Auto"), L"auto"},
         {L"PS", L"ps"},
         {L"HP", L"hp"},
         {L"BHP", L"bhp"},
         {L"CV", L"cv"},
         {L"kW", L"kw"}});
    add(TAB_GAME, tr(L"Torque unit"), L"Game", L"TorqueUnit", L"auto",
        {{tr(L"Auto"), L"auto"},
         {L"kgf.m", L"kgfm"},
         {L"ft.lb", L"ftlb"},
         {L"Nm", L"nm"}});
    add(TAB_GAME, tr(L"Starting credits"), L"Game", L"StartingCredits", L"auto",
        {{tr(L"Default"), L"auto"},
         {L"15,000", L"15000"},
         {L"18,000", L"18000"},
         {tr(L"Custom"), L"custom"}});
    /* PC (2026-11-14): custom's amount (settings.h's kStartingCreditsCustomMin .. Max), shown with it */
    add_number(TAB_GAME, tr(L"Custom starting credits"), L"Game", L"StartingCreditsCustom", L"18000", 0, 999999999);
    g.options.back().whenKey = L"StartingCredits";
    g.options.back().whenValue = L"custom";
    add(TAB_GAME, tr(L"Split screen"), L"Graphics", L"SplitScreen", L"horizontal",
        {{tr(L"Horizontal"), L"horizontal"},
         {tr(L"Vertical"), L"vertical"},
         {tr(L"Two monitors"), L"monitors"}});
    /* Game > HUD (the colours: platform/hud_colors.h) */
    const size_t hud = g.options.size();
    add(TAB_GAME, tr(L"Split screen HUD"), L"Graphics", L"SplitHud", L"digital",
        {{tr(L"Digital"), L"digital"}, {tr(L"Analog"), L"analog"}});
    add(TAB_GAME, tr(L"Single player HUD"), L"Graphics", L"SingleHud", L"analog",
        {{tr(L"Analog"), L"analog"}, {tr(L"Digital"), L"digital"}});
    add(TAB_GAME, tr(L"Race HUD max width"), L"Display", L"HudMaxAspect", L"21:9",
        {{L"4:3", L"4:3"}, {L"16:10", L"16:10"}, {L"16:9", L"16:9"}, {L"21:9", L"21:9"}, {L"32:9", L"32:9"}});
    add_slider(TAB_GAME, tr(L"HUD safe zone"), L"Graphics", L"HudSafeZone", L"100", 75, 100, 1, 0, safe_zone_text);
    add(TAB_GAME, tr(L"Replay logo"), L"Graphics", L"ReplayLogo", L"on", {{tr(L"On"), L"on"}, {tr(L"Off"), L"off"}});
    for (const HudColorInfo &c : kHudColors) {
        Option o;
        o.tab = TAB_GAME;
        o.label = tr(c.label);              /* (hud_colors.h's table) */
        o.section = L"Hud";
        o.key = c.wideKey;
        o.def = L"";
        o.color = true;
        o.gameRgb = c.rgb;
        g.options.push_back(std::move(o));
    }
    for (size_t i = hud; i < g.options.size(); i++)
        g.options[i].sub = GAME_HUD;
    /* Game > Cameras (PC 2026-11-08: as the F2 menu's CAMERAS; the keys stay
     * in [Graphics]): settings.h's kFovMin .., kChaseAngleMin ..,
     * kCockpitFovMin .. */
    const size_t cameras = g.options.size();
    add_slider(TAB_GAME, tr(L"Field of view"), L"Graphics", L"Fov", L"100", 50, 200, 1, 0, percent_text);
    add_slider(TAB_GAME, tr(L"Replay field of view"), L"Graphics", L"ReplayFov", L"100", 50, 200, 1, 0, percent_text);
    /* (PC 2026-11-15: the rear view's two rows moved to the Graphics tab) */
    add(TAB_GAME, tr(L"Replay cameras"), L"Graphics", L"ExtraCameras", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add_slider(TAB_GAME, tr(L"Chase cam angle"), L"Graphics", L"ChaseAngle", L"0", -45, 45, 1, 0, angle_text);
    add(TAB_GAME, tr(L"Cockpit camera"), L"Graphics", L"CockpitCamera", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add_slider(TAB_GAME, tr(L"Cockpit field of view"), L"Graphics", L"CockpitFov", L"90", 30, 120, 1, 0, angle_text);
    /* (PC 2026-11-15: Cockpit menu model is per car now, the in-game
     * calibration's row: [CockpitMenuModels], settings.h) */
    for (size_t i = cameras; i < g.options.size(); i++)
        g.options[i].sub = GAME_CAMERAS;
    /* Game > Extras (settings.h's kCourseScaleMin .. kCourseScaleMax, not 0; the
     * height from kCourseScaleNearest: no negative, settings::course_scale_min) */
    const size_t extras = g.options.size();
    add_slider(TAB_GAME, tr(L"Course scale X"), L"Game", L"CourseScaleX", L"100", -1000, 1000, 5, 0, course_scale_text);
    add_slider(TAB_GAME, tr(L"Course scale Y"), L"Game", L"CourseScaleY", L"100", 5, 1000, 5, 0, course_height_text);
    add_slider(TAB_GAME, tr(L"Course scale Z"), L"Game", L"CourseScaleZ", L"100", -1000, 1000, 5, 0, course_scale_text);
    for (size_t i = g.options.size() - 3; i < g.options.size(); i++)
        g.options[i].noZero = true;
    add(TAB_GAME, tr(L"Prize car randomizer"), L"Game", L"PrizeCarRandomizer", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_GAME, tr(L"Course randomizer"), L"Game", L"CourseRandomizer", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}, {tr(L"Extended"), L"extended"}});
    /* PC (2026-11-13): settings.h's unrestricted_wheels (game/app/pc_wheel_shop.h) */
    add(TAB_GAME, tr(L"Unrestrict wheels"), L"Game", L"UnrestrictedWheels", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* PC (2026-11-14): settings.h's buy_not_for_sale (game/app/pc_car_shop.h) */
    add(TAB_GAME, tr(L"Buy cars not for sale"), L"Game", L"BuyNotForSale", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* PC (2026-11-23): settings.h's new_car_assists_off (GameStatus::addCar) */
    add(TAB_GAME, tr(L"New cars without assists"), L"Game", L"NewCarAssistsOff", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* (settings.h's kRaceCarsMin .. kRaceCarsMax) */
    add_slider(TAB_GAME, tr(L"Cars in a race"), L"Game", L"RaceCars", L"6", 6, 32, 1, 0, count_text);
    add(TAB_GAME, tr(L"Override race limits"), L"Game", L"RaceCarsOverride", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* PC (2026-11-08): settings.h's RevCut (game/app/pc_rev_cut.h) */
    add(TAB_GAME, tr(L"Rev cut behavior"), L"Game", L"RevCut", L"original",
        {{tr(L"Original"), L"original"}, {tr(L"Realistic"), L"realistic"}});
    /* PC (2026-11-08): settings.h's ShiftBehavior (game/app/pc_shift.h) */
    add(TAB_GAME, tr(L"Gear shift behavior"), L"Game", L"ShiftBehavior", L"original",
        {{tr(L"Original"), L"original"}, {tr(L"Realistic"), L"realistic"}, {tr(L"Realistic + Clutch"), L"clutch"}});
    /* PC (2026-11-09): settings.h's HShifter, shown with the clutch mode
     * (game/app/pc_clutch.h) */
    add(TAB_GAME, tr(L"H-shifter"), L"Game", L"HShifter", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    g.options.back().whenKey = L"ShiftBehavior";
    g.options.back().whenValue = L"clutch";
    /* (the car multipliers: settings.h's car_tune_min .. kCarTuneMax) */
    add_slider(TAB_GAME, tr(L"Tire wear (player)"), L"Game", L"TireWearPlayer", L"100", 0, 1000, 5, 0, percent_text);
    add_slider(TAB_GAME, tr(L"Tire wear (enemies)"), L"Game", L"TireWearEnemies", L"100", 0, 1000, 5, 0, percent_text);
    add_slider(TAB_GAME, tr(L"Power (player)"), L"Game", L"PowerPlayer", L"100", 10, 1000, 5, 0, percent_text);
    add_slider(TAB_GAME, tr(L"Power (enemies)"), L"Game", L"PowerEnemies", L"100", 10, 1000, 5, 0, percent_text);
    add_slider(TAB_GAME, tr(L"Grip (player)"), L"Game", L"GripPlayer", L"100", 10, 1000, 5, 0, percent_text);
    add_slider(TAB_GAME, tr(L"Grip (enemies)"), L"Game", L"GripEnemies", L"100", 10, 1000, 5, 0, percent_text);
    for (size_t i = extras; i < g.options.size(); i++)
        g.options[i].sub = GAME_EXTRAS;
    /* Game > Surfaces: each surface type's grip (0.01 .. 10.00) and rolling
     * resistance (0.00 .. 327.67), the raw values; settings.h's surface_key
     * order, the defaults its surface_tune_default (a key not written, or
     * removed by the row's Default button: the game's own) */
    {
        static const wchar_t *const grip[] = {N_(L"Tarmac grip"), N_(L"Kerbs grip"), N_(L"Grass grip"),
                                              N_(L"Sand grip"), N_(L"Gravel grip"), N_(L"Dirt grip"),
                                              N_(L"SF grip"), N_(L"WD grip"), N_(L"HT grip"),
                                              N_(L"Wet tarmac grip"), N_(L"Wet kerbs grip"), N_(L"Wet HT grip")};
        static const wchar_t *const resist[] = {N_(L"Tarmac resistance"), N_(L"Kerbs resistance"),
                                                N_(L"Grass resistance"), N_(L"Sand resistance"),
                                                N_(L"Gravel resistance"), N_(L"Dirt resistance"),
                                                N_(L"SF resistance"), N_(L"WD resistance"), N_(L"HT resistance"),
                                                N_(L"Wet tarmac resistance"), N_(L"Wet kerbs resistance"),
                                                N_(L"Wet HT resistance")};
        static const wchar_t *const gripKeys[] = {L"SurfaceGripTA", L"SurfaceGripGU", L"SurfaceGripGR",
                                                  L"SurfaceGripSA", L"SurfaceGripGV", L"SurfaceGripDT",
                                                  L"SurfaceGripSF", L"SurfaceGripWD", L"SurfaceGripHT",
                                                  L"SurfaceGripWR", L"SurfaceGripWH", L"SurfaceGripWG"};
        static const wchar_t *const resistKeys[] = {
            L"SurfaceResistanceTA", L"SurfaceResistanceGU", L"SurfaceResistanceGR", L"SurfaceResistanceSA",
            L"SurfaceResistanceGV", L"SurfaceResistanceDT", L"SurfaceResistanceSF", L"SurfaceResistanceWD",
            L"SurfaceResistanceHT", L"SurfaceResistanceWR", L"SurfaceResistanceWH", L"SurfaceResistanceWG"};
        static const wchar_t *const gripDefs[] = {L"1.00", L"0.90", L"0.60", L"0.60", L"0.60", L"0.80",
                                                  L"1.00", L"1.00", L"1.10", L"0.80", L"0.80", L"0.80"};
        static const wchar_t *const resistDefs[] = {L"0.10", L"0.10", L"0.20", L"25.50", L"0.30", L"0.20",
                                                    L"0.10", L"0.10", L"0.10", L"0.10", L"0.10", L"0.10"};
        const size_t surfaces = g.options.size();
        for (int s = 0; s < 12; s++)
            add_slider(TAB_GAME, tr(grip[s]), L"Game", gripKeys[s], gripDefs[s], 0.01, 10.0, 0.01, 2, raw_text);
        for (int s = 0; s < 12; s++)
            add_slider(TAB_GAME, tr(resist[s]), L"Game", resistKeys[s], resistDefs[s], 0.0, 327.67, 0.01, 2, raw_text);
        for (size_t i = surfaces; i < g.options.size(); i++) {
            g.options[i].sub = GAME_SURFACES;
            g.options[i].resettable = true;
        }
    }
    /* Game > Challenges (PC 2026-11-14: settings.h's [Challenges], game/app/
     * pc_challenge.h; user request) */
    {
        const size_t challenges = g.options.size();
        add(TAB_GAME, tr(L"Money decay"), L"Challenges", L"MoneyDecay", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
        add(TAB_GAME, tr(L"Car decay"), L"Challenges", L"CarDecay", L"off", {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
        add(TAB_GAME, tr(L"Course shrinkage"), L"Challenges", L"CourseShrinkage", L"off",
            {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
        for (size_t i = challenges; i < g.options.size(); i++)
            g.options[i].sub = GAME_CHALLENGES;
    }
    /* Debug */
    add(TAB_DEBUG, tr(L"Debug console"), L"Debug", L"Console", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Log file loads"), L"Debug", L"FileLog", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Log D3D syncs"), L"Debug", L"SyncLog", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Physics threads"), L"Debug", L"PhysicsThreads", L"auto",
        {{tr(L"Auto (all cores)"), L"auto"}, {tr(L"Off"), L"0"}, {L"2", L"2"}, {L"4", L"4"}, {L"8", L"8"}, {L"16", L"16"}});
    add(TAB_DEBUG, tr(L"Fill garage"), L"Debug", L"FillGarage", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Fill money"), L"Debug", L"FillMoney", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Unlock arcade tracks"), L"Debug", L"UnlockArcade", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Unlock all licenses"), L"Debug", L"UnlockLicenses", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    /* PC (2026-11-12): the menus' cars with their race models */
    add(TAB_DEBUG, tr(L"Menu race model"), L"Debug", L"MenuRaceModel", L"off",
        {{tr(L"Off"), L"off"}, {tr(L"On"), L"on"}});
    add(TAB_DEBUG, tr(L"Race model level of detail"), L"Debug", L"MenuRaceModelLod", L"auto",
        {{tr(L"Auto"), L"auto"}, {tr(L"0 (finest)"), L"0"}, {L"1", L"1"}, {tr(L"2 (coarsest)"), L"2"}});
}

/* A number of an ini value: "12.5", or "W:H" as W / H. */
bool parse_number(const std::wstring &s, double *v)
{
    const wchar_t *p = s.c_str();
    wchar_t *end = nullptr;
    double a = wcstod(p, &end);
    if (end == p)
        return false;
    if (*end == L':') {
        const wchar_t *q = end + 1;
        double b = wcstod(q, &end);
        if (end == q || b == 0.0)
            return false;
        a /= b;
    }
    while (*end == L' ')
        end++;
    if (*end != 0)
        return false;
    *v = a;
    return true;
}

/* Two ini values meaning the same: numbers within 0.15 % (the frame cap's
 * ms are rounded to 0.001), else the same text in any case. */
bool same_value(const std::wstring &a, const std::wstring &b)
{
    double x, y;
    if (parse_number(a, &x) && parse_number(b, &y))
        return std::fabs(x - y) <= 0.0015 * std::max(1.0, std::fabs(y));
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

/* A slider's positions: 0 .. slider_count, value min + position * step. */
int slider_count(const Option &o) { return (int)std::lround((o.max - o.min) / o.step); }
double slider_value(const Option &o, int pos)
{
    double v = o.min + std::clamp(pos, 0, slider_count(o)) * o.step;
    const double unit = std::pow(10.0, o.decimals);
    v = std::round(v * unit) / unit;
    return o.noZero && v == 0.0 ? o.step : v;
}

void fill_option(Option &o)
{
    wchar_t buf[128] = L"";
    GetPrivateProfileStringW(o.section, o.key, o.def, buf, 128, ini_path().c_str());
    const std::wstring cur = buf;
    if (o.color) {
        /* (as settings.cpp reads it: not 6 hex digits, the game's own) */
        wchar_t *end = nullptr;
        const unsigned long v = wcstoul(buf, &end, 16);
        o.rgb = cur.size() == 6 && *end == 0 ? (uint32_t)v : o.gameRgb;
        InvalidateRect(o.combo, nullptr, FALSE);
        return;
    }
    if (o.number) {
        /* PC (2026-11-14): the file's number held to min .. max, its digits
         * in threes (a field takes them with or without the commas) */
        double v = 0.0;
        if (!parse_number(cur, &v) && !parse_number(o.def, &v))
            v = o.min;
        v = std::clamp(std::floor(v + 0.5), o.min, o.max);
        o.cur = v;
        std::wstring digits = fmt(L"%.0f", v), text;
        for (size_t i = 0; i < digits.size(); i++) {
            if (i != 0 && (digits.size() - i) % 3 == 0)
                text += L',';
            text += digits[i];
        }
        SetWindowTextW(o.combo, text.c_str());
        SendMessageW(o.combo, EM_SETMODIFY, FALSE, 0);
        return;
    }
    if (o.step > 0) {
        /* the file's value as it is, the slider at the nearest step */
        double v = 0.0;
        if (!parse_number(cur, &v) && !parse_number(o.def, &v))
            v = o.min;
        o.cur = v;
        SendMessageW(o.combo, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(o.combo, TBM_SETRANGEMAX, FALSE, slider_count(o));
        SendMessageW(o.combo, TBM_SETLINESIZE, 0, 1);
        SendMessageW(o.combo, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(o.combo, TBM_SETPOS, TRUE,
                     std::clamp((int)std::lround((v - o.min) / o.step), 0, slider_count(o)));
        SetWindowTextW(o.value, o.text(v).c_str());
        return;
    }
    combo_reset(o.combo);
    int sel = -1;
    for (size_t i = 0; i < o.choices.size(); i++) {
        combo_add(o.combo, o.choices[i].text);
        if (sel < 0 && same_value(cur, o.choices[i].value))
            sel = (int)i;
    }
    /* a value none of the entries stands for, kept */
    if (sel < 0) {
        o.choices.push_back({trf(L"%s (as set)", cur.c_str()), cur});
        combo_add(o.combo, o.choices.back().text);
        sel = (int)o.choices.size() - 1;
    }
    SendMessageW(o.combo, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
}

void cap_show(double ms);                   /* (below: the frame cap's fields) */
double cap_ms();
void cap_changed(int id);
void cap_spin(int id, int delta);

void load_settings()
{
    g.disp = display::load(ini_path());
    combo_reset(g.mode);
    combo_add(g.mode, tr(L"Windowed"));         /* in display::Mode's order */
    combo_add(g.mode, tr(L"Borderless window"));
    combo_add(g.mode, tr(L"Fullscreen"));
    SendMessageW(g.mode, CB_SETCURSEL, (WPARAM)g.disp.mode, 0);
    fill_resolutions();
    fill_monitors();
    for (Option &o : g.options)
        fill_option(o);
    cap_show(cap_ms());
}

/* A colour's value, shown and written to gt3pc.ini at once (the game's own:
 * its key removed). */
void set_color(Option &o, uint32_t rgb)
{
    o.rgb = rgb;
    wchar_t hex[8];
    swprintf(hex, 8, L"%06X", rgb);
    WritePrivateProfileStringW(o.section, o.key, rgb == o.gameRgb ? nullptr : hex, ini_path().c_str());
    InvalidateRect(o.combo, nullptr, FALSE);
}

/* A colour's button pressed: the colour dialog. */
void pick_color(Option &o)
{
    static COLORREF custom[16];             /* the dialog's own colours, kept while the launcher runs */
    CHOOSECOLORW c = {};
    c.lStructSize = sizeof(c);
    c.hwndOwner = g.wnd;
    c.rgbResult = RGB((o.rgb >> 16) & 0xFF, (o.rgb >> 8) & 0xFF, o.rgb & 0xFF);
    c.lpCustColors = custom;
    c.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&c))
        set_color(o, ((uint32_t)GetRValue(c.rgbResult) << 16) | ((uint32_t)GetGValue(c.rgbResult) << 8) |
                         GetBValue(c.rgbResult));
}

/* A colour's button: filled with it, its value over it. */
void draw_color(const DRAWITEMSTRUCT *d, const Option &o)
{
    const int r = (o.rgb >> 16) & 0xFF, gr = (o.rgb >> 8) & 0xFF, b = o.rgb & 0xFF;
    HBRUSH fill = CreateSolidBrush(RGB(r, gr, b));
    FillRect(d->hDC, &d->rcItem, fill);
    DeleteObject(fill);
    FrameRect(d->hDC, &d->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, r * 3 + gr * 6 + b >= 1280 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(d->hDC, g.font);
    wchar_t text[8];
    swprintf(text, 8, L"%06X", o.rgb);
    RECT t = d->rcItem;
    DrawTextW(d->hDC, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d->hDC, old);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = d->rcItem;
        InflateRect(&f, -px(3), -px(3));
        DrawFocusRect(d->hDC, &f);
    }
}

/* A slider moved (WM_HSCROLL's code): its value shown, and written to
 * gt3pc.ini once it is let go (not while its thumb is dragged). */
void slider_moved(int id, int code)
{
    if (id < IDC_OPTION0 || id >= IDC_OPTION0 + (int)g.options.size())
        return;
    Option &o = g.options[id - IDC_OPTION0];
    if (o.step <= 0)
        return;
    const double v = slider_value(o, (int)SendMessageW(o.combo, TBM_GETPOS, 0, 0));
    SetWindowTextW(o.value, o.text(v).c_str());
    if (code == TB_THUMBTRACK || std::fabs(v - o.cur) < 1e-6)
        return;
    o.cur = v;
    const wchar_t *f = o.decimals >= 2 ? L"%.2f" : o.decimals == 1 ? L"%.1f" : L"%.0f";
    WritePrivateProfileStringW(o.section, o.key, fmt(f, v).c_str(), ini_path().c_str());
}

/* PC (2026-11-14): a number's field left (or Enter in it): its digits -
 * whatever separators were typed with them - held to min .. max and written;
 * no digit at all: the file's value shown again. */
void number_changed(Option &o)
{
    wchar_t text[64] = L"";
    GetWindowTextW(o.combo, text, 64);
    std::wstring digits;
    for (const wchar_t *p = text; *p != 0; p++)
        if (*p >= L'0' && *p <= L'9')
            digits += *p;
    if (!digits.empty()) {
        const double v = std::clamp(wcstod(digits.c_str(), nullptr), o.min, o.max);
        WritePrivateProfileStringW(o.section, o.key, fmt(L"%.0f", v).c_str(), ini_path().c_str());
    }
    fill_option(o);
}

/* A slider's Default button: its key removed, the slider at the default. */
void slider_default(Option &o)
{
    WritePrivateProfileStringW(o.section, o.key, nullptr, ini_path().c_str());
    fill_option(o);
}

/* PC (2026-11-09): the clutch mode chosen (Gear shift behavior, Realistic +
 * Clutch): a popup of what each controller's / wheel's Clutch is mapped to,
 * or that nothing is and where to map it (the Controls tab). */
void clutch_popup()
{
    const std::wstring iniW = ini_path();
    const int n = WideCharToMultiByte(CP_ACP, 0, iniW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ini(n > 0 ? (size_t)n : 1, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, 0, iniW.c_str(), -1, &ini[0], n, nullptr, nullptr);
    ini.resize(strlen(ini.c_str()));
    std::wstring mapped;
    for (int p = input_map::kController1; p <= input_map::kWheel2; p++) {
        input_map::Profile prof;
        input_map::load(ini.c_str(), p, &prof);
        const int c = input_map::clutch_control(p);
        std::wstring what;
        for (int s = 0; s < input_map::kSlots && c >= 0; s++) {
            char d[160];
            input_map::describe(prof.map[c][s], d, sizeof(d));
            if (d[0] == '\0')
                continue;
            wchar_t w[160];
            MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 160);
            what += (what.empty() ? L"" : L" / ") + std::wstring(w);
        }
        if (!what.empty()) {
            wchar_t name[128];              /* (input_map.cpp's table: "Controller 1" .. "Wheel 2") */
            MultiByteToWideChar(CP_UTF8, 0, tr(input_map::profile_name(p)), -1, name, 128);
            mapped += std::wstring(L"\n") + name + L": " + what;
        }
    }
    /* ("85 %%": formats, so the percent sign is one in a translation too) */
    if (mapped.empty())
        message(trf(L"Realistic + Clutch: nothing is mapped to the clutch.\n\nAdd a mapping in the Controls tab: choose a "
                    L"controller or a wheel, then its Clutch row (an axis or a pedal; 85 %% of its travel is fully "
                    L"out).\n\nUntil then the gears shift as Realistic."),
                MB_ICONWARNING);
    else
        message(trf(L"Realistic + Clutch: the clutch is mapped to%s\n\n85 %% of its travel is fully out. Change it in "
                    L"the Controls tab.",
                    mapped.c_str()),
                MB_ICONINFORMATION);
}

/* PC (2026-11-16): the interface's language ([Interface] Language,
 * platform/ui_text.h) chosen: the launcher starts again in it - its window at
 * once (--engine), with the gt3pc.ini it reads (--shared) - unless an install
 * runs (then from its next start). */
void restart_in_language()
{
    if (g.worker.joinable()) {
        message(tr(L"The launcher shows the new language from its next start."), MB_ICONINFORMATION);
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --engine";
    if (_wcsicmp(g.shared.c_str(), g.folder.c_str()) != 0) {
        std::wstring s = g.shared;
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
            s.pop_back();               /* (a \ before the closing quote would escape it) */
        cmd += L" --shared \"" + s + L"\"";
    }
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        message(tr(L"The launcher shows the new language from its next start."), MB_ICONINFORMATION);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageW(g.wnd, WM_CLOSE, 0, 0);
}

/* A list's choice, written to gt3pc.ini at once. */
void setting_changed(int id)
{
    if (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size()) {
        Option &o = g.options[id - IDC_OPTION0];
        int i = combo_sel(o.combo);
        if (i >= 0 && i < (int)o.choices.size()) {
            WritePrivateProfileStringW(o.section, o.key, o.choices[i].value.c_str(), ini_path().c_str());
            /* PC (2026-11-09): the H-shifter's row with the clutch mode, its
             * popup when chosen */
            if (_wcsicmp(o.key, L"ShiftBehavior") == 0) {
                show_game_rows();
                layout();
                if (o.choices[i].value == L"clutch")
                    clutch_popup();
            }
            /* PC (2026-11-14): the custom amount's row with Custom */
            if (_wcsicmp(o.key, L"StartingCredits") == 0) {
                show_game_rows();
                layout();
            }
            /* PC (2026-11-16): the interface's language */
            if (_wcsicmp(o.section, L"Interface") == 0 && _wcsicmp(o.key, L"Language") == 0) {
                restart_in_language();
                return;
            }
        }
        return;
    }
    if (id == IDC_MODE) {
        int i = combo_sel(g.mode);
        if (i < 0)
            return;
        g.disp.mode = (display::Mode)i;
        fill_resolutions();
        EnableWindow(g.span, g.disp.mode != display::Mode::windowed);   /* (its choice kept) */
    } else if (id == IDC_RES) {
        int i = combo_sel(g.res);
        if (i < 0 || i >= (int)g.resSizes.size())
            return;
        const display::Size &s = g.resSizes[i];
        if (g.disp.mode == display::Mode::fullscreen) {
            g.disp.width = s.w;
            g.disp.height = s.h;
            fill_rates();
        } else if (g.disp.mode == display::Mode::windowed) {
            g.disp.windowW = s.w;
            g.disp.windowH = s.h;
        }
    } else if (id == IDC_RATE) {
        int i = combo_sel(g.rate);
        if (i < 0 || i >= (int)g.rateValues.size())
            return;
        g.disp.refresh = g.rateValues[i];
    } else if (id == IDC_SPAN) {
        int i = combo_sel(g.span);
        if (i < 0 || i >= (int)g.spanValues.size())
            return;
        g.disp.spanMonitors = g.spanValues[i];
    } else if (id == IDC_SPLIT) {
        int i = combo_sel(g.split);
        if (i < 0 || i >= (int)g.splitValues.size())
            return;
        g.disp.splitMonitor = g.splitValues[i];
    }
    display::save(ini_path(), g.disp);
}

/* Starts version v: gt3pc.exe --data <its data folder>, remembered as the
 * last played (gt3pc.ini [Game] Version: the game started without --data
 * takes it too, platform/paths.h). */
void add_new_songs(int v);                  /* (below) */
void add_new_songs_in(const std::wstring &data, const std::wstring &name);
std::wstring disc_song_name(const std::wstring &release);

void play(int v)
{
    if (v < 0 || v >= install::kVersions || version_state(v) != install::InstallState::ok) {
        refresh();
        return;
    }
    std::wstring exe = g.folder + kGameExe;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message(trf(L"The game (%s) was not found in %s.", kGameExe, g.folder.c_str()), MB_ICONERROR);
        return;
    }
    add_new_songs(v);                    /* (songs put into its music folder since the launcher started) */
    /* (PC 2026-11-14: in the engine's own file, engines.h; no disc image is
     * the last played: platform/paths.h) */
    WritePrivateProfileStringW(L"Game", L"Version", util::widen(install::version_code((install::Version)v)).c_str(),
                               engine_ini_path().c_str());
    WritePrivateProfileStringW(L"Game", L"Disc", nullptr, engine_ini_path().c_str());
    ensure_save_folders();                  /* PC (2026-11-15): the game never without its two cards */
    controls::show(false);                  /* (SDL lets go of the devices before the game opens them) */
    std::wstring cmd = L"\"" + exe + L"\" --data \"" + g.folder + install::version_folder((install::Version)v) + L"\"" +
                       shared_arg();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g.folder.c_str(), &si, &pi)) {
        message(trf(L"The game could not be started: %s.", util::system_error_text(GetLastError()).c_str()), MB_ICONERROR);
        controls::show(g.tab == TAB_CONTROLS);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g.wnd);
}

/* The worker's view of the window. */
class WindowListener : public install::Listener {
public:
    void phase(const std::wstring &text) override
    {
        auto *s = new std::wstring(text);
        if (!PostMessageW(g.wnd, WM_APP_PHASE, 0, (LPARAM)s))
            delete s;
    }
    void progress(uint64_t done, uint64_t total) override
    {
        g.done = done;
        g.total = total;
        int permille = total ? (int)(done * 1000 / total) : 0;
        if (permille != last_) {
            last_ = permille;
            PostMessageW(g.wnd, WM_APP_PROGRESS, 0, 0);
        }
    }
    bool cancel_requested() override { return g.cancel_requested; }

private:
    int last_ = -1;
};

/* Installs from the disc image `source`, or unpacks the earlier version's
 * volume file `source` when from_volume. */
void start_install(const std::wstring &source, bool from_volume)
{
    g.installing = true;
    g.close_when_done = false;
    g.cancel_requested = false;
    g.done = 0;
    g.total = 0;
    g.copying = false;
    g.phase.clear();
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    refresh();
    SetFocus(g.cancel);
    std::wstring folder = g.folder;
    g.worker = std::thread([source, from_volume, folder] {
        WindowListener listener;
        std::wstring msg;
        install::Result r = from_volume ? install::install_from_volume(source, folder, listener, &msg)
                                        : install::install_from_iso(source, folder, listener, &msg);
        g.result = r;
        g.result_message = msg;
        PostMessageW(g.wnd, WM_APP_DONE, 0, 0);
    });
}

/* The new songs in version v's music folder (music.h) added to its race
 * songs - their tags' titles and artists, else their names and "unknown" -
 * marked new in the song manager, and noted on the Audio tab's Songs page
 * (with the files that could not be added). */
void add_new_songs_in(const std::wstring &data, const std::wstring &name);

void add_new_songs(int v)
{
    add_new_songs_in(g.folder + install::version_folder((install::Version)v), install::version_name((install::Version)v));
}

/* (PC 2026-11-15: the data folder `data', named `name': an installed
 * version's, or a disc image's) */
void add_new_songs_in(const std::wstring &data, const std::wstring &name)
{
    std::vector<std::wstring> unplayable;
    std::vector<music::NewSong> songs = music::find_new_songs(data, &unplayable);
    if (!songs.empty()) {
        std::wstring m;
        if (music::add_songs(data, songs, &m)) {
            std::wstring files;
            for (size_t i = 0; i < songs.size(); i++) {
                song_manager::mark_new(data, songs[i].file);
                files += (i ? L", " : L"") + songs[i].file;
            }
            music_note(trf(L"%s: %s (%s) Set their titles, artists and tempos (BPM: the replay cameras' beat markers) in "
                           L"the song manager.",
                           name.c_str(), m.c_str(), files.c_str()));
        } else {
            music_note(name + L": " + m);
        }
    }
    for (const std::wstring &u : unplayable)
        music_note(trf(L"%s: not added, %s.", name.c_str(), u.c_str()));
}

/* add_new_songs for every installed version, and (PC 2026-11-15) every disc
 * image of the list with its data folder (the songs put into its bgm\). */
void add_all_new_songs()
{
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok)
            add_new_songs(v);
    for (const SongFolder &d : disc_song_folders())
        add_new_songs_in(d.data, d.name);
    disc_files::release_all();
}

/* The game configs and the database folders of the installed versions
 * written as SQLite databases, and their arcade stills as PNG files, where
 * they are not yet (gameconf.h, databases.h, stills.h), and the demo replays
 * built into this launcher written where an installed version's differ
 * (install::refresh_demos); what was done in a message box (the window
 * shown: the status line is too short for it). */
void convert_all_tables()
{
    std::wstring confDone, confRemoved, dbDone, dbUpdated, stillDone, packDone, demoDone, problems;
    auto add = [](std::wstring *to, const std::wstring &version, const std::vector<std::wstring> &names) {
        std::wstring list;
        for (const std::wstring &n : names)
            list += (list.empty() ? L"" : L", ") + n;
        if (!list.empty())
            *to += L"\n    " + version + L": " + list;
    };
    for (int v = 0; v < install::kVersions; v++) {
        const std::wstring data = g.folder + install::version_folder((install::Version)v);
        if (version_state(v) != install::InstallState::ok)
            continue;
        const std::wstring version = install::version_name((install::Version)v);
        std::wstring m;
        std::vector<std::wstring> names, failed;
        if (gameconf::needs_conversion(data)) {
            std::vector<std::wstring> removed;
            gameconf::convert(data, &m, &names, &failed, &removed);
            add(&confDone, version, names);
            add(&confRemoved, version, removed);
            for (const std::wstring &f : failed)
                problems += L"\n    " + version + L": gameconf\\" + f;
        }
        if (databases::needs_conversion(data)) {
            std::vector<std::wstring> updated;
            databases::convert(data, &m, &names, &failed, &updated);
            add(&dbDone, version, names);
            add(&dbUpdated, version, updated);
            for (const std::wstring &f : failed)
                problems += L"\n    " + version + L": " + f;
        }
        if (stills::needs_conversion(data)) {
            std::wstring note;
            if (stills::convert(data, &note))
                add(&stillDone, version, {note});
            else
                problems += L"\n    " + version + L": " + note;
        }
        /* PC (2026-11-08): the image packs as folders of PNG files
         * (image_packs.h); what could not be converted stays */
        if (image_packs::needs_conversion(data)) {
            std::wstring note;
            if (image_packs::convert(data, &note))
                add(&packDone, version, {note});
            else
                problems += L"\n    " + version + L": " + note;
        }
        /* (a folder converted before the pictures' sizes were kept: from the
         * disc image) */
        if (image_packs::needs_sizes(data)) {
            std::wstring note;
            if (image_packs::write_sizes(data, &note))
                add(&packDone, version, {note});
            else
                problems += L"\n    " + version + L": " + note;
        }
        {
            int written = 0;
            std::wstring note;
            if (!install::refresh_demos(data, &written, &note))
                problems += L"\n    " + version + L": " + note;
            else if (written != 0)
                add(&demoDone, version, {trf(L"%d demo replays", written)});
        }
    }
    if (confDone.empty() && confRemoved.empty() && dbDone.empty() && dbUpdated.empty() && stillDone.empty() &&
        packDone.empty() && demoDone.empty() && problems.empty())
        return;
    std::wstring text;
    if (!confDone.empty())
        text = trf(L"The game configs were written as SQLite databases, which the game reads from now on:\n%s"
                   L"\n\nEach is gameconf\\<config>.sqlite3 in the version's folder, a table for each part of the "
                   L"config, for any SQLite editor (DB Browser for SQLite...); the tables' schema says what the columns "
                   L"are. The .gcf files they were made of are deleted.",
                   confDone.c_str());
    if (!confRemoved.empty())
        text += (text.empty() ? L"" : L"\n\n") +
                trf(L"The game configs' .gcf files were deleted - their databases "
                    L"(gameconf\\<config>.sqlite3), which the game reads, hold what they held:\n%s",
                    confRemoved.c_str());
    if (!dbDone.empty())
        text += (text.empty() ? L"" : L"\n\n") +
                trf(L"Game data files were written as SQLite databases, which the game reads from now "
                    L"on:\n%s"
                    L"\n\nA region's car database is database\\paramdb<region>.sqlite3 in the version's folder (its "
                    L"parameters, strings and labels in one), the colours and race modes database\\common.sqlite3, "
                    L"the texts the game prints in each language text\\realtime.sqlite3, for any SQLite editor; the "
                    L"tables' schema says what the columns are. The files they were made of are deleted.",
                    dbDone.c_str());
    if (!dbUpdated.empty())
        text += (text.empty() ? L"" : L"\n\n") +
                trf(L"Databases updated (the race display's rows that had only a number for a name now "
                    L"have names, which the game takes as before):\n%s",
                    dbUpdated.c_str());
    if (!stillDone.empty())
        text += (text.empty() ? L"" : L"\n\n") +
                trf(L"The arcade menus' course and class stills were written as PNG files, which the game "
                    L"reads from now on:\n%s",
                    stillDone.c_str());
    if (!packDone.empty())
        text += (text.empty() ? L"" : L"\n\n") +
                trf(L"The image packs (.imgs: the race HUD's pictures, race\\<language>\\display.imgs, and "
                    L"the packs the game leaves unused) were written as folders of PNG files, which the game "
                    L"reads from now on - race\\US\\display\\<picture>.png, any picture editor's, alpha "
                    L"included; a picture of another size is drawn at its original's (the folder's "
                    L"sizes.txt):\n%s",
                    packDone.c_str());
    if (!demoDone.empty())
        text += (text.empty() ? L"" : L"\n\n") +
                trf(L"The demo replays of the licence tests and the arcade time trials built into this "
                    L"launcher were written where the installed ones differed (demo\\<name>, and the "
                    L"disc's copies of it):\n%s",
                    demoDone.c_str());
    if (!problems.empty())
        text += (text.empty() ? L"" : L"\n\n") + trf(L"Left as they were:\n%s", problems.c_str());
    MessageBoxW(g.wnd, text.c_str(), kAppName, MB_OK | (problems.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
}

/* The beat markers an earlier conversion misread, repaired in every
 * installed version's ads.txt (music::repair_markers; a notice each). */
void repair_all_markers()
{
    for (int v = 0; v < install::kVersions; v++) {
        if (version_state(v) != install::InstallState::ok)
            continue;
        std::wstring m;
        if (music::repair_markers(g.folder + install::version_folder((install::Version)v), &m) != 0)
            music_note(std::wstring(install::version_name((install::Version)v)) + L": " + m);
    }
}

/* The music of the installed versions that need it converted (music.h), one
 * after the other on the worker; false when none needs it. */
bool start_conversion()
{
    std::vector<int> versions;
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok &&
            music::needs_conversion(g.folder + install::version_folder((install::Version)v)))
            versions.push_back(v);
    if (versions.empty())
        return false;
    g.installing = true;
    g.converting = true;
    g.close_when_done = false;
    g.cancel_requested = false;
    g.done = 0;
    g.total = 0;
    g.copying = false;
    g.phase.clear();
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    refresh();
    std::wstring folder = g.folder;
    g.worker = std::thread([versions, folder] {
        WindowListener listener;
        install::Result worst = install::Result::ok;
        std::wstring msg;
        for (int v : versions) {
            listener.phase(trf(L"Converting the music of %s to WAV (once)...",
                               install::version_name((install::Version)v)));
            std::wstring m;
            install::Result r =
                music::convert(folder + install::version_folder((install::Version)v), listener, &m);
            msg += (msg.empty() ? L"" : L"\n") + std::wstring(install::version_name((install::Version)v)) + L": " + m;
            if (r != install::Result::ok)
                worst = r;
            if (r == install::Result::cancelled)
                break;
        }
        g.result = worst;
        g.result_message = msg;
        PostMessageW(g.wnd, WM_APP_DONE, 0, 0);
    });
    return true;
}

/* After an install: the earlier version's gt3.vol is not read any more. */
void offer_volume_delete()
{
    if (!install::has_volume(g.folder))
        return;
    std::wstring vol = g.folder + install::kVolumeFile;
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    uint64_t size = GetFileAttributesExW(vol.c_str(), GetFileExInfoStandard, &a)
                        ? ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow
                        : 0;
    std::wstring q = trf(L"The game now reads the unpacked files in the data folder, and no longer uses gt3.vol (%s), "
                         L"which an earlier version installed.\n\nDelete gt3.vol?",
                         util::format_size(size).c_str());
    if (MessageBoxW(g.wnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!DeleteFileW(vol.c_str()))
        message(trf(L"gt3.vol could not be deleted: %s.", util::system_error_text(GetLastError()).c_str()), MB_ICONWARNING);
    refresh();
}

void install_done()
{
    if (g.worker.joinable())
        g.worker.join();   /* after the join g.result is the worker's */
    g.installing = false;
    if (g.close_when_done) {
        DestroyWindow(g.wnd);
        return;
    }
    if (g.converting) {
        /* the music's notices: on the Audio tab's Songs page */
        g.converting = false;
        refresh();
        if (g.result == install::Result::cancelled)
            music_note(tr(L"The music conversion was stopped: it carries on the next time the launcher starts."));
        else
            music_note(g.result_message + (g.result == install::Result::ok
                                               ? std::wstring()
                                               : L"\n" + std::wstring(tr(L"(The game plays the files not "
                                                                         L"converted as they were.)"))));
        add_all_new_songs();                /* (a mod's .ads, converted: new songs) */
        SetFocus(default_button());
        return;
    }
    /* the languages the files have now */
    for (Option &o : g.options)
        if (o.step <= 0 && wcscmp(o.key, L"Language") == 0 && wcscmp(o.section, L"Game") == 0) {
            o.choices = language_choices();
            fill_option(o);
        }
    switch (g.result) {
    case install::Result::ok:
        refresh();
        offer_volume_delete();
        SetFocus(default_button());
        break;
    case install::Result::cancelled:
        refresh(tr(L"The installation was cancelled."));
        SetFocus(g.install);
        break;
    case install::Result::wrong_disc:
        refresh();
        message(g.result_message, MB_ICONWARNING);
        SetFocus(g.install);
        break;
    default:
        refresh(tr(L"The installation failed."));
        message(g.result_message, MB_ICONERROR);
        SetFocus(g.install);
        break;
    }
}

/* The Open dialog's filter for a disc image: "PlayStation 2 disc image
 * (*.iso; *.chd)\0*.iso;*.chd\0All files (*.*)\0*.*\0" (its descriptions
 * translated).  PC (2026-11-23): a CHD too (chd_image.h; the image is told
 * by its first bytes: one of another name opens through "All files"). */
std::wstring disc_image_filter()
{
    std::wstring f = tr(L"PlayStation 2 disc image");
    f += L" (*.iso; *.chd)";
    f.push_back(L'\0');
    f += L"*.iso;*.chd";
    f.push_back(L'\0');
    f += tr(L"All files");
    f += L" (*.*)";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    return f;                               /* (c_str()'s own L'\0' ends the list) */
}

void choose_and_install()
{
    if (g.installing)
        return;
    /* An earlier version's gt3.vol holds the US disc's files: no image
     * needed while that version is not installed. */
    if (version_state((int)install::Version::us) != install::InstallState::ok && install::has_volume(g.folder)) {
        int a = MessageBoxW(g.wnd,
                            tr(L"This folder has gt3.vol, the game files as an earlier version installed them.\n\n"
                               L"Unpack the game files from gt3.vol? (No: install them from a disc image instead.)"),
                            kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL)
            return;
        if (a == IDYES) {
            start_install(g.folder + install::kVolumeFile, true);
            return;
        }
    }

    std::wstring file(32768, L'\0');
    const std::wstring filter = disc_image_filter();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = &file[0];
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = tr(L"Choose a Gran Turismo 3 disc image (USA, Europe, Japan; other Gran Turismo 3 engine discs: data only)");
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn))
        return;
    file.resize(wcslen(file.c_str()));

    /* which disc it is: its data folder, replaced or extracted (the worker
     * checks it again) */
    iso9660::Image image;
    std::wstring err, msg;
    install::DiscInfo disc;
    bool wrong = false;
    if (!image.open(file, &err)) {
        message(trf(L"The disc image could not be used: %s.", err.c_str()), MB_ICONERROR);
        return;
    }
    if (!install::check_disc(image, &disc, &msg, &wrong)) {
        message(msg, wrong ? MB_ICONWARNING : MB_ICONERROR);
        return;
    }
    image.close();
    const bool dataOnly = disc.version == install::Version::data_only;
    std::wstring q = dataOnly ? trf(L"%s is not a version the game plays: its files are extracted as data "
                                    L"only, into %s.",
                                    disc.release.c_str(), disc.data.c_str())
                              : trf(L"%s is installed as %s, into %s.", disc.release.c_str(),
                                    install::version_name(disc.version), disc.data.c_str());
    if (install::install_state(g.folder, disc.data) == install::InstallState::ok)
        q += L"\n\n" + trf(L"The files of %s there are replaced.",
                           install::installed_release(g.folder, disc.data).c_str());
    q += L"\n\n";
    q += dataOnly ? tr(L"Extract them?") : tr(L"Install?");
    if (MessageBoxW(g.wnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    start_install(file, false);
}

/* ---- the Disc sub-tab (PC 2026-11-14) -------------------------------------------- */

/* The rows of the disc images of gt3pc.ini [Discs], made again (the list
 * read again): each its release (the file's name without one), its path
 * under it, Play and Remove.  refresh() sets the paths and lays them out. */
void rebuild_disc_rows()
{
    HWND p = g.pages[TAB_PLAY];
    /* (the tooltip's tools: a button's own window, taken out with it) */
    TOOLINFOW ti = {};
    ti.cbSize = TTTOOLINFOW_V1_SIZE;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = p;
    for (const DiscRow &r : g.discRows) {
        ti.uId = (UINT_PTR)r.remove;
        SendMessageW(g.tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
        for (HWND c : {r.name, r.path, r.play, r.remove})
            DestroyWindow(c);
    }
    g.discRows.clear();
    g.discs = disc_list::load(engine_ini_path());
    g.discThere.assign(g.discs.size(), 0);
    const DWORD shown = g.playSub == PLAY_DISC ? WS_VISIBLE : 0;
    for (size_t i = 0; i < g.discs.size(); i++) {
        const disc_list::Disc &d = g.discs[i];
        const std::wstring name = !d.name.empty() ? d.name : d.path.substr(d.path.find_last_of(L"\\/") + 1);
        DiscRow r;
        r.name = CreateWindowExW(0, WC_STATICW, name.c_str(),
                                 WS_CHILD | shown | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10, p,
                                 (HMENU)(INT_PTR)(IDC_DISC_NAME0 + (int)i), g.inst, nullptr);
        r.path = CreateWindowExW(0, WC_STATICW, d.path.c_str(),
                                 WS_CHILD | shown | SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10, p,
                                 (HMENU)(INT_PTR)(IDC_DISC_PATH0 + (int)i), g.inst, nullptr);
        r.play = CreateWindowExW(0, WC_BUTTONW, tr(L"&Play"), WS_CHILD | shown | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, p,
                                 (HMENU)(INT_PTR)(IDC_DISC_PLAY0 + (int)i), g.inst, nullptr);
        r.remove = CreateWindowExW(0, WC_BUTTONW, L"X", WS_CHILD | shown | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, p,
                                   (HMENU)(INT_PTR)(IDC_DISC_REMOVE0 + (int)i), g.inst, nullptr);
        ti.uId = (UINT_PTR)r.remove;
        ti.lpszText = const_cast<wchar_t *>(tr(kDiscRemoveTip));
        SendMessageW(g.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(r.name, WM_SETFONT, (WPARAM)(g.tabFont ? g.tabFont : g.font), FALSE);
        for (HWND c : {r.path, r.play, r.remove})
            SendMessageW(c, WM_SETFONT, (WPARAM)g.font, FALSE);
        g.discRows.push_back(r);
    }
    ShowWindow(g.discNone, g.playSub == PLAY_DISC && g.discRows.empty() ? SW_SHOW : SW_HIDE);
}

/* Starts the game on the disc image at `path', straight from the file: its
 * own small data folder readied (install::prepare_disc: checked, the marker
 * naming the image, the demo replays), the image kept in the list as the
 * last played (the first row) and in [Game] Disc (the game started without
 * --data takes it: platform/paths.h), then gt3pc.exe --data <that folder>. */
void play_disc(const std::wstring &path)
{
    if (g.installing)
        return;
    const std::wstring exe = g.folder + kGameExe;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message(trf(L"The game (%s) was not found in %s.", kGameExe, g.folder.c_str()), MB_ICONERROR);
        return;
    }
    install::DiscFolder disc;
    std::wstring msg;
    HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const install::Result r = install::prepare_disc(path, g.folder, &disc, &msg);
    SetCursor(cursor);
    if (r != install::Result::ok) {
        message(msg, r == install::Result::wrong_disc ? MB_ICONWARNING : MB_ICONERROR);
        refresh();
        return;
    }
    disc_list::remember(engine_ini_path(), path, disc.release, disc.name);
    WritePrivateProfileStringW(L"Game", L"Disc", disc.name.c_str(), engine_ini_path().c_str());
    /* (PC 2026-11-15: songs put into its folder's bgm\ since the launcher started, as an installed version's) */
    add_new_songs_in(g.folder + install::kDiscsFolder + L"\\" + disc.name, disc_song_name(disc.release));
    ensure_save_folders();                  /* PC (2026-11-15) */
    controls::show(false);                  /* (SDL lets go of the devices before the game opens them) */
    std::wstring cmd = L"\"" + exe + L"\" --data \"" + g.folder + install::kDiscsFolder + L"\\" + disc.name + L"\"" +
                       shared_arg();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g.folder.c_str(), &si, &pi)) {
        message(trf(L"The game could not be started: %s.", util::system_error_text(GetLastError()).c_str()), MB_ICONERROR);
        controls::show(g.tab == TAB_CONTROLS);
        rebuild_disc_rows();
        refresh();
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g.wnd);
}

/* Add disc... (PC 2026-11-15, user request; was Play from disc..., which
 * played it at once): a disc image chosen in the Open dialog, checked and
 * its data folder readied (install::prepare_disc: the song manager works on
 * it from now on), first in the list - not started: its row's Play is the
 * only way to start it.  An image the list has already moves first. */
void choose_and_add_disc()
{
    if (g.installing)
        return;
    std::wstring file(32768, L'\0');
    const std::wstring filter = disc_image_filter();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = &file[0];
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = tr(L"Choose a Gran Turismo 3 disc image to add (USA, Europe, Japan)");
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn))
        return;
    file.resize(wcslen(file.c_str()));
    install::DiscFolder disc;
    std::wstring msg;
    HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const install::Result r = install::prepare_disc(file, g.folder, &disc, &msg);
    SetCursor(cursor);
    if (r != install::Result::ok) {
        message(msg, r == install::Result::wrong_disc ? MB_ICONWARNING : MB_ICONERROR);
        refresh();
        return;
    }
    disc_list::remember(engine_ini_path(), file, disc.release, disc.name);
    rebuild_disc_rows();
    refresh(trf(L"%s added: its Play starts it.", disc.release.c_str()));
    layout();                               /* (the Songs page counts it) */
    if (!g.discRows.empty())
        SetFocus(g.discRows[0].play);
}

/* A row's Remove: the image out of the list (its file and its folder in
 * discs\ stay); the focus to the row now in its place, else Play from
 * disc.... */
void remove_disc(size_t index)
{
    if (g.installing || index >= g.discs.size())
        return;
    disc_list::remove(engine_ini_path(), index);
    rebuild_disc_rows();
    refresh();
    SetFocus(index < g.discRows.size() ? g.discRows[index].remove
                                       : !g.discRows.empty() ? g.discRows.back().remove : g.discOpen);
    layout();                               /* (PC 2026-11-15: the Songs page has it no more) */
}

bool ask(const wchar_t *question)
{
    return MessageBoxW(g.wnd, question, kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

/* Stops a running install (the worker checks between files and deletes
 * its temporary folder). */
void request_cancel()
{
    g.cancel_requested = true;
    EnableWindow(g.cancel, FALSE);
    g.phase = tr(L"Cancelling...");
    g.copying = false;
    show_progress_text();
}

void cancel_clicked()
{
    /* (the install may finish while the question is up) */
    if (g.installing && !g.cancel_requested &&
        ask(g.converting ? tr(L"Stop converting the music? (It carries on the next time the launcher starts.)")
                         : tr(L"Stop installing the game files?")) &&
        g.installing)
        request_cancel();
}


/* ---- the window procedure ------------------------------------------------------- */

HWND make_button(const wchar_t *text, int id)
{
    return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g.wnd,
                           (HMENU)(INT_PTR)id, g.inst, nullptr);
}

HWND make_tab(int i)
{
    return CreateWindowExW(0, WC_BUTTONW, tr(kTabNames[i]), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                           g.wnd, (HMENU)(INT_PTR)(IDC_TAB0 + i), g.inst, nullptr);
}

HWND make_label(HWND parent, const wchar_t *text, int id)
{
    return CreateWindowExW(0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, g.inst, nullptr);
}

HWND make_combo(HWND parent, int id)
{
    return CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                           0, 10, 10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

/* A row of tab `tab': a label and a list, on its page (shown with it:
 * show_tab). */
HWND add_row(int tab, const wchar_t *label, int id)
{
    Row r;
    r.tab = tab;
    r.label = make_label(g.pages[tab], label, IDC_LABEL0 + (int)g.rows.size());
    r.combo = make_combo(g.pages[tab], id);
    g.rows.push_back(r);
    return r.combo;
}

HWND make_edit(HWND parent, int id)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

/* The frame cap's row (PC 2026-11-08, user request: "two text inputs on one
 * line: One for frame rate and the other for frame time"): the Display tab's,
 * two fields that follow each other, written as [Graphics] FrameCap, the
 * shortest frame time in ms (settings.h: 1 .. 100; 0 off - the in-game
 * menu's FRAME RATE CAP / FRAME TIME CAP rows). */
HWND make_updown(HWND parent, int id)
{
    HWND h = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, g.inst, nullptr);
    /* (the arrows' steps as the in-game menu's held key: x1, then x10 after
     * 2 s, x100 after 5 s - cap_spin takes the delta) */
    UDACCEL accel[3] = {{0, 1}, {2, 10}, {5, 100}};
    SendMessageW(h, UDM_SETACCEL, 3, (LPARAM)accel);
    SendMessageW(h, UDM_SETRANGE32, -1000000, 1000000);
    return h;
}

void add_cap_row()
{
    HWND p = g.pages[TAB_DISPLAY];
    Row r;
    r.tab = TAB_DISPLAY;
    r.button = true;                        /* (the second field as tall as the first) */
    r.cap = true;
    r.label = make_label(p, tr(L"Frame rate cap"), IDC_LABEL0 + (int)g.rows.size());
    r.combo = make_edit(p, IDC_CAP_FPS);
    r.value = make_edit(p, IDC_CAP_MS);
    r.extra = {make_label(p, L"FPS", IDC_CAP_FPS_LABEL), make_updown(p, IDC_CAP_FPS_UD),
               make_label(p, L"ms", IDC_CAP_MS_LABEL), make_updown(p, IDC_CAP_MS_UD)};
    /* (each arrow's buddy its field: the field's up / down keys step too) */
    SendMessageW(r.extra[1], UDM_SETBUDDY, (WPARAM)r.combo, 0);
    SendMessageW(r.extra[3], UDM_SETBUDDY, (WPARAM)r.value, 0);
    g.rows.push_back(r);
    g.capFps = r.combo;
    g.capMs = r.value;
}

/* The in-game menu's steps (settings_menu.cpp rate_step / time_step): the
 * rate to the next of its ring of rates (from off: the lowest, or down the
 * highest); the time by 0.01 ms, x10 and x100 held (the arrows' delta), off
 * under 1 ms, 100 ms at most, from off the refresh rate's time. */
void cap_spin(int id, int delta)
{
    static const double kRates[] = {30, 40, 45, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 170,
                                    175, 180, 200, 240, 280, 300, 360, 480, 500};
    constexpr int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));
    double ms = cap_ms();
    const int dir = delta > 0 ? 1 : -1;
    if (id == IDC_CAP_FPS_UD) {
        const double r = ms > 0.0 ? 1000.0 / ms : 0.0;
        double next = -1.0;
        if (r <= 0.0) {
            next = dir > 0 ? kRates[0] : kRates[kRateCount - 1];
        } else if (dir > 0) {
            for (int i = 0; i < kRateCount && next < 0.0; i++)
                if (kRates[i] > r * 1.002)
                    next = kRates[i];
            if (next < 0.0)
                next = 0.0;                 /* (past the highest: off, the ring) */
        } else {
            for (int i = kRateCount - 1; i >= 0 && next < 0.0; i--)
                if (kRates[i] < r * 0.998)
                    next = kRates[i];
            if (next < 0.0)
                next = 0.0;
        }
        ms = next > 0.0 ? 1000.0 / next : 0.0;
    } else {
        if (ms <= 0.0) {
            if (dir > 0) {
                /* (the in-game row starts at the output's refresh rate: the
                 * fullscreen rate chosen, else the desktop's) */
                double hz = g.disp.mode == display::Mode::fullscreen ? g.disp.refresh : 0;
                if (hz <= 1.0) {
                    DEVMODEW dm = {};
                    dm.dmSize = sizeof(dm);
                    if (g.disp.mode != display::Mode::fullscreen && EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
                        hz = dm.dmDisplayFrequency;
                }
                ms = std::floor(100000.0 / (hz > 1.0 ? hz : 60.0)) / 100.0;
            }
        } else {
            const double step = 0.01 * std::abs(delta);
            const double units = ms / step;
            const double v = (dir > 0 ? std::floor(units + 1e-6) + 1.0 : std::ceil(units - 1e-6) - 1.0) * step;
            ms = v < 1.0 - 1e-9 ? 0.0 : std::min(v, 100.0);
        }
    }
    WritePrivateProfileStringW(L"Graphics", L"FrameCap", ms > 0.0 ? fmt(L"%.3f", ms).c_str() : L"0",
                               ini_path().c_str());
    cap_show(ms);
}

/* The two fields from the cap: "Off", or the rate and the time. */
void cap_show(double ms)
{
    /* (the numbers alone - user request; the label names the units) */
    if (ms < 1.0) {
        SetWindowTextW(g.capFps, L"0");
        SetWindowTextW(g.capMs, L"0");
        SendMessageW(g.capFps, EM_SETMODIFY, FALSE, 0);
        SendMessageW(g.capMs, EM_SETMODIFY, FALSE, 0);
        return;
    }
    SetWindowTextW(g.capFps, fmt(L"%g", std::round(100000.0 / ms) / 100.0).c_str());
    SetWindowTextW(g.capMs, fmt(L"%.3f", ms).c_str());
    SendMessageW(g.capFps, EM_SETMODIFY, FALSE, 0);   /* (shown, not typed: the arrows step the setting itself) */
    SendMessageW(g.capMs, EM_SETMODIFY, FALSE, 0);
}

double cap_ms()
{
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"Graphics", L"FrameCap", L"0", v, 32, ini_path().c_str());
    return _wtof(v);
}

/* A field left (or Enter in it): its number - a rate (IDC_CAP_FPS) or a time
 * in ms - as the cap, held to 1 .. 100 ms (10 .. 1000 FPS); nothing
 * readable, 0 or "off": no cap.  Written, both fields shown again. */
void cap_changed(int id)
{
    wchar_t text[64] = L"";
    GetWindowTextW(id == IDC_CAP_FPS ? g.capFps : g.capMs, text, 64);
    const wchar_t *p = text;
    while (*p == L' ')
        p++;
    wchar_t *end = nullptr;
    double v = wcstod(p, &end);
    if (end == p || !(v > 0.0))
        v = 0.0;
    double ms = id == IDC_CAP_FPS ? (v > 0.0 ? 1000.0 / v : 0.0) : v;
    if (ms > 0.0)
        ms = std::min(100.0, std::max(1.0, ms));
    WritePrivateProfileStringW(L"Graphics", L"FrameCap", ms > 0.0 ? fmt(L"%.3f", ms).c_str() : L"0",
                               ini_path().c_str());
    cap_show(ms);
}

/* A slider: its background the parent's, drawn at each paint
 * (TBS_TRANSPARENTBKGND: the panel's WM_PRINTCLIENT) - the brush of
 * WM_CTLCOLORSTATIC was kept from where it first stood, and a slider the
 * panel scrolled showed the art of its old place. */
HWND make_slider(HWND parent, int id)
{
    return CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND, 0, 0, 10,
                           10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

/* The rows of the options of tab `tab' from the index `from' up to the
 * next option of another tab, or `count' of them. */
void add_options(int tab, size_t &next, int count = 1000)
{
    for (; next < g.options.size() && g.options[next].tab == tab && count > 0; next++, count--) {
        Option &o = g.options[next];
        if (o.color) {
            Row r;
            r.tab = tab;
            r.sub = o.sub;
            r.button = true;
            r.label = make_label(g.pages[tab], o.label, IDC_LABEL0 + (int)g.rows.size());
            r.combo = CreateWindowExW(0, WC_BUTTONW, o.label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                                      10, g.pages[tab], (HMENU)(INT_PTR)(IDC_OPTION0 + (int)next), g.inst, nullptr);
            r.value = CreateWindowExW(0, WC_BUTTONW, tr(L"Default"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                      10, 10, g.pages[tab], (HMENU)(INT_PTR)(IDC_VALUE0 + (int)next), g.inst, nullptr);
            g.rows.push_back(r);
            o.combo = r.combo;
            o.value = r.value;
            continue;
        }
        if (o.number) {
            /* PC (2026-11-14): a label and a field */
            Row r;
            r.tab = tab;
            r.sub = o.sub;
            r.number = true;
            r.option = (int)next;
            r.label = make_label(g.pages[tab], o.label, IDC_LABEL0 + (int)g.rows.size());
            r.combo = make_edit(g.pages[tab], IDC_OPTION0 + (int)next);
            SendMessageW(r.combo, EM_SETLIMITTEXT, 15, 0);
            g.rows.push_back(r);
            o.combo = r.combo;
            continue;
        }
        if (o.step <= 0) {
            o.combo = add_row(tab, o.label, IDC_OPTION0 + (int)next);
            g.rows.back().sub = o.sub;
            g.rows.back().option = (int)next;   /* PC (2026-11-09): row_shown */
            continue;
        }
        Row r;
        r.tab = tab;
        r.sub = o.sub;
        r.label = make_label(g.pages[tab], o.label, IDC_LABEL0 + (int)g.rows.size());
        r.combo = make_slider(g.pages[tab], IDC_OPTION0 + (int)next);
        r.value = make_label(g.pages[tab], L"", IDC_VALUE0 + (int)next);
        if (o.resettable)
            r.reset = CreateWindowExW(0, WC_BUTTONW, tr(L"Default"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                      10, 10, g.pages[tab], (HMENU)(INT_PTR)(IDC_DEFAULT0 + (int)next), g.inst, nullptr);
        g.rows.push_back(r);
        o.combo = r.combo;
        o.value = r.value;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g.wnd = hwnd;
        /* Creation order = tab order: the tabs' headers, the rows tab by
         * tab, then the buttons left to right. */
        for (int i = 0; i < kTabs; i++)
            g.tabs[i] = make_tab(i);
        for (int i = 0; i < kTabs; i++)
            g.pages[i] = panel::create(hwnd, g.inst, IDC_PAGE0 + i);
        build_options();
        size_t next = 0;
        /* PC (2026-11-14): the Play tab's sub-tabs, then each one's own
         * button on their line */
        for (int s = 0; s < 2; s++)
            g.playSubTab[s] = CreateWindowExW(0, WC_BUTTONW, tr(kPlaySubNames[s]),
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                              g.pages[TAB_PLAY], (HMENU)(INT_PTR)(IDC_PLAY_SUB0 + s), g.inst, nullptr);
        g.install = CreateWindowExW(0, WC_BUTTONW, tr(L"&Install game files..."),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g.pages[TAB_PLAY],
                                    (HMENU)(INT_PTR)IDC_INSTALL, g.inst, nullptr);
        g.discOpen = CreateWindowExW(0, WC_BUTTONW, tr(L"Add &disc..."), WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                     10, 10, g.pages[TAB_PLAY], (HMENU)(INT_PTR)IDC_DISC_OPEN, g.inst, nullptr);
        for (int v = 0; v < install::kVersions; v++) {
            HWND p = g.pages[TAB_PLAY];
            g.versionName[v] = make_label(p, install::version_name((install::Version)v), IDC_VERSION_NAME0 + v);
            g.versionState[v] = CreateWindowExW(0, WC_STATICW, L"",
                                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX,
                                                0, 0, 10, 10, p, (HMENU)(INT_PTR)(IDC_VERSION_STATE0 + v), g.inst, nullptr);
            g.versionPlay[v] = CreateWindowExW(0, WC_BUTTONW, tr(L"&Play"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                               0, 0, 10, 10, p, (HMENU)(INT_PTR)(IDC_VERSION_PLAY0 + v), g.inst, nullptr);
        }
        g.extractionsLabel = make_label(g.pages[TAB_PLAY], L"", IDC_EXTRACTIONS);
        /* PC (2026-11-14): the Disc sub-tab's text with no disc image (its
         * rows: rebuild_disc_rows) */
        g.discNone = CreateWindowExW(0, WC_STATICW, tr(kDiscNoneText), WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10,
                                     g.pages[TAB_PLAY], (HMENU)(INT_PTR)IDC_DISC_NONE, g.inst, nullptr);
        g.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g.inst,
                                nullptr);
        g.mode = add_row(TAB_DISPLAY, tr(L"Display mode"), IDC_MODE);
        g.res = add_row(TAB_DISPLAY, tr(L"Resolution"), IDC_RES);
        g.rate = add_row(TAB_DISPLAY, tr(L"Refresh rate"), IDC_RATE);
        add_options(TAB_DISPLAY, next, 1);              /* vertical sync */
        add_cap_row();                                  /* the frame cap's two fields */
        add_options(TAB_DISPLAY, next);                 /* the menu and movie aspects */
        g.span = add_row(TAB_DISPLAY, tr(L"Multi-Monitor"), IDC_SPAN);
        add_options(TAB_GRAPHICS, next);
        for (int s = 0; s < 2; s++)         /* the Audio tab's sub-tabs, its Songs page */
            g.audioSubTab[s] = CreateWindowExW(0, WC_BUTTONW, tr(kAudioSubNames[s]),
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                               g.pages[TAB_AUDIO], (HMENU)(INT_PTR)(IDC_AUDIO_SUB0 + s), g.inst, nullptr);
        add_options(TAB_AUDIO, next);
        g.songsText = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10,
                                      g.pages[TAB_AUDIO], (HMENU)(INT_PTR)IDC_SONGS_TEXT, g.inst, nullptr);
        g.songsOpen = CreateWindowExW(0, WC_BUTTONW, tr(L"Song &manager..."), WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                                      10, 10, g.pages[TAB_AUDIO], (HMENU)(INT_PTR)IDC_SONGS_OPEN, g.inst, nullptr);
        for (int s = 0; s < kGameSubs; s++) /* the Game tab's sub-tabs */
            g.gameSubTab[s] = CreateWindowExW(0, WC_BUTTONW, tr(kGameSubNames[s]),
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                              g.pages[TAB_GAME], (HMENU)(INT_PTR)(IDC_GAME_SUB0 + s), g.inst, nullptr);
        add_options(TAB_GAME, next, 7);                 /* language, the units, starting credits and their
                                                           custom amount, split screen */
        g.split = add_row(TAB_GAME, tr(L"Split screen 2nd monitor"), IDC_SPLIT);
        add_options(TAB_GAME, next);                    /* HUD; Cameras; Extras: the course scale; Surfaces;
                                                           Challenges */
        show_game_rows();
        controls::create(g.pages[TAB_CONTROLS], g.inst, ini_path());
        add_options(TAB_DEBUG, next);
        /* PC (2026-11-14, user request): the mod manager's button and the
         * texture manager's (PC 2026-11-13) right of the panel, the window's
         * own controls (were the Play tab's page's), what each has on under
         * its button, wrapped */
        g.modsOpen = make_button(tr(L"M&ods..."), IDC_MODS_OPEN);
        g.modsText = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 0, 10, 10,
                                     hwnd, (HMENU)(INT_PTR)IDC_MODS_TEXT, g.inst, nullptr);
        g.texturesOpen = make_button(tr(L"&Textures..."), IDC_TEXTURES_OPEN);
        g.texturesText = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 0, 10,
                                         10, hwnd, (HMENU)(INT_PTR)IDC_TEXTURES_TEXT, g.inst, nullptr);
        /* PC (2026-11-15, user request: "Save manager, under the Textures
         * manager button in the launcher") */
        g.savesOpen = make_button(tr(L"&Saves..."), IDC_SAVES_OPEN);
        g.savesText = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 0, 10, 10,
                                      hwnd, (HMENU)(INT_PTR)IDC_SAVES_TEXT, g.inst, nullptr);
        g.cancel = make_button(tr(L"&Cancel"), IDC_CANCEL);
        g.exit = make_button(tr(L"E&xit"), IDC_EXIT);
        g.playTime = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 0, 10, 10,
                                     hwnd, (HMENU)(INT_PTR)IDC_PLAY_TIME, g.inst, nullptr);
        g.status =CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, hwnd,
                                   (HMENU)(INT_PTR)IDC_STATUS, g.inst, nullptr);
        g.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS,
                                     g.inst, nullptr);
        load_settings();
        show_tab(TAB_PLAY);
        return 0;
    }

    case DM_GETDEFID:
        /* (PC 2026-11-08: the Play tab's only, or Cancel while installing;
         * elsewhere Enter presses nothing - user report: it started the game
         * from another tab) */
        if (g.installing || g.tab == TAB_PLAY)
            return MAKELRESULT(g.default_id, DC_HASDEFID);
        return 0;
    case WM_ACTIVATE:
        /* Keep the focused button across deactivation, as a dialog does. */
        if (LOWORD(wp) == WA_INACTIVE) {
            g.last_focus = GetFocus();
        } else {
            HWND f = g.last_focus;
            if (!f || !IsChild(hwnd, f) || !IsWindowVisible(f) || !IsWindowEnabled(f))
                f = default_button();
            SetFocus(f);
        }
        return 0;

    case WM_DPICHANGED:
        apply_dpi(HIWORD(wp), reinterpret_cast<const RECT *>(lp));
        return 0;
    case WM_DISPLAYCHANGE:
        /* A monitor plugged in, unplugged or moved: the monitor lists again
         * (a choice gone stays, marked). */
        fill_monitors();
        for (HWND c : {g.span, g.split})
            fit_list(c);
        return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        draw_background(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        /* Themed buttons draw the parent's background behind their corners. */
        RECT rc;
        GetClientRect(hwnd, &rc);
        draw_background((HDC)wp, rc);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the labels and the status line over the background: its brush,
         * from the child's place */
        HWND c = (HWND)lp;
        if (!is_text(c) || g.bgBrush == nullptr)
            break;
        HDC dc = (HDC)wp;
        const POINT o = child_origin(c);
        SetTextColor(dc, RGB(236, 236, 236));
        SetBkMode(dc, TRANSPARENT);
        SetBrushOrgEx(dc, -o.x, -o.y, nullptr);
        return (LRESULT)g.bgBrush;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)GetStockObject(BLACK_BRUSH);
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
        if (d->CtlID >= IDC_TAB0 && d->CtlID < (UINT)(IDC_TAB0 + kTabs)) {
            draw_tab(d);
            return TRUE;
        }
        if (d->CtlID == IDC_AUDIO_SUB0 || d->CtlID == IDC_AUDIO_SUB0 + 1) {
            const int s = (int)d->CtlID - IDC_AUDIO_SUB0;
            draw_header(d, tr(kAudioSubNames[s]), s == g.audioSub, s == 1 && g.musicUnread);
            return TRUE;
        }
        if (d->CtlID >= IDC_GAME_SUB0 && d->CtlID < IDC_GAME_SUB0 + kGameSubs) {
            const int s = (int)d->CtlID - IDC_GAME_SUB0;
            draw_header(d, tr(kGameSubNames[s]), s == g.gameSub, false);
            return TRUE;
        }
        if (d->CtlID == IDC_PLAY_SUB0 || d->CtlID == IDC_PLAY_SUB0 + 1) {   /* PC (2026-11-14) */
            const int s = (int)d->CtlID - IDC_PLAY_SUB0;
            draw_header(d, tr(kPlaySubNames[s]), s == g.playSub, false);
            return TRUE;
        }
        if (d->CtlID >= IDC_OPTION0 && d->CtlID < IDC_OPTION0 + (UINT)g.options.size() &&
            g.options[d->CtlID - IDC_OPTION0].color) {
            draw_color(d, g.options[d->CtlID - IDC_OPTION0]);
            return TRUE;
        }
        if (controls::draw_item(d))
            return TRUE;
        break;
    }
    case WM_TIMER:
        if (controls::timer(wp))
            return 0;
        break;
    case WM_NOTIFY: {
        /* the frame cap's up / down arrows (their page passes it on: panel.h);
         * the field's own text is taken first, so a typed value steps on */
        const NMHDR *h = (const NMHDR *)lp;
        if (h != nullptr && h->code == UDN_DELTAPOS && (h->idFrom == IDC_CAP_FPS_UD || h->idFrom == IDC_CAP_MS_UD)) {
            const int field = h->idFrom == IDC_CAP_FPS_UD ? IDC_CAP_FPS : IDC_CAP_MS;
            if (SendMessageW(field == IDC_CAP_FPS ? g.capFps : g.capMs, EM_GETMODIFY, 0, 0) != 0)
                cap_changed(field);
            cap_spin((int)h->idFrom, ((const NMUPDOWN *)lp)->iDelta);    /* (up: +1, x10 / x100 held) */
            return TRUE;                    /* (the control's own position stays) */
        }
        break;
    }
    case WM_HSCROLL: {
        /* a slider moved (its page passes it on: panel.h) */
        const int id = lp != 0 ? GetDlgCtrlID((HWND)lp) : 0;
        if (id >= controls::kFirstId && id <= controls::kLastId)
            controls::slider(id, LOWORD(wp));
        else if (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size())
            slider_moved(id, LOWORD(wp));
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= controls::kFirstId && id <= controls::kLastId) {
            controls::command(id, HIWORD(wp), (HWND)lp);
            return 0;
        }
        if (id >= IDC_TAB0 && id < IDC_TAB0 + kTabs) {
            if (HIWORD(wp) == BN_CLICKED)
                show_tab(id - IDC_TAB0);
            return 0;
        }
        if (id == IDC_AUDIO_SUB0 || id == IDC_AUDIO_SUB0 + 1) {
            if (HIWORD(wp) == BN_CLICKED && id - IDC_AUDIO_SUB0 != g.audioSub)
                show_audio_sub(id - IDC_AUDIO_SUB0);
            return 0;
        }
        if (id >= IDC_GAME_SUB0 && id < IDC_GAME_SUB0 + kGameSubs) {
            if (HIWORD(wp) == BN_CLICKED && id - IDC_GAME_SUB0 != g.gameSub)
                show_game_sub(id - IDC_GAME_SUB0);
            return 0;
        }
        /* PC (2026-11-14): the Play tab's sub-tabs, the Disc sub-tab's buttons */
        if (id == IDC_PLAY_SUB0 || id == IDC_PLAY_SUB0 + 1) {
            if (HIWORD(wp) == BN_CLICKED && id - IDC_PLAY_SUB0 != g.playSub)
                show_play_sub(id - IDC_PLAY_SUB0);
            return 0;
        }
        if (id == IDC_DISC_OPEN) {
            if (HIWORD(wp) == BN_CLICKED)
                choose_and_add_disc();
            return 0;
        }
        if (id >= IDC_DISC_PLAY0 && id < IDC_DISC_PLAY0 + (int)g.discs.size()) {
            if (HIWORD(wp) == BN_CLICKED && !g.installing)
                play_disc(std::wstring(g.discs[id - IDC_DISC_PLAY0].path));
            return 0;
        }
        if (id >= IDC_DISC_REMOVE0 && id < IDC_DISC_REMOVE0 + (int)g.discs.size()) {
            if (HIWORD(wp) == BN_CLICKED)   /* (after this click: the row's controls are destroyed) */
                PostMessageW(hwnd, WM_APP_DISC_REMOVE, (WPARAM)(id - IDC_DISC_REMOVE0), 0);
            return 0;
        }
        if (id == IDC_SONGS_OPEN) {
            if (HIWORD(wp) == BN_CLICKED)
                open_song_manager();
            return 0;
        }
        if (id == IDC_MODS_OPEN) {
            if (HIWORD(wp) == BN_CLICKED)
                open_mod_manager();
            return 0;
        }
        if (id == IDC_TEXTURES_OPEN) {      /* PC (2026-11-13) */
            if (HIWORD(wp) == BN_CLICKED)
                open_texture_manager();
            return 0;
        }
        if (id == IDC_SAVES_OPEN) {         /* PC (2026-11-15) */
            if (HIWORD(wp) == BN_CLICKED)
                open_save_manager();
            return 0;
        }
        if (id >= IDC_VALUE0 && id < IDC_VALUE0 + (int)g.options.size() && g.options[id - IDC_VALUE0].color) {
            if (HIWORD(wp) == BN_CLICKED)
                set_color(g.options[id - IDC_VALUE0], g.options[id - IDC_VALUE0].gameRgb);
            return 0;
        }
        if (id >= IDC_DEFAULT0 && id < IDC_DEFAULT0 + (int)g.options.size()) {
            if (HIWORD(wp) == BN_CLICKED)
                slider_default(g.options[id - IDC_DEFAULT0]);
            return 0;
        }
        if (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size() && g.options[id - IDC_OPTION0].color) {
            if (HIWORD(wp) == BN_CLICKED)
                pick_color(g.options[id - IDC_OPTION0]);
            return 0;
        }
        if (id == IDC_CAP_FPS || id == IDC_CAP_MS) {
            if (HIWORD(wp) == EN_KILLFOCUS)
                cap_changed(id);
            return 0;
        }
        /* PC (2026-11-14): a number's field left with something typed */
        if (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size() && g.options[id - IDC_OPTION0].number) {
            Option &o = g.options[id - IDC_OPTION0];
            if (HIWORD(wp) == EN_KILLFOCUS && SendMessageW(o.combo, EM_GETMODIFY, 0, 0) != 0)
                number_changed(o);
            return 0;
        }
        if ((id >= IDC_MODE && id <= IDC_SPLIT) || (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size())) {
            if (HIWORD(wp) == CBN_SELCHANGE)
                setting_changed(id);
            return 0;
        }
        if (id >= IDC_VERSION_PLAY0 && id < IDC_VERSION_PLAY0 + install::kVersions) {
            if (HIWORD(wp) == BN_CLICKED && !g.installing)
                play(id - IDC_VERSION_PLAY0);
            return 0;
        }
        switch (id) {
        case IDOK:   /* Enter (IsDialogMessage): the default version, else the install */
            if (GetFocus() == g.capFps || GetFocus() == g.capMs) {
                cap_changed(GetDlgCtrlID(GetFocus()));     /* (Enter in a frame cap field: the field) */
                return 0;
            }
            for (Option &o : g.options)
                if (o.number && o.combo != nullptr && GetFocus() == o.combo) {
                    number_changed(o);                      /* PC (2026-11-14): Enter in a number's field */
                    return 0;
                }
            if (g.installing || g.tab != TAB_PLAY)      /* (PC 2026-11-08: the Play tab's Enter only) */
                return 0;
            if (g.playSub == PLAY_DISC) {               /* PC (2026-11-14): the Disc sub-tab's */
                if (default_disc() >= 0)
                    play_disc(std::wstring(g.discs[default_disc()].path));
                else
                    choose_and_add_disc();
                return 0;
            }
            if (default_version() >= 0)
                play(default_version());
            else
                choose_and_install();
            return 0;
        case IDCANCEL:   /* Esc */
            if (g.installing)
                cancel_clicked();
            else
                DestroyWindow(hwnd);
            return 0;
        case IDC_INSTALL:
            choose_and_install();
            return 0;
        case IDC_CANCEL:
            cancel_clicked();
            return 0;
        case IDC_EXIT:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }

    case WM_APP_PHASE: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lp));
        if (!g.cancel_requested) {
            g.phase = *text;
            g.copying = false;
            show_progress_text();
        }
        return 0;
    }
    case WM_APP_PROGRESS:
        if (!g.cancel_requested) {
            g.copying = true;
            show_progress_text();
        }
        return 0;
    case WM_APP_DONE:
        install_done();
        return 0;
    case WM_APP_DISC_REMOVE:                /* PC (2026-11-14) */
        remove_disc((size_t)wp);
        return 0;

    case WM_CLOSE:
        if (g.installing) {
            if (!g.cancel_requested &&
                !ask(g.converting ? tr(L"The music is being converted.\n\nStop the conversion and exit? (It carries on "
                                       L"the next time the launcher starts.)")
                                  : tr(L"The game files are being installed.\n\nStop the installation and exit?")))
                return 0;
            if (g.installing) {
                /* Exit once the worker has removed its temporary folder. */
                if (!g.cancel_requested)
                    request_cancel();
                g.close_when_done = true;
                return 0;
            }
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        controls::show(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int run(int show_command, const std::wstring &shared)
{
    g.inst = GetModuleHandleW(nullptr);
    g.folder = util::exe_folder();
    /* PC (2026-11-14): an engine started by another launcher's selector reads
     * that launcher's gt3pc.ini; its own file is made when missing */
    g.shared = shared.empty() ? g.folder : util::with_slash(shared);
    engines::ensure_engine_ini(g.folder);
    ensure_save_folders();                  /* PC (2026-11-15): default1, default2 when none */

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    /* The Open dialog is a shell component and wants COM on this thread (WIC
     * too: the background). */
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g.inst;
    wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, tr(kTitle), kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                                nullptr, g.inst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, tr(L"The launcher window could not be created."), kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    apply_dpi(GetDpiForWindow(hwnd), nullptr);
    /* PC (2026-11-14): an engine's window says whose it is */
    if (!shared_arg().empty())
        SetWindowTextW(hwnd, (std::wstring(tr(kTitle)) + L" - " + engines::name_of(g.folder)).c_str());
    /* an earlier launcher's data\ into its version's folder; the last
     * played version (Enter's) */
    const std::wstring moved = install::migrate_earlier_install(g.folder);
    wchar_t last[16] = L"";
    GetPrivateProfileStringW(L"Game", L"Version", L"US", last, 16, engine_ini_path().c_str());
    for (int v = 0; v < install::kVersions; v++)
        if (_wcsicmp(last, util::widen(install::version_code((install::Version)v)).c_str()) == 0)
            g.lastVersion = v;
    if (!moved.empty())
        for (Option &o : g.options)
            if (o.step <= 0 && wcscmp(o.key, L"Language") == 0 && wcscmp(o.section, L"Game") == 0) {
                o.choices = language_choices();
                fill_option(o);
            }
    /* PC (2026-11-14): the disc images played before; the Disc sub-tab shown
     * when the last Play was a disc image's ([Game] Disc).  PC (2026-11-15):
     * one listed before its folder was kept in the list: readied now (its
     * folder made or written again, as its Play does), for the song manager */
    {
        const std::vector<disc_list::Disc> discs = disc_list::load(engine_ini_path());
        for (size_t i = 0; i < discs.size(); i++) {
            if (!discs[i].folder.empty() || !disc_there(discs[i]))
                continue;
            install::DiscFolder disc;
            std::wstring msg;
            if (install::prepare_disc(discs[i].path, g.folder, &disc, &msg) == install::Result::ok)
                disc_list::set_folder(engine_ini_path(), i, disc.name);
        }
    }
    rebuild_disc_rows();
    {
        wchar_t lastDisc[128] = L"";
        GetPrivateProfileStringW(L"Game", L"Disc", L"", lastDisc, 128, engine_ini_path().c_str());
        if (lastDisc[0] != 0 && !g.discs.empty())
            show_play_sub(PLAY_DISC);
    }
    refresh();
    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);
    /* (a message box: the status line is too short for it) */
    if (!moved.empty())
        MessageBoxW(hwnd, moved.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
    /* the installed versions' music, once (music.h); then (or else now) the
     * songs added to their music folders (their notices: the Audio tab) */
    repair_all_markers();
    convert_all_tables();
    if (!start_conversion())
        add_all_new_songs();
    SetFocus(default_button());

    MSG m;
    HWND focus = nullptr;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        /* the song manager's window: its own keyboard navigation */
        if (song_manager::filter(m))
            continue;
        if (mod_manager::filter(m))
            continue;
        if (texture_manager::filter(m))     /* PC (2026-11-13) */
            continue;
        if (save_manager::filter(m))        /* PC (2026-11-15) */
            continue;
        /* the Controls tab's capture first (a key or click taken as a
         * binding goes nowhere else) */
        if (controls::filter(m))
            continue;
        /* the mouse wheel over a page that scrolls: the page's */
        if (panel::wheel(m))
            continue;
        /* Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PageDown / PageUp: the next /
         * previous tab */
        if (m.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 &&
            (m.wParam == VK_TAB || m.wParam == VK_NEXT || m.wParam == VK_PRIOR)) {
            const bool back = m.wParam == VK_PRIOR || (m.wParam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
            show_tab((g.tab + (back ? kTabs - 1 : 1)) % kTabs);
            continue;
        }
        /* Tab between the controls, Enter and Esc. */
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        /* the focus moved to a control out of its page's view: scrolled to */
        HWND f = GetFocus();
        if (f != focus) {
            focus = f;
            if (f != nullptr)
                panel::show(f);
        }
    }

    if (g.worker.joinable()) {
        g.cancel_requested = true;
        g.worker.join();
    }
    if (g.font)
        DeleteObject(g.font);
    if (g.tabFont)
        DeleteObject(g.tabFont);
    if (g.bgBrush)
        DeleteObject(g.bgBrush);
    if (g.bgDC) {
        SelectObject(g.bgDC, g.bgOld);
        DeleteDC(g.bgDC);
    }
    if (g.bg)
        DeleteObject(g.bg);
    if (g.icon_big)
        DestroyIcon(g.icon_big);
    if (g.icon_small)
        DestroyIcon(g.icon_small);
    if (SUCCEEDED(com))
        CoUninitialize();
    return (int)m.wParam;
}

} // namespace ui
