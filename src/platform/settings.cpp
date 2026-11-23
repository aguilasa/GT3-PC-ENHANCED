/* settings.cpp - the settings of GT3 PC (settings.h). */
#include "platform/settings.h"
#include "platform/hud_colors.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): [Interface] Language */

#include "platform/os/win_compat.h"

#include <atomic>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

namespace settings {
namespace {

const char kSection[] = "Display";
const char kGraphics[] = "Graphics";
Display g_display;
std::atomic<int> g_aa{(int)AntiAliasing::off};
std::atomic<int> g_renderScale{100};
std::atomic<bool> g_lod{true};
std::atomic<int> g_reflDetail{(int)ReflectionDetail::original};    /* PC: [Graphics] ReflectionDetail */
/* PC (2026-11-14): [Graphics] ReflectionResolution */
std::atomic<int> g_reflResolution{(int)ReflectionResolution::normal};
const char *const kReflectionResolutionNames[kReflectionResolutionCount] = {"normal", "half", "full"};
/* PC (2026-11-15): [Display] MenuAspect, 16:9 added (was a bool, stretch or not) */
std::atomic<int> g_menuAspect{(int)MenuAspect::original};
const char *const kMenuAspectNames[kMenuAspectCount] = {"original", "16:9", "stretch"};
const char *const kMenuAspectLog[kMenuAspectCount] = {"at 4:3", "at 16:9, stretched", "stretched over the window"};
std::atomic<bool> g_movieStretch{false};
std::atomic<bool> g_rearView[kRearViewCameras] = {{true}, {false}};     /* bumper, chase */
const char *const kRearViewKeys[kRearViewCameras] = {"RearViewBumper", "RearViewChase"};
std::atomic<bool> g_extraCameras{false};
std::atomic<int> g_chaseAngle{0};
std::atomic<bool> g_cockpitCamera{false};   /* PC (2026-11-07): [Graphics] CockpitCamera, CockpitFov */
std::atomic<int> g_cockpitFov{kCockpitFovDefault};
std::atomic<bool> g_menuWheels{false};          /* PC (2026-11-12): [Graphics] MenuWheels */
std::atomic<int>  g_menuWheelsSpeed{20};        /* PC (2026-11-12): [Graphics] MenuWheelsSpeed, km/h */
std::atomic<bool> g_menuTires{false};           /* PC (2026-11-13): [Graphics] MenuTires */
std::atomic<int> g_renderDistance{100};
std::atomic<double> g_frameCap{0.0};
std::atomic<int> g_vsync{0};
std::atomic<int> g_fov{100};
std::atomic<int> g_replayFov{100};
std::atomic<int> g_splitScreen{(int)SplitScreen::horizontal};
const char *const kSplitNames[kSplitScreenCount] = {"horizontal", "vertical", "monitors"};
std::atomic<bool> g_splitHudAnalog{false};
std::atomic<bool> g_singleHudDigital{false};
std::atomic<int> g_hudSafeZone{100};
std::atomic<bool> g_replayLogo{true};
std::atomic<uint32_t> g_hudColors[kHudColorCount];
std::atomic<int> g_smokeOpacity{(int)SmokeOpacity::normal};
const char *const kSmokeOpacityNames[kSmokeOpacityCount] = {"normal", "reduced", "half"};
const int kSmokeOpacityPercents[kSmokeOpacityCount] = {100, 75, 50};
/* PC (2026-11-05): [Graphics] CarReflections */
std::atomic<int> g_carReflections{(int)CarReflections::all};
const char *const kCarReflectionsNames[kCarReflectionsCount] = {"all", "large", "none"};
const int kCarReflectionsMinPx[kCarReflectionsCount] = {0, 24, 1 << 20};
/* PC (2026-11-10): [Graphics] AnisotropicFiltering: 1 off, 2 .. 16 */
std::atomic<int> g_aniso{1};
/* PC (2026-11-06): [Graphics] EnvMapDetail */
std::atomic<int> g_envMapDetail{(int)EnvMapDetail::scaled};
const char *const kEnvMapDetailNames[kEnvMapDetailCount] = {"scaled", "original"};
/* PC (2026-11-07): [Graphics] ReplayDepthOfField */
std::atomic<int> g_replayDof{(int)ReplayDepthOfField::original};
const char *const kReplayDofNames[kReplayDepthOfFieldCount] = {"original", "reduced", "off"};

int clamp_safe_zone(int p)
{
    return p < kHudSafeZoneMin ? kHudSafeZoneMin : p > kHudSafeZoneMax ? kHudSafeZoneMax : p;
}

int clamp_fov(int p)
{
    return p < kFovMin ? kFovMin : p > kFovMax ? kFovMax : p;
}

int clamp_render_scale(int p)
{
    return p < kRenderScaleMin ? kRenderScaleMin : p > kRenderScaleMax ? kRenderScaleMax : p;
}

double clamp_cap(double ms)
{
    if (!(ms > 0.0))
        return 0.0;
    if (ms < kFrameCapMin)
        ms = kFrameCapMin;
    if (ms > kFrameCapMax)
        ms = kFrameCapMax;
    return (double)(long long)(ms * 1000.0 + 0.5) / 1000.0;
}
const char *const kAaNames[kAntiAliasingCount] = {"off",   "fxaa",  "msaa2", "msaa4",
                                                  "msaa8", "agss2", "agss4", "agss8"};     /* (PC 2026-11-15: AGSS) */

const char kAudio[] = "Audio";
const char *const kAudioNames[kAudioOutputCount] = {"auto", "ks", "wasapi"};
std::atomic<int> g_audioOut{(int)AudioOutput::wasapi};     /* (PC 2026-11-15: the default; was automatic) */
std::atomic<double> g_audioBuffer{2.0};
std::atomic<int> g_audioGen{0};

double clamp_buffer(double ms)
{
    if (!(ms >= kAudioBufferMin))
        ms = kAudioBufferMin;
    if (ms > kAudioBufferMax)
        ms = kAudioBufferMax;
    return (double)(long long)(ms * 10.0 + 0.5) / 10.0;
}
/* the volumes ([Audio] MenuBgm .. ReplaySe, percent) */
const char *const kVolumeKeys[kVolumeCount] = {"MenuBgm", "RaceBgm", "ReplayBgm", "MenuSe", "RaceSe", "ReplaySe"};
const int kVolumeDefaults[kVolumeCount] = {100, 50, 50, 100, 100, 100};
std::atomic<int> g_volume[kVolumeCount] = {{100}, {50}, {50}, {100}, {100}, {100}};

int clamp_volume(int percent)
{
    return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}
const char kGame[] = "Game";
constexpr int kLanguages = 7;
const char *const kLanguageNames[kLanguages] = {"japanese", "english_us", "english_uk", "french",
                                                "german",   "italian",    "spanish"};
std::atomic<int> g_language{kLanguageAuto};
std::atomic<int> g_interfaceLanguage{ui_text::kAuto};  /* PC (2026-11-16): [Interface] Language */

CarKind g_carKinds[32];
int g_carKindCount;
bool g_debugConsole;
bool g_debugFileLog;
bool g_debugSyncLog;
bool g_debugReplaySync;
std::atomic<bool> g_debugCameraYLook{false};
std::atomic<bool> g_debugCameraGround{false};
int g_physicsThreads;                           /* [Debug] PhysicsThreads */
std::atomic<bool> g_debugFillGarage{false}, g_debugFillMoney{false}, g_debugUnlockArcade{false},
    g_debugUnlockLicenses{false};       /* (the settings menu's DEBUG sets them) */
std::atomic<bool> g_courseForcer{false};        /* [Debug] CourseForcer */
std::mutex g_courseLock;                        /* the forced course, the choices */
std::string g_forcedCourse;                     /* [Debug] ForcedCourse: a course's label */
std::vector<CourseChoice> g_courseChoices;      /* the game's (set_course_choices) */
std::atomic<bool> g_lapForcer{false};           /* [Debug] LapForcer */
std::atomic<int> g_forcedLaps{3};               /* [Debug] ForcedLaps */
std::atomic<bool> g_raceTimeForcer{false};      /* [Debug] RaceTimeForcer */
std::atomic<int> g_forcedRaceTime{10};          /* [Debug] ForcedRaceTime, minutes */
/* PC (2026-11-07): [Debug] CourseModeForcer */
std::atomic<int> g_courseMode{(int)CourseMode::off};
const char *const kCourseModeNames[kCourseModeCount] = {"off", "finish", "400", "1000", "max"};
/* PC (2026-11-12): [Debug] MenuRaceModel / MenuRaceModelLod (-1 auto) */
std::atomic<bool> g_menuRaceModel{false};
std::atomic<int> g_menuRaceModelLod{-1};
/* PC (2026-11-13): [Textures] (the folders: read once, at load) */
const char kTextures[] = "Textures";
std::atomic<bool> g_texReplace{true}, g_texPrecache{true}, g_texCompress{true};
std::atomic<int> g_texDump{(int)TextureDump::off};
const char *const kTextureDumpNames[kTextureDumpCount] = {"off", "select", "all"};
std::vector<std::string> g_texPacks;
std::atomic<int> g_startingCredits{0};          /* the mode (StartingCredits; 0: auto) */
std::atomic<int> g_startingCreditsCustom{kStartingCreditsCustomDefault};    /* PC (2026-11-14): custom's amount */
std::atomic<int> g_courseScale[3] = {100, 100, 100};   /* [Game] CourseScaleX / Y / Z, percent */
const char *const kCourseScaleKeys[3] = {"CourseScaleX", "CourseScaleY", "CourseScaleZ"};
std::atomic<bool> g_prizeCarRandomizer{false};  /* [Game] PrizeCarRandomizer */
std::atomic<bool> g_unrestrictedWheels{false};  /* PC (2026-11-13): [Game] UnrestrictedWheels */
std::atomic<bool> g_buyNotForSale{false};       /* PC (2026-11-14): [Game] BuyNotForSale */
/* PC (2026-11-14): [Challenges] MoneyDecay, CarDecay, CourseShrinkage */
const char kChallenges[] = "Challenges";
const char *const kChallengeKeys[kChallengeCount] = {"MoneyDecay", "CarDecay", "CourseShrinkage"};
const char *const kChallengeNames[kChallengeCount] = {"money decay", "car decay", "course shrinkage"};
std::atomic<bool> g_challenge[kChallengeCount] = {{false}, {false}, {false}};
std::atomic<int> g_courseRandomizer{(int)CourseRandomizer::off};    /* [Game] CourseRandomizer */
const char *const kCourseRandomizerNames[kCourseRandomizerCount] = {"off", "on", "extended"};    /* PC (2026-11-07) */
std::atomic<int> g_revCut{(int)RevCut::original};       /* PC (2026-11-08) [Game] RevCut */
const char *const kRevCutNames[kRevCutCount] = {"original", "realistic"};
std::atomic<int> g_shiftBehavior{(int)ShiftBehavior::original};    /* PC (2026-11-08) [Game] ShiftBehavior */
const char *const kShiftBehaviorNames[kShiftBehaviorCount] = {"original", "realistic", "clutch"};
std::atomic<bool> g_hShifter{false};            /* PC (2026-11-09) [Game] HShifter */
std::atomic<int> g_raceCars{6};                 /* [Game] RaceCars */
std::atomic<bool> g_raceCarsOverride{false};    /* [Game] RaceCarsOverride */
/* [Game] TireWearPlayer .. GripEnemies, percent: [tune][0 the players', 1 the
 * enemies'] */
std::atomic<int> g_carTune[kCarTunes][2] = {{{100}, {100}}, {{100}, {100}}, {{100}, {100}}};
const char *const kCarTuneKeys[kCarTunes][2] = {{"TireWearPlayer", "TireWearEnemies"},
                                                {"PowerPlayer", "PowerEnemies"},
                                                {"GripPlayer", "GripEnemies"}};

int clamp_car_tune(CarTune t, int p)
{
    const int lo = car_tune_min(t);
    return p < lo ? lo : p > kCarTuneMax ? kCarTuneMax : p;
}

/* [Game] SurfaceGrip<key> / SurfaceResistance<key>, hundredths or
 * kSurfaceOwn: [surface][0 grip, 1 resistance].  The surfaces: the codes
 * the courses use (6, WT, none; 10, 11, 15 no surface), by the tyre
 * table's column names (ForceVol<key>).  The game's own: the US tyre
 * table's one row (TIREFORCEVOL "n"), DynParams.rollingResistance (T325;
 * retail's, not ENH_PHYSICS_DYNPARAMS's). */
const char *const kSurfaceKeys[kSurfaces] = {"TA", "GU", "GR", "SA", "GV", "DT", "SF", "WD", "HT", "WR", "WH", "WG"};
const int kSurfaceCodes[kSurfaces] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 12, 13, 14};
const int kSurfaceDefaults[kSurfaces][2] = {{100, 10}, {90, 10}, {60, 20}, {60, 2550}, {60, 30}, {80, 20},
                                            {100, 10}, {100, 10}, {110, 10}, {80, 10}, {80, 10}, {80, 10}};
std::atomic<int> g_surfaceTune[kSurfaces][2];

std::string surface_tune_key(int s, SurfaceTune t)
{
    return std::string(t == SurfaceTune::grip ? "SurfaceGrip" : "SurfaceResistance") + kSurfaceKeys[s];
}

int clamp_surface_tune(SurfaceTune t, int v)
{
    if (v < 0)
        return kSurfaceOwn;
    const int lo = surface_tune_min(t), hi = surface_tune_max(t);
    return v < lo ? lo : v > hi ? hi : v;
}

std::string ini_path();

/* an ini value: the raw value ("0.60"), or missing / "auto": the own */
int read_surface_tune(const std::string &key, SurfaceTune t)
{
    char v[32] = "";
    GetPrivateProfileStringA(kGame, key.c_str(), "", v, sizeof(v), ini_path().c_str());
    char *end = nullptr;
    const double d = strtod(v, &end);
    if (v[0] == '\0' || end == v || _stricmp(v, "auto") == 0)
        return kSurfaceOwn;
    return clamp_surface_tune(t, (int)floor(d * 100.0 + 0.5));
}

} // namespace

int clamp_course_scale_recorded(int v)
{
    v = v < kCourseScaleMin ? kCourseScaleMin : v > kCourseScaleMax ? kCourseScaleMax : v;
    if (v > -kCourseScaleNearest && v < kCourseScaleNearest)
        v = v < 0 ? -kCourseScaleNearest : kCourseScaleNearest;
    return v;
}

int course_scale_min(int axis)
{
    return axis == 1 ? kCourseScaleNearest : kCourseScaleMin;    /* PC (2026-11-07): the height's floor */
}

int clamp_course_scale(int axis, int v)
{
    v = clamp_course_scale_recorded(v);
    const int least = course_scale_min(axis);
    return v < least ? least : v;
}

namespace {
/* the units: [Game] SpeedUnit / PowerUnit / TorqueUnit (kUnitAuto or the value) */
const char *const kSpeedUnitNames[kSpeedUnits] = {"kmh", "mph"};
const char *const kPowerUnitNames[kPowerUnits] = {"ps", "hp", "bhp", "cv", "kw"};
const char *const kTorqueUnitNames[kTorqueUnits] = {"kgfm", "ftlb", "nm"};
std::atomic<int> g_speedUnit{kUnitAuto}, g_powerUnit{kUnitAuto}, g_torqueUnit{kUnitAuto};
std::atomic<int> g_unitsGen{0};

std::string ini_path();

int read_unit(const char *key, const char *const *names, int count)
{
    char v[16] = "";
    GetPrivateProfileStringA(kGame, key, "auto", v, sizeof(v), ini_path().c_str());
    for (int i = 0; i < count; i++)
        if (_stricmp(v, names[i]) == 0)
            return i;
    return kUnitAuto;
}

void write_unit(std::atomic<int> &cur, int u, const char *key, const char *const *names, int count)
{
    if (u < kUnitAuto || u >= count || u == cur.load())
        return;
    cur = u;
    g_unitsGen++;
    WritePrivateProfileStringA(kGame, key, u == kUnitAuto ? "auto" : names[u], ini_path().c_str());
    pclog::write("settings: %s %s", key, u == kUnitAuto ? "auto" : names[u]);
}

std::string ini_path()
{
    return paths::ini_path();               /* PC (2026-11-14): the shared one with --shared (paths.h) */
}

/* s without the spaces and tabs at both ends, into dst (size n) */
void copy_trimmed(char *dst, size_t n, const char *s, size_t len)
{
    while (len > 0 && (*s == ' ' || *s == '\t')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (len >= n)
        len = n - 1;
    memcpy(dst, s, len);
    dst[len] = '\0';
}

/* [CarKinds]: "code=kind" entries (comments and entries without '='
 * skipped) */
void load_car_kinds()
{
    char buf[4096];
    DWORD got = GetPrivateProfileSectionA("CarKinds", buf, sizeof(buf), ini_path().c_str());
    g_carKindCount = 0;
    for (const char *p = buf; p < buf + got && *p; p += strlen(p) + 1) {
        const char *eq = strchr(p, '=');
        if (*p == ';' || *p == '#' || eq == nullptr)
            continue;
        if (g_carKindCount == (int)(sizeof(g_carKinds) / sizeof(g_carKinds[0])))
            break;
        CarKind &ck = g_carKinds[g_carKindCount];
        copy_trimmed(ck.code, sizeof(ck.code), p, (size_t)(eq - p));
        copy_trimmed(ck.kind, sizeof(ck.kind), eq + 1, strlen(eq + 1));
        if (ck.code[0] == '\0' || ck.kind[0] == '\0')
            continue;
        pclog::write("settings: car kind %s = %s", ck.code, ck.kind);
        g_carKindCount++;
    }
}

int read_int(const char *key, int def)
{
    return (int)GetPrivateProfileIntA(kSection, key, def, ini_path().c_str());
}

void write_int(const char *key, int v)
{
    char s[16];
    snprintf(s, sizeof(s), "%d", v);
    WritePrivateProfileStringA(kSection, key, s, ini_path().c_str());
}

} // namespace

const char *mode_name(DisplayMode m)
{
    switch (m) {
    case DisplayMode::borderless: return "borderless";
    case DisplayMode::fullscreen: return "fullscreen";
    default: return "windowed";
    }
}

void load()
{
    Display d;
    char mode[32] = "";
    GetPrivateProfileStringA(kSection, "Mode", "windowed", mode, sizeof(mode), ini_path().c_str());
    if (_stricmp(mode, "borderless") == 0)
        d.mode = DisplayMode::borderless;
    else if (_stricmp(mode, "fullscreen") == 0)
        d.mode = DisplayMode::fullscreen;
    d.width = read_int("Width", 0);
    d.height = read_int("Height", 0);
    if (d.width < 640 || d.height < 480)
        d.width = d.height = 0;
    d.refresh = read_int("Refresh", 0);
    d.windowW = read_int("WindowWidth", 1280);
    d.windowH = read_int("WindowHeight", 960);
    if (d.windowW < 320 || d.windowH < 240) {
        d.windowW = 1280;
        d.windowH = 960;
    }
    d.windowX = read_int("WindowX", INT_MIN);
    d.windowY = read_int("WindowY", INT_MIN);
    d.windowPlaced = d.windowX != INT_MIN && d.windowY != INT_MIN;
    {
        /* "W:H" or a number */
        char hud[32] = "";
        GetPrivateProfileStringA(kSection, "HudMaxAspect", "", hud, sizeof(hud), ini_path().c_str());
        float w = 0.0f, h = 0.0f;
        if (sscanf(hud, "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
            d.hudMaxAspect = w / h;
        else if (sscanf(hud, "%f", &w) == 1 && w > 0.0f)
            d.hudMaxAspect = w;
        if (d.hudMaxAspect < 4.0f / 3.0f)
            d.hudMaxAspect = 4.0f / 3.0f;
        if (d.hudMaxAspect > 32.0f / 9.0f)
            d.hudMaxAspect = 32.0f / 9.0f;
    }
    GetPrivateProfileStringA(kSection, "SpanMonitors", "", d.spanMonitors, sizeof(d.spanMonitors),
                             ini_path().c_str());
    GetPrivateProfileStringA(kSection, "SplitMonitor", "auto", d.splitMonitor, sizeof(d.splitMonitor),
                             ini_path().c_str());
    if (_stricmp(d.splitMonitor, "auto") == 0)
        d.splitMonitor[0] = 0;
    {
        char menu[16] = "";
        GetPrivateProfileStringA(kSection, "MenuAspect", "original", menu, sizeof(menu), ini_path().c_str());
        int ma = (int)MenuAspect::original;
        for (int i = 0; i < kMenuAspectCount; i++)
            if (_stricmp(menu, kMenuAspectNames[i]) == 0)
                ma = i;
        g_menuAspect = ma;
        pclog::write("settings: menus %s", kMenuAspectLog[ma]);
        GetPrivateProfileStringA(kSection, "MovieAspect", "original", menu, sizeof(menu), ini_path().c_str());
        g_movieStretch = _stricmp(menu, "stretch") == 0;
        pclog::write("settings: movies %s", g_movieStretch.load() ? "stretched over their place" : "in their own shape");
    }
    {
        char lang[32] = "";
        GetPrivateProfileStringA(kGame, "Language", "auto", lang, sizeof(lang), ini_path().c_str());
        int v = kLanguageAuto;
        for (int i = 0; i < kLanguages; i++)
            if (_stricmp(lang, kLanguageNames[i]) == 0)
                v = i;
        g_language = v;
        pclog::write("settings: language %s", language_name(v));
    }
    {
        /* PC (2026-11-16): GT3 PC's own text (platform/ui_text.h) */
        char lang[32] = "";
        GetPrivateProfileStringA("Interface", "Language", "auto", lang, sizeof(lang), ini_path().c_str());
        const int v = ui_text::from_value(lang);
        g_interfaceLanguage = v;
        ui_text::set_language(ui_text::resolve(v));
        pclog::write("settings: interface language %s (%s)", ui_text::value(v), ui_text::code(ui_text::language()));
    }
    g_display = d;
    pclog::write("settings: display %s, fullscreen %dx%d %d Hz (0: the desktop's / the nearest to 60), window"
                 " %dx%d, race HUD up to %.3f:1", mode_name(d.mode), d.width, d.height, d.refresh, d.windowW,
                 d.windowH, d.hudMaxAspect);
    if (d.spanMonitors[0] != 0)
        pclog::write("settings: span monitors %s (borderless / fullscreen)", d.spanMonitors);
    if (d.splitMonitor[0] != 0)
        pclog::write("settings: split screen second monitor %s", d.splitMonitor);
    {
        char aa[32] = "";
        GetPrivateProfileStringA(kGraphics, "AntiAliasing", "off", aa, sizeof(aa), ini_path().c_str());
        int v = (int)AntiAliasing::off;
        for (int i = 0; i < kAntiAliasingCount; i++)
            if (_stricmp(aa, kAaNames[i]) == 0)
                v = i;
        g_aa = v;
        pclog::write("settings: anti-aliasing %s", kAaNames[v]);
        g_renderScale = clamp_render_scale((int)GetPrivateProfileIntA(kGraphics, "RenderScale", 100,
                                                                      ini_path().c_str()));
        if (g_renderScale.load() != 100)
            pclog::write("settings: resolution scale %d%%", g_renderScale.load());
    }
    {
        char lod[16] = "";
        GetPrivateProfileStringA(kGraphics, "Lod", "on", lod, sizeof(lod), ini_path().c_str());
        g_lod = !(_stricmp(lod, "off") == 0 || strcmp(lod, "0") == 0);
        int rd = (int)GetPrivateProfileIntA(kGraphics, "RenderDistance", 100, ini_path().c_str());
        g_renderDistance = rd < kRenderDistanceMin ? kRenderDistanceMin : rd > kRenderDistanceMax ? kRenderDistanceMax : rd;
        pclog::write("settings: levels of detail %s, render distance %d%%", g_lod.load() ? "on" : "off",
                     g_renderDistance.load());
        char refl[16] = "";
        GetPrivateProfileStringA(kGraphics, "ReflectionDetail", "original", refl, sizeof(refl), ini_path().c_str());
        g_reflDetail = (int)(_stricmp(refl, "full") == 0 ? ReflectionDetail::full : ReflectionDetail::original);
        pclog::write("settings: road reflection detail %s", g_reflDetail.load() ? "full" : "original");
        /* PC (2026-11-14) */
        GetPrivateProfileStringA(kGraphics, "ReflectionResolution", "normal", refl, sizeof(refl), ini_path().c_str());
        g_reflResolution = (int)ReflectionResolution::normal;
        for (int r = 0; r < kReflectionResolutionCount; r++)
            if (_stricmp(refl, kReflectionResolutionNames[r]) == 0)
                g_reflResolution = r;
        if (g_reflResolution.load() != (int)ReflectionResolution::normal)
            pclog::write("settings: road reflection resolution %s", kReflectionResolutionNames[g_reflResolution.load()]);
        for (int c = 0; c < kRearViewCameras; c++) {
            char on[16] = "";
            GetPrivateProfileStringA(kGraphics, kRearViewKeys[c], c == 0 ? "on" : "off", on, sizeof(on),
                                     ini_path().c_str());
            g_rearView[c] = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        }
        pclog::write("settings: rear view: bumper camera %s, chase camera %s", g_rearView[0].load() ? "on" : "off",
                     g_rearView[1].load() ? "on" : "off");
        char on[16] = "";
        GetPrivateProfileStringA(kGraphics, "ExtraCameras", "off", on, sizeof(on), ini_path().c_str());
        g_extraCameras = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        char angle[16] = "";
        GetPrivateProfileStringA(kGraphics, "ChaseAngle", "0", angle, sizeof(angle), ini_path().c_str());
        const int a = atoi(angle);
        g_chaseAngle = a < kChaseAngleMin ? kChaseAngleMin : a > kChaseAngleMax ? kChaseAngleMax : a;
        if (g_extraCameras.load() || g_chaseAngle.load() != 0)
            pclog::write("settings: extra cameras %s, chase camera angle %d", g_extraCameras.load() ? "on" : "off",
                         g_chaseAngle.load());
        char cockpit[16] = "", fov[16] = "";
        GetPrivateProfileStringA(kGraphics, "CockpitCamera", "off", cockpit, sizeof(cockpit), ini_path().c_str());
        g_cockpitCamera = _stricmp(cockpit, "on") == 0 || strcmp(cockpit, "1") == 0;
        GetPrivateProfileStringA(kGraphics, "CockpitFov", "90", fov, sizeof(fov), ini_path().c_str());
        const int f = atoi(fov);
        g_cockpitFov = f < kCockpitFovMin ? kCockpitFovMin : f > kCockpitFovMax ? kCockpitFovMax : f;
        /* (PC 2026-11-15: the menu model is per car model now, the
         * calibrator's: [CockpitMenuModels], cockpit_menu_model below) */
        if (g_cockpitCamera.load())
            pclog::write("settings: cockpit camera on, field of view %d", g_cockpitFov.load());
        /* PC (2026-11-12): [Graphics] MenuWheels / MenuWheelsSpeed */
        char wheels[16] = "", wheelSpeed[16] = "";
        GetPrivateProfileStringA(kGraphics, "MenuWheels", "off", wheels, sizeof(wheels), ini_path().c_str());
        g_menuWheels = _stricmp(wheels, "on") == 0 || strcmp(wheels, "1") == 0;
        GetPrivateProfileStringA(kGraphics, "MenuWheelsSpeed", "20", wheelSpeed, sizeof(wheelSpeed),
                                 ini_path().c_str());
        const int ws = atoi(wheelSpeed);
        g_menuWheelsSpeed = ws < kMenuWheelsSpeedMin ? kMenuWheelsSpeedMin : ws > kMenuWheelsSpeedMax ? kMenuWheelsSpeedMax : ws;
        if (g_menuWheels.load())
            pclog::write("settings: menu wheels on, the race wheels from %d km/h", g_menuWheelsSpeed.load());
        /* PC (2026-11-13): [Graphics] MenuTires */
        char tires[16] = "";
        GetPrivateProfileStringA(kGraphics, "MenuTires", "off", tires, sizeof(tires), ini_path().c_str());
        g_menuTires = _stricmp(tires, "on") == 0 || strcmp(tires, "1") == 0;
        if (g_menuTires.load())
            pclog::write("settings: menu tires on");
    }
    {
        char cap[32] = "", vs[16] = "";
        GetPrivateProfileStringA(kGraphics, "FrameCap", "0", cap, sizeof(cap), ini_path().c_str());
        g_frameCap = clamp_cap(atof(cap));
        GetPrivateProfileStringA(kGraphics, "VSync", "off", vs, sizeof(vs), ini_path().c_str());
        const int n = atoi(vs);
        g_vsync = n >= 1 && n <= 3 ? n : 0;
        if (g_frameCap.load() > 0.0)
            pclog::write("settings: frame cap %.3f ms (%.2f frames a second), vsync %s", g_frameCap.load(),
                         1000.0 / g_frameCap.load(), g_vsync.load() ? vs : "off");
        else
            pclog::write("settings: no frame cap, vsync %s", g_vsync.load() ? vs : "off");
        g_fov = clamp_fov((int)GetPrivateProfileIntA(kGraphics, "Fov", 100, ini_path().c_str()));
        g_replayFov = clamp_fov((int)GetPrivateProfileIntA(kGraphics, "ReplayFov", 100, ini_path().c_str()));
        if (g_fov.load() != 100 || g_replayFov.load() != 100)
            pclog::write("settings: field of view %d%%, replays %d%%", g_fov.load(), g_replayFov.load());
        char split[16] = "";
        GetPrivateProfileStringA(kGraphics, "SplitScreen", "horizontal", split, sizeof(split), ini_path().c_str());
        int sv = (int)SplitScreen::horizontal;
        for (int i = 0; i < kSplitScreenCount; i++)
            if (_stricmp(split, kSplitNames[i]) == 0)
                sv = i;
        g_splitScreen = sv;
        if (sv != (int)SplitScreen::horizontal)
            pclog::write("settings: split screen %s", kSplitNames[sv]);
        char hud[16] = "";
        GetPrivateProfileStringA(kGraphics, "SplitHud", "digital", hud, sizeof(hud), ini_path().c_str());
        g_splitHudAnalog = _stricmp(hud, "analog") == 0;
        GetPrivateProfileStringA(kGraphics, "SingleHud", "analog", hud, sizeof(hud), ini_path().c_str());
        g_singleHudDigital = _stricmp(hud, "digital") == 0;
        if (g_splitHudAnalog.load() || g_singleHudDigital.load())
            pclog::write("settings: split screen HUD %s, single player HUD %s",
                         g_splitHudAnalog.load() ? "analog" : "digital", g_singleHudDigital.load() ? "digital" : "analog");
        g_hudSafeZone = clamp_safe_zone((int)GetPrivateProfileIntA(kGraphics, "HudSafeZone", 100, ini_path().c_str()));
        if (g_hudSafeZone.load() != 100)
            pclog::write("settings: HUD safe zone %d%%", g_hudSafeZone.load());
        char logo[16] = "";
        GetPrivateProfileStringA(kGraphics, "ReplayLogo", "on", logo, sizeof(logo), ini_path().c_str());
        g_replayLogo = !(_stricmp(logo, "off") == 0 || strcmp(logo, "0") == 0);
        if (!g_replayLogo.load())
            pclog::write("settings: no replay logo");
        for (int i = 0; i < kHudColorCount; i++) {
            char hex[16] = "";
            GetPrivateProfileStringA("Hud", kHudColors[i].key, "", hex, sizeof(hex), ini_path().c_str());
            char *end = nullptr;
            const unsigned long v = strtoul(hex, &end, 16);
            g_hudColors[i] = strlen(hex) == 6 && *end == '\0' ? (uint32_t)v : kHudColorGame;
            if (g_hudColors[i].load() != kHudColorGame)
                pclog::write("settings: HUD %s %06X", kHudColors[i].key, g_hudColors[i].load());
        }
        char smoke[16] = "";
        GetPrivateProfileStringA(kGraphics, "SmokeOpacity", "normal", smoke, sizeof(smoke), ini_path().c_str());
        int so = (int)SmokeOpacity::normal;
        for (int i = 0; i < kSmokeOpacityCount; i++)
            if (_stricmp(smoke, kSmokeOpacityNames[i]) == 0)
                so = i;
        g_smokeOpacity = so;
        if (so != (int)SmokeOpacity::normal)
            pclog::write("settings: smoke opacity %s (%d%%)", kSmokeOpacityNames[so], kSmokeOpacityPercents[so]);
        /* PC (2026-11-05): [Graphics] CarReflections */
        char cr[16] = "";
        GetPrivateProfileStringA(kGraphics, "CarReflections", "all", cr, sizeof(cr), ini_path().c_str());
        int crv = (int)CarReflections::all;
        for (int i = 0; i < kCarReflectionsCount; i++)
            if (_stricmp(cr, kCarReflectionsNames[i]) == 0)
                crv = i;
        g_carReflections = crv;
        if (crv != (int)CarReflections::all)
            pclog::write("settings: car reflections %s", kCarReflectionsNames[crv]);
        /* PC (2026-11-06): [Graphics] EnvMapDetail */
        char em[16] = "";
        GetPrivateProfileStringA(kGraphics, "EnvMapDetail", "scaled", em, sizeof(em), ini_path().c_str());
        int emv = (int)EnvMapDetail::scaled;
        for (int i = 0; i < kEnvMapDetailCount; i++)
            if (_stricmp(em, kEnvMapDetailNames[i]) == 0)
                emv = i;
        g_envMapDetail = emv;
        if (emv != (int)EnvMapDetail::scaled)
            pclog::write("settings: environment map detail %s", kEnvMapDetailNames[emv]);
        /* PC (2026-11-10): [Graphics] AnisotropicFiltering */
        {
            char af[16] = "";
            GetPrivateProfileStringA(kGraphics, "AnisotropicFiltering", "off", af, sizeof(af), ini_path().c_str());
            const int n = atoi(af);
            const int v = n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
            g_aniso = v;
            if (v > 1)
                pclog::write("settings: anisotropic filtering %dx", v);
        }
        /* PC (2026-11-07): [Graphics] ReplayDepthOfField */
        char dof[16] = "";
        GetPrivateProfileStringA(kGraphics, "ReplayDepthOfField", "original", dof, sizeof(dof), ini_path().c_str());
        int dofv = (int)ReplayDepthOfField::original;
        for (int i = 0; i < kReplayDepthOfFieldCount; i++)
            if (_stricmp(dof, kReplayDofNames[i]) == 0)
                dofv = i;
        g_replayDof = dofv;
        if (dofv != (int)ReplayDepthOfField::original)
            pclog::write("settings: replay depth of field %s", kReplayDofNames[dofv]);
    }
    {
        char out[16] = "", buf[32] = "";
        /* PC (2026-11-15, user request: "Audio Output: Windows Wasapi is the
         * default"): no key, or a value not known, wasapi (was auto) */
        GetPrivateProfileStringA(kAudio, "Output", "wasapi", out, sizeof(out), ini_path().c_str());
        int v = (int)AudioOutput::wasapi;
        for (int i = 0; i < kAudioOutputCount; i++)
            if (_stricmp(out, kAudioNames[i]) == 0)
                v = i;
        g_audioOut = v;
        GetPrivateProfileStringA(kAudio, "Buffer", "2", buf, sizeof(buf), ini_path().c_str());
        g_audioBuffer = clamp_buffer(atof(buf));
        pclog::write("settings: audio output %s, buffer %.1f ms", kAudioNames[v], g_audioBuffer.load());
        for (int i = 0; i < kVolumeCount; i++) {
            GetPrivateProfileStringA(kAudio, kVolumeKeys[i], "", buf, sizeof(buf), ini_path().c_str());
            g_volume[i] = buf[0] != '\0' ? clamp_volume(atoi(buf)) : kVolumeDefaults[i];
        }
        pclog::write("settings: volumes: music menus %d%%, races %d%%, replays %d%%; sound effects menus %d%%, "
                     "races %d%%, replays %d%%",
                     g_volume[0].load(), g_volume[1].load(), g_volume[2].load(), g_volume[3].load(),
                     g_volume[4].load(), g_volume[5].load());
    }
    load_car_kinds();
    input_map::init(ini_path().c_str());
    {
        /* the hotkeys, as read ([Hotkeys]; PC: a device's binding after `;') */
        char line[1024];
        size_t at = 0;
        for (int h = 0; h < input_map::kHotkeyCount && at < sizeof(line); h++) {
            char k[32], d[300];
            input_map::format_combo(input_map::hotkey(h), k, sizeof(k));
            input_map::format(input_map::hotkey_device(h), d, sizeof(d));
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s=%s%s%s", h ? ", " : "", input_map::hotkey_key(h),
                                   k, d[0] != '\0' ? ";" : "", d);
        }
        pclog::write("settings: hotkeys %s", line);
    }
    {
        char con[16] = "";
        GetPrivateProfileStringA("Debug", "Console", "off", con, sizeof(con), ini_path().c_str());
        g_debugConsole = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        if (g_debugConsole)
            pclog::write("settings: debug console on");
        GetPrivateProfileStringA("Debug", "FileLog", "off", con, sizeof(con), ini_path().c_str());
        g_debugFileLog = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        if (g_debugFileLog)
            pclog::write("settings: the files read and written logged (file: ...)");
        GetPrivateProfileStringA("Debug", "SyncLog", "off", con, sizeof(con), ini_path().c_str());
        g_debugSyncLog = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        GetPrivateProfileStringA("Debug", "ReplaySync", "off", con, sizeof(con), ini_path().c_str());
        g_debugReplaySync = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        GetPrivateProfileStringA("Debug", "CameraYLook", "off", con, sizeof(con), ini_path().c_str());
        g_debugCameraYLook = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        GetPrivateProfileStringA("Debug", "CameraGround", "off", con, sizeof(con), ini_path().c_str());
        g_debugCameraGround = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        GetPrivateProfileStringA("Debug", "PhysicsThreads", "auto", con, sizeof(con), ini_path().c_str());
        const int threads = atoi(con);
        g_physicsThreads = _stricmp(con, "auto") == 0 ? kPhysicsThreadsAuto
                           : threads < 0 ? 0 : threads > 16 ? 16 : threads;
        struct {
            const char *key;
            std::atomic<bool> *flag;
        } const kStart[] = {{"FillGarage", &g_debugFillGarage},
                            {"FillMoney", &g_debugFillMoney},
                            {"UnlockArcade", &g_debugUnlockArcade},
                            {"UnlockLicenses", &g_debugUnlockLicenses}};
        for (const auto &k : kStart) {
            GetPrivateProfileStringA("Debug", k.key, "off", con, sizeof(con), ini_path().c_str());
            *k.flag = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
            if (*k.flag)
                pclog::write("settings: debug %s on", k.key);
        }
        GetPrivateProfileStringA("Debug", "CourseForcer", "off", con, sizeof(con), ini_path().c_str());
        g_courseForcer = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        char course[64] = "";
        GetPrivateProfileStringA("Debug", "ForcedCourse", "", course, sizeof(course), ini_path().c_str());
        g_forcedCourse = course;
        if (g_courseForcer)
            pclog::write("settings: debug course forcer on: %s", course[0] != '\0' ? course : "(no course)");
        GetPrivateProfileStringA("Debug", "LapForcer", "off", con, sizeof(con), ini_path().c_str());
        g_lapForcer = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        const int laps = (int)GetPrivateProfileIntA("Debug", "ForcedLaps", 3, ini_path().c_str());
        g_forcedLaps = laps < kForcedLapsMin ? kForcedLapsMin : laps > kForcedLapsMax ? kForcedLapsMax : laps;
        if (g_lapForcer)
            pclog::write("settings: debug lap forcer on: %d laps", g_forcedLaps.load());
        GetPrivateProfileStringA("Debug", "RaceTimeForcer", "off", con, sizeof(con), ini_path().c_str());
        g_raceTimeForcer = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        const int minutes = (int)GetPrivateProfileIntA("Debug", "ForcedRaceTime", 10, ini_path().c_str());
        g_forcedRaceTime = minutes < kForcedRaceTimeMin ? kForcedRaceTimeMin
                           : minutes > kForcedRaceTimeMax ? kForcedRaceTimeMax : minutes;
        if (g_raceTimeForcer)
            pclog::write("settings: debug race time forcer on: %d minutes", g_forcedRaceTime.load());
        /* PC (2026-11-07): [Debug] CourseModeForcer */
        GetPrivateProfileStringA("Debug", "CourseModeForcer", "off", con, sizeof(con), ini_path().c_str());
        int cm = (int)CourseMode::off;
        for (int i = 0; i < kCourseModeCount; i++)
            if (_stricmp(con, kCourseModeNames[i]) == 0)
                cm = i;
        g_courseMode = cm;
        if (cm != (int)CourseMode::off)
            pclog::write("settings: debug course mode forcer on: %s", kCourseModeNames[cm]);
        /* PC (2026-11-12): [Debug] MenuRaceModel / MenuRaceModelLod */
        GetPrivateProfileStringA("Debug", "MenuRaceModel", "off", con, sizeof(con), ini_path().c_str());
        g_menuRaceModel = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        GetPrivateProfileStringA("Debug", "MenuRaceModelLod", "auto", con, sizeof(con), ini_path().c_str());
        const int lod = con[0] >= '0' && con[0] <= '9' ? atoi(con) : -1;
        g_menuRaceModelLod = lod > kMenuRaceModelLodMax ? kMenuRaceModelLodMax : lod;
        if (g_menuRaceModel)
            pclog::write("settings: debug menu race model on, level of detail %s", con);
    }
    {
        /* PC (2026-11-13): [Textures] */
        char v[16] = "";
        auto on_off = [&](const char *key, bool def) {
            GetPrivateProfileStringA(kTextures, key, def ? "on" : "off", v, sizeof(v), ini_path().c_str());
            return _stricmp(v, "on") == 0 || strcmp(v, "1") == 0;
        };
        g_texReplace = on_off("Replace", true);
        g_texPrecache = on_off("PreCache", true);
        g_texCompress = on_off("Compress", true);
        GetPrivateProfileStringA(kTextures, "Dump", "off", v, sizeof(v), ini_path().c_str());
        int dump = (int)TextureDump::off;
        for (int i = 0; i < kTextureDumpCount; i++)
            if (_stricmp(v, kTextureDumpNames[i]) == 0)
                dump = i;
        g_texDump = dump;
        g_texPacks.clear();
        const int count = (int)GetPrivateProfileIntA(kTextures, "Count", 0, ini_path().c_str());
        for (int i = 1; i <= count && i <= 4096; i++) {
            char key[32], folder[MAX_PATH * 2] = "";
            snprintf(key, sizeof(key), "Pack%dOn", i);
            if (!on_off(key, true))
                continue;
            snprintf(key, sizeof(key), "Pack%d", i);
            GetPrivateProfileStringA(kTextures, key, "", folder, sizeof(folder), ini_path().c_str());
            size_t len = strlen(folder);
            while (len > 0 && (folder[len - 1] == '\\' || folder[len - 1] == '/'))
                folder[--len] = '\0';
            if (len > 0)
                g_texPacks.push_back(folder);
        }
        pclog::write("settings: textures: replacing %s, dumping %s, pre-cache %s, compression %s, %d folder%s on",
                     g_texReplace.load() ? "on" : "off", kTextureDumpNames[dump], g_texPrecache.load() ? "on" : "off",
                     g_texCompress.load() ? "on" : "off", (int)g_texPacks.size(), g_texPacks.size() == 1 ? "" : "s");
    }
    {
        /* PC (2026-11-14): custom and its amount; a number of its own as
         * StartingCredits is custom with that amount */
        char credits[24] = "", custom[24] = "";
        GetPrivateProfileStringA(kGame, "StartingCredits", "auto", credits, sizeof(credits), ini_path().c_str());
        GetPrivateProfileStringA(kGame, "StartingCreditsCustom", "", custom, sizeof(custom), ini_path().c_str());
        char *end = nullptr;
        long long amount = strtoll(custom, &end, 10);
        if (end == custom || *end != '\0')
            amount = kStartingCreditsCustomDefault;
        end = nullptr;
        const long long c = strtoll(credits, &end, 10);
        const bool number = end != credits && *end == '\0';
        StartingCredits mode = StartingCredits::automatic;
        if (_stricmp(credits, "custom") == 0) {
            mode = StartingCredits::custom;
        } else if (number && c == 15000) {
            mode = StartingCredits::c15000;
        } else if (number && c == 18000) {
            mode = StartingCredits::c18000;
        } else if (number) {
            mode = StartingCredits::custom;
            amount = c;
        }
        amount = amount < kStartingCreditsCustomMin ? kStartingCreditsCustomMin
                 : amount > kStartingCreditsCustomMax ? kStartingCreditsCustomMax : amount;
        g_startingCreditsCustom = (int)amount;
        g_startingCredits = (int)mode;
        if (mode != StartingCredits::automatic)
            pclog::write("settings: starting credits %d%s", starting_credits(),
                         mode == StartingCredits::custom ? " (custom)" : "");
    }
    {
        for (int a = 0; a < 3; a++) {
            /* (as text: GetPrivateProfileInt gives 0 for a negative value) */
            char v[16] = "";
            GetPrivateProfileStringA(kGame, kCourseScaleKeys[a], "100", v, sizeof(v), ini_path().c_str());
            g_courseScale[a] = clamp_course_scale(a, atoi(v));
        }
        if (g_courseScale[0] != 100 || g_courseScale[1] != 100 || g_courseScale[2] != 100)
            pclog::write("settings: course scale x %d%%, y %d%%, z %d%%", g_courseScale[0].load(),
                         g_courseScale[1].load(), g_courseScale[2].load());
    }
    {
        char on[16] = "";
        GetPrivateProfileStringA(kGame, "PrizeCarRandomizer", "off", on, sizeof(on), ini_path().c_str());
        g_prizeCarRandomizer = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        if (g_prizeCarRandomizer.load())
            pclog::write("settings: prize car randomizer on");
        /* PC (2026-11-13): [Game] UnrestrictedWheels */
        GetPrivateProfileStringA(kGame, "UnrestrictedWheels", "off", on, sizeof(on), ini_path().c_str());
        g_unrestrictedWheels = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        if (g_unrestrictedWheels.load())
            pclog::write("settings: unrestricted aftermarket wheels on");
        /* PC (2026-11-14): [Game] BuyNotForSale */
        GetPrivateProfileStringA(kGame, "BuyNotForSale", "off", on, sizeof(on), ini_path().c_str());
        g_buyNotForSale = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        if (g_buyNotForSale.load())
            pclog::write("settings: buy cars not for sale on");
        /* PC (2026-11-14): [Challenges] */
        for (int c = 0; c < kChallengeCount; c++) {
            GetPrivateProfileStringA(kChallenges, kChallengeKeys[c], "off", on, sizeof(on), ini_path().c_str());
            g_challenge[c] = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
            if (g_challenge[c].load())
                pclog::write("settings: challenge %s on", kChallengeNames[c]);
        }
        GetPrivateProfileStringA(kGame, "CourseRandomizer", "off", on, sizeof(on), ini_path().c_str());
        g_courseRandomizer = _stricmp(on, "extended") == 0 ? (int)CourseRandomizer::extended      /* PC (2026-11-07) */
                             : _stricmp(on, "on") == 0 || strcmp(on, "1") == 0 ? (int)CourseRandomizer::on
                                                                              : (int)CourseRandomizer::off;
        if (g_courseRandomizer.load() != (int)CourseRandomizer::off)
            pclog::write("settings: course randomizer %s", kCourseRandomizerNames[g_courseRandomizer.load()]);
        /* PC (2026-11-08): the rev cut behavior */
        GetPrivateProfileStringA(kGame, "RevCut", "original", on, sizeof(on), ini_path().c_str());
        g_revCut = _stricmp(on, "realistic") == 0 ? (int)RevCut::realistic : (int)RevCut::original;
        if (g_revCut.load() != (int)RevCut::original)
            pclog::write("settings: rev cut behavior %s", kRevCutNames[g_revCut.load()]);
        /* PC (2026-11-08): the gear shift behavior */
        GetPrivateProfileStringA(kGame, "ShiftBehavior", "original", on, sizeof(on), ini_path().c_str());
        g_shiftBehavior = _stricmp(on, "realistic") == 0 ? (int)ShiftBehavior::realistic
                          : _stricmp(on, "clutch") == 0  ? (int)ShiftBehavior::clutch     /* PC (2026-11-09) */
                                                         : (int)ShiftBehavior::original;
        if (g_shiftBehavior.load() != (int)ShiftBehavior::original)
            pclog::write("settings: gear shift behavior %s", kShiftBehaviorNames[g_shiftBehavior.load()]);
        /* PC (2026-11-09): the H-Shifters (with the clutch mode) */
        GetPrivateProfileStringA(kGame, "HShifter", "off", on, sizeof(on), ini_path().c_str());
        g_hShifter = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        if (g_hShifter.load())
            pclog::write("settings: H-shifter on");
        const int cars = (int)GetPrivateProfileIntA(kGame, "RaceCars", kRaceCarsMin, ini_path().c_str());
        g_raceCars = cars < kRaceCarsMin ? kRaceCarsMin : cars > kRaceCarsMax ? kRaceCarsMax : cars;
        GetPrivateProfileStringA(kGame, "RaceCarsOverride", "off", on, sizeof(on), ini_path().c_str());
        g_raceCarsOverride = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        if (g_raceCars.load() != kRaceCarsMin || g_raceCarsOverride.load())
            pclog::write("settings: %d cars in a race%s", g_raceCars.load(),
                         g_raceCarsOverride.load() ? " (also the races of fewer)" : "");
    }
    {
        bool any = false;
        for (int t = 0; t < kCarTunes; t++)
            for (int e = 0; e < 2; e++) {
                g_carTune[t][e] = clamp_car_tune((CarTune)t, (int)GetPrivateProfileIntA(kGame, kCarTuneKeys[t][e], 100,
                                                                                       ini_path().c_str()));
                any = any || g_carTune[t][e].load() != 100;
            }
        if (any)
            pclog::write("settings: car multipliers (players / enemies): tyre wear %d%% / %d%%, power %d%% / %d%%, "
                         "grip %d%% / %d%%", g_carTune[0][0].load(), g_carTune[0][1].load(), g_carTune[1][0].load(),
                         g_carTune[1][1].load(), g_carTune[2][0].load(), g_carTune[2][1].load());
    }
    {
        std::string line;
        for (int s = 0; s < kSurfaces; s++)
            for (int t = 0; t < 2; t++) {
                const std::string key = surface_tune_key(s, (SurfaceTune)t);
                const int v = read_surface_tune(key, (SurfaceTune)t);
                g_surfaceTune[s][t] = v;
                if (v != kSurfaceOwn) {
                    char n[32];
                    snprintf(n, sizeof(n), " %.2f", v / 100.0);
                    line += (line.empty() ? "" : ", ") + key + n;
                }
            }
        if (!line.empty())
            pclog::write("settings: surfaces: %s", line.c_str());
    }
    g_speedUnit = read_unit("SpeedUnit", kSpeedUnitNames, kSpeedUnits);
    g_powerUnit = read_unit("PowerUnit", kPowerUnitNames, kPowerUnits);
    g_torqueUnit = read_unit("TorqueUnit", kTorqueUnitNames, kTorqueUnits);
    pclog::write("settings: units speed %s, power %s, torque %s (auto: the language's)",
                 g_speedUnit.load() < 0 ? "auto" : kSpeedUnitNames[g_speedUnit.load()],
                 g_powerUnit.load() < 0 ? "auto" : kPowerUnitNames[g_powerUnit.load()],
                 g_torqueUnit.load() < 0 ? "auto" : kTorqueUnitNames[g_torqueUnit.load()]);
}

int speed_unit() { return g_speedUnit.load(std::memory_order_relaxed); }
int power_unit() { return g_powerUnit.load(std::memory_order_relaxed); }
int torque_unit() { return g_torqueUnit.load(std::memory_order_relaxed); }
void set_speed_unit(int u) { write_unit(g_speedUnit, u, "SpeedUnit", kSpeedUnitNames, kSpeedUnits); }
void set_power_unit(int u) { write_unit(g_powerUnit, u, "PowerUnit", kPowerUnitNames, kPowerUnits); }
void set_torque_unit(int u) { write_unit(g_torqueUnit, u, "TorqueUnit", kTorqueUnitNames, kTorqueUnits); }
int units_generation() { return g_unitsGen.load(std::memory_order_acquire); }

StartingCredits starting_credits_mode()
{
    const int m = g_startingCredits.load(std::memory_order_relaxed);
    return m >= 0 && m < kStartingCreditsCount ? (StartingCredits)m : StartingCredits::automatic;
}

int starting_credits_custom()
{
    return g_startingCreditsCustom.load(std::memory_order_relaxed);
}

int starting_credits()
{
    switch (starting_credits_mode()) {
    case StartingCredits::c15000:
        return 15000;
    case StartingCredits::c18000:
        return 18000;
    case StartingCredits::custom:
        return starting_credits_custom();
    default:
        return -1;
    }
}

namespace {
void write_starting_credits_custom()
{
    char s[16];
    snprintf(s, sizeof(s), "%d", g_startingCreditsCustom.load());
    WritePrivateProfileStringA(kGame, "StartingCreditsCustom", s, ini_path().c_str());
}
} // namespace

void set_starting_credits_mode(StartingCredits mode)
{
    if ((int)mode < 0 || (int)mode >= kStartingCreditsCount || mode == starting_credits_mode())
        return;
    static const char *const kNames[kStartingCreditsCount] = {"auto", "15000", "18000", "custom"};
    g_startingCredits = (int)mode;
    WritePrivateProfileStringA(kGame, "StartingCredits", kNames[(int)mode], ini_path().c_str());
    /* (custom: its amount beside it - one read from StartingCredits itself
     * was never written under its own key) */
    if (mode == StartingCredits::custom)
        write_starting_credits_custom();
    if (mode == StartingCredits::automatic)
        pclog::write("settings: starting credits auto (from the next new game)");
    else
        pclog::write("settings: starting credits %d%s (from the next new game)", starting_credits(),
                     mode == StartingCredits::custom ? " (custom)" : "");
}

void set_starting_credits_custom(int credits)
{
    credits = credits < kStartingCreditsCustomMin ? kStartingCreditsCustomMin
              : credits > kStartingCreditsCustomMax ? kStartingCreditsCustomMax : credits;
    if (credits == g_startingCreditsCustom.load())
        return;
    g_startingCreditsCustom = credits;
    write_starting_credits_custom();
    pclog::write("settings: starting credits, custom amount %d (from the next new game)", credits);
}

int course_scale(int axis)
{
    return axis >= 0 && axis < 3 ? g_courseScale[axis].load(std::memory_order_relaxed) : 100;
}

void set_course_scale(int axis, int percent)
{
    if (axis < 0 || axis >= 3)
        return;
    percent = clamp_course_scale(axis, percent);
    if (g_courseScale[axis].exchange(percent) == percent)
        return;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGame, kCourseScaleKeys[axis], s, ini_path().c_str());
    pclog::write("settings: course scale %c %d%% (from the next course loaded)", "xyz"[axis], percent);
}

bool prize_car_randomizer()
{
    return g_prizeCarRandomizer.load(std::memory_order_relaxed);
}

void set_prize_car_randomizer(bool on)
{
    if (g_prizeCarRandomizer.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kGame, "PrizeCarRandomizer", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: prize car randomizer %s (from the next prize car screen)", on ? "on" : "off");
}

/* PC (2026-11-13): [Game] UnrestrictedWheels */
bool unrestricted_wheels()
{
    return g_unrestrictedWheels.load(std::memory_order_relaxed);
}

void set_unrestricted_wheels(bool on)
{
    if (g_unrestrictedWheels.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kGame, "UnrestrictedWheels", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: unrestricted aftermarket wheels %s", on ? "on" : "off");
}

/* PC (2026-11-14): [Game] BuyNotForSale */
bool buy_not_for_sale()
{
    return g_buyNotForSale.load(std::memory_order_relaxed);
}

void set_buy_not_for_sale(bool on)
{
    if (g_buyNotForSale.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kGame, "BuyNotForSale", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: buy cars not for sale %s", on ? "on" : "off");
}

/* PC (2026-11-14): [Challenges] */
bool challenge(Challenge c)
{
    const int i = (int)c;
    return i >= 0 && i < kChallengeCount && g_challenge[i].load(std::memory_order_relaxed);
}

void set_challenge(Challenge c, bool on)
{
    const int i = (int)c;
    if (i < 0 || i >= kChallengeCount || g_challenge[i].exchange(on) == on)
        return;
    WritePrivateProfileStringA(kChallenges, kChallengeKeys[i], on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: challenge %s %s", kChallengeNames[i], on ? "on" : "off");
}

bool course_randomizer()
{
    return g_courseRandomizer.load(std::memory_order_relaxed) != (int)CourseRandomizer::off;
}

/* PC (2026-11-07): off, on or extended */
CourseRandomizer course_randomizer_mode()
{
    return (CourseRandomizer)g_courseRandomizer.load(std::memory_order_relaxed);
}

void set_course_randomizer(CourseRandomizer mode)
{
    const int v = (int)mode;
    if (v < 0 || v >= kCourseRandomizerCount || g_courseRandomizer.exchange(v) == v)
        return;
    WritePrivateProfileStringA(kGame, "CourseRandomizer", kCourseRandomizerNames[v], ini_path().c_str());
    pclog::write("settings: course randomizer %s (from the next event entered)", kCourseRandomizerNames[v]);
}

/* PC (2026-11-08) */
RevCut rev_cut()
{
    return (RevCut)g_revCut.load(std::memory_order_relaxed);
}

void set_rev_cut(RevCut mode)
{
    const int v = (int)mode;
    if (v < 0 || v >= kRevCutCount || g_revCut.exchange(v) == v)
        return;
    WritePrivateProfileStringA(kGame, "RevCut", kRevCutNames[v], ini_path().c_str());
    pclog::write("settings: rev cut behavior %s (from the next race entered)", kRevCutNames[v]);
}

const char *rev_cut_name(RevCut mode)
{
    const int v = (int)mode;
    return v >= 0 && v < kRevCutCount ? kRevCutNames[v] : "?";
}

/* PC (2026-11-08) */
ShiftBehavior shift_behavior()
{
    return (ShiftBehavior)g_shiftBehavior.load(std::memory_order_relaxed);
}

void set_shift_behavior(ShiftBehavior mode)
{
    const int v = (int)mode;
    if (v < 0 || v >= kShiftBehaviorCount || g_shiftBehavior.exchange(v) == v)
        return;
    WritePrivateProfileStringA(kGame, "ShiftBehavior", kShiftBehaviorNames[v], ini_path().c_str());
    pclog::write("settings: gear shift behavior %s (from the next race entered)", kShiftBehaviorNames[v]);
}

const char *shift_behavior_name(ShiftBehavior mode)
{
    const int v = (int)mode;
    return v >= 0 && v < kShiftBehaviorCount ? kShiftBehaviorNames[v] : "?";
}

/* PC (2026-11-09) */
bool h_shifter()
{
    return g_hShifter.load(std::memory_order_relaxed);
}

void set_h_shifter(bool on)
{
    if (g_hShifter.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kGame, "HShifter", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: H-shifter %s (from the next race entered)", on ? "on" : "off");
}

int race_cars()
{
    return g_raceCars.load(std::memory_order_relaxed);
}

void set_race_cars(int cars)
{
    cars = cars < kRaceCarsMin ? kRaceCarsMin : cars > kRaceCarsMax ? kRaceCarsMax : cars;
    if (g_raceCars.exchange(cars) == cars)
        return;
    char s[16];
    snprintf(s, sizeof(s), "%d", cars);
    WritePrivateProfileStringA(kGame, "RaceCars", s, ini_path().c_str());
    pclog::write("settings: %d cars in a race (from the next race entered)", cars);
}

bool race_cars_override()
{
    return g_raceCarsOverride.load(std::memory_order_relaxed);
}

void set_race_cars_override(bool on)
{
    if (g_raceCarsOverride.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kGame, "RaceCarsOverride", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: cars in a race %s the races of fewer (from the next race entered)",
                 on ? "for" : "not for");
}

int car_tune_min(CarTune t)
{
    return t == CarTune::tireWear ? 0 : 10;
}

int car_tune(CarTune t, bool enemies)
{
    const int i = (int)t;
    return i >= 0 && i < kCarTunes ? g_carTune[i][enemies ? 1 : 0].load(std::memory_order_relaxed) : 100;
}

void set_car_tune(CarTune t, bool enemies, int percent)
{
    const int i = (int)t;
    if (i < 0 || i >= kCarTunes)
        return;
    percent = clamp_car_tune(t, percent);
    if (g_carTune[i][enemies ? 1 : 0].exchange(percent) == percent)
        return;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGame, kCarTuneKeys[i][enemies ? 1 : 0], s, ini_path().c_str());
    pclog::write("settings: %s %d%% (from the next race entered)", kCarTuneKeys[i][enemies ? 1 : 0], percent);
}

const char *surface_key(int s)
{
    return s >= 0 && s < kSurfaces ? kSurfaceKeys[s] : "";
}

int surface_code(int s)
{
    return s >= 0 && s < kSurfaces ? kSurfaceCodes[s] : -1;
}

int surface_tune_min(SurfaceTune t)
{
    return t == SurfaceTune::grip ? 1 : 0;
}

int surface_tune_max(SurfaceTune t)
{
    return t == SurfaceTune::grip ? 1000 : 32767;
}

int surface_tune_default(int s, SurfaceTune t)
{
    return s >= 0 && s < kSurfaces ? kSurfaceDefaults[s][(int)t] : 0;
}

int surface_tune(int s, SurfaceTune t)
{
    return s >= 0 && s < kSurfaces ? g_surfaceTune[s][(int)t].load(std::memory_order_relaxed) : kSurfaceOwn;
}

void set_surface_tune(int s, SurfaceTune t, int hundredths)
{
    if (s < 0 || s >= kSurfaces)
        return;
    hundredths = clamp_surface_tune(t, hundredths);
    if (g_surfaceTune[s][(int)t].exchange(hundredths) == hundredths)
        return;
    const std::string key = surface_tune_key(s, t);
    if (hundredths == kSurfaceOwn) {
        WritePrivateProfileStringA(kGame, key.c_str(), nullptr, ini_path().c_str());
        pclog::write("settings: %s the game's own (from the next race entered)", key.c_str());
        return;
    }
    char v[32];
    snprintf(v, sizeof(v), "%.2f", hundredths / 100.0);
    WritePrivateProfileStringA(kGame, key.c_str(), v, ini_path().c_str());
    pclog::write("settings: %s %s (from the next race entered)", key.c_str(), v);
}

bool course_forcer()
{
    return g_courseForcer.load(std::memory_order_relaxed);
}

void set_course_forcer(bool on)
{
    if (g_courseForcer.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", "CourseForcer", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug course forcer %s (from the next race entered)", on ? "on" : "off");
}

void forced_course(char *out, int n)
{
    std::lock_guard<std::mutex> l(g_courseLock);
    if (n > 0)
        snprintf(out, (size_t)n, "%s", g_forcedCourse.c_str());
}

void set_forced_course(const char *label)
{
    {
        std::lock_guard<std::mutex> l(g_courseLock);
        if (g_forcedCourse == label)
            return;
        g_forcedCourse = label;
    }
    WritePrivateProfileStringA("Debug", "ForcedCourse", label, ini_path().c_str());
    pclog::write("settings: debug forced course %s (from the next race entered)", label);
}

bool lap_forcer()
{
    return g_lapForcer.load(std::memory_order_relaxed);
}

void set_lap_forcer(bool on)
{
    if (g_lapForcer.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", "LapForcer", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug lap forcer %s (from the next race entered)", on ? "on" : "off");
}

int forced_laps()
{
    return g_forcedLaps.load(std::memory_order_relaxed);
}

void set_forced_laps(int laps)
{
    laps = laps < kForcedLapsMin ? kForcedLapsMin : laps > kForcedLapsMax ? kForcedLapsMax : laps;
    if (g_forcedLaps.exchange(laps) == laps)
        return;
    char s[16];
    snprintf(s, sizeof(s), "%d", laps);
    WritePrivateProfileStringA("Debug", "ForcedLaps", s, ini_path().c_str());
    pclog::write("settings: debug forced laps %d (from the next race entered)", laps);
}

bool race_time_forcer()
{
    return g_raceTimeForcer.load(std::memory_order_relaxed);
}

void set_race_time_forcer(bool on)
{
    if (g_raceTimeForcer.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", "RaceTimeForcer", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug race time forcer %s (from the next race entered)", on ? "on" : "off");
}

int forced_race_time()
{
    return g_forcedRaceTime.load(std::memory_order_relaxed);
}

/* PC (2026-11-07): [Debug] CourseModeForcer */
CourseMode course_mode_forcer()
{
    return (CourseMode)g_courseMode.load(std::memory_order_relaxed);
}

void set_course_mode_forcer(CourseMode m)
{
    const int v = (int)m;
    if (v < 0 || v >= kCourseModeCount || g_courseMode.exchange(v) == v)
        return;
    WritePrivateProfileStringA("Debug", "CourseModeForcer", kCourseModeNames[v], ini_path().c_str());
    pclog::write("settings: debug course mode forcer %s (from the next race entered)", kCourseModeNames[v]);
}

/* PC (2026-11-12): [Debug] MenuRaceModel / MenuRaceModelLod */
bool menu_race_model()
{
    return g_menuRaceModel.load(std::memory_order_relaxed);
}

void set_menu_race_model(bool on)
{
    if (g_menuRaceModel.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", "MenuRaceModel", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug menu race model %s", on ? "on" : "off");
}

int menu_race_model_lod()
{
    return g_menuRaceModelLod.load(std::memory_order_relaxed);
}

/* PC (2026-11-13): [Textures] */
static void set_texture_switch(std::atomic<bool> &value, const char *key, const char *what, bool on)
{
    if (value.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kTextures, key, on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: textures: %s %s", what, on ? "on" : "off");
}

bool texture_replace() { return g_texReplace.load(std::memory_order_relaxed); }
void set_texture_replace(bool on) { set_texture_switch(g_texReplace, "Replace", "replacing", on); }
bool texture_precache() { return g_texPrecache.load(std::memory_order_relaxed); }
void set_texture_precache(bool on) { set_texture_switch(g_texPrecache, "PreCache", "pre-cache", on); }
bool texture_compress() { return g_texCompress.load(std::memory_order_relaxed); }
void set_texture_compress(bool on) { set_texture_switch(g_texCompress, "Compress", "compression", on); }

TextureDump texture_dump()
{
    return (TextureDump)g_texDump.load(std::memory_order_relaxed);
}

void set_texture_dump(TextureDump m)
{
    const int v = (int)m;
    if (v < 0 || v >= kTextureDumpCount || g_texDump.exchange(v) == v)
        return;
    WritePrivateProfileStringA(kTextures, "Dump", kTextureDumpNames[v], ini_path().c_str());
    pclog::write("settings: textures: dumping %s", kTextureDumpNames[v]);
}

int texture_pack_count()
{
    return (int)g_texPacks.size();
}

void texture_pack(int i, char *out, int n)
{
    if (n <= 0)
        return;
    snprintf(out, (size_t)n, "%s", i >= 0 && i < (int)g_texPacks.size() ? g_texPacks[(size_t)i].c_str() : "");
}

void texture_root(char *out, int n)
{
    if (n > 0)
        snprintf(out, (size_t)n, "%stex_replacements\\", paths::game_dir().c_str());
}

void texture_cache(char *out, int n)
{
    if (n > 0)
        snprintf(out, (size_t)n, "%scache\\textures\\", paths::game_dir().c_str());
}

void set_menu_race_model_lod(int level)
{
    level = level < 0 ? -1 : level > kMenuRaceModelLodMax ? kMenuRaceModelLodMax : level;
    if (g_menuRaceModelLod.exchange(level) == level)
        return;
    char s[16];
    snprintf(s, sizeof(s), "%d", level);
    WritePrivateProfileStringA("Debug", "MenuRaceModelLod", level < 0 ? "auto" : s, ini_path().c_str());
    pclog::write("settings: debug menu race model level of detail %s", level < 0 ? "auto" : s);
}

void set_forced_race_time(int minutes)
{
    minutes = minutes < kForcedRaceTimeMin ? kForcedRaceTimeMin
              : minutes > kForcedRaceTimeMax ? kForcedRaceTimeMax : minutes;
    if (g_forcedRaceTime.exchange(minutes) == minutes)
        return;
    char s[16];
    snprintf(s, sizeof(s), "%d", minutes);
    WritePrivateProfileStringA("Debug", "ForcedRaceTime", s, ini_path().c_str());
    pclog::write("settings: debug forced race time %d minutes (from the next race entered)", minutes);
}

void set_course_choices(const CourseChoice *choices, int count)
{
    std::lock_guard<std::mutex> l(g_courseLock);
    g_courseChoices.assign(choices, choices + (count > 0 ? count : 0));
}

int course_choice_count()
{
    std::lock_guard<std::mutex> l(g_courseLock);
    return (int)g_courseChoices.size();
}

CourseChoice course_choice(int i)
{
    std::lock_guard<std::mutex> l(g_courseLock);
    if (i >= 0 && i < (int)g_courseChoices.size())
        return g_courseChoices[i];
    return CourseChoice{};
}

bool debug_fill_garage() { return g_debugFillGarage; }
bool debug_fill_money() { return g_debugFillMoney; }
bool debug_unlock_arcade() { return g_debugUnlockArcade; }
bool debug_unlock_licenses() { return g_debugUnlockLicenses; }

/* A [Debug] flag set (the settings menu's DEBUG): written at once. */
static void set_debug_flag(std::atomic<bool> &flag, const char *key, bool on)
{
    if (flag.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", key, on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug %s %s", key, on ? "on" : "off");
}

void set_debug_fill_garage(bool on) { set_debug_flag(g_debugFillGarage, "FillGarage", on); }
void set_debug_fill_money(bool on) { set_debug_flag(g_debugFillMoney, "FillMoney", on); }
void set_debug_unlock_arcade(bool on) { set_debug_flag(g_debugUnlockArcade, "UnlockArcade", on); }
void set_debug_unlock_licenses(bool on) { set_debug_flag(g_debugUnlockLicenses, "UnlockLicenses", on); }

bool debug_console()
{
    return g_debugConsole;
}

bool debug_file_log()
{
    return g_debugFileLog;
}

bool debug_sync_log()
{
    return g_debugSyncLog;
}

bool debug_replay_sync()
{
    return g_debugReplaySync;
}

bool debug_camera_y_look()
{
    return g_debugCameraYLook.load(std::memory_order_relaxed);
}

void set_debug_camera_y_look(bool on)
{
    if (g_debugCameraYLook.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", "CameraYLook", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug camera Y-axis look %s", on ? "on" : "off");
}

bool debug_camera_ground()
{
    return g_debugCameraGround.load(std::memory_order_relaxed);
}

void set_debug_camera_ground(bool on)
{
    if (g_debugCameraGround.exchange(on) == on)
        return;
    WritePrivateProfileStringA("Debug", "CameraGround", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: debug camera ground collision %s", on ? "on" : "off");
}

int physics_threads()
{
    return g_physicsThreads;
}

double frame_cap_ms()
{
    return g_frameCap.load(std::memory_order_relaxed);
}

void set_frame_cap_ms(double ms)
{
    ms = clamp_cap(ms);
    if (ms == g_frameCap.load())
        return;
    g_frameCap = ms;
    char s[32];
    snprintf(s, sizeof(s), "%.3f", ms);
    WritePrivateProfileStringA(kGraphics, "FrameCap", ms > 0.0 ? s : "0", ini_path().c_str());
    if (ms > 0.0)
        pclog::write("settings: frame cap %.3f ms (%.2f frames a second)", ms, 1000.0 / ms);
    else
        pclog::write("settings: no frame cap");
}

int vsync()
{
    return g_vsync.load(std::memory_order_relaxed);
}

int fov_percent()
{
    return g_fov.load(std::memory_order_relaxed);
}

void set_fov_percent(int percent)
{
    percent = clamp_fov(percent);
    if (percent == g_fov.load())
        return;
    g_fov = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "Fov", s, ini_path().c_str());
    pclog::write("settings: field of view %d%%", percent);
}

int replay_fov_percent()
{
    return g_replayFov.load(std::memory_order_relaxed);
}

void set_replay_fov_percent(int percent)
{
    percent = clamp_fov(percent);
    if (percent == g_replayFov.load())
        return;
    g_replayFov = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "ReplayFov", s, ini_path().c_str());
    pclog::write("settings: replay field of view %d%%", percent);
}

void set_vsync(int frames)
{
    if (frames < 0 || frames > 3)
        frames = 0;
    if (frames == g_vsync.load())
        return;
    g_vsync = frames;
    char s[8];
    snprintf(s, sizeof(s), "%d", frames);
    WritePrivateProfileStringA(kGraphics, "VSync", frames ? s : "off", ini_path().c_str());
    pclog::write(frames ? "settings: vsync, %d frame(s) queued at most" : "settings: vsync off", frames);
}

AudioOutput audio_output()
{
    return (AudioOutput)g_audioOut.load(std::memory_order_relaxed);
}

void set_audio_output(AudioOutput out)
{
    const int v = (int)out;
    if (v < 0 || v >= kAudioOutputCount || v == g_audioOut.load())
        return;
    g_audioOut = v;
    g_audioGen++;
    WritePrivateProfileStringA(kAudio, "Output", kAudioNames[v], ini_path().c_str());
    pclog::write("settings: audio output %s", kAudioNames[v]);
}

const char *audio_output_name(AudioOutput out)
{
    const int v = (int)out;
    return v >= 0 && v < kAudioOutputCount ? kAudioNames[v] : "auto";
}

double audio_buffer_ms()
{
    return g_audioBuffer.load(std::memory_order_relaxed);
}

void set_audio_buffer_ms(double ms)
{
    ms = clamp_buffer(ms);
    if (ms == g_audioBuffer.load())
        return;
    g_audioBuffer = ms;
    g_audioGen++;
    char s[32];
    snprintf(s, sizeof(s), "%.1f", ms);
    WritePrivateProfileStringA(kAudio, "Buffer", s, ini_path().c_str());
    pclog::write("settings: audio buffer %.1f ms", ms);
}

int audio_generation()
{
    return g_audioGen.load(std::memory_order_relaxed);
}

int volume(Volume v)
{
    const int i = (int)v;
    return i >= 0 && i < kVolumeCount ? g_volume[i].load(std::memory_order_relaxed) : 100;
}

void set_volume(Volume v, int percent)
{
    const int i = (int)v;
    if (i < 0 || i >= kVolumeCount)
        return;
    percent = clamp_volume(percent);
    if (percent == g_volume[i].load())
        return;
    g_volume[i] = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kAudio, kVolumeKeys[i], s, ini_path().c_str());
    pclog::write("settings: volume %s %d%%", kVolumeKeys[i], percent);
}

int volume_default(Volume v)
{
    const int i = (int)v;
    return i >= 0 && i < kVolumeCount ? kVolumeDefaults[i] : 100;
}

SplitScreen split_screen()
{
    return (SplitScreen)g_splitScreen.load(std::memory_order_relaxed);
}

void set_split_screen(SplitScreen s)
{
    const int v = (int)s;
    if (v < 0 || v >= kSplitScreenCount || v == g_splitScreen.load())
        return;
    g_splitScreen = v;
    WritePrivateProfileStringA(kGraphics, "SplitScreen", kSplitNames[v], ini_path().c_str());
    pclog::write("settings: split screen %s", kSplitNames[v]);
}

bool split_hud_analog()
{
    return g_splitHudAnalog.load(std::memory_order_relaxed);
}

void set_split_hud_analog(bool on)
{
    if (on == g_splitHudAnalog.load())
        return;
    g_splitHudAnalog = on;
    WritePrivateProfileStringA(kGraphics, "SplitHud", on ? "analog" : "digital", ini_path().c_str());
    pclog::write("settings: split screen HUD %s", on ? "analog" : "digital");
}

bool single_hud_digital()
{
    return g_singleHudDigital.load(std::memory_order_relaxed);
}

void set_single_hud_digital(bool on)
{
    if (on == g_singleHudDigital.load())
        return;
    g_singleHudDigital = on;
    WritePrivateProfileStringA(kGraphics, "SingleHud", on ? "digital" : "analog", ini_path().c_str());
    pclog::write("settings: single player HUD %s", on ? "digital" : "analog");
}

int hud_safe_zone()
{
    return g_hudSafeZone.load(std::memory_order_relaxed);
}

void set_hud_safe_zone(int percent)
{
    percent = clamp_safe_zone(percent);
    if (percent == g_hudSafeZone.load())
        return;
    g_hudSafeZone = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "HudSafeZone", s, ini_path().c_str());
    pclog::write("settings: HUD safe zone %d%%", percent);
}

bool replay_logo()
{
    return g_replayLogo.load(std::memory_order_relaxed);
}

void set_replay_logo(bool on)
{
    if (g_replayLogo.exchange(on) == on)
        return;
    WritePrivateProfileStringA(kGraphics, "ReplayLogo", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: replay logo %s", on ? "on" : "off");
}

uint32_t hud_color(int i)
{
    if (i < 0 || i >= kHudColorCount)
        return 0;
    const uint32_t rgb = g_hudColors[i].load(std::memory_order_relaxed);
    return rgb != kHudColorGame ? rgb : kHudColors[i].rgb;
}

uint32_t hud_color_set(int i)
{
    return i >= 0 && i < kHudColorCount ? g_hudColors[i].load(std::memory_order_relaxed) : kHudColorGame;
}

void set_hud_color(int i, uint32_t rgb)
{
    if (i < 0 || i >= kHudColorCount || rgb == hud_color(i))
        return;
    const bool own = rgb == kHudColors[i].rgb;
    g_hudColors[i] = own ? kHudColorGame : rgb;
    char hex[16];
    snprintf(hex, sizeof(hex), "%06X", rgb);
    WritePrivateProfileStringA("Hud", kHudColors[i].key, own ? nullptr : hex, ini_path().c_str());
    pclog::write("settings: HUD %s %s", kHudColors[i].key, own ? "the game's" : hex);
}

SmokeOpacity smoke_opacity()
{
    return (SmokeOpacity)g_smokeOpacity.load(std::memory_order_relaxed);
}

void set_smoke_opacity(SmokeOpacity o)
{
    const int v = (int)o;
    if (v < 0 || v >= kSmokeOpacityCount || v == g_smokeOpacity.load())
        return;
    g_smokeOpacity = v;
    WritePrivateProfileStringA(kGraphics, "SmokeOpacity", kSmokeOpacityNames[v], ini_path().c_str());
    pclog::write("settings: smoke opacity %s (%d%%)", kSmokeOpacityNames[v], kSmokeOpacityPercents[v]);
}

int smoke_opacity_percent(SmokeOpacity o)
{
    const int v = (int)o;
    return v >= 0 && v < kSmokeOpacityCount ? kSmokeOpacityPercents[v] : 100;
}

/* PC (2026-11-05): [Graphics] CarReflections */
CarReflections car_reflections()
{
    return (CarReflections)g_carReflections.load(std::memory_order_relaxed);
}

void set_car_reflections(CarReflections c)
{
    const int v = (int)c;
    if (v < 0 || v >= kCarReflectionsCount || v == g_carReflections.load())
        return;
    g_carReflections = v;
    WritePrivateProfileStringA(kGraphics, "CarReflections", kCarReflectionsNames[v], ini_path().c_str());
    pclog::write("settings: car reflections %s", kCarReflectionsNames[v]);
}

int car_reflection_min_px()
{
    const int v = g_carReflections.load(std::memory_order_relaxed);
    return v >= 0 && v < kCarReflectionsCount ? kCarReflectionsMinPx[v] : 0;
}

/* PC (2026-11-06): [Graphics] EnvMapDetail */
EnvMapDetail env_map_detail()
{
    return (EnvMapDetail)g_envMapDetail.load(std::memory_order_relaxed);
}

void set_env_map_detail(EnvMapDetail d)
{
    const int v = (int)d;
    if (v < 0 || v >= kEnvMapDetailCount || v == g_envMapDetail.load())
        return;
    g_envMapDetail = v;
    WritePrivateProfileStringA(kGraphics, "EnvMapDetail", kEnvMapDetailNames[v], ini_path().c_str());
    pclog::write("settings: environment map detail %s", kEnvMapDetailNames[v]);
}

/* PC (2026-11-10): [Graphics] AnisotropicFiltering */
int anisotropic_filtering()
{
    return g_aniso.load(std::memory_order_relaxed);
}

void set_anisotropic_filtering(int most)
{
    const int v = most >= 16 ? 16 : most >= 8 ? 8 : most >= 4 ? 4 : most >= 2 ? 2 : 1;
    if (v == g_aniso.exchange(v))
        return;
    char text[8];
    snprintf(text, sizeof(text), "%d", v);
    WritePrivateProfileStringA(kGraphics, "AnisotropicFiltering", v > 1 ? text : "off", ini_path().c_str());
    if (v > 1)
        pclog::write("settings: anisotropic filtering %dx", v);
    else
        pclog::write("settings: anisotropic filtering off");
}

/* PC (2026-11-07): [Graphics] ReplayDepthOfField */
ReplayDepthOfField replay_depth_of_field()
{
    return (ReplayDepthOfField)g_replayDof.load(std::memory_order_relaxed);
}

void set_replay_depth_of_field(ReplayDepthOfField d)
{
    const int v = (int)d;
    if (v < 0 || v >= kReplayDepthOfFieldCount || v == g_replayDof.load())
        return;
    g_replayDof = v;
    WritePrivateProfileStringA(kGraphics, "ReplayDepthOfField", kReplayDofNames[v], ini_path().c_str());
    pclog::write("settings: replay depth of field %s", kReplayDofNames[v]);
}

bool lod_enabled()
{
    return g_lod.load(std::memory_order_relaxed);
}

void set_lod_enabled(bool on)
{
    if (on == g_lod.load())
        return;
    g_lod = on;
    WritePrivateProfileStringA(kGraphics, "Lod", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: levels of detail %s", on ? "on" : "off");
}

ReflectionDetail reflection_detail()
{
    return (ReflectionDetail)g_reflDetail.load(std::memory_order_relaxed);
}

void set_reflection_detail(ReflectionDetail d)
{
    if ((int)d == g_reflDetail.load())
        return;
    g_reflDetail = (int)d;
    WritePrivateProfileStringA(kGraphics, "ReflectionDetail", d == ReflectionDetail::full ? "full" : "original",
                               ini_path().c_str());
    pclog::write("settings: road reflection detail %s", d == ReflectionDetail::full ? "full" : "original");
}

/* PC (2026-11-14): [Graphics] ReflectionResolution */
ReflectionResolution reflection_resolution()
{
    return (ReflectionResolution)g_reflResolution.load(std::memory_order_relaxed);
}

void set_reflection_resolution(ReflectionResolution r)
{
    const int i = (int)r;
    if (i < 0 || i >= kReflectionResolutionCount || g_reflResolution.exchange(i) == i)
        return;
    WritePrivateProfileStringA(kGraphics, "ReflectionResolution", kReflectionResolutionNames[i], ini_path().c_str());
    pclog::write("settings: road reflection resolution %s", kReflectionResolutionNames[i]);
}

bool rear_view(RearViewCamera c)
{
    const int i = (int)c;
    return i >= 0 && i < kRearViewCameras && g_rearView[i].load(std::memory_order_relaxed);
}

void set_rear_view(RearViewCamera c, bool on)
{
    const int i = (int)c;
    if (i < 0 || i >= kRearViewCameras || on == g_rearView[i].load())
        return;
    g_rearView[i] = on;
    WritePrivateProfileStringA(kGraphics, kRearViewKeys[i], on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: rear view, %s camera %s", i == 0 ? "bumper" : "chase", on ? "on" : "off");
}

bool extra_cameras()
{
    return g_extraCameras.load(std::memory_order_relaxed);
}

void set_extra_cameras(bool on)
{
    if (on == g_extraCameras.load())
        return;
    g_extraCameras = on;
    WritePrivateProfileStringA(kGraphics, "ExtraCameras", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: extra cameras %s", on ? "on" : "off");
}

int chase_angle()
{
    return g_chaseAngle.load(std::memory_order_relaxed);
}

void set_chase_angle(int degrees)
{
    degrees = degrees < kChaseAngleMin ? kChaseAngleMin : degrees > kChaseAngleMax ? kChaseAngleMax : degrees;
    if (degrees == g_chaseAngle.load())
        return;
    g_chaseAngle = degrees;
    char s[16];
    snprintf(s, sizeof(s), "%d", degrees);
    WritePrivateProfileStringA(kGraphics, "ChaseAngle", s, ini_path().c_str());
    pclog::write("settings: chase camera angle %d", degrees);
}

bool cockpit_camera()
{
    return g_cockpitCamera.load(std::memory_order_relaxed);
}

void set_cockpit_camera(bool on)
{
    if (on == g_cockpitCamera.load())
        return;
    g_cockpitCamera = on;
    WritePrivateProfileStringA(kGraphics, "CockpitCamera", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: cockpit camera %s", on ? "on" : "off");
}

int cockpit_fov()
{
    return g_cockpitFov.load(std::memory_order_relaxed);
}

void set_cockpit_fov(int degrees)
{
    degrees = degrees < kCockpitFovMin ? kCockpitFovMin : degrees > kCockpitFovMax ? kCockpitFovMax : degrees;
    if (degrees == g_cockpitFov.load())
        return;
    g_cockpitFov = degrees;
    char s[16];
    snprintf(s, sizeof(s), "%d", degrees);
    WritePrivateProfileStringA(kGraphics, "CockpitFov", s, ini_path().c_str());
    pclog::write("settings: cockpit camera field of view %d", degrees);
}

/* PC (2026-11-15): [CockpitMenuModels] <model>=on (settings.h) */
namespace {
const char kCockpitMenuModels[] = "CockpitMenuModels";
} // namespace

bool cockpit_menu_model(const char *model)
{
    if (model == nullptr || model[0] == '\0')
        return false;
    char v[16] = "";
    GetPrivateProfileStringA(kCockpitMenuModels, model, "off", v, sizeof(v), ini_path().c_str());
    return _stricmp(v, "on") == 0 || strcmp(v, "1") == 0;
}

void set_cockpit_menu_model(const char *model, bool on)
{
    if (model == nullptr || model[0] == '\0')
        return;
    WritePrivateProfileStringA(kCockpitMenuModels, model, on ? "on" : nullptr, ini_path().c_str());
    pclog::write("settings: cockpit camera menu model of %s %s", model, on ? "on" : "off");
}

bool menu_wheels()
{
    return g_menuWheels.load(std::memory_order_relaxed);
}

void set_menu_wheels(bool on)
{
    if (on == g_menuWheels.load())
        return;
    g_menuWheels = on;
    WritePrivateProfileStringA(kGraphics, "MenuWheels", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: menu wheels %s", on ? "on" : "off");
}

int menu_wheels_speed()
{
    return g_menuWheelsSpeed.load(std::memory_order_relaxed);
}

void set_menu_wheels_speed(int kmh)
{
    kmh = kmh < kMenuWheelsSpeedMin ? kMenuWheelsSpeedMin : kmh > kMenuWheelsSpeedMax ? kMenuWheelsSpeedMax : kmh;
    if (kmh == g_menuWheelsSpeed.load())
        return;
    g_menuWheelsSpeed = kmh;
    char s[16];
    snprintf(s, sizeof(s), "%d", kmh);
    WritePrivateProfileStringA(kGraphics, "MenuWheelsSpeed", s, ini_path().c_str());
    pclog::write("settings: menu wheels, the race wheels from %d km/h", kmh);
}

bool menu_tires()
{
    return g_menuTires.load(std::memory_order_relaxed);
}

void set_menu_tires(bool on)
{
    if (on == g_menuTires.load())
        return;
    g_menuTires = on;
    WritePrivateProfileStringA(kGraphics, "MenuTires", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: menu tires %s", on ? "on" : "off");
}

/* PC (2026-11-08): [CockpitOffsets] <model>[.2] = "x y z tilt" */
namespace {
const char kCockpitOffsets[] = "CockpitOffsets";
std::string cockpit_offset_key(const char *model, int slot)
{
    std::string key = model != nullptr ? model : "";
    if (slot == 2)
        key += ".2";
    return key;
}
} // namespace

bool cockpit_offset(const char *model, int slot, float out[4])
{
    for (int k = 0; k < 4; k++)
        out[k] = 0.0f;
    if (model == nullptr || model[0] == '\0')
        return false;
    char v[128] = "";
    GetPrivateProfileStringA(kCockpitOffsets, cockpit_offset_key(model, slot).c_str(), "", v, sizeof(v),
                             ini_path().c_str());
    float x, y, z, t;
    if (sscanf(v, "%f %f %f %f", &x, &y, &z, &t) != 4)
        return false;
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = t;
    return true;
}

void set_cockpit_offset(const char *model, int slot, const float v[4])
{
    if (model == nullptr || model[0] == '\0')
        return;
    const std::string key = cockpit_offset_key(model, slot);
    if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f) {
        WritePrivateProfileStringA(kCockpitOffsets, key.c_str(), nullptr, ini_path().c_str());
        pclog::write("settings: cockpit camera offsets of %s cleared", key.c_str());
        return;
    }
    char s[128];
    snprintf(s, sizeof(s), "%.3f %.3f %.3f %.1f", v[0], v[1], v[2], v[3]);
    WritePrivateProfileStringA(kCockpitOffsets, key.c_str(), s, ini_path().c_str());
    pclog::write("settings: cockpit camera offsets of %s: %s", key.c_str(), s);
}

/* PC (2026-11-14): [CustomCameras] <scope>.<n> = "from type x y z pan tilt
 * angle fov r1 .. r8 name" (settings.h) */
namespace {
const char kCustomCameras[] = "CustomCameras";
const char *const kCustomFromNames[kCustomFromCount] = {"center", "bumper", "chase", "cockpit"};
const char *const kCustomTypeNames[kCustomTypeCount] = {"chase", "onboard"};

float custom_clamp(float v, float lo, float hi)
{
    return !(v == v) ? 0.0f : v < lo ? lo : v > hi ? hi : v;    /* (a NaN: 0) */
}

/* One value read (false: not a camera - the key is missing or the line is
 * not one). */
bool custom_parse(const char *v, CustomCamera *c)
{
    char from[16] = "", type[16] = "";
    int used = 0;
    CustomCamera t = {};
    if (sscanf(v, "%15s %15s %f %f %f %f %f %f %f %d %d %d %d %d %d %d %d %n", from, type, &t.pos[0], &t.pos[1],
               &t.pos[2], &t.pan, &t.tilt, &t.angle, &t.fov, &t.rigid[0], &t.rigid[1], &t.rigid[2], &t.rigid[3],
               &t.rigid[4], &t.rigid[5], &t.rigid[6], &t.rigid[7], &used) < 17)
        return false;
    t.from = -1;
    for (int i = 0; i < kCustomFromCount; i++)
        if (_stricmp(from, kCustomFromNames[i]) == 0)
            t.from = i;
    if (_stricmp(from, "centre") == 0)
        t.from = kCustomFromCentre;
    t.type = -1;
    for (int i = 0; i < kCustomTypeCount; i++)
        if (_stricmp(type, kCustomTypeNames[i]) == 0)
            t.type = i;
    if (t.from < 0 || t.type < 0)
        return false;
    /* (the position has no limits; only what keeps the numbers sound) */
    for (float &p : t.pos)
        p = custom_clamp(p, -100000.0f, 100000.0f);
    t.pan = custom_clamp(t.pan, -180.0f, 180.0f);
    t.tilt = custom_clamp(t.tilt, -90.0f, 90.0f);
    t.angle = custom_clamp(t.angle, -kCustomAngleMax, kCustomAngleMax);
    t.fov = custom_clamp(t.fov, kCustomFovMin, kCustomFovMax);
    for (int &r : t.rigid)
        r = r < 0 ? 0 : r > 100 ? 100 : r;
    const char *name = used > 0 ? v + used : "";
    snprintf(t.name, sizeof(t.name), "%s", name);
    for (size_t n = strlen(t.name); n > 0 && (t.name[n - 1] == ' ' || t.name[n - 1] == '\t'); n--)
        t.name[n - 1] = '\0';
    *c = t;
    return true;
}
} // namespace

int custom_cameras(const char *scope, CustomCamera out[kCustomCameraMax])
{
    if (scope == nullptr || scope[0] == '\0')
        return 0;
    int n = 0;
    for (; n < kCustomCameraMax; n++) {
        char key[96], v[256] = "";
        snprintf(key, sizeof(key), "%s.%d", scope, n + 1);
        GetPrivateProfileStringA(kCustomCameras, key, "", v, sizeof(v), ini_path().c_str());
        if (!custom_parse(v, &out[n]))
            break;
    }
    return n;
}

void set_custom_cameras(const char *scope, const CustomCamera *list, int count)
{
    if (scope == nullptr || scope[0] == '\0')
        return;
    count = count < 0 ? 0 : count > kCustomCameraMax ? kCustomCameraMax : count;
    for (int n = 0; n < kCustomCameraMax; n++) {
        char key[96];
        snprintf(key, sizeof(key), "%s.%d", scope, n + 1);
        if (n >= count) {
            WritePrivateProfileStringA(kCustomCameras, key, nullptr, ini_path().c_str());
            continue;
        }
        const CustomCamera &c = list[n];
        char s[256];
        snprintf(s, sizeof(s), "%s %s %.3f %.3f %.3f %.1f %.1f %.1f %.1f %d %d %d %d %d %d %d %d %s",
                 kCustomFromNames[c.from >= 0 && c.from < kCustomFromCount ? c.from : 0],
                 kCustomTypeNames[c.type >= 0 && c.type < kCustomTypeCount ? c.type : 0], c.pos[0], c.pos[1], c.pos[2],
                 c.pan, c.tilt, c.angle, c.fov, c.rigid[0], c.rigid[1], c.rigid[2], c.rigid[3], c.rigid[4], c.rigid[5],
                 c.rigid[6], c.rigid[7], c.name);
        WritePrivateProfileStringA(kCustomCameras, key, s, ini_path().c_str());
    }
    pclog::write("settings: custom cameras of %s: %d", scope, count);
}

int render_distance()
{
    return g_renderDistance.load(std::memory_order_relaxed);
}

void set_render_distance(int percent)
{
    if (percent < kRenderDistanceMin)
        percent = kRenderDistanceMin;
    if (percent > kRenderDistanceMax)
        percent = kRenderDistanceMax;
    if (percent == g_renderDistance.load())
        return;
    g_renderDistance = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "RenderDistance", s, ini_path().c_str());
    pclog::write("settings: render distance %d%%", percent);
}

AntiAliasing anti_aliasing()
{
    return (AntiAliasing)g_aa.load(std::memory_order_relaxed);
}

void set_anti_aliasing(AntiAliasing aa)
{
    const int v = (int)aa;
    if (v < 0 || v >= kAntiAliasingCount || v == g_aa.load())
        return;
    g_aa = v;
    WritePrivateProfileStringA(kGraphics, "AntiAliasing", kAaNames[v], ini_path().c_str());
    pclog::write("settings: anti-aliasing %s", kAaNames[v]);
}

int render_scale()
{
    return g_renderScale.load(std::memory_order_relaxed);
}

void set_render_scale(int percent)
{
    percent = clamp_render_scale(percent);
    if (percent == g_renderScale.load())
        return;
    g_renderScale = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "RenderScale", s, ini_path().c_str());
    pclog::write("settings: resolution scale %d%%", percent);
}

int msaa_samples(AntiAliasing aa)
{
    switch (aa) {
    case AntiAliasing::msaa2: return 2;
    case AntiAliasing::msaa4: return 4;
    case AntiAliasing::msaa8: return 8;
    case AntiAliasing::agss2: return 2;     /* PC (2026-11-15): AGSS - the MSAA targets, shaded per sample */
    case AntiAliasing::agss4: return 4;
    case AntiAliasing::agss8: return 8;
    default: return 1;
    }
}

bool per_sample_shading(AntiAliasing aa)
{
    return aa == AntiAliasing::agss2 || aa == AntiAliasing::agss4 || aa == AntiAliasing::agss8;
}

const char *anti_aliasing_name(AntiAliasing aa)
{
    const int v = (int)aa;
    return v >= 0 && v < kAntiAliasingCount ? kAaNames[v] : "off";
}

void set_hud_max_aspect(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    float aspect = (float)w / (float)h;
    if (aspect < 4.0f / 3.0f)
        aspect = 4.0f / 3.0f;
    if (aspect > 32.0f / 9.0f)
        aspect = 32.0f / 9.0f;
    g_display.hudMaxAspect = aspect;
    char s[32];
    snprintf(s, sizeof(s), "%d:%d", w, h);
    WritePrivateProfileStringA(kSection, "HudMaxAspect", s, ini_path().c_str());
    pclog::write("settings: race HUD up to %s", s);
}

int car_kind_count()
{
    return g_carKindCount;
}

const CarKind &car_kind(int i)
{
    return g_carKinds[i];
}

const Display &display()
{
    return g_display;
}

void save_window(int x, int y, int w, int h)
{
    if (w < 320 || h < 240)
        return;
    g_display.windowX = x;
    g_display.windowY = y;
    g_display.windowW = w;
    g_display.windowH = h;
    g_display.windowPlaced = true;
    write_int("WindowX", x);
    write_int("WindowY", y);
    write_int("WindowWidth", w);
    write_int("WindowHeight", h);
}

MenuAspect menu_aspect()
{
    return (MenuAspect)g_menuAspect.load(std::memory_order_relaxed);
}

bool movie_stretch()
{
    return g_movieStretch.load(std::memory_order_relaxed);
}

void set_movie_stretch(bool on)
{
    if (on == g_movieStretch.load())
        return;
    g_movieStretch = on;
    WritePrivateProfileStringA(kSection, "MovieAspect", on ? "stretch" : "original", ini_path().c_str());
}

void set_menu_aspect(MenuAspect a)
{
    const int v = (int)a;
    if (v < 0 || v >= kMenuAspectCount || v == g_menuAspect.load())
        return;
    g_menuAspect = v;
    WritePrivateProfileStringA(kSection, "MenuAspect", kMenuAspectNames[v], ini_path().c_str());
    pclog::write("settings: menus %s", kMenuAspectLog[v]);
}

int language()
{
    return g_language.load(std::memory_order_relaxed);
}

void set_language(int lang)
{
    if (lang < kLanguageAuto || lang >= kLanguages || lang == g_language.load())
        return;
    g_language = lang;
    WritePrivateProfileStringA(kGame, "Language", language_name(lang), ini_path().c_str());
    pclog::write("settings: language %s (from the next start)", language_name(lang));
}

const char *language_name(int lang)
{
    return lang >= 0 && lang < kLanguages ? kLanguageNames[lang] : "auto";
}

/* PC (2026-11-16): [Interface] Language */
int interface_language()
{
    return g_interfaceLanguage.load(std::memory_order_relaxed);
}

void set_interface_language(int lang)
{
    if (lang < ui_text::kAuto || lang >= ui_text::kCount || lang == g_interfaceLanguage.load())
        return;
    g_interfaceLanguage = lang;
    ui_text::set_language(ui_text::resolve(lang));
    WritePrivateProfileStringA("Interface", "Language", ui_text::value(lang), ini_path().c_str());
    pclog::write("settings: interface language %s (%s)", ui_text::value(lang), ui_text::code(ui_text::language()));
}

void set_display_mode(DisplayMode m)
{
    if (m != DisplayMode::windowed && m != DisplayMode::borderless && m != DisplayMode::fullscreen)
        return;
    if (m == g_display.mode)
        return;
    g_display.mode = m;
    WritePrivateProfileStringA(kSection, "Mode", mode_name(m), ini_path().c_str());
    pclog::write("settings: display mode %s", mode_name(m));
}

} // namespace settings
