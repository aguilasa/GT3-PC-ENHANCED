/* T132_race_basic.h - RaceBasic, RaceBasicWithRaceDisplay (translation unit
 * T132, unit u045), 0x00180EB8..0x001817F8.
 *
 * RaceBasic is the registered race mode "RaceBasic" (mode name
 * "RACE_MODE_SINGLE"): a RacePS2Base with three optional parts the derived
 * modes plug in -
 *   +0xC880 a pause menu (PauseBase): START (bit 0x10 of the newly pressed
 *           buttons) opens it when there is one (in replay mode 2 it posts
 *           event 1 instead); while paused virtual_30 runs the menu, whose
 *           result 2 / 4 / 5 ends the race through virtual_34(3 / 2 / 4) and
 *           3 resumes;
 *   +0xC884 the result sequence of the mode (RaceResultBase: ResultArcade,
 *           Result2PBattle, ResultTimeTrial, ...), started / updated / drawn
 *           with the race;
 *   +0xC888 the race display (a RaceDisplayBase: RaceDisplay, or the
 *           RaceSplitDisplay of the split battle and the logger), started
 *           with the mode id.
 * RaceBasicWithRaceDisplay embeds a RaceDisplay at +0xC8C0 and makes it the
 * race display.
 *
 * The race classes are 64-byte aligned (RaceBase's CameraManager at +0x40:
 * the cameras' matrices are aligned(64), T397), so every size below is a
 * multiple of 0x40 and the space after the last member (RaceBasic
 * 0xC890..0xC8C0) is alignment, not data.
 *
 * The operator new / delete pairs of the race classes are each class's own
 * copies of the race-pool operators (heap_alloc_core / heap_free_core on the
 * pool 0x002B3BF0, 16-byte aligned).  race_basic_construct uses the global
 * placement form of <new> (::new: egcs's is inline and throw(), so the
 * result is tested for NULL, and egcs emits no copy of it; a member
 * placement new would be emitted out of line in T132, which retail is
 * not; u045).
 */
#ifndef GAME_T132_RACE_BASIC_H
#define GAME_T132_RACE_BASIC_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/race/core/T130_race_ps2_base.h"        /* base RacePS2Base */
#include "game/race/display/T154_race_display.h"         /* RaceDisplay (held by value) */

class GSBuffer;                             /* pointers only: forward declarations */
class PauseBase;                            /* T168 */
class RaceResultBase;                       /* T116 */
class RaceDisplayBase;                      /* T154 */
class RaceBase;                             /* T129 */
class type_info;

/* The typeinfo functions (vtable entry 0).  Declared before the classes:
 * egcs declares a polymorphic class's __tf<mangled> itself (C++ linkage)
 * when the class is complete and rejects a later extern "C" declaration; an
 * earlier one is the declaration it uses - and then it does not generate
 * the function (u045 matching), so egcs does not see them at all. */

/* vtable 0x003284E8 (56 slots: [0] [8] [24] [28] [30] [35] [45] its own,
 * the others RaceBase's / RacePS2Base's; index.txt names slots 8, 24, 28,
 * 30, 35 and 45 RaceBasicWithRaceDisplay::virtual_N, they are RaceBasic's).
 * typeinfo __tf9RaceBasic 0x001817A8 (si, base RacePS2Base).  vptr +0
 * (GameObject's).  Size 0xC8C0 (race_basic_create; the deleting destructor
 * and the template RaceBattleT<N>: operator delete(this, 0xC8C0); the data
 * of RaceBattleT's subclasses and RaceSolitaire start at +0xC8C0). */
class RaceBasic : public RacePS2Base {
public:
    static void *operator new(size_t size);                 /* 0x00180EB8 the race pool */
    static void  operator delete(void *p, size_t size);     /* 0x00180EE0 (also the deleting form of
                                                               RaceBasicWithRaceDisplay, RaceBattleT<2> / <8>) */
    /* RacePS2Base, no menu / result / display, setRaceMode("RACE_MODE_SINGLE"). */
    RaceBasic();                                            /* 0x00180F98 */
    virtual ~RaceBasic();                                   /* [ 0] +0x0C  0x00181020 */
    /* Race start: RacePS2Base's, then the race display (virtual_0(org,
     * raceModeId, replay); the inner panel only outside replays), the pause
     * menu (virtual_1) and the result (virtual_1(org, replay)). */
    virtual void virtual_8(int replay);                     /* [ 8] +0x4C  0x00181078 */
    virtual const char *virtual_24();                       /* [24] +0xCC  0x00180F08 "RaceBasic" */
    /* The update: RacePS2Base's, the result's, then START outside background
     * mode: pause (setPause(1), stopSounds) or, in replay mode 2, event 1.
     * The enhanced engine (hud.md C14, race_logic.md C4 a): in a replay,
     * L1 + L2 + R1 + R2 together toggle the race display's mini logo. */
    virtual void virtual_28(float dt);                      /* [28] +0xEC  0x001811B8 */
    /* The update while paused: the pause menu's result (no NULL check);
     * the enhanced engine: then cycleTelemetry. */
    virtual void virtual_30();                              /* [30] +0xFC  0x001812D8 */
    /* The race display's virtual_1 and the result's virtual_2, then
     * RacePS2Base's. */
    virtual void virtual_35();                              /* [35] +0x124 0x00181140 */
    /* The 2D overlays: RacePS2Base's, the race display, the result and,
     * while paused, the pause menu (with view 0's camera). */
    virtual void virtual_45(GSBuffer *buf);                 /* [45] +0x174 0x00181400 */

    /* paused = pauseMenu ? on : 0.  The enhanced engine: through
     * RaceBase::setPausedState (the wheel's centring spring, backend.md B2). */
    void setPause(int on);                  /* 0x001814D8 */
    /* The enhanced engine (hud.md C3, race_logic.md C4 b; Lupo 2 L2
     * 0x0017F9A0): in a live race, SELECT pressed alone cycles the race
     * display's telemetry (none, the accel / brake monitor, both with the
     * steering display) and lays the display out again at once.  From the
     * paused updates (virtual_30 of RaceBasic, RaceFreeRun,
     * RaceMachineTest). */
    void cycleTelemetry();
    /* Ignored in background mode; opens the menu (virtual_1). */
    void setPauseMenu(PauseBase *m);        /* 0x001814F0 */
    /* Ignored in background mode; starts it (virtual_1(org, replayMode)). */
    void setSubDisplay(RaceResultBase *r);  /* 0x00181538 */
    void setRaceDisplay(RaceDisplayBase *d);/* 0x00181588 */
    /* raceModeId = the id of name in the mode name table g_002F00C0 (-1:
     * unknown; F_00239650). */
    void setRaceMode(const char *name);     /* 0x00181598 */
    /* Inline (no callers; egcs emits the class's inline members at the end
     * of T132 in reverse order: setInnerPanel, then this). */
    void setRaceModeId(int id) { raceModeId = id; }     /* 0x00181798 */
    /* No NULL check.  Inline in the original: RaceArcadeDemo's constructor
     * expands it (u046), the copy at 0x00181760 has no callers. */
    void setInnerPanel(bool on) { raceDisplay->innerPanel(on); }   /* 0x00181760 */

    PauseBase       *pauseMenu;             /* 0xC880 NULL: START does nothing */
    RaceResultBase  *subDisplay;            /* 0xC884 the result sequence (u046: result) */
    RaceDisplayBase *raceDisplay;           /* 0xC888 (u046: display) */
    int              raceModeId;            /* 0xC88C the id of the "RACE_MODE_..." name */
    /*  0xC890 .. 0xC8BF: alignment */
    /* PC: 0x20510..0x20540 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x20540, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_20510, 0x30)
};
EGCS_SIZE(RaceBasic, 0xC8C0);

/* vtable 0x00328320 (56 slots: [0] its own, the others RaceBasic's).
 * typeinfo __tf24RaceBasicWithRaceDisplay 0x00181710 (si, base RaceBasic).
 * Size 0x16D40 (the deleting destructor: operator delete(this, 0x16D40);
 * RaceSinglePlayer's input at +0x16D40). */
class RaceBasicWithRaceDisplay : public RaceBasic {
public:
    /* RaceBasic, the RaceDisplay, setRaceDisplay(&display). */
    RaceBasicWithRaceDisplay();             /* 0x001815D0 */
    virtual ~RaceBasicWithRaceDisplay();    /* [ 0] +0x0C  0x00181678 ~display (flags 2), ~RaceBasic */

    RaceDisplay display;                    /* 0xC8C0 (u046 / u050: raceDisplay) */
    /*  0x16D3C .. 0x16D3F: alignment */
};
/* (Wave 1: the hud's RaceDisplay (0xA8E8, GT3 0xA47C) and the embedded race classes' growth; the size
 * from tools/sizeprobe.py, the member offsets above stay
 * GT3's.) */
EGCS_SIZE(RaceBasicWithRaceDisplay, 0x171C0);

/* RaceClassInfo "RaceBasic" (g_002B6218: size 0xC8C0, these two). */
RaceBase *race_basic_create(void);                      /* 0x00180F18 new RaceBasic */
RaceBase *race_basic_construct(void *buf, int arg);     /* 0x00180F70 a RaceBasic in buf (NULL: none); arg unused */
/* .ctors entry 0x0028F0F0: race_class_register(&g_00351E10, &g_002B6218). */
void      race_basic_static_init(void);                 /* 0x001816E8 */

/* T132's registry data (RaceClassInfo / RaceClassNode: T129). */
extern RaceClassInfo g_002B6218;            /* 0x002B6218 {"RaceBasic", 0xC8C0, race_basic_create, race_basic_construct} */
extern RaceClassNode g_00351E10;            /* 0x00351E10 gp-0x5BE0 RaceBasic's node in the registry */


#endif /* GAME_T132_RACE_BASIC_H */
