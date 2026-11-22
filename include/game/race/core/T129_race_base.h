/* T129_race_base.h - RaceBase, the registry of race classes, the race info
 * block (translation unit T129, unit u043), 0x0017B8F0..0x0017CF30.
 *
 * RaceBase is the base of every race mode (RaceBase -> RacePS2Base (T130 /
 * T131) -> RaceBasic (T132) -> ...).  It ties a RaceOrganization (+4, the
 * race state), a DynamicsConductor (+8, the physics) and the race BGM (+0xC)
 * to a CameraManager (embedded at +0x40).  update() (GameObject slot 1) runs
 * once per frame: virtual_27 consumes the race events, virtual_28 / 29 run
 * the frame (virtual_30 while paused).  virtual_8 starts the race (virtual_9
 * is one of its steps); virtual_7 / 11 / 17 / 19 write and read a
 * 0x191C-byte description of the race (RaceInfoBlock, magic 'GT2K') that
 * the RaceSolitaire modes extend (virtual_13 / 14 / 15 and 20 / 21 / 22).
 * The race thread sleeps in virtual_38 until signal() wakes it.
 *
 * The race classes are 64-byte aligned: CameraManager is (T383: its views'
 * matrices), so camera sits at +0x40 after 0x30 bytes of alignment and every
 * size is a multiple of 0x40 (RaceBase 0x97C0, RacePS2Base 0xC880,
 * RaceBasic 0xC8C0).
 *
 * Slot names: slots 1..3 are GameObject's (T002, u001's names update /
 * render / virtual_3), slot 40 is Lupo 2's getNumberOfLaps
 * (RaceArcadeDemo::getNumberOfLaps, T135); the others keep the dossier's
 * virtual_N, the names the derived classes (T130..T152) override.
 *
 * Inline in the original: RaceBase's operator new / delete (copies in T132,
 * T175 and the race modes) and the accessors at the end of the unit, among
 * them three of RaceLapTime's (declared in T126).
 */
#ifndef GAME_T129_RACE_BASE_H
#define GAME_T129_RACE_BASE_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/app/T002_render_thread.h"        /* base GameObject */
/* The accessors' inline bodies (getEnvPtr, getCourseData, getCourse) need
 * the course, the organization and the course data complete.  (u043's data
 * pass: no includer's object changes.) */
#include "game/race/core/T153_race_course.h"          /* RaceCourse */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization */
#include "game/course/T179_course_data.h"          /* CourseData */
#include "game/camera/T383_camera_manager.h"       /* CameraManager (held by value) */
#include "game/menu/race_menu/T115_race_option.h"          /* RaceOption (held by value) */
#include "game/data/T482_car_equipments.h"       /* CarSaveEquipments (held by value) */
#include "game/car/physics/T280_car_condition.h"        /* CarCondition (held by value) */

class type_info;
class RaceOrganization;                     /* T125 (pointers only: forward declarations) */
class DynamicsConductor;                    /* T305 */
class RaceBGMBase;                          /* T175 */
class RaceMemPool;                          /* T174 */
class RaceInput;                            /* T413 */
class RaceCourse;                           /* T153 */
class CourseData;                           /* T179 */
class CourseEnv;                            /* T178 */
class RaceBase;
namespace gt2k_game {
struct GameThumbnail;                       /* T108 (the save's description) */
}

/* RaceBase::replayMode, virtual_8's / RacePS2Base::virtual_41's `replay'.
 * (The sweep moved u001 / u016 / u017 / u044 / u045 / u046 / u048 / u050's
 * copies here.) */
enum RaceReplayMode {
    RACE_LIVE       = 0,                    /* a race */
    RACE_REPLAY     = 1,                    /* a replay of a recorded race (or of a demo) */
    RACE_DEMO       = 2,                    /* the race plays by itself (an attract demo, behind the
                                               short menu) */
    RACE_REPLAY_ALT = 3                     /* ? no caller passes it that is known; RacePS2Base::
                                               getVolume treats it as a race, RaceDisplay as a replay */
};

/* RaceBase::endCode: virtual_34's code.  RaceBasic::virtual_30 maps the pause
 * results (T168) to 2 / 3 / 4.  (The sweep moved u001 / u044 / u045 / u046 /
 * u048 / u049's copies here.) */
enum RaceEndCode {
    RACE_END_EXIT         = 2,              /* the pause menu's exit (PAUSE_RESULT_EXIT) */
    RACE_END_TIMEOUT      = 3,              /* the pause menu timed out (PAUSE_RESULT_TIMEOUT) */
    RACE_END_RETRY        = 4,              /* retry (PAUSE_RESULT_RETRY): RacePS2Base::virtual_41
                                               runs the race again */
    RACE_END_DEMO_TIME_UP = 5               /* an arcade demo ran to its time limit (RaceArcadeDemo::
                                               virtual_27: event 7); ? no caller of getEndCode tests it */
};


/* ------------------------------------------------------------------------ */
/* The race info block                                                      */
/* ------------------------------------------------------------------------ */

/* One car of a RaceInfoBlock.  Not polymorphic.  Size 0x280 (8 of them at
 * +0x518; memset 0x1400). */
struct RaceInfoSlot {
    /* RaceBase::virtual_7: CarEquipments::operator= on the slot (0x1E8
     * bytes), then the car's condition (0x20 bytes) over its tail. */
    CarSaveEquipments info;                 /* 0x000 (u049 CarEquipmentsData) */
    CarCondition      condition;            /* 0x1D0 RaceEntryCar::condition (u043 / u049 name) */
    int               inputId;              /* 0x1F0 */
    int               order;                /* 0x1F4 */
    /* The entry's options, read signed (lb: race_entry_load_slot, load_demo_setting;
     * u015 / u049). */
    s8                transmission;         /* 0x1F8 RaceEntryCar::transmission (u043 opt0) */
    s8                tyre;                 /* 0x1F9 RaceEntryCar::tyre (u043 opt1) */
    s8                steeringAssist;       /* 0x1FA RaceEntryCar::steeringAssist (u043 opt2) */
    s8                assistAsm;            /* 0x1FB RaceEntryCar::assistAsm (u043 opt3) */
    s8                assistTcs;            /* 0x1FC RaceEntryCar::assistTcs (u043 flag) */
    u8                unk_1FD[0x83];        /* 0x1FD */
};
EGCS_SIZE(RaceInfoSlot, 0x280);

/* A serialised description of a race: the header of the replay / ghost data
 * the RaceSolitaire modes save (RaceBase::virtual_7 builds it, virtual_19
 * reads the first car back).  0x191C bytes are written (the size word); the
 * u64 members align the struct to 8.  Not polymorphic. */
struct RaceInfoBlock {
    int          size;                      /* 0x000 0x191C */
    u32          magic;                     /* 0x004 'GT2K' (0x4B325447) */
    u32          version;                   /* 0x008 0x30000 */
    char         raceName[0x40];            /* 0x00C RaceBase::virtual_24() */
    RaceOption   option;                    /* 0x04C the organization's */
    /*  0x064: alignment */
    u64          raceCode;                  /* 0x068 */
    u64          courseCode;                /* 0x070 */
    u32          total;                     /* 0x078 car 0: the lap total */
    u32          bestLap;                   /* 0x07C car 0: the best lap */
    u8           unk_080[0x80];             /* 0x080 cleared by virtual_7 */
    u8           unk_100[0x410];            /* 0x100 */
    int          carCount;                  /* 0x510 */
    /*  0x514: alignment */
    RaceInfoSlot slots[8];                  /* 0x518 */
    /* The input records appended after the block: 0 here, 1 single player,
     * 2 split battle (u046; u043 field_1918). */
    int          recordCount;               /* 0x1918 */
};

/* (RaceBase::virtual_12 / virtual_18 fill the replay / best lap part of
 * the save's gt2k_game::GameThumbnail, T108: u043's RaceSummary view of it
 * is gone, u049.) */

/* ------------------------------------------------------------------------ */
/* The registry of race classes                                             */
/* ------------------------------------------------------------------------ */

/* One registered race class (RaceBasic, RaceSinglePlayer, ...), static data
 * of its unit (RaceBasic: g_002B6218). */
struct RaceClassInfo {
    const char *name;                       /* 0x00 */
    u32         size;                       /* 0x04 sizeof the class */
    RaceBase  *(*create)(void);             /* 0x08 new + construct */
    RaceBase  *(*construct)(void *buf, int arg);    /* 0x0C construct in buf */
};
EGCS_SIZE(RaceClassInfo, 0x10);

/* A registry node: the static constructor of each class links one into
 * g_00351DF4 with race_class_register (RaceBasic: g_00351E10). */
struct RaceClassNode {
    RaceClassNode *next;                    /* 0x00 */
    RaceClassInfo *info;                    /* 0x04 */
};
EGCS_SIZE(RaceClassNode, 0x8);

/* ------------------------------------------------------------------------ */
/* RaceBase                                                                 */
/* ------------------------------------------------------------------------ */

/* vtable 0x003279E0 (41 slots: [2] / [3] GameObject's).  typeinfo
 * __tf8RaceBase 0x0017CED8 (si, base GameObject).  vptr +0 (GameObject's;
 * GameObject has no data).  Size 0x97C0 (the deleting destructor:
 * operator delete(this, 0x97C0)). */
class RaceBase : public GameObject {
public:
    static void *operator new(size_t size);             /* 0x0017BA18 the race pool, 16-byte aligned */
    static void  operator delete(void *p, size_t size); /* 0x0017BA40 */
    /* GameObject, the camera; bgm = the dummy g_003521D0, threadId the
     * constructing thread, renderEnabled 1, the rest 0. */
    RaceBase();                                         /* 0x0017BA68 */
    virtual ~RaceBase();                                /* [ 0] +0x0C  0x0017BB38 */
    /* The per-frame driver: when virtual_27 accepts the frame, virtual_28
     * (dt 1/60) or virtual_30 while paused, then virtual_31; event 9:
     * virtual_33. */
    virtual void update();                              /* [ 1] +0x14  0x0017C570 */
    /*  [ 2] +0x1C render, [ 3] +0x24 virtual_3: GameObject's (RacePS2Base
     *  overrides them) */
    virtual RaceMemPool *virtual_4();                   /* [ 4] +0x2C  0x0017BBA0 &g_0049C378 */
    virtual int  virtual_5();                           /* [ 5] +0x34  0x0017BBD8 0 */
    virtual CameraManager *virtual_6();                 /* [ 6] +0x3C  0x0017BBD0 &camera */
    virtual void virtual_7(RaceInfoBlock *blk);         /* [ 7] +0x44  0x0017C000 builds the info block */
    /* Starts the race (called by RacePS2Base::virtual_8): replayMode =
     * replay (frozen lap times), the organization's race state, the
     * conductor and its start kind, the inputs, the course, the cars'
     * models and sounds, the camera, startSounds. */
    virtual void virtual_8(int replay);                 /* [ 8] +0x4C  0x0017BBE0 */
    virtual void virtual_9();                           /* [ 9] +0x54  0x0017BF98 org->attachInputs(this), dc->virtual_22 */
    virtual int  virtual_10();                          /* [10] +0x5C  0x0017C1F0 virtual_15() + 0x191C */
    virtual void virtual_11(RaceInfoBlock *blk);        /* [11] +0x64  0x0017C220 virtual_7, virtual_14 */
    /* The replay save's thumbnail (Serialize::ReplayData::describe): car
     * 0's lap total, the stored laps, its car, the course, its power. */
    virtual void virtual_12(gt2k_game::GameThumbnail *thumb);   /* [12] +0x6C  0x0017C278 */
    virtual void virtual_13(RaceInfoBlock *blk);        /* [13] +0x74  0x0017C318 empty (the overrides load after blk) */
    virtual void virtual_14(RaceInfoBlock *blk);        /* [14] +0x7C  0x0017C320 empty (the extension of virtual_11) */
    virtual int  virtual_15();                          /* [15] +0x84  0x0017C328 0 (its size) */
    virtual int  virtual_16();                          /* [16] +0x8C  0x0017C330 virtual_22() + 0x191C */
    virtual void virtual_17(RaceInfoBlock *blk);        /* [17] +0x94  0x0017C360 virtual_7, virtual_21 */
    /* The ghost save's (GhostData / MaxSpeed::describe): car 0's
     * ghostLapTime, its car, the course, its power, mass 0. */
    virtual void virtual_18(gt2k_game::GameThumbnail *thumb);   /* [18] +0x9C  0x0017C3B8 */
    /* Reads a block: car 1 made (setupGhost(2, 0)), reloaded from slot 0 (race_entry_load_slot),
     * input id 1. */
    virtual void virtual_19(RaceInfoBlock *blk);        /* [19] +0xA4  0x0017C448 */
    virtual void virtual_20(RaceInfoBlock *blk);        /* [20] +0xAC  0x0017C4B8 empty (u043: no argument; h25) */
    virtual void virtual_21(RaceInfoBlock *blk);        /* [21] +0xB4  0x0017C4C0 empty (the extension of virtual_17) */
    virtual int  virtual_22();                          /* [22] +0xBC  0x0017C4C8 0 (its size) */
    virtual void virtual_23();                          /* [23] +0xC4  0x0017BBB0 empty */
    virtual const char *virtual_24();                   /* [24] +0xCC  0x0017C4D0 the mode's name: "<RACE@BASE>" */
    /* The default input: the function-local static RaceInput g_0049C230
     * (guard g_00351DF8; destroyed at exit by race_input_static_dtor). */
    virtual RaceInput *virtual_25();                    /* [25] +0xD4  0x0017C508 */
    /* The input of input id `id' (RaceEntryInformation::inputId); here the
     * disabled RaceInput g_002DFB58 (T413) (u043 / u045: CarDynamics *). */
    virtual RaceInput *virtual_26(int id);              /* [26] +0xDC  0x0017C560 */
    /* The events: 1 -> virtual_33, 0 (stop this frame); 4 -> start;
     * 0x14 -> the BGM (-2 pause, -3 resume, -1 stop, else play(code,
     * code == 0x14)).  1 otherwise. */
    virtual int  virtual_27();                          /* [27] +0xE4  0x0017C630 */
    virtual void virtual_28(f32 dt);                    /* [28] +0xEC  0x0017C778 virtual_29(dt) */
    /* The frame: the organization's start / timer, the BGM, the course, the
     * conductor, the ranks and events, the camera. */
    virtual void virtual_29(f32 dt);                    /* [29] +0xF4  0x0017C7A0 */
    virtual void virtual_30();                          /* [30] +0xFC  0x0017C908 empty (the frame while paused) */
    virtual void virtual_31();                          /* [31] +0x104 0x0017C910 empty */
    /* The start (RACE_EVENT_START): posts RACE_EVENT_GO, then the race music
     * (RACE_EVENT_BGM, code 0x14).  (u043 virtual_32.) */
    virtual void start();                               /* [32] +0x10C 0x0017C918 */
    virtual void virtual_33();                          /* [33] +0x114 0x0017C968 empty */
    virtual void virtual_34(int code);                  /* [34] +0x11C 0x0017C9F0 ends the race: endCode = code, virtual_33 */
    virtual void virtual_35();                          /* [35] +0x124 0x0017CA28 stopSounds */
    /* The cars' sounds started / stopped and the BGM resumed / paused
     * (codes -3 / -2): pause and resume of a race (u043 virtual_36 / 37). */
    virtual void startSounds();                         /* [36] +0x12C 0x0017CA50 */
    virtual void stopSounds();                          /* [37] +0x134 0x0017CAE8 */
    virtual int  virtual_38();                          /* [38] +0x13C 0x0017CBF0 SleepThread, then takeSignal() (u043: void) */
    /* The kind of the DB's course (getCourseKind): 1 the second-set tyres,
     * 2 (dirt) dirt tyres on every car. */
    virtual void virtual_39();                          /* [39] +0x144 0x0017CC80 */
    /* The race record's numberOfLaps (u043 virtual_40; u046 / T135
     * getNumberOfLaps). */
    virtual int  getNumberOfLaps();                     /* [40] +0x14C 0x0017CD98 */

    void setBackgroundMode(int v);          /* 0x0017BBB8 also g_003521E8 (u043 F_0017BBB8) */
    /* Ends the race frame: stopSounds, bgm->stop(), dc->virtual_26,
     * org->F_0017AA48, signal(1) (RacePS2Base::virtual_27). */
    void endRace();                         /* 0x0017C970 (u043 F_0017C970) */
    void signal(int v);                     /* 0x0017CB80 wakeup = v, WakeupThread(threadId) */
    int  takeField9798();                   /* 0x0017CBA8 read and cleared, interrupts off */
    void setField9798(int v);               /* 0x0017CC20 */
    int  takeSignal();                      /* 0x0017CC38 wakeup, read and cleared, interrupts off */
    /* The accessors: inline in the original (Lupo 2: linkonce copies).  The
     * key function is in T129, so egcs writes every one there, at the end
     * of the unit, in the reverse of this order (retail 0x0017CDF8 getEndCode
     * .. 0x0017CED0 setOrganization; the frame info's FDE order).  Lupo 2
     * names from setReplayMode up. */
    void setOrganization(RaceOrganization &o) { org = &o; }    /* 0x0017CED0 */
    RaceOrganization &getOrganization() { return *org; }        /* 0x0017CEC8 (not RaceLapTime::getCurrent) */
    void setDynamicsConductor(DynamicsConductor &d) { dc = &d; }    /* 0x0017CEC0 */
    DynamicsConductor &getDynamicsConductor() { return *dc; }   /* 0x0017CEB8 (not RaceLapTime::getStart) */
    RaceCourse *getCourse() { return org->course; }             /* 0x0017CEA8 */
    CourseData *getCourseData() { return org->course->data; }   /* 0x0017CE98 */
    CourseEnv  *getEnvPtr() { return org->course->data->env; }  /* 0x0017CE80 */
    RaceBGMBase *getBGM() { return bgm; }   /* 0x0017CE78 (the map's RaceLapTime::getMax there is a code match) */
    void setReplayMode(int v) { replayMode = (RaceReplayMode)v; }  /* 0x0017CE68 */
    int  getReplayMode() { return replayMode; }                 /* 0x0017CE58 */
    int  getBackgroundMode() { return backgroundMode; }         /* 0x0017CE48 */
    void setRenderEnabled(int v) { renderEnabled = v; }         /* 0x0017CE38 */
    int  getRenderEnabled() { return renderEnabled; }           /* 0x0017CE28 (u043 getField978C) */
    void setPaused(int v) { paused = v; }   /* 0x0017CE18 */
    /* The enhanced engine (backend.md B2, race_logic.md C5; Lupo 2 L2
     * 0x0017A650): paused = on, and in a live race a change goes to the
     * player's input (RaceInput::setMode: the wheel's spring).  Callers:
     * virtual_8, RaceBasic::setPause. */
    void setPausedState(int on);
    int  getPaused() { return paused; }     /* 0x0017CE08 */
    int  getEndCode() { return endCode; }   /* 0x0017CDF8 (u043 getField9794) */
    /* (0x0017CF28, the last function of T129, is its weak copy of T121's
     * inline F_00179460: T129_race_base.cpp.) */

    RaceOrganization  *org;                 /* 0x0004 */
    DynamicsConductor *dc;                  /* 0x0008 */
    /* The race BGM: the dummy g_003521D0 until RacePS2Base installs its
     * RaceBGMPS2 (u044 / u048; u043 RaceInput *input). */
    RaceBGMBase       *bgm;                 /* 0x000C */
    /*  0x0010 .. 0x003F: alignment (CameraManager is 64-byte aligned) */
    CameraManager      camera;              /* 0x0040 */
    int                threadId;            /* 0x9780 the race thread (GetThreadId in the constructor) */
    /* The enum (not int): its own alias set lets virtual_8's store follow the
     * organization's int loads as retail does (0x0017BBE0). */
    RaceReplayMode     replayMode;          /* 0x9784 virtual_8's argument: RACE_LIVE / REPLAY / DEMO / REPLAY_ALT */
    int                backgroundMode;      /* 0x9788 the organization's logger / background mode */
    int                renderEnabled;       /* 0x978C 1; RacePS2Base::render draws only while set (u018: the logger) */
    int                paused;              /* 0x9790 update() runs virtual_30 instead of virtual_28 */
    int                endCode;             /* 0x9794 virtual_34's code (RACE_END_*) */
    /* Volatile: shared with other threads and interrupt-off sections (the
     * stores stay out of delay slots, setField9798 0x0017CC20). */
    volatile int       field_9798;          /* 0x9798 read and cleared with interrupts off */
    volatile int       wakeup;              /* 0x979C signal() / takeSignal() */
    /*  0x97A0 .. 0x97BF: alignment */
    /* PC: 0xE2E0..0xE300 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0xE300, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_E2E0, 0x20)
};
EGCS_SIZE(RaceBase, 0x97C0);

/* The class `name' of the registry: buf == NULL: info->create(), else
 * info->construct(buf, arg); NULL when unknown. */
RaceBase *race_create_by_name(const char *name, void *buf, int arg);    /* 0x0017B8F0 */
/* heap: delete race (null-safe); else race->~RaceBase() in place. */
void      race_destroy(RaceBase *race, int heap);                       /* 0x0017B990 */
/* node in front of the registry; g_00351DF0 = the largest size. */
RaceClassNode *race_class_register(RaceClassNode *node, RaceClassInfo *info);  /* 0x0017B9E8 */

extern int            g_00351DF0;           /* 0x00351DF0 gp-0x5C00 the largest sizeof of a registered class
                                               (int: its users round it up with the signed mask -64,
                                               T000 play_saved_replay and T055) */
extern RaceClassNode *g_00351DF4;           /* 0x00351DF4 gp-0x5BFC the head of the registry */



#endif /* GAME_T129_RACE_BASE_H */
