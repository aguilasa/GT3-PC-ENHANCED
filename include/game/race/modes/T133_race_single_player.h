/* T133_race_single_player.h - RaceSinglePlayerInformation, RaceSinglePlayer
 * (translation unit T133, unit u046), 0x001817F8..0x00182118.
 *
 * RaceSinglePlayerInformation describes a race with one human player: the
 * player's car (code, colour id, equipments and two values that go to the
 * car's +0x20E4 / +0x20E8) in entry slot 0.  It creates a RaceEntryPS2
 * list, gives the player inputId 0x100, fills the other slots with
 * opponents (assignCarRange from 1) and puts the player last on the grid.
 * The number of cars is 6, 4 on courses of kind 1 and 2 on kind 2.
 *
 * RaceSinglePlayer is RaceBasicWithRaceDisplay plus the player's RaceInput
 * (pad slot 0x14, its recorder bound to player 0).  The input's record is
 * what the replay data holds (virtual_13 / 14 / 15 load, save and size it,
 * after the 0x191C-byte RaceInfoBlock); in a race (replay mode 0) a full
 * record posts event 0x16, in a replay (mode 1) its end posts event 1.
 * Each frame it hands the race display the car the current view follows.
 *
 * (RaceDisplayBase, which h24 first declared here, is T154's.)
 */
#ifndef GAME_T133_RACE_SINGLE_PLAYER_H
#define GAME_T133_RACE_SINGLE_PLAYER_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/race/core/T132_race_basic.h"           /* base RaceBasicWithRaceDisplay */
#include "game/race/core/T124_race_information.h"     /* base RaceInformation */
#include "game/app/T000_main.h"                 /* RaceEntry (held by value) */
#include "game/race/core/T413_race_input.h"           /* RaceInput (held by value) */

class RaceBase;                             /* pointers only: forward declarations */
class RaceEntryBase;                        /* T121 */
class RaceEntryCar;                         /* T121 */
class RaceOrganization;                     /* T125 */
struct RaceInfoBlock;                       /* T129 */
class CarEquipments;                        /* T482 (a by-value parameter) */
class type_info;

/* The typeinfo functions, declared before the classes (see T132): GCC 14
 * build only.  egcs generates them itself (and their type names), unless an
 * earlier extern "C" declaration hides them (u046 matching). */

/* vtable 0x00328998 (15 slots: [0] [2] [3] [4] [10] [11] its own, the
 * others RaceInformation's).  typeinfo __tf27RaceSinglePlayerInformation
 * 0x001820C8 (si, base RaceInformation).  vptr +0x54 (RaceInformation's).
 * Size 0x258 (RaceArcadeDemoInformation's data at +0x258). */
class RaceSinglePlayerInformation : public RaceInformation {
public:
    /* 6 cars, an empty player entry (no car, colour -1). */
    RaceSinglePlayerInformation();                      /* 0x001817F8 */
    virtual ~RaceSinglePlayerInformation();             /* [ 0] +0x0C 0x00181890 ~entry.equip, ~RaceInformation */
    virtual u64  getCourseCode();                       /* [ 2] +0x1C 0x001818E0 the race DB's course (as the base) */
    /* The course; the car count from its kind (1: 4 cars, 2: 2 cars). */
    virtual void setCourseCode(u64 code);               /* [ 3] +0x24 0x00181900 */
    virtual RaceEntryBase *createEntries();             /* [ 4] +0x2C 0x00181958 new RaceEntryPS2 */
    /* Car 0 only: the entry's equipments, inputId 0x100 and (when a car is
     * set) the entry's +0x0C / +0x10 to the car's +0x20E4 / +0x20E8. */
    virtual void setupEntry(RaceEntryCar *car);         /* [10] +0x5C 0x001819B0 */
    /* Opponents in slots 1..count (assignCarRange), the grid order reset,
     * the player moved to the last place. */
    virtual void setupOrganization(RaceOrganization *org);  /* [11] +0x64 0x00181A20 */

    /* The player's car (no callers: out-of-line copies of inline setters,
     * except setEquipments, run_arcade_race's).  The names are descriptive
     * (no symbol). */
    void setCar(u64 code);                  /* 0x00181A88 entry.car, equip.setCar(code), equip.presetCar = 1
                                               (whatever setCar returns) */
    void setColor(int color);               /* 0x00181AC0 entry.color, equip.setColorIndex(color): a colour
                                               index of the car */
    void setEquipments(CarEquipments eq);   /* 0x00181AE8 entry.equip = eq; by value (egcs destroys eq here) */
    /* entry.vibration / tyre / transmission (RaceEntry, T000; u046
     * setField14 / 10 / 0C).  Inline: T133 (the key function's file) emits
     * the copies at its end, in the reverse of this order, before
     * __tf27RaceSinglePlayerInformation (retail's frame info, u046's data
     * pass; as RaceSolitaireInformation's, T139). */
    void setTransmission(int v) { entry.transmission = v; } /* 0x001820C0 entry +0x0C (-> the car's transmission
                                                               +0x20E4) */
    void setTyre(int v) { entry.tyre = v; }                 /* 0x001820B8 entry +0x10 (-> the car's tyre +0x20E8) */
    void setVibration(int v) { entry.vibration = v; }       /* 0x001820B0 entry +0x14 */

    RaceEntry entry;                        /* 0x058 the player's car (u001 / u016: entries[1]) */
};
EGCS_SIZE(RaceSinglePlayerInformation, 0x258);

/* vtable 0x003287D0 (56 slots: [0] [8] [13] [14] [15] [24] [25] [26] [28]
 * its own, [30] [35] [45] RaceBasic's).  typeinfo __tf16RaceSinglePlayer
 * 0x00182060 (si, base RaceBasicWithRaceDisplay).  vptr +0.  Size 0x16EC0
 * (RaceSinglePlayer_create; the data of RaceArcade, RaceFreePractice and
 * RaceChampionship start at +0x16EC0). */
class RaceSinglePlayer : public RaceBasicWithRaceDisplay {
public:
    /* (No class placement new: RaceSinglePlayer_construct uses <new>'s
     * ::new, and an inline member would be emitted with the vtable, which
     * retail's T133 has not.) */
    static void *operator new(size_t size);                 /* 0x00181B40 the race pool */
    static void  operator delete(void *p, size_t size);     /* 0x00181B68 */
    /* The input goes to pad slot 0x14 (F_0022EA88), its recorder to
     * player 0. */
    RaceSinglePlayer();                                     /* 0x00181C28 */
    virtual ~RaceSinglePlayer();                            /* [ 0] +0x0C  0x00181CF0 the input leaves the pad manager */
    /* RaceBasic's, then the input takes replayMode (not the argument). */
    virtual void virtual_8(int replay);                     /* [ 8] +0x4C  0x00181E50 */
    /* Loads the input record from right after the block (blk + 0x191C;
     * rec.load(.., 1), its end dropped). */
    virtual void virtual_13(RaceInfoBlock *blk);            /* [13] +0x74  0x00181DC8 */
    /* One recorded input (blk->recordCount = 1); the record appended after
     * the block (rec.save at blk + blk->size). */
    virtual void virtual_14(RaceInfoBlock *blk);            /* [14] +0x7C  0x00181DF8 */
    virtual int  virtual_15();                              /* [15] +0x84  0x00181E28 the saved record's size */
    virtual const char *virtual_24();                       /* [24] +0xCC  0x00181B90 "RaceSinglePlayer" */
    virtual RaceInput *virtual_25();                        /* [25] +0xD4  0x00181D80 &input */
    virtual RaceInput *virtual_26(int id);                  /* [26] +0xDC  0x00181D90 0x100: &input, else RaceBase's */
    /* RaceBasic's; the race display gets the current view's car (slot 8),
     * view 0's camera value (slot 9) and its update (slot 2), between
     * race_display_poll_events / race_display_drop_events (T154); then the
     * record checks (event 0x16 in a race, event 1 at the end of a replay). */
    virtual void virtual_28(float dt);                      /* [28] +0xEC  0x00181E90 */

    /* &input (descriptive name).  Inline: T133 emits the copy at its end,
     * before __tf16RaceSinglePlayer (retail's frame info). */
    RaceInput *getInput() { return &input; }    /* 0x00182050 */

    RaceInput input;                        /* 0x16D40 pad slot 0x14, inputId 0x100 */
    /*  0x16E88 .. 0x16EBF: alignment */
    /* PC: 0x2AF88..0x2AFC0 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x2AFC0, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_2AF88, 0x38)
};
/* (Wave 1: the hud's RaceDisplay (0xA8E8, GT3 0xA47C) and the embedded race classes' growth; the size
 * from tools/sizeprobe.py, the member offsets above stay
 * GT3's.) */
EGCS_SIZE(RaceSinglePlayer, 0x17340);

/* RaceClassInfo "RaceSinglePlayer" (g_002B65D8: size 0x16EC0, these two). */
RaceBase *RaceSinglePlayer_create(void);                    /* 0x00181BA0 */
RaceBase *RaceSinglePlayer_construct(void *buf, int arg);   /* 0x00181C00 a RaceSinglePlayer in buf (NULL: none) */
/* .ctors entry 0x0028F0F4: race_class_register(&g_00351E20, &g_002B65D8). */
void      race_single_player_static_ctor(void);             /* 0x00182028 */


#endif /* GAME_T133_RACE_SINGLE_PLAYER_H */
