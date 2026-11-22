/* T124_race_information.h - RaceInformation (translation unit T124, unit
 * u042), 0x00179B78..0x0017A038.
 *
 * The description of a race to set up, which race_organization_create (T123)
 * turns into a RaceOrganization: the race database (a RaceDataBase member),
 * the option block, the race code, the number of entries, the start
 * countdown and the time limit.  The mode classes (RaceSinglePlayer-
 * Information T133, RaceBattleInformationT<n>, RaceSolitaireInformation
 * T139, ...) override createEntries and the set-up slots.  assignCars gives
 * entries random cars of the race's car list without repeating one, with
 * the generator g_00351DE0.
 *
 * Its __user_type_info storage (__ti15RaceInformation, 0x00353378) is
 * declared at the end (the plan had put it in T121_race_entry.h).
 */
#ifndef GAME_T124_RACE_INFORMATION_H
#define GAME_T124_RACE_INFORMATION_H

#include "game/game.h"
#include "game/data/T484_race_database.h"        /* RaceDataBase (held by value) */
#include "game/menu/race_menu/T115_race_option.h"          /* RaceOption (held by value) */

class type_info;
class RaceEntryBase;                        /* T121 (pointers only: forward declarations) */
class RaceEntryCar;                         /* T121 */
class RaceOrganization;                     /* T125 */
class Random;                               /* T531 */

/* The typeinfo function (vtable entry 0), declared before the class, for
 * GCC 14 only: egcs generates it itself (a declaration it sees stops it
 * emitting the function; u042 matching). */

/* vtable 0x00327478 (15 slots).  typeinfo __tf15RaceInformation 0x00179FF8.
 * vptr +0x54 (after the data; the RaceDataBase member has its own at +0xC).
 * Size 0x58 (the u64 members align it to 8). */
class RaceInformation {
public:
    RaceInformation();                                  /* 0x00179B78 raceCode, entryCount, countDown, timeLimit 0 */
    virtual ~RaceInformation();                         /* [ 0] +0x0C 0x00179BF0 */
    virtual u64  getRaceCode();                         /* [ 1] +0x14 0x00179C90 */
    virtual u64  getCourseCode();                       /* [ 2] +0x1C 0x00179C98 db.getCourse() */
    virtual void setCourseCode(u64 code);               /* [ 3] +0x24 0x00179CB8 db.setCourse(code) */
    virtual RaceEntryBase *createEntries();             /* [ 4] +0x2C 0x00179CD8 NULL: the modes create them */
    virtual int  getCountDown();                        /* [ 5] +0x34 0x00179D00 */
    virtual int  getTimeLimit();                        /* [ 6] +0x3C 0x00179D28 */
    virtual void setEntryCount(int n);                  /* [ 7] +0x44 0x00179D30 */
    virtual int  getEntryCount();                       /* [ 8] +0x4C 0x00179D38 */
    /* 0 here; RaceChampionshipInformation (T151) returns entry i's points
     * (u042 declared it without the int: h25). */
    virtual int  virtual_9(int i);                      /* [ 9] +0x54 0x00179FC0 */
    /* For every entry race_organization_create adds; nothing here. */
    virtual void setupEntry(RaceEntryCar *car);         /* [10] +0x5C 0x00179D40 */
    virtual void setupOrganization(RaceOrganization *org);  /* [11] +0x64 0x00179D48 org->resetEntryOrder() */
    /* Entries entries[0..n) get random cars of the race's car list, each
     * once while unused ones are left (db_car_is_locked ones count as used;
     * quirk: the count of unused ones starts at all the cars, so with a
     * locked car the search can loop forever); then race_pick_random_colors
     * (*org, &g_00351DE0) (T193, the colours). */
    virtual void assignCars(RaceOrganization *org, int *entries, int n);    /* [12] +0x6C 0x00179D70 */
    virtual void assignCarRange(RaceOrganization *org, int from, int to);   /* [13] +0x74 0x00179F28 */
    /* Inline (no callers anywhere; T124 has the key function, so egcs writes
     * the copies there, after the static initialiser of g_00351DE0, in the
     * reverse order of these bodies: getRaceDB 0x00179FD8 .. setCountDown
     * 0x00179FF0). */
    void setCountDown(int v) { countDown = v; }         /* 0x00179FF0 */
    void setTimeLimit(int v) { timeLimit = v; }         /* 0x00179FE8 */
    RaceOption *getRaceOption() { return &option; }     /* 0x00179FE0 (Lupo 2 names from here) */
    virtual RaceDataBase *getRaceDB() { return &db; }   /* [14] +0x7C 0x00179FD8 */

    /* raceCode = code, db.select(code); entryCount capped at the race's
     * entry count (RaceEvent::entryCount).  13 callers. */
    void setRaceCode(u64 code);                         /* 0x00179C48 */
    /* countDown < 0: 0 (none), 0: def, else countDown (no callers). */
    int  getCountDownOr(int def);                       /* 0x00179CE0 */
    int  getTimeLimitOr(int def);                       /* 0x00179D08 the same for timeLimit */
    static void setRandomSeed(int seed);                /* 0x00179D68 g_00351DE0 = seed */

    RaceDataBase db;                        /* 0x00 (u049: db[0x28]) */
    RaceOption   option;                    /* 0x28 */
    u64          raceCode;                  /* 0x40 */
    int          entryCount;                /* 0x48 (u017 maxCars) */
    int          countDown;                 /* 0x4C seconds; < 0 none, 0 the default */
    int          timeLimit;                 /* 0x50 seconds; < 0 none, 0 the default */
    /*  vptr                                   0x54 */
    /* PC: 0x5C..0x60 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x60, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_5C, 0x4)
};
EGCS_SIZE(RaceInformation, 0x58);

/* The generator state assignCars draws with (a Random, 0x87654321 after
 * T124's static initialiser; T124 defines it, .sdata).  Random is only
 * forward-declared here: a unit that uses g_00351DE0 includes
 * T531_random.h first, or egcs addresses it with %hi / %lo instead of $gp
 * (u042). */
extern Random g_00351DE0;                   /* 0x00351DE0 gp-0x5C10 */
/* 0x00179FC8 (.ctors 0x0028F0E8): egcs's static initialiser of g_00351DE0
 * (T124), _GLOBAL_$I$__15RaceInformation (seed = 0x87654321); its FUNC
 * marker names it race_info_static_ctor. */


#endif /* GAME_T124_RACE_INFORMATION_H */
