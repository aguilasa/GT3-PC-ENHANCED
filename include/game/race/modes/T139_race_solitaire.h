/* T139_race_solitaire.h - RaceSolitaireEntry, RaceSolitaireInformation,
 * RaceSolitaire (translation unit T139, unit u047), 0x00186968..0x00187F98.
 *
 * RaceSolitaire is the one-player race base of RaceTimeTrial (T140),
 * RaceFreeRun (T142), RaceLicense (T144) and RaceMachineTest (T148);
 * registered itself as "RaceSolitaire" (g_002B8180).  It has four inputs:
 * input0 is the pad (slot 0x14 of the pad manager) and records the player's
 * controls (two more control records, the best lap's and the current lap's,
 * hang off it); input1 is a second car (? the ghost) played from a record;
 * input2 / input3 play the two cars of the data logger replay
 * (RaceOrganization::loggerMode) out of the replay memory: three
 * 0xA8000-byte blocks of the block pool 0x002C2F50 (the lowest of them
 * starts a 0x1F8000-byte RaceMemPool).  In logger mode the race is drawn as
 * two side-by-side views (RaceSplitDisplay).  The RaceInfoBlock slots (7,
 * 11, 13, 14, 17, 19..21, 58, 59) save and load the race description and,
 * after it (+0x191C), the control records.
 *
 * RaceSolitaireInformation describes a one-car race (base of
 * RaceFreeRunInformation, T142); RaceSolitaireEntry is its entry list, which
 * can be switched to show only the first car.
 *
 * __ti13RaceSolitaire (0x004B1FE0) and __ti24RaceSolitaireInformation
 * (0x004B1FC0) are declared at the end (the plan had put them in
 * T138_race_link_battle.h).
 */
#ifndef GAME_T139_RACE_SOLITAIRE_H
#define GAME_T139_RACE_SOLITAIRE_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/race/core/T132_race_basic.h"           /* base RaceBasic */
#include "game/race/core/T130_race_ps2_base.h"        /* base RaceEntryPS2 */
#include "game/race/core/T124_race_information.h"     /* base RaceInformation */
#include "game/app/T000_main.h"                 /* RaceEntry (held by value) */
#include "game/race/core/T413_race_input.h"           /* RaceInput (held by value) */
#include "game/race/display/T155_race_split_display.h"   /* RaceSplitDisplay (held by value) */
#include "game/race/dynamics/T318_control_record.h"       /* AutomobileControlRecord::Manager (held by value) */
#include "game/race/core/T174_race_mem_pool.h"        /* RaceMemPool (held by value) */

class GSBuffer;                             /* pointers only: forward declarations */
class RaceBase;                             /* T129 */
class RaceEntryBase;                        /* T121 */
class RaceEntryCar;                         /* T121 */
class RaceOrganization;                     /* T125 */
struct RaceInfoBlock;                       /* T129 */
class CarEquipments;                        /* T482 (a by-value parameter) */
class type_info;

/* The typeinfo functions, declared before the classes (see T132): for GCC 14
 * only; egcs declares and generates them itself (u047 matching: guarded, all
 * three compile exact). */

/* vtable 0x0032AFD0 (5 slots: [0] [4] its own, [1..3] RaceEntryBase's).
 * typeinfo __tf18RaceSolitaireEntry 0x00187F48 (si, base RaceEntryPS2).
 * vptr +0x1C (RaceEntryBase's).  Size 0x60 (operator delete(p, 0x60) in
 * RaceSolitaireInformation::createEntries). */
class RaceSolitaireEntry : public RaceEntryPS2 {
public:
    static void *operator new(size_t size);                 /* 0x00186968 the race pool */
    static void  operator delete(void *p, size_t size);     /* 0x00186990 */
    RaceSolitaireEntry();                   /* 0x001869B8 the two counts cleared */
    virtual ~RaceSolitaireEntry();          /* [0] +0x0C 0x001869F8 */
    /* on: the list shows only the first car (cars -> soloSlots, count 1 if
     * there is a car; the real count is kept); off: the full list again. */
    virtual void virtual_4(int on);         /* [4] +0x2C 0x00186A50 */

    RaceEntryCar *soloSlots[7];             /* 0x3C RaceEntryBase::cars points here while on */
    int           savedCount;               /* 0x58 the count while on */
    int           soloCount;                /* 0x5C the count shown while on (0 or 1) */
};
EGCS_SIZE(RaceSolitaireEntry, 0x60);

/* vtable 0x0032AF50 (15 slots: [0] [4] [8] [10] [11] its own).  typeinfo
 * __tf24RaceSolitaireInformation 0x00187EF8 (si, base RaceInformation).
 * vptr +0x54.  Size 0x258 (RaceFreeRunInformation's data at +0x258). */
class RaceSolitaireInformation : public RaceInformation {
public:
    /* The entry cleared (no car, colour -1).  Callers: F_00130A50,
     * RaceFreeRunInformation's constructor (F_00188FE0). */
    RaceSolitaireInformation();                         /* 0x00186AA8 */
    virtual ~RaceSolitaireInformation();                /* [ 0] +0x0C 0x00186B38 */
    virtual RaceEntryBase *createEntries();             /* [ 4] +0x2C 0x00186B88 new RaceSolitaireEntry */
    virtual int  getEntryCount();                       /* [ 8] +0x4C 0x00186BE0 1 */
    /* The entry's equipments, inputId 0x100 for car 0 (1 otherwise), the
     * entry's two values when a car is set; start order 0. */
    virtual void setupEntry(RaceEntryCar *car);         /* [10] +0x5C 0x00186BE8 */
    virtual void setupOrganization(RaceOrganization *org);  /* [11] +0x64 0x00186C68 empty */

    void setCar(s64 code);                  /* 0x00186C70 entry.car, equip.setCar, presetCar 1 (unreferenced) */
    void setColor(int color);               /* 0x00186CA8 entry.color, equip.setColorIndex (unreferenced) */
    void setEquipments(CarEquipments e);    /* 0x00186CD0 ? name: by value (egcs destroys e here) */
    /* Inline (u047 matching): their copies have no callers and follow
     * RaceSolitaire's at the end of the unit.  Declared in reverse address
     * order: egcs emits a class's inline copies in reverse declaration order. */
    void setTransmission(int v) { entry.transmission = v; } /* 0x00187EF0 (u047 setF0C) */
    void setTyre(int v) { entry.tyre = v; }                 /* 0x00187EE8 (u047 setF10) */
    void setVibration(int v) { entry.vibration = v; }       /* 0x00187EE0 (u047 setF14) */

    RaceEntry entry;                        /* 0x058 the one car of the race (u047: RaceCarEntry) */
};
EGCS_SIZE(RaceSolitaireInformation, 0x258);

/* vtable 0x0032AD68 (60 slots: [56..59] new; [0] [4] [8] [9] [11] [13]
 * [14] [15] [17] [20] [21] [22] [23] [24] [25] [26] [28] [35] [45] its own,
 * [30] RaceBasic's).  typeinfo __tf13RaceSolitaire 0x00187E90 (si, base
 * RaceBasic).  vptr +0.  Size 0x21800 (race_solitaire_create; the data of
 * RaceTimeTrial start at +0x21800). */
class RaceSolitaire : public RaceBasic {
public:
    static void *operator new(size_t size);                 /* 0x00186D28 the race pool */
    static void  operator delete(void *p, size_t size);     /* 0x00186D50 */
    /* input0 to pad slot 0x14, recording into record slot 0; the lap
     * records (slot 1, frames 1 / 2) hung off it; input1 plays record slot
     * 1; inputs 1..3 inactive; then the replay memory (virtual_57). */
    RaceSolitaire();                                        /* 0x00186E10 */
    virtual ~RaceSolitaire();                               /* [ 0] +0x0C  0x00187120 gives the replay blocks back */
    virtual RaceMemPool *virtual_4();                       /* [ 4] +0x2C  0x001872A8 &pool */
    /* The replay memory, RaceBasic's; in a race both records rewound; the
     * split display set up for "RACE_MODE_LOGGER"; no time step. */
    virtual void virtual_8(int replay);                     /* [ 8] +0x4C  0x00187598 */
    virtual void virtual_9();                               /* [ 9] +0x54  0x00187660 */
    virtual void virtual_11(RaceInfoBlock *blk);            /* [11] +0x64  0x001877E8 RaceBase's, one car */
    virtual void virtual_13(RaceInfoBlock *blk);            /* [13] +0x74  0x00187840 input0 plays the record after blk */
    virtual void virtual_14(RaceInfoBlock *blk);            /* [14] +0x7C  0x00187898 appends input0's record */
    virtual int  virtual_15();                              /* [15] +0x84  0x001878C8 its size */
    virtual void virtual_17(RaceInfoBlock *blk);            /* [17] +0x94  0x001878F0 RaceBase's, one car */
    virtual void virtual_20(RaceInfoBlock *blk);            /* [20] +0xAC  0x00187920 the second input plays the record */
    virtual void virtual_21(RaceInfoBlock *blk);            /* [21] +0xB4  0x00187968 appends input2's record */
    virtual int  virtual_22();                              /* [22] +0xBC  0x001879A8 its size */
    virtual void virtual_23();                              /* [23] +0xC4  0x00187430 input2 takes input0's record */
    virtual const char *virtual_24();                       /* [24] +0xCC  0x00186D78 "RaceSolitaire" */
    virtual RaceInput *virtual_25();                        /* [25] +0xD4  0x00187520 input2 in the logger, else input0 */
    virtual RaceInput *virtual_26(int id);                  /* [26] +0xDC  0x00187548 1: the second input, 0x100 the main */
    /* timeStep (non-zero) replaces dt; SELECT (raw pad bit 0x100) flips
     * showGhost. */
    virtual void virtual_28(float dt);                      /* [28] +0xEC  0x00187A50 */
    virtual void virtual_35();                              /* [35] +0x124 0x00187790 RaceBasic's, the records finished */
    /* Outside the logger RaceBasic's; in the logger the two split views. */
    virtual void virtual_45(GSBuffer *buf);                 /* [45] +0x174 0x00187D30 */
    virtual void virtual_56();                              /* [56] +0x1CC 0x00187760 clears the pool's frames */
    virtual void virtual_57();                              /* [57] +0x1D4 0x001872B8 takes the replay memory (once) */
    virtual void virtual_58(RaceInfoBlock *blk);            /* [58] +0x1DC 0x00187818 input0 plays the record after blk */
    /* which 0: virtual_13(blk), else virtual_19(blk), virtual_20(blk). */
    virtual void virtual_59(int which, RaceInfoBlock *blk); /* [59] +0x1E4 0x001879D0 */

    void seekRecords(int sector);           /* 0x001873B8 car 0 (and 1) back to logger sector `sector' (dc_sector_restart) */
    RaceInput *getMainInput();              /* 0x001874B0 ? name: input2 in the logger (or field_217A8), else input0 */
    RaceInput *getSecondInput();            /* 0x001874E8 ? name: input3 in the logger (or field_217A8), else input1 */
    /* Inline (u047 / u048 matching): the copies have no callers and sit at
     * the end of the unit; RaceFreeRun::virtual_23 (u048) expands
     * getPadInput.  Declared in reverse address order (see
     * RaceSolitaireInformation). */
    void setTimeStep(float t) { timeStep = t; }     /* 0x00187E80 ? name */
    void setShowGhost(int v) { showGhost = v; }     /* 0x00187E70 (u047 setField217A4) */
    int  getShowGhost() { return showGhost; }       /* 0x00187E60 (u047 getField217A4) */
    RaceInput *getPadInput() { return &input0; }    /* 0x00187E50 ? name */
    /* The enhanced engine (race_logic.md M1, ENH_RACE_GHOST_CAR; Lupo 2's
     * virtual_23s, L2 0x00184624 / 0x00186D5C): the first car's equipments,
     * condition and aids when its best lap becomes the ghost.  Callers:
     * virtual_23 (RaceSolitaire's, RaceFreeRun's). */
    void keepGhostCar();

    RaceInput        input0;                /* 0x0C8C0 the pad (slot 0x14 of the pad manager) */
    RaceInput        input1;                /* 0x0CA08 the second car */
    RaceInput        input2;                /* 0x0CB50 the logger's first car */
    RaceInput        input3;                /* 0x0CC98 the logger's second car */
    RaceSplitDisplay splitDisplay;          /* 0x0CDE0 */
    int              firstTransmission;     /* 0x216E0 the first car's transmission (virtual_23; virtual_21 saves it; u047 field_216E0) */
    AutomobileControlRecord::Manager bestLapRecord; /* 0x216E4 input0.bestLap (u047: rec1) */
    AutomobileControlRecord::Manager lapRecord;     /* 0x21744 input0.lap (u047: rec2) */
    int              showGhost;             /* 0x217A4 1; SELECT flips it: car_display_level shows the ghost cars (u078; u047 field_217A4) */
    int              field_217A8;           /* 0x217A8 0 (non-zero: inputs 2 / 3 as in the logger) */
    RaceMemPool      pool;                  /* 0x217AC over the replay memory */
    u8              *replayMem;             /* 0x217E4 three 0xA8000-byte blocks of g_002C2F50 (the lowest;
                                               PC: one malloc of 0x1F8000 bytes) */
    u32              replayMemSize;         /* 0x217E8 0x1F8000 */
    f32              timeStep;              /* 0x217EC overrides virtual_28's dt when non-zero */
    /* The enhanced engine (race_logic.md M1): the car whose best lap became
     * the ghost (keepGhostCar), written into the saved ghost's car slot 0 by
     * virtual_21 - the slot GT3's block already has, so the ghost file's
     * layout and size do not change (Lupo 2 +0x22420..+0x2261C). */
    CarSaveEquipments ghostInfo;            /* 0x217F0 the CarSaveEquipments part of its CarEquipments */
    CarCondition     ghostCondition;        /* 0x219C0 RaceEntryCar::condition */
    int              ghostTransmission;     /* 0x219E0 */
    int              ghostSteeringAssist;   /* 0x219E4 */
    int              ghostAsm;              /* 0x219E8 */
    int              ghostTcs;              /* 0x219EC */
    int              ghostKept;             /* 0x219F0 1 once keepGhostCar has run (virtual_21 writes the car only then) */
    /*  0x219F4 .. 0x219FF: alignment */
    /* PC: 0x35F8C..0x35FC0 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x35FC0, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_35F8C, 0x34)
};
/* The enhanced engine: the split display 8 longer (the members after
 * splitDisplay sit 8 later than shown) and the ghost car block, 0x200
 * (GT3 0x21800). */
/* (Wave 1: the hud's split display (two RaceDisplays: 0x151D8, GT3 0x14900) and the embedded race classes' growth; the size
 * from tools/sizeprobe.py, the member offsets above stay
 * GT3's.) */
EGCS_SIZE(RaceSolitaire, 0x22300);

/* RaceClassInfo "RaceSolitaire" (g_002B8180: size 0x21800, these two). */
RaceBase *race_solitaire_create(void);                      /* 0x00186D88 */
RaceBase *race_solitaire_construct(void *buf, int arg);     /* 0x00186DE8 a RaceSolitaire in buf (NULL: none) */
/* .ctors entry 0x0028F10C: race_class_register(&g_00351E90, &g_002B8180). */
void      race_solitaire_static_init(void);                 /* 0x00187E28 */


#endif /* GAME_T139_RACE_SOLITAIRE_H */
