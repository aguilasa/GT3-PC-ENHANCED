/* T130_race_ps2_base.cpp - RaceEntryPS2, RacePS2Base and RaceBGMPS2
 * (0x0017CF30..0x0017E6C0; the classes: include/game/race/core/T130_race_ps2_base.h).
 *
 * RacePS2Base is the PS2 layer between RaceBase (T129) and the race modes
 * (RaceBasic, T132, and everything built on it).  It holds, as members, the
 * per-race effect managers (ReplayBlur, FlareClass, RaceSmoke,
 * ParticleManager, RigidBodyManager), the error view, the race BGM
 * (RaceBGMPS2), the ambient sound (RaceAmbientSound), the rear-view mirror
 * rectangle and the pad input (RaceInput, attached to the pad manager as slot
 * 0x15), plus the two malloc'ed weather buffers (G001).  Its overrides wrap
 * RaceBase's: virtual_8 (start) wires the effect managers into the course and
 * copies the music play list from the game options, virtual_28 / 29 run the
 * managers and the cars' sounds each frame, virtual_35 (end) writes the play
 * list back.  virtual_41 is the race loop the game modes call (start, sleep
 * until the race thread signals the end, the transition, end; again while
 * endCode == 4).  The renderer half (render, drawView .. addCourseLights, slots 44..55) is T131 (u045).
 *
 * RaceBGMPS2 plays the race music: code 0x14 takes the next song of the play
 * list (or, in the demo, BGM group 0x22), other codes start a random track of
 * that group.  Every frame it follows 32 channels of beat markers of the
 * current song: markerTime[ch] is the time to the next marker, markerHit[ch]
 * says that a marker passed this frame (CameraManager cuts the replay
 * cameras on them, T383 / T386).
 *
 * The three operator new / delete pairs are class-specific copies of the race
 * pool allocator (the same pool as RaceBase / RaceOrganization, T123).
 *
 * #pragma implementation (egcs only, first lines): the pair of
 * T130_race_ps2_base.h's #pragma interface, so the out-of-line copies of the
 * inline accessors of RacePS2Base and RaceBGMPS2, the vtables and the
 * typeinfo functions are emitted here, at the end, in the listing's order
 * (the #line names this file: the SCE driver compiles a temporary .ii, see
 * T530).
 *
 * Compiler notes: egcs passes `this + delta` to virtual calls (delta 0
 * here); registers that only held a value the compiler needed elsewhere are
 * not arguments (checked in the callees where it mattered: lcg_rand_seed,
 * RigidBodyManager::reset, ParticleManager::clear take one argument).
 *
 * Includes (u044's data pass): the classes retail's T130 saw, in its order.
 * Its .rodata (0x00327C58..0x003280C8) is "bad_alloc" (<new>), the three
 * vtables, then the type names in reverse definition order: OperatorList,
 * List / OperatorHolder, OperatorHolder, Cell / OperatorHolder, the own
 * three (RacePS2Base, RaceBGMPS2, RaceEntryPS2), RaceCameraBase, the
 * GameStatus's garage lists, UnitArena<64>, and Operator (the
 * non-polymorphic base, after all the others).  So UnitArena<64> is
 * instantiated first (T153), the GameStatus (T429) comes before RaceBase's
 * RaceCameraBase, and the menu operator classes (T216) after T130's own
 * header.  start_transition comes from T005's entry header (retail's T130
 * has none of the transition objects' names).
 */
// DATA_START 0x00327C58
#include "types.h"
#include "libc.h"                           /* malloc, free */
#include <new>                              /* (no use: "bad_alloc", the first .rodata string) */
#define GAME_T153_EMITS_TYPEINFO            /* UnitArena<64>'s type name */
#include "game/race/core/T153_race_course.h"          /* RaceCourse (data, smoke, rigid, particle), setupRigidBodies */
#include "game/save/T429_game_status.h"          /* getGameStatus, GameStatus::options */
#include "game/camera/T383_race_camera_base.h"     /* (RaceCameraBase, through RaceBase) */
#include "game/race/core/T130_race_ps2_base.h"        /* own: + T121 RaceEntryBase / RaceEntryCar, T129 RaceBase,
                                               T175 RaceBGMBase, T430 MusicPlayList, T182 ReplayBlur,
                                               T159 FlareClass, T173 RaceSmoke, T165
                                               ParticleManager, T167 RigidBodyManager (and its
                                               statics), T185 ErrorEventView, T177
                                               RaceAmbientSound, T413 RaceInput */
#include "game/menu/framework/T216_menu_transitive.h"      /* (no use: the menu operator classes retail's T130 saw) */
#include "game/app/T002_render_thread.h"        /* render_set_object */
#include "game/app/T005_transition_entry.h"     /* start_transition */
#include "game/menu/framework/T246_render_base.h"          /* TransitionType */
#include "game/race/core/T123_race_setup.h"           /* the race pool g_002B3BF0 (pdistd::Allocator) */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization (entries, course, race, racePreviewMode) */
#include "game/course/T179_course_data.h"          /* CourseData (smokeTex, env, objs), F_001A9180 / 9768 / 97E8 */
#include "game/course/T178_course_env.h"           /* CourseEnv (weatherDrops), course_env_lights_off, F_001A8678 */
#include "game/sound/T176_car_sound.h"            /* car_sound_update_se_switch, g_003521E4 */
#include "game/car/model/T180_race_car_model.h"       /* RaceCarModel (state, updateBodyState) */
#include "game/menu/race_menu/T197_race_menu_page.h"       /* race_menu_heap_push / race_menu_heap_pop */
#include "game/effects/T276_envmap_render.h"        /* envmapInit, F_001D8280 */
#include "game/race/dynamics/T305_dynamics_conductor.h"   /* DynamicsConductor::boxes */
#include "game/camera/T383_camera_manager.h"       /* CameraManager (views, current), CameraView */
#include "game/camera/T396_camera_request.h"       /* CameraRequest::cameraChanged */
#include "game/save/T428_game_options.h"         /* GameOptions (raceBgm, replayBgm, music) */
#include "game/sound/T464_bgm.h"                  /* bgm_play, bgm_get_tracks, bgm_stop */
#include "game/sound/T466_bgm_ads.h"              /* BgmTrack, BgmMarkers, BgmTrackList */
#include "engine/input/T474_control.h"              /* Control::getButtons */
#include "engine/input/T475_pad_manager.h"          /* PadManager g_002EA7F8 (attach, detach) */
#include "engine/pdi/T521_pdibgm.h"               /* pdibgm_pause / resume / set_volume / get_status */
#include "engine/pgl/G001_flare.h"                /* lcg_rand_seed, weather_init / set / update,
                                               pgl_matrix_mode, gfx_vsync, vu0_load_identity_regs,
                                               Tex1Image */
#include "game/race/core/race_ps2_base_internal.h"                           /* the unit's constants */
#include "game/app/pc_frame.h"                  /* PC: pc_frame_request_interpolation */
#include "game/app/pc_debugcam.h"               /* PC: pc_debugcam_race */
#include "game/app/pc_race_songs.h"             /* PC: the race's song changer */
#include "game/app/pc_headcam.h"                /* PC (2026-11-11): the cockpit camera's menu model */
#include "game/app/pc_menu_wheels.h"            /* PC (2026-11-12): the menu wheels */
#include "game/app/pc_menu_tires.h"             /* PC (2026-11-13): the menu tyres */
#include "game/app/pc_clutch.h"                 /* PC (2026-11-23): pc_hshifter_alert_arm */

/* ======================================================================== */
/* RaceEntryPS2                                                             */
/* ======================================================================== */

// FUNC 0x0017CF30 RaceEntryPS2::operator new
//   From the race pool, 16-byte aligned.  Callers: the *Information::
//   virtual_4 functions of the race modes (they build the entry list).
void *RaceEntryPS2::operator new(size_t size)
{
    return g_002B3BF0.allocate(size, RACE_POOL_ALIGN);
}

// FUNC 0x0017CF58 RaceEntryPS2::operator delete
//   The size (0x3C) is ignored.
void RaceEntryPS2::operator delete(void *p, size_t size)
{
    g_002B3BF0.free(p);
}

// FUNC 0x0017CF80 RaceEntryPS2::RaceEntryPS2
//   RaceEntryBase::RaceEntryBase, the vptr (+0x1C), then a capacity of 7
//   cars stored inline.
//   PC: PC_RACE_CARS_MAX (game/race/race_cars.h), the slots cleared.
RaceEntryPS2::RaceEntryPS2()
{
    capacity = PC_RACE_CARS_MAX;            /* PC: was 7 */
    cars = slots;
    for (int i = 0; i < PC_RACE_CARS_MAX; i++)
        slots[i] = NULL;                    /* PC */

    /* implicit: RaceEntryBase (the base's constructor) */
}

// FUNC 0x0017CFC8 RaceEntryPS2::~RaceEntryPS2
//   The vptr reset, RaceEntryBase::~RaceEntryBase (flags 0), operator
//   delete(this, 0x3C) when flags & 1.  Also called by
//   RaceSolitaireEntry::~RaceSolitaireEntry.
RaceEntryPS2::~RaceEntryPS2()
{
    /* implicit: ~RaceEntryBase (the base / member destructors) */
}

/* ======================================================================== */
/* RacePS2Base: allocation, construction, destruction                       */
/* ======================================================================== */

// FUNC 0x0017D020 RacePS2Base::operator new
//   From the race pool, 16-byte aligned.  Unreferenced.
void *RacePS2Base::operator new(size_t size)
{
    return g_002B3BF0.allocate(size, RACE_POOL_ALIGN);
}

// FUNC 0x0017D048 RacePS2Base::operator delete
//   The size (0xC880 from the destructor) is ignored.
void RacePS2Base::operator delete(void *p, size_t size)
{
    g_002B3BF0.free(p);
}

// FUNC 0x0017D070 RacePS2Base::RacePS2Base
//   RaceBase::RaceBase, the vptr, then the members in order: ReplayBlur
//   (blur), FlareClass (flare), RaceSmoke (smoke), ParticleManager
//   (particles), RigidBodyManager (rigidBodies), ErrorEventView
//   (errorView), RaceBGMPS2 (bgmPS2), RaceAmbientSound (ambientSound),
//   RaceInput (input).  The body attaches the input to the pad manager
//   (slot 0x15), allocates the two weather buffers, installs the BGM in
//   RaceBase::bgm and pushes the race menu heap.  The exception landing
//   pads destroy the members built so far (flags 0; the GSBuffer of blur
//   with flags 2: ~ReplayBlur is implicit) and RaceBase.  Caller:
//   RaceBasic::RaceBasic.
RacePS2Base::RacePS2Base()
{
    /* implicit: RaceBase, ReplayBlur, FlareClass, RaceSmoke, ParticleManager,
     * RigidBodyManager, ErrorEventView, RaceBGMPS2, RaceAmbientSound,
     * RaceInput (the base's and the members' constructors), ~RaceInput,
     * ~RaceAmbientSound, ~RaceBGMPS2, ~ErrorEventView, ~RigidBodyManager,
     * ~ParticleManager, ~RaceSmoke, ~FlareClass, ~GSBuffer, ~RaceBase (their
     * cleanup on a throw) */
    g_002EA7F8.attach(&input, RACE_PS2_INPUT_SLOT);
    weather = (Weather *)malloc(WEATHER_SIZE);
    weatherDrops = (WeatherDrop *)malloc(WEATHER_DROPS_SIZE);
    field_BCC0 = 0;
    bgm = &bgmPS2;
    race_menu_heap_push();
    pc_debugcam_race(1);                    /* PC: the debug camera's hotkey from now (game/app/pc_debugcam.h) */
}

// FUNC 0x0017D2B8 RacePS2Base::~RacePS2Base
//   The vptr reset, race_menu_heap_pop, frees the two weather buffers,
//   detaches the input from the pad manager, then the members in reverse
//   order (flags 2): ~RaceInput, ~RaceAmbientSound, ~RaceBGMPS2,
//   ~ErrorEventView, ~RigidBodyManager, ~ParticleManager, ~RaceSmoke,
//   ~FlareClass, blur's GSBuffer; RaceBase::~RaceBase (flags 0); operator
//   delete(this, 0xC880) when flags & 1.  Callers: RaceBasic::RaceBasic
//   (its landing pad), RaceBasic::~RaceBasic.
RacePS2Base::~RacePS2Base()
{
    pc_debugcam_race(0);                    /* PC: the debug camera off with the race */
    race_menu_heap_pop();
    free(weatherDrops);
    free(weather);
    g_002EA7F8.detach(&input);
    /* implicit: ~RaceBase, ~RaceBGMPS2, ~FlareClass, ~ParticleManager,
     * ~RigidBodyManager, ~RaceSmoke, ~RaceAmbientSound, ~ErrorEventView,
     * ~RaceInput, ~GSBuffer (the base / member destructors) */
}

/* ======================================================================== */
/* RacePS2Base: the race loop                                               */
/* ======================================================================== */

// FUNC 0x0017D3D8 RacePS2Base::virtual_41
//   Runs one race to its end (11 callers: the game modes).  Starts it
//   (virtual_8(replay)), runs virtual_43, hands the object to the render
//   thread, then sleeps (virtual_38) until the race thread signals the end,
//   starts the delayed fade-out transition, waits two vsyncs and ends the
//   race (virtual_35).  With retry != 0 the whole thing is repeated while
//   endCode == 4 (RaceBasic's virtual_34(4): the pause menu's retry).
void RacePS2Base::virtual_41(int replay, int retry)
{
    do {
        virtual_8(replay);
        virtual_43();
        /* The enhanced engine (backend.md B2): no wheel spring while racing
         * (Lupo 2 L2 0x0017B168) */
        if (replayMode == RACE_LIVE) {
            virtual_25()->setMode(0);
            virtual_25()->pcFfbNotice();    /* PC (2026-11-20): GT3's force feedback option off (game/app/pc_ffb.h) */
        }
        /* PC (2026-11-23): the notice that the H-shifter is off, while a race
         * is driven (game/app/pc_clutch.h) */
        pc_hshifter_alert_arm(replayMode == RACE_LIVE);
        render_set_object(this);
        while (virtual_38() != RACE_SIGNAL_END)
            ;
        pc_hshifter_alert_arm(0);
        start_transition(TRANSITION_DELAY_FADEOUT, 1);
        gfx_vsync(2);
        virtual_35();
    } while (retry != 0 && endCode == RACE_END_RETRY);
    virtual_25()->setMode(1);               /* the enhanced engine: the spring back on (L2 0x0017B240) */
}

// FUNC 0x0017D4C8 RacePS2Base::virtual_42
//   When the race thread signalled the end (takeSignal), restarts the race
//   in place: virtual_35 (end), virtual_8 with the same replayMode, the same
//   backgroundMode again (setBackgroundMode, 0x0017BBB8); renderEnabled is
//   0 meanwhile and restored.
//   Returns 1 when it restarted, else 0.
int RacePS2Base::virtual_42()
{
    int restarted = 0;
    if (takeSignal() == RACE_SIGNAL_END) {
        int render = renderEnabled;
        renderEnabled = 0;
        int replay = replayMode;
        int background = backgroundMode;
        virtual_35();
        virtual_8(replay);
        setBackgroundMode(background);
        renderEnabled = render;
        restarted = 1;
    }
    return restarted;
}

// FUNC 0x0017D580 RacePS2Base::virtual_8
//   Starts the race.  Clears endRequested, runs RaceBase::virtual_8, then
//   sets up the course side: F_001A9180 on the course data; the course gets
//   pointers to the smoke, the rigid bodies and the particles; the smoke is
//   reset and takes the course's smoke texture and environment; the
//   particles are cleared; RigidBodyManager's statics take the conductor's
//   car boxes, the course and its gadget shapes before the rigid bodies are
//   reset and the course adds its own (setupRigidBodies); the random seed
//   0x7F7F7F7F; the weather is set up for the environment (weather_init
//   with env->weatherDrops, F_001A8678: weather_set); the blur's f-number 9;
//   the error view and the environment map (envmapInit) are initialised;
//   the BGM takes the play list of the game options (both copies);
//   car_sound_update_se_switch(replayMode != 0); and the afterimage is reset.
//   The managers through local pointers (the accessors) and the blur through
//   &blur: retail keeps &smoke, &particles and &rigidBodies in s0..s2 and
//   stores smoke.env / blur.fNumber through them (a member access would
//   fold the offset into this + 0x8000); the play lists through RaceBGMPS2's
//   inline setPlayList / commitPlayList (&bgmPS2 + 0x198 / 0x5A8) and
//   GameStatus::getGameOptions (&options = + 0x2FF38).
void RacePS2Base::virtual_8(int replay)
{
    endRequested = 0;
    pc_replay_speed_reset();                /* PC: each replay from the normal speed */
    RaceBase::virtual_8(replay);
    F_001A9180(org->course->data);

    RaceSmoke *smk = getSmoke();
    ParticleManager *pm = getParticles();
    RigidBodyManager *rb = getRigidBodies();
    org->course->smoke = smk;
    org->course->particle = pm;
    org->course->rigid = rb;
    smk->reset();
    smk->setTexture(org->course->data->smokeTex);
    smk->env = org->course->data->env;
    pm->clear();

    /* RigidBodyManager's static setters, inline (T167): the stores of the
     * statics, no calls. */
    RaceCourse *course = org->course;
    RigidBodyManager::SetCarBoxes(&dc->boxes);
    RigidBodyManager::SetCourse(course);
    RigidBodyManager::SetGadgetShapes(course->data->objs->obj);
    rb->reset();
    org->course->setupRigidBodies();

    CourseEnv *env = org->course->data->env;
    lcg_rand_seed(RACE_RANDOM_SEED);
    weather_init(weather, weatherDrops, env->weatherDrops);
    F_001A8678(env, weather);
    ReplayBlur *dof = &blur;
    dof->fNumber = RACE_BLUR_F_NUMBER;
    errorView.reset();
    envmapInit();

    bgmPS2.setPlayList(getGameStatus()->getGameOptions()->music);
    bgmPS2.commitPlayList();
    car_sound_update_se_switch(replayMode != 0);

    g_003521E4 = 1.0f;
    afterimageTime = 1.0f;
    afterimage = 0.0f;
    frameBits = 0;
}

// FUNC 0x0017D7D8 RacePS2Base::virtual_43
//   Empty hook run by virtual_41 after the start.
void RacePS2Base::virtual_43()
{
}

// FUNC 0x0017D7E0 RacePS2Base::virtual_35
//   Ends the race: F_001D8280 (T276, empty; Lupo 2 matched it to
//   DynamicsConductor::ReportTimeDifference), RaceBase::virtual_35,
//   course_env_lights_off (the environment's lights off) and the BGM's play list
//   (playlist2: the one that was played) written back into the game
//   options (getPlayList2 / getGameOptions: retail adds 0xBCF0, then
//   0x5A8).  Caller: RaceBasicWithRaceDisplay::virtual_35.
void RacePS2Base::virtual_35()
{
    F_001D8280();
    RaceBase::virtual_35();
    course_env_lights_off(org->course->data->env);
    getGameStatus()->getGameOptions()->music = *bgmPS2.getPlayList2();
}

// FUNC 0x0017D888 RacePS2Base::startSounds
//   RaceBase::startSounds (the cars' sounds, BGM code -3), then the ambient
//   sound starts.
void RacePS2Base::startSounds()
{
    RaceBase::startSounds();
    ambientSound.start();
}

// FUNC 0x0017D8B8 RacePS2Base::stopSounds
//   RaceBase::stopSounds (the cars' sounds, BGM code -2), then the ambient
//   sound stops.
void RacePS2Base::stopSounds()
{
    RaceBase::stopSounds();
    ambientSound.stop();
}

// FUNC 0x0017D8E8 RacePS2Base::virtual_27
//   The frame gate.  After virtual_33 (endRequested) it ends the race
//   frame (endRace) and returns 0.  While bit 29 of the input's buttons
//   (Control::getButtons, called directly: the member's type is known) is
//   set it returns 0.  Otherwise the error view runs a frame and
//   RaceBase::virtual_27 decides.  Callers: RaceArcadeDemo::virtual_27,
//   RaceLinkBattle::virtual_27.
int RacePS2Base::virtual_27()
{
    if (endRequested != 0) {
        endRace();
        return 0;
    }
    if (input.getButtons() & RACE_INPUT_HOLD_FRAME)
        return 0;
    errorView.update(org);
    return RaceBase::virtual_27();
}

// FUNC 0x0017D960 RacePS2Base::virtual_28
//   RaceBase::virtual_28(dt), then the per-frame updates: the weather, the
//   course data's animations (course_step_anims with -1), the flares, the smoke,
//   the rigid bodies, and the particles, told whether the current view's
//   camera changed this frame (CameraRequest::cameraChanged).  The
//   afterimage fades by dt / afterimageTime, not below 0 (max.s, race_maxf).
//   Callers: RaceBasicWithRaceDisplay::virtual_28, RaceLinkBattle::
//   virtual_28.
/* max.s: a volatile asm statement, as every max.s / min.s of the image
 * behaves (u018's T055, u032's T077): egcs's `>?' gives the same max.s, but
 * sched2 then moves the epilogue's loads into the div.s latency, which
 * retail does not (and sched1 orders virtual_28 otherwise).  GCC 14: the C
 * ternary (the same result but for -0.0 against 0.0). */
static inline f32 race_maxf(f32 a, f32 b)
{
    return a > b ? a : b;
}

void RacePS2Base::virtual_28(f32 dt)
{
    RaceBase::virtual_28(dt);
    weather_update(weather);
    course_step_anims(org->course->data, -1, dt);
    flare.update(dt);
    smoke.update(dt);
    rigidBodies.update(dt);

    CameraManager *cameras = virtual_6();
    particles.update(cameras->views[cameras->current].request.cameraChanged, dt);

    afterimage = race_maxf(afterimage - dt / afterimageTime, 0.0f);
}

// FUNC 0x0017DA58 RacePS2Base::virtual_29
//   RaceBase::virtual_29(dt), then the cars.  Unless backgroundMode: the
//   model-view matrix mode, the VU0 identity registers and the camera's
//   slot 2 first.  For every entry: unless backgroundMode, its sound
//   (updateSound, no skid sound in replays); while its model is enabled,
//   its sparks into the particles and its lamps.  Unless backgroundMode,
//   the ambient sound's frame at the end.
void RacePS2Base::virtual_29(f32 dt)
{
    RaceBase::virtual_29(dt);
    RaceOrganization *organization = org;
    CameraManager *cameras = virtual_6();
    /* PC: under the debug camera the sounds are heard at it (game/app/
     * pc_debugcam.h: its view, its motion for the Doppler) */

    if (backgroundMode == 0) {
        pgl_matrix_mode(PGL_MTX_MODELVIEW);
        vu0_load_identity_regs();
        cameras->applyView();
    }
    int count = organization->entries->count;   /* compared signed (blez / slt) */
    for (int i = 0; i < count; i++) {
        RaceEntryCar *car = organization->entries->cars[i];
        if (backgroundMode == 0)
            car->updateSound(cameras, replayMode != 0);
        if (car->modelEnabled != 0) {
            car->emitSparks(&particles, 0.0f);
            car->model.updateBodyState();
        }
    }
    if (backgroundMode == 0) {
        ambientSound.update(organization);
        pc_debugcam_listener_tick();        /* PC: the debug camera's eye for the next tick's Doppler */
        /* PC: the race runs (this is not called while paused) and is not
         * behind a menu: frames between the ticks, interpolated
         * (game/app/pc_frame.h) */
        pc_frame_request_interpolation();
        if (replayMode == RACE_REPLAY)
            pc_replay_speed_request();      /* PC: L2 / R2 step its speed */
    }
}

// FUNC 0x0017DBA8 RacePS2Base::virtual_31
//   Empty (RaceBase::virtual_31 is another empty function).
void RacePS2Base::virtual_31()
{
}

// FUNC 0x0017DBB0 RacePS2Base::virtual_33
//   RaceBase::virtual_33 (empty), then endRequested = 1: the next frame
//   gate (virtual_27) ends the race.  Callers: RaceSplitBattle::
//   virtual_33, RaceLinkBattle::virtual_33.
void RacePS2Base::virtual_33()
{
    RaceBase::virtual_33();
    endRequested = 1;
}

// FUNC 0x0017DBE8 RacePS2Base::virtual_3
//   GameObject slot 3.  The course data's CLUT animations (course_apply_clut_anims);
//   with exactly two entries, when car 1 takes over a deferred model
//   (applyPendingModel), the model gets the car's state.
//   The enhanced engine (rendering.md 1, the rendering area's request):
//   then every car's model animations (RaceCarModel::applyAnims: the Pod's
//   face palettes; Lupo 2 L2 0x0017BA98..0x0017BADC, its VehicleModel's
//   slot 3).
void RacePS2Base::virtual_3()
{
    course_apply_clut_anims(org->course->data);
    RaceEntryBase *entries = org->entries;
    /* Lupo 2: the cars' model animations (L2 0x0017BA98) */
    for (int i = 0; i < entries->count; i++)
        entries->cars[i]->model.applyAnims();
    if (entries->count == 2) {
        RaceEntryCar *car = entries->cars[1];
        if (car->applyPendingModel() != 0)
            car->model.state = car->state;
    }
    /* PC (2026-11-11): the cockpit camera's menu model of each car a view
     * follows, read the first frame it is followed while the setting is on
     * (game/app/pc_headcam.h), so that the view button swaps nothing: here,
     * where the models change between the frames' draws.  PC (2026-11-15):
     * the setting is the car model's (the calibrator's), so the one turned
     * on while calibrating is read at the next frame too */
    if (pc_headcam_on()) {
        CameraManager *cm = virtual_6();
        const int views = cm->getViewCount();
        for (int v = 0; v < views && v < 4; v++) {
            RaceEntryCar *car = cm->views[v].targetInfo.car[0];
            if (car != NULL && car->model.data != NULL && car->model.pcMenuState == 0 && pc_headcam_menu_model(car))
                car->model.pcLoadMenu(car->info.getModelCode());
        }
    }
    /* PC (2026-11-12): the menu wheels of a race whose cars were set up
     * without them ([Graphics] MenuWheels turned on in the race;
     * game/app/pc_menu_wheels.h) */
    if (pc_menu_wheels()) {
        for (int i = 0; i < entries->count; i++) {
            RaceEntryCar *car = entries->cars[i];
            if (car != NULL && car->model.data != NULL && car->model.pcMenuWheelState == 0)
                car->model.pcLoadMenuWheel(car->info.getModelCode());
        }
    }
    /* PC (2026-11-13): the menu tyres of a race whose cars were set up
     * without them ([Graphics] MenuTires turned on in the race; game/app/
     * pc_menu_tires.h): their file read and their packets built here,
     * between the frames' draws (new packets: nothing drawn is written) */
    if (pc_menu_tires()) {
        for (int i = 0; i < entries->count; i++) {
            RaceEntryCar *car = entries->cars[i];
            if (car != NULL && car->model.data != NULL && car->model.pcMenuTireState == 0) {
                car->model.pcLoadMenuTire();
                car->model.pcBuildMenuTires();
            }
        }
    }
}

/* ======================================================================== */
/* RaceBGMPS2                                                               */
/* ======================================================================== */

/* PC: the race's music for the player's volume of it (pdibgm_pc_music): a
 * race's (replayMode 0 and 3, as getVolume) or a replay's (the demo's too). */
static int pc_race_music(RaceBase *race)
{
    int mode = race->replayMode;
    return (mode == RACE_LIVE || mode == RACE_REPLAY_ALT) ? PDIBGM_MUSIC_RACE : PDIBGM_MUSIC_REPLAY;
}

// FUNC 0x0017DC50 RaceBGMPS2::operator new
//   From the race pool, 16-byte aligned.  Unreferenced.
void *RaceBGMPS2::operator new(size_t size)
{
    return g_002B3BF0.allocate(size, RACE_POOL_ALIGN);
}

// FUNC 0x0017DC78 RaceBGMPS2::operator delete
//   The size (0x9B8) is ignored.  Also used by ~RaceLicenseBGM.
void RaceBGMPS2::operator delete(void *p, size_t size)
{
    g_002B3BF0.free(p);
}

// FUNC 0x0017DCA0 RaceBGMPS2::RaceBGMPS2
//   RaceBGMBase::RaceBGMBase (org is set later, by RaceBase::virtual_8),
//   the vptr, the two play lists filled (MusicPlayList::init: it is not a
//   constructor, u097 / u098), no song, the markers reset with volume 1.
//   Callers: RacePS2Base::RacePS2Base, RaceLicenseBGM::RaceLicenseBGM.
RaceBGMPS2::RaceBGMPS2()
{
    /* implicit: RaceBGMBase, ~RaceBGMBase (the base's constructor and its
     * cleanup on a throw) */
    playlist.init();
    playlist2.init();
    song = 0;
    resetMarkers(1.0f);
    pc_race_songs_begin(this);              /* PC: no songs heard, the song changer closed */
}

// FUNC 0x0017DD28 RaceBGMPS2::~RaceBGMPS2
//   The vptr reset, RaceBGMBase::~RaceBGMBase (flags 0), operator
//   delete(this, 0x9B8) when flags & 1.  Callers: RacePS2Base (destructor
//   and constructor cleanup), ~RaceLicenseBGM.
RaceBGMPS2::~RaceBGMPS2()
{
    /* implicit: ~RaceBGMBase (the base / member destructors) */
    pc_race_songs_end(this);                /* PC: the song changer closed */
}

// FUNC 0x0017DD80 RaceBGMPS2::isDemoWithoutPreview
//   1 when the race runs as the demo (replayMode 2) and the organization's
//   racePreviewMode is 0: then the demo music (group 0x22) plays instead
//   of the play list, and an ended song does not start the next one.
int RaceBGMPS2::isDemoWithoutPreview()
{
    RaceOrganization *organization = org;
    if (organization->race->replayMode == RACE_DEMO && organization->racePreviewMode == 0)
        return 1;
    return 0;
}

// FUNC 0x0017DDB0 RaceBGMPS2::resetMarkers
//   Stores the volume, clears songId and the three marker arrays (32
//   channels): the next update restarts every channel.
void RaceBGMPS2::resetMarkers(f32 volume)
{
    this->volume = volume;
    songId = 0;
    for (int ch = 0; ch < BGM_MARKER_CHANNELS; ch++) {
        markerIndex[ch] = 0;
        markerTime[ch] = 0.0f;
        markerHit[ch] = 0;
    }
}

// FUNC 0x0017DDF0 RaceBGMPS2::update
//   The update, once per frame.  Unless backgroundMode, the stream volume
//   is set (getVolume).  The stream position (pdibgm_get_status) plus 1/30
//   s is "now"; for every channel with markers in the current song: a
//   marker before now is passed (markerHit = 1, index + 1); past the last
//   marker the next one is the first marker of the next loop (+ length);
//   when the stream id changed (a new song) the channel restarts at its
//   first marker (markerHit = 1).  markerTime = next marker - now; FLT_MAX
//   for channels without markers.  When the song ended (and this is not the
//   demo music) the next song is started (play(0x14, 0)) and the old
//   id kept.  All conversions of the marker times and the length are
//   unsigned (u32 -> float), the rate is signed.  Also RaceLicenseBGM
//   slot 1.  song is read in the loop (retail keeps a second copy of it for
//   the test, loop.c's hoisted load); the passed marker's index is read
//   before markerHit is stored (retail's load / store order).
void RaceBGMPS2::update()
{
    if (org->race->backgroundMode == 0) {
        pdibgm_pc_music(pc_race_music(org->race));      /* PC */
        pdibgm_set_volume(getVolume());
    }

    int pos, id, ended;
    f32 now = pdibgm_get_status(&pos, &id, &ended) + BGM_LOOKAHEAD;

    for (int ch = 0; ch < BGM_MARKER_CHANNELS; ch++) {
        markerTime[ch] = FLT_MAX;
        markerHit[ch] = 0;
        if (song == 0)
            continue;
        const BgmMarkers *m = song->markers;
        if (m == NULL)                      /* PC (2026-11-23): a track without marker data (see play) */
            continue;
        if (m->count[ch] == 0)
            continue;

        f32 next = -FLT_MAX;
        if (markerIndex[ch] < m->count[ch]) {
            next = (f32)m->times[ch][markerIndex[ch]] / (f32)m->rate;
            if (next < now) {
                int index = markerIndex[ch] + 1;
                markerHit[ch] = 1;
                markerIndex[ch] = index;
                if (index == m->count[ch])
                    next = -FLT_MAX;
            }
        }
        if (next < 0.0f)
            next = (f32)m->times[ch][0] / (f32)m->rate
                 + (f32)m->length / (f32)m->rate;
        if (id != songId) {
            markerIndex[ch] = 0;
            markerHit[ch] = 1;
            next = (f32)m->times[ch][0] / (f32)m->rate;
        }
        markerTime[ch] = next - now;
    }

    if (song != 0 && ended != 0 && isDemoWithoutPreview() == 0) {
        play(BGM_GROUP_SONGS, 0);
        id = songId;
    }
    songId = id;

    /* PC: the song changer (game/app/pc_race_songs.h, platform/song_osd.h)
     * in a live race: the race song playing on its panel, a step asked for -
     * on, the next song (play, as a song's end); back, the one before - while
     * a race song plays (not the race's jingles) */
    RaceBase *race = org->race;
    if (race->replayMode == RACE_LIVE && race->backgroundMode == 0 && isDemoWithoutPreview() == 0) {
        int track = song != 0 ? (int)(song - bgm_get_tracks(BGM_GROUP_SONGS).tracks) : -1;
        int step = pc_race_songs_frame(&playlist2, track);
        if (step > 0 && track >= 0) {
            play(BGM_GROUP_SONGS, 0);
        } else if (step < 0 && track >= 0) {
            int back = pc_race_songs_back(&playlist2, track);
            if (back >= 0) {
                playTrack(back, 0, (getVolume() != 0.0f) ? 1.0f : 0.0f);
                update();
            }
        }
    }
}

// FUNC 0x0017E0B8 RaceBGMPS2::getMarkerTime
//   The time to the next marker of channel ch.  Also RaceLicenseBGM slot 2.
f32 RaceBGMPS2::getMarkerTime(int ch)
{
    return markerTime[ch];
}

// FUNC 0x0017E0C8 RaceBGMPS2::getMarkerHit
//   1 when a marker of channel ch passed this frame.  Also RaceLicenseBGM
//   slot 3.
int RaceBGMPS2::getMarkerHit(int ch)
{
    return markerHit[ch];
}

// FUNC 0x0017E0D8 RaceBGMPS2::getVolume
//   The volume, or 0 when the music is switched off in the options:
//   GameOptions::raceBgm for replayMode 0 (a race) and 3, replayBgm for
//   the others (1 replay, 2 demo).
f32 RaceBGMPS2::getVolume()
{
    int mode = org->race->replayMode;
    int on;
    if (mode == RACE_LIVE || mode == RACE_REPLAY_ALT)
        on = getGameStatus()->getGameOptions()->raceBgm;
    else
        on = getGameStatus()->getGameOptions()->replayBgm;
    f32 result = 0.0f;
    if (on != 0)
        result = volume;
    return result;
}

// FUNC 0x0017E160 RaceBGMPS2::play
//   Play.  Nothing while backgroundMode.  The volume factor is 1, or 0
//   when the music is off.  In the demo (isDemoWithoutPreview) the music
//   is stopped and, for code 0x14, track 0 of group 0x22 starts (its marker
//   data become the song).  A negative code stops the music.  Code 0x14 is
//   the next song of playlist2 at 0.7 (replays) or 0.5 (races) of the
//   volume factor - or a stop when the play list has none; any other code
//   starts a random track of that group with no marker data.  Both end
//   with an update (slot 1).  The factor becomes the stored volume
//   (resetMarkers).  Caller: RaceLicenseBGM::play.  The track index is an
//   int (-1: a random track, retail's `li -1'; a u32 gives lui / ori),
//   handed to findMusicInfo's u32 *.  Retail loads the demo call's volume
//   before its arg ($f12 before $a2): bgm_play's parameters are (group,
//   track, volume, arg) (u104's matching pass reordered them).
void RaceBGMPS2::play(int code, int arg)
{
    RaceOrganization *organization = org;
    RaceBase *race = organization->race;
    f32 scale = (getVolume() != 0.0f) ? 1.0f : 0.0f;

    if (race->backgroundMode != 0)
        return;
    pdibgm_pc_music(pc_race_music(race));               /* PC */

    if (isDemoWithoutPreview() != 0) {
        stop();
        if (code == BGM_GROUP_SONGS) {
            /* PC (2026-11-23): a music list without a demo track - the
             * European disc's (bgm/ads.inf group 16 is empty there): no
             * music, no song.  Retail took track 0 of the empty list all
             * the same, whatever lay there (the next group's memory): its
             * marker pointer was 0, which the PS2 reads without a fault and
             * the PC does not - a user's crash in update() when the attract
             * demo started from the title screen (RaceBGMPS2::update+0x94,
             * reading 0xC), reproduced with the European data. */
            const BgmTrackList demo = bgm_get_tracks(BGM_GROUP_DEMO);
            if (demo.tracks != NULL && demo.count != 0) {
                bgm_play(BGM_GROUP_DEMO, 0, scale, 0);
                song = demo.tracks;
            } else {
                song = 0;
            }
            resetMarkers(scale);
        }
        return;
    }
    if (code < 0) {
        stop();
        return;
    }
    /* PC: the songs play straight, as the rest of the race's music: the
     * player's volume of it (pdibgm_pc_music; [Audio] RaceBgm / ReplayBgm,
     * 50 % by default) instead of BGM_SONG_VOLUME_RACE (0.5) /
     * BGM_SONG_VOLUME_REPLAY (0.7) of it, the original's for the songs alone */

    int index = -1;
    if (code == BGM_GROUP_SONGS) {
        /* PC: after the song changer's step back, the songs heard after it
         * again (game/app/pc_race_songs.h) */
        index = pc_race_songs_forward();
        if (index >= 0) {
            playTrack(index, 0, scale);
        } else {
        MusicEntry *next = playlist2.nextThere();       /* PC: was next() */
        if (next != 0) {
            findMusicInfo(&next->code, (u32 *)&index);
            playTrack(index, 0, scale);
            pc_race_songs_heard(index);                 /* PC */
        } else {
            stop();
        }
        }
    } else {
        bgm_play(code, index, scale, arg);
        song = 0;
        resetMarkers(scale);
    }
    update();
}

// FUNC 0x0017E348 RaceBGMPS2::playTrack
//   Starts track `index' of group 0x14 (the play-list songs) and takes its
//   marker data as the song (the group's track table, 0x14 bytes per
//   track); the markers restart with that volume.  The group in a local:
//   retail keeps 0x14 in s1 across bgm_get_tracks for the multiply (CSE
//   replaces the constant by the register holding it).
void RaceBGMPS2::playTrack(int index, int arg, f32 volume)
{
    int group = BGM_GROUP_SONGS;
    bgm_play(group, index, volume, arg);
    song = bgm_get_tracks(group).tracks + index;
    resetMarkers(volume);
}

// FUNC 0x0017E3C0 RaceBGMPS2::pause
//   Pause (BGM code -2).  Also RaceLicenseBGM slot 5.
void RaceBGMPS2::pause()
{
    pdibgm_pause();
}

// FUNC 0x0017E3E0 RaceBGMPS2::resume
//   Resume (BGM code -3).  Also RaceLicenseBGM slot 6.
void RaceBGMPS2::resume()
{
    pdibgm_resume();
}

// FUNC 0x0017E400 RaceBGMPS2::stop
//   Stop (BGM code -1): stops the music unless backgroundMode, forgets the
//   song and resets the markers with volume 1.  Also RaceLicenseBGM slot 7.
void RaceBGMPS2::stop()
{
    if (org->race->backgroundMode == 0)
        bgm_stop();
    song = 0;
    resetMarkers(1.0f);
}

/* ======================================================================== */
/* Out-of-line copies of the inline accessors, typeinfo functions           */
/* ======================================================================== */

/* egcs emits these at the end of the object (#pragma implementation, first
 * lines): the inline accessors of game/race/core/T130_race_ps2_base.h in reverse
 * definition order, each class's before its typeinfo function, the classes
 * in reverse order of definition.  No callers. */

// FUNC 0x0017E458 RacePS2Base::getEndRequested
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E468 RacePS2Base::getFlare
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E478 RacePS2Base::getRigidBodies
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E488 RacePS2Base::getParticles
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E498 RacePS2Base::getSmoke
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E4A8 RacePS2Base::setAfterimageTime
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E4B8 RacePS2Base::setMirrorRect
//   The rear-view mirror: a w x h rectangle centred on (cx, cy) (signed
//   halving), stored as floats with the zoom (MirrorView::set).  h comes in
//   $a4.
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E518 __tf11RacePS2Base
//   Builds the __si_type_info at 0x004B1E90 on the first call (name at
//   0x00327F68, base __ti8RaceBase 0x004B1E80).  Caller: __tf9RaceBasic.

// FUNC 0x0017E568 RaceBGMPS2::setVolume
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E570 RaceBGMPS2::commitPlayList
//   playlist2 (the list that is played) = playlist (0x410-byte copy).
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E5C8 RaceBGMPS2::getPlayList2
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E5D0 RaceBGMPS2::setPlayList
//   playlist = pl (0x410-byte copy).
/* inline in game/race/core/T130_race_ps2_base.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x0017E620 __tf10RaceBGMPS2
//   0x004B1EA0, base __ti11RaceBGMBase (gp-0x4628).  Caller:
//   __tf14RaceLicenseBGM.

// FUNC 0x0017E670 __tf12RaceEntryPS2
//   0x004B1EB0, base __ti13RaceEntryBase (gp-0x4688).  Caller:
//   __tf18RaceSolitaireEntry.
