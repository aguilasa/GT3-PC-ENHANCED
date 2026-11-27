/* T413_race_input.h - RaceInput, the input of one player in a race
 * (translation unit T413, unit u095), 0x002104E8..0x00211A00.
 *
 * A Control (T474) whose slot-9 hook turns the pad / USB wheel buttons into
 * function bits through the controller settings (GameOptions::config, T426):
 * mode 0 digital / 1 analog pad, 2 / 3 by USB wheel type.  read() fills the
 * car's CarInput from the pad (by pad type) and the wheel, then records it
 * into `rec' (live), or plays the record back (replay / ghost); it also
 * keeps the lap records (bestLap / lap, u084) and drives the pad actuators.
 * The derived classes are RaceLinkBattleInput, RaceLinkBattleNodeInput (T138)
 * and gt2k_menu::LoggerInput (T055). */
#ifndef GAME_T413_RACE_INPUT_H
#define GAME_T413_RACE_INPUT_H

#include "game/game.h"
#include "engine/input/T474_control.h"                  /* base Control */
#include "game/race/dynamics/T318_control_record.h"           /* AutomobileControlRecord::Manager (by value) */

class CarInput;                                 /* pointers only: forward declarations */
class OneLapGhostInfo;                          /* T313_dc_free_run.h, 0x1B00 bytes */
class PadVibration;
class ControllerConfig;

/* The typeinfo function (vtable entry 0) builds the __si_type_info at
 * 0x004B2990 (name 0x0033F840 "9RaceInput", base __ti7Control 0x00353478) on
 * the first call.  egcs generates it itself (and rejects a hand
 * declaration). */

/* vtable 0x0033F7E0 (11 slots): [0] its destructor, [1..8] Control's, [9]
 * its emulatePressure override (0x00210AA0), [10] getControllerConfig (its own).
 * typeinfo __tf9RaceInput 0x002119B0 (si, base Control).  vptr +0xC0
 * (Control's).  Size 0x148 (RaceSplitBattle's array of them: stride 0x148). */
class RaceInput : public Control {
public:
    RaceInput();                                /* 0x002104E8 */
    /* Unlike the default constructor it leaves lastReplayMode, port and
     * snapshots unset (the static g_002DFB58 is built with 0). */
    RaceInput(int enable);                      /* 0x00210578 */
    virtual ~RaceInput();                       /* [0] +0x0C 0x00210608 rec.~Manager(), then ~Control */
    /* Control::emulatePressure, then the buttons word (Control +0x2C): the
     * low byte of getButtons(), the pad's function bits, the wheel's, and in
     * replay modes 1..3 extra bits from the raw buttons. */
    virtual void emulatePressure();             /* [9] +0x54 0x00210AA0 */
    virtual ControllerConfig *getControllerConfig(); /* [10] +0x5C 0x00210A78 the controller settings (GameStatus options.config) */

    void setReplayMode(int mode);               /* 0x00210658 also rec.playback = (mode != 0) */
    /* The enhanced engine (backend.md B2, race_logic.md C5; Lupo 2 L2
     * 0x0021CE60): the race state for the controller - 0 racing, 1 paused or
     * out of the race; the port's wheel spring follows it (on with 1).
     * Callers: RaceBase::setPausedState, RacePS2Base::virtual_41. */
    void setMode(int mode);
    /* PC (2026-11-20): a live race begins - the notice when this input's
     * player plays with the wheel and GT3's own Force Feedback option is off
     * (game/app/pc_ffb.h).  Caller: RacePS2Base::virtual_41. */
    void pcFfbNotice();
    void resetLap(u32 startTime);               /* 0x00210668 lap->reset(); startTime (ghost_record_start: (clock + 50) / 3) is not read */
    int  isLapFinished();                       /* 0x00210690 */
    void recordLap(const CarInput *in);         /* 0x002106B0 */
    void stopLap();                             /* 0x002106D8 */
    void keepLap();                             /* 0x00210700 the lap's record becomes the best one (live only) */
    /* Live (mode 0 or 3): reads the pad and the wheel and records the frame;
     * otherwise plays the next recorded frame unless held. */
    void read(CarInput *in);                    /* 0x00210740 */
    void sendVibration(const PadVibration *v);  /* 0x002107D8 */
    void readPressureSteer(CarInput *in);       /* 0x00210DA8 */
    void readAnalogSteer(CarInput *in, ControllerConfig *cfg, int mode, int port);     /* 0x00210E68 */
    void readAnalogButtons(CarInput *in, ControllerConfig *cfg, int mode, int port);   /* 0x00211050 */
    void readPressureButtons(CarInput *in, ControllerConfig *cfg, int mode, int port); /* 0x002111C0 */
    int  isAnalogPressed(ControllerConfig *cfg, int mode, int port, int func);         /* 0x00211330 */
    void readAnalogShift(CarInput *in, ControllerConfig *cfg, int mode, int port);     /* 0x00211398 */
    void readPad(CarInput *in);                 /* 0x00211430 */
    void readWheel(CarInput *in);               /* 0x002115F8 */

    /* maxFrames .. isEnabled (0x002117A0..0x002119A8) are inline (u047
     * matching): none of their copies has a caller, they are the inline
     * copies egcs emits at the end of T413, and RaceSolitaire's constructor
     * (T139) reaches &inputN through them (getRecord, setRecordMode, setHold,
     * setLap, setBestLap).  u095's T413 keeps comment-only FUNC blocks.
     * Declared from the last copy (isEnabled) to the first (maxFrames):
     * egcs writes a class's in-class inline members in reverse declaration
     * order (DATA.md), and T413's frame info (0x002DFB18..) has retail's
     * copies in address order (u095's data pass). */
    int  isEnabled() { return enabled; }        /* 0x002119A8 */
    int  getHold() { return hold; }             /* 0x002119A0 */
    int  getReplayMode() { return replayMode; } /* 0x00211998 */
    void setRecordMode(int mode) { rec.setMode(mode); }   /* 0x00211990 */
    int  getRecordMode() { return rec.mode; }   /* 0x00211988 */
    void setVibration(int on) { vibration = on; }   /* 0x00211980 */
    int  getVibration() { return vibration; }   /* 0x00211978 */
    void setHold(int h) { hold = h; }           /* 0x00211970 */
    void setPort(int p) { port = p; }           /* 0x00211968 */
    AutomobileControlRecord::Manager *getRecord() { return &rec; }  /* 0x00211960 */
    void attachBuffer(int i) { rec.attachBuffer(i); }           /* 0x00211940 */
    void reset() { rec.reset(); }               /* 0x00211920 */
    void play(CarInput *out) { rec.rec.play(out); }             /* 0x00211900 */
    void skip(int n, int keep) { rec.rec.skip(n, keep); }      /* 0x002118E0 */
    void record(const CarInput *in) { rec.rec.record(in); }     /* 0x002118C0 */
    void stop() { rec.rec.stop(); }             /* 0x002118A0 */
    int  isFinished() { return rec.rec.finished; }  /* 0x00211898 */
    u32  getFrames() { return rec.rec.frames; } /* 0x00211890 */
    u32  frameCount() { return rec.rec.frameCount(); }  /* 0x00211870 the Recorder's */
    u8  *getSlot() { return rec.getSlot(); }    /* 0x00211850 */
    const u32 *load(const u32 *src, int multi) { return rec.load(src, multi); }    /* 0x00211830 */
    void save(int *buf, int multi) { rec.save(buf, multi); }    /* 0x00211810 rec.save(buf, multi) */
    int  saveSize(int multi) { return rec.saveSize(multi); }    /* 0x002117F0 rec.saveSize(multi) (not read there) */
    void setBestLap(AutomobileControlRecord::Manager *m) { bestLap = m; }   /* 0x002117E8 */
    AutomobileControlRecord::Manager *getBestLap() { return bestLap; }      /* 0x002117E0 */
    void setLap(AutomobileControlRecord::Manager *m) { lap = m; }           /* 0x002117D8 */
    AutomobileControlRecord::Manager *getLap() { return lap; }              /* 0x002117D0 */
    /* Lupo 2 (by position): setSnapshots, getSnapshot, getSnapshots are
     * setLoggerSector(OneLapGhostInfo *), getLoggerSector(int),
     * getLoggerSector(). */
    OneLapGhostInfo *getSnapshots() { return snapshots; }       /* 0x002117C8 */
    OneLapGhostInfo *getSnapshot(int n)         /* 0x002117B0 &snapshots[n] (0x1B00 bytes each) */
        { return (OneLapGhostInfo *)((u8 *)snapshots + n * 0x1B00); }
    void setSnapshots(OneLapGhostInfo *p) { snapshots = p; }    /* 0x002117A8 */
    static int maxFrames() { return 36000; }    /* 0x002117A0 36000: 10 minutes of frames at 60 Hz (Lupo 2: getLoggerStatusLimit) */

    AutomobileControlRecord::Manager  rec;      /* 0x0C4 the record of the race (replay) */
    AutomobileControlRecord::Manager *bestLap;  /* 0x124 the best lap's record (u084) */
    AutomobileControlRecord::Manager *lap;      /* 0x128 the record of the lap being driven (u084) */
    OneLapGhostInfo *snapshots;                 /* 0x12C the car's sector records (T313; RaceMemPool bufs[].sectors) */
    int  replayMode;                            /* 0x130 0: live, 3: live (? recorded), else playback (u089 / u092 test 1..3) */
    int  lastReplayMode;                        /* 0x134 */
    int  vibration;                             /* 0x138 actuators 0 / 1 on */
    int  enabled;                               /* 0x13C (? u085: 1 a human player) */
    int  hold;                                  /* 0x140 non-zero: playback paused (? name; u047: the input does not drive its car) */
    int  port;                                  /* 0x144 the player (0 / 1) */
};
EGCS_SIZE(RaceInput, 0x148);

/* A disabled RaceInput (RaceInput(0)), the inert input RaceBase::virtual_26
 * returns (u047; u043 typed it CarDynamics). */
extern RaceInput g_002DFB58;                    /* 0x002DFB58 */


/* The static destructor / constructor of g_002DFB58 (.dtors entry
 * 0x0028F240, .ctors entry 0x0028F168, 0x00211750 / 0x00211778) are egcs's
 * _GLOBAL_$D$g_002DFB18 / _GLOBAL_$I$g_002DFB18: T413 defines the object
 * (u095 matching). */

/* T413's helpers (no callers outside it). */
class FuncBit {                                 /* an entry of g_002DFB18.  Size 8. */
public:
    int func;                                   /* 0x0 the configured function 1..8 */
    u32 bit;                                    /* 0x4 its button bit: 0x400, 0x800, 0x2000, 0x1000, 0x4000, 0x8000, 0x10000, 0x20000 */
};
EGCS_SIZE(FuncBit, 8);
extern FuncBit g_002DFB18[8];                   /* 0x002DFB18 (.data: not const in retail) */

/* d-pad left / right -> 0x100 / 0x200, each configured function's button ->
 * its bit; raw == 0x8013 adds 0x20000000. */
u32  raceinput_padButtons(ControllerConfig *cfg, int mode, int port, u32 raw);   /* 0x002108F0 */
u32  raceinput_wheelButtons(ControllerConfig *cfg, int mode, int port, u32 raw); /* 0x002109C0 */
void raceinput_digitalSteer(CarInput *in, u32 bits);    /* 0x00210D68 */
void raceinput_digitalButtons(CarInput *in, u32 bits);  /* 0x00210EF8 */
int  raceinput_analogValue(int idx, int value, int scale); /* 0x00210F90 */
int  raceinput_wheelPedal(int value, int deadZone);     /* 0x002115D8 */

#endif /* GAME_T413_RACE_INPUT_H */
