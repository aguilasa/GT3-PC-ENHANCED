/* T134_race_arcade.h - RaceArcadeInformation, RaceArcade, RaceArcadeSingle
 * (translation unit T134, unit u046), 0x00182118..0x00182850.
 *
 * RaceArcade is the arcade race ("RACE_MODE_SINGLE"): RaceSinglePlayer with
 * one CarIcon per car (7), the arcade result (ResultArcade), the race pause
 * menu, the replay pause menu and its car dynamics
 * (DynamicsConductorSinglePlayer, which RaceBase::dc points to).  At the
 * race start (virtual_8) it hands RaceBasic the race pause menu and the
 * result, or in a replay (mode 1) the replay pause menu and no result.
 * virtual_43 (before the race runs) sets the icons up and gives them to the
 * result board and the race display.  RaceArcadeSingle adds nothing; both
 * are registered race classes ("RaceArcade" g_002B6A58, "RaceArcadeSingle"
 * g_002B6A68).
 */
#ifndef GAME_T134_RACE_ARCADE_H
#define GAME_T134_RACE_ARCADE_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/race/race_cars.h"            /* PC: PC_RACE_CARS_MAX */
#include "game/race/modes/T133_race_single_player.h"   /* bases RaceSinglePlayer, RaceSinglePlayerInformation */
#include "game/car/model/T184_car_icon_maker.h"       /* CarIcon (held by value) */
#include "game/menu/records/T117_result_arcade.h"        /* ResultArcade (held by value) */
#include "game/race/pause/T169_race_pause.h"           /* RacePause (held by value) */
#include "game/race/pause/T170_replay_pause.h"         /* ReplayPause (held by value) */
#include "game/race/dynamics/T316_dc_single_player.h"     /* DynamicsConductorSinglePlayer (held by value) */

class RaceBase;                             /* pointers only */
class type_info;

/* The typeinfo functions, declared before the classes (see T132): GCC 14
 * build only.  egcs generates them itself (and their type names), unless an
 * earlier extern "C" declaration hides them (u046 matching). */

/* vtable 0x00328EF8 (15 slots: [0] its own, the others
 * RaceSinglePlayerInformation's / RaceInformation's).  typeinfo
 * __tf21RaceArcadeInformation 0x00182800 (si, base
 * RaceSinglePlayerInformation).  vptr +0x54.  No data of its own: size
 * 0x258. */
class RaceArcadeInformation : public RaceSinglePlayerInformation {
public:
    RaceArcadeInformation();                /* 0x00182118 */
    virtual ~RaceArcadeInformation();       /* [ 0] +0x0C 0x00182150 */
};
EGCS_SIZE(RaceArcadeInformation, 0x258);

/* vtable 0x00328D30 (56 slots: [0] [8] [24] [43] its own; index.txt names
 * [8] and [43] RaceArcadeSingle::virtual_N, they are RaceArcade's).
 * typeinfo __tf10RaceArcade 0x001827B0 (si, base RaceSinglePlayer).  vptr
 * +0.  Size 0x34580 (RaceArcade_create; the deleting destructor; the data of
 * RaceArcadeDemo start at +0x34580). */
class RaceArcade : public RaceSinglePlayer {
public:
    static void *operator new(size_t size);                 /* 0x00182178 the race pool */
    /* The placement new of run_gtmode_race (T053: new (p) RaceArcade, no
     * NULL test, so not <new>'s throw() one).  A member template: an exact
     * form, not the original.  An inline member would be emitted with the
     * vtable in T134 (the key function's file: every inline member), which
     * retail's T134 has not (its frame info; u046's data pass); a template
     * is emitted only where it is instantiated (T053, inline there).  The
     * construct functions use <new>'s ::new. */
    template <class T>
    static void *operator new(size_t size, T *p) { return p; }
    static void  operator delete(void *p, size_t size);     /* 0x001821A0 */
    /* The members (replayPause in mode 0), setRaceMode("RACE_MODE_SINGLE"),
     * dc = &dcSingle. */
    RaceArcade();                                           /* 0x00182260 */
    virtual ~RaceArcade();                                  /* [ 0] +0x0C  0x00182408 */
    /* RaceSinglePlayer's, then the pause menu and the result (a replay:
     * replayPause, told the organization, and no result). */
    virtual void virtual_8(int replay);                     /* [ 8] +0x4C  0x001824B8 */
    virtual const char *virtual_24();                       /* [24] +0xCC  0x001821C8 "RaceArcade" */
    /* The icons for the organization (F_001AE3F8, T184), given to the result
     * board and to the race display (RaceDisplay +0x1C). */
    virtual void virtual_43();                              /* [43] +0x164 0x00182550 */

    CarIcon      icons[PC_RACE_CARS_MAX];   /* 0x16EC0 one per car (PC: was 7; game/race/race_cars.h) */
    ResultArcade resultArcade;              /* 0x2509C */
    RacePause    pause;                     /* 0x259E8 */
    ReplayPause  replayPause;               /* 0x25C94 (mode 0) */
    DynamicsConductorSinglePlayer dcSingle; /* 0x261D4 RaceBase::dc points here (never destroyed) */
    /* PC: 0xA7AEC..0xA7B00 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0xA7B00, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_A7AEC, 0x14)
};
/* The enhanced engine: its conductor (the last member) 0x1E0 longer (GT3
 * 0x34580). */
/* (Wave 1: the hud's RaceDisplay (0xA8E8, GT3 0xA47C) and the embedded race classes' growth; the size
 * from tools/sizeprobe.py, the member offsets above stay
 * GT3's.) */
EGCS_SIZE(RaceArcade, 0x34C00);

/* vtable 0x00328B68 (56 slots: [0] [24] its own).  typeinfo
 * __tf16RaceArcadeSingle 0x00182760 (si, base RaceArcade).  No data of its
 * own: size 0x34580. */
class RaceArcadeSingle : public RaceArcade {
public:
    static void *operator new(size_t size);                 /* 0x001825B0 the race pool */
    static void  operator delete(void *p, size_t size);     /* 0x001825D8 */
    RaceArcadeSingle();                                     /* 0x00182698 */
    virtual ~RaceArcadeSingle();                            /* [ 0] +0x0C  0x001826D0 */
    virtual const char *virtual_24();                       /* [24] +0xCC  0x00182600 "RaceArcadeSingle" */
};
/* The enhanced engine: RaceArcade's (GT3 0x34580). */
/* (Wave 1: the hud's RaceDisplay (0xA8E8, GT3 0xA47C) and the embedded race classes' growth; the size
 * from tools/sizeprobe.py, the member offsets above stay
 * GT3's.) */
EGCS_SIZE(RaceArcadeSingle, 0x34C00);

/* RaceClassInfo "RaceArcade" (g_002B6A58) and "RaceArcadeSingle"
 * (g_002B6A68): size 0x34580, these functions. */
RaceBase *RaceArcade_create(void);                          /* 0x001821D8 */
RaceBase *RaceArcade_construct(void *buf, int arg);         /* 0x00182238 a RaceArcade in buf (NULL: none) */
RaceBase *RaceArcadeSingle_create(void);                    /* 0x00182610 */
RaceBase *RaceArcadeSingle_construct(void *buf, int arg);   /* 0x00182670 */
/* .ctors entry 0x0028F0F8: registers both (nodes g_00351E30, g_00351E38). */
void      race_arcade_static_ctor(void);                    /* 0x00182728 */


#endif /* GAME_T134_RACE_ARCADE_H */
