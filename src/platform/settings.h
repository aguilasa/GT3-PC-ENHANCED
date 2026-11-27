/* settings.h - the settings of GT3 PC (gt3pc.ini).
 *
 * gt3pc.ini lives in the game folder (paths::game_dir()); the launcher
 * writes it (docs/launcher.md, "Settings"), the game reads it at start and
 * writes back what the player changes in the game itself (the window's size
 * and place).  A missing file or key gives the default.
 *
 *   [Display]
 *   Mode=windowed | borderless | fullscreen
 *   Width=0, Height=0       fullscreen resolution (0: the desktop's)
 *   Refresh=0               fullscreen refresh rate in Hz (0: the nearest to 60)
 *   WindowWidth=1280, WindowHeight=960   the window's client size
 *   WindowX, WindowY        the window's place (missing: centred)
 *   HudMaxAspect=21:9       the widest the race HUD's canvas gets (W:H or a
 *                           number, 4:3 .. 32:9); a wider window centres it
 *                           (game/app/pc_aspect.h)
 *   SpanMonitors=           monitors the picture spans in borderless /
 *                           fullscreen mode (borderless either way), their
 *                           device names (\\.\DISPLAYn) with commas, any order
 *                           (left to right by their desktop places); empty:
 *                           one monitor
 *   SplitMonitor=auto       the 2-player split screen's second monitor
 *                           ([Graphics] SplitScreen=monitors): a device name,
 *                           or auto (the one nearest the game's)
 *
 *   [Graphics]              (the in-game settings menu, F2, writes it)
 *   AntiAliasing=off | fxaa | msaa2 | msaa4 | msaa8
 *                           fxaa: the race views' 3D picture filtered before
 *                           the HUD; msaa*: every picture multisampled
 *                           (docs/renderer.md "Anti-aliasing")
 *   RenderScale=100         the pictures' resolution, percent of the window's
 *                           (10 .. 400); above 100 they are filtered down to
 *                           it (supersampling) (docs/renderer.md "Targets")
 *   Lod=on | off            off: the models' levels of detail unused (always
 *                           the full detail; the split screen draws the full
 *                           course) (docs/renderer.md "Detail and distance")
 *   ReflectionDetail=original | full   the cars in the wet road's reflection:
 *                           the original's lowest detail and tyre cylinders,
 *                           or level 0 and the real tyres (whatever Lod)
 *   ReflectionResolution=normal | half | full   PC (2026-11-14): the road
 *                           reflection's buffer (64 x 128 GS pixels for the
 *                           whole screen) at the PS2's resolution, at half the
 *                           screen's, or at the screen's; its blur that much
 *                           finer (docs/renderer.md "Road reflection
 *                           resolution")
 *   RenderDistance=100      percent of the course's own draw distance
 *                           (100 .. 400)
 *   FrameCap=0              the shortest frame time in ms (a frame rate cap:
 *                           1000 / the rate), 1 .. 100; 0: none
 *   VSync=off | 1 | 2 | 3   vertical sync with at most that many frames
 *                           queued (docs/renderer.md "Frame rate")
 *   Fov=100                 the driving views' field of view, percent of
 *                           the camera's angle (50 .. 200; not the mirror)
 *   ReplayFov=100           the same for the replays' cameras
 *   ExtraCameras=off | on   on: the view button also steps a live race through
 *                           the cockpit view and the car's own cameras
 *                           (game/app/pc_detail.h)
 *   ChaseAngle=0            the chase camera's height round the car at rest,
 *                           degrees (-45 .. 45; above 0 looking down on the
 *                           car; game/app/pc_freecam.h)
 *   CockpitCamera=off | on  on: the cockpit view is the camera in the
 *                           driver's head (the head not drawn, the chase
 *                           camera's controls looking round), reached by
 *                           the view button with or without ExtraCameras
 *                           (game/app/pc_headcam.h)
 *   CockpitFov=90           its field of view, degrees (30 .. 120, vertical)
 *   (CockpitMenuModel)      PC (2026-11-11 .. 2026-11-15): no longer read -
 *                           per car model now, [CockpitMenuModels] below
 *   MenuWheels=off | on     PC (2026-11-12): on: the race cars' wheels are
 *                           their menu wheel models (3D spokes), loaded with
 *                           the race (game/app/pc_menu_wheels.h)
 *   MenuWheelsSpeed=20      the wheel speed, km/h, from which the race wheel
 *                           and its motion blur take over (0 .. 400; 0: the
 *                           menu wheel at every speed)
 *   MenuTires=off | on      PC (2026-11-13): on: the race cars' tyres are the
 *                           menus' - their 256 x 128 texture, 30 segments
 *                           round (game/app/pc_menu_tires.h)
 * [CockpitOffsets]          PC (2026-11-08): the cockpit camera's calibrated
 *   r_gt_00=x y z tilt      offsets per car model (cockpit_offset below)
 *   sb0097.2=x y z tilt     (".2": the second cockpit of a two-driver car)
 * [CockpitMenuModels]       PC (2026-11-15): the cockpit camera's own car
 *   r_gt_00=on              drawn with its menu model (the showroom's,
 *                           menu/cars/), per car model; the calibrator's row
 *                           (cockpit_menu_model below; no key: off)
 * [CustomCameras]           PC (2026-11-14): the user's own cameras on the
 *   global.1=from type x y z pan tilt angle fov r1 .. r8 name
 *   r_gt_00.1=...           view button, for every car / for one car model
 *                           (custom_cameras below)
 * [Graphics] (continued)
 *   SplitScreen=horizontal | vertical | monitors   the 2-player split screen:
 *                           one view above the other, side by side, or one
 *                           monitor each (from the next race or replay)
 *   SplitHud=digital | analog   the split screens' meters
 *   SingleHud=analog | digital  a single player's meters
 *   HudSafeZone=100         the race HUD's part of the window in percent (75 ..
 *                           100): below 100 the HUD is scaled down about the
 *                           window's centre to fit it (game/app/pc_aspect.h)
 *   ReplayLogo=on | off     off: no GT logo over the bottom right corner of
 *                           the replays (and the attract mode's demos)
 *   SmokeOpacity=normal | reduced | half   the tyre smoke and dust drawn at
 *                           100, 75 or 50 % of their opacity (T173
 *                           RaceSmoke::color)
 *   CarReflections=all | large | none   the environment map's reflections
 *                           on the cars (their env passes): on every part,
 *                           only on parts at least 24 pixels across (of the
 *                           640 x 448 frame: the near cars), or on none
 *                           (docs/renderer.md "Detail and distance")
 *   EnvMapDetail=scaled | original   the cars' environment map (the 256 x 256
 *                           buffer the car reflections sample, blurred 32
 *                           times a frame) at the resolution scale, or at the
 *                           PS2's size (less GPU, softer reflections;
 *                           docs/renderer.md 3, "Environment map detail")
 *   AnisotropicFiltering=off | 2 | 4 | 8 | 16   PC (2026-11-10): the 3D
 *                           textures' anisotropic filtering, at most that
 *                           many taps, weighted by their alpha (transparent
 *                           texels' hidden colours stay hidden)
 *   ReplayDepthOfField=original | reduced | off   the replays' depth of
 *                           field (T182 ReplayBlur): the game's own (its blur
 *                           chain composited over the whole screen up to 11
 *                           times a frame), with one step per doubling of
 *                           the blur behind the focus instead of two (8
 *                           composites), or none (the live race's plain
 *                           copy; docs/renderer.md "Detail and distance")
 *
 *   [Hud]                   (the settings menu writes it too)
 *   Color<name>=RRGGBB      a race HUD colour (platform/hud_colors.h: its keys);
 *                           missing or not 6 hex digits: the game's own
 *
 *   [Audio]                 (the settings menu writes it too)
 *   Output=auto | ks | wasapi   ks: kernel streaming straight to the driver
 *                           (the device is the game's alone while it plays);
 *                           wasapi: through the Windows audio engine; auto:
 *                           kernel streaming, else WASAPI (docs/sound.md 3.5)
 *   Buffer=2                the output period in ms (1 .. 20; the device
 *                           holds about two)
 *   MenuBgm=100, RaceBgm=50, ReplayBgm=50   the music's volume in the
 *                           menus, the races and the replays, percent
 *                           (0 .. 100; 100: the music files as they are)
 *   MenuSe=100, RaceSe=100, ReplaySe=100    the sound effects' (100: the
 *                           game's own level)
 *
 *   [Interface]             (PC 2026-11-16; the settings menu writes it too)
 *   Language=auto           GT3 PC's own text - the launcher, the settings
 *                           menu and the panels over the picture, not the
 *                           game: auto (Windows' display language when it is
 *                           one of these, else English), english, french,
 *                           german, italian, spanish, japanese, russian,
 *                           portuguese_br, polish (platform/ui_text.h)
 *
 *   [Game]                  (the settings menu writes it too)
 *   Language=auto           the game's language from the next start: auto
 *                           (the installed disc's own), japanese, english_us,
 *                           english_uk, french, german, italian, spanish
 *                           (platform/language.h)
 *   Version=US              US | EU | JP: the version the launcher last
 *                           played, played when the game starts without
 *                           --data (platform/paths.h reads it)
 *   SpeedUnit=auto          auto | kmh | mph: speed and distance (km/h, km, m
 *                           or mph, miles); auto: the language's own
 *   PowerUnit=auto          auto | ps | hp | bhp | cv | kw
 *   TorqueUnit=auto         auto | kgfm | ftlb | nm (game/app/pc_units.h; the
 *                           settings menu changes them at once)
 *   StartingCredits=auto    auto | 15000 | 18000 | custom: a new game's money
 *                           (auto: the version's own, its product config's
 *                           "initial": USA / Europe 18,000, Japan 15,000);
 *                           in Japanese x 100 (yen), as every price
 *                           (game/app/pc_new_game.h)
 *   StartingCreditsCustom=18000   PC (2026-11-14): custom's amount, 0 ..
 *                           999,999,999 (x 100 in Japanese too)
 *   CourseScaleX=100, CourseScaleY=100, CourseScaleZ=100   the courses'
 *                           geometry scaled along the world's x, y (up) and
 *                           z, percent (x, z: -1000 .. 1000, negative
 *                           mirrored, at least 5 from 0; y: 5 .. 1000): their
 *                           models, road, cameras and the rest (game/app/
 *                           pc_course_scale.h); the next course loaded takes
 *                           them (the launcher's Game > Extras, the settings
 *                           menu's GAME > EXTRAS)
 *   PrizeCarRandomizer=off  on: the prize cars drawn from the car database's
 *                           cars, not the prize sets' (game/app/
 *                           pc_prize_cars.h); the next prize car screen
 *                           takes it (Game > Extras, GAME > EXTRAS)
 *   UnrestrictedWheels=off  on: the wheel shop and the garage's wheel change
 *                           take every car - the eleven whose wheels the
 *                           game fixes too (the F1 cars, four concept cars:
 *                           game/app/pc_wheel_shop.h; Game > Extras, GAME >
 *                           EXTRAS)
 *   BuyNotForSale=off       on: the dealers sell the cars they show as not
 *                           for sale ("----": race, rally and concept cars),
 *                           at their database price, and the trade buys a
 *                           guest garage's car the game would refuse (game/
 *                           app/pc_car_shop.h; Game > Extras, GAME > EXTRAS)
 *   NewCarAssistsOff=off    on: a car added to the garage gets its TCS and
 *                           ASM settings at 0 (PC 2026-11-23; Game > Extras,
 *                           GAME > EXTRAS)
 *   CourseRandomizer=off    on: the GT mode's career races on a random course
 *                           of their discipline; extended: on any course of
 *                           the database, whatever its surface (tarmac,
 *                           dirt, wet; not the licence sections); the event
 *                           pages' course pictures "?" (game/app/
 *                           pc_course_pick.h; Game > Extras, GAME > EXTRAS)
 *   RevCut=original         realistic: the rev limiter cuts the fuel for a
 *                           time, the time some engine cycles take at the
 *                           cutting rpm (shorter at higher revs), not until
 *                           the revs fall 500 rpm (game/app/pc_rev_cut.h;
 *                           Game > Extras, GAME > EXTRAS)
 *   ShiftBehavior=original  realistic: a shift of a gear type 0 gearbox
 *                           holds the clutch out for a time (the gear part's
 *                           category, the engine's fall rate), then snaps it
 *                           in wherever the revs are, not when they reach
 *                           the new gear's (a player's downshift: the
 *                           player blips; game/app/pc_shift.h; Game >
 *                           Extras, GAME > EXTRAS); clutch: realistic, and
 *                           the players' manual gearboxes worked by their
 *                           own clutch (game/app/pc_clutch.h)
 *   HShifter=off            on (with ShiftBehavior=clutch): the players'
 *                           gears from their H-Shifters
 *   RaceCars=6             the cars of a race the game runs with 6 (6 ..
 *                           32; game/app/pc_race_cars.h; Game > Extras,
 *                           GAME > EXTRAS)
 *   RaceCarsOverride=off    on: also the races with opponents the game runs
 *                           with fewer (rally 2, wet courses 4)
 *   TireWearPlayer=100, TireWearEnemies=100   the cars' tyre wear (where a
 *                           race has it), percent (0 .. 1000)
 *   PowerPlayer=100, PowerEnemies=100   their engines' power (10 .. 1000)
 *   GripPlayer=100, GripEnemies=100     their tyres' grip (10 .. 1000)
 *                           Player: the players' cars; enemies: the
 *                           computer's.  The next race entered takes them
 *                           (game/app/pc_car_tuning.h; Game > Extras, GAME >
 *                           EXTRAS > CAR MULTIPLIERS)
 *   SurfaceGripTA=1.00 .. SurfaceGripWG=0.80   each surface type's tyre grip
 *                           (0.01 .. 10.00: the tyre table's value, x the
 *                           tyre's grip) and rolling resistance (0.00 ..
 *   SurfaceResistanceTA=0.10 .. SurfaceResistanceWG=0.10   327.67), the raw
 *                           values; missing or auto: the game's own.  The
 *                           types TA GU GR SA GV DT SF WD HT WR WH WG
 *                           (tarmac, kerbs, grass, sand, gravel, dirt, three
 *                           more road kinds, the wet ones).  The next race
 *                           entered takes them (game/app/pc_car_tuning.h;
 *                           Game > Surfaces, GAME > EXTRAS > SURFACE GRIP /
 *                           SURFACE RESISTANCE)
 *
 *   [Challenges]            PC (2026-11-14): the challenge modes (the
 *                           launcher's Game > Challenges, the settings menu's
 *                           CHALLENGE MODES, which writes them too; game/app/
 *                           pc_challenge.h), by the GT mode's day count:
 *   MoneyDecay=off | on     on: each day passed takes credits - days 2 .. 9
 *                           (day - 1) x 50, from day 10 (day - 9) x 500, at
 *                           most 200,000 a day (x 100 in Japanese); a licence
 *                           earned: none for the next 10 days
 *   CarDecay=off | on       on: the players' cars' power 0.25 % less for each
 *                           day passed, at least 5 %, in place of PowerPlayer
 *                           (the GT mode's races; PC 2026-11-15: not the
 *                           licence tests)
 *   CourseShrinkage=off | on   on: the courses 0.2 % smaller on each axis for
 *                           each day passed, at least 25 %, in place of
 *                           CourseScaleX / Y / Z (the GT mode's races)
 *
 *   [Debug]                 (the launcher's Debug tab; read at start)
 *   Console=off | on        on: the log (gt3pc.log: the PC side's messages
 *                           and the game's printf) also in a console window
 *                           of its own (platform/log.h open_console)
 *   FileLog=off | on        on: every file the game reads (with its size),
 *                           writes or misses in the log ("file: ...",
 *                           platform/host_file.h)
 *   PhysicsThreads=auto     worker threads for the cars' own physics steps
 *                           (auto: the processor's logical cores less one,
 *                           at most 16; 0 off .. 16; the launcher's Debug
 *                           tab; game/race/dynamics/pc_physics_threads.h)
 *   SyncLog=off | on        on: what makes the game thread wait for the
 *                           renderer's D3D thread, each caller once ("pgl
 *                           sync: ...", engine/pgl/native/pgl_queue.h)
 *   ReplaySync=off | on     on: a race's cars kept frame by frame and its
 *                           replay compared with them ("PC: replay sync:
 *                           ...", game/race/dynamics/pc_replay_sync.h)
 *   CameraYLook=off | on    on: the debug camera's forward and back follow
 *                           its pitch (the profile's Y-axis look control
 *                           sets it; platform/debug_camera.h)
 *   CameraGround=off | on   on: the debug camera follows the road, up and
 *                           down its height above it (the profile's Ground
 *                           collision control sets it; the same)
 *   FillGarage=off | on     a new game's garage: one of every car of the
 *                           database in every colour (game/app/pc_new_game.h)
 *   FillMoney=off | on      a new game's money: 100,000,000 (x 100 in
 *                           Japanese)
 *   UnlockArcade=off | on   every course of the arcade modes in each of them
 *                           (Cote d'Azur in two players, slalom80r in single
 *                           race...), all open (a new game's and a loaded
 *                           one's); off, the product config decides
 *   UnlockLicenses=off | on every licence held (each test at least bronze; a
 *                           new game's and a loaded one's)
 *                           (the four above: also the settings menu's DEBUG,
 *                           which writes them)
 *   CourseForcer=off | on   on: every race entered (not a replay or a demo)
 *   ForcedCourse=<label>    on the course of that label ("trialmt"): any file
 *                           of the course folder (the settings menu's DEBUG
 *                           > COURSE FORCER only; game/app/pc_course_pick.h)
 *   LapForcer=off | on      on: every lap race entered runs ForcedLaps laps
 *   ForcedLaps=3            (1 .. 99; the settings menu's DEBUG > LAP FORCER
 *                           only; game/app/pc_course_pick.h)
 *   RaceTimeForcer=off|on   on: every race entered with a time limit (the
 *   ForcedRaceTime=10       two-hour endurance races) runs ForcedRaceTime
 *                           minutes (1 .. 999; the settings menu's DEBUG >
 *                           RACE TIME FORCER only)
 *   CourseModeForcer=off    finish | 400 | 1000 | max: what ends every race
 *                           entered (not a replay, a demo, a machine or a
 *                           licence test): the course's own finish line
 *                           (the licence tests'), or a line 400 / 1000 /
 *                           6845 m along the lap with no lap count (the
 *                           machine tests'); the settings menu's DEBUG >
 *                           COURSE MODE FORCER only; game/app/
 *                           pc_course_pick.h
 *   MenuRaceModel=off | on  on: the menus' cars drawn with their race models
 *                           (cars/day/<car>: the body, the race wheel)
 *   MenuRaceModelLod=auto   0 | 1 | 2: the race model's level of detail
 *                           forced (0 the finest); auto: as the game picks
 *                           ([Graphics] Lod); game/app/pc_menu_race_model.h
 *
 *   [Textures]              the texture replacements (engine/pgl/native/
 *                           pgl_texrepl.h; the texture interface, F10:
 *                           platform/texture_menu.h, which writes the four
 *                           below; the folders: the launcher's Textures...)
 *   Replace=on | off        on: a texture with a file of its hash's name in
 *                           a texture folder is drawn from that file
 *   Dump=off                select | all: the textures written as PNG files
 *                           named by their hashes - all: each as it is first
 *                           decoded, into tex_replacements\dumps; select:
 *                           the one chosen in the interface, into
 *                           tex_replacements\dumps_selected
 *   PreCache=on | off       on: each replacement file's ready-to-load form
 *                           kept in cache\textures (made ahead, in the
 *                           background, and at a file's first use)
 *   Compress=on | off       on: a .png / .jpg replacement held block-
 *                           compressed (BC1 / BC3: a quarter to an eighth
 *                           of the video memory); off: as 32-bit pixels. A
 *                           .dds file is used as it is
 *   Count=N                 the texture folders (the launcher's Textures...
 *   Pack1=<folder>          manager): 1 the first, which wins over the ones
 *   Pack1On=on | off        after it for a texture both have
 *
 *   [Controller1], [Controller2], [Wheel1], [Wheel2]   the controls'
 *                           mappings and dead zones (the launcher's Controls
 *                           tab; the settings menu writes the dead zones):
 *                           platform/input_map.h, read by load
 *
 *   [CarKinds]              (no launcher UI; read at start) the enhanced
 *   <car code>=<kind>       engine's special kind for a car code, beyond the
 *                           built-in table (T282): for a car whose model was
 *                           transplanted (the Pod's on another car), e.g.
 *                           celica_gt_four_95=pod.  Kinds: pod, sl, wing_gtr,
 *                           wing_xf, wing_cien, wing_w12, none (or the
 *                           number). */
#pragma once

#include <stdint.h>

namespace settings {

enum class DisplayMode { windowed, borderless, fullscreen };

struct Display {
    DisplayMode mode = DisplayMode::windowed;
    int width = 0, height = 0;      /* fullscreen; 0: the desktop's */
    int refresh = 0;                /* fullscreen; 0: the nearest to 60 Hz */
    int windowW = 1280, windowH = 960;
    bool windowPlaced = false;      /* windowX / windowY valid */
    int windowX = 0, windowY = 0;
    float hudMaxAspect = 21.0f / 9.0f;  /* the race HUD canvas's widest aspect */
    char spanMonitors[256] = "";    /* SpanMonitors (empty: none) */
    char splitMonitor[64] = "";     /* SplitMonitor (empty: auto) */
};

/* PC (2026-11-15, user request: "Another Anti-aliasing setting: NxAGSS ...
 * GPU's own sample grid. 2x / 4x / 8x AGSS beside the MSAA ones"): agss2 ..
 * agss8, N x Automatic Grid SuperSampling - MSAA's targets and resolve with
 * the 3D models' pixels shaded at every sample (docs/renderer.md
 * "Anti-aliasing"). */
enum class AntiAliasing { off, fxaa, msaa2, msaa4, msaa8, agss2, agss4, agss8 };
constexpr int kAntiAliasingCount = 8;

/* Reads gt3pc.ini (after paths::init). */
void load();
const Display &display();

/* The window's client size and place in windowed mode, written to
 * gt3pc.ini at once. */
void save_window(int x, int y, int w, int h);
/* [Display] Mode, changed in the game (the settings menu's SCREEN MODE,
 * which switches the window to it too), written to gt3pc.ini at once. */
void set_display_mode(DisplayMode m);
/* [Display] MenuAspect=original | stretch: the menus (every frame without a
 * race view) at 4:3 with bars, or stretched over the window, their car
 * models and texts squeezed back to their shape (game/app/pc_aspect.h).
 * Any thread; changed in the game (the settings menu), written at once.
 * PC (2026-11-15, user request: "Menu aspect: 16:9 option"): 16:9 (ini value
 * "16:9") - stretched as by stretch, over the largest 16:9 rectangle of the
 * window, bars where the window is wider or narrower. */
enum class MenuAspect { original, wide, stretch };
constexpr int kMenuAspectCount = 3;
MenuAspect menu_aspect();
void set_menu_aspect(MenuAspect a);
/* [Display] MovieAspect=original | stretch: a movie in its own shape (an
 * MP4's; a PSS's is 4:3), as big as its place lets it, black around it, or
 * over the whole of its place (a full-screen movie's: the window), whatever
 * MenuAspect is (game/app/pc_aspect.h pc_movie_fit).  Any thread; changed in
 * the game (the settings menu), written at once. */
bool movie_stretch();
void set_movie_stretch(bool on);
/* PC (2026-11-19, user request: "Make this a setting in the display
 * category: 'Game Clock Sync': Variable, Static NTSC.  Variable is the
 * default"): [Display] GameClockSync=variable | ntsc - variable, the game's
 * vblanks on the display's when it refreshes at 59.90 .. 60.10 Hz or 2, 3, 4
 * times that (platform/gpu.cpp's watcher, scheduler.h set_display_clock);
 * ntsc, the PS2's 59.94 Hz always.  Any thread; changed in the game (the
 * settings menu: at once), written at once. */
bool game_clock_variable();
void set_game_clock_variable(bool on);
/* The race HUD canvas's widest aspect w:h, changed in the game (held to
 * 4:3 .. 32:9), written to gt3pc.ini at once. */
void set_hud_max_aspect(int w, int h);

/* [Graphics]: read by any thread (the renderer each frame), changed in the
 * game (the settings menu) and written to gt3pc.ini at once. */
AntiAliasing anti_aliasing();
void set_anti_aliasing(AntiAliasing aa);
/* The samples per pixel of an MSAA mode (1 for the others).  PC
 * (2026-11-15): an AGSS mode's too (its targets are MSAA's). */
int msaa_samples(AntiAliasing aa);
/* PC (2026-11-15): an AGSS mode - the 3D draws shaded per sample. */
bool per_sample_shading(AntiAliasing aa);
const char *anti_aliasing_name(AntiAliasing aa);     /* the ini value */
/* [Graphics] RenderScale: the pictures' resolution in percent of the
 * window's (100: the window's own). */
constexpr int kRenderScaleMin = 10, kRenderScaleMax = 400;
int render_scale();
void set_render_scale(int percent);
/* [Graphics] Lod: the levels of detail used (the original game), else the
 * full detail always. */
bool lod_enabled();
void set_lod_enabled(bool on);
/* PC: [Graphics] ReflectionDetail: the cars mirrored in a wet road (T131
 * drawCarReflections, into a small blurred buffer) - original: the
 * original game's lowest level of detail and tyre cylinders, whatever Lod;
 * full: every model's level 0 and the real tyres, whatever Lod. */
enum class ReflectionDetail { original, full };
constexpr int kReflectionDetailCount = 2;
ReflectionDetail reflection_detail();
void set_reflection_detail(ReflectionDetail d);
/* PC (2026-11-14): [Graphics] ReflectionResolution: the road reflection's
 * buffer (T131 drawReflections: the course's reflW x reflH, 64 x 128 GS
 * pixels stretched over the 640 x 448 screen) drawn finer by the renderer -
 * normal: the PS2's; half: half the screen's resolution on each axis; full:
 * the screen's - and its blur that much finer (game/app/pc_detail.h
 * pc_reflection_scale).  Any thread; the next frame takes it. */
enum class ReflectionResolution { normal, half, full };
constexpr int kReflectionResolutionCount = 3;
ReflectionResolution reflection_resolution();
void set_reflection_resolution(ReflectionResolution r);
/* [Graphics] RearViewBumper=on, RearViewChase=off: a live race's rear-view
 * mirror with the bumper camera (the original's) / the chase camera (none
 * in the original), on or off (game/app/pc_detail.h). */
enum class RearViewCamera { bumper, chase };
constexpr int kRearViewCameras = 2;
bool rear_view(RearViewCamera c);
void set_rear_view(RearViewCamera c, bool on);
/* [Graphics] ExtraCameras: the view button also steps a live race through
 * the cockpit view and the car's own cameras (game/app/pc_detail.h). */
bool extra_cameras();
void set_extra_cameras(bool on);
/* [Graphics] ChaseAngle: the chase camera's height round the car at rest, in
 * degrees (above 0 looking down on the car; 0 the original), where the swing
 * of its controls comes back to (game/app/pc_freecam.h). */
constexpr int kChaseAngleMin = -45, kChaseAngleMax = 45;
int chase_angle();
void set_chase_angle(int degrees);
/* PC (2026-11-07) [Graphics] CockpitCamera: the cockpit view is the camera in
 * the driver's head - the head model not drawn, the chase camera's controls
 * looking round, its own field of view - and the view button reaches it with
 * or without ExtraCameras (game/app/pc_headcam.h).  CockpitFov: that field
 * of view in degrees (vertical, as the debug camera's; 90 the user's
 * tuning, 75 at first). */
bool cockpit_camera();
void set_cockpit_camera(bool on);
constexpr int kCockpitFovMin = 30, kCockpitFovMax = 120, kCockpitFovDefault = 90;    /* (75 at first; 90: user, 2026-11-07) */
int cockpit_fov();
void set_cockpit_fov(int degrees);
/* PC (2026-11-11) the cockpit camera's own car drawn with its menu model
 * (user: "use the menu model of a car while the cockpit cam is active";
 * game/app/pc_headcam.h).  PC (2026-11-15, user: "Cockpit menu model needs
 * to be per-car.  The option needs to be part of the calibrator"): per car
 * model, [CockpitMenuModels] <model>=on (the key as [CockpitOffsets]'s, no
 * slot: both cockpits), set by the calibration's save (platform/
 * cockpit_osd.h); was [Graphics] CockpitMenuModel for every car.
 * cockpit_menu_model: read at each call (the race's side keeps it); set_:
 * written at once, off deletes the key.  Any thread. */
bool cockpit_menu_model(const char *model);
void set_cockpit_menu_model(const char *model, bool on);
/* PC (2026-11-12) [Graphics] MenuWheels: the race cars' wheels drawn with
 * their menu (showroom) wheel models, 3D spokes, in place of the race wheels
 * (user: "loads the menu wheel models instead of the race wheels";
 * game/app/pc_menu_wheels.h).  MenuWheelsSpeed: the wheel speed (km/h at the
 * tyre) from which the race wheel and its motion blur take over; 0: the
 * menu wheel at every speed. */
bool menu_wheels();
void set_menu_wheels(bool on);
constexpr int kMenuWheelsSpeedMin = 0, kMenuWheelsSpeedMax = 400, kMenuWheelsSpeedDefault = 20;
int menu_wheels_speed();
void set_menu_wheels_speed(int kmh);
/* PC (2026-11-13) [Graphics] MenuTires: the race cars' tyres built as the
 * menus build them, with the menu tyre file's texture (user: "use the tire's
 * menu model instead of the race one"; game/app/pc_menu_tires.h). */
bool menu_tires();
void set_menu_tires(bool on);
/* PC (2026-11-08) [CockpitOffsets]: the cockpit camera's calibrated offsets
 * per car model (platform/cockpit_osd.h, game/app/pc_headcam.h): the key
 * the model file's name ("r_gt_00"; ".2" after it for the second cockpit of
 * a car with two drivers), the value "x y z tilt" - the eye's shift in the
 * car's frame, metres (x the car's right, y up, z back), and the view's
 * tilt in degrees (up positive).  cockpit_offset: out, zeros and false when
 * the model has none.  set_: written at once (read by the game only here:
 * no cache); all four 0 deletes the key.  Any thread. */
bool cockpit_offset(const char *model, int slot, float out[4]);
void set_cockpit_offset(const char *model, int slot, const float v[4]);
/* PC (2026-11-14) [CustomCameras]: the user's own cameras on the view button
 * (user: "User makes custom cameras per model code.  Also global cameras as
 * offsets the same way the cockpit cam works, but these need unrestricted
 * camera position limits.  Options for it: Camera Rigidity settings for each
 * way the camera deviates from the position slightly based on the car's
 * movement.  The existing chase cam angle setting should only apply to the
 * base chase cam.  The user-made ones get their own"; game/app/
 * pc_customcam.h, docs/renderer.md "Custom cameras").  A key is
 * `<scope>.<n>': the scope "global" (every car) or a car's model name
 * ("r_gt_00": that model alone), n from 1 without gaps, kCustomCameraMax at
 * most a scope.  Its value, in this order:
 *   from type x y z pan tilt angle fov r1 r2 r3 r4 r5 r6 r7 r8 name
 * from: where the offset starts, center (the car's origin: on the ground at
 * its middle) | bumper (the bumper camera's eye) | chase (the chase
 * camera's) | cockpit (the cockpit camera's: the driver's head with the
 * model's calibration); type: chase (the camera controls swing it round the
 * car; the chase camera's sounds, HUD and rear view setting) | onboard (they
 * look round from the eye; the bumper camera's); x y z: the offset in the
 * car's frame, metres - x the car's right, y up, z back - without limits;
 * pan, tilt: the view turned right / up, degrees; angle: the camera swung up
 * over the car as ChaseAngle swings the chase camera, degrees (its own:
 * ChaseAngle is the chase camera's alone); fov: degrees, vertical; r1 .. r8:
 * the rigidities, 0 .. 100 % - how firmly the camera is fixed to the car
 * against each way the game's cameras move with it (CustomRigid: 100 none of
 * it, 0 all of it); name: what the view button shows, the rest of the line.
 * custom_cameras: the scope's cameras read from the file (their count);
 * set_custom_cameras: the scope's cameras written at once, the keys past
 * `count' deleted.  The game's thread. */
enum CustomFrom { kCustomFromCentre, kCustomFromBumper, kCustomFromChase, kCustomFromCockpit, kCustomFromCount };
enum CustomType { kCustomTypeChase, kCustomTypeOnboard, kCustomTypeCount };
/* The ways a camera of the game moves with its car, each with a rigidity:
 * the chase camera's lag behind the car's turn and behind its pitch, and its
 * level horizon (100: the camera rolls with the car); the in-car cameras'
 * shake that grows with the speed; the cockpit camera's shake while the
 * tyres slip; the in-car cameras' sideways and vertical springs under the
 * car's acceleration; the bumper camera's sway while the tyres slip. */
enum CustomRigid {
    kCustomRigidTurn, kCustomRigidPitch, kCustomRigidRoll, kCustomRigidSpeedShake, kCustomRigidSlipShake,
    kCustomRigidSide, kCustomRigidBumps, kCustomRigidSway, kCustomRigidCount
};
constexpr int kCustomCameraMax = 8;
constexpr float kCustomFovMin = 10.0f, kCustomFovMax = 150.0f, kCustomAngleMax = 90.0f;
struct CustomCamera {
    char  name[24];
    int   from;                                 /* CustomFrom */
    int   type;                                 /* CustomType */
    float pos[3];
    float pan, tilt, angle, fov;
    int   rigid[kCustomRigidCount];
};
int custom_cameras(const char *scope, CustomCamera out[kCustomCameraMax]);
void set_custom_cameras(const char *scope, const CustomCamera *list, int count);
/* [Graphics] RenderDistance: percent of the original draw distance. */
constexpr int kRenderDistanceMin = 100, kRenderDistanceMax = 400;
int render_distance();
void set_render_distance(int percent);
/* [Graphics] FrameCap: the shortest frame time in ms (0: no cap; else
 * kFrameCapMin .. kFrameCapMax, kept to 0.001 ms). */
constexpr double kFrameCapMin = 1.0, kFrameCapMax = 100.0;
double frame_cap_ms();
void set_frame_cap_ms(double ms);
/* [Graphics] VSync: 0 off, else vertical sync with at most 1 .. 3 frames
 * queued. */
int vsync();
void set_vsync(int frames);
/* [Graphics] Fov: the driving views' field of view in percent of the
 * camera's own (kFovMin .. kFovMax; 100 the original). */
constexpr int kFovMin = 50, kFovMax = 200;
int fov_percent();
void set_fov_percent(int percent);
/* [Graphics] ReplayFov: the same for the replays' cameras (the attract
 * mode's demo races too). */
int replay_fov_percent();
void set_replay_fov_percent(int percent);
/* [Graphics] SplitScreen: the 2-player split screen's views one above the
 * other (horizontal, the default), side by side (vertical), or each on a
 * monitor of its own (monitors: player 2 on [Display] SplitMonitor,
 * borderless - the game window too meanwhile; side by side when there is
 * no second monitor).  A race takes it at its start (game/app/pc_aspect.h
 * pc_split_side). */
enum class SplitScreen { horizontal, vertical, monitors };
constexpr int kSplitScreenCount = 3;
SplitScreen split_screen();
void set_split_screen(SplitScreen s);
/* [Graphics] SplitHud=digital | analog: the split screens' meters (the
 * original's digital set, or the single player's analog ones);
 * SingleHud=analog | digital: a single player's (the original's analog
 * sets, or the split screen's digital one).  A race takes a change at once. */
bool split_hud_analog();
void set_split_hud_analog(bool on);
bool single_hud_digital();
void set_single_hud_digital(bool on);
/* [Graphics] HudSafeZone: the race HUD's part of the window in percent
 * (100: all of it, the original layout). */
constexpr int kHudSafeZoneMin = 75, kHudSafeZoneMax = 100;
int hud_safe_zone();
void set_hud_safe_zone(int percent);
/* [Hud] Color<name>: race HUD colour i (HudColor, platform/hud_colors.h),
 * 0xRRGGBB.  hud_color: what it is now.  hud_color_set: the same, or
 * kHudColorGame while it is the game's own.  Set to the game's own, its key
 * goes from the file. */
constexpr uint32_t kHudColorGame = 0xFFFFFFFFu;
uint32_t hud_color(int i);
uint32_t hud_color_set(int i);
void set_hud_color(int i, uint32_t rgb);
/* [Graphics] ReplayLogo: the GT logo the replays (and the attract mode's
 * demos) show over their bottom right corner, on (the original) or off.
 * A replay takes a change at once. */
bool replay_logo();
void set_replay_logo(bool on);
/* [Graphics] SmokeOpacity: the tyre smoke and dust (T173 RaceSmoke) at their
 * own opacity (normal, the original), 75 % of it (reduced) or 50 % (half).
 * Any thread; a race takes a change at once. */
enum class SmokeOpacity { normal, reduced, half };
constexpr int kSmokeOpacityCount = 3;
SmokeOpacity smoke_opacity();
void set_smoke_opacity(SmokeOpacity o);
int smoke_opacity_percent(SmokeOpacity o);     /* 100, 75, 50 */
/* PC (2026-11-05): [Graphics] CarReflections: the environment map's
 * reflections on the cars (the VU1 env passes into the frame, two
 * rasterizations each: pgl_exec.cpp submit_vu1) - all: every part (the
 * original); large: only parts at least car_reflection_min_px pixels wide
 * or tall in the 640 x 448 frame (the near cars; a far car's are a few
 * pixels); none.  Any thread; a race takes a change at once. */
enum class CarReflections { all, large, none };
constexpr int kCarReflectionsCount = 3;
CarReflections car_reflections();
void set_car_reflections(CarReflections c);
int car_reflection_min_px();                    /* 0 all, 24 large, a huge number for none */
/* PC (2026-11-06): [Graphics] EnvMapDetail: the cars' environment map - a
 * 256 x 256 GS buffer the course's env view is copied into and blurred in, 32
 * sprite passes a frame, which the env passes sample for the car bodies'
 * reflections - at the resolution scale like every target (scaled, the PC's
 * picture so far: 1152 x 926 at 150 %, 34 Mpx of blur a frame), or at its GS
 * size as the PS2 had it (original: the blur over 65k pixels, the
 * reflections softer; pgl_targets.cpp targets_env_maps).  Any thread; a race
 * takes a change at its next frame. */
enum class EnvMapDetail { scaled, original };
constexpr int kEnvMapDetailCount = 2;
EnvMapDetail env_map_detail();
void set_env_map_detail(EnvMapDetail d);
/* PC (2026-11-10): [Graphics] AnisotropicFiltering=off | 2 | 4 | 8 | 16 - the
 * 3D textures' colours over their footprint on the screen (the road and the
 * walls seen at a grazing angle sharper), up to that many taps along it,
 * each weighted by its alpha: a texture's transparent texels lend no colour
 * (pgl_d3d.cpp aniso_rgb; docs/renderer.md "Anisotropic filtering").  1 off.
 * Any thread; a race takes a change at its next frame. */
constexpr int kAnisotropicCount = 5;        /* off, 2x, 4x, 8x, 16x */
int anisotropic_filtering();                /* 1, 2, 4, 8, 16 */
void set_anisotropic_filtering(int most);   /* (rounded down to one of them) */
/* PC (2026-11-07): [Graphics] ReplayDepthOfField: the replays' depth of
 * field (T182 ReplayBlur, drawn by T131 drawView in replays of courses with
 * the flag): the frame downsampled into a chain of five levels, which are
 * composited over the whole screen depth-tested at the distance of their
 * blur - three in front of the focus, the sharp frame, and behind it two a
 * doubling of the blur (farSteps 2: each level at half alpha, then opaque)
 * up to 8 pixels: 11 full-screen draws, 47 Mpx of a 118 Mpx frame at 150 %
 * (the user's capture).  original: the game's; reduced: one step a doubling
 * behind the focus (farSteps 1: each level once, opaque; 8 draws); off: no
 * depth of field (the live race's plain copy).  Any thread; a replay takes a
 * change at its next frame. */
enum class ReplayDepthOfField { original, reduced, off };
constexpr int kReplayDepthOfFieldCount = 3;
ReplayDepthOfField replay_depth_of_field();
void set_replay_depth_of_field(ReplayDepthOfField d);

/* [Audio]: read by the audio thread, changed in the game (the settings
 * menu: the output restarts) and written to gt3pc.ini at once.  Output:
 * auto | ks | wasapi; PC (2026-11-15, user request) wasapi by default (was
 * auto). */
enum class AudioOutput { automatic, ks, wasapi };
constexpr int kAudioOutputCount = 3;
AudioOutput audio_output();
void set_audio_output(AudioOutput out);
const char *audio_output_name(AudioOutput out);     /* the ini value */
/* [Audio] Buffer: the output period in ms (kept to 0.1 ms). */
constexpr double kAudioBufferMin = 1.0, kAudioBufferMax = 20.0;
double audio_buffer_ms();
void set_audio_buffer_ms(double ms);
/* Bumped by each [Audio] change (the audio thread restarts the output). */
int audio_generation();
/* [Audio] MenuBgm, RaceBgm, ReplayBgm, MenuSe, RaceSe, ReplaySe: the
 * volumes of the music and of the sound effects in the menus, the races and
 * the replays (the attract mode's demo races too), in percent (0 .. 100):
 * levels for the game's own six on / off options (GameOptions menuBgm ..
 * replaySe, which still switch each off).  The music files play straight
 * at 100, at their own level as Windows plays them (docs/sound.md 2.4); the
 * sound effects at the game's.  Defaults: the race and replay music 50, the
 * rest 100.  Any thread (the game reads them as it plays: a change is heard
 * at once); changed in the game (the settings menu), written to gt3pc.ini at
 * once.  Not an output change (audio_generation stays). */
enum class Volume { menuBgm, raceBgm, replayBgm, menuSe, raceSe, replaySe };
constexpr int kVolumeCount = 6;
int volume(Volume v);
void set_volume(Volume v, int percent);
int volume_default(Volume v);

/* [Game] Language: kLanguageAuto, or a language of platform/language.h (0
 * Japanese .. 6 Spanish, the game's Country values).  The game takes it at
 * its start; changed in the game (the settings menu), written at once. */
constexpr int kLanguageAuto = -1;
int language();
void set_language(int lang);
const char *language_name(int lang);    /* the ini value ("auto", "english_uk"...) */
/* PC (2026-11-16): [Interface] Language - ui_text::kAuto or a language of
 * platform/ui_text.h; the settings menu's text changes at once (the
 * launcher's at its next start). */
int interface_language();
void set_interface_language(int lang);
/* [Game] SpeedUnit / PowerUnit / TorqueUnit: kUnitAuto (the language's
 * preset) or the game's UnitSettings value (speed: 0 metric, 1 imperial;
 * power: 0 PS, 1 HP, 2 BHP, 3 CV, 4 kW; torque: 0 kgf.m, 1 ft.lb, 2 Nm).
 * Any thread; changed in the game, written at once; units_generation bumps
 * with each change (the game applies it at its next tick). */
constexpr int kUnitAuto = -1;
constexpr int kSpeedUnits = 2, kPowerUnits = 5, kTorqueUnits = 3;
int speed_unit();
int power_unit();
int torque_unit();
void set_speed_unit(int u);
void set_power_unit(int u);
void set_torque_unit(int u);
int units_generation();

/* [Game] StartingCredits: what a new game's money is - the version's own
 * (auto), 15,000, 18,000 or (PC 2026-11-14; user: "The starting credits
 * feature, add a custom option to it") custom: [Game] StartingCreditsCustom,
 * any amount of kStartingCreditsCustomMin .. Max.  A number written as
 * StartingCredits itself (other than the two) is read as custom with that
 * amount.  Changed in the game, written at once; starting_credits: the
 * credits a new game takes, or -1 for the version's own (0 is an amount). */
enum class StartingCredits { automatic, c15000, c18000, custom };
constexpr int kStartingCreditsCount = 4;
constexpr int kStartingCreditsCustomMin = 0, kStartingCreditsCustomMax = 999999999,
              kStartingCreditsCustomDefault = 18000;
StartingCredits starting_credits_mode();
void set_starting_credits_mode(StartingCredits mode);
int starting_credits_custom();
void set_starting_credits_custom(int credits);
int starting_credits();

/* [Game] CourseScaleX / CourseScaleY / CourseScaleZ: the courses' geometry
 * scaled along the world's axes (axis 0 x, 1 y, 2 z), percent, up to
 * kCourseScaleMax (100: as it is; game/app/pc_course_scale.h).  X and Z go
 * down to kCourseScaleMin, negative: mirrored along the axis, never nearer
 * 0 than kCourseScaleNearest (a course of no size along an axis: 0 is taken
 * as that, with its sign); Y (the height) has kCourseScaleNearest as its
 * least (PC 2026-11-07: no upside-down course; a negative or 0 is taken as
 * 5 %): course_scale_min.  Read at start (the launcher's Game > Extras); set
 * (the settings menu's GAME > EXTRAS): held to the axis's range, written at
 * once, taken by the next course loaded.  Any thread. */
constexpr int kCourseScaleMin = -1000, kCourseScaleMax = 1000, kCourseScaleNearest = 5;
int course_scale_min(int axis);
int course_scale(int axis);
void set_course_scale(int axis, int percent);
/* percent held to the setting's values for the axis (above). */
int clamp_course_scale(int axis, int percent);
/* percent held to what any axis could once be set to (kCourseScaleMin ..
 * kCourseScaleMax, not within kCourseScaleNearest of 0): a replay's or a
 * ghost's recorded scale, which may predate the height's floor and plays as
 * it was run (game/app/pc_course_scale.cpp block_scale). */
int clamp_course_scale_recorded(int percent);

/* [Game] PrizeCarRandomizer: the prize cars drawn from the car database
 * instead of the prize sets (game/app/pc_prize_cars.h).  Read at start (the
 * launcher's Game > Extras); set (the settings menu's GAME > EXTRAS): written
 * at once, taken by the next prize car screen.  Any thread. */
bool prize_car_randomizer();
void set_prize_car_randomizer(bool on);

/* PC (2026-11-13): [Game] UnrestrictedWheels: aftermarket wheels for the
 * cars whose wheels the game fixes too (game/app/pc_wheel_shop.h).  Read at
 * start (the launcher's Game > Extras); set (the settings menu's GAME >
 * EXTRAS): written at once, taken by the next visit of the wheel shop.  Any
 * thread. */
bool unrestricted_wheels();
void set_unrestricted_wheels(bool on);

/* PC (2026-11-14): [Game] BuyNotForSale: the cars the game does not sell
 * (shop flag bits 0 and 1) bought as any other (game/app/pc_car_shop.h).
 * Read at start (the launcher's Game > Extras); set (the settings menu's
 * GAME > EXTRAS): written at once, taken by the next page shown.  Any
 * thread. */
bool buy_not_for_sale();
void set_buy_not_for_sale(bool on);

/* PC (2026-11-23, user: "An option to toggle driving assists for new cars
 * added to the garage: When a car is added, the game has a default TCS and
 * ASM setting.  While the setting is on, cars that get added have those 2
 * settings set to 0 / off"): [Game] NewCarAssistsOff - a car added to the
 * garage (bought new or used, won, the debug fill's) gets its TCS and ASM
 * settings at their least, the settings page's 0 (GameStatus::addCar, T429);
 * the cars already there keep theirs.  Read at start (the launcher's Game >
 * Extras); set (the settings menu's GAME > EXTRAS): written at once, taken
 * by the next car added.  Any thread. */
bool new_car_assists_off();
void set_new_car_assists_off(bool on);

/* PC (2026-11-14): [Challenges] MoneyDecay, CarDecay, CourseShrinkage: the
 * challenge modes (game/app/pc_challenge.h).  Read at start (the launcher's
 * Game > Challenges); set (the settings menu's CHALLENGE MODES): written at
 * once; the money at the next day passed, the power and the course's size
 * from the next race entered, the panel at once.  Any thread. */
enum class Challenge { moneyDecay, carDecay, courseShrinkage };
constexpr int kChallengeCount = 3;
bool challenge(Challenge c);
void set_challenge(Challenge c, bool on);

/* [Game] CourseRandomizer: the GT mode's career races (the leagues' events)
 * on a random course of their discipline (on), or of any - every course of
 * the database with a file but the licence sections, whatever its surface
 * (extended; PC 2026-11-07) - the events' course pictures a "?" (game/app/
 * pc_course_pick.h).  course_randomizer: on or extended.  Read at start (the
 * launcher's Game > Extras); set (the settings menu's GAME > EXTRAS): written
 * at once, taken by the next event entered and the next event page.  Any
 * thread. */
enum class CourseRandomizer { off, on, extended };
constexpr int kCourseRandomizerCount = 3;
bool course_randomizer();
CourseRandomizer course_randomizer_mode();
void set_course_randomizer(CourseRandomizer mode);

/* PC (2026-11-08) [Game] RevCut ("rev cut behavior"): the engines' rev limiter - original
 * (the fuel cut at the redline until the revs are 500 rpm below it) or
 * realistic (a cut of the time some engine cycles take at the cutting rpm:
 * shorter at higher revs, longer at lower; game/app/pc_rev_cut.h).  Read at start (the launcher's Game > Extras); set (the
 * settings menu's GAME > EXTRAS): written at once, taken by the next race
 * entered.  Any thread. */
enum class RevCut { original, realistic };
constexpr int kRevCutCount = 2;
RevCut rev_cut();
void set_rev_cut(RevCut mode);
const char *rev_cut_name(RevCut mode);      /* "original", "realistic" (the ini's) */

/* PC (2026-11-08) [Game] ShiftBehavior ("gear shift behavior"): the shifts
 * of a gear type 0 gearbox (GearCalc.gearboxType 0) - original (the clutch
 * out until the revs reach the new gear's, then locked) or realistic (out
 * for a time - the gear part's category, the engine's fall rate - then
 * snapped in wherever the revs are; game/app/pc_shift.h).  Read
 * at start (the launcher's Game > Extras); set (the settings menu's GAME >
 * EXTRAS): written at once, taken by the next race entered.  Any thread. */
enum class ShiftBehavior { original, realistic, clutch };
constexpr int kShiftBehaviorCount = 3;
ShiftBehavior shift_behavior();
void set_shift_behavior(ShiftBehavior mode);
const char *shift_behavior_name(ShiftBehavior mode);    /* "original", "realistic", "clutch" (the ini's) */
/* PC (2026-11-09): ShiftBehavior=clutch ("Realistic + Clutch"): the realistic
 * shifts, and the players' manual gearboxes worked by their own clutch
 * (game/app/pc_clutch.h); [Game] HShifter=off | on (shown with it): their
 * gears from their H-Shifters (input_map.h kHShifter1 / 2).  Read at
 * start; set: written at once, taken by the next race entered.  Any
 * thread. */
bool h_shifter();
void set_h_shifter(bool on);

/* [Game] TireWearPlayer / TireWearEnemies, PowerPlayer / PowerEnemies,
 * GripPlayer / GripEnemies: the cars' tyre wear, engine power and tyre grip
 * in percent of the game's own (100), the players' cars and the computer's
 * (enemies) (game/app/pc_car_tuning.h).  Held to car_tune_min ..
 * kCarTuneMax.  Read at start (the launcher's Game > Extras); set (the
 * settings menu's GAME > EXTRAS > CAR MULTIPLIERS): written at once, taken
 * by the next race entered.  Any thread. */
/* [Game] RaceCars: the cars of a race the game runs with 6 (the GT mode's
 * races and championships, the arcade's single races...), kRaceCarsMin ..
 * kRaceCarsMax (game/app/pc_race_cars.h).  Read at start (the launcher's
 * Game > Extras); set (the settings menu's GAME > EXTRAS): written at
 * once, taken by the next race entered.  Any thread. */
constexpr int kRaceCarsMin = 6, kRaceCarsMax = 32;
int race_cars();
void set_race_cars(int cars);
/* [Game] RaceCarsOverride: on, the races with opponents the game runs with
 * fewer than 6 cars (the rally events and the arcade's dirt races 2, the wet
 * courses 4) take RaceCars too.  Read at start; set (the settings menu):
 * written at once, taken by the next race entered.  Any thread. */
bool race_cars_override();
void set_race_cars_override(bool on);

enum class CarTune { tireWear, power, grip };
constexpr int kCarTunes = 3;
constexpr int kCarTuneMax = 1000;
int car_tune_min(CarTune t);                    /* tyre wear 0, the others 10 */
int car_tune(CarTune t, bool enemies);
void set_car_tune(CarTune t, bool enemies, int percent);

/* [Game] SurfaceGrip<code> / SurfaceResistance<code>: each road surface
 * type's tyre grip (the tyre table's ForceVol<code>: x the tyre's grip) and
 * rolling resistance (DynParams.rollingResistance), as the raw values, in
 * hundredths here (1.25 -> 125); kSurfaceOwn (the key missing or "auto"):
 * the game's own (game/app/pc_car_tuning.h).  The surfaces (s 0 ..
 * kSurfaces - 1) are the types the courses use, by the database's names
 * (surface_key: "TA" tarmac ...); surface_code gives the game's code of s
 * (0 .. 15), surface_tune_default the game's own value (the US tyre table's
 * one row, DynParams).  Held to surface_tune_min .. surface_tune_max.  Read
 * at start (the launcher's Game > Surfaces); set (the settings menu's GAME
 * > EXTRAS > SURFACE GRIP / SURFACE RESISTANCE): written at once (the own:
 * the key removed), taken by the next race entered.  Any thread. */
enum class SurfaceTune { grip, resistance };
constexpr int kSurfaces = 12;
constexpr int kSurfaceOwn = -1;
const char *surface_key(int s);
int surface_code(int s);
int surface_tune_min(SurfaceTune t);            /* grip 1 (0.01), resistance 0 */
int surface_tune_max(SurfaceTune t);            /* grip 1000 (10.00), resistance 32767 (327.67) */
int surface_tune_default(int s, SurfaceTune t);
int surface_tune(int s, SurfaceTune t);         /* hundredths, or kSurfaceOwn */
void set_surface_tune(int s, SurfaceTune t, int hundredths);

/* [CarKinds], in the file's order: the car code's name and the kind's
 * (as written, trimmed). */
struct CarKind {
    char code[48];
    char kind[16];
};
int car_kind_count();
const CarKind &car_kind(int i);

/* [Debug] Console: the log in a console window too. */
bool debug_console();
/* [Debug] FileLog: the files read and written in the log. */
bool debug_file_log();
/* [Debug] SyncLog: the waits for the renderer's D3D thread in the log. */
bool debug_sync_log();
/* [Debug] ReplaySync: a replay checked against its race, frame by frame. */
bool debug_replay_sync();
/* [Debug] CameraYLook: the debug camera moves forward and back along its
 * look, pitch included (else in the heading's horizontal plane). */
bool debug_camera_y_look();
void set_debug_camera_y_look(bool on);
/* [Debug] CameraGround: the debug camera stays a height above the road,
 * which its vertical moves change (else they move it freely). */
bool debug_camera_ground();
void set_debug_camera_ground(bool on);
/* [Debug] PhysicsThreads: worker threads for the cars' own physics steps of
 * a frame (0 off .. 16, or kPhysicsThreadsAuto: the processor's logical
 * cores less the game thread's, at most 16; GT3PC_PHYSICS_THREADS
 * overrides it; game/race/dynamics/pc_physics_threads.h).  Read at
 * start. */
constexpr int kPhysicsThreadsAuto = -1;
int physics_threads();
/* [Debug] FillGarage, FillMoney (a new game's), UnlockArcade, UnlockLicenses
 * (a new game's and a loaded one's): game/app/pc_new_game.h. */
bool debug_fill_garage();
bool debug_fill_money();
bool debug_unlock_arcade();
bool debug_unlock_licenses();
/* Set by the settings menu's DEBUG: written at once, taken when the game
 * reads them (a new game, a loaded one; UnlockArcade's course lists at the
 * next start).  Any thread. */
void set_debug_fill_garage(bool on);
void set_debug_fill_money(bool on);
void set_debug_unlock_arcade(bool on);
void set_debug_unlock_licenses(bool on);

/* [Debug] CourseForcer, ForcedCourse: every race entered (not a replay or a
 * demo) on the course ForcedCourse names (its label, "trialmt"; game/app/
 * pc_course_pick.h).  Read at start; set by the settings menu's DEBUG >
 * COURSE FORCER: written at once, taken by the next race entered.  Any
 * thread. */
bool course_forcer();
void set_course_forcer(bool on);
void forced_course(char *out, int n);           /* "" none */
void set_forced_course(const char *label);
/* The courses the forcer offers, published by the game once it has read
 * them (game/app/pc_course_pick.h, at its start): a course's label and its
 * start places (-1: not known), in the order shown. */
struct CourseChoice {
    char label[48];
    int places;
};
void set_course_choices(const CourseChoice *choices, int count);
int course_choice_count();
CourseChoice course_choice(int i);              /* (an empty one past the end) */

/* [Debug] LapForcer, ForcedLaps: every lap race entered (not a replay, a
 * demo or a machine test; a time-limit race stays one) runs ForcedLaps laps
 * (kForcedLapsMin .. kForcedLapsMax; game/app/pc_course_pick.h).  Read at
 * start; set by the settings menu's DEBUG > LAP FORCER: written at once,
 * taken by the next race entered.  Any thread. */
constexpr int kForcedLapsMin = 1, kForcedLapsMax = 99;
bool lap_forcer();
void set_lap_forcer(bool on);
int forced_laps();
void set_forced_laps(int laps);

/* [Debug] RaceTimeForcer, ForcedRaceTime: every race entered with a time
 * limit (the two-hour endurance races; not a replay, a demo or a machine
 * test; a race without one stays without) runs ForcedRaceTime minutes
 * (kForcedRaceTimeMin .. kForcedRaceTimeMax; game/app/pc_course_pick.h).
 * Read at start; set by the settings menu's DEBUG > RACE TIME FORCER:
 * written at once, taken by the next race entered.  Any thread. */
constexpr int kForcedRaceTimeMin = 1, kForcedRaceTimeMax = 999;
bool race_time_forcer();
void set_race_time_forcer(bool on);
int forced_race_time();
void set_forced_race_time(int minutes);

/* PC (2026-11-07): [Debug] CourseModeForcer: what ends every race entered
 * (not a replay, a demo, a machine test or a licence test; game/app/
 * pc_course_pick.h) - off: the race's own lap count; finish: the course's
 * own finish line, the runway's separate line where the licence tests end
 * (a course without one keeps its laps); 400 / 1000 / max: a line 400 m,
 * 1000 m or 6845 m along the lap and no lap count, as the machine tests
 * (a line past the lap goes 10 m before its end).  Read at start; set by
 * the settings menu's DEBUG > COURSE MODE FORCER: written at once, taken by
 * the next race entered.  Any thread. */
enum class CourseMode { off, finish, m400, m1000, maxSpeed };
constexpr int kCourseModeCount = 5;
CourseMode course_mode_forcer();
void set_course_mode_forcer(CourseMode m);

/* PC (2026-11-12): [Debug] MenuRaceModel: the menus' cars (the showroom,
 * the garage, the prize cars...) loaded from their race model files instead
 * of their menu ones; MenuRaceModelLod: that model's level of detail, -1
 * auto (the game's choice), else 0 .. kMenuRaceModelLodMax.  Read at start;
 * set by the settings menu's DEBUG > MENU RACE MODEL (a menu car shown
 * loads again) and the launcher's Debug tab.  Any thread. */
bool menu_race_model();
void set_menu_race_model(bool on);
constexpr int kMenuRaceModelLodMax = 2;
int menu_race_model_lod();
void set_menu_race_model_lod(int level);

/* PC (2026-11-13): [Textures]: the texture replacements (engine/pgl/native/
 * pgl_texrepl.h).  Read at start; the four switches set by the texture
 * interface (platform/texture_menu.h), written at once and taken by the
 * renderer at its next frame.  The folders are read at start only (the
 * launcher's texture manager writes them): texture_pack(i) the i-th folder
 * that is on, the first the one that wins, without a trailing '\\'.
 * texture_root: the game folder's tex_replacements\\ (dumps\\ and
 * dumps_selected\\ in it); texture_cache: its cache\\textures\\.  Any
 * thread. */
bool texture_replace();
void set_texture_replace(bool on);
enum class TextureDump { off, select, all };
constexpr int kTextureDumpCount = 3;
TextureDump texture_dump();
void set_texture_dump(TextureDump m);
bool texture_precache();
void set_texture_precache(bool on);
bool texture_compress();
void set_texture_compress(bool on);
int texture_pack_count();
void texture_pack(int i, char *out, int n);
void texture_root(char *out, int n);
void texture_cache(char *out, int n);

const char *mode_name(DisplayMode m);

} // namespace settings
