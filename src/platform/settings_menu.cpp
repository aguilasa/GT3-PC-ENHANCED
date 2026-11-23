/* settings_menu.cpp - the in-game settings menu of GT3 PC (settings_menu.h). */
#include "platform/settings_menu.h"
#include "platform/audio.h"
#include "platform/clutch_input.h"             /* PC (2026-11-09): the clutch mode's notice */
#include "platform/cockpit_osd.h"              /* PC (2026-11-08): the cockpit camera's calibration */
#include "platform/frame_monitor.h"
#include "platform/gamepad.h"
#include "platform/gpu.h"
#include "platform/hud_colors.h"
#include "platform/input_map.h"
#include "platform/language.h"
#include "platform/memcard.h"                  /* PC (2026-11-15): MEMORY CARDS */
#include "platform/overlay.h"
#include "platform/screenshot.h"               /* PC (2026-11-15): TAKE SCREENSHOT */
#include "platform/settings.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages */
#include "platform/window.h"

#include "platform/os/win_compat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace settings_menu {
namespace {

using overlay::rgba;

std::atomic<bool> g_shown{false};
std::mutex g_lock;                              /* the selection and the changes (window / game threads) */
int g_sel;

/* ---- the settings ---------------------------------------------------- */

/* A setting: a list of values (count, get, set, value), or a value of its
 * own (count 0: text, step, edges); or a category (count kCategory), the
 * items after it one level deeper its own.  A list setting may have items
 * one level deeper too: shown while its value is not 0 (on). */
constexpr int kCategory = -1;
struct Item {
    int depth;                                  /* 0 the top level */
    const char *label;
    const char *help;                           /* under the list while it is chosen */
    int count;                                  /* its values */
    int (*get)();
    void (*set)(int);
    const char *(*value)(int);
    void (*text)(char *out, size_t n);          /* the value shown */
    /* Left / Right (-1 / +1; heldMs: how long the key has been held, 0 a
     * fresh press), or 0 the next value (Enter, Cross) */
    void (*step)(int dir, int heldMs);
    int (*edges)();                             /* bit 0: a lower value exists, bit 1: a higher one */
    int hotkey = -1;                            /* the label followed by "(key)" (input_map::Hotkey) */
    uint32_t (*swatch)() = nullptr;             /* a square of this colour left of the value */
    /* PC (2026-11-09): a list setting's items one level deeper shown while
     * this is not 0 (nullptr: while its value is not 0) */
    int (*nest)() = nullptr;
};

int aa_get() { return (int)settings::anti_aliasing(); }
void aa_set(int v) { settings::set_anti_aliasing((settings::AntiAliasing)v); }
const char *aa_value(int v)
{
    static const char *const names[settings::kAntiAliasingCount] = {N_("OFF"), "FXAA",    "MSAA 2X", "MSAA 4X",
                                                                    "MSAA 8X", "AGSS 2X", "AGSS 4X", "AGSS 8X"};
    return v >= 0 && v < settings::kAntiAliasingCount ? names[v] : "?";
}

struct Ratio {
    int w, h;
    const char *name;
};
const Ratio kHudRatios[] = {{4, 3, "4:3"}, {16, 10, "16:10"}, {16, 9, "16:9"}, {21, 9, "21:9"}, {32, 9, "32:9"}};
constexpr int kHudRatioCount = (int)(sizeof(kHudRatios) / sizeof(kHudRatios[0]));

int hud_get()
{
    const float a = settings::display().hudMaxAspect;
    int best = 0;
    for (int i = 1; i < kHudRatioCount; i++) {
        const float d = std::fabs(std::log(a * kHudRatios[i].h / kHudRatios[i].w));
        const float db = std::fabs(std::log(a * kHudRatios[best].h / kHudRatios[best].w));
        if (d < db)
            best = i;
    }
    return best;
}
void hud_set(int v) { settings::set_hud_max_aspect(kHudRatios[v].w, kHudRatios[v].h); }
const char *hud_value(int v) { return v >= 0 && v < kHudRatioCount ? kHudRatios[v].name : "?"; }

int lod_get() { return settings::lod_enabled() ? 1 : 0; }
void lod_set(int v) { settings::set_lod_enabled(v != 0); }
const char *lod_value(int v) { return v ? N_("NORMAL") : N_("ALWAYS BEST"); }

/* PC: the road reflection's detail (settings::ReflectionDetail) */
int refl_get() { return settings::reflection_detail() == settings::ReflectionDetail::full ? 1 : 0; }
void refl_set(int v)
{
    settings::set_reflection_detail(v != 0 ? settings::ReflectionDetail::full : settings::ReflectionDetail::original);
}
const char *refl_value(int v) { return v ? N_("HIGH") : N_("NORMAL"); }
/* PC (2026-11-14): the road reflection's resolution (settings::ReflectionResolution) */
int refl_res_get() { return (int)settings::reflection_resolution(); }
void refl_res_set(int v) { settings::set_reflection_resolution((settings::ReflectionResolution)v); }
const char *refl_res_value(int v)
{
    static const char *const names[settings::kReflectionResolutionCount] = {N_("NORMAL"), N_("HALF"), N_("FULL")};
    return v >= 0 && v < settings::kReflectionResolutionCount ? names[v] : "?";
}

/* PC (2026-11-05): the cars' reflections (settings::CarReflections) */
int car_refl_get() { return (int)settings::car_reflections(); }
void car_refl_set(int v) { settings::set_car_reflections((settings::CarReflections)v); }
const char *car_refl_value(int v) { return v == 2 ? N_("OFF") : v == 1 ? N_("LOW") : N_("NORMAL"); }

/* PC (2026-11-06): the cars' environment map's size (settings::EnvMapDetail) */
int env_map_get() { return (int)settings::env_map_detail(); }
void env_map_set(int v) { settings::set_env_map_detail((settings::EnvMapDetail)v); }
const char *env_map_value(int v) { return v == 1 ? N_("NORMAL") : N_("HIGH"); }

/* PC (2026-11-10): the anisotropic filtering (settings::anisotropic_filtering):
 * off, 2x .. 16x as 0 .. 4 */
int aniso_get()
{
    const int a = settings::anisotropic_filtering();
    return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0;
}
void aniso_set(int v) { settings::set_anisotropic_filtering(v > 0 ? 1 << v : 1); }
const char *aniso_value(int v)
{
    static const char *const names[settings::kAnisotropicCount] = {N_("OFF"), "2X", "4X", "8X", "16X"};
    return v >= 0 && v < settings::kAnisotropicCount ? names[v] : N_("OFF");
}

/* PC (2026-11-07): the replays' depth of field (settings::ReplayDepthOfField) */
int replay_dof_get() { return (int)settings::replay_depth_of_field(); }
void replay_dof_set(int v) { settings::set_replay_depth_of_field((settings::ReplayDepthOfField)v); }
const char *replay_dof_value(int v) { return v == 2 ? N_("OFF") : v == 1 ? N_("REDUCED") : N_("NORMAL"); }

/* The rear view by camera (settings::RearViewCamera) */
template <int C> int rear_get() { return settings::rear_view((settings::RearViewCamera)C) ? 1 : 0; }
template <int C> void rear_set(int v) { settings::set_rear_view((settings::RearViewCamera)C, v != 0); }
const char *on_off_value(int v) { return v ? N_("ON") : N_("OFF"); }

int extra_cameras_get() { return settings::extra_cameras() ? 1 : 0; }
void extra_cameras_set(int v) { settings::set_extra_cameras(v != 0); }

/* The chase camera's angle (settings::chase_angle): Left / Right by 1 degree
 * (5 once held 1.5 s); Enter / Cross back to 0. */
void chase_angle_text(char *out, size_t n) { snprintf(out, n, "%d", settings::chase_angle()); }
void chase_angle_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_chase_angle(0);
        return;
    }
    settings::set_chase_angle(settings::chase_angle() + (dir > 0 ? 1 : -1) * (heldMs < 1500 ? 1 : 5));
}
int chase_angle_edges()
{
    const int a = settings::chase_angle();
    return (a > settings::kChaseAngleMin ? 1 : 0) | (a < settings::kChaseAngleMax ? 2 : 0);
}

/* PC (2026-11-07): CAMERAS > COCKPIT CAMERA (settings::cockpit_camera) and
 * its field of view (settings::cockpit_fov): Left / Right by 1 degree (5
 * once held 1.5 s), Enter / Cross back to the default. */
int cockpit_get() { return settings::cockpit_camera() ? 1 : 0; }
void cockpit_set(int v) { settings::set_cockpit_camera(v != 0); }
/* (PC 2026-11-15: COCKPIT MENU MODEL moved into the calibration - per car
 * model, settings::cockpit_menu_model) */
void cockpit_fov_text(char *out, size_t n) { snprintf(out, n, "%d", settings::cockpit_fov()); }
void cockpit_fov_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_cockpit_fov(settings::kCockpitFovDefault);
        return;
    }
    settings::set_cockpit_fov(settings::cockpit_fov() + (dir > 0 ? 1 : -1) * (heldMs < 1500 ? 1 : 5));
}
int cockpit_fov_edges()
{
    const int f = settings::cockpit_fov();
    return (f > settings::kCockpitFovMin ? 1 : 0) | (f < settings::kCockpitFovMax ? 2 : 0);
}

/* PC (2026-11-12): GRAPHICS > MENU WHEELS (settings::menu_wheels) and the
 * speed from which the race wheels take over (settings::menu_wheels_speed):
 * Left / Right by 1 km/h (10 once held 1.5 s), Enter / Cross back to the
 * default; 0 shows ALWAYS. */
int menu_wheels_get() { return settings::menu_wheels() ? 1 : 0; }
void menu_wheels_set(int v) { settings::set_menu_wheels(v != 0); }
void menu_wheels_speed_text(char *out, size_t n)
{
    const int kmh = settings::menu_wheels_speed();
    if (kmh <= 0)
        snprintf(out, n, "%s", tr("ALWAYS"));
    else
        snprintf(out, n, "%d KM/H", kmh);
}
void menu_wheels_speed_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_menu_wheels_speed(settings::kMenuWheelsSpeedDefault);
        return;
    }
    settings::set_menu_wheels_speed(settings::menu_wheels_speed() + (dir > 0 ? 1 : -1) * (heldMs < 1500 ? 1 : 10));
}
int menu_wheels_speed_edges()
{
    const int k = settings::menu_wheels_speed();
    return (k > settings::kMenuWheelsSpeedMin ? 1 : 0) | (k < settings::kMenuWheelsSpeedMax ? 2 : 0);
}

/* PC (2026-11-13): GRAPHICS > MENU TIRES (settings::menu_tires) */
int menu_tires_get() { return settings::menu_tires() ? 1 : 0; }
void menu_tires_set(int v) { settings::set_menu_tires(v != 0); }

/* PC (2026-11-15): TAKE SCREENSHOT (platform/screenshot.h): Enter / Cross
 * asks for one - the game's picture, which this menu is not in; the menu
 * stays.  Left / Right nothing.  SAVING... while one is taken or written. */
void shot_text(char *out, size_t n)
{
    snprintf(out, n, "%s", screenshot::pending() > 0 ? tr("SAVING...") : tr("ENTER"));
}
void shot_step(int dir, int heldMs)
{
    if (dir == 0 && heldMs == 0)
        screenshot::request();
}

/* PC (2026-11-16): INTERFACE LANGUAGE (settings::interface_language,
 * platform/ui_text.h): AUTO, then the languages, each in its own words (AUTO
 * with the one it stands for); Left / Right by one, Enter / Cross the next
 * (wrapping).  The menu's text changes at once. */
void ui_lang_text(char *out, size_t n)
{
    const int v = settings::interface_language();
    if (v == ui_text::kAuto)
        snprintf(out, n, "%s (%s)", tr("AUTO"), ui_text::native_upper(ui_text::language()));
    else
        snprintf(out, n, "%s", ui_text::native_upper(v));
}
void ui_lang_step(int dir, int heldMs)
{
    const int cur = settings::interface_language() + 1;     /* 0 AUTO, 1 .. kCount the languages */
    int v = dir == 0 ? (heldMs == 0 ? (cur + 1) % (ui_text::kCount + 1) : cur) : cur + dir;
    v = std::max(0, std::min((int)ui_text::kCount, v));
    if (v != cur)
        settings::set_interface_language(v - 1);
}
int ui_lang_edges()
{
    const int cur = settings::interface_language() + 1;
    return (cur > 0 ? 1 : 0) | (cur < ui_text::kCount ? 2 : 0);
}

/* PC (2026-11-08): CAMERAS > COCKPIT CAMERA > CALIBRATE COCKPIT CAMERA
 * (platform/cockpit_osd.h): Enter / Cross starts the calibration mode and
 * closes the menu (the panel at the top right takes over), or stops it;
 * Left / Right nothing. */
void calibrate_text(char *out, size_t n)
{
    /* (PC 2026-11-14: the mode is the calibration's, not the custom cameras' editor's) */
    const bool on = cockpit_osd::active() && cockpit_osd::mode() == cockpit_osd::kModeCockpit;
    snprintf(out, n, "%s", on ? tr("STOP") : tr("START"));
}
void calibrate_step(int dir, int heldMs)
{
    if (dir != 0 || heldMs != 0)
        return;
    if (cockpit_osd::toggle())
        g_shown = false;
}

/* PC (2026-11-14): CAMERAS > CUSTOM CAMERAS (platform/cockpit_osd.h, game/app/
 * pc_customcam.h).  NEW CAMERA: EVERY CAR / THIS CAR and EDIT CAMERA: Enter /
 * Cross starts the editor with that request and closes the menu (the panel at
 * the top right takes over), or stops it; DELETE CAMERA: Enter twice within
 * four seconds asks the race to delete the custom camera in view and closes
 * the menu.  Left / Right nothing. */
void custom_text(char *out, size_t n)
{
    const bool on = cockpit_osd::active() && cockpit_osd::mode() == cockpit_osd::kModeCustom;
    snprintf(out, n, "%s", on ? tr("STOP") : tr("START"));
}
template <int R> void custom_step(int dir, int heldMs)
{
    if (dir != 0 || heldMs != 0)
        return;
    if (cockpit_osd::toggle_custom(R))
        g_shown = false;
}
ULONGLONG g_customDeleteAt;                     /* the first Enter on DELETE CAMERA (0: none) */
constexpr ULONGLONG kCustomDeleteMs = 4000;
void custom_delete_text(char *out, size_t n)
{
    const bool armed = g_customDeleteAt != 0 && GetTickCount64() - g_customDeleteAt < kCustomDeleteMs;
    snprintf(out, n, "%s", armed ? tr("ENTER AGAIN") : tr("ENTER"));
}
void custom_delete_step(int dir, int heldMs)
{
    if (dir != 0 || heldMs != 0)
        return;
    const ULONGLONG now = GetTickCount64();
    if (g_customDeleteAt != 0 && now - g_customDeleteAt < kCustomDeleteMs) {
        g_customDeleteAt = 0;
        cockpit_osd::request(cockpit_osd::kRequestDelete);
        g_shown = false;
    } else {
        g_customDeleteAt = now;
    }
}

int smoke_get() { return (int)settings::smoke_opacity(); }
void smoke_set(int v) { settings::set_smoke_opacity((settings::SmokeOpacity)v); }
const char *smoke_value(int v) { return v == 2 ? N_("HALF") : v == 1 ? N_("REDUCED (75%)") : N_("NORMAL"); }

int split_get() { return (int)settings::split_screen(); }
void split_set(int v) { settings::set_split_screen((settings::SplitScreen)v); }
const char *split_value(int v) { return v == 2 ? N_("TWO MONITORS") : v == 1 ? N_("VERTICAL") : N_("HORIZONTAL"); }

int split_hud_get() { return settings::split_hud_analog() ? 1 : 0; }
void split_hud_set(int v) { settings::set_split_hud_analog(v != 0); }
int single_hud_get() { return settings::single_hud_digital() ? 1 : 0; }
void single_hud_set(int v) { settings::set_single_hud_digital(v != 0); }
const char *hud_digital_value(int v) { return v ? N_("ANALOG") : N_("DIGITAL"); }
const char *hud_analog_value(int v) { return v ? N_("DIGITAL") : N_("ANALOG"); }

const int kDistances[] = {100, 125, 150, 200, 300, 400};
const char *const kDistanceNames[] = {"100%", "125%", "150%", "200%", "300%", "400%"};
constexpr int kDistanceCount = (int)(sizeof(kDistances) / sizeof(kDistances[0]));

int distance_get()
{
    const int d = settings::render_distance();
    int best = 0;
    for (int i = 1; i < kDistanceCount; i++)
        if (std::abs(kDistances[i] - d) < std::abs(kDistances[best] - d))
            best = i;
    return best;
}
void distance_set(int v) { settings::set_render_distance(kDistances[v]); }
const char *distance_value(int v) { return v >= 0 && v < kDistanceCount ? kDistanceNames[v] : "?"; }

/* The fields of view (settings::fov_percent, replay_fov_percent): Left /
 * Right by 5 % (10 once held 1.5 s); Enter / Cross back to 100. */
void fov_step_of(int cur, void (*set)(int), int dir, int heldMs)
{
    if (dir == 0) {
        set(100);
        return;
    }
    const int step = heldMs < 1500 ? 5 : 10;
    /* to the step's grid first (a value set in the ini between them) */
    set(dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}

int fov_edges_of(int cur)
{
    return (cur > settings::kFovMin ? 1 : 0) | (cur < settings::kFovMax ? 2 : 0);
}

void fov_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::fov_percent()); }
void fov_step(int dir, int heldMs) { fov_step_of(settings::fov_percent(), settings::set_fov_percent, dir, heldMs); }
int fov_edges() { return fov_edges_of(settings::fov_percent()); }

/* The resolution scale (settings::render_scale): as the fields of view (5 %
 * a step, 10 once held; Enter / Cross: 100), shown with the window's size
 * at it. */
void scale_text(char *out, size_t n)
{
    const int s = settings::render_scale();
    int w = 0, h = 0;
    gpu::backbuffer_size(&w, &h);
    if (w > 0 && h > 0)
        snprintf(out, n, "%d%% (%dX%d)", s, (w * s + 50) / 100, (h * s + 50) / 100);
    else
        snprintf(out, n, "%d%%", s);
}
void scale_step(int dir, int heldMs) { fov_step_of(settings::render_scale(), settings::set_render_scale, dir, heldMs); }
int scale_edges()
{
    const int s = settings::render_scale();
    return (s > settings::kRenderScaleMin ? 1 : 0) | (s < settings::kRenderScaleMax ? 2 : 0);
}

/* The HUD safe zone (settings::hud_safe_zone): Left / Right by 1 % (5 once
 * held 1.5 s); Enter / Cross back to 100. */
void safe_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::hud_safe_zone()); }
void safe_step(int dir, int heldMs)
{
    const int cur = settings::hud_safe_zone();
    if (dir == 0) {
        settings::set_hud_safe_zone(100);
        return;
    }
    const int step = heldMs < 1500 ? 1 : 5;
    settings::set_hud_safe_zone(dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}
int safe_edges()
{
    const int s = settings::hud_safe_zone();
    return (s > settings::kHudSafeZoneMin ? 1 : 0) | (s < settings::kHudSafeZoneMax ? 2 : 0);
}

void replay_fov_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::replay_fov_percent()); }
void replay_fov_step(int dir, int heldMs)
{
    fov_step_of(settings::replay_fov_percent(), settings::set_replay_fov_percent, dir, heldMs);
}
int replay_fov_edges() { return fov_edges_of(settings::replay_fov_percent()); }

/* The HUD's colours (settings::hud_color): a category for each, its value and
 * a square of it, with its red, green and blue (S: the channel's shift) in
 * it: Left / Right by 1 (8 once held 1.5 s); Enter / Cross back to the
 * game's. */
template <int I, int S> int hud_ch() { return (int)((settings::hud_color(I) >> S) & 0xFF); }
template <int I, int S> void hud_ch_set(int v)
{
    settings::set_hud_color(I, (settings::hud_color(I) & ~(0xFFu << S)) | ((uint32_t)v << S));
}
template <int I, int S> void hud_ch_text(char *out, size_t n) { snprintf(out, n, "%d", hud_ch<I, S>()); }
template <int I, int S> void hud_ch_step(int dir, int heldMs)
{
    if (dir == 0) {
        hud_ch_set<I, S>((int)((kHudColors[I].rgb >> S) & 0xFF));
        return;
    }
    hud_ch_set<I, S>(std::max(0, std::min(255, hud_ch<I, S>() + dir * (heldMs < 1500 ? 1 : 8))));
}
template <int I, int S> int hud_ch_edges()
{
    const int v = hud_ch<I, S>();
    return (v > 0 ? 1 : 0) | (v < 255 ? 2 : 0);
}
template <int I> void hud_color_text(char *out, size_t n) { snprintf(out, n, "%06X", settings::hud_color(I)); }
template <int I> uint32_t hud_color_swatch()
{
    const uint32_t c = settings::hud_color(I);
    return overlay::rgba((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
}

constexpr char kHudRedHelp[] = N_("ITS RED, 0 .. 255. ENTER: THE GAME'S.");
constexpr char kHudGreenHelp[] = N_("ITS GREEN, 0 .. 255. ENTER: THE GAME'S.");
constexpr char kHudBlueHelp[] = N_("ITS BLUE, 0 .. 255. ENTER: THE GAME'S.");
#define HUD_CHANNEL(I, S, LABEL, HELP) \
    {4, LABEL, HELP, 0, nullptr, nullptr, nullptr, hud_ch_text<I, S>, hud_ch_step<I, S>, hud_ch_edges<I, S>}
#define HUD_COLOR(I, LABEL, HELP) \
    {3, LABEL, HELP, kCategory, nullptr, nullptr, nullptr, hud_color_text<I>, nullptr, nullptr, -1, hud_color_swatch<I>}, \
    HUD_CHANNEL(I, 16, N_("RED"), kHudRedHelp), HUD_CHANNEL(I, 8, N_("GREEN"), kHudGreenHelp), \
    HUD_CHANNEL(I, 0, N_("BLUE"), kHudBlueHelp)

int monitor_get() { return (int)frame_monitor::mode(); }
void monitor_set(int v) { frame_monitor::set_mode((frame_monitor::Mode)v); }
const char *monitor_value(int v)
{
    static const char *const names[frame_monitor::kModes] = {N_("OFF"), N_("SIMPLE"), N_("DETAILED"), N_("VERBOSE")};
    return v >= 0 && v < frame_monitor::kModes ? names[v] : "?";
}

/* The screen mode: the window's (Alt+Enter's too); set, the settings' as
 * well (the game starts in it) */
int screen_get() { return (int)window::mode(); }
void screen_set(int v)
{
    settings::set_display_mode((settings::DisplayMode)v);
    window::request_mode((settings::DisplayMode)v);
}
const char *screen_value(int v)
{
    static const char *const names[3] = {N_("WINDOWED"), N_("BORDERLESS"), N_("FULLSCREEN")};
    return v >= 0 && v < 3 ? names[v] : "?";
}

/* (PC 2026-11-15: 16:9 between the two, settings::MenuAspect) */
int menu_aspect_get() { return (int)settings::menu_aspect(); }
void menu_aspect_set(int v) { settings::set_menu_aspect((settings::MenuAspect)v); }
const char *menu_aspect_value(int v)
{
    static const char *const names[settings::kMenuAspectCount] = {N_("ORIGINAL (4:3)"), "16:9", N_("STRETCH")};
    return v >= 0 && v < settings::kMenuAspectCount ? names[v] : "?";
}

int movie_aspect_get() { return settings::movie_stretch() ? 1 : 0; }
void movie_aspect_set(int v) { settings::set_movie_stretch(v != 0); }
const char *movie_aspect_value(int v) { return v ? N_("STRETCH") : N_("ORIGINAL"); }

int vsync_get() { return settings::vsync(); }
void vsync_set(int v) { settings::set_vsync(v); }
const char *vsync_value(int v)
{
    static const char *const names[4] = {N_("OFF"), N_("1 FRAME"), N_("2 FRAMES"), N_("3 FRAMES")};
    return v >= 0 && v < 4 ? names[v] : "?";
}

/* The frame cap (settings::frame_cap_ms), as a frame rate: Left / Right to
 * the next of the usual rates below / above the cap's, OFF (no cap) at both
 * ends - a ring: right of the highest and left of the lowest, a held key
 * stopping there; as a frame time: Left / Right by 0.01 ms, 0.1 once held
 * 1.5 s, 1 once held 4 s (below the shortest, OFF; from OFF, the display's
 * refresh period).  A rate is the cap's within 0.2 %: the ms are kept to
 * 0.001, and 240 a second (4.167 ms) reads back as 239.98, which a tighter
 * test took for "below 240" - Right picked 240 again and stuck there, OFF out
 * of reach (user report). */
const double kRates[] = {30, 40, 45, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 170,
                         175, 180, 200, 240, 280, 300, 360, 480, 500};
constexpr int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));
constexpr double kRateTolerance = 0.002;

double cap_rate()
{
    const double ms = settings::frame_cap_ms();
    return ms > 0.0 ? 1000.0 / ms : 0.0;
}

void rate_text(char *out, size_t n)
{
    const double r = cap_rate();
    if (r <= 0.0)
        snprintf(out, n, "%s", tr("OFF"));
    else if (std::fabs(r - std::floor(r + 0.5)) < 0.005)
        snprintf(out, n, "%.0f FPS", r);
    else
        snprintf(out, n, "%.2f FPS", r);
}

void rate_step(int dir, int heldMs)
{
    const double r = cap_rate();
    if (dir == 0)
        dir = 1;                                /* (the next value: the ring onwards) */
    if (r <= 0.0) {
        /* from OFF: the lowest / the highest (a held key stays at OFF) */
        if (heldMs == 0)
            settings::set_frame_cap_ms(1000.0 / (dir > 0 ? kRates[0] : kRates[kRateCount - 1]));
        return;
    }
    const double tol = r * kRateTolerance;
    if (dir > 0) {
        for (int i = 0; i < kRateCount; i++)
            if (kRates[i] > r + tol) {
                settings::set_frame_cap_ms(1000.0 / kRates[i]);
                return;
            }
    } else {
        for (int i = kRateCount - 1; i >= 0; i--)
            if (kRates[i] < r - tol) {
                settings::set_frame_cap_ms(1000.0 / kRates[i]);
                return;
            }
    }
    settings::set_frame_cap_ms(0.0);            /* past the highest / the lowest: no cap */
}

int rate_edges()
{
    return 3;                                   /* (a ring) */
}

void time_text(char *out, size_t n)
{
    const double ms = settings::frame_cap_ms();
    if (ms <= 0.0)
        snprintf(out, n, "%s", tr("OFF"));
    else
        snprintf(out, n, "%.3f MS", ms);
}

void time_step(int dir, int heldMs)
{
    const double ms = settings::frame_cap_ms();
    if (dir == 0)
        dir = 1;
    if (ms <= 0.0) {
        if (dir > 0 && heldMs == 0) {
            const double hz = gpu::refresh_hz();
            settings::set_frame_cap_ms(std::floor(100000.0 / (hz > 1.0 ? hz : 60.0)) / 100.0);
        }
        return;
    }
    const double step = heldMs < 1500 ? 0.01 : heldMs < 4000 ? 0.1 : 1.0;
    const double units = ms / step;
    const double v = (dir > 0 ? std::floor(units + 1e-6) + 1.0 : std::ceil(units - 1e-6) - 1.0) * step;
    settings::set_frame_cap_ms(v < settings::kFrameCapMin - 1e-9 ? 0.0 : std::min(v, settings::kFrameCapMax));
}

int time_edges()
{
    const double ms = settings::frame_cap_ms();
    if (ms <= 0.0)
        return 2;
    return 1 | (ms < settings::kFrameCapMax - 1e-9 ? 2 : 0);
}

int audio_out_get() { return (int)settings::audio_output(); }
void audio_out_set(int v) { settings::set_audio_output((settings::AudioOutput)v); }
const char *audio_out_value(int v)
{
    static const char *const names[settings::kAudioOutputCount] = {N_("AUTO"), N_("LOW LATENCY KERNEL STREAMING"), "WINDOWS WASAPI"};
    return v >= 0 && v < settings::kAudioOutputCount ? names[v] : "?";
}

const double kBuffers[] = {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 15.0, 20.0};
constexpr int kBufferCount = (int)(sizeof(kBuffers) / sizeof(kBuffers[0]));

/* The buffer as set (a crackle raises it by 0.5 ms: between the presets
 * too); Left / Right to the next preset below / above, the next value wraps
 * from the largest to the smallest. */
void buffer_text(char *out, size_t n) { snprintf(out, n, "%g MS", settings::audio_buffer_ms()); }

void buffer_step(int dir, int repeats)
{
    (void)repeats;
    const double ms = settings::audio_buffer_ms();
    if (dir < 0) {
        for (int i = kBufferCount - 1; i >= 0; i--)
            if (kBuffers[i] < ms - 0.01) {
                settings::set_audio_buffer_ms(kBuffers[i]);
                return;
            }
        return;
    }
    for (int i = 0; i < kBufferCount; i++)
        if (kBuffers[i] > ms + 0.01) {
            settings::set_audio_buffer_ms(kBuffers[i]);
            return;
        }
    if (dir == 0)
        settings::set_audio_buffer_ms(kBuffers[0]);
}

int buffer_edges()
{
    const double ms = settings::audio_buffer_ms();
    return (ms > kBuffers[0] + 0.01 ? 1 : 0) | (ms < kBuffers[kBufferCount - 1] - 0.01 ? 2 : 0);
}

/* The language (settings::language, platform/language.h): AUTO and the
 * languages installed, a ring; the game takes it at its next start (the
 * value says so while it differs from the running one's). */
void language_text(char *out, size_t n)
{
    const int s = settings::language();
    const bool later = language::chosen() != language::current();
    /* (PC 2026-11-16: the interface's language - one sentence each; the
     * names language.cpp's kNames, translated here) */
    if (s == settings::kLanguageAuto) {
        const char *const disc = tr(language::name(language::disc_language()));
        snprintf(out, n, "%s", (later ? trf("AUTO (%s) - NEXT START", disc) : trf("AUTO (%s)", disc)).c_str());
    } else {
        const char *const name = tr(language::name(s));
        snprintf(out, n, "%s", later ? trf("%s - NEXT START", name).c_str() : name);
    }
}

void language_step(int dir, int heldMs)
{
    if (heldMs != 0)
        return;                                 /* (one step a press) */
    int list[1 + language::kCount], count = 0, at = 0;
    list[count++] = settings::kLanguageAuto;
    for (int i = 0; i < language::kCount; i++)
        if (language::installed(i)) {
            if (settings::language() == i)
                at = count;
            list[count++] = i;
        }
    at = (at + (dir < 0 ? count - 1 : 1)) % count;
    settings::set_language(list[at]);
}

int language_edges()
{
    return 3;                                   /* (a ring) */
}

/* The units (settings::speed_unit, power_unit, torque_unit): AUTO (the
 * language's preset) then the units, in the game's own order; the game
 * takes a change at its next tick (game/app/pc_units.h). */
int speed_unit_get() { return settings::speed_unit() + 1; }
void speed_unit_set(int v) { settings::set_speed_unit(v - 1); }
const char *speed_unit_value(int v)
{
    static const char *const names[1 + settings::kSpeedUnits] = {N_("AUTO"), "KM/H, KM", N_("MPH, MILES")};
    return v >= 0 && v <= settings::kSpeedUnits ? names[v] : "?";
}
int power_unit_get() { return settings::power_unit() + 1; }
void power_unit_set(int v) { settings::set_power_unit(v - 1); }
const char *power_unit_value(int v)
{
    static const char *const names[1 + settings::kPowerUnits] = {N_("AUTO"), "PS", "HP", "BHP", "CV",
                                                                 "KW"};
    return v >= 0 && v <= settings::kPowerUnits ? names[v] : "?";
}
int torque_unit_get() { return settings::torque_unit() + 1; }
void torque_unit_set(int v) { settings::set_torque_unit(v - 1); }
const char *torque_unit_value(int v)
{
    static const char *const names[1 + settings::kTorqueUnits] = {N_("AUTO"), "KGF.M", "FT.LB", "NM"};
    return v >= 0 && v <= settings::kTorqueUnits ? names[v] : "?";
}

/* The starting credits (settings::starting_credits): the version's own,
 * 15,000, 18,000; x 100 (yen) shown in a Japanese run, as the game pays them
 * (game/app/pc_new_game.h). */
int credits_get() { return (int)settings::starting_credits_mode(); }
void credits_set(int v)
{
    settings::set_starting_credits_mode(
        (settings::StartingCredits)(v >= 0 && v < settings::kStartingCreditsCount ? v : 0));
}
const char *credits_value(int v)
{
    const bool yen = language::current() == language::kJapanese;
    switch (v) {
    case 1:
        return yen ? "1,500,000" : "15,000";
    case 2:
        return yen ? "1,800,000" : "18,000";
    case 3:
        return N_("CUSTOM");                    /* PC (2026-11-14): its amount one level deeper */
    default:
        return N_("Default");
    }
}
/* PC (2026-11-14): STARTING CREDITS > CUSTOM AMOUNT (settings::
 * starting_credits_custom), shown while CUSTOM is chosen: Left / Right by 100,
 * growing while held (1,000 from 0.7 s, 10,000 from 2 s, 100,000 from 3.5 s,
 * 1,000,000 from 5 s, 10,000,000 from 7 s), to the step's next multiple;
 * Enter / Cross back to 18,000.  Shown x 100 (yen) in a Japanese run, as the
 * two fixed amounts. */
int credits_custom() { return settings::starting_credits_mode() == settings::StartingCredits::custom ? 1 : 0; }
void credits_custom_text(char *out, size_t n)
{
    const long long v = (long long)settings::starting_credits_custom() *
                        (language::current() == language::kJapanese ? 100 : 1);
    /* (digits in threes) */
    char digits[24];
    snprintf(digits, sizeof(digits), "%lld", v);
    const size_t len = strlen(digits);
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i != 0 && (len - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = digits[i];
    }
    if (n > 0)
        out[o < n ? o : n - 1] = '\0';
}
void credits_custom_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_starting_credits_custom(settings::kStartingCreditsCustomDefault);
        return;
    }
    const long long step = heldMs < 700 ? 100 : heldMs < 2000 ? 1000 : heldMs < 3500 ? 10000
                           : heldMs < 5000 ? 100000 : heldMs < 7000 ? 1000000 : 10000000;
    const long long cur = settings::starting_credits_custom();
    long long v = dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step;
    v = std::max((long long)settings::kStartingCreditsCustomMin,
                 std::min((long long)settings::kStartingCreditsCustomMax, v));
    settings::set_starting_credits_custom((int)v);
}
int credits_custom_edges()
{
    const int v = settings::starting_credits_custom();
    return (v > settings::kStartingCreditsCustomMin ? 1 : 0) | (v < settings::kStartingCreditsCustomMax ? 2 : 0);
}

/* the output running (audio::describe), shown only */
void audio_now_text(char *out, size_t n) { audio::describe(out, n); }
void no_step(int, int) {}
int no_edges() { return 0; }

/* The volumes (settings::volume of V): Left / Right by 5 % (10 once held
 * 1.5 s); Enter / Cross back to the default (100; the race and replay music
 * 50). */
template <settings::Volume V> void vol_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::volume(V)); }
template <settings::Volume V> void vol_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_volume(V, settings::volume_default(V));
        return;
    }
    const int cur = settings::volume(V);
    const int step = heldMs < 1500 ? 5 : 10;
    settings::set_volume(V, dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}
template <settings::Volume V> int vol_edges()
{
    const int v = settings::volume(V);
    return (v > 0 ? 1 : 0) | (v < 100 ? 2 : 0);
}
#define VOLUME(V) 0, nullptr, nullptr, nullptr, vol_text<settings::Volume::V>, vol_step<settings::Volume::V>, \
                  vol_edges<settings::Volume::V>

/* The dead zones (input_map::deadzone of profile P, dead zone I): Left /
 * Right by 1 % (5 once held 1.5 s); Enter / Cross back to 0. */
template <int P, int I> void dz_text(char *out, size_t n) { snprintf(out, n, "%d%%", input_map::deadzone(P, I)); }
template <int P, int I> void dz_step(int dir, int heldMs)
{
    const int cur = input_map::deadzone(P, I);
    if (dir == 0) {
        input_map::set_deadzone(P, I, 0);
        return;
    }
    const int step = heldMs < 1500 ? 1 : 5;
    input_map::set_deadzone(P, I, dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}
template <int P, int I> int dz_edges()
{
    const int d = input_map::deadzone(P, I);
    return (d > 0 ? 1 : 0) | (d < input_map::kDeadzoneMax ? 2 : 0);
}
#define DEADZONE(P, I) 0, nullptr, nullptr, nullptr, dz_text<P, I>, dz_step<P, I>, dz_edges<P, I>

/* The course scale (settings::course_scale of axis A): Left / Right by 5 %
 * (25 once held 1.5 s) to the next multiple, X and Z over 0 to the other
 * sign's 5 % (no scale is 0; negative: mirrored), Y no lower than 5 %
 * (settings::course_scale_min; the setter holds it there); Enter / Cross
 * back to 100. */
template <int A> void course_text(char *out, size_t n)
{
    const int s = settings::course_scale(A);
    if (s < 0)
        snprintf(out, n, "%s", trf("%d%% MIRRORED", s).c_str());
    else
        snprintf(out, n, "%d%%", s);
}
template <int A> void course_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_course_scale(A, 100);
        return;
    }
    const int cur = settings::course_scale(A);
    const int step = heldMs < 1500 ? 5 : 25;
    const double q = (double)cur / step;
    int next = (int)(dir > 0 ? std::floor(q) + 1.0 : std::ceil(q) - 1.0) * step;
    if (next > -settings::kCourseScaleNearest && next < settings::kCourseScaleNearest)
        next = dir > 0 ? settings::kCourseScaleNearest : -settings::kCourseScaleNearest;
    settings::set_course_scale(A, next);
}
template <int A> int course_edges()
{
    const int s = settings::course_scale(A);
    return (s > settings::course_scale_min(A) ? 1 : 0) | (s < settings::kCourseScaleMax ? 2 : 0);
}
#define COURSE_SCALE(A) 0, nullptr, nullptr, nullptr, course_text<A>, course_step<A>, course_edges<A>

int prize_random_get() { return settings::prize_car_randomizer() ? 1 : 0; }
void prize_random_set(int v) { settings::set_prize_car_randomizer(v != 0); }
/* PC (2026-11-13): GAME > EXTRAS > UNRESTRICTED WHEELS */
int free_wheels_get() { return settings::unrestricted_wheels() ? 1 : 0; }
void free_wheels_set(int v) { settings::set_unrestricted_wheels(v != 0); }
/* PC (2026-11-14): GAME > EXTRAS > BUY CARS NOT FOR SALE */
int not_for_sale_get() { return settings::buy_not_for_sale() ? 1 : 0; }
void not_for_sale_set(int v) { settings::set_buy_not_for_sale(v != 0); }
/* PC (2026-11-14): CHALLENGE MODES (settings::challenge, game/app/pc_challenge.h) */
template <settings::Challenge C> int challenge_get() { return settings::challenge(C) ? 1 : 0; }
template <settings::Challenge C> void challenge_set(int v) { settings::set_challenge(C, v != 0); }

/* PC (2026-11-15, user request: "F2 menu gets selectors for slot 1 and 2,
 * options are populated from the engine's save folder"): MEMORY CARDS >
 * SLOT 1 / 2 (memcard.h set_slot_folder) - NONE (no card), then the save
 * folders (memcard::folders) but the other slot's; the name shown upper
 * case, "(NOT FOUND)" when its folder is gone, "(AFTER THE SAVE)" while the
 * card has a file open (the change waits for it) */
template <int P> std::vector<std::string> card_choices()
{
    std::vector<std::string> c(1, std::string());
    const std::string other = memcard::slot_folder(1 - P);
    for (const std::string &f : memcard::folders())
        if (_stricmp(f.c_str(), other.c_str()) != 0)
            c.push_back(f);
    return c;
}
/* -1: the slot's folder is not one of them (gone) */
template <int P> int card_index(const std::vector<std::string> &c)
{
    const std::string cur = memcard::slot_folder(P);
    for (size_t i = 0; i < c.size(); i++)
        if (_stricmp(c[i].c_str(), cur.c_str()) == 0)
            return (int)i;
    return -1;
}
template <int P> void card_text(char *out, size_t n)
{
    std::string cur = memcard::slot_folder(P);
    if (cur.empty()) {
        snprintf(out, n, "%s", memcard::folders().empty() ? tr("NONE (NO SAVE FOLDERS)") : tr("NONE"));
        return;
    }
    for (char &c : cur)
        c = (char)toupper((unsigned char)c);
    if (!memcard::slot_folder_there(P))
        snprintf(out, n, "%s", trf("%s (NOT FOUND)", cur.c_str()).c_str());
    else if (memcard::slot_waiting(P))
        snprintf(out, n, "%s", trf("%s (AFTER THE SAVE)", cur.c_str()).c_str());
    else
        snprintf(out, n, "%s", cur.c_str());
}
template <int P> void card_step(int dir, int heldMs)
{
    (void)heldMs;
    const std::vector<std::string> c = card_choices<P>();
    const int n = (int)c.size();
    int i = card_index<P>(c);
    if (dir == 0)
        i = (i + 1) % n;                                /* the next one, wrapping */
    else if (i < 0)
        i = 0;
    else
        i = std::max(0, std::min(n - 1, i + dir));
    memcard::set_slot_folder(P, c[(size_t)i]);
}
template <int P> int card_edges()
{
    const std::vector<std::string> c = card_choices<P>();
    const int n = (int)c.size(), i = card_index<P>(c);
    if (i < 0)
        return 3;
    return (i > 0 ? 1 : 0) | (i < n - 1 ? 2 : 0);
}
constexpr const char kCardHelp[] =
    N_("THE SAVE FOLDER IN THIS SLOT: ONE OF THE ENGINE'S SAVES FOLDER.\n"
       "NONE: NO CARD. CHANGING IT SWAPS THE CARD: THE GAME SEES THE NEW ONE\n"
       "AT ONCE (ITS GAME DATA: LOAD IT IN THE GAME). ENTER: THE NEXT ONE.\n"
       "THE LAUNCHER'S SAVES... WINDOW MAKES, RENAMES AND CLEARS THEM.");
/* PC (2026-11-07): off / on / extended (settings::CourseRandomizer) */
int course_random_get() { return (int)settings::course_randomizer_mode(); }
void course_random_set(int v) { settings::set_course_randomizer((settings::CourseRandomizer)v); }
const char *course_random_value(int v) { return v == 2 ? N_("EXTENDED") : v == 1 ? N_("ON") : N_("OFF"); }
/* PC (2026-11-08): original / realistic (settings::RevCut) */
int rev_cut_get() { return (int)settings::rev_cut(); }
void rev_cut_set(int v) { settings::set_rev_cut((settings::RevCut)v); }
const char *rev_cut_value(int v) { return v == 1 ? N_("REALISTIC") : N_("ORIGINAL"); }
/* PC (2026-11-08): original / realistic (settings::ShiftBehavior); PC
 * (2026-11-09) realistic + clutch, its H-SHIFTER item one level deeper
 * (shift_clutch), and choosing it shows what the clutch is mapped to */
int shift_get() { return (int)settings::shift_behavior(); }
void clutch_notice();
void shift_set(int v)
{
    settings::set_shift_behavior((settings::ShiftBehavior)v);
    if (v == (int)settings::ShiftBehavior::clutch)
        clutch_notice();
}
const char *shift_value(int v) { return v == 2 ? N_("REALISTIC + CLUTCH") : v == 1 ? N_("REALISTIC") : N_("ORIGINAL"); }
int shift_clutch() { return settings::shift_behavior() == settings::ShiftBehavior::clutch ? 1 : 0; }
int hshift_get() { return settings::h_shifter() ? 1 : 0; }
void hshift_set(int v) { settings::set_h_shifter(v != 0); }

/* The notice of the clutch mode: each controller's / wheel's clutch, or
 * that none is mapped and where to map one (upper case: the menu's font) */
void clutch_notice()
{
    char text[1024] = "";                       /* (PC 2026-11-16: 320 before; UTF-8 translations) */
    int lines = 0, n = 0;
    for (int p = input_map::kController1; p <= input_map::kWheel2 && lines < 3; p++) {
        char what[256];
        clutch_input::clutch_text(p, what, sizeof(what));
        if (what[0] == '\0')
            continue;
        n += snprintf(text + n, sizeof(text) - n, "%s\n%s: %s", lines == 0 ? tr("THE CLUTCH IS MAPPED TO") : "",
                      tr(input_map::profile_name(p)), what);
        lines++;
    }
    if (lines == 0)
        snprintf(text, sizeof(text), "%s",
                 tr("NOTHING IS MAPPED TO THE CLUTCH\nADD ONE IN THE LAUNCHER: CONTROLS, A CONTROLLER\n"
                    "OR A WHEEL, CLUTCH. UNTIL THEN THE GEARS\nSHIFT AS REALISTIC."));
    for (char *c = text; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);
    notice(text, 7.0);
}

/* The cars of a race (settings::race_cars): Left / Right by 1 (4 once held
 * 1.5 s); Enter / Cross back to 6. */
void race_cars_text(char *out, size_t n) { snprintf(out, n, "%d", settings::race_cars()); }
void race_cars_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_race_cars(settings::kRaceCarsMin);
        return;
    }
    settings::set_race_cars(settings::race_cars() + dir * (heldMs < 1500 ? 1 : 4));
}
int race_cars_edges()
{
    const int n = settings::race_cars();
    return (n > settings::kRaceCarsMin ? 1 : 0) | (n < settings::kRaceCarsMax ? 2 : 0);
}
int race_cars_override_get() { return settings::race_cars_override() ? 1 : 0; }
void race_cars_override_set(int v) { settings::set_race_cars_override(v != 0); }

/* The car multipliers (settings::car_tune of T, the enemies' when E): Left /
 * Right by 5 % (25 once held 1.5 s) to the next multiple; Enter / Cross
 * back to 100. */
template <settings::CarTune T, bool E> void tune_text(char *out, size_t n)
{
    snprintf(out, n, "%d%%", settings::car_tune(T, E));
}
template <settings::CarTune T, bool E> void tune_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_car_tune(T, E, 100);
        return;
    }
    const int cur = settings::car_tune(T, E);
    const int step = heldMs < 1500 ? 5 : 25;
    settings::set_car_tune(T, E, dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}
template <settings::CarTune T, bool E> int tune_edges()
{
    const int v = settings::car_tune(T, E);
    return (v > settings::car_tune_min(T) ? 1 : 0) | (v < settings::kCarTuneMax ? 2 : 0);
}
#define CAR_TUNE(T, E) 0, nullptr, nullptr, nullptr, tune_text<settings::CarTune::T, E>, \
                       tune_step<settings::CarTune::T, E>, tune_edges<settings::CarTune::T, E>

/* The surfaces' raw values (settings::surface_tune of surface S, in
 * hundredths; unset: the game's own, shown with "GAME'S"): Left / Right by
 * 0.01 (0.10 once held 1.5 s, 1.00 once held 3 s) to the next multiple,
 * from the game's value when unset; Enter / Cross back to the game's own. */
template <int S, settings::SurfaceTune T> int surf_value()
{
    const int v = settings::surface_tune(S, T);
    return v == settings::kSurfaceOwn ? settings::surface_tune_default(S, T) : v;
}
template <int S, settings::SurfaceTune T> void surf_text(char *out, size_t n)
{
    const bool own = settings::surface_tune(S, T) == settings::kSurfaceOwn;
    if (own)
        snprintf(out, n, "%s", trf("%.2f (GAME'S)", surf_value<S, T>() / 100.0).c_str());
    else
        snprintf(out, n, "%.2f", surf_value<S, T>() / 100.0);
}
template <int S, settings::SurfaceTune T> void surf_step(int dir, int heldMs)
{
    if (dir == 0) {
        settings::set_surface_tune(S, T, settings::kSurfaceOwn);
        return;
    }
    const int cur = surf_value<S, T>();
    const int step = heldMs < 1500 ? 1 : heldMs < 3000 ? 10 : 100;
    const int next = dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step;
    settings::set_surface_tune(S, T, std::max(next, settings::surface_tune_min(T)));   /* (never the own's -1) */
}
template <int S, settings::SurfaceTune T> int surf_edges()
{
    const int v = surf_value<S, T>();
    return (v > settings::surface_tune_min(T) ? 1 : 0) | (v < settings::surface_tune_max(T) ? 2 : 0);
}
#define SURFACE(S, T) 0, nullptr, nullptr, nullptr, surf_text<S, settings::SurfaceTune::T>, \
                      surf_step<S, settings::SurfaceTune::T>, surf_edges<S, settings::SurfaceTune::T>
/* where each surface is (settings::surface_key's order) */
const char *const kSurfaceHelp[settings::kSurfaces] = {
    N_("THE ROAD (THE DATABASE'S TA). ENTER: THE GAME'S."),
    N_("THE KERBS (GU). ENTER: THE GAME'S."),
    N_("THE GRASS (GR). ENTER: THE GAME'S."),
    N_("THE SAND TRAPS (SA). ENTER: THE GAME'S."),
    N_("THE GRAVEL TRAPS: LAGUNA SECA (GV). ENTER: THE GAME'S."),
    N_("THE RALLY COURSES' DIRT (DT). ENTER: THE GAME'S."),
    N_("A ROAD SURFACE OF ROME, COTE D'AZUR, GRAND VALLEY,\nDEEP FOREST AND THE TEST CIRCLES (THE DATABASE'S SF).\nENTER: THE GAME'S."),
    N_("A SURFACE OF SMOKEY MOUNTAIN SOUTH (THE DATABASE'S WD).\nENTER: THE GAME'S."),
    N_("A ROAD SURFACE OF TOKYO R246 AND ROUTE 5 DRY\n(THE DATABASE'S HT). ENTER: THE GAME'S."),
    N_("THE WET ROAD: ROUTE 5 WET, THE WET CIRCLES (WR).\nENTER: THE GAME'S."),
    N_("THE WET KERBS (WH). ENTER: THE GAME'S."),
    N_("HT WET: ROUTE 5 WET (WG). ENTER: THE GAME'S."),
};

int replay_logo_get() { return settings::replay_logo() ? 1 : 0; }
void replay_logo_set(int v) { settings::set_replay_logo(v != 0); }

/* DEBUG: the launcher's Debug tab's new-game and unlock switches */
int fill_garage_get() { return settings::debug_fill_garage() ? 1 : 0; }
void fill_garage_set(int v) { settings::set_debug_fill_garage(v != 0); }
int fill_money_get() { return settings::debug_fill_money() ? 1 : 0; }
void fill_money_set(int v) { settings::set_debug_fill_money(v != 0); }
int unlock_arcade_get() { return settings::debug_unlock_arcade() ? 1 : 0; }
void unlock_arcade_set(int v) { settings::set_debug_unlock_arcade(v != 0); }
int unlock_licenses_get() { return settings::debug_unlock_licenses() ? 1 : 0; }
void unlock_licenses_set(int v) { settings::set_debug_unlock_licenses(v != 0); }

/* DEBUG > COURSE FORCER: on / off, the course nested in it (the game's
 * choices, settings::course_choice; the label shown upper case, with its
 * start places when fewer than a 6-car race needs) */
int forced_index();
int forcer_get() { return settings::course_forcer() ? 1 : 0; }
void forcer_set(int v)
{
    settings::set_course_forcer(v != 0);
    /* on without a course of the list: the list's first */
    if (v != 0 && forced_index() < 0 && settings::course_choice_count() > 0)
        settings::set_forced_course(settings::course_choice(0).label);
}
int forced_index()
{
    char cur[64];
    settings::forced_course(cur, sizeof(cur));
    const int n = settings::course_choice_count();
    for (int i = 0; i < n; i++)
        if (_stricmp(settings::course_choice(i).label, cur) == 0)
            return i;
    return -1;
}
void forced_text(char *out, size_t n)
{
    char cur[64];
    settings::forced_course(cur, sizeof(cur));
    if (cur[0] == '\0') {
        snprintf(out, n, "%s", settings::course_choice_count() > 0 ? tr("NONE") : tr("NOT KNOWN YET"));
        return;
    }
    for (char *p = cur; *p != '\0'; p++)
        *p = (char)toupper((unsigned char)*p);
    const int i = forced_index();
    const int places = i >= 0 ? settings::course_choice(i).places : -1;
    if (i < 0 && settings::course_choice_count() > 0)
        snprintf(out, n, "%s ?", cur);             /* not a course of this data */
    else if (places >= 0 && places < 6)
        snprintf(out, n, "%s", (places == 1 ? trf("%s (%d CAR)", cur, places) : trf("%s (%d CARS)", cur, places)).c_str());
    else
        snprintf(out, n, "%s", cur);
}
void forced_step(int dir, int heldMs)
{
    (void)heldMs;
    const int n = settings::course_choice_count();
    if (n == 0)
        return;
    int i = forced_index();
    if (dir == 0)
        i = (i + 1) % n;                            /* the next one, wrapping */
    else if (i < 0)
        i = 0;
    else
        i = std::max(0, std::min(n - 1, i + dir));
    settings::set_forced_course(settings::course_choice(i).label);
}
int forced_edges()
{
    const int n = settings::course_choice_count(), i = forced_index();
    if (n == 0)
        return 0;
    if (i < 0)
        return 3;
    return (i > 0 ? 1 : 0) | (i < n - 1 ? 2 : 0);
}

/* DEBUG > LAP FORCER: on / off, the laps nested in it (1 a step, 5 once
 * held; Enter the next, wrapping) */
int lap_forcer_get() { return settings::lap_forcer() ? 1 : 0; }
void lap_forcer_set(int v) { settings::set_lap_forcer(v != 0); }
void laps_text(char *out, size_t n)
{
    const int laps = settings::forced_laps();
    snprintf(out, n, "%s", (laps == 1 ? trf("%d LAP", laps) : trf("%d LAPS", laps)).c_str());
}
void laps_step(int dir, int heldMs)
{
    const int cur = settings::forced_laps();
    if (dir == 0) {
        settings::set_forced_laps(cur >= settings::kForcedLapsMax ? settings::kForcedLapsMin : cur + 1);
        return;
    }
    settings::set_forced_laps(cur + dir * (heldMs < 1500 ? 1 : 5));
}
int laps_edges()
{
    const int laps = settings::forced_laps();
    return (laps > settings::kForcedLapsMin ? 1 : 0) | (laps < settings::kForcedLapsMax ? 2 : 0);
}

/* DEBUG > RACE TIME FORCER: on / off, the minutes nested in it (1 a step,
 * 10 once held; Enter the next, wrapping) */
int race_time_forcer_get() { return settings::race_time_forcer() ? 1 : 0; }
void race_time_forcer_set(int v) { settings::set_race_time_forcer(v != 0); }
void race_time_text(char *out, size_t n)
{
    const int minutes = settings::forced_race_time();
    if (minutes >= 60)
        snprintf(out, n, "%d MIN (%d:%02d H)", minutes, minutes / 60, minutes % 60);
    else
        snprintf(out, n, "%d MIN", minutes);
}
void race_time_step(int dir, int heldMs)
{
    const int cur = settings::forced_race_time();
    if (dir == 0) {
        settings::set_forced_race_time(cur >= settings::kForcedRaceTimeMax ? settings::kForcedRaceTimeMin : cur + 1);
        return;
    }
    settings::set_forced_race_time(cur + dir * (heldMs < 1500 ? 1 : 10));
}
int race_time_edges()
{
    const int minutes = settings::forced_race_time();
    return (minutes > settings::kForcedRaceTimeMin ? 1 : 0) | (minutes < settings::kForcedRaceTimeMax ? 2 : 0);
}

/* PC (2026-11-07): DEBUG > COURSE MODE FORCER (settings::CourseMode): what
 * ends every race entered */
int course_mode_get() { return (int)settings::course_mode_forcer(); }
void course_mode_set(int v) { settings::set_course_mode_forcer((settings::CourseMode)v); }
const char *course_mode_value(int v)
{
    static const char *const names[settings::kCourseModeCount] = {N_("OFF"), N_("FINISH LINE"), "0-400 M", "0-1000 M",
                                                                   N_("MAX SPEED (6845 M)")};
    return v >= 0 && v < settings::kCourseModeCount ? names[v] : "?";
}

/* PC (2026-11-12): DEBUG > MENU RACE MODEL and its LEVEL OF DETAIL (0 AUTO,
 * then the levels 0 .. kMenuRaceModelLodMax) */
int menu_race_model_get() { return settings::menu_race_model() ? 1 : 0; }
void menu_race_model_set(int v) { settings::set_menu_race_model(v != 0); }
int menu_race_lod_get() { return settings::menu_race_model_lod() + 1; }
void menu_race_lod_set(int v) { settings::set_menu_race_model_lod(v - 1); }
const char *menu_race_lod_value(int v)
{
    static const char *const names[settings::kMenuRaceModelLodMax + 2] = {N_("AUTO"), N_("0 (FINEST)"), "1",
                                                                         N_("2 (COARSEST)")};
    return v >= 0 && v < settings::kMenuRaceModelLodMax + 2 ? names[v] : "?";
}

/* What player N plays with while both the controller and the wheel are
 * connected (input_map::plays_with_controller): 0 the controller, 1 the
 * wheel. */
template <int N> int device_get() { return input_map::plays_with_controller(N) ? 0 : 1; }
template <int N> void device_set(int v) { input_map::set_plays_with_controller(N, v == 0); }
const char *device_value(int v) { return v ? N_("WHEEL") : N_("CONTROLLER"); }
const char kDeviceHelp[] = N_("THE DEVICE IN USE WHILE BOTH A CONTROLLER AND A WHEEL ARE CONNECTED.");

const char kStickHelp[] = N_("HOW FAR THE STICK MOVES BEFORE IT COUNTS.\nENTER: 0%.");
const char kTriggerHelp[] = N_("HOW FAR THE TRIGGERS MOVE BEFORE THEY COUNT.\nENTER: 0%.");
const char kSteerHelp[] = N_("HOW FAR THE WHEEL TURNS BEFORE IT STEERS.\nENTER: 0%.");
const char kPedalHelp[] = N_("HOW FAR THE PEDAL GOES BEFORE IT COUNTS.\nENTER: 0%.");

/* (help: any number of lines, '\n' between; the panel takes the most lines
 * and the longest line of them all) */
const Item kItems[] = {
    {0, N_("DISPLAY"), N_("SCREEN MODE, MENU ASPECT, VERTICAL SYNC, FRAME RATE\nAND FRAME TIMES."), kCategory},
    {1, N_("SCREEN MODE"), N_("WINDOWED: A RESIZABLE WINDOW.\nBORDERLESS: A WINDOW COVERING THE MONITOR.\nFULLSCREEN: EXCLUSIVE, THE MONITOR'S DISPLAY MODE."),
     3, screen_get, screen_set, screen_value},
    {1, N_("MENU ASPECT"), "",
     settings::kMenuAspectCount, menu_aspect_get, menu_aspect_set, menu_aspect_value},
    {1, N_("MOVIE ASPECT"), "",
     2, movie_aspect_get, movie_aspect_set, movie_aspect_value},
    {1, N_("VERTICAL SYNC"), N_("OFF: NO VSYNC.\n1-3: VSYNC ON, NUMBER OF BUFFERED FRAMES.\n1 FRAME PROVIDES THE BEST FRAME PACING\nIF YOUR MACHINE CAN MAINTAIN FRAME RATE."), 4,
     vsync_get, vsync_set, vsync_value},
    {1, N_("FRAME RATE CAP"), N_("THE FRAMES AT MOST THIS OFTEN (A FRAME TIME CAP);\nOFF: AS FAST AS THEY ARE DRAWN."), 0, nullptr,
     nullptr, nullptr, rate_text, rate_step, rate_edges},
    {1, N_("  FRAME TIME CAP"), N_("THE SAME CAP IN MS, 0.01 A STEP (HOLD: FASTER);\nBELOW 1 MS, OFF."), 0, nullptr, nullptr,
     nullptr, time_text, time_step, time_edges},
    {1, N_("FRAME TIME MONITOR"), N_("THE FRAME TIMES OVER THE TOP LEFT CORNER.\nDETAILED: WITH THE FRAME TIME'S BREAKDOWN.\nVERBOSE: ALSO THE GPU'S TIME BY SECTION."),
     frame_monitor::kModes, monitor_get, monitor_set, monitor_value, nullptr, nullptr, nullptr,
     input_map::kHotFrameMonitor},
    {0, N_("GRAPHICS"), N_("RESOLUTION, ANTI-ALIASING, DETAIL.\nSMOKE OPACITY. REAR VIEW."), kCategory},
    {1, N_("RESOLUTION SCALE"), N_("THE GAME'S RESOLUTION, OF THE WINDOW'S.\nABOVE 100%: SCALED DOWN TO THE WINDOW\n(SUPERSAMPLING AA); MORE GPU. ENTER: 100%."),
     0, nullptr, nullptr, nullptr, scale_text, scale_step, scale_edges},
    {1, N_("ANTI-ALIASING"), N_("FXAA: THE RACE VIEWS' 3D, BEFORE THE HUD.\nMSAA: EVERY PICTURE, MENUS TOO; MORE GPU.\nAGSS: MSAA WITH THE 3D SHADED AT EVERY SAMPLE (SUPERSAMPLING):\nTEXTURE EDGES (TREES, FENCES) AND SHIMMER TOO; MUCH MORE GPU."),
     settings::kAntiAliasingCount, aa_get, aa_set, aa_value},
    /* PC (2026-11-10): settings::anisotropic_filtering */
    {1, N_("ANISOTROPIC FILTERING"), N_("SHARPER TEXTURES ON SURFACES SEEN AT AN ANGLE (THE ROAD).\nTHE TRANSPARENT PARTS OF TEXTURES ADD NO COLOR (NO HIDDEN COLORS SHOWING)."),
     settings::kAnisotropicCount, aniso_get, aniso_set, aniso_value},
    {1, N_("LEVELS OF DETAIL"), N_("BEST: CARS, COURSES, AND SPLIT-SCREEN ALWAYS AT FULL DETAIL."), 2,
     lod_get, lod_set, lod_value},
    {1, N_("ROAD REFLECTION"), N_("HIGH IMPACT ON FRAME RATE WITH MANY CARS."), 2,
     refl_get, refl_set, refl_value},
    /* PC (2026-11-14): the road reflection's buffer finer (settings::ReflectionResolution) */
    {1, N_("ROAD REFLECTION RESOLUTION"), N_("THE REFLECTIONS ON ROUTE 5, ROUTE 11 AND THE TEST CIRCLES.\nNORMAL: THE PS2'S (64 X 128 FOR THE WHOLE SCREEN).\nHALF: HALF THE SCREEN'S RESOLUTION. FULL: THE SCREEN'S. SHARPER; MORE GPU."),
     settings::kReflectionResolutionCount, refl_res_get, refl_res_set, refl_res_value},
    {1, N_("CAR REFLECTIONS"), N_("HIGH IMPACT ON FRAME RATE WITH MANY CARS."),
     settings::kCarReflectionsCount, car_refl_get, car_refl_set, car_refl_value},
    {1, N_("ENV MAP DETAIL"), N_("CAR REFLECTIONS QUALITY"),
     settings::kEnvMapDetailCount, env_map_get, env_map_set, env_map_value},
    {1, N_("REPLAY DEPTH OF FIELD"), "",
     settings::kReplayDepthOfFieldCount, replay_dof_get, replay_dof_set, replay_dof_value},
    {1, N_("RENDER DISTANCE"), N_("HOW FAR THE COURSE AND THE CARS ARE DRAWN,\nOF THE ORIGINAL GAME'S DISTANCE."), kDistanceCount,
     distance_get, distance_set, distance_value},
    {1, N_("SMOKE OPACITY"), N_("THE TIRE SMOKE AND DUST.\nREDUCED: 75% OF THEIR OPACITY. HALF: 50%."),
     settings::kSmokeOpacityCount, smoke_get, smoke_set, smoke_value},
    /* PC (2026-11-12): the menu (showroom) wheel models on the race cars */
    {1, N_("MENU WHEELS"), N_("SHOWROOM WHEEL MODELS IN RACES"),
     2, menu_wheels_get, menu_wheels_set, on_off_value},
    {2, N_("RACE WHEELS FROM"), N_("WHEEL MODEL SWITCH THRESHOLD"),
     0, nullptr, nullptr, nullptr, menu_wheels_speed_text, menu_wheels_speed_step, menu_wheels_speed_edges},
    /* PC (2026-11-13): the menu (showroom) tyres on the race cars */
    {1, N_("MENU TIRES"), N_("SHOWROOM TIRE MODELS IN RACES"),
     2, menu_tires_get, menu_tires_set, on_off_value},
    /* PC (2026-11-15, user request: "Move Rear view settings into graphics
     * category"; was CAMERAS') */
    {1, N_("REAR VIEW"), N_("THE REAR-VIEW MIRROR, BY CAMERA.\nHIGH IMPACT ON FRAME RATE."), kCategory},
    {2, N_("BUMPER CAM"), N_("THE REAR-VIEW MIRROR IN THE BUMPER CAMERA.\nHIGH IMPACT ON FRAME RATE."), 2, rear_get<0>,
     rear_set<0>, on_off_value},
    {2, N_("CHASE CAM"), N_("THE REAR-VIEW MIRROR IN THE CHASE CAMERA.\nHIGH IMPACT ON FRAME RATE."), 2, rear_get<1>,
     rear_set<1>, on_off_value},
    /* PC (2026-11-07): the cameras in a category of their own (user request) */
    {0, N_("CAMERAS"), N_("FIELD OF VIEW, EXTRA CAMERAS,\nTHE CHASE CAMERA'S ANGLE, THE COCKPIT CAMERA.\nCUSTOM CAMERAS."), kCategory},
    {1, N_("FIELD OF VIEW"), N_("THE DRIVING VIEWS' ANGLE, OF EACH CAMERA'S OWN.\nNOT THE MIRROR. ENTER: 100%."), 0,
     nullptr, nullptr, nullptr, fov_text, fov_step, fov_edges},
    {1, N_("  REPLAY FIELD OF VIEW"), N_("THE REPLAY CAMERAS' ANGLE, OF EACH CAMERA'S OWN\n(THE ATTRACT MODE'S DEMOS TOO). ENTER: 100%."),
     0, nullptr, nullptr, nullptr, replay_fov_text, replay_fov_step, replay_fov_edges},
    /* (PC 2026-11-15: REAR VIEW moved to GRAPHICS, user request) */
    {1, N_("REPLAY CAMERAS"), N_("CYCLE THROUGH REPLAY CAMERAS IN RACES (MIRRORS, NOSE, ROOF, TAIL, WHEELS...)."),
     2, extra_cameras_get, extra_cameras_set, on_off_value},
    {1, N_("CHASE CAM ANGLE"), N_("CHASE CAMERA'S HEIGHT ADJUSTMENT.\nABOVE 0 LOOKS DOWN ON THE CAR. ENTER: 0."), 0,
     nullptr, nullptr, nullptr, chase_angle_text, chase_angle_step, chase_angle_edges},
    {1, N_("COCKPIT CAMERA"), N_("THE COCKPIT VIEW FROM THE DRIVER'S HEAD, THE HEAD HIDDEN.\nTHE CHASE CAMERA'S CONTROLS LOOK ROUND.\nTHE VIEW BUTTON REACHES IT, EXTRA CAMERAS OR NOT."),
     2, cockpit_get, cockpit_set, on_off_value},
    {2, N_("COCKPIT FIELD OF VIEW"), N_("THE COCKPIT CAMERA'S VERTICAL ANGLE IN DEGREES.\nENTER: 90."), 0,
     nullptr, nullptr, nullptr, cockpit_fov_text, cockpit_fov_step, cockpit_fov_edges},
    /* PC (2026-11-08): the calibration mode (cockpit_osd.h); PC (2026-11-15):
     * the menu model (was COCKPIT MENU MODEL here, for every car) its row */
    {2, N_("CALIBRATE COCKPIT CAMERA"), N_("IN A RACE, IN THE COCKPIT VIEW: THE D-PAD, THE ARROW KEYS\nOR THE CAMERA CONTROLS MOVE THE CAMERA AND TILT IT, ONE\nAXIS AT A TIME; SAVED FOR THIS CAR MODEL.\nITS MENU MODEL ROW: THE CAR DRAWN WITH ITS SHOWROOM MODEL.\nENTER: START (THE MENU CLOSES, A PANEL SHOWS THE KEYS)."),
     0, nullptr, nullptr, nullptr, calibrate_text, calibrate_step, no_edges},
    /* PC (2026-11-14): the user's own cameras (cockpit_osd.h, game/app/pc_customcam.h) */
    {1, N_("CUSTOM CAMERAS"), N_("YOUR OWN CAMERAS, ON THE VIEW BUTTON AFTER THE COCKPIT:\nFOR EVERY CAR, OR FOR ONE CAR MODEL.\nMADE AND EDITED IN A RACE. SAVED IN GT3PC.INI."), kCategory},
    {2, N_("NEW CAMERA: EVERY CAR"), N_("IN A RACE: A NEW CAMERA FOR EVERY CAR, STARTED AS A COPY\nOF THE VIEW SHOWN. ITS EDITOR: POSITION WITHOUT LIMITS,\nPAN, TILT, ANGLE, FIELD OF VIEW, RIGIDITY.\nENTER: START (THE MENU CLOSES, A PANEL SHOWS THE KEYS)."),
     0, nullptr, nullptr, nullptr, custom_text, custom_step<cockpit_osd::kRequestNewGlobal>, no_edges},
    {2, N_("NEW CAMERA: THIS CAR"), N_("IN A RACE: A NEW CAMERA FOR THE MODEL OF THE CAR IN VIEW\nALONE, STARTED AS A COPY OF THE VIEW SHOWN.\nENTER: START (THE MENU CLOSES, A PANEL SHOWS THE KEYS)."),
     0, nullptr, nullptr, nullptr, custom_text, custom_step<cockpit_osd::kRequestNewCar>, no_edges},
    {2, N_("EDIT CAMERA"), N_("IN A RACE, IN A CUSTOM CAMERA'S VIEW: THE D-PAD, THE ARROW\nKEYS OR THE CAMERA CONTROLS CHANGE IT, ONE ROW AT A TIME.\nENTER: START (THE MENU CLOSES, A PANEL SHOWS THE KEYS)."),
     0, nullptr, nullptr, nullptr, custom_text, custom_step<cockpit_osd::kRequestEdit>, no_edges},
    {2, N_("DELETE CAMERA"), N_("IN A RACE: DELETES THE CUSTOM CAMERA IN VIEW.\nENTER TWICE."),
     0, nullptr, nullptr, nullptr, custom_delete_text, custom_delete_step, no_edges},
    {0, N_("SPLIT SCREEN"), N_("THE 2-PLAYER SPLIT SCREEN."), kCategory},
    {1, N_("SPLIT SCREEN"), N_("HORIZONTAL: ONE VIEW ABOVE THE OTHER.\nVERTICAL: SIDE BY SIDE.\nTWO MONITORS: PLAYER 2 ON A SECOND MONITOR.\nFROM THE NEXT RACE OR REPLAY."),
     settings::kSplitScreenCount, split_get, split_set, split_value},
    {0, N_("AUDIO"), N_("THE SOUND OUTPUT AND THE VOLUMES."), kCategory},
    /* (PC 2026-11-15, user request: "Volume goes above the output settings") */
    {1, N_("VOLUME"), N_("THE MUSIC'S AND THE SOUND EFFECTS' LEVELS.\nTHE GAME'S OWN OPTIONS STILL TURN EACH ON OR OFF."),
     kCategory},
    {2, N_("MENU MUSIC"), N_("THE MENUS' MUSIC. 100%: THE MUSIC FILES AS THEY ARE.\nENTER: 100%."), VOLUME(menuBgm)},
    {2, N_("RACE MUSIC"), N_("A RACE'S MUSIC: THE SONGS AND THE JINGLES.\n100%: THE MUSIC FILES AS THEY ARE. ENTER: 50%."),
     VOLUME(raceBgm)},
    {2, N_("REPLAY MUSIC"), N_("A REPLAY'S MUSIC (THE DEMOS' TOO).\n100%: THE MUSIC FILES AS THEY ARE. ENTER: 50%."),
     VOLUME(replayBgm)},
    {2, N_("MENU SOUND EFFECTS"), N_("THE MENUS' SOUNDS: CURSOR, DECIDE, CANCEL.\nENTER: 100%."), VOLUME(menuSe)},
    {2, N_("RACE SOUND EFFECTS"), N_("A RACE'S CARS AND COURSE. ENTER: 100%."), VOLUME(raceSe)},
    {2, N_("REPLAY SOUND EFFECTS"), N_("A REPLAY'S CARS AND COURSE. ENTER: 100%."), VOLUME(replaySe)},
    {1, N_("AUDIO OUTPUT"), N_("KS: DIRECT-TO-DRIVER AUDIO, BEST LATENCY.\nOTHER PROGRAMS MAY NOT HAVE AUDIO\nIF YOUR AUDIO DEVICE IS NOT SHAREABLE.\nWASAPI: NORMAL WINDOWS WASAPI, WORSE AUDIO LATENCY\nAUTO: TRIES KS, ELSE WASAPI."),
     settings::kAudioOutputCount, audio_out_get, audio_out_set, audio_out_value},
    {1, N_("AUDIO BUFFER"), N_("SHORTER: LESS LATENCY.\n AUTOMATICALLY ADJUSTS IF A CRACKLE IS DETECTED."),
     0, nullptr, nullptr, nullptr, buffer_text, buffer_step, buffer_edges},
    {1, N_("  RUNNING"), N_("THE OUTPUT IN USE AND ITS LATENCY\n(OUTPUT + SCHEDULING)"), 0, nullptr, nullptr, nullptr,
     audio_now_text, no_step, no_edges},
    {0, N_("GAME"), N_("THE GAME'S LANGUAGE, UNITS AND A NEW GAME'S MONEY.\nTHE RACE HUD."), kCategory},
    {1, N_("LANGUAGE"), N_("THE MENUS, TEXTS, CAR NAMES AND UNITS.\nAUTO: THE INSTALLED DISC'S OWN.\nTAKES EFFECT AT THE NEXT START."),
     0, nullptr, nullptr, nullptr, language_text, language_step, language_edges},
    {1, N_("SPEED UNIT"), N_("SPEEDS AND DISTANCES.\nAUTO: THE LANGUAGE'S OWN (US ENGLISH MPH, THE OTHERS KM/H).\nA RACE'S SPEEDOMETER DIAL: FROM THE NEXT RACE."),
     1 + settings::kSpeedUnits, speed_unit_get, speed_unit_set, speed_unit_value},
    {1, N_("POWER UNIT"), N_("ENGINE POWER.\nAUTO: THE LANGUAGE'S OWN (US ENGLISH HP, JAPANESE AND GERMAN PS,\nUK ENGLISH, ITALIAN AND SPANISH BHP, FRENCH CV)."),
     1 + settings::kPowerUnits, power_unit_get, power_unit_set, power_unit_value},
    {1, N_("TORQUE UNIT"), N_("ENGINE TORQUE.\nAUTO: THE LANGUAGE'S OWN (US ENGLISH FT.LB, THE OTHERS KGF.M)."),
     1 + settings::kTorqueUnits, torque_unit_get, torque_unit_set, torque_unit_value},
    {1, N_("STARTING CREDITS"), N_("THE MONEY A NEW GAME STARTS WITH (JAPANESE: x100, YEN).\nVERSION'S OWN: USA / EUROPE 18,000, JAPAN 1,500,000 YEN.\nFROM THE NEXT NEW GAME.\nCUSTOM: YOUR OWN AMOUNT."),
     settings::kStartingCreditsCount, credits_get, credits_set, credits_value, nullptr, nullptr, nullptr, -1, nullptr,
     credits_custom},
    /* PC (2026-11-14): with CUSTOM (settings::starting_credits_custom) */
    {2, N_("CUSTOM AMOUNT"), N_("THE MONEY A NEW GAME STARTS WITH: 0 .. 999,999,999.\nHOLD LEFT / RIGHT FOR BIGGER STEPS. ENTER: 18,000.\nTHE LAUNCHER'S GAME TAB TAKES A TYPED AMOUNT."),
     0, nullptr, nullptr, nullptr, credits_custom_text, credits_custom_step, credits_custom_edges},
    {1, N_("HUD"), N_("THE RACE HUD'S WIDTH, SIZE AND METERS. THE REPLAY LOGO.\nITS COLORS."), kCategory},
    {2, N_("RACE HUD MAX WIDTH"), N_("ADJUST SIDE-ANCHORED HUD ELEMENTS."), kHudRatioCount, hud_get,
     hud_set, hud_value},
    {2, N_("HUD SAFE ZONE"), N_("THE RACE HUD SCALED DOWN TO FIT THIS MUCH\nOF THE SCREEN. 100%: THE WHOLE SCREEN. ENTER: 100%."),
     0, nullptr, nullptr, nullptr, safe_text, safe_step, safe_edges},
    {2, N_("SINGLE PLAYER HUD"), N_("A SINGLE PLAYER'S METERS."), 2, single_hud_get, single_hud_set, hud_analog_value},
    {2, N_("SPLIT SCREEN HUD"), N_("THE SPLIT SCREEN'S METERS."), 2, split_hud_get, split_hud_set, hud_digital_value},
    {2, N_("REPLAY LOGO"), N_("THE GT LOGO OVER THE BOTTOM RIGHT CORNER\nOF THE REPLAYS AND THE DEMOS."), 2, replay_logo_get,
     replay_logo_set, on_off_value},
    {2, N_("COLORS"), N_("THE RACE HUD'S COLORS, EACH BY ITS RED, GREEN AND BLUE.\nON AN IMAGE THEY TINT IT. THE LAUNCHER HAS THEM TOO."), kCategory},
    HUD_COLOR(kHudText, N_("TEXT"), N_("THE LAPS, THE RANK, THE GEAR, THE SPEED, THE TIMES.")),
    HUD_COLOR(kHudMessages, N_("MESSAGES"), N_("THE TITLE, THE CAR'S NAME, THE START AND FINISH.")),
    HUD_COLOR(kHudWarnings, N_("WARNINGS"), N_("WRONG WAY, REVERSE, THE SUGGESTED GEAR, WORN TIRES.")),
    HUD_COLOR(kHudEvents, N_("EVENT BOARD"), N_("THE EVENT BOARD'S MESSAGES.")),
    HUD_COLOR(kHudGapAhead, N_("GAP AHEAD"), N_("A GAP ON THE EVENT BOARD, AHEAD.")),
    HUD_COLOR(kHudGapBehind, N_("GAP BEHIND"), N_("A GAP ON THE EVENT BOARD, BEHIND.")),
    HUD_COLOR(kHudOdometer, N_("ODOMETER"), N_("THE ODOMETER AND THE TRIP METER.")),
    HUD_COLOR(kHudMeterDial, N_("METER DIAL"), N_("TINTS THE METERS' DIAL IMAGES.")),
    HUD_COLOR(kHudMeterMarks, N_("METER MARKS"), N_("THE MARKS ROUND THE DIALS.")),
    HUD_COLOR(kHudMeterRed, N_("METER RED ZONE"), N_("THE MARKS OF THE RED ZONE.")),
    HUD_COLOR(kHudNeedle, N_("NEEDLE"), N_("THE METERS' NEEDLES.")),
    HUD_COLOR(kHudRevBar, N_("REV BAR"), N_("TINTS THE DIGITAL METERS' REV BAR IMAGE.")),
    HUD_COLOR(kHudShiftLamp, N_("SHIFT LAMP"), N_("THE SHIFT LAMP, LIT.")),
    HUD_COLOR(kHudVscLamp, N_("VSC LAMP"), N_("THE VSC LAMP, LIT.")),
    HUD_COLOR(kHudTcsLamp, N_("TCS LAMP"), N_("THE TCS LAMP, LIT.")),
    HUD_COLOR(kHudBrakeLamps, N_("BRAKE AND OIL LAMPS"), N_("THE HANDBRAKE AND OIL LAMPS, LIT.")),
    HUD_COLOR(kHudThrottleBar, N_("THROTTLE BAR"), N_("THE THROTTLE'S BAR.")),
    HUD_COLOR(kHudBrakeBar, N_("BRAKE BAR"), N_("THE BRAKE'S BAR.")),
    HUD_COLOR(kHudMapShown, N_("MAP: CAR SHOWN"), N_("THE CAR SHOWN, ON THE MINI MAP.")),
    HUD_COLOR(kHudMapOthers, N_("MAP: OTHER CARS"), N_("THE OTHER CARS, ON THE MINI MAP.")),
    HUD_COLOR(kHudPanels, N_("BACK PANELS"), N_("THE TRANSLUCENT PANELS BEHIND THE HUD, THE UNLIT LAMPS.")),
    {1, N_("EXTRAS"),N_("THE COURSES SCALED: THEIR MODELS, ROAD, CAMERAS...\nTHE CARS KEEP THEIR SIZE.\nRANDOM PRIZE CARS. RANDOM COURSES. CAR MULTIPLIERS.\nSURFACE GRIP AND RESISTANCE. THE REV CUT BEHAVIOR. THE GEAR SHIFT BEHAVIOR."), kCategory},
    {2, N_("COURSE SCALE X"), N_("THE COURSES ALONG THE WORLD'S X (ACROSS).\nENTER: 100%. FROM THE NEXT COURSE LOADED.\n-1000% .. 1000%; NEGATIVE: MIRRORED."),
     COURSE_SCALE(0)},
    {2, N_("COURSE SCALE Y"), N_("THE COURSES' HEIGHT (THE WORLD'S Y).\nENTER: 100%. FROM THE NEXT COURSE LOADED.\n5% .. 1000%"),
     COURSE_SCALE(1)},
    {2, N_("COURSE SCALE Z"), N_("THE COURSES ALONG THE WORLD'S Z (ACROSS).\nENTER: 100%. FROM THE NEXT COURSE LOADED.\n-1000% .. 1000%; NEGATIVE: MIRRORED."),
     COURSE_SCALE(2)},
    {2, N_("PRIZE CAR RANDOMIZER"), N_("THE PRIZE CARS ARE DRAWN FROM ALL CARS IN THE GAME'S DATABASE."),
     2, prize_random_get, prize_random_set, on_off_value},
    {2, N_("COURSE RANDOMIZER"), N_("GT MODE'S CAREER EVENTS WILL HAVE RANDOM COURSES.\nTHE LAPS GET SCALED BY THE LAP LENGTHS.\nEXTENDED: ANY COURSE OF THE DATABASE, WHATEVER ITS SURFACE."),
     settings::kCourseRandomizerCount, course_random_get, course_random_set, course_random_value},
    /* PC (2026-11-13): aftermarket wheels for every car */
    {2, N_("UNRESTRICTED WHEELS"), N_(" EVERY CAR CAN ENTER THE WHEEL SHOP AND THE GARAGE'S WHEEL CHANGE"),
     2, free_wheels_get, free_wheels_set, on_off_value},
    /* PC (2026-11-14): the cars the dealers show as not for sale */
    {2, N_("BUY CARS NOT FOR SALE"), N_("CARS NOT FOR SALE IN SHOWROOMS CAN BE BOUGHT"),
     2, not_for_sale_get, not_for_sale_set, on_off_value},
    {2, N_("CARS IN A RACE"), N_("THE AMOUNT OF CARS IN A RACE.\n IF A RACE HAS A LIMIT, IT WILL NEED TO BE OVERRIDDEN WITH THE SETTING BELOW."),
     0, nullptr, nullptr, nullptr, race_cars_text, race_cars_step, race_cars_edges},
    {2, N_("  OVERRIDE RACE LIMITS"), N_("RALLY AND WET COURSES CAN HAVE MORE CARS TOO."),
     2, race_cars_override_get, race_cars_override_set, on_off_value},
    /* PC (2026-11-08): the rev cut behavior (settings::RevCut) */
    {2, N_("REV CUT BEHAVIOR"), N_("ORIGINAL: THE FUEL IS CUT AT THE REDLINE UNTIL THE REVS DROP 500 RPM.\nREALISTIC: A SHORT CUT OF A TIME, SHORTER THE HIGHER THE REVS.\nFROM THE NEXT RACE ENTERED."),
     settings::kRevCutCount, rev_cut_get, rev_cut_set, rev_cut_value},
    /* PC (2026-11-08): the gear shift behavior (settings::ShiftBehavior) */
    {2, N_("GEAR SHIFT BEHAVIOR"), N_("ORIGINAL: A SHIFT WAITS FOR THE REVS TO REACH THE NEW GEAR'S.\nREALISTIC: THE CLUTCH IS OUT FOR A TIME (THE GEARBOX PART, THE FLYWHEEL\nAND ENGINE BRAKE), THEN SNAPS IN WHEREVER THE REVS ARE. GEAR TYPE 0.\nDOWNSHIFTS: BLIP THE THROTTLE, ELSE THE ENGINE BRAKES THE WHEELS.\nREALISTIC + CLUTCH: A MANUAL GEARBOX IS WORKED BY YOUR CLUTCH (85% IS OUT).\nFROM THE NEXT RACE ENTERED."),
     settings::kShiftBehaviorCount, shift_get, shift_set, shift_value, nullptr, nullptr, nullptr, -1, nullptr,
     shift_clutch},
    /* PC (2026-11-09): with the clutch mode (settings::h_shifter) */
    {3, N_("H-SHIFTER"), N_("THE GEARS FROM YOUR H-SHIFTER'S GATES (THE LAUNCHER: CONTROLS,\nH-SHIFTER 1 / 2); NEUTRAL FREES THE CLUTCH. A GEAR TAKEN WITHOUT\nTHE CLUTCH PAST 85% GRINDS.\nFROM THE NEXT RACE ENTERED."),
     2, hshift_get, hshift_set, on_off_value},
    {2, N_("CAR MULTIPLIERS"), N_("THE CARS' TIRE WEAR, POWER AND GRIP."), kCategory},
    {3, N_("TIRE WEAR (PLAYER)"), N_("HOW FAST THE TIRES WEAR, WHERE THE RACE HAS TIRE WEAR.\n0%: NO WEAR. ENTER: 100%."), CAR_TUNE(tireWear, false)},
    {3, N_("TIRE WEAR (ENEMIES)"), N_("HOW FAST THE TIRES WEAR, WHERE THE RACE HAS TIRE WEAR.\n0%: NO WEAR. ENTER: 100%."), CAR_TUNE(tireWear, true)},
    {3, N_("POWER (PLAYER)"), N_("THE ENGINE'S POWER AT EVERY RPM. ENTER: 100%."), CAR_TUNE(power, false)},
    {3, N_("POWER (ENEMIES)"), N_("THE ENGINE'S POWER AT EVERY RPM. ENTER: 100%."), CAR_TUNE(power, true)},
    {3, N_("GRIP (PLAYER)"), N_("THE TIRES' GRIP. ENTER: 100%."), CAR_TUNE(grip, false)},
    {3, N_("GRIP (ENEMIES)"), N_("THE TIRES' GRIP; THE COMPUTER DRIVES THE CORNERS\nAT THE GRIP IT HAS. ENTER: 100%."), CAR_TUNE(grip, true)},
    {2, N_("SURFACE GRIP"), N_("THE TIRES' GRIP ON EACH SURFACE (X THE TIRE'S GRIP; 0.01 .. 10.00).\nTHE GAME'S: ROAD 1.00, GRASS / SAND / GRAVEL 0.60, DIRT 0.80.\nEVERY CAR. FROM THE NEXT RACE ENTERED."), kCategory},
    {3, N_("TARMAC"), kSurfaceHelp[0], SURFACE(0, grip)},
    {3, N_("KERBS"), kSurfaceHelp[1], SURFACE(1, grip)},
    {3, N_("GRASS"), kSurfaceHelp[2], SURFACE(2, grip)},
    {3, N_("SAND"), kSurfaceHelp[3], SURFACE(3, grip)},
    {3, N_("GRAVEL"), kSurfaceHelp[4], SURFACE(4, grip)},
    {3, N_("DIRT"), kSurfaceHelp[5], SURFACE(5, grip)},
    {3, "SF", kSurfaceHelp[6], SURFACE(6, grip)},
    {3, "WD", kSurfaceHelp[7], SURFACE(7, grip)},
    {3, "HT", kSurfaceHelp[8], SURFACE(8, grip)},
    {3, N_("WET TARMAC"), kSurfaceHelp[9], SURFACE(9, grip)},
    {3, N_("WET KERBS"), kSurfaceHelp[10], SURFACE(10, grip)},
    {3, N_("WET HT"), kSurfaceHelp[11], SURFACE(11, grip)},
    {2, N_("SURFACE RESISTANCE"), N_("WHAT SLOWS THE CARS ON EACH SURFACE (ROLLING RESISTANCE,\n0.00 .. 327.67). THE GAME'S: ROAD 0.10, GRASS 0.20, GRAVEL 0.30,\nDIRT 0.20, SAND 25.50. EVERY CAR. FROM THE NEXT RACE ENTERED."), kCategory},
    {3, N_("TARMAC"), kSurfaceHelp[0], SURFACE(0, resistance)},
    {3, N_("KERBS"), kSurfaceHelp[1], SURFACE(1, resistance)},
    {3, N_("GRASS"), kSurfaceHelp[2], SURFACE(2, resistance)},
    {3, N_("SAND"), kSurfaceHelp[3], SURFACE(3, resistance)},
    {3, N_("GRAVEL"), kSurfaceHelp[4], SURFACE(4, resistance)},
    {3, N_("DIRT"), kSurfaceHelp[5], SURFACE(5, resistance)},
    {3, "SF", kSurfaceHelp[6], SURFACE(6, resistance)},
    {3, "WD", kSurfaceHelp[7], SURFACE(7, resistance)},
    {3, "HT", kSurfaceHelp[8], SURFACE(8, resistance)},
    {3, N_("WET TARMAC"), kSurfaceHelp[9], SURFACE(9, resistance)},
    {3, N_("WET KERBS"), kSurfaceHelp[10], SURFACE(10, resistance)},
    {3, N_("WET HT"), kSurfaceHelp[11], SURFACE(11, resistance)},
    /* PC (2026-11-14): the challenge modes (user: "F2 menu: A new Challenge modes category") */
    {0, N_("CHALLENGE MODES"), N_("ADDITIONAL GAME RULES"), kCategory},
    {1, N_("MONEY DECAY"), N_("EACH DAY PASSED TAKES CREDITS: DAYS 2-9 (DAY - 1) X 50,\nFROM DAY 10 (DAY - 9) X 500, AT MOST 200,000 A DAY.\nA LICENSE EARNED PAUSES DECAY FOR 10 DAYS (DOES NOT STACK).\nFROM THE NEXT DAY PASSED."),
     2, challenge_get<settings::Challenge::moneyDecay>, challenge_set<settings::Challenge::moneyDecay>, on_off_value},
    {1, N_("CAR DECAY"), N_("YOUR CARS' POWER 0.25% LESS FOR EACH DAY PASSED, DOWN TO 5%,\nFROM THE NEXT RACE ENTERED.\nNOT ACTIVE DURING LICENSE TESTS."),
     2, challenge_get<settings::Challenge::carDecay>, challenge_set<settings::Challenge::carDecay>, on_off_value},
    {1, N_("COURSE SHRINKAGE"), N_("COURSES GET 0.2% SMALLER ON EACH AXIS FOR EACH DAY PASSED,\nDOWN TO 25%.\nFROM THE NEXT RACE ENTERED."),
     2, challenge_get<settings::Challenge::courseShrinkage>, challenge_set<settings::Challenge::courseShrinkage>,
     on_off_value},
    /* PC (2026-11-15): the save folder in each memory card slot */
    {0, N_("MEMORY CARDS"), N_("THE SAVE FOLDER IN EACH MEMORY CARD SLOT."), kCategory},
    {1, N_("SLOT 1"), kCardHelp, 0, nullptr, nullptr, nullptr, card_text<0>, card_step<0>, card_edges<0>},
    {1, N_("SLOT 2"), kCardHelp, 0, nullptr, nullptr, nullptr, card_text<1>, card_step<1>, card_edges<1>},
    {0, N_("CONTROLS"),N_("EACH PLAYER'S DEVICE AND DEAD ZONES.\nTHE MAPPINGS: THE LAUNCHER'S CONTROLS TAB."), kCategory},
    {1, N_("PLAYER 1"), N_("PLAYER 1'S DEVICE, CONTROLLER AND WHEEL."), kCategory},
    {2, N_("DEVICE"), kDeviceHelp, 2, device_get<0>, device_set<0>, device_value},
    {2, N_("CONTROLLER"), N_("PLAYER 1'S CONTROLLER (PORT 1)."), kCategory},
    {3, N_("LEFT STICK DEAD ZONE"), kStickHelp, DEADZONE(input_map::kController1, 0)},
    {3, N_("RIGHT STICK DEAD ZONE"), kStickHelp, DEADZONE(input_map::kController1, 1)},
    {3, N_("TRIGGER DEAD ZONE"), kTriggerHelp, DEADZONE(input_map::kController1, 2)},
    {2, N_("WHEEL"), N_("PLAYER 1'S WHEEL."), kCategory},
    {3, N_("STEERING DEAD ZONE"), kSteerHelp, DEADZONE(input_map::kWheel1, 0)},
    {3, N_("GAS PEDAL DEAD ZONE"), kPedalHelp, DEADZONE(input_map::kWheel1, 1)},
    {3, N_("BRAKE PEDAL DEAD ZONE"), kPedalHelp, DEADZONE(input_map::kWheel1, 2)},
    /* PC (2026-11-10): the clutch pedal's (input_map::kWheelClutchDeadzone) */
    {3, N_("CLUTCH PEDAL DEAD ZONE"), kPedalHelp, DEADZONE(input_map::kWheel1, input_map::kWheelClutchDeadzone)},
    {1, N_("PLAYER 2"), N_("PLAYER 2'S DEVICE, CONTROLLER AND WHEEL."), kCategory},
    {2, N_("DEVICE"), kDeviceHelp, 2, device_get<1>, device_set<1>, device_value},
    {2, N_("CONTROLLER"), N_("PLAYER 2'S CONTROLLER (PORT 2)."), kCategory},
    {3, N_("LEFT STICK DEAD ZONE"), kStickHelp, DEADZONE(input_map::kController2, 0)},
    {3, N_("RIGHT STICK DEAD ZONE"), kStickHelp, DEADZONE(input_map::kController2, 1)},
    {3, N_("TRIGGER DEAD ZONE"), kTriggerHelp, DEADZONE(input_map::kController2, 2)},
    {2, N_("WHEEL"), N_("PLAYER 2'S WHEEL."), kCategory},
    {3, N_("STEERING DEAD ZONE"), kSteerHelp, DEADZONE(input_map::kWheel2, 0)},
    {3, N_("GAS PEDAL DEAD ZONE"), kPedalHelp, DEADZONE(input_map::kWheel2, 1)},
    {3, N_("BRAKE PEDAL DEAD ZONE"), kPedalHelp, DEADZONE(input_map::kWheel2, 2)},
    {3, N_("CLUTCH PEDAL DEAD ZONE"), kPedalHelp, DEADZONE(input_map::kWheel2, input_map::kWheelClutchDeadzone)},
    {0, N_("DEBUG"), N_("TESTING AIDS AND DEVELOPER CHEATS"), kCategory},
    {1, N_("FILL GARAGE"), N_("NEW GAME: GET ONE OF EVERY CAR IN THE DATABASE\nIN EVERY COLOR."),
     2, fill_garage_get, fill_garage_set, on_off_value},
    {1, N_("FILL MONEY"), N_("NEW GAME: GET CR. 100,000,000"),
     2, fill_money_get, fill_money_set, on_off_value},
    {1, N_("UNLOCK ARCADE TRACKS"), N_("EVERY COURSE IN ALL MODES OF ARCADE MODE\n ARE AVAILABLE TO RACE."),
     2, unlock_arcade_get, unlock_arcade_set, on_off_value},
    {1, N_("UNLOCK ALL LICENSES"), N_("NEW GAME: GET BRONZE ON ALL LICENSES"),
     2, unlock_licenses_get, unlock_licenses_set, on_off_value},
    {1, N_("COURSE FORCER"), N_("FORCES THE GAME TO LOAD ANY COURSE FOUND IN THE GAME FILES\n WHEN ENTERING ANY RACE."),
     2, forcer_get, forcer_set, on_off_value},
    {2, N_("COURSE"), N_("THE COURSE, BY ITS FILE NAME: ANY FILE OF THE COURSE FOLDER.\n(N CARS): ITS START PLACES; MORE CARS START ALONG ITS RACING LINE.\nENTER: THE NEXT ONE."),
     0, nullptr, nullptr, nullptr, forced_text, forced_step, forced_edges},
    {1, N_("LAP FORCER"), N_("OVERRIDES THE AMOUNT OF LAPS OF A RACE"),
     2, lap_forcer_get, lap_forcer_set, on_off_value},
    {2, N_("LAPS"), N_("# OF LAPS"),
     0, nullptr, nullptr, nullptr, laps_text, laps_step, laps_edges},
    {1, N_("RACE TIME FORCER"), N_("OVERRIDES THE TIME LIMIT OF A RACE"),
     2, race_time_forcer_get, race_time_forcer_set, on_off_value},
    {2, N_("MINUTES"), N_("THE TIME OF EVERY TIMED RACE."),
     0, nullptr, nullptr, nullptr, race_time_text, race_time_step, race_time_edges},
    {1, N_("COURSE MODE FORCER"), N_("WHAT ENDS EVERY RACE ENTERED. FINISH LINE: THE COURSE'S OWN\nFINISH LINE, WHERE THE LICENSE TESTS END (NONE: ITS LAPS).\n0-400 M, 0-1000 M, MAX SPEED: A LINE THAT FAR ALONG THE LAP\nAND NO LAP COUNT, AS THE MACHINE TESTS."),
     settings::kCourseModeCount, course_mode_get, course_mode_set, course_mode_value},
    /* PC (2026-11-12): the menus' cars with their race models */
    {1, N_("MENU RACE MODEL"), N_("THE MENUS' CARS DRAWN WITH THEIR RACE MODELS (BODY AND WHEELS),\nNOT THEIR SHOWROOM ONES. A CAR ON SHOW LOADS AGAIN.\nTHE PRIZE CARS: FROM THE NEXT PRIZE."),
     2, menu_race_model_get, menu_race_model_set, on_off_value},
    {2, N_("LEVEL OF DETAIL"), N_("THE RACE MODEL'S LEVEL OF DETAIL, FORCED. 0: THE FINEST.\nAUTO: AS THE GAME PICKS (GRAPHICS > LEVELS OF DETAIL)."),
     settings::kMenuRaceModelLodMax + 2, menu_race_lod_get, menu_race_lod_set, menu_race_lod_value},
    /* PC (2026-11-16): GT3 PC's own text's language (platform/ui_text.h), the
     * values in their own languages (never translated) */
    {0, N_("INTERFACE LANGUAGE"), N_("THE LANGUAGE OF THIS MENU, THE ESC MENU, THE PANELS AND THE LAUNCHER,\nNOT THE GAME'S (GAME > LANGUAGE). AUTO: WINDOWS' DISPLAY LANGUAGE.\nTHE LAUNCHER TAKES IT AT ITS NEXT START."),
     0, nullptr, nullptr, nullptr, ui_lang_text, ui_lang_step, ui_lang_edges},
    /* PC (2026-11-15): a screenshot (platform/screenshot.h), the last top-level row: F2, Up, Enter */
    {0, N_("TAKE SCREENSHOT"), N_("THE GAME'S PICTURE AT ITS OWN RESOLUTION, WITHOUT THIS MENU,\nAS A PNG IN THE SCREENSHOTS FOLDER (BESIDE GT3PC.INI)."),
     0, nullptr, nullptr, nullptr, shot_text, shot_step, no_edges, input_map::kHotScreenshot},
};
constexpr int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));
#undef DEADZONE
#undef VOLUME
#undef COURSE_SCALE
#undef CAR_TUNE
#undef SURFACE
#undef HUD_COLOR
#undef HUD_CHANNEL

/* ---- the tree -------------------------------------------------------- */

/* The categories open (all closed at first; kept while the game runs). */
bool g_open[kItemCount];

/* The category item i is in (-1 at the top level). */
int parent(int i)
{
    for (int j = i - 1; j >= 0; j--)
        if (kItems[j].depth < kItems[i].depth)
            return j;
    return -1;
}

/* Every category above item i is open, and every setting above it (an
 * on / off with the items one level deeper nested in it: COURSE FORCER >
 * COURSE) is on. */
bool visible(int i)
{
    for (int p = parent(i); p >= 0; p = parent(p)) {
        const Item &it = kItems[p];
        if (it.count == kCategory ? !g_open[p]
            : it.nest != nullptr  ? it.nest() == 0     /* PC (2026-11-09) */
                                  : it.get == nullptr || it.get() == 0)
            return false;
    }
    return true;
}

/* The visible items in their order (n: how many); the index of item `sel'
 * among them in *at. */
int visible_items(int *out, int sel, int *at)
{
    int n = 0;
    *at = 0;
    for (int i = 0; i < kItemCount; i++)
        if (visible(i)) {
            if (i == sel)
                *at = n;
            out[n++] = i;
        }
    return n;
}

/* The chosen setting's value moved by d (held at its ends), or to the next
 * one (wrapping) for d = 0; heldMs: how long the key has been held (0: a
 * fresh press).  On a category: Right (or the next value) opens it, Left
 * closes it (one already closed: its own category is chosen); a held key
 * does neither. */
void change(int d, int heldMs)
{
    const Item &it = kItems[g_sel];
    if (it.count == kCategory) {
        if (heldMs != 0)
            return;
        if (d == 0)
            g_open[g_sel] = !g_open[g_sel];
        else if (d > 0)
            g_open[g_sel] = true;
        else if (g_open[g_sel])
            g_open[g_sel] = false;
        else if (parent(g_sel) >= 0)
            g_sel = parent(g_sel);
        return;
    }
    if (it.count == 0) {
        it.step(d, heldMs);
        return;
    }
    const int cur = it.get();
    int v = d == 0 ? (cur + 1) % it.count : cur + d;
    v = std::max(0, std::min(it.count - 1, v));
    if (v != cur)
        it.set(v);
}

/* The value shown and whether lower / higher ones exist (bits 0 / 1). */
int item_value(const Item &it, char *out, size_t n)
{
    if (it.count == kCategory) {
        if (it.text != nullptr)
            it.text(out, n);
        else if (n > 0)
            out[0] = '\0';
        return 0;
    }
    if (it.count == 0) {
        it.text(out, n);
        return it.edges();
    }
    const int cur = it.get();
    snprintf(out, n, "%s", tr(it.value(cur)));      /* PC (2026-11-16): the interface's language */
    return (cur > 0 ? 1 : 0) | (cur < it.count - 1 ? 2 : 0);
}

/* To the next / the previous visible item (wrapping). */
void move(int d)
{
    int list[kItemCount], at;
    const int n = visible_items(list, g_sel, &at);
    g_sel = list[(at + d + n) % n];
}

/* ---- the game controllers (read at each present) --------------------- */

enum : uint16_t {
    PAD_UP = 1u << 4, PAD_RIGHT = 1u << 5, PAD_DOWN = 1u << 6, PAD_LEFT = 1u << 7,
    PAD_CIRCLE = 1u << 13, PAD_CROSS = 1u << 14
};
uint16_t g_padPrev[2];
bool g_padSync = true;                          /* the next read only takes the buttons down (just opened) */
uint16_t g_repeatButton;                        /* the D-pad direction held */
ULONGLONG g_repeatAt;
ULONGLONG g_padDownAt;                          /* when it went down */
/* the keyboard's held key (key): when it went down, its last step */
ULONGLONG g_keyDownAt, g_keyStepAt;
/* A held key or direction repeats at most this often (ms): the keyboard's
 * autorepeat (~30 a second) made the frame time's steps run away */
constexpr ULONGLONG kRepeatMs = 66;

void poll_pads()
{
    uint16_t now = 0;
    for (int port = 0; port < 2; port++) {
        gamepad::State st;
        const uint16_t b = gamepad::read(port, &st) ? st.buttons : 0;
        if (g_padSync) {
            g_padPrev[port] = b;
            continue;
        }
        const uint16_t pressed = (uint16_t)(b & ~g_padPrev[port]);
        g_padPrev[port] = b;
        now |= b;
        if (pressed & PAD_CIRCLE) {
            g_shown = false;
            return;
        }
        if (pressed & PAD_UP) move(-1);
        if (pressed & PAD_DOWN) move(1);
        if (pressed & PAD_LEFT) change(-1, 0);
        if (pressed & PAD_RIGHT) change(1, 0);
        if (pressed & PAD_CROSS) change(0, 0);
        const uint16_t dir = pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
        if (dir != 0) {
            g_repeatButton = dir;
            g_padDownAt = GetTickCount64();
            g_repeatAt = g_padDownAt + 400;
        }
    }
    g_padSync = false;
    /* a held direction repeats after 0.4 s, every kRepeatMs */
    if (g_repeatButton != 0 && (now & g_repeatButton) == g_repeatButton) {
        const ULONGLONG t = GetTickCount64();
        if (t >= g_repeatAt) {
            const int held = (int)(t - g_padDownAt);
            if (g_repeatButton & PAD_UP) move(-1);
            if (g_repeatButton & PAD_DOWN) move(1);
            if (g_repeatButton & PAD_LEFT) change(-1, held);
            if (g_repeatButton & PAD_RIGHT) change(1, held);
            g_repeatAt = t + kRepeatMs;
        }
    } else {
        g_repeatButton = 0;
    }
}

/* ---- drawing --------------------------------------------------------- */

const uint32_t kPanel = rgba(10, 12, 16, 215);
const uint32_t kFrame = rgba(120, 190, 255, 160);
const uint32_t kTitle = rgba(255, 255, 255, 255);
const uint32_t kLabel = rgba(170, 170, 170, 255);
const uint32_t kLabelSel = rgba(255, 255, 255, 255);
const uint32_t kValue = rgba(120, 190, 255, 255);
const uint32_t kHighlight = rgba(255, 255, 255, 30);
const uint32_t kHelp = rgba(200, 200, 150, 255);
const uint32_t kHint = rgba(130, 130, 130, 255);
const uint32_t kNotice = rgba(255, 200, 80, 255);

/* The help texts' most lines and longest line (cells: PC 2026-11-16, in the
 * interface's language - a Japanese character two cells). */
void help_extent(int *rows, int *cols)
{
    *rows = 1;
    *cols = 0;
    for (const Item &it : kItems) {
        char help[2048];
        snprintf(help, sizeof(help), "%s", tr(it.help));
        int n = 0;
        for (char *line = help; line != nullptr; n++) {
            char *next = strchr(line, '\n');
            if (next != nullptr)
                *next++ = '\0';
            *cols = std::max(*cols, overlay::text_cells(line));
            line = next;
        }
        *rows = std::max(*rows, n);
    }
}

/* The list's rows (more visible items scroll); the first one shown. */
constexpr int kListRows = 18;
int g_scroll;

/* (%s: the settings menu's hotkey, F2) */
const char kHint1[] = N_("UP/DOWN CHOOSE   LEFT/RIGHT CHANGE   %s%sESC CLOSE");
const char kHint2[] = N_("A GROUP: RIGHT OPENS IT, LEFT CLOSES IT");
/* (%s: the controller's names of its south, east and Guide buttons - CROSS,
 * CIRCLE, GUIDE; A, B, XBOX... - gamepad::button_names; the hotkey) */
const char kHint3[] = N_("PAD: D-PAD, %s NEXT VALUE, %s CLOSE, %s %s");

void build(int bw, int bh)
{
    /* PC (2026-11-16): the text in the interface's language (platform/ui_text.h),
     * its widths in cells (overlay::text_cells), the buffers for UTF-8 */
    char key[32], hint1[256], hint3[256];
    input_map::hotkey_text(input_map::kHotSettingsMenu, key, sizeof(key));
    snprintf(hint1, sizeof(hint1), tr(kHint1), key, key[0] != '\0' ? "/" : "");
    const gamepad::ButtonNames names = gamepad::button_names();   /* PC (2026-11-07) */
    snprintf(hint3, sizeof(hint3), tr(kHint3), tr(names.south), tr(names.east), tr(names.guide),
             key[0] != '\0' ? key : tr("CLOSE"));
    const char *const title = tr("GT3 PC SETTINGS");
    const char *const hint0 = tr("CHANGES APPLY AT ONCE, SAVED IN GT3PC.INI.");
    const char *const hint2 = tr(kHint2);
    int helpRows, helpCols;
    help_extent(&helpRows, &helpCols);
    const int cols = std::max({52, helpCols, overlay::text_cells(hint0), overlay::text_cells(hint1),
                               overlay::text_cells(hint2), overlay::text_cells(hint3)});
    const int rows = kListRows + helpRows + 8;
    /* 1080p: 3 screen pixels a font pixel; a small window fits it */
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
    overlay::text(x0, y, s, title, kTitle);
    overlay::text(x1 - overlay::text_width(key, s), y, s, key, kHint);
    y += 2.0f * lineH;

    /* the visible items, the chosen one in view; a category "+" closed, "-"
     * open, each level two characters in */
    int list[kItemCount], at;
    const int n = visible_items(list, g_sel, &at);
    g_scroll = std::max(0, std::min({g_scroll, at, n - kListRows}));
    if (at >= g_scroll + kListRows)
        g_scroll = at - kListRows + 1;
    const float listY = y;
    for (int r = 0; r < kListRows && g_scroll + r < n; r++) {
        const int i = list[g_scroll + r];
        const Item &it = kItems[i];
        const bool sel = i == g_sel;
        if (sel)
            overlay::rect(x0 - 4.0f * s, y - 3.0f * s, x1 - x0 + 8.0f * s, lineH, kHighlight);
        char label[256], hot[32] = "";
        if (it.hotkey >= 0)
            input_map::hotkey_text(it.hotkey, hot, sizeof(hot));
        if (it.count == kCategory)
            snprintf(label, sizeof(label), "%s %s", g_open[i] ? "-" : "+", tr(it.label));
        else if (hot[0] != '\0')
            snprintf(label, sizeof(label), "%s (%s)", tr(it.label), hot);
        else
            snprintf(label, sizeof(label), "%s", tr(it.label));
        overlay::text(x0 + 2.0f * cw * it.depth, y, s, label, sel ? kLabelSel : it.count == kCategory ? kTitle : kLabel);
        char value[160], v[192];
        const int edges = item_value(it, value, sizeof(value));
        if (sel && it.count != kCategory)
            snprintf(v, sizeof(v), "%s %s %s", (edges & 1) ? "<" : " ", value, (edges & 2) ? ">" : " ");
        else
            snprintf(v, sizeof(v), "%s", value);
        const float vx = x1 - overlay::text_width(v, s);
        overlay::text(vx, y, s, v, kValue);
        if (it.swatch != nullptr)
            overlay::rect(vx - 2.0f * cw, y, 1.5f * cw, 8.0f * s, it.swatch());
        y += lineH;
    }
    if (n > kListRows) {
        /* a scroll bar by the right edge */
        const float trackH = kListRows * lineH - 3.0f * s, bx = px + pw - pad * 0.5f - b;
        overlay::rect(bx, listY - 3.0f * s, b, trackH, kHighlight);
        overlay::rect(bx, listY - 3.0f * s + trackH * g_scroll / n, b, trackH * kListRows / n, kFrame);
    }
    y = listY + kListRows * lineH + lineH;
    {
        /* every line of the chosen setting's help */
        char help[2048];
        snprintf(help, sizeof(help), "%s", tr(kItems[g_sel].help));
        float hy = y;
        for (char *line = help; line != nullptr; hy += lineH) {
            char *next = strchr(line, '\n');
            if (next != nullptr)
                *next++ = '\0';
            overlay::text(x0, hy, s, line, kHelp);
            line = next;
        }
    }
    y += (float)(helpRows + 1) * lineH;
    overlay::text(x0, y, s, hint0, kHint);
    y += 2.0f * lineH;
    overlay::text(x0, y, s, hint1, kHint);
    y += lineH;
    overlay::text(x0, y, s, hint2, kHint);
    y += lineH;
    overlay::text(x0, y, s, hint3, kHint);
}

/* ---- the notice ------------------------------------------------------ */

std::mutex g_noticeLock;
std::atomic<bool> g_noticeOn{false};
char g_noticeText[1024];                        /* PC (2026-11-16): 320 before (UTF-8 translations) */
ULONGLONG g_noticeMs, g_noticeEnd;

void draw_notice(ID3D11RenderTargetView *bb)
{
    std::lock_guard<std::mutex> l(g_noticeLock);
    const ULONGLONG now = GetTickCount64();
    if (g_noticeEnd == 0)
        g_noticeEnd = now + g_noticeMs;
    if (now >= g_noticeEnd) {
        g_noticeOn = false;
        return;
    }
    char text[sizeof(g_noticeText)];
    snprintf(text, sizeof(text), "%s", g_noticeText);
    const char *lines[4];
    int n = 0, cols = 0;
    for (char *p = text; p != nullptr && n < 4;) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (nl != nullptr)
            *nl++ = '\0';
        cols = std::max(cols, overlay::text_cells(p));     /* PC (2026-11-16): cells, UTF-8 */
        p = nl;
    }
    if (!overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    const int s = std::max(1, std::min(bh / 480, (bw - 16) / (cols * 6 + 20)));
    const float lineH = 12.0f * s, pad = 8.0f * s;
    const float pw = cols * 6.0f * s + 2.0f * pad, ph = n * lineH + 2.0f * pad - 3.0f * s;
    const float px = std::floor(((float)bw - pw) * 0.5f), py = (float)(16 * s);
    const float b = (float)std::max(1, s / 2);
    overlay::rect(px, py, pw, ph, kPanel);
    overlay::rect(px, py, pw, b, kNotice);
    overlay::rect(px, py + ph - b, pw, b, kNotice);
    overlay::rect(px, py, b, ph, kNotice);
    overlay::rect(px + pw - b, py, b, ph, kNotice);
    for (int i = 0; i < n; i++)
        overlay::text(px + pad, py + pad + i * lineH, s, lines[i], i == 0 ? kNotice : kTitle);
    overlay::end(bb);
}

} // namespace

void toggle()
{
    std::lock_guard<std::mutex> l(g_lock);
    g_padSync = true;
    g_repeatButton = 0;
    g_shown = !g_shown.load();
}

void hide()
{
    std::lock_guard<std::mutex> l(g_lock);
    g_shown = false;
}

bool shown()
{
    return g_shown.load(std::memory_order_relaxed);
}

void key(int vk, bool repeat)
{
    std::lock_guard<std::mutex> l(g_lock);
    /* a held key: its autorepeat taken at most every kRepeatMs, and how long
     * it has been held (the frame time's steps grow with it) */
    const ULONGLONG t = GetTickCount64();
    if (!repeat) {
        g_keyDownAt = t;
    } else {
        if (t - g_keyStepAt < kRepeatMs)
            return;
    }
    g_keyStepAt = t;
    const int held = repeat ? (int)(t - g_keyDownAt) : 0;
    switch (vk) {
    case VK_UP: move(-1); break;
    case VK_DOWN: move(1); break;
    case VK_LEFT: change(-1, held); break;
    case VK_RIGHT: change(1, held); break;
    case VK_RETURN:
    case VK_SPACE: change(0, 0); break;
    case VK_ESCAPE:
    case VK_BACK: g_shown = false; break;
    }
}

void present(ID3D11RenderTargetView *bb)
{
    if (bb == nullptr)
        return;
    if (!g_shown.load(std::memory_order_relaxed)) {
        if (g_noticeOn.load(std::memory_order_relaxed))
            draw_notice(bb);
        return;
    }
    std::lock_guard<std::mutex> l(g_lock);
    poll_pads();
    if (!g_shown.load() || !overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    build(bw, bh);
    overlay::end(bb);
    /* PC (2026-11-09): a notice over the menu too (a setting's: the clutch
     * mode's mapping) */
    if (g_noticeOn.load(std::memory_order_relaxed))
        draw_notice(bb);
}

void notice(const char *text, double seconds)
{
    std::lock_guard<std::mutex> l(g_noticeLock);
    snprintf(g_noticeText, sizeof(g_noticeText), "%s", text);
    g_noticeMs = (ULONGLONG)(seconds * 1000.0);
    g_noticeEnd = 0;                            /* from its first frame */
    g_noticeOn = true;
}

} // namespace settings_menu
