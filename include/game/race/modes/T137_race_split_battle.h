/* T137_race_split_battle.h - RaceSplitBattleInformation, RaceSplitBattle and
 * the templates RaceBattleInformationT<N>, RaceBattleT<N> (translation unit
 * T137, unit u046), 0x00183240..0x001848A8.  T137 emits the instances <2>;
 * T138 (RaceLinkBattle) the instances <8>.
 *
 * The 2-player battle on one console ("RACE_MODE_2P_BATTLE"): two RaceInputs
 * (pad slots 0x14 / 0x15, inputIds 0x100 / 0x101, port 0 / 1), one camera
 * view per player (CameraManager viewCount 2, view i
 * follows car i and reads input i), the 2-player result (Result2PBattle),
 * the race / replay pause menus (the replay one in mode 1), the split race
 * display (one display per screen; RaceSplitDisplay::target selects it) and
 * DynamicsConductorBattle2P.  The render (virtual_2) draws the two views one
 * above the other (each h / 2, mirror at 200,16 / 200,8, zoom 1.2) or, when
 * g_002F424C is set, side by side (each w / 2; the 3D views h * 8 / 11 high
 * from h / 6, the overlays full height, mirror at 90,5, zoom 1.0 / 1.3); a
 * replay is drawn full screen (one view) unless the game option +0x50 asks
 * for the split screen.  Both inputs' records go into the replay data
 * (virtual_13 / 14 / 15).  Registered as "RaceSplitBattle" (g_002B72B8).
 *
 * RaceSplitBattleInformation (RaceBattleInformationT<2>) gives the two cars
 * their equipments and inputIds 0x100 + i; the grid order is the entry
 * order.
 */
#ifndef GAME_T137_RACE_SPLIT_BATTLE_H
#define GAME_T137_RACE_SPLIT_BATTLE_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/race/core/T132_race_basic.h"           /* base RaceBasic */
#include "game/race/core/T124_race_information.h"     /* base RaceInformation */
#include "game/app/T000_main.h"                 /* RaceEntry (held by value) */
#include "game/race/dynamics/T309_dynamics_conductor_battle2p.h"  /* DynamicsConductorBattle2P (held by value) */
#include "game/race/core/T413_race_input.h"           /* RaceInput (held by value) */
#include "game/car/model/T184_car_icon_maker.h"       /* CarIcon (held by value) */
#include "game/menu/records/T192_result_2p_battle.h"     /* Result2PBattle (held by value) */
#include "game/race/pause/T169_race_pause.h"           /* RacePause (held by value) */
#include "game/race/pause/T170_replay_pause.h"         /* ReplayPause (held by value) */
#include "game/race/display/T155_race_split_display.h"   /* RaceSplitDisplay (held by value) */

class GSBuffer;                             /* pointers only: forward declarations */
class RaceBase;                             /* T129 */
class RaceEntryBase;                        /* T121 */
class RaceEntryCar;                         /* T121 */
class RaceOrganization;                     /* T125 */
struct RaceInfoBlock;                       /* T129 */
class CarEquipments;                        /* T482 */
class RacePOD2PDisplay;                     /* T155_race_pod2p_display.h (the enhanced engine) */
class type_info;

/* The typeinfo functions, declared before the classes (see T132): GCC 14
 * build only.  egcs generates them itself (and their type names), unless an
 * earlier extern "C" declaration hides them (u046 matching). */

/* The templates RaceBattleInformationT<N> and RaceBattleT<N> (their own
 * header since u047's data pass: T138 needs them without the <2> instances
 * this header makes). */
#include "game/race/modes/T137_race_battle_templates.h"

/* vtable 0x00329EC0 (15 slots: [0] [4] [10] [11] its own).  typeinfo
 * __tf26RaceSplitBattleInformation 0x00184808 (si, base
 * RaceBattleInformationT<2>).  vptr +0x54.  No data of its own: size 0x468
 * (u017: a 0x470-byte stack slot). */
class RaceSplitBattleInformation : public RaceBattleInformationT<2> {
public:
    RaceSplitBattleInformation();                       /* 0x00183240 2 cars (caller F_00133F48) */
    virtual ~RaceSplitBattleInformation();              /* [ 0] +0x0C 0x00183280 */
    virtual RaceEntryBase *createEntries();             /* [ 4] +0x2C 0x001832A8 new RaceEntryPS2 */
    /* Car n: entries[n]'s equipments, inputId 0x100 + n and (a car set) the
     * entry's +0x10 / +0x0C to the car's +0x20E8 / +0x20E4.  No range check. */
    virtual void setupEntry(RaceEntryCar *car);         /* [10] +0x5C 0x00183300 */
    virtual void setupOrganization(RaceOrganization *org);  /* [11] +0x64 0x00183378 only the grid order reset */
    /* entries[i].equip = eq (unreferenced; descriptive name, as T133's).
     * Inline: T137 (the key function's file) emits the copy at its end,
     * before __tf26RaceSplitBattleInformation: retail's frame info has it
     * there, between RaceBattleT<2>'s __tf and this class's, so it is this
     * class's member, not RaceBattleInformationT<2>'s (u046's data pass). */
    void setEquipments(const CarEquipments &eq, int i) { entries[i].setEquipments(eq); }  /* 0x001847E0 */
};
EGCS_SIZE(RaceBattleInformationT<2>, 0x468);
EGCS_SIZE(RaceSplitBattleInformation, 0x468);

/* (RaceBattleT<N>: T137_race_battle_templates.h.) */
EGCS_SIZE(RaceBattleT<2>, 0xC8C0);

/* vtable 0x00329B30 (56 slots: [0] [2] [8] [13] [14] [15] [24] [25] [26]
 * [28] [30] [33] [43] [45] [46] its own, [35] RaceBasic's).  typeinfo
 * __tf15RaceSplitBattle 0x00184740 (si, base RaceBattleT<2>).  vptr +0.
 * Size 0x3EC80 (RaceSplitBattle_create; the deleting destructor). */
class RaceSplitBattle : public RaceBattleT<2> {
public:
    /* (No class placement new: RaceSplitBattle_construct uses <new>'s ::new,
     * and an inline member would be emitted with the vtable, which retail's
     * T137 has not; u046's data pass.) */
    static void *operator new(size_t size);                 /* 0x00183398 the race pool */
    static void  operator delete(void *p, size_t size);     /* 0x001833C0 */
    /* The members (replayPause in mode 1), setRaceMode
     * ("RACE_MODE_2P_BATTLE"), the split display as the race display,
     * dc = &dc2P, inputs[i] to pad slot 0x14 + i, their recorders to
     * player i. */
    RaceSplitBattle();                                      /* 0x00183480 */
    virtual ~RaceSplitBattle();                             /* [ 0] +0x0C  0x001837F0 the inputs leave the pad manager */
    virtual void render(GSBuffer &buf);                     /* [ 2] +0x1C  0x00183E08 (GameObject::render, as RacePS2Base's) */
    /* RaceBasic's, the menus (as RaceArcade's, with the 2P result), then two
     * camera views: view i reads inputs[i] and follows car i. */
    virtual void virtual_8(int replay);                     /* [ 8] +0x4C  0x00183948 */
    virtual void virtual_13(RaceInfoBlock *blk);            /* [13] +0x74  0x00184358 both records loaded from after the block */
    virtual void virtual_14(RaceInfoBlock *blk);            /* [14] +0x7C  0x001843D8 two recorded inputs, appended */
    virtual int  virtual_15();                              /* [15] +0x84  0x00184460 both records' size */
    virtual const char *virtual_24();                       /* [24] +0xCC  0x001833E8 "RaceSplitBattle" */
    virtual RaceInput *virtual_25();                        /* [25] +0xD4  0x00183AE8 &inputs[0] */
    virtual RaceInput *virtual_26(int id);                  /* [26] +0xDC  0x00183AF8 0x100 / 0x101: inputs[i], else RaceBase's */
    /* RaceBasic's, updateDisplay, then the record checks of both players
     * (event 0x16 with the player in a race, event 1 in a replay). */
    virtual void virtual_28(float dt);                      /* [28] +0xEC  0x00183B40 */
    virtual void virtual_30();                              /* [30] +0xFC  0x00183C70 RaceBasic's, updateDisplay */
    virtual void virtual_33();                              /* [33] +0x114 0x00183DD0 RacePS2Base's, field_97C0 = 1 */
    virtual void virtual_43();                              /* [43] +0x164 0x00183A90 the icons, given to the result */
    virtual void virtual_45(GSBuffer *buf);                 /* [45] +0x174 0x00184310 the split display's screen (its slot 17) */
    virtual void virtual_46(GSBuffer *buf);                 /* [46] +0x17C 0x00184338 RacePS2Base's */

    /* Descriptive name.  Per screen i: the current view i, the split
     * display's target i, player i, the view's car and view i's camera
     * mount (its slots 7, 8, 9, called directly); then its update (slot 2);
     * between race_display_poll_events / race_display_drop_events (T154). */
    void updateDisplay();                                   /* 0x00183CA0 */
    /* The enhanced engine: splitDisplay's target i, and the Pod 2P race's
     * display's (render's two screens). */
    void setDisplayTarget(int i);

    DynamicsConductorBattle2P dc2P;         /* 0x0C8C0 RaceBase::dc points here (never destroyed) */
    RaceInput        input;                 /* 0x1AC6C (? not attached, not used by this class) */
    RaceInput        inputs[2];             /* 0x1ADB4 the players' (stride 0x148) */
    CarIcon          icons[7];              /* 0x1B044 */
    Result2PBattle   result2P;              /* 0x29220 */
    RacePause        pause;                 /* 0x29B6C */
    ReplayPause      replayPause;           /* 0x29E18 (mode 1) */
    RaceSplitDisplay splitDisplay;          /* 0x2A358 */
    /* The enhanced engine (Lupo 2's RaceSplitBattleBase::getDisplay, slots
     * 63 / 64): the Pod 2P race's split display (RacePOD2P sets it), which
     * the battle then drives instead of splitDisplay; NULL: splitDisplay. */
    RacePOD2PDisplay *podDisplay;           /* 0x3EC58 (in GT3's alignment) */
    /*  0x3EC5C .. 0x3EC7F: alignment */
    /* PC: 0x7FFAC..0x7FFC0 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x7FFC0, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_7FFAC, 0x14)
};
/* The enhanced engine: its conductor 0x1E0 and its split display 8 longer:
 * the members after dc2P sit 0x1E0 later than shown, after splitDisplay
 * 0x1E8 (GT3 0x3EC80). */
/* (Wave 1: the hud's split display (two RaceDisplays: 0x151D8, GT3 0x14900) and the embedded race classes' growth; the size
 * from tools/sizeprobe.py, the member offsets above stay
 * GT3's.) */
EGCS_SIZE(RaceSplitBattle, 0x3F740);

/* RaceClassInfo "RaceSplitBattle" (g_002B72B8: size 0x3EC80, these two). */
RaceBase *RaceSplitBattle_create(void);                     /* 0x001833F8 */
RaceBase *RaceSplitBattle_construct(void *buf, int arg);    /* 0x00183458 a RaceSplitBattle in buf (NULL: none) */
/* .ctors entry 0x0028F104: race_class_register(&g_00351E68, &g_002B72B8). */
void      race_split_battle_static_ctor(void);              /* 0x00184718 */


#endif /* GAME_T137_RACE_SPLIT_BATTLE_H */
