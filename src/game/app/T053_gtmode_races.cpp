/* T053 - the race modes of the GT mode, 0x00131410..0x00133F48.
 *
 * main() enters them through GameStatus::menu.nextMode (T000): the machine
 * test (mode 7, main_machine_test), the licence tests (mode 6,
 * main_license_test), one GT mode race (modes 3 / 4, main_gtmode_race) and
 * a championship (mode 2, main_championship).  Each one runs between
 * race_begin(CONTROL_RECORD_SIZE, CONTROL_RECORDS) and race_end (T007): it
 * builds a race information for GameStatus::menu.event and the player's car
 * (menu.raceCar[0]), the RaceOrganization the information makes
 * (race_organization_create) and a race object; then it loops over a short
 * menu (run_short_menu, T102) drawn over the race (RACE_DEMO) until the
 * player picks "exit".  The menu's action (QM_*) starts the race
 * (RacePS2Base::virtual_41 RACE_LIVE), replays it (RACE_REPLAY), opens the
 * setting / option / memory card / records screens, ...  The odometer gain
 * of the races goes to GameStatus::distance.
 *
 * The four entry points take no mode argument: main (T000) stores the
 * runMode into GameStatus::menu.runMode first, and the constant left in
 * $a0 / $a1 is the store's register, not an argument (u001's matching pass).
 *
 * ShortMenuArgs' inline constructor (expanded in every mode) constructs the
 * 6 result lines and the 12 rank lines (their constructors call the lines'
 * inline clear(): T102's copy 0x0012F820, T052's 0x001313D0), then sets the
 * members.
 *
 * Matching notes (u017's pass): the modes reach the menu arguments, the
 * copied RaceResult and some other objects through inline accessors (the
 * object's own address in a register plus the member's offset, where a
 * direct access to a local beyond sp+0x8000 would split the frame offset);
 * the switch cases are in retail's block order (EXIT last); the GO_RACE
 * prize screens are an if / else of two calls (cross-jumped into one jal).
 *
 * T053 also holds unreferenced copies of inline functions (the linker kept
 * the first definition of each): the two entry lines' clear(), Cell<GarageCar>'s
 * and GarageCar's destructors and their typeinfo functions.
 *
 * The includes, in the original's class order (egcs writes the type names
 * of the classes it sees in the reverse order of their definitions; retail
 * T053's .rodata 0x00316080..0x0031648E): "bad_alloc" first (<new>, retail
 * 0x00315E10), then the GameStatus's garage, RecordsMenuArg,
 * RegurationResult, UnitArena<64>, the race camera and displays, the menu
 * classes, the race menus, PauseMenuWithRetry, the prize model classes,
 * ExaminationDisplayer, LicensePause, SmartIndexT<int> and ShortmenuControl
 * last (as T000's, with the licence race's classes in place of the split
 * battles).  The race headers after them add nothing new; the screens T053
 * calls come from their entry headers (retail T053 has none of their
 * controllers' names).  T102_race_menu_helpers.h after the garage:
 * ShortMenuResultEntry / ShortMenuRankEntry are defined after GarageCar (and
 * after Cell<GarageCar>'s destructor, which T036_garage_car.h's
 * garage_car_cell_delete instantiates right after the class), so their
 * clear() copies come before that destructor at the end of the unit (retail
 * 0x00133DC8 / 0x00133DE8 / 0x00133E08). */
#include "game/app/gtmode_races_internal.h"
#include <new>                              /* first: "bad_alloc"; the placement new of the races */
#include "game/save/T429_game_status.h"          /* gt2k_game::GameStatus, getGameStatus; GarageCar */
#include "game/menu/records/T089_records_menu_args.h"    /* gt2k_menu::RecordsMenuArg_0_400, ..._0_1000,
                                               ...MaxSpeed, ...License */
#include "game/menu/gtmode/T045_reguration_result.h"    /* (no use: RegurationResult's name) */
/* UnitArena<64> instantiated by T180's g_002C2F50, with its type name
 * (T153_race_course.h leaves it out unless asked). */
#define GAME_T153_EMITS_TYPEINFO
#include "game/race/core/T153_race_course.h"
#include "game/car/model/T180_race_car_model.h"
#include "game/camera/T383_race_camera_base.h"     /* RaceCameraBase */
#include "game/race/display/T154_race_display.h"         /* the race displays (their T157's "" opens the .sdata) */
#include "game/menu/framework/T216_menu_transitive.h"      /* the menu classes */
#include "game/menu/race_menu/T198_race_menu_button.h"
#include "game/menu/race_menu/T199_race_menu_layer.h"
#include "game/race/pause/T169_race_pause.h"
#include "game/race/pause/T170_replay_pause.h"
#include "game/menu/records/T200_result_board.h"
#include "game/race/pause/T171_race_pause_with_retry.h" /* PauseMenuWithRetry */
#include "game/menu/gtmode/T460_prize_base.h"           /* PrizeModelSetContext, CallBackWhenPrizeRendering */
#include "game/race/display/T141_race_time_trial_display.h"  /* ExaminationDisplayer */
#include "game/race/modes/T144_race_license.h"         /* RaceLicense (LicensePause) */
#include "game/menu/race_menu/T111_shortmenu_control.h"    /* (no use: SmartIndexT<int>, ShortmenuControl) */
#include "libc.h"                           /* strcmp */
#include "game/app/T053_gtmode_races.h"
#include "game/app/T007_game_init.h"            /* race_begin, race_end */
#include "game/menu/race_menu/T102_race_menu_helpers.h"    /* ShortMenuArgs / Params, the T052 rank lines, QM_*,
                                               run_short_menu(_license), race_memcard, ... */
#include "game/debug/T055_logger.h"               /* logger_run */
#include "game/menu/gtmode/T056_trophy_prize_entry.h"   /* trophy_prize_screen */
#include "game/menu/gtmode/T059_license_get_entry.h"    /* license_get_screen */
#include "game/menu/gtmode/T060_trophy_get_entry.h"     /* trophy_get */
#include "game/menu/gtmode/T061_car_get_entry.h"        /* F_0013F508 (the prize cars, the endings) */
#include "game/menu/options/T062_option_control.h"       /* F_0013FE58 (the options menu) */
#include "game/menu/memorycard/T068_memorycard_entry.h"     /* gt2k_menu::MEMCARD_* (race_memcard's modes) */
#include "game/menu/car_settings/T080_setting_entry.h"        /* setting_menu */
#include "game/menu/records/T090_records_entry.h"        /* run_records_menu */
#include "game/menu/framework/T097_name_entry_screen.h"    /* name_entry */
#include "game/menu/race_menu/T115_race_option.h"          /* race_option_from_game_options */
#include "game/race/core/T121_race_entry.h"           /* RaceEntryBase, RaceEntryCar */
#include "game/race/core/T123_race_setup.h"           /* race_organization_create */
#include "game/race/core/T124_race_information.h"     /* RaceInformation */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization, RaceResult */
#include "game/race/core/T130_race_ps2_base.h"        /* RacePS2Base (virtual_41) */
#include "game/race/modes/T134_race_arcade.h"          /* RaceArcade */
#include "game/race/modes/T136_race_free_practice.h"   /* RaceFreePractice */
#include "game/race/modes/T139_race_solitaire.h"       /* RaceSolitaire (showGhost) */
#include "game/race/modes/T148_race_machine_test.h"    /* RaceMachineTest, RaceMachineTestInformation */
#include "game/race/modes/T151_race_championship.h"    /* RaceChampionship, RaceChampionshipInformation */
#include "game/race/core/T152_race_gtmode_information.h"  /* RaceGTmodeInformation */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState */
#include "game/race/dynamics/T303_race_time_events.h"     /* car_best_lap */
#include "game/save/T415_machine_test_records.h" /* MachineTestRecords, MACHINE_TEST_* */
#include "game/save/T420_license_records.h"      /* LicenseRecords, LICENSE_*, LicenseMedal */
#include "game/save/T427_menu_status.h"          /* gt2k_game::MenuStatus */
#include "game/save/T428_game_options.h"         /* ::GameOptions */
#include "game/save/T431_ranking_code.h"         /* RankingCode, RANKING_KIND_* */
#include "game/data/T481_car_data_base.h"        /* nameToCode, CarDataBase::isJapan / isUK */
#include "game/data/T482_car_equipments.h"       /* CarEquipments */
#include "game/data/T483_car_information.h"      /* CarInformation, __tf14CarInformation */
#include "game/data/T484_race_database.h"        /* RaceDataBase, COURSE_KIND_* */
#include "game/data/T489_car_database.h"         /* race_name_split, RACE_KIND_* */
#include "pc_ee.h"                          /* PC: ee_f2u, the EE's float -> unsigned conversion */
#include "game/app/pc_course_pick.h"            /* PC: pc_course_career (the course randomizer / forcer) */

/* T053's data starts unreferenced: its .rodata with <new>'s "bad_alloc" and
 * gtMenus' file names (only the table points at them), its .sdata with
 * T157's "" and gtMenus' "root" (the code's first reference is "M1000"). */
// DATA_START 0x00315E10
// DATA_START 0x00351508

/* T053's short menus: {file, first page, menu music}, copied to the modes'
 * ShortMenuParams.  One table in T053's .data 0x0029D678..0x0029D6D8:
 * writable (retail has it in .data, before the object's frame block) and
 * packed in 12-byte entries, as one array is (egcs aligns every separate
 * struct variable to 8 bytes). */
enum {
    GT_MENU_EVENT,                          /* the race */
    GT_MENU_RESULT,                         /* after the race */
    GT_MENU_CHAMP_EVENT,
    GT_MENU_CHAMP_RESULT,
    GT_MENU_MACHINE_TEST,                   /* + MACHINE_TEST_400M, _1000M, _MAX_SPEED */
    GT_MENU_LICENSE = GT_MENU_MACHINE_TEST + 3
};
static ShortMenuParams gtMenus[] = {                                                        /* 0x0029D678 */
    { "menu/qm_gt_event.mbl", "root", 0x15 },                                               /* 0x0029D678 */
    { "menu/qm_gt_result.mbl", "root", 0x16 },                                              /* 0x0029D684 */
    { "menu/qm_gt_event_champ.mbl", "root", 0x15 },                                         /* 0x0029D690 */
    { "menu/qm_gt_result_champ.mbl", "root", 0x16 },                                        /* 0x0029D69C */
    { "menu/qm_gt_mt_400.mbl", "root", 0x15 },                                              /* 0x0029D6A8 */
    { "menu/qm_gt_mt_1000.mbl", "root", 0x15 },                                             /* 0x0029D6B4 */
    { "menu/qm_gt_mt_max.mbl", "root", 0x15 },                                              /* 0x0029D6C0 */
    { "menu/qm_gt_license.mbl", "root", 0x15 }                                              /* 0x0029D6CC */
};

/* Enters a machine test result into the records (GameStatus machineTest)
 * when it makes the table.  The 0-400 m / 0-1000 m tests rank `time' (ms,
 * at most RECORD_TIME_MAX); the max speed test ranks `speed' (km/h), whose
 * x 1000 value (1..RECORD_TIME_MAX) goes into the ranking code instead of a
 * time.  insertTime / insertSpeed insert the value and give its place (-1:
 * not in the table); then the player types a name (name_entry) and the
 * name, the car code and the ranking code (make_ranking_code, kind 3 / 4 /
 * 5) are stored at that place.  Caller: run_machine_test.  Frame 0xE0: the
 * name sp+0x00, the ranking code sp+0x40. */
// FUNC 0x00131410 machine_test_new_record
void machine_test_new_record(int kind, u32 time, GarageCar *car, u64 course, f32 speed)
{
    MachineTestRecords *records = getGameStatus()->getMachineTestRecords();    /* s2 */
    char name[0x40];                                        /* sp+0x00 */
    RankingCode code;                                       /* sp+0x40 */
    int place;                                              /* s1 */

    switch (kind) {                                         /* cases in retail's block order */
    case MACHINE_TEST_MAX_SPEED: {
        u32 speedValue = ee_f2u(speed * 1000.0f);          /* fptoui (F_0027AEA0, libgcc's __fixunssfsi); PC: ee_f2u */
        /* implicit: fptoui (float to unsigned conversion) */
        if (speedValue - 1 > RECORD_TIME_MAX - 1)           /* unsigned: only 1..RECORD_TIME_MAX */
            break;
        place = records->insertSpeed(MACHINE_TEST_MAX_SPEED, speed);
        if (place < 0)
            break;
        name_entry(name);
        records->setName(MACHINE_TEST_MAX_SPEED, name, place);
        records->setCar(MACHINE_TEST_MAX_SPEED, car->carCode, place);
        make_ranking_code(code.s, speedValue, car, course, RANKING_KIND_MAX_SPEED, name);
        records->setCode(MACHINE_TEST_MAX_SPEED, code.s, place);
        break;
    }

    case MACHINE_TEST_1000M:
        if (time > RECORD_TIME_MAX)
            break;
        place = records->insertTime(MACHINE_TEST_1000M, time);
        if (place < 0)
            break;
        name_entry(name);
        records->setName(MACHINE_TEST_1000M, name, place);
        records->setCar(MACHINE_TEST_1000M, car->carCode, place);
        make_ranking_code(code.s, time, car, course, RANKING_KIND_1000M, name);
        records->setCode(MACHINE_TEST_1000M, code.s, place);
        break;

    case MACHINE_TEST_400M:
        if (time > RECORD_TIME_MAX)
            break;
        place = records->insertTime(MACHINE_TEST_400M, time);
        if (place < 0)
            break;
        name_entry(name);
        records->setName(MACHINE_TEST_400M, name, place);
        records->setCar(MACHINE_TEST_400M, car->carCode, place);
        make_ranking_code(code.s, time, car, course, RANKING_KIND_400M, name);
        records->setCode(MACHINE_TEST_400M, code.s, place);
        break;
    }
}

/* Mode 7 of main() (runMode 5): the machine test GameStatus::menu.event
 * ("M400" 0-400 m, "M1000" 0-1000 m, anything else the max speed test)
 * with the player's car: a RaceMachineTest(kind) under the short menu
 * qm_gt_mt_400 / _1000 / _max.mbl.  Before every menu the first three
 * places of the kind's records (and the best value) go into the menu's
 * ranking.  A run: the race, then, when car 0 has finished,
 * machine_test_new_record with its best lap and top speed, then the replay.
 * A new ghost replaces the old one after the "ghost_dlg" question (or at
 * once with GameOptions::ghost set).  "setting" with a change reloads the
 * car and drops the replay, the ghost and the logger data.  Frame 0x48F60:
 * information sp+0x0, the short menu's params sp+0x250, the race
 * sp+0x260, the menu arguments sp+0x48CE0, the rank line sp+0x48E80, then
 * raceName, gcar, carCode, distance, loop, startPage (sp+0x48E90 ..).
 * Returns 0. */
// FUNC 0x00131690 run_machine_test
int run_machine_test(void)
{
    race_begin(CONTROL_RECORD_SIZE, CONTROL_RECORDS);
    const char *raceName = getGameStatus()->getMenuStatus()->event;     /* sp+0x48E90 */
    GarageCar *gcar = getGameStatus()->getMenuStatus()->raceCar[0];     /* sp+0x48E94 */
    select_race(raceName);
    {
        RaceMachineTestInformation info;                    /* sp+0x00000 */
        CarInformation &car = *gcar;                        /* GarageCar +0x10 (no NULL test: a reference) */
        info.setCar(car);
        info.transmission = getGameStatus()->getGameOptions()->gearAuto ^ 1;
        info.vibration = getGameStatus()->getGameOptions()->vibration1p;
        info.setRaceCode(nameToCode(raceName));
        RaceOrganization *org = race_organization_create(&info);    /* s4 */

        ShortMenuParams params = gtMenus[GT_MENU_MACHINE_TEST + MACHINE_TEST_MAX_SPEED];  /* sp+0x00250 */
        int kind = MACHINE_TEST_MAX_SPEED;                  /* s6 */
        if (strcmp(raceName, "M1000") == 0) {
            params = gtMenus[GT_MENU_MACHINE_TEST + MACHINE_TEST_1000M];
            kind = MACHINE_TEST_1000M;
        }
        if (strcmp(raceName, "M400") == 0) {
            params = gtMenus[GT_MENU_MACHINE_TEST + MACHINE_TEST_400M];
            kind = MACHINE_TEST_400M;
        }

        RaceMachineTest race(kind);                         /* sp+0x00260 */
        race.org = org;
        u64 carCode = gcar->carCode;                        /* sp+0x48E98 */
        u64 courseCode = info.getCourseCode();              /* fp (direct: RaceInformation's) */

        ShortMenuArgs menuArgs;                             /* sp+0x48CE0: ShortMenuResultEntry() x 6,
                                                               ShortMenuRankEntry() x 12 */
        /* implicit: ShortMenuResultEntry::clear, ShortMenuRankEntry::clear (the entry
           constructors of ShortMenuArgs' arrays call them) */
        int distance = 0;                                   /* sp+0x48EA0 driven in the garage car */
        menuArgs.setRaceInfo(&info);
        int loop = 1;                                       /* sp+0x48EA4 */
        const char *startPage = NULL;                       /* sp+0x48EA8 the ghost question, once */
        RaceResult result;                                  /* sp+0x48E70 */

        do {
            {
                /* the first places of the kind's records (`line''s slot is
                 * the records argument's below).  The lines are copied
                 * through a reference: retail copies from line's own address
                 * (the register of its constructor call), where a direct
                 * copy splits the frame offset. */
                ShortMenuRankEntry line;                    /* sp+0x48E80 */
                const ShortMenuRankEntry &src = line;
                MachineTestRecords *records = getGameStatus()->getMachineTestRecords();    /* s3 */
                int i;
                if (kind == MACHINE_TEST_MAX_SPEED) {
                    menuArgs.setBestSpeed(records->getSpeed(MACHINE_TEST_MAX_SPEED, 0));
                    for (i = 0; i < RECORD_LINES_SHOWN; i++) {
                        line.speed = records->getSpeed(MACHINE_TEST_MAX_SPEED, i);
                        line.name = records->getName(MACHINE_TEST_MAX_SPEED, i);
                        menuArgs.rank[i] = src;
                    }
                } else {
                    menuArgs.setBestTime(records->getTime(kind, 0));
                    for (i = 0; i < RECORD_LINES_SHOWN; i++) {
                        line.time = records->getTime(kind, i);
                        line.name = records->getName(kind, i);
                        menuArgs.rank[i] = src;
                    }
                }
            }

            org->backgroundMode = 1;
            race.virtual_8(RACE_DEMO);                      /* direct: RaceMachineTest's */
            int action = run_short_menu(&params, &menuArgs, &race, NULL, startPage);   /* s1 */
            startPage = NULL;
            if (menuArgs.getGhostAsk() != 0 && menuArgs.getYes() != 0) {
                /* "yes": the new ghost replaces the old one */
                org->setupGhost(0, 0);
                menuArgs.setGhostAsk(0);
                menuArgs.setYes(0);
            }
            race.virtual_35();                              /* direct: RaceMachineTest's */
            org->backgroundMode = 0;

            switch (action) {                               /* jump table 0x00315F00 */
            case QM_GO_RACE: {
                race_option_from_game_options(&org->option);
                org->setEntrySteeringAssist(getGameStatus()->getGameOptions()->steeringAssist1p, 0);
                org->setEntryVibration(getGameStatus()->getGameOptions()->vibration1p, 0);
                org->setEntryTransmission(getGameStatus()->getGameOptions()->gearAuto ^ 1, 0);
                race_set_entry_condition(gcar, org, 0);
                race.virtual_41(RACE_LIVE, 1);              /* the race (direct: RacePS2Base's) */
                distance += race_get_entry_condition(gcar, org, 0);
                org->entries->sortByRank();
                result = org->result;
                menuArgs.setCanReplay(1);
                if (result.getBeatGhost() != 0) {
                    /* a new ghost: asked on the next menu */
                    menuArgs.setGhostAsk(1);
                    if (getGameStatus()->getGameOptions()->ghost != 0) {
                        menuArgs.setYes(1);                 /* taken without asking */
                        startPage = NULL;
                    } else {
                        menuArgs.setYes(0);
                        startPage = "ghost_dlg";
                    }
                }
                if (result.getLapRecord() != 0) {
                    org->F_0017AD28();
                    menuArgs.setCanLogger(1);
                    menuArgs.setCanSaveGhost(1);
                }
                RaceCarState *state = org->entries->cars[0]->state;
                if (state->st.finished != 0) {
                    f32 topSpeed = state->topSpeed;
                    machine_test_new_record(kind, car_best_lap(state), gcar, courseCode, topSpeed);
                }
                race.virtual_41(RACE_REPLAY, 1);
                break;
            }

            case QM_REPLAY:
                org->backgroundMode = 0;                    /* (again; it is already 0) */
                race.virtual_41(RACE_REPLAY, 1);
                break;

            case QM_SAVE_REPLAY:
                race_memcard(gt2k_menu::MEMCARD_REPLAY_SAVE, courseCode, carCode, raceName, org);
                break;

            case QM_SETTING:
                if (setting_menu(courseCode, org) != 0) {
                    /* the changed car: no replay, ghost or logger data of the old one */
                    org->setEntryInfo(0, &car);
                    org->unloadModels(0);
                    menuArgs.setCanLogger(0);
                    menuArgs.setCanSaveGhost(0);
                    menuArgs.setCanReplay(0);
                }
                break;

            case QM_OPTION:
                F_0013FE58();
                break;

            case QM_LOAD_GHOST:
                race_memcard(gt2k_menu::MEMCARD_GHOST_LOAD, courseCode, carCode, raceName, org);
                break;

            case QM_SAVE_GHOST:
                race_memcard(gt2k_menu::MEMCARD_GHOST_SAVE, courseCode, carCode, raceName, org);
                break;

            case QM_LOGGER:
                logger_run(org, 1);
                break;

            case QM_RECORDS:
                /* The records screen of the kind; the argument object shares
                 * the stack slot of `line' (its inline constructor stores the
                 * vptr only). */
                switch (kind) {
                case MACHINE_TEST_400M: {
                    gt2k_menu::RecordsMenuArg_0_400 recordsArg;
                    run_records_menu(&recordsArg);
                    break;
                }
                case MACHINE_TEST_1000M: {
                    gt2k_menu::RecordsMenuArg_0_1000 recordsArg;
                    run_records_menu(&recordsArg);
                    break;
                }
                case MACHINE_TEST_MAX_SPEED: {
                    gt2k_menu::RecordsMenuArgMaxSpeed recordsArg;
                    run_records_menu(&recordsArg);
                    break;
                }
                }
                break;

            case QM_EXIT:
                loop = 0;
                break;
            }
        } while (loop != 0);

        delete org;                                         /* virtual destructor, slot 0 (vptr +0x8C0) */
        getGameStatus()->incDistance(distance);
    }                                                       /* ~RaceMachineTest, ~RaceMachineTestInformation */
    race_end();
    return 0;
}

/* Mode 6 of main() (runMode 4): the licence test GameStatus::menu.event.
 * The car is the test's own: entry car 0 of the race record, or entry car 1
 * in the UK (CarDataBase::isUK) when car 0 is region-locked there
 * (F_00236A38), in the record's colour for it.  The race is a RaceLicense
 * under the licence short menu (run_short_menu_license, with the lower-case
 * test name as the imported page); the test's demonstration is loaded first
 * (load_demo_setting, "demo/<name>") and the menu runs over it.  The
 * demonstration flag of the organization is set while the menu runs and
 * SELECT's ghost display (RaceSolitaire::showGhost) is off.  A run with a
 * time is graded by license_record_result (the medal, 0 gold; the
 * session's best medal kept); a time that makes the test's records is
 * entered with a name and a ranking code (kind 2, made with a GarageCar of
 * the test car).  Afterwards: the "licence acquired" screen when the
 * licence was passed now, and the licence's prize car when every test of
 * the licence has gold and this session won gold.  Frame 0x3BDC0:
 * information sp+0x0, the car sp+0x250, the race sp+0x440, the menu
 * arguments sp+0x3B890, pageArg sp+0x3BA20, params sp+0x3BA40, name /
 * records argument sp+0x3BA50, the code's GarageCar sp+0x3BA90, the
 * ranking code sp+0x3BCC0, kind / test / resultLicence / resultFlags /
 * licence / bestMedal sp+0x3BCE0..0x3BCF4.  Returns 0. */
// FUNC 0x00132030 run_license_test
int run_license_test(void)
{
    race_begin(CONTROL_RECORD_SIZE, CONTROL_RECORDS);
    int licence = -1;                                       /* sp+0x3BCF0 LICENSE_B .. LICENSE_R */
    const char *raceName = getGameStatus()->getMenuStatus()->event;     /* s7 */
    int bestMedal = LICENSE_MEDAL_WORST;                    /* sp+0x3BCF4 the session's best medal */
    select_race(raceName);
    int resultFlags = 0;                                    /* sp+0x3BCEC license_record_result's LICENSE_FLAG_* */
    int resultLicence;                                      /* sp+0x3BCE8 its licence */
    {
        RaceMachineTestInformation info;                    /* sp+0x00000 */
        info.vibration = getGameStatus()->getGameOptions()->vibration1p;
        info.transmission = getGameStatus()->getGameOptions()->gearAuto ^ 1;
        info.setRaceCode(nameToCode(raceName));

        CarEquipments car;                                  /* sp+0x00250 the test's car */
        CarEquipments *pcar = &car;                         /* spilled (sp+0x3BCF8): retail sets presetCar
                                                               through it */
        RaceDataBase &db = info.db;                         /* s0: getEntryColor's `this' after the test */
        int entry = 0;                                      /* s1 */
        db.setupEntryCar(0, pcar);
        if (db.isUK() != 0 && pcar->F_00236A38() != 0) {
            /* the UK, and car 0 is not sold there: entry car 1 */
            entry = 1;
            db.setupEntryCar(1, pcar);
        }
        u64 color = (u32)db.getEntryColor(entry);           /* compared zero-extended to 64 bits */
        if (color != 0xFFFFFFFF)
            pcar->setColorCode(color);
        pcar->presetCar = 1;
        info.setCar(*pcar);
        RaceOrganization *org = race_organization_create(&info);    /* s3 */

        RaceLicense race;                                   /* sp+0x00440 (fp) */
        race.org = org;
        u64 carCode = car.carCode;                          /* sp+0x3BD00 */
        u64 courseCode = info.getCourseCode();              /* sp+0x3BD08 (direct: RaceInformation's) */

        int kind;                                           /* sp+0x3BCE0 */
        int test;                                           /* sp+0x3BCE4 the test's index in its licence */
        race_name_split(raceName, &kind, &test);
        switch (kind) {                                     /* jump table 0x00315F50 */
        case RACE_KIND_LICENSE_B:
            licence = LICENSE_B;
            break;
        case RACE_KIND_LICENSE_A:
            licence = LICENSE_A;
            break;
        case RACE_KIND_LICENSE_IB:
            licence = LICENSE_IB;
            break;
        case RACE_KIND_LICENSE_IA:
            licence = LICENSE_IA;
            break;
        case RACE_KIND_LICENSE_S:
            licence = LICENSE_S;
            break;
        case RACE_KIND_LICENSE_R:
            licence = LICENSE_R;
            break;
        }

        RaceResult result;                                  /* sp+0x3B880 (declared here: its slot is
                                                               before the menu arguments) */
        ShortMenuArgs menuArgs;                             /* sp+0x3B890: ShortMenuResultEntry() x 6,
                                                               ShortMenuRankEntry() x 12 */
        /* implicit: ShortMenuResultEntry::clear, ShortMenuRankEntry::clear (the entry
           constructors of ShortMenuArgs' arrays call them) */
        menuArgs.setRaceInfo(&info);
        char pageArg[0x20];                                 /* sp+0x3BA20 the test's name in lower case */
        strcpy_lower(pageArg, raceName);
        int loop = 1;                                       /* sp+0x3BD10 */
        const char *startPage = NULL;                       /* sp+0x3BD14 the ghost question, once */
        ShortMenuParams params = gtMenus[GT_MENU_LICENSE];               /* sp+0x3BA40 */
        load_demo_setting(&race, raceName);

        do {
            org->demonstration = 1;
            int ghostShown = race.showGhost;              /* s4: SELECT's ghost display, off during the menu */
            org->backgroundMode = 1;
            race.showGhost = 0;
            int action = run_short_menu_license(&params, &menuArgs, &race, pageArg, startPage, 1);  /* s2 */
            race.showGhost = ghostShown;
            startPage = NULL;
            if (menuArgs.getGhostAsk() != 0 && menuArgs.getYes() != 0) {
                /* "yes": the new ghost replaces the old one */
                org->setupGhost(0, 0);
                menuArgs.setGhostAsk(0);
                menuArgs.setYes(0);
            }
            org->backgroundMode = 0;
            org->demonstration = 0;

            switch (action) {                               /* jump table 0x00315F70 */
            case QM_TRY_AGAIN:
            case QM_GO_RACE: {
                race_option_from_game_options(&org->option);
                org->setEntrySteeringAssist(getGameStatus()->getGameOptions()->steeringAssist1p, 0);
                org->setEntryVibration(getGameStatus()->getGameOptions()->vibration1p, 0);
                org->setEntryTransmission(getGameStatus()->getGameOptions()->gearAuto ^ 1, 0);
                race.virtual_41(RACE_LIVE, 1);              /* the race (direct: RacePS2Base's) */
                menuArgs.setCanReplay(1);
                org->entries->sortByRank();
                result = org->result;
                if (result.getLapRecord() != 0) {
                    org->F_0017AD28();
                    menuArgs.setCanLogger(1);
                    menuArgs.setCanSaveGhost(1);
                }
                if (result.getBeatGhost() != 0) {
                    /* a new ghost: asked on the next menu */
                    menuArgs.setGhostAsk(1);
                    if (getGameStatus()->getGameOptions()->ghost != 0) {
                        menuArgs.setYes(1);                   /* taken without asking */
                        startPage = NULL;
                    } else {
                        menuArgs.setYes(0);
                        startPage = "ghost_dlg";
                    }
                } else if (org->entries->cars[0]->state->st.finished == 0) {
                    /* not finished: the demonstration again, no replay */
                    load_demo_setting(&race, raceName);
                    menuArgs.setCanReplay(0);
                }

                u32 time = car_best_lap(org->entries->cars[0]->state);  /* s6 */
                if (time == RACE_TIME_INVALID)
                    break;
                int medal = license_record_result(org, raceName, &resultLicence, &resultFlags);
                if (medal == LICENSE_MEDAL_NONE)
                    break;
                if (medal < bestMedal)
                    bestMedal = medal;
                if (time > RECORD_TIME_MAX)
                    break;
                int place = getGameStatus()->getLicenseRecords()->insertTime(licence, test, time);   /* s4 */
                if (place < 0)
                    break;
                char name[0x40];                            /* sp+0x3BA50 */
                name_entry(name);
                getGameStatus()->getLicenseRecords()->insertName(licence, test, name, place);
                {
                    /* make_ranking_code takes a GarageCar: one of the test
                     * car's code (its inline destructor is expanded at the
                     * end of the block) */
                    GarageCar codeCar;                      /* sp+0x3BA90 */
                    codeCar.loadCar(car.carCode);
                    RankingCode code;                       /* sp+0x3BCC0 */
                    make_ranking_code(code.s, time, &codeCar, courseCode, RANKING_KIND_LICENSE, name);
                    getGameStatus()->getLicenseRecords()->insertCode(licence, test, &code, place);
                }
                break;
            }

            case QM_REPLAY:
                race.virtual_41(RACE_REPLAY, 1);
                break;

            case QM_LOAD_GHOST:
                race_memcard(gt2k_menu::MEMCARD_GHOST_LOAD, courseCode, carCode, raceName, org);
                break;

            case QM_SAVE_GHOST:
                race_memcard(gt2k_menu::MEMCARD_GHOST_SAVE, courseCode, carCode, raceName, org);
                break;

            case QM_LOGGER:
                logger_run(org, 1);
                break;

            case QM_OPTION:
                F_0013FE58();
                break;

            case QM_RECORDS: {
                /* (its inline constructor: the vptr, test, licence) */
                gt2k_menu::RecordsMenuArgLicense recordsArg(licence, test);  /* sp+0x3BA50 */
                run_records_menu(&recordsArg);
                break;
            }

            case QM_DEMO: {
                /* the demonstration, car 0's transmission kept over it */
                int transmission = org->entries->cars[0]->transmission;    /* s0 */
                org->demonstration = 1;
                load_demo_setting(&race, raceName);
                race.virtual_41(RACE_REPLAY, 1);
                org->entries->cars[0]->transmission = transmission;
                menuArgs.setCanReplay(0);
                menuArgs.setGhostAsk(0);
                menuArgs.setYes(0);
                org->demonstration = 0;
                break;
            }

            case QM_EXIT:
                loop = 0;
                break;
            }
        } while (loop != 0);

        delete org;                                         /* virtual destructor, slot 0 (vptr +0x8C0) */
    }                                                       /* ~RaceLicense, ~CarEquipments,
                                                               ~RaceMachineTestInformation */
    race_end();
    if (resultFlags != 0 && (resultFlags & LICENSE_FLAG_PASSED) != 0)
        license_get_screen(resultLicence);
    if (getGameStatus()->getLicenseRecords()->isAllGold(licence) != 0 && bestMedal == LICENSE_MEDAL_GOLD)
        F_0013F508(raceName, -1);
    return 0;
}

/* Modes 3 and 4 of main() (runMode 2): one GT mode race,
 * GameStatus::menu.event, with the player's car.  The race is a RaceArcade
 * built in alloca'd memory (with placement new: a free session destroys
 * it, runs a RaceFreePractice on the same RaceOrganization and builds it
 * again) under qm_gt_event.mbl, the start order by power
 * (race_order_by_power); the dirt events have no free session.  "go race":
 * GameStatus::battleNum + 1, the race; a finished race pays the prize
 * money of the place (trophy_prize_screen: with 6 cars or more any place,
 * the consolation screen off the podium; with fewer only a win), records
 * the result (event_record_result) and counts a win; then the replay, and
 * the menu becomes qm_gt_result.mbl without the race behind it.  A win in
 * the last race gives the race's prize car (F_0013F508) at the end.  Uses
 * $fp as the frame pointer (alloca): information fp+0x0, the menu
 * arguments fp+0x250, params fp+0x3E0, the free run fp+0x3F0, then gcar,
 * &car, carCode, courseCode, distance, loop (fp+0x34030 ..).  Returns 0. */
// FUNC 0x00132BA0 run_gtmode_race
int run_gtmode_race(void)
{
    race_begin(CONTROL_RECORD_SIZE, CONTROL_RECORDS);
    const char *raceName = getGameStatus()->getMenuStatus()->event;     /* s6 */
    GarageCar *gcar = getGameStatus()->getMenuStatus()->raceCar[0];     /* fp+0x34030 */
    RaceGTmodeInformation info;                             /* fp+0x00000 */
    select_race(raceName);
    int rank = -1;                                          /* s7 the last race's place; -1 none */
    CarInformation &car = *gcar;                            /* fp+0x34034 GarageCar +0x10 (a reference) */
    info.setCar(car);
    info.vibration = getGameStatus()->getGameOptions()->vibration1p;
    info.transmission = getGameStatus()->getGameOptions()->gearAuto ^ 1;
    info.setRaceCode(nameToCode(raceName));
    pc_course_career(&info);                                /* PC: the course randomizer's draw (forcer first) */
    RaceOrganization *org = race_organization_create(&info);    /* s2 */

    ShortMenuArgs menuArgs;                                 /* fp+0x00250: ShortMenuResultEntry() x 6,
                                                               ShortMenuRankEntry() x 12 */
    /* implicit: ShortMenuResultEntry::clear, ShortMenuRankEntry::clear (the entry
       constructors of ShortMenuArgs' arrays call them) */
    menuArgs.setRaceInfo(&info);
    if (org->raceDB->getCourseKind(org->raceDB->course) == COURSE_KIND_DIRT)
        menuArgs.setNoFreeSession(1);
    u64 carCode = gcar->carCode;                            /* fp+0x34038 */
    u64 courseCode = info.getCourseCode();                  /* fp+0x34040 (direct: RaceInformation's) */

    ShortMenuParams params = gtMenus[GT_MENU_EVENT];                   /* fp+0x003E0 */
    RaceArcade *race = new (__builtin_alloca(sizeof(RaceArcade))) RaceArcade;  /* s3 */
    race->org = org;
    race_order_by_power(org);
    RacePS2Base *shown = race;                              /* s5 the race behind the menu */
    int distance = 0;                                       /* fp+0x34048 driven in the garage car */
    result_list_by_order(org, &menuArgs, 1);
    int loop = 1;                                           /* fp+0x3404C */
    menuArgs.laps = pc_race_laps(org, info.db.event->numberOfLaps);    /* PC: the laps run (pc_course_pick.h) */

    do {
        org->backgroundMode = 1;
        race->virtual_8(RACE_DEMO);
        /* PC (2026-11-20): nor on a course without a pit lane (the free
         * session starts the player's car in the pit, pit_slot_pose): a
         * mod's course the randomizer or the forcer drew (GT3's courses
         * without one are its dirt courses, caught above) */
        if (race->dc->hasPitLane() == 0)
            menuArgs.setNoFreeSession(1);
        int action = run_short_menu(&params, &menuArgs, shown, NULL, NULL);   /* s0 */
        race->virtual_35();
        org->backgroundMode = 0;

        switch (action) {                                   /* jump table 0x00315FC0 */
        case QM_GO_RACE: {
            rank = -1;
            getGameStatus()->incBattleNum();
            race_option_from_game_options(&org->option);
            org->setEntrySteeringAssist(getGameStatus()->getGameOptions()->steeringAssist1p, 0);
            org->setEntryVibration(getGameStatus()->getGameOptions()->vibration1p, 0);
            org->setEntryTransmission(getGameStatus()->getGameOptions()->gearAuto ^ 1, 0);
            race_set_entry_condition(gcar, org, 0);
            race->virtual_41(RACE_LIVE, 1);
            int driven = race_get_entry_condition(gcar, org, 0);
            menuArgs.setCanReplay(1);
            distance += driven;
            if (org->entries->cars[0]->state->st.finished != 0) {
                org->entries->sortByRank();
                rank = race_entry_rank(org, 0);
                int prize = org->raceDB->getPrize(rank);
                int trophyType = org->raceDB->event->trophyType;
                if (org->entries->count >= PRIZE_ALL_PLACES_MIN_CARS) {
                    if (rank < PODIUM_PLACES)
                        trophy_prize_screen(prize, rank, trophyType, 0);
                    else
                        trophy_prize_screen(prize, rank, trophyType, 1);
                } else if (rank == 0)
                    trophy_prize_screen(prize, 0, trophyType, 0);
                event_record_result(rank, raceName);
                if (rank == 0)
                    getGameStatus()->incWinnerNum();
                result_list_by_rank(org, &menuArgs, 1);
            } else {
                menuArgs.clearList();                       /* not finished: every line empty */
            }
            race->virtual_41(RACE_REPLAY, 1);
            params = gtMenus[GT_MENU_RESULT];
            shown = NULL;                                   /* the result menu has no race behind it */
            break;
        }

        case QM_FREE_RUN:
            race->~RaceArcade();                            /* virtual (slot 0, flags 2): the memory stays */
            {
                RaceFreePractice freeRun;                   /* fp+0x003F0 */
                freeRun.org = org;
                race_option_from_game_options(&org->option);
                org->setEntryVibration(getGameStatus()->getGameOptions()->vibration1p, 0);
                org->setEntryTransmission(getGameStatus()->getGameOptions()->gearAuto ^ 1, 0);
                org->backgroundMode = 0;
                freeRun.virtual_41(RACE_LIVE, 1);           /* direct: RacePS2Base's */
                race_order_by_time(org);
                result_list_by_order(org, &menuArgs, 0);
            }                                               /* ~RaceFreePractice */
            race = new (race) RaceArcade;
            race->org = org;
            break;

        case QM_SETTING:
            if (setting_menu(courseCode, org) != 0) {
                org->setEntryInfo(0, &car);
                org->unloadModels(0);
            }
            break;

        case QM_OPTION:
            F_0013FE58();
            break;

        case QM_REPLAY:
            race->virtual_41(RACE_REPLAY, 1);
            break;

        case QM_SAVE_REPLAY:
            race_memcard(gt2k_menu::MEMCARD_REPLAY_SAVE, courseCode, carCode, raceName, org);
            break;

        case QM_EXIT:
            loop = 0;
            break;
        }
    } while (loop != 0);

    if (race != NULL)
        race->~RaceArcade();                                /* virtual (slot 0, flags 2) */
    delete org;                                             /* virtual destructor, slot 0 (vptr +0x8C0) */
    getGameStatus()->incDistance(distance);
    race_end();
    if (rank == 0)
        F_0013F508(raceName, 0);
    return 0;
}                                                           /* ~RaceGTmodeInformation (after race_end) */

/* Mode 2 of main() (runMode 2; main_championship passes cont = 1 after a
 * memory card load that left a championship running): a championship.
 * cont == 0 starts GameStatus::menu.event: an organization is built and
 * dropped at once, for its side effect (the first session's
 * setupOrganization draws the opponents, selectEnemies); cont != 0 resumes
 * the suspended one (resume) at its current session, with its opponents.
 * The race is a RaceChampionship in alloca'd memory under
 * qm_gt_event_champ.mbl (qm_gt_result_champ.mbl when resumed); the race
 * cars get the championship points so far (slot 9) before every race.
 * After a race: the prize money, the session's points, and
 * qm_gt_result_champ.mbl; after the last session `noExit' is set, and "next
 * session" then ends the loop.  "save game" suspends the championship into
 * the GameStatus around the save.  At the end, a won championship (the
 * player leads the points after the last session) pays its bonus (the race
 * record's bonus x 100, x 100 again in Japan), shows the cup (trophy_get),
 * records the championship as won and gives its prize car.  Uses $fp
 * (alloca): the menu arguments fp+0x0, information fp+0x190, params
 * fp+0x13F0, the free run fp+0x1400, then carCode, distance, loop, &car
 * (fp+0x35048 ..).  Returns 0. */
// FUNC 0x00133388 run_championship
int run_championship(int cont)
{
    race_begin(CONTROL_RECORD_SIZE, CONTROL_RECORDS);
    GarageCar *gcar = getGameStatus()->getMenuStatus()->raceCar[0];     /* s7 */
    ShortMenuArgs menuArgs;                                 /* fp+0x00000: ShortMenuResultEntry() x 6,
                                                               ShortMenuRankEntry() x 12 */
    /* implicit: ShortMenuResultEntry::clear, ShortMenuRankEntry::clear (the entry
       constructors of ShortMenuArgs' arrays call them) */
    int bonus;                                              /* s0 the championship's prize money */
    int won;                                                /* s1 */
    const char *champCode;                                  /* s2 */
    {
        RaceChampionshipInformation info;                   /* fp+0x00190 (s5) */
        info.setPlayerCar(*gcar);
        RaceOrganization *org;                              /* s3 (also the dropped one below) */
        if (cont == 0) {
            const char *raceName = getGameStatus()->getMenuStatus()->event;
            select_race(raceName);
            info.setChampionship(raceName, 0);
            pc_course_career(&info);                        /* PC: session 1's course (randomizer / forcer) */
            org = race_organization_create(&info);          /* draws the opponents (see above) */
            delete org;
        } else {
            info.resume();
            select_race(info.currentRaceCode);
            info.setChampionship(info.currentRaceCode, 1);
            info.selectEnemies = 1;                         /* the resumed opponents are kept */
            pc_course_career(&info);                        /* PC: the session's course (randomizer / forcer) */
        }
        info.transmission = getGameStatus()->getGameOptions()->gearAuto ^ 1;
        info.vibration = getGameStatus()->getGameOptions()->vibration1p;
        u64 carCode = gcar->carCode;                        /* fp+0x35048 */
        u64 courseCode = info.getCourseCode();              /* s6 (direct: RaceInformation's) */

        RaceChampionship *race = new (__builtin_alloca(sizeof(RaceChampionship))) RaceChampionship; /* s4 */
        RacePS2Base *shown = race;                          /* s2 the race behind the menu */
        org = race_organization_create(&info);
        race->org = org;
        info.orderByPowerWeight(org);
        ShortMenuParams params;                             /* fp+0x013F0 */
        if (cont == 0)
            params = gtMenus[GT_MENU_CHAMP_EVENT];
        else
            params = gtMenus[GT_MENU_CHAMP_RESULT];
        int distance = 0;                                   /* fp+0x35050 driven in the garage car */
        menuArgs.session = info.session;
        result_list_from_entries(org, &menuArgs, &info);
        int loop = 1;                                       /* fp+0x35054 */

        do {
            menuArgs.laps = pc_race_laps(org, info.db.event->numberOfLaps);    /* PC: the laps run (pc_course_pick.h) */
            org->backgroundMode = 1;
            race->virtual_8(RACE_DEMO);
            /* PC (2026-11-20): no free session on a dirt course (GT3's rule
             * for its rally events, run_gtmode_race above) nor on a course
             * without a pit lane (lines 3 and 4; the conductor read them for
             * the race just started): the free session starts the player's
             * car in the pit (DynamicsConductorFreePractice start type 6,
             * pit_slot_pose).  GT3's championships never ran on dirt; the
             * course randomizer's extended setting (and the forcer) can draw
             * a rally course for a session, whose free session crashed (user
             * report).  Each session: its own course. */
            menuArgs.setNoFreeSession(org->raceDB->getCourseKind(org->raceDB->course) == COURSE_KIND_DIRT ||
                                      race->dc->hasPitLane() == 0);
            int action = run_short_menu(&params, &menuArgs, shown, NULL, NULL);   /* s0 */
            race->virtual_35();
            org->backgroundMode = 0;

            switch (action) {                               /* jump table 0x00315FF0 */
            case QM_GO_RACE: {
                getGameStatus()->incBattleNum();
                {
                    /* the garage car's CarInformation, converted at each use
                     * (GCSE keeps one copy, spilled to fp+0x35058) */
                    CarInformation &car = *gcar;
                    org->setEntryInfo(0, &car);
                }
                RaceEntryBase *entries = org->entries;      /* s2 */
                for (int i = 0; i < entries->count; i++) {
                    RaceEntryCar *entryCar = entries->cars[i];  /* s1, read before the call */
                    entryCar->points = info.virtual_9(i);   /* direct: the points so far */
                }
                race_option_from_game_options(&org->option);
                org->setEntrySteeringAssist(getGameStatus()->getGameOptions()->steeringAssist1p, 0);
                org->setEntryVibration(getGameStatus()->getGameOptions()->vibration1p, 0);
                org->setEntryTransmission(getGameStatus()->getGameOptions()->gearAuto ^ 1, 0);
                race_set_entry_condition(gcar, org, 0);
                race->virtual_41(RACE_LIVE, 1);
                int driven = race_get_entry_condition(gcar, org, 0);
                menuArgs.canReplay = 1;
                distance += driven;
                if (org->entries->cars[0]->state->st.finished != 0) {
                    int place = race_entry_rank(org, 0);    /* s0 */
                    int prize = org->raceDB->getPrize(place);
                    int trophyType = org->raceDB->event->trophyType;
                    if (place < PODIUM_PLACES)
                        trophy_prize_screen(prize, place, trophyType, 0);
                    else
                        trophy_prize_screen(prize, place, trophyType, 1);
                    event_record_result(place, info.currentRaceCode);
                    if (place == 0)
                        getGameStatus()->incWinnerNum();
                    info.addPoints(org);
                    result_list_by_rank(org, &menuArgs, 1);
                } else {
                    info.addEnemyPoints(org);               /* not finished: the others' points only */
                }
                race->virtual_41(RACE_REPLAY, 1);
                if (info.session >= info.sessions)          /* isLastSession(), expanded */
                    menuArgs.noExit = 1;                    /* the last session was run */
                params = gtMenus[GT_MENU_CHAMP_RESULT];
                shown = NULL;                               /* the result menu has no race behind it */
                break;
            }

            case QM_NEXT_SESSION:
                if (menuArgs.noExit != 0) {
                    loop = 0;                               /* the championship is over */
                } else {
                    race->~RaceChampionship();              /* virtual (slot 0, flags 2) */
                    info.nextSession();
                    pc_course_career(&info);                /* PC: the session's course (randomizer / forcer) */
                    courseCode = info.getCourseCode();
                    delete org;
                    org = race_organization_create(&info);
                    info.orderByPowerWeight(org);
                    race = new (race) RaceChampionship;
                    race->org = org;
                    menuArgs.session = info.session;
                    result_list_from_entries(org, &menuArgs, &info);
                    params = gtMenus[GT_MENU_CHAMP_EVENT];
                    shown = race;
                }
                break;

            case QM_REPLAY:
                race->virtual_41(RACE_REPLAY, 1);
                break;

            case QM_FREE_RUN:
                race->~RaceChampionship();                  /* virtual (slot 0, flags 2): the memory stays */
                info.setPlayerCar(*gcar);
                {
                    RaceFreePractice freeRun;               /* fp+0x01400 */
                    freeRun.org = org;
                    race_option_from_game_options(&org->option);
                    org->setEntryVibration(getGameStatus()->getGameOptions()->vibration1p, 0);
                    org->setEntryTransmission(getGameStatus()->getGameOptions()->gearAuto ^ 1, 0);
                    race_set_entry_condition(gcar, org, 0);
                    freeRun.virtual_41(RACE_LIVE, 1);       /* direct: RacePS2Base's */
                    distance += race_get_entry_condition(gcar, org, 0);
                    if (car_best_lap(org->entries->cars[0]->state) != RACE_TIME_INVALID)
                        info.orderByTime(org);              /* a lap: the grid by best lap */
                }                                           /* ~RaceFreePractice */
                race = new (race) RaceChampionship;
                race->org = org;
                result_list_from_entries(org, &menuArgs, &info);
                break;

            case QM_SETTING:
                if (setting_menu(courseCode, org) != 0) {
                    CarInformation &car = *gcar;
                    org->setEntryInfo(0, &car);
                    org->unloadModels(0);
                }
                break;

            case QM_SAVE_REPLAY:
                race_memcard(gt2k_menu::MEMCARD_REPLAY_SAVE, courseCode, carCode, info.currentRaceCode, org);
                break;

            case QM_OPTION:
                F_0013FE58();
                break;

            case QM_SAVE_GAME:
                info.suspend();
                race_memcard(gt2k_menu::MEMCARD_GAME_SAVE, courseCode, carCode, info.currentRaceCode, org);
                info.clearSuspend();
                break;

            case QM_EXIT:
                loop = 0;
                break;
            }
        } while (loop != 0);

        if (race != NULL)
            race->~RaceChampionship();                      /* virtual (slot 0, flags 2) */
        delete org;                                         /* virtual destructor, slot 0 (vptr +0x8C0) */
        getGameStatus()->incDistance(distance);
        bonus = info.db.event->bonus * 100;                 /* RaceDataBase::F_00238BC0, expanded */
        if (info.db.isJapan() != 0)
            bonus *= 100;                                   /* the prices of Japan */
        champCode = info.globalRaceCode;
        won = info.isPlayerLeading();
    }                                                       /* ~RaceChampionshipInformation */
    race_end();
    /* champCode points into `info', whose destructor has run (the stack
     * memory is untouched; as in the original) */
    if (menuArgs.noExit != 0 && won != 0) {
        trophy_get(bonus, TROPHY_RANDOM);
        event_record_result(0, champCode);
        F_0013F508(champCode, 0);
    }
    return 0;
}

/* ======================================================================== */
/* T053's out-of-line copies of inline functions (unreferenced: the calls   */
/* and the vtables use the copies the linker kept)                          */
/* ======================================================================== */

// FUNC 0x00133DC8 ShortMenuRankEntry::clear
/* ShortMenuRankEntry's inline clear() (its constructor calls it), game/
 * T052_arcade_race_modes.h; egcs emits this copy (T053's):
 * the empty line of the rank list (the calls go to T052's copy
 * 0x001313D0). */

// FUNC 0x00133DE8 ShortMenuResultEntry::clear
/* ShortMenuResultEntry's inline clear() (its constructor calls it), game/
 * T102_race_menu_helpers.h; egcs emits this copy (T053's):
 * the empty line of the result list (the calls go to T102's copy
 * 0x0012F820; pad14 is not written). */

// FUNC 0x00133E08 gt2k_menu::Cell<GarageCar>::~Cell
/* inline in game/menu/framework/menu_list.h; egcs emits this copy (T053's; T036's is
 * 0x00125A10): the vptr (+0x8) = 0x00311C50, then
 * gt2k_menu::pCell::~pCell(this, flags), which does the delete. */
/* implicit: ~pCell (base destructor) */


// FUNC 0x00133E90 GarageCar::~GarageCar
/* inline in game/menu/gtmode/T036_garage.h; egcs emits this copy (T053's; T036's is
 * 0x00125A98): the vptrs +0x1EC = 0x00311C20 and +0x8 = 0x00311C30,
 * CarInformation::~CarInformation(this + 0x10, 0), then the inline
 * Cell<GarageCar> destructor (vptr 0x00311C50,
 * gt2k_menu::pCell::~pCell(this, flags), which does the delete). */
/* implicit: ~CarInformation, ~pCell (base destructors) */

