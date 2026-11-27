/* race_input_internal.h - unit u095 of the GT3 A-Spec US main executable (SCUS-97102),
 * 0x002104E8..0x00213210.
 *
 * Translation units (link order):
 *   T413  RaceInput: the input of one player in a race (a Control, T474).
 *   T414  RaceRandom: the race random generator at gp-0x47A0 (? name).
 *   T415  MachineTestRecords (GameStatus+0x2E560) and RankingCode.
 *   T416  FreeRunRecords (GameStatus+0x26160).
 *
 * The classes and functions are declared in the canonical headers
 * include/game/T413..T416 and in the headers of the units they use.  This
 * header keeps only what is local to the unit: named constants for the bit
 * masks and codes the code tests, and an accessor of Control::type.
 *
 * Data (the data pass, ref/data/u095/notes.md): each source includes the
 * canonical headers it needs itself, in the order that gives retail's
 * .rodata (T413: <new>'s "bad_alloc" first, then its jump table, vtable and
 * the type names 9RaceInput, 14RaceCameraBase, t9UnitArena1Ui64 and the
 * garage's; T414: no .rodata; T415 / T416: their literals only).  So this
 * header includes nothing that emits a type name or a literal (checked with
 * tools/hdrnames.py --each): Control (T474) for the accessor below only.
 *
 * egcs ABI reminder: the vptr is a data member placed after the data members
 * of the first polymorphic class (Control: +0xC0, Manager: +0x5C).  A modern
 * g++ build puts it at +0x00; the offsets in the comments are the original
 * ones.
 */
#ifndef U095_H
#define U095_H

#include "types.h"
#include "libc.h"                               /* strcpy, strlen, memcpy */
#include <stddef.h>
#include "engine/input/T474_control.h"                  /* Control (raceinput_usbType) */

/* ======================================================================== */
/* T413: Control::type, the pad / wheel kinds                               */
/* ======================================================================== */

/* RaceInput reads Control::type (T474: bits 0-4 the PS pad type PS_PAD_*,
 * bits 5-7 the USB wheel type USB_WHEEL_*) through the inline rawPSType
 * (lw, andi 0x1F) and rawUSBType (lbu, dsrl 5), and in emulatePressure /
 * readWheel as the int it is: sra 5, andi 7. */
static inline int raceinput_usbType(const Control *c) { return (c->type >> USB_TYPE_SHIFT) & 7; }

/* The one PS type besides T474's that emulatePressure tells apart (? which
 * device: it skips the pad's bits but takes raw[1] for the wheel). */
enum { PS_PAD_TYPE_31 = 31 };

/* The ControllerConfig modes (CONFIG_*) and functions (KEY_FUNC_*): T426. */

/* The raw PS pad bits (Control::curButtons.raw[0]): T474's PADRAW_*. */

/* RaceInput's button word (Control::curButtons.buttons after
 * emulatePressure): the low byte of Control's GT3 buttons, then these. */
enum {
    RIB_STEER_LEFT  = 0x100,                    /* d-pad left */
    RIB_STEER_RIGHT = 0x200,                    /* d-pad right */
    RIB_THROTTLE    = 0x400,                    /* the bits of g_002DFB18 (functions 1..8) */
    RIB_BRAKE       = 0x800,
    RIB_HANDBRAKE   = 0x1000,
    RIB_THROTTLE2   = 0x2000,
    RIB_SHIFT_UP    = 0x4000,
    RIB_SHIFT_DOWN  = 0x8000,
    RIB_FUNC7       = 0x10000,
    /* Replay modes 1..3 only: the pad / wheel button that sets each (? the
     * replay screen's controls). */
    RIB_REPLAY_RIGHT  = 0x40000,
    RIB_REPLAY_LEFT   = 0x80000,
    RIB_REPLAY_UP     = 0x100000,
    RIB_REPLAY_DOWN   = 0x200000,
    RIB_REPLAY_CROSS  = 0x400000,
    RIB_REPLAY_CROSS2 = 0x2000000,              /* cross sets both */
    RIB_REPLAY_CIRCLE = 0x4000000,
    RIB_REPLAY_R1     = 0x8000000,
    RIB_REPLAY_L1     = 0x10000000,
    /* The raw buttons exactly left + triangle + L2 + R2 (? a debug combination). */
    RIB_COMBO_8013    = 0x20000000,
    /* The enhanced engine (cameras_sound.md C5, ENH_CAMERA_ZOOM_BUTTON): the
     * replay's TV-camera zoom (CAMBTN_ZOOM, camera_modes_internal.h).
     * Lupo 2's is 0x00800000, which is GT3's view-mode bit here. */
    RIB_REPLAY_ZOOM   = 0x40000000
};
#define PADRAW_COMBO_8013 (PADRAW_LEFT | PADRAW_TRIANGLE | PADRAW_R2 | PADRAW_L2)

/* CarInput::flags (T342): the value is analog. */
enum {
    CARINPUT_ANALOG_STEER     = 0x01,
    CARINPUT_ANALOG_THROTTLE  = 0x02,
    CARINPUT_ANALOG_BRAKE     = 0x04,
    CARINPUT_ANALOG_THROTTLE2 = 0x08,
    CARINPUT_ANALOG_HANDBRAKE = 0x10
};

/* CarInput::device (T342): the steering source. */
enum {
    STEER_DIGITAL  = 0,
    STEER_ANALOG   = 2,
    STEER_PRESSURE = 3,
    STEER_WHEEL    = 4,
    STEER_WHEEL_DFP = 6,                        /* the enhanced engine: the Driving Force Pro (backend.md B1,
                                               physics.md 19: full lock, extrapolated input) */
    STEER_WHEEL_DIRECT = 7                      /* PC (2026-11-20): a wheel, its steering filter off (T342's
                                               INPUT_DEV_WHEEL_DIRECT) */
};

/* Control::channels (T474's CHAN_*): with a pad and a wheel, the wheel's
 * channels start at CHAN_PAD_COUNT (0x10). */

/* RaceInput::replayMode values tested here. */
enum {
    REPLAY_LIVE     = 0,
    REPLAY_RECORDED = 3                         /* (? live, as 0) */
};

/* ======================================================================== */
/* T415 / T416: the ranking tables                                          */
/* ======================================================================== */

/* An empty place's time (T128 defines the same value). */
#ifndef RACE_TIME_INVALID
#define RACE_TIME_INVALID 0x157529FF
#endif

#define RANKING_PLACES     12
#define RANKING_CODE_EMPTY "------------------------------"   /* 30 dashes */
#define FREE_RUN_COURSES   0x2C

#endif /* U095_H */
