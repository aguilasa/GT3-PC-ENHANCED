/* T129_race_base.cpp - RaceBase (0x0017B8F0..0x0017CF30): the base class of
 * every race mode (RacePS2Base -> RaceBasic -> RaceSinglePlayer / battles /
 * time trial / ...), with the registry of race classes before it.
 *
 * A RaceBase ties a RaceOrganization (org, the race state), a
 * DynamicsConductor (dc, the physics) and the race BGM (bgm) to a
 * CameraManager (embedded at +0x40).  update() (GameObject slot 1) runs once
 * per frame: virtual_27 consumes the race events, virtual_28 / 29 run the
 * frame.  virtual_8 starts the race (virtual_9 is one of its steps);
 * virtual_7 / 11 / 17 / 19 write and read a 0x191C-byte description of the
 * race (RaceInfoBlock, magic 'GT2K') that the RaceSolitaire modes extend.
 * Most virtuals are overridden in the derived classes (T130..T152); the ones
 * here are the defaults.
 *
 * The race thread's id, the wakeup flag and field_9798 (+0x9780..+0x979C)
 * are shared with other threads: two of them are read and cleared with
 * interrupts disabled.
 */
/* Data: retail T129's .rodata (0x003279C0..0x00327C58) is "bad_alloc"
 * (<new>, unreferenced: anchor), "<RACE@BASE>" (virtual_24), the vtable,
 * then the type names in the reverse of their definition order: RaceBase,
 * RaceCameraBase, the garage lists, UnitArena<64>.  So: <new>, T153 with
 * UnitArena<64>'s name, the GameStatus (the garage), then the rest (T383's
 * RaceCameraBase through T129's own header, last); no T157 (retail's T129
 * has no "" in .sdata) and only the thumbnail of T108 (not its menu lists).
 * .sdata (0x00351DF0..0x00351DFC): g_00351DF0, g_00351DF4 (defined below
 * with an initialiser: written at their definition) and virtual_25's
 * guard. */
// DATA_START 0x003279C0
#include <new>                              /* "bad_alloc"; placement new (virtual_25's static, GCC 14) */
#define GAME_T153_EMITS_TYPEINFO            /* UnitArena<64>'s type name */
#include "game/race/core/T153_race_course.h"          /* RaceCourse */
/* The dummy BGM (T175), declared while RaceBGMBase is still incomplete
 * (before T175_race_bgm_base.h): egcs then leaves the 8-byte object out of
 * small data, and RaceBase::RaceBase (-G8) builds its address with lui /
 * addiu as retail does, not $gp-relative.  T175_race_bgm_base.h declares it
 * again. */
class RaceBGMBase;
extern RaceBGMBase g_003521D0;              /* 0x003521D0 */
#include "game/save/T429_game_status.h"          /* (seen: the garage) getGameStatus (GameOptions::ghost) */
#include "game/menu/memorycard/T108_game_thumbnail.h"       /* gt2k_game::GameThumbnail (RaceBase::virtual_12 / 18) */
#include "game/race/core/T121_race_entry.h"           /* RaceEntryBase, RaceEntryCar */
#include "game/race/core/T123_race_setup.h"           /* g_002B3BF0, the race object pool */
#include "game/race/core/T149_race_replay_information.h"  /* race_entry_load_slot */
#include "game/race/core/T150_serialize_replay_data.h"    /* serialize_set_organization */
#include "game/race/core/T174_race_mem_pool.h"        /* RaceMemPool (g_0049C378) */
#include "game/sound/T175_race_bgm_base.h"        /* RaceBGMBase, g_003521D0 (the dummy BGM) */
#include "game/sound/T176_car_sound.h"            /* car_sound_reset_spu, car_sound_update_se_switch, g_003521E4, g_003521E8 */
#include "game/course/T178_course_env.h"           /* CourseEnv, F_001A80E8, F_001A8128 */
#include "game/course/T179_course_data.h"          /* CourseData, F_001A96B8 */
#include "game/car/model/T278_show_room_car.h"        /* ShowRoomCar (RaceBase::virtual_12 / 18) */
#include "game/race/dynamics/T305_dynamics_conductor.h"   /* DynamicsConductor */
#include "game/race/dynamics/T318_control_record.h"       /* AutomobileControlRecord */
#include "game/race/core/T413_race_input.h"           /* RaceInput, g_002DFB58 */
#include "game/app/T008_build_switches.h"       /* F_00102E38 */
#include "engine/io/T504_file_queue.h"           /* file_req_run */
#include "engine/media/G002_mpeg_frameq.h"          /* intr_suspend */
#include "runtime/newlib/G007_stdio.h"                /* memset, strcpy, atexit, GetThreadId, SleepThread, WakeupThread */
#include "game/enhanced.h"                  /* ENH_RACE_TIME_UP_ENDS */
#include "game/race/modes/race_pod_rules.h" /* the Pod race's rules (pod_rules_init / _step) */
#include "game/app/pc_course_scale.h"       /* PC: the course scale in the info block */
#include "game/app/pc_course_pick.h"        /* PC: the laps run (pc_race_laps, in the info block) */
#include "game/app/pc_car_tuning.h"         /* PC: the car multipliers (in the info block) */
#include "game/app/pc_rev_cut.h"            /* PC (2026-11-08): the rev limiter (in the info block; 2026-11-20: a demo's over) */
#include "game/app/pc_shift.h"              /* PC (2026-11-08): the gear shifts (in the info block; 2026-11-20: a demo's over) */
#include "game/app/pc_race_cars.h"          /* PC: the slots of the cars past 8 (in the info block) */
/* own translation units */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization, RaceResult; T484 RaceDataBase / RaceEvent,
                                               T115 RaceOption, T122 RaceEventQueue, T127, T128 */
#include "game/race/core/T126_race_lap_time.h"        /* RaceLapTime */
#include "game/race/core/T127_race_records.h"         /* TimeRecord, SpeedRecord */
#include "game/race/core/T128_race_lap_record.h"      /* LapRecord, RACE_TIME_INVALID */
#include "game/race/core/T129_race_base.h"            /* RaceBase, RaceInfoBlock, the registry; T002 GameObject,
                                               T383 CameraManager */
#include "game/race/core/race_organization_internal.h"                           /* the unit's constants */

/* One frame at 60 Hz (gp-0x7D18 / gp-0x7D14, 0x3C888889). */
#define FRAME_TIME 0.016666668f

/* ======================================================================== */
/* the registry of race classes                                             */
/* ======================================================================== */

// FUNC 0x0017B8F0 race_create_by_name
//   Walks the registry (g_00351DF4) for the class called `name': buf == NULL
//   gives info->create() (new + construct), else info->construct(buf, arg)
//   (in place); NULL when the name is unknown.  Caller:
//   RaceReplayInformation::createRace (the info block's raceName).
RaceBase *race_create_by_name(const char *name, void *buf, int arg)
{
    RaceClassNode *node = g_00351DF4;
    while (node != 0) {
        if (strcmp(name, node->info->name) == 0) {
            if (buf == 0)
                return node->info->create();
            return node->info->construct(buf, arg);
        }
        node = node->next;
    }
    return 0;
}

// FUNC 0x0017B990 race_destroy
//   heap != 0: delete race (the virtual destructor with flags 3; NULL-safe);
//   heap == 0: destroyed in place (flags 2; no NULL test).  Callers:
//   logger_main, RaceReplayInformation::destroyRace.
void race_destroy(RaceBase *race, int heap)
{
    if (heap) {
        delete race;
    } else {
        race->~RaceBase();
    }
}

/* The registry (T129 .sdata, before virtual_25's guard: written here, at
 * their definition, with explicit zero initialisers; after the first
 * function, which names the static constructor / destructor). */
int            g_00351DF0 = 0;              /* 0x00351DF0 the largest sizeof of a registered class */
RaceClassNode *g_00351DF4 = 0;              /* 0x00351DF4 the head of the registry */

// FUNC 0x0017B9E8 race_class_register
//   Links `node' in front of the registry and raises g_00351DF0 to the
//   largest class size (unsigned).  Called by the static constructor of
//   each registered class (13 callers, e.g. race_basic_static_init).
RaceClassNode *race_class_register(RaceClassNode *node, RaceClassInfo *info)
{
    node->next = g_00351DF4;
    node->info = info;
    g_00351DF4 = node;
    if ((u32)g_00351DF0 < info->size)       /* an unsigned compare (sltu) */
        g_00351DF0 = info->size;
    return node;
}

/* ======================================================================== */
/* RaceBase: allocation, construction, destruction                          */
/* ======================================================================== */

// FUNC 0x0017BA18 RaceBase::operator new
//   The race pool g_002B3BF0, 16-byte aligned (as RaceOrganization's).
//   Unreferenced.
void *RaceBase::operator new(size_t size)
{
    return g_002B3BF0.allocate(size, 0x10);
}

// FUNC 0x0017BA40 RaceBase::operator delete
//   The size (0x97C0 from the destructor) is ignored.
void RaceBase::operator delete(void *p, size_t size)
{
    g_002B3BF0.free(p);
}

// FUNC 0x0017BA68 RaceBase::RaceBase
//   GameObject::GameObject, the vptr, CameraManager::CameraManager (the
//   member at +0x40; ~CameraManager and ~GameObject in the cleanup), then the
//   fields: the BGM is the dummy g_003521D0 until RacePS2Base installs its
//   own, threadId the constructing thread (GetThreadId), rendering on.
//   Caller: RacePS2Base::RacePS2Base.  (&g_003521D0 is built with lui /
//   addiu: declared above before RaceBGMBase is complete.)
RaceBase::RaceBase()
{
    org = 0;
    dc = 0;
    bgm = &g_003521D0;
    threadId = GetThreadId();
    renderEnabled = 1;
    replayMode = RACE_LIVE;
    backgroundMode = 0;
    paused = 0;
    endCode = 0;

    /* implicit: GameObject, CameraManager (the base's and the member's
     * constructors), ~CameraManager, ~GameObject (their cleanup on a throw) */
}

// FUNC 0x0017BB38 RaceBase::~RaceBase
//   Slot 0.  The vptr, the camera member destroyed (flags 2), then
//   GameObject::~GameObject with flags 0; flags & 1 frees the object with
//   operator delete(this, 0x97C0).  Callers: RacePS2Base::RacePS2Base (its
//   cleanup), RacePS2Base::~RacePS2Base.
RaceBase::~RaceBase()
{
    /* implicit: ~GameObject, ~CameraManager (the base / member destructors) */
}

/* The replay memory pool: a static object of the unit (.bss 0x0049C378).
 * PC: the egcs form (the C++ runtime constructs it); the GCC 14 build's
 * explicit race_mem_pool_static_ctor / _dtor are not needed. */
static RaceMemPool g_0049C378;              /* 0x0049C378 */

// FUNC 0x0017BBA0 RaceBase::virtual_4
//   The static replay memory pool g_0049C378 (race_mem_pool_static_ctor).
RaceMemPool *RaceBase::virtual_4()
{
    return &g_0049C378;
}

// FUNC 0x0017BBB0 RaceBase::virtual_23
//   Empty hook (RaceOrganization::F_0017AD28).
void RaceBase::virtual_23()
{
}

// FUNC 0x0017BBB8 RaceBase::setBackgroundMode
//   backgroundMode, mirrored in g_003521E8 (no sound effects while set,
//   T176).  Callers: RaceBase::virtual_8, RacePS2Base::virtual_42.
void RaceBase::setBackgroundMode(int v)
{
    backgroundMode = v;
    g_003521E8 = v;
}

// FUNC 0x0017BBD0 RaceBase::virtual_6
//   The embedded CameraManager.
CameraManager *RaceBase::virtual_6()
{
    return &camera;
}

// FUNC 0x0017BBD8 RaceBase::virtual_5
//   0.
int RaceBase::virtual_5()
{
    return 0;
}

/* ======================================================================== */
/* starting, ending and describing a race                                   */
/* ======================================================================== */

// FUNC 0x0017BBE0 RaceBase::virtual_8
//   Starts the race (RacePS2Base::virtual_8).  `replay' goes to replayMode
//   and freezes the cars' lap times.  Steps: the flags cleared,
//   serialize_set_organization(org), the BGM's organization, backgroundMode from the
//   organization's logger / background mode, the organization's race state
//   (initRaceState) and race; the default input's buttons cleared, the
//   conductor attached and its starting format (getStartingFormat: 0 and 3
//   -> START_ACTION_NONE, 2 -> START_ACTION_NO_COUNTDOWN, 1 keeps the
//   record's) taken; virtual_39; the conductor's car count; every car's
//   state (getCar) and lap times; the course loaded; the cars' models (the
//   kind by the course's light), sounds (car_sound_reset_spu first), file_req_run,
//   F_001A80E8, virtual_9, the tyres and wheels, file_req_run, setupTires and
//   the body model's LOD distance 200; the camera (init, update, view 0's
//   input unless in the background); startSounds.  Matching: replayMode is
//   the enum RaceReplayMode (its store follows the organization's int
//   loads); the cars' states through the inline getDynamicsConductor() (its
//   value is a pseudo GCSE carries into the loop: retail loads dc before
//   the loop and again on the back edge); the model kind as an if / else
//   chain (andi + sltu, not a shifted bit).
void RaceBase::virtual_8(int replay)
{
    RaceOrganization *o = org;
    int count;
    int i;

    /* PC (2026-11-20): a race that is not a replay (a live run, a race playing
     * by itself) records over a demo file's recording: the race's own rev
     * limiter and gear shifts again (game/app/pc_rev_cut.h, pc_shift.h) */
    if (replay != RACE_REPLAY) {
        pc_rev_cut_demo_end(o);
        pc_shift_demo_end(o);
    }
    field_9798 = 0;
    wakeup = 0;
    endCode = 0;
    serialize_set_organization(o);
    bgm->org = o;
    replayMode = (RaceReplayMode)replay;
    int background = 0;
    if (o->loggerMode != 0 || o->backgroundMode != 0)
        background = 1;
    setBackgroundMode(background);
    setPausedState(0);                      /* the enhanced engine: through the setter (Lupo 2 L2 0x0017988C) */
    o->initRaceState();
    o->race = this;
    virtual_25()->curButtons.mask = 0;      /* Control::invalidButtons, inline here */
    dc->attach(o);

    switch (dc->getStartingFormat()) {
    case 0:
        o->startAction = START_ACTION_NONE;
        break;
    case 1:
        break;
    case 2:
        o->startAction = START_ACTION_NO_COUNTDOWN;
        break;
    case 3:
        o->startAction = START_ACTION_NONE;
        break;
    }

    virtual_39();
    count = o->entries->count;
    dc->carCount = count;
    for (i = 0; i < count; i++) {
        RaceEntryCar *car = o->entries->cars[i];
        RaceCarState *state = getDynamicsConductor().getCar(i);
        car->state = state;
        car->model.state = state;
        car->lapTime.reset(replay != 0);
    }

    o->course->load(o->courseCode);
    u32 envFlags = org->course->data->env->flags;
    int modelKind;
    if (envFlags & COURSE_ENV_EVENING)
        modelKind = CAR_MODEL_EVENING;
    else if (envFlags & COURSE_ENV_NIGHT)
        modelKind = CAR_MODEL_NIGHT;
    else
        modelKind = CAR_MODEL_DAY;
    for (i = 0; i < count; i++)
        o->entries->cars[i]->loadModel(modelKind);

    g_003521E4 = 1.0f;
    car_sound_reset_spu();
    for (i = 0; i < count; i++)
        o->entries->cars[i]->loadSound();
    file_req_run();

    CourseData *courseData = org->course->data;
    F_001A80E8(courseData->env, courseData);
    virtual_9();
    for (i = 0; i < count; i++) {
        o->entries->cars[i]->loadTires();
        o->entries->cars[i]->loadWheel();
    }
    file_req_run();
    for (i = 0; i < count; i++) {
        RaceEntryCar *car = o->entries->cars[i];
        car->setupTires();
        F_00179460(&car->model.carModel)->callback.lodDist = 200.0f;
    }

    CameraManager *cm = virtual_6();
    cm->init(o, 0);
    cm->update(o, dc);
    if (backgroundMode == 0)
        cm->views[0].input = virtual_25();
    startSounds();
}

/* New in the enhanced engine (Lupo 2 L2 0x0017A650; backend.md B2,
 * race_logic.md C5): paused = on; in a live race a change also tells the
 * player's input (virtual_25), whose wheel spring is on while paused. */
void RaceBase::setPausedState(int on)
{
    if (replayMode == RACE_LIVE && paused != on)
        virtual_25()->setMode(on);
    paused = on;
}

// FUNC 0x0017BF98 RaceBase::virtual_9
//   A step of virtual_8 (also called by RaceSolitaire::virtual_9): the
//   cars' inputs (attachInputs), the conductor's initialize with the replay
//   mode, the ranks and events.
void RaceBase::virtual_9()
{
    RaceOrganization *o = org;
    o->attachInputs(this);
    dc->initialize(replayMode);
    /* The enhanced engine: the Pod race's stop-area rules (Lupo 2 sets them
     * up in the conductor's virtual_20, L2 0x001F0D50 -> 0x001F75C8; on only
     * for a race with licenseType 5, race_pod_rules.h) */
    pod_rules_init(dc);
    o->updateRanksAndEvents();
}

// FUNC 0x0017C000 RaceBase::virtual_7
//   Builds the race info block: size 0x191C, magic 'GT2K', version 0x30000,
//   the mode's name (virtual_24), the option block, the race and course
//   codes, car 0's lap total and best lap, the car count and for each car
//   (at most 8 slots of 0x280 bytes; nothing checks) its car data
//   (CarEquipments::operator=, 0x1E8 bytes: the condition copied next
//   overwrites the tail), condition, input id, order and five option bytes.
//   Callers: virtual_11, virtual_17.
//   Not exact (92.7%).  The input id through RaceEntryInformation::
//   getInputID() (retail keeps &car->info in $s1 from the operator= call).
//   The option words are read whole (lw, then sb) and transmission's load
//   passes the int store before it: retail read them through RaceEntryCar's
//   inline getters, with transmission / tyre / assistAsm / assistTcs bool
//   fields (race_entry_load_slot stores `!= 0' into them); the int locals
//   stand in for the getters' values (T121 is u042's: ref/matching/u042/
//   pending.md; with a bool view of transmission the loop body is exact).
//   Left as well: retail reloads o->entries in the loop body on the first
//   pass too (ours takes the count's load, PRE), and sets memset's $a0
//   before $a1.
void RaceBase::virtual_7(RaceInfoBlock *blk)
{
    char *name = blk->raceName;
    RaceInfoSlot *slot = blk->slots;
    RaceOrganization *o;
    int i;

    memset(name, 0, sizeof(blk->raceName));
    memset((void *)slot, 0, sizeof(blk->slots));
    memset(blk->unk_080, 0, sizeof(blk->unk_080));
    /* PC: a race of more than 8 cars: its slots 8.. after the block, counted
     * by the size word (game/app/pc_race_cars.h); RACE_INFO_SIZE up to 8 */
    blk->size = pc_race_info_size(org->entries->count);
    blk->magic = RACE_INFO_MAGIC;
    blk->version = RACE_INFO_VERSION;
    strcpy(name, virtual_24());

    o = org;
    pc_course_scale_store(blk, o->course);  /* PC: the course scale in unk_080 (game/app/pc_course_scale.h) */
    pc_race_run_store(blk, o);              /* PC: and the laps / time run, when not the record's (game/app/pc_course_pick.h) */
    pc_car_tuning_store(blk, o);            /* PC: and the car multipliers, when not 100 % (game/app/pc_car_tuning.h) */
    pc_rev_cut_store(blk, o);               /* PC (2026-11-08): and the rev limiter, when realistic (game/app/pc_rev_cut.h) */
    pc_shift_store(blk, o);                 /* PC (2026-11-08): and the gear shifts, when realistic (game/app/pc_shift.h) */
    blk->option = o->option;
    blk->raceCode = o->raceCode;
    RaceLapTime *lapTime = &o->entries->cars[0]->lapTime;
    blk->courseCode = o->courseCode;
    blk->total = lapTime->total;
    blk->bestLap = lapTime->getBest(-1);

    blk->carCount = o->entries->count;
    for (i = 0; i < blk->carCount; i++, slot++) {
        RaceEntryCar *car = o->entries->cars[i];
        if (i >= 8) {
            slot = pc_race_info_slot(blk, i);   /* PC: past the block's 8 */
            memset((void *)slot, 0, sizeof(*slot));
        }
        /* the slot's CarSaveEquipments as a whole CarEquipments */
        /* PC: the CarSaveEquipments part only.  Retail copies a whole
         * CarEquipments (0x1E8 bytes) over the slot; egcs puts its
         * CarSaveEquipments base at +0, so the copy's tail (the CarDataBase
         * base, presetCar) lands on 0x1D0..0x1E8, which the condition
         * below overwrites.  MSVC puts the polymorphic CarDataBase base
         * first and CarSaveEquipments at +0x10: the downcast of the slot
         * started 0x10 bytes before it and the copy overwrote the block's
         * carCount (slot 0) with a pointer, so the loop ran past the 8
         * slots.  The block gets the same bytes either way. */
        slot->info = static_cast<const CarSaveEquipments &>(car->info);
        slot->condition = car->condition;
        slot->inputId = car->info.getInputID();
        int transmission = car->transmission;
        int order = car->order;
        slot->transmission = transmission;
        slot->order = order;
        int tyre = car->tyre;
        slot->tyre = tyre;
        int steeringAssist = car->steeringAssist;
        slot->steeringAssist = steeringAssist;
        int assistAsm = car->assistAsm;
        slot->assistAsm = assistAsm;
        int assistTcs = car->assistTcs;
        slot->assistTcs = assistTcs;
    }
    blk->recordCount = 0;
}

// FUNC 0x0017C1F0 RaceBase::virtual_10
//   virtual_15() + 0x191C: the size of the info block with its extension.
//   PC: the block of the race's cars (game/app/pc_race_cars.h).
int RaceBase::virtual_10()
{
    return virtual_15() + pc_race_info_size(org->entries->count);
}

// FUNC 0x0017C220 RaceBase::virtual_11
//   The info block (virtual_7) and its extension (virtual_14).  Caller:
//   RaceSolitaire::virtual_11.
void RaceBase::virtual_11(RaceInfoBlock *blk)
{
    virtual_7(blk);
    virtual_14(blk);
}

// FUNC 0x0017C278 RaceBase::virtual_12
//   Car 0's part of the replay save's thumbnail: the lap total, the number
//   of stored laps, the car's code, the course code and the car's power
//   (ShowRoomCar::power, computed: catalogue 0) as a float.
void RaceBase::virtual_12(gt2k_game::GameThumbnail *thumb)
{
    ShowRoomCar room;
    int rpm;
    RaceOrganization *o = org;
    RaceEntryCar *car = o->entries->cars[0];
    RaceLapTime *lapTime = &car->lapTime;

    room.SetupShowRoomCar(&car->info, 0);
    thumb->replay.time = lapTime->total;
    thumb->replay.laps = lapTime->max;
    thumb->replay.car = car->info.carCode;
    thumb->replay.course = o->courseCode;
    thumb->replay.power = (f32)room.pcPowerPS(&rpm, 0);    /* PC: PS, converted where shown (T069) */
}

// FUNC 0x0017C318 RaceBase::virtual_13
//   Empty (the overrides load what follows the info block).
void RaceBase::virtual_13(RaceInfoBlock *blk)
{
}

// FUNC 0x0017C320 RaceBase::virtual_14
//   Empty (the extension of virtual_11).
void RaceBase::virtual_14(RaceInfoBlock *blk)
{
}

// FUNC 0x0017C328 RaceBase::virtual_15
//   0: the size of virtual_14's extension.
int RaceBase::virtual_15()
{
    return 0;
}

// FUNC 0x0017C330 RaceBase::virtual_16
//   virtual_22() + 0x191C.
//   PC: the block of the race's cars (game/app/pc_race_cars.h).
int RaceBase::virtual_16()
{
    return virtual_22() + pc_race_info_size(org->entries->count);
}

// FUNC 0x0017C360 RaceBase::virtual_17
//   The info block (virtual_7) and the extension virtual_21.  Caller:
//   RaceSolitaire::virtual_17.
void RaceBase::virtual_17(RaceInfoBlock *blk)
{
    virtual_7(blk);
    virtual_21(blk);
}

// FUNC 0x0017C3B8 RaceBase::virtual_18
//   Like virtual_12 for the best lap part: car 0's ghostLapTime, the power
//   at +0x20 and 0 (sw $zero) for the mass.
void RaceBase::virtual_18(gt2k_game::GameThumbnail *thumb)
{
    ShowRoomCar room;
    int rpm;
    RaceOrganization *o = org;
    RaceEntryCar *car = o->entries->cars[0];

    room.SetupShowRoomCar(&car->info, 0);
    thumb->bestlap.time = car->ghostLapTime;
    thumb->bestlap.car = car->info.carCode;
    thumb->bestlap.course = o->courseCode;
    f32 power = (f32)room.pcPowerPS(&rpm, 0);  /* PC: PS, converted where shown (T069) */
    thumb->bestlap.mass = 0.0f;
    thumb->bestlap.power = power;
}

// FUNC 0x0017C448 RaceBase::virtual_19
//   Reads an info block: car 1 made sure of (setupGhost(2, 0)), its model
//   unloaded, loaded from slot 0 of the block (race_entry_load_slot), input id
//   INPUT_GHOST, its colour index.
void RaceBase::virtual_19(RaceInfoBlock *blk)
{
    RaceOrganization *o = org;
    pc_course_scale_check_ghost(blk, o->course);   /* PC: a ghost of another course scale, logged */
    pc_rev_cut_check_ghost(blk, o);                /* PC (2026-11-08): or of the other rev limiter */
    pc_shift_check_ghost(blk, o);                  /* PC (2026-11-08): or of the other gear shifts */
    o->setupGhost(2, 0);
    RaceEntryCar *ghost = o->entries->cars[1];
    ghost->unloadModel();
    race_entry_load_slot(ghost, &blk->slots[0]);
    ghost->info.inputId = INPUT_GHOST;
    ghost->updateColorIndex();
}

// FUNC 0x0017C4B8 RaceBase::virtual_20
//   Empty (RaceFreeRun's override reads blk).
void RaceBase::virtual_20(RaceInfoBlock *blk)
{
}

// FUNC 0x0017C4C0 RaceBase::virtual_21
//   Empty (the extension of virtual_17).
void RaceBase::virtual_21(RaceInfoBlock *blk)
{
}

// FUNC 0x0017C4C8 RaceBase::virtual_22
//   0: the size of virtual_21's extension.
int RaceBase::virtual_22()
{
    return 0;
}

// FUNC 0x0017C4D0 RaceBase::virtual_24
//   The mode's name in the info block; every mode returns its own.
const char *RaceBase::virtual_24()
{
    return "<RACE@BASE>";
}

// FUNC 0x0017C4E0 race_input_static_dtor (__tcf_0)
//   The atexit callback of virtual_25's static RaceInput: its destructor
//   without the delete (flags 2).  egcs writes it (__tcf_0) right before the
//   function that holds the static.

// FUNC 0x0017C508 RaceBase::virtual_25
//   The default input: a function-local static RaceInput (.bss 0x0049C230,
//   T129's first .bss object; its guard .sdata 0x00351DF8), constructed on
//   the first call and destroyed at exit (atexit(__tcf_0)).  The GCC 14
//   branch spells out the same steps (no thread-safe static guards there).
RaceInput *RaceBase::virtual_25()
{
    /* PC: the egcs form (the C++ runtime guards and destroys it). */
    static RaceInput input;                 /* 0x0049C230; guard 0x00351DF8 */
    return &input;
}

// FUNC 0x0017C560 RaceBase::virtual_26
//   The input of input id `id': here the disabled RaceInput g_002DFB58 (its
//   enabled flag is 0).  Callers: RaceSinglePlayer::virtual_26,
//   RaceSplitBattle::virtual_26, RaceSolitaire::virtual_26.
RaceInput *RaceBase::virtual_26(int id)
{
    return &g_002DFB58;
}

/* ======================================================================== */
/* the frame                                                                */
/* ======================================================================== */

// FUNC 0x0017C570 RaceBase::update
//   Per frame (GameObject slot 1).  When virtual_27 accepts the frame,
//   virtual_28 with dt 1/60 (virtual_30 while paused), then virtual_31.
//   A RACE_EVENT_LICENSE_FAILED nobody took during the frame (RaceLicenseDisplay
//   does) then runs virtual_33.
void RaceBase::update()
{
    if (virtual_27()) {
        if (paused == 0)
            virtual_28(FRAME_TIME);
        else
            virtual_30();
        virtual_31();
    }
    if (org->eventQueue.find(RACE_EVENT_LICENSE_FAILED, 0, 1))
        virtual_33();
}

// FUNC 0x0017C630 RaceBase::virtual_27
//   The events of the frame (eventQueue.update first): RACE_EVENT_END ->
//   virtual_33 and 0 (no frame); RACE_EVENT_START -> start;
//   RACE_EVENT_BGM -> the BGM (BGM_PAUSE pause, BGM_RESUME resume, BGM_STOP
//   stop, else play(code, code == BGM_RACE)).  1
//   otherwise.  Caller: RacePS2Base::virtual_27.  (The cases in retail's
//   block order: stop, pause, resume.)
//   The enhanced engine (race_logic.md C1, ENH_RACE_TIME_UP_ENDS): first
//   RACE_EVENT_TIME_UP ends any race with code 5 (Lupo 2 L2 0x0017A328).
int RaceBase::virtual_27()
{
    RaceBGMBase *music = bgm;
    RaceEventQueue *queue = &org->eventQueue;
    int code;

    queue->update();
#if ENH_RACE_TIME_UP_ENDS
    /* Lupo 2: the time limit ends every race, code 5 (L2 0x0017A354; GT3:
     * RaceArcadeDemo::virtual_27 only) */
    if (queue->find(RACE_EVENT_TIME_UP, 0, 1))
        virtual_34(RACE_END_DEMO_TIME_UP);
#endif
    if (queue->find(RACE_EVENT_END, 0, 1)) {
        virtual_33();
        return 0;
    }
    if (queue->find(RACE_EVENT_START, 0, 1))
        start();
    if (queue->find(RACE_EVENT_BGM, &code, 1)) {
        switch (code) {
        case BGM_STOP:
            music->stop();
            break;
        case BGM_PAUSE:
            music->pause();
            break;
        case BGM_RESUME:
            music->resume();
            break;
        default:
            music->play(code, code == BGM_RACE);
            break;
        }
    }
    return 1;
}

// FUNC 0x0017C778 RaceBase::virtual_28
//   virtual_29(dt) (dt passed through in $f12).  Caller: RacePS2Base::
//   virtual_28.
void RaceBase::virtual_28(f32 dt)
{
    virtual_29(dt);
}

// FUNC 0x0017C7A0 RaceBase::virtual_29
//   The frame: the organization's start / timer, the BGM's update, the
//   sound effects switch (car_sound_update_se_switch, not in the background), the course
//   (RaceCourse::update with 1/60; dt is not used), F_001A8128, the conductor
//   (DynamicsConductor::update(0)), the ranks and events, the camera
//   (CameraManager::update).  When F_00102E38() (a build switch, 1) is 0 a
//   temporary RaceInput in the replay mode of virtual_25's lives around the
//   camera update.  Caller: RacePS2Base::virtual_29.
void RaceBase::virtual_29(f32 dt)
{
    RaceOrganization *o = org;
    CameraManager *cm = virtual_6();
    RaceBGMBase *music = bgm;

    o->updateStartAndTimer();
    music->update();
    if (backgroundMode == 0)
        car_sound_update_se_switch(replayMode != 0);
    org->course->update(FRAME_TIME);
    CourseData *courseData = org->course->data;
    F_001A8128(courseData->env, courseData);
    dc->update(0);
    /* The enhanced engine: the Pod race's rules of the frame (Lupo 2 runs
     * them in the conductor's per-frame slot, L2 0x001F05E8 / 0x001EE148 ->
     * 0x001F7BF0; nothing outside a Pod race) */
    pod_rules_step(dc);
    o->updateRanksAndEvents();
    if (F_00102E38() != 0) {
        cm->update(o, dc);
        return;
    }
    RaceInput input;
    input.setReplayMode(virtual_25()->replayMode);
    cm->update(o, dc);
}

// FUNC 0x0017C908 RaceBase::virtual_30
//   Empty (the frame while paused).
void RaceBase::virtual_30()
{
}

// FUNC 0x0017C910 RaceBase::virtual_31
//   Empty.
void RaceBase::virtual_31()
{
}

// FUNC 0x0017C918 RaceBase::start
//   The start: RACE_EVENT_GO, then the race music (RACE_EVENT_BGM,
//   BGM_RACE).
void RaceBase::start()
{
    RaceEventQueue *queue = &org->eventQueue;
    queue->post(RACE_EVENT_GO, 0, 0);
    queue->post(RACE_EVENT_BGM, BGM_RACE, 0);
}

// FUNC 0x0017C968 RaceBase::virtual_33
//   Empty.  Caller: RacePS2Base::virtual_33.
void RaceBase::virtual_33()
{
}

// FUNC 0x0017C970 RaceBase::endRace
//   Ends the race: stopSounds, the BGM stopped (stop), the conductor's
//   virtual_26, the inputs' records closed (F_0017AA48), signal(1) to the
//   race thread.  Caller: RacePS2Base::virtual_27.
void RaceBase::endRace()
{
    stopSounds();
    bgm->stop();
    dc->virtual_26();
    org->F_0017AA48();
    signal(1);
}

// FUNC 0x0017C9F0 RaceBase::virtual_34
//   endCode = code, then virtual_33.
void RaceBase::virtual_34(int code)
{
    endCode = code;
    virtual_33();
}

// FUNC 0x0017CA28 RaceBase::virtual_35
//   stopSounds.  Caller: RacePS2Base::virtual_35.
void RaceBase::virtual_35()
{
    stopSounds();
}

// FUNC 0x0017CA50 RaceBase::startSounds
//   Every car's sound started (CarSound::start), then the BGM resumed
//   (BGM_RESUME).  Caller: RacePS2Base::startSounds.
void RaceBase::startSounds()
{
    RaceOrganization *o = org;
    for (int i = 0; i < o->entries->count; i++)
        o->entries->cars[i]->sound.start();
    o->eventQueue.post(RACE_EVENT_BGM, BGM_RESUME, 0);
}

// FUNC 0x0017CAE8 RaceBase::stopSounds
//   Every car's sound stopped (CarSound::stop), then the BGM paused
//   (BGM_PAUSE).  Caller: RacePS2Base::stopSounds.
void RaceBase::stopSounds()
{
    RaceOrganization *o = org;
    for (int i = 0; i < o->entries->count; i++)
        o->entries->cars[i]->sound.stop();
    o->eventQueue.post(RACE_EVENT_BGM, BGM_PAUSE, 0);
}

/* ======================================================================== */
/* thread signalling                                                        */
/* ======================================================================== */

// FUNC 0x0017CB80 RaceBase::signal
//   wakeup = v and WakeupThread(threadId).  Caller: endRace.
void RaceBase::signal(int v)
{
    wakeup = v;
    WakeupThread(threadId);
}

// FUNC 0x0017CBA8 RaceBase::takeField9798
//   field_9798, read and cleared with interrupts off (intr_suspend).
//   Unreferenced (nothing sets the field but setField9798, also
//   unreferenced): its meaning is not known.
int RaceBase::takeField9798()
{
    int was = intr_suspend();
    int value = field_9798;
    field_9798 = 0;
    intr_resume(was);
    return value;
}

// FUNC 0x0017CBF0 RaceBase::virtual_38
//   The race thread sleeps (SleepThread) until signal() wakes it, then
//   returns the signal (takeSignal; the race loop tests it).
int RaceBase::virtual_38()
{
    SleepThread();
    return takeSignal();
}

// FUNC 0x0017CC20 RaceBase::setField9798
//   Unreferenced.
void RaceBase::setField9798(int v)
{
    field_9798 = v;
}

// FUNC 0x0017CC38 RaceBase::takeSignal
//   wakeup, read and cleared with interrupts off (intr_suspend).  Callers:
//   ShortmenuLicenseControl::updateRace, logger_record_thread, virtual_38,
//   RacePS2Base::virtual_42.
int RaceBase::takeSignal()
{
    int was = intr_suspend();
    int value = wakeup;
    wakeup = 0;
    intr_resume(was);
    return value;
}

/* ======================================================================== */
/* the race kind's car set-up, data, accessors                              */
/* ======================================================================== */

// FUNC 0x0017CC80 RaceBase::virtual_39
//   The kind of the database's course (RaceDataBase::getCourseKind):
//   COURSE_KIND_1 gives every car the second-set tyres matching its front
//   tyre (CarEquipments::F_00236798, then F_00236600, the result unused),
//   COURSE_KIND_DIRT dirt tyres (F_002366D8, then hasDirtTires, unused);
//   other kinds nothing.  Caller: virtual_8.
void RaceBase::virtual_39()
{
    RaceOrganization *o = org;
    RaceDataBase *db = o->raceDB;
    switch (db->getCourseKind(db->course)) {
    case COURSE_KIND_1:
        for (u32 i = 0; i < (u32)o->entries->count; i++) {
            o->entries->cars[i]->info.F_00236798();
            o->entries->cars[i]->info.F_00236600();
        }
        break;
    case COURSE_KIND_DIRT:
        for (u32 i = 0; i < (u32)o->entries->count; i++) {
            o->entries->cars[i]->info.F_002366D8();
            o->entries->cars[i]->info.hasDirtTires();
        }
        break;
    }
}

// FUNC 0x0017CD98 RaceBase::getNumberOfLaps
//   The race record's numberOfLaps (lbu).
int RaceBase::getNumberOfLaps()
{
    return pc_race_laps(org, org->raceDB->event->numberOfLaps);   /* PC: the laps run (game/app/pc_course_pick.h) */
}

/* Compiler-generated under egcs (the static object g_0049C378 above),
 * emitted after the unit's functions and before the inline copies. */
// FUNC 0x0017CDB0 race_mem_pool_static_dtor (_GLOBAL_$D$race_create_by_name__FPCcPvi)
//   The static destructor (.dtors entry 0x0028F210): ~RaceMemPool of
//   g_0049C378 (flags 2).

// FUNC 0x0017CDD8 race_mem_pool_static_ctor (_GLOBAL_$I$race_create_by_name__FPCcPvi)
//   The static constructor (.ctors entry 0x0028F0EC): RaceMemPool::
//   RaceMemPool on g_0049C378.

/* RaceBase's accessors: inline in game/race/core/T129_race_base.h (the original's
 * linkonce copies); the key function is here, so egcs writes each one at
 * the end of the unit, after the static constructor / destructor, in the
 * reverse of their declaration order (as these markers). */
// FUNC 0x0017CDF8 RaceBase::getEndCode
// FUNC 0x0017CE08 RaceBase::getPaused
// FUNC 0x0017CE18 RaceBase::setPaused
// FUNC 0x0017CE28 RaceBase::getRenderEnabled
// FUNC 0x0017CE38 RaceBase::setRenderEnabled
// FUNC 0x0017CE48 RaceBase::getBackgroundMode
// FUNC 0x0017CE58 RaceBase::getReplayMode
// FUNC 0x0017CE68 RaceBase::setReplayMode
// FUNC 0x0017CE78 RaceBase::getBGM
// FUNC 0x0017CE80 RaceBase::getEnvPtr
// FUNC 0x0017CE98 RaceBase::getCourseData
// FUNC 0x0017CEA8 RaceBase::getCourse
// FUNC 0x0017CEB8 RaceBase::getDynamicsConductor
// FUNC 0x0017CEC0 RaceBase::setDynamicsConductor
// FUNC 0x0017CEC8 RaceBase::getOrganization
// FUNC 0x0017CED0 RaceBase::setOrganization

