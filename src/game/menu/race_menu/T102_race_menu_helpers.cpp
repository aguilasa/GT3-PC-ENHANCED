/* T102 - running a short menu over a race, and race-result helpers of the
 * race modes (T052 and the following units).  0x0012E2C8..0x0012F840.
 *
 * run_short_menu / run_short_menu_license run one menu session over the
 * race (the race keeps running and is drawn behind the menu) and return the
 * action code chosen (QM_*, QM_EXIT if none).  The other functions work on
 * the race's RaceOrganization: grid and result orders (RaceEntryCar::order),
 * the result lines of ShortMenuArgs, the licence and event results, the car
 * condition (odometer, engine, oil) moved between the garage car and the
 * race entry, the memory card screens, the ranking code of a record and the
 * demo setting loader.
 *
 * Includes (data pass): retail's .rodata (0x003154E0..0x003158C8) is
 * "bad_alloc", license_record_result's jump table, then the type names
 * ShortmenuLicenseControl, ShortmenuControl, SmartIndexT<int>,
 * ResultTermBar, ResultTitleBar, ReplayPauseMenu, PauseMenu,
 * RaceMenuSelectWindow, RaceMenuWindow, RaceMenuButton, the OperatorHolder
 * group, RaceMiniMap .. RaceDisplayBase, RaceCameraBase, UnitArena<64>,
 * RegurationResult, RecordsMenuArg, the garage lists, Operator (reverse
 * definition order; the memory card screens' entry point without
 * MemorycardControl, no ShortMenuText).  Its .sdata opens with T157's ""
 * (the race displays), then "demo/%s" and g_003514F0.  "bad_alloc" and the
 * "" are unreferenced: the anchors. */
// DATA_START 0x003154E0
// DATA_START 0x003514E0
#include <new>                              /* "bad_alloc" (retail's first .rodata item) */
#include "game/save/T429_game_status.h"          /* getGameStatus (T420 LicenseRecords, T425 EventResults);
                                               GarageCar (T036) */
#include "game/menu/records/T089_records_menu_args.h"    /* (seen, not used: RecordsMenuArg) */
#include "game/menu/gtmode/T045_reguration_result.h"    /* (seen, not used: RegurationResult) */
#define GAME_T153_EMITS_TYPEINFO            /* UnitArena<64>'s type name */
#include "game/race/core/T153_race_course.h"          /* (seen, not used) */
#include "game/camera/T383_race_camera_base.h"     /* (seen, not used) */
#include "game/race/display/T154_race_display.h"         /* (seen, not used: the race displays) */
#include "game/menu/framework/T216_menu_transitive.h"      /* (seen, not used: the OperatorHolder group) */
#include "game/menu/race_menu/T198_race_menu_button.h"     /* (seen, not used) */
#include "game/menu/race_menu/T199_race_menu_layer.h"      /* (seen, not used) */
#include "game/race/pause/T169_race_pause.h"           /* (seen, not used) */
#include "game/race/pause/T170_replay_pause.h"         /* (seen, not used) */
#include "game/menu/records/T200_result_board.h"         /* (seen, not used) */
#include "game/menu/race_menu/T111_shortmenu_control.h"    /* ShortmenuControl (run_short_menu) */
#include "game/menu/race_menu/T103_shortmenu_license.h"    /* ShortmenuLicenseControl (run_short_menu_license) */
#include "game/menu/race_menu/T102_race_menu_helpers.h"
#include "game/menu/memorycard/T068_memorycard_entry.h"     /* F_0014AE00, gt2k_menu::MEMCARD_* */
#include "game/race/core/T121_race_entry.h"           /* RaceEntryBase, RaceEntryCar */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization, RaceResult */
#include "game/race/core/T129_race_base.h"            /* RaceInfoBlock / RaceInfoSlot */
#include "game/race/core/T130_race_ps2_base.h"        /* RacePS2Base */
#include "game/race/modes/T139_race_solitaire.h"       /* RaceSolitaire::virtual_58 */
#include "game/race/modes/T151_race_championship.h"    /* RaceChampionshipInformation, ChampionshipEntry */
#include "game/menu/framework/T261_menu_draw_state.h"      /* menu_draw_reset */
#include "game/menu/framework/T262_menu_bgm.h"             /* menu_bgm_play_if_changed */
#include "game/car/model/T278_show_room_car.h"        /* ShowRoomCar (SetupShowRoomCar, power) */
#include "game/car/physics/T280_car_condition.h"        /* CarCondition */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState (st: DynCarState, T288) */
#include "game/race/dynamics/T303_race_time_events.h"     /* car_best_lap */
#include "game/save/T431_ranking_code.h"         /* RankingRecord::encode */
#include "game/sound/T464_bgm.h"                  /* bgm_stop */
#include "engine/pdistd/T471_unit_arena.h"           /* _UnitArenaBase (BlockPool) */
#include "game/data/T481_car_data_base.h"        /* nameToCode */
#include "game/data/T482_car_equipments.h"       /* CarEquipments (getFronttireKind, isTuned) */
#include "game/data/T484_race_database.h"        /* RaceDataBase */
#include "game/save/T488_setting_serialize.h"    /* SettingSerialize (Serialize::Base::unseal, T509) */
#include "game/data/T489_car_database.h"         /* db_* lookups, race_name_split, RACE_KIND_* */
#include "engine/io/T503_vol_thread.h"           /* file_read_blocking, vol_device_object */
#include "engine/pdistd/T534_sprintf.h"              /* sprintf */
#include "engine/pgl/G001_flare.h"                /* gfx_vsync */
#include "libc.h"                           /* strcpy, strlen, memset */
#include "game/menu/race_menu/shortmenu_internal.h"                           /* g_003514F0, the named constants */
#include "game/app/pc_rev_cut.h"                 /* PC (2026-11-20): pc_rev_cut_demo */
#include "game/app/pc_shift.h"                   /* PC (2026-11-20): pc_shift_demo */

using namespace gt2k_menu;

/* ------------------------------------------------------------------------ */
/* Running a short menu                                                     */
/* ------------------------------------------------------------------------ */

/* The ShortMenuArgs of the running session (T102 .sdata, after "demo/%s":
 * no initialiser, so egcs writes it as zeros at the end of the file's
 * output; $gp-relative from shortmenu_internal.h's extern). */
ShortMenuArgs *g_003514F0;                  /* 0x003514F0 */

/* args->result = QM_EXIT and g_003514F0 = args while the menu runs.  The
 * menu heap is a SHORT_MENU_HEAP_ALLOC-byte alloca (the frame uses $fp),
 * aligned to 64 bytes (the size is a variable, reduced by the alignment:
 * retail keeps 0x40040 in $s2 for the alloca and the heap size; the mask
 * is int arithmetic, retail's -64).  race (may be NULL) is drawn and run behind the
 * menu; pageArg is imported into the first page; startPage, if any, opens
 * over it; params->bgmGroup is the music (menu_bgm_play_if_changed's group).
 * The ShortmenuControl's implicit destructor is expanded at the end of the
 * scope, after the result is read: RootObject::~RootObject (+0x294) and
 * GameObject::~GameObject (+0x290) with flags 2, then ~ControlBase with
 * flags 2; the landing pad does the same. */
/* PC (ROADMAP 18): the menu heap of a session from the main heap (was an
 * alloca of SHORT_MENU_HEAP_ALLOC bytes), freed when the function returns:
 * declared before the control, it goes after the control's destructor, as
 * the alloca did.  No memory: an empty heap (menu_heap_push(NULL): every
 * block from the main heap). */
struct ShortMenuHeapBlock {
    explicit ShortMenuHeapBlock(int size) : mem((char *)memalign(0x40, size)) {}
    ~ShortMenuHeapBlock() { free(mem); }
    char *mem;
};

// FUNC 0x0012E2C8 run_short_menu
int run_short_menu(ShortMenuParams *params, ShortMenuArgs *args, RacePS2Base *race,
                   const char *pageArg, const char *startPage)
{
    int heapSize = SHORT_MENU_HEAP_ALLOC;
    ShortMenuHeapBlock heapBlock(heapSize); /* PC: was __builtin_alloca(heapSize) */
    char *block = heapBlock.mem;
    args->result = QM_EXIT;
    char *heap = (char *)(((int)block + 0x3F) & ~0x3F);
    g_003514F0 = args;
    heapSize -= heap - block;

    /* implicit: ~RootObject, ~GameObject, ~ControlBase (the implicit ~ShortmenuControl of control, inline) */
    ShortmenuControl control;               /* fp+0x000 */
    control.loadPages(params->file);
    if (race != NULL)
        control.render.setRace(race);
    control.startPage = startPage;
    menu_bgm_play_if_changed(params->bgmGroup, 0, 1);
    menu_draw_reset();
    control.setHeap(heap, heapSize);
    control.run(params->page, pageArg);
    if (race != NULL)
        control.render.clearRace();
    control.releaseHeap();
    gfx_vsync(1);
    control.unloadPages();
    g_003514F0 = NULL;
    bgm_stop();
    return args->result;
    /* ~control: RootObject::~RootObject, GameObject::~GameObject, ~ControlBase */
}

/* The same with the licence variant of the control (T103): no music change
 * and no bgm_stop; params->bgmGroup goes to the control, and flag != 0
 * sets its replayMode to 1 (else it keeps the constructor's 2).  The
 * implicit ShortmenuLicenseControl destructor is expanded in the same way
 * (RootObject, GameObject, ~ControlBase, flags 2). */
// FUNC 0x0012E480 run_short_menu_license
int run_short_menu_license(ShortMenuParams *params, ShortMenuArgs *args, RacePS2Base *race,
                           const char *pageArg, const char *startPage, int flag)
{
    int heapSize = SHORT_MENU_HEAP_ALLOC;
    ShortMenuHeapBlock heapBlock(heapSize); /* PC: was __builtin_alloca(heapSize) */
    char *block = heapBlock.mem;
    args->result = QM_EXIT;
    char *heap = (char *)(((int)block + 0x3F) & ~0x3F);
    g_003514F0 = args;
    heapSize -= heap - block;

    /* implicit: ~RootObject, ~GameObject, ~ControlBase (the implicit ~ShortmenuLicenseControl of control, inline) */
    ShortmenuLicenseControl control;        /* fp+0x000 */
    control.loadPages(params->file);
    if (race != NULL)
        control.render.setRace(race);
    control.startPage = startPage;
    control.bgmGroup = params->bgmGroup;
    if (flag != 0)
        control.replayMode = 1;
    menu_draw_reset();
    control.setHeap(heap, heapSize);
    control.run(params->page, pageArg);
    if (race != NULL)
        control.render.clearRace();
    control.releaseHeap();
    gfx_vsync(1);
    control.unloadPages();
    int result = args->result;
    g_003514F0 = NULL;
    return result;
    /* ~control: RootObject::~RootObject, GameObject::~GameObject, ~ControlBase */
}

/* ------------------------------------------------------------------------ */
/* Orders                                                                   */
/* ------------------------------------------------------------------------ */

/* The place of entry `number' in the rank order list (0 = first); the entry
 * count if it is not there. */
// FUNC 0x0012E638 race_entry_rank
int race_entry_rank(RaceOrganization *org, int number)
{
    RaceEntryBase *entries = org->entries;
    int count = entries->count;
    RaceEntryCar *car = entries->rankTop;
    int place;

    for (place = 0; place < count; place++) {
        if (car->number == number)
            break;
        car = car->rankNext;
    }
    return place;
}

/* The grid order (RaceEntryCar::order) by power: entry 0 (the player)
 * starts last (count - 1); the others by weight / power (an integer
 * division, kept as a float), the smallest ratio first, equal ratios in
 * entry order.  Each car's ShowRoomCar is set up in a new condition (a
 * CarCondition constructed every time).  The three arrays come from
 * operator new[] and are freed with operator delete, not delete[] (sic).
 * The weight is read before the power is computed. */
// FUNC 0x0012E670 race_order_by_power
void race_order_by_power(RaceOrganization *org)
{
    ShowRoomCar showRoom;                   /* sp+0x0000 (a 0x1350-byte slot) */
    int rpm;                                /* sp+0x1370 */
    int i, j, place;

    int count = org->entries->count;
    /* implicit: operator new[], operator delete (array new-expressions, the deletes below) */
    int *ahead = new int[count];            /* the cars ahead of each */
    int *taken = new int[count];            /* places given */
    float *weightPerPower = new float[count];

    for (i = 0; i < count; i++) {
        taken[i] = 0;
        ahead[i] = 0;
        weightPerPower[i] = 0.0f;
    }
    for (i = 0; i < count; i++) {
        CarCondition condition;             /* sp+0x1350 */
        showRoom.SetupShowRoomCar(&org->entries->cars[i]->info, &condition);
        int weight = showRoom.spec.weight;
        /* PC: the language's own units (retail's figure), not the shown ones
         * (game/app/pc_units.h) */
        weightPerPower[i] = (float)(weight / showRoom.pcPowerGame(&rpm, 0));
    }
    for (i = 1; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (weightPerPower[i] == weightPerPower[j])
                continue;
            if (weightPerPower[j] < weightPerPower[i])
                ahead[i]++;
            else
                ahead[j]++;
        }
    }
    /* count - 1 here, not before the first loop: GCSE computes it on both
     * paths after that loop's entry test and spills it to sp+0x1378 */
    org->entries->cars[0]->order = count - 1;
    for (i = 1; i < count; i++) {
        place = ahead[i];
        while (taken[place] == 1)
            place++;
        org->entries->cars[i]->order = place;
        taken[place] = 1;
    }
    delete ahead;                           /* sic: operator delete */
    delete taken;
    delete weightPerPower;
}

/* The order (RaceEntryCar::order) by best lap (car_best_lap), the fastest
 * first, equal times in entry order.  Nothing when the player (entry 0) has
 * no time (RACE_TIME_INVALID).  The first loop is what is left of a debug
 * print of every car's time (min / sec / ms): the calls, and a
 * divide-by-zero check of the constant 60000 (break 7, never taken).  The
 * arrays come from operator new[] and are freed with operator delete (sic).
 * Each comparison fetches both times twice. */
// FUNC 0x0012E920 race_order_by_time
void race_order_by_time(RaceOrganization *org)
{
    int i, j, place;
    int count = org->entries->count;

    if (car_best_lap(org->entries->cars[0]->state) >= RACE_TIME_INVALID)
        return;
    for (i = 0; i < count; i++) {
        /* every car's time for a debug print that is compiled out: the
         * divisions are dead, egcs keeps the divide-by-zero check of 60000
         * and the constants (retail) */
        u32 lap = car_best_lap(org->entries->cars[i]->state);
        int min = lap / 60000;
        int sec = lap / 1000 % 60;
        int ms = lap % 1000;
        (void)min; (void)sec; (void)ms;
    }

    /* implicit: operator new[], operator delete (array new-expressions, the deletes below) */
    int *ahead = new int[count];            /* operator new[] */
    int *taken = new int[count];
    for (i = 0; i < count; i++) {
        taken[i] = 0;
        ahead[i] = 0;
    }
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            u32 lapI = car_best_lap(org->entries->cars[i]->state);
            u32 lapJ = car_best_lap(org->entries->cars[j]->state);
            if (lapI == lapJ)
                continue;
            lapI = car_best_lap(org->entries->cars[i]->state);
            if (car_best_lap(org->entries->cars[j]->state) < lapI)
                ahead[i]++;
            else
                ahead[j]++;
        }
    }
    for (i = 0; i < count; i++) {
        place = ahead[i];
        while (taken[place] == 1)
            place++;
        org->entries->cars[i]->order = place;
        taken[place] = 1;
    }
    delete ahead;                           /* sic: operator delete */
    delete taken;
}

/* ------------------------------------------------------------------------ */
/* Licence and event results                                                */
/* ------------------------------------------------------------------------ */

/* Records the result of licence test `race': the medal from the
 * organization's result rank (2 gold, 3 silver, 4 bronze, else none; rank
 * <= 0: LICENSE_MEDAL_NONE and nothing recorded).  *licence = the test's
 * licence (race_licence_split's kinds RACE_KIND_LICENSE_B .. _R = licences
 * 0..5, else -1).  *flags |= 1 when the licence is passed now and was not
 * before, |= 2 the same for all gold.  Returns the medal.  (At the start, a
 * divide-by-zero check of the constant 60000 is left of a debug print.) */
// FUNC 0x0012EBB0 license_record_result
int license_record_result(RaceOrganization *org, const char *race, int *licence, int *flags)
{
    int kind;                               /* sp+0x00 */
    int test;                               /* sp+0x04 */
    int medal, lic;

    {
        /* the result time for a debug print that is compiled out: the
         * divisions are dead, egcs keeps the divide-by-zero check of 60000
         * and the constants (retail) */
        u32 time = org->result.time;
        int min = time / 60000;
        int sec = time / 1000 % 60;
        int ms = time % 1000;
        (void)min; (void)sec; (void)ms;
    }
    if (org->result.rank <= 0)
        return LICENSE_MEDAL_NONE;
    medal = LICENSE_MEDAL_NONE;
    switch (org->result.rank) {
    case 2:
        medal = LICENSE_MEDAL_GOLD;
        break;
    case 3:
        medal = LICENSE_MEDAL_SILVER;
        break;
    case 4:
        medal = LICENSE_MEDAL_BRONZE;
        break;
    }
    race_licence_split(race, &kind, &test);
    switch (kind) {                         /* jump table 0x003154F0 */
    case RACE_KIND_LICENSE_B:
        lic = 0;
        break;
    case RACE_KIND_LICENSE_A:
        lic = 1;
        break;
    case RACE_KIND_LICENSE_IB:
        lic = 2;
        break;
    case RACE_KIND_LICENSE_IA:
        lic = 3;
        break;
    case RACE_KIND_LICENSE_S:
        lic = 4;
        break;
    case RACE_KIND_LICENSE_R:
        lic = 5;
        break;
    default:
        lic = -1;
        break;
    }
    *licence = lic;
    int hadAllGold = getGameStatus()->getLicenseRecords()->isAllGold(lic);
    int hadPassed  = getGameStatus()->getLicenseRecords()->isPassed(lic, 0);
    getGameStatus()->getLicenseRecords()->setMedal(lic, test, medal);
    if (getGameStatus()->getLicenseRecords()->isPassed(lic, 0) != 0 && hadPassed == 0)
        *flags |= 1;
    if (getGameStatus()->getLicenseRecords()->isAllGold(lic) != 0 && hadAllGold == 0)
        *flags |= 2;
    return medal;
}

/* Records `rank' as the result of event `race' (EventResults::updatePlace;
 * whether it is a new best is not used). */
// FUNC 0x0012EDA8 event_record_result
void event_record_result(int rank, const char *race)
{
    getGameStatus()->getEventResults()->updatePlace(race, rank);
}

/* strcpy, then the copy in lower case (A..Z only; up to its strlen). */
// FUNC 0x0012EDF0 strcpy_lower
char *strcpy_lower(char *dst, const char *src)
{
    strcpy(dst, src);
    int length = strlen(dst);
    for (int i = 0; i < length && dst[i] != '\0'; i++) {
        u8 c = dst[i];
        if (c >= 'A' && c <= 'Z')
            dst[i] = c + ('a' - 'A');
    }
    return dst;
}

/* ------------------------------------------------------------------------ */
/* Car condition (odometer, engine, oil, dirt)                              */
/* ------------------------------------------------------------------------ */

/* Before the race: entry `entry' starts with the garage car's condition
 * (the 0x20 bytes copied unaligned: ldl / ldr). */
// FUNC 0x0012EE78 race_set_entry_condition
void race_set_entry_condition(GarageCar *car, RaceOrganization *org, int entry)
{
    org->entries->cars[entry]->condition = car->condition;
}

/* After the race: the garage car takes the condition of the entry's car
 * state; returns the odometer gain of the race (units of 2 m). */
// FUNC 0x0012EED8 race_get_entry_condition
int race_get_entry_condition(GarageCar *car, RaceOrganization *org, int entry)
{
    RaceEntryCar *entryCar = org->entries->cars[entry];
    RaceCarState *state = entryCar->state;
    car->condition = state->st.condition;
    return state->st.condition.odometer - entryCar->condition.odometer;
}

/* ------------------------------------------------------------------------ */
/* Memory card                                                              */
/* ------------------------------------------------------------------------ */

/* Runs the memory card screen `mode' (F_0014AE00) for race `race' / car
 * `car', with the kind and index of `raceName' (race_name_split; index -1
 * if it has none) as its licence and test.  The saves (replay, ghost, game)
 * get a work buffer: the course data are unloaded and a unit of their arena
 * is borrowed (_UnitArenaBase::alloc / release).  The ghost load has a case
 * of its own that does the same as the other modes. */
// FUNC 0x0012EF40 race_memcard
void race_memcard(int mode, u64 race, u64 car, const char *raceName, RaceOrganization *org)
{
    MemCardRaceInfo info;                   /* sp+0x10 */
    int kind;                               /* sp+0x20 */
    int index = -1;                         /* sp+0x24 */

    race_name_split(raceName, &kind, &index);
    info.licenceName = race_kind_text(kind);
    info.test = index;
    info.licence = kind;
    switch (mode) {
    case MEMCARD_REPLAY_SAVE:
    case MEMCARD_GHOST_SAVE:
    case MEMCARD_GAME_SAVE: {
        BlockPool *arena = org->unloadCourseData();     /* the course data's arena (0x002BB5B8) */
        void *work = arena->alloc(0);
        int size = arena->unitSize;     /* read before the arguments: retail loads it first */
        F_0014AE00(mode, race, car, &info, work, size, kind, index, NULL);
        arena->release(work);
        break;
    }
    case MEMCARD_GHOST_LOAD:
    default:
        F_0014AE00(mode, race, car, &info, NULL, 0, kind, index, NULL);
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Result lines of ShortMenuArgs                                            */
/* ------------------------------------------------------------------------ */

/* args->list in rank order (the list is sorted first, RaceEntryBase::
 * sortByRank): car, points, the time (the car state's lapStartTime once it
 * has finished: ? the race time at which it crossed the line;
 * RACE_TIME_INVALID before)
 * and, when `highlight', the player's line (entry 0) marked; without
 * `highlight' the mark stays 0 from the constructor.  One line per entry
 * (the list holds 6).  PC (ROADMAP 35): the race's result shows the top 6
 * of a bigger race (the lines past list[5] are not written); no list of
 * every car (allCount 0: the short menu's overlay stays off). */
// FUNC 0x0012F070 result_list_by_rank
void result_list_by_rank(RaceOrganization *org, ShortMenuArgs *args, int highlight)
{
    org->entries->sortByRank();
    RaceEntryCar *car = org->entries->rankTop;
    ShortMenuResultEntry line;              /* sp+0x00 (its constructor calls clear) */
    /* implicit: clear (the inline constructor calls it out of line) */
    args->allCount = 0;                     /* PC: the results keep the top 6 */
    for (int i = 0; i < org->entries->count; i++) {
        if (i >= 6)                         /* PC: list[6]: the first 6 places */
            break;
        line.car = car->info.carCode;
        if (car->state->st.finished != 0)
            line.time = car->state->st.lapStartTime;
        else
            line.time = RACE_TIME_INVALID;
        if (highlight != 0) {
            if (car->number == 0)
                line.highlight = 1;
            else
                line.highlight = 0;
        }
        line.points = car->points;
        args->list[i] = line;
        car = car->rankNext;
    }
}

/* args->list[car->order] for every entry: car, points, the best lap
 * (car_best_lap; RACE_TIME_INVALID when noTime) and the first entry (the
 * player) marked.  Not exact (93%): retail gives the car $a0 and keeps
 * org->entries (the loop test's and GCSE's copy for the points line) in $v1
 * throughout; here global-alloc gives the car $v1 first (priority 4 against
 * 1 for the entries copy), so the copy takes $a0 and costs two moves.
 * Tried: no car local, the reload for car_best_lap, an inline wrapper for
 * it, the time as a ?:, a temporary, both branch senses, the order load
 * after the car code; the reference compiler gives the same.
 * PC (ROADMAP 35): a race of up to PC_RACE_CARS_MAX cars: list[] takes the
 * orders 0..5 (retail wrote list[order] for every order: past list[5] with
 * more than 6 cars), every line also goes to all[order], and allCount = the
 * cars (the pre-race menu's list of every car, pc_race_list.h). */
// FUNC 0x0012F160 result_list_by_order
void result_list_by_order(RaceOrganization *org, ShortMenuArgs *args, int noTime)
{
    ShortMenuResultEntry line;              /* sp+0x00 (its constructor calls clear) */
    /* implicit: clear (the inline constructor calls it out of line) */

    int cars = org->entries->count;         /* PC: the list of every car */
    args->allCount = cars < PC_RACE_CARS_MAX ? cars : PC_RACE_CARS_MAX;
    for (int i = 0; i < org->entries->count; i++) {
        RaceEntryCar *car = org->entries->cars[i];
        int order = car->order;
        line.car = car->info.carCode;
        if (noTime != 0)
            line.time = RACE_TIME_INVALID;
        else
            line.time = car_best_lap(car->state);
        line.points = org->entries->cars[i]->points;
        if (i == 0)
            line.highlight = 1;
        else
            line.highlight = 0;
        if (order >= 0 && order < 6)        /* PC: list[6] (retail: list[order] unchecked) */
            args->list[order] = line;
        if (order >= 0 && order < PC_RACE_CARS_MAX)     /* PC: every car */
            args->all[order] = line;
    }
}

/* args->list[i] from champ->getEntryByOrder(i) for every entry (the
 * championship standings): car, points and highlight; the time stays
 * RACE_TIME_INVALID from the constructor.  The returned temporary (at
 * sp+0x220) is assigned to `standing' (ChampionshipEntry's implicit
 * assignment: CarEquipments::operator=, then the three words) and destroyed
 * (~CarEquipments, flags 2) each time; `standing' is built by
 * CarEquipments() and destroyed the same way at the end.
 * PC (ROADMAP 35): list[] takes the first 6 (retail wrote list[i] for every
 * entry), every line also goes to all[i], allCount = the cars (the
 * championship menu's list of every car, pc_race_list.h). */
// FUNC 0x0012F290 result_list_from_entries
void result_list_from_entries(RaceOrganization *org, ShortMenuArgs *args,
                              RaceChampionshipInformation *champ)
{
    ShortMenuResultEntry line;              /* sp+0x000 (its constructor calls clear) */
    /* implicit: clear (the inline constructor calls it out of line) */
    /* implicit: CarEquipments, ~CarEquipments, operator= (member of ChampionshipEntry: constructor, the temporary's and standing's destructor, the assignment) */
    ChampionshipEntry standing;             /* sp+0x020 CarEquipments::CarEquipments */

    int cars = org->entries->count;         /* PC: the list of every car */
    args->allCount = cars < PC_RACE_CARS_MAX ? cars : PC_RACE_CARS_MAX;
    for (int i = 0; i < org->entries->count; i++) {
        /* CarEquipments::operator=, the words; the temporary's ~CarEquipments */
        standing = champ->getEntryByOrder(i);
        line.car = standing.car.carCode;
        line.points = standing.points;
        line.highlight = standing.highlight;
        if (i < 6)                          /* PC: list[6] */
            args->list[i] = line;
        if (i < PC_RACE_CARS_MAX)           /* PC: every car */
            args->all[i] = line;
    }
}

/* ------------------------------------------------------------------------ */
/* Ranking code, demo setting, race DB                                      */
/* ------------------------------------------------------------------------ */

/* Fills the 0x20-byte `out' with the ranking code of a RankingRecord
 * (RankingRecord::encode, 0x1E characters): the car (its index, isTuned,
 * the power and the weight of a ShowRoomCar set up in the car's own
 * condition), the course (its index; `course' is a course code: the
 * callers pass RaceInformation::getCourseCode), `kind', `time' and the tyre
 * kinds (folded below 13; both the dirt tyre on a dirt course).  name goes
 * to the record's +0x28, which encode does not read.  When kind is not the
 * max speed test (whose `time' is a speed x 1000), a divide-by-zero check
 * of the constant 60000 is left of a compiled-out debug print of the time
 * (min / sec / ms).  Not exact (91%, 108 of 110): retail copies each tyre
 * kind (the u8 call result) into $a1 for the compare and the subtraction
 * and selects $v0 itself in movn; here the value stays in $v0.  Tried: the
 * tyre as int / u8 / s8 / u16 / short / unsigned, ?: both ways, if / else
 * into the field or a local, the subtraction first, an inline fold helper
 * (on the call or a local); the reference compiler gives the same. */
// FUNC 0x0012F3F0 make_ranking_code
void make_ranking_code(char *out, int time, GarageCar *car, u64 course, int kind,
                       const char *name)
{
    RankingRecord record;                   /* sp+0x0000 */
    ShowRoomCar showRoom;                   /* sp+0x0030 (a 0x1350-byte slot) */
    int rpm;                                /* sp+0x13B0 */
    CarEquipments &equipments = *car;       /* GarageCar +0x10 (a reference: no NULL test) */

    showRoom.SetupShowRoomCar(&equipments, &car->condition);
    record.zero = 0;
    record.carIndex = db_car_index(car->carCode);
    record.courseIndex = db_course_index(course);
    record.tuned = equipments.isTuned();
    record.power = showRoom.pcPowerGame(&rpm, 0);  /* PC: the language's own units (game/app/pc_units.h) */
    record.weight = showRoom.spec.weight;
    record.time = time;
    record.name = name;
    record.kind = kind;
    int tyre = equipments.getFronttireKind();
    record.tyreFront = tyre < TIRE_KIND_SECOND_SET ? tyre : tyre - TIRE_KIND_SECOND_SET;
    tyre = equipments.getReartireKind();
    record.tyreRear = tyre < TIRE_KIND_SECOND_SET ? tyre : tyre - TIRE_KIND_SECOND_SET;

    RaceDataBase db;                        /* sp+0x1380 */
    if (db.getCourseKind(course) == COURSE_KIND_DIRT) {
        record.tyreFront = TIRE_KIND_DIRT;
        record.tyreRear = TIRE_KIND_DIRT;
    }
    if (kind != RANKING_KIND_MAX_SPEED) {
        /* the time for a debug print that is compiled out: the divisions
         * are dead, egcs keeps the divide-by-zero check of 60000 and the
         * constants (retail) */
        int min = time / 60000;
        int sec = time / 1000 % 60;
        int ms = time % 1000;
        (void)min; (void)sec; (void)ms;
    }
    memset(out, 0, 0x20);
    record.encode(out, 0x1E);
}

/* (? name) The stack buffer of load_demo_setting: DEMO_FILE_SIZE bytes,
 * 64-byte aligned by get() (int arithmetic: retail's mask is the signed
 * -64).  Its destructor is declared and empty: retail has an exception
 * region over the whole body of load_demo_setting whose landing pad only
 * rethrows (0x0012F5D8..0x0012F724, pad 0x0012F760), the cleanup of a
 * local constructed first that has no code. */
struct DemoBuffer {
    /* PC (ROADMAP 18): on the heap (was DEMO_FILE_SIZE + 0x40 bytes of the
     * stack), as big as the file is (DEMO_FILE_SIZE at least), freed with
     * the buffer; get(path) makes it (NULL: no memory). */
    DemoBuffer() : mem(NULL), size(DEMO_FILE_SIZE), read(0) {}
    ~DemoBuffer() { free(mem); }
    u8 *get(const char *path)
    {
        read = (u32)file_read_size(path, 1, &vol_device_object);
        if (read > size) {
            size = read;
            printf("PC: demo file %s: 0x%X bytes (retail at most 0x%X)\n", path, read,
                   (u32)DEMO_FILE_SIZE);
        }
        mem = (u8 *)memalign(0x40, size);
        return mem;
    }
    u8 *mem;
    u32 size;                               /* the buffer's bytes */
    u32 read;                               /* the file's (64-aligned): what the read fills */
};

/* Reads "demo/<name>" (at most DEMO_FILE_SIZE bytes, unpacked, into a
 * 64-byte aligned stack buffer) and applies its setting to the race: a
 * SettingSerialize (its inline constructor and destructor expanded:
 * Serialize::Base(), the vptr, CarEquipments(); ~CarEquipments, then
 * Serialize::Base::~Base) unseals the data; the RaceInfoBlock found at data
 * + its first word gives entry 0 its slot 0 options (steeringAssist,
 * transmission as a flag, assistAsm as a flag, assistTcs; the bytes are
 * s8, read with lb) and goes to RaceSolitaire slot 58 (the
 * replay input plays the records after it).  0 if the file cannot be read,
 * else 1.  Not exact (96%, 126 of 127): retail computes the aligned buffer
 * once into $s0 (the read's argument) and copies it to $s2 before sprintf
 * (the data pointer used afterwards); here one register holds both.  Tried:
 * get() once or twice (before / after the read, in the unseal), two
 * variables, the expression without the class; the reference compiler
 * gives the same. */
// FUNC 0x0012F5A8 load_demo_setting
int load_demo_setting(RaceSolitaire *race, const char *name)
{
    char path[0x80];                        /* sp+0x00000 */
    DemoBuffer file;                        /* sp+0x00080 */

    sprintf(path, "demo/%s", name);
    u8 *data = file.get(path);              /* PC: sized by the file, after its name */
    if (data == NULL)                       /* PC: no memory */
        return 0;
    if (file_read_blocking_cap(path, data, file.size, 1, &vol_device_object) == NULL)  /* PC: _cap */
        return 0;

    /* Inline SettingSerialize(): Serialize::Base::Base, the vptr,
     * CarEquipments::CarEquipments; at the end the inline destructor: the
     * vptr, CarEquipments::~CarEquipments, Serialize::Base::~Base. */
    /* implicit: Base, CarEquipments, ~CarEquipments, ~Base (the inline SettingSerialize constructor and destructor) */
    SettingSerialize setting;               /* sp+0x41A00 */
    /* PC: with the bytes read; a header past them (or a bad checksum, which
     * retail applied anyway, inverted) is a file that cannot be read */
    if (setting.unseal(data, file.read) == 0)
        return 0;
    RaceInfoBlock *info = (RaceInfoBlock *)(data + *(int *)data);
    /* PC (2026-11-20): the rev limiter and the gear shifts the demo was
     * recorded with (none: the original) while it plays, not the settings'
     * the session's race took (game/app/pc_rev_cut.h, pc_shift.h) */
    pc_rev_cut_demo(race->org, info);
    pc_shift_demo(race->org, info);
    race->org->setEntrySteeringAssist(info->slots[0].steeringAssist, 0);
    race->org->setEntryTransmission(info->slots[0].transmission != 0, 0);
    race->org->setEntryAsm(info->slots[0].assistAsm != 0, 0);
    race->org->setEntryTcs(info->slots[0].assistTcs, 0);
    race->virtual_58(info);
    return 1;
}

/* Builds a RaceDataBase, selects race `name' in it and destroys it again
 * (RaceDataBase::RaceDataBase, ~RaceDataBase with flags 2).  No lasting
 * effect: select only looks the record up (CarDataBase::getRecord) and
 * stores it in the local.  The race modes call it first with their race. */
// FUNC 0x0012F7A8 select_race
void select_race(const char *name)
{
    RaceDataBase db;                        /* sp+0x00 */
    db.select(nameToCode(name));
}

// FUNC 0x0012F820 ShortMenuResultEntry::clear
/* inline in game/menu/race_menu/T102_race_menu_helpers.h; egcs emits this copy (the
 * inline constructor calls it, from T102's locals and the race modes'
 * array loops): car 0, points 0, time RACE_TIME_INVALID, highlight 0; a
 * member function, not the constructor (it does not return this). */
