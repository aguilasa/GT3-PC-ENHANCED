/* T402_automount_camera.h - AutomountCamera and CameraWobble (translation
 * unit T402, unit u090), 0x00204030..0x002047D0.
 *
 * A random mount on the car: set up (setup) with a random offset around
 * it (1..9 out, 3..7 up), a random field of view (55..85 degrees) and bank
 * (-20..20); the update aims it at the car's centre from there, with four
 * wobbling values (CameraWobble) shaking the angles and the place, and
 * follows the car's heading.  getChangeLevel asks for a change while the
 * target is in a zone of mask 1 of the course's replay cameras.  HeliCamera
 * (T404) derives from it.
 */
#ifndef GAME_T402_AUTOMOUNT_CAMERA_H
#define GAME_T402_AUTOMOUNT_CAMERA_H

#include "game/game.h"
#include "game/camera/T397_variable_camera_base.h" /* base VariableCameraBase */
#include "engine/pdistd/T531_random.h"               /* Random (held by value) */

class CameraProjection;                     /* T383 (pointers only) */
class CameraTargetInfo;                     /* T377 */
class CarProgress;                          /* T408 */
class RaceInput;                            /* T413 */
class RaceOrganization;                     /* T125 */

/* ? name.  A wobbling value, 0xC bytes: a spring (stiffness k, damping 10,
 * 60 steps a second) towards a target that jumps to a new random value
 * (-128..128) one frame in ten.  (T378's CameraShake is the same with 0.4.) */
struct CameraWobble {
    void reset();                           /* 0x00204030 all 0 */
    void retarget(Random *rng);             /* 0x00204040 */
    f32  get();                             /* 0x002040C0 value / 128 */
    void update(Random *rng, f32 k);        /* 0x002040D8 retarget, one spring step */

    f32 value;                              /* 0x0 */
    f32 target;                             /* 0x4 -128..128 */
    f32 speed;                              /* 0x8 */
};
EGCS_SIZE(CameraWobble, 0xC);

/* vtable 0x0033E850 (24 slots; [23] new).  typeinfo __tf15AutomountCamera
 * 0x00204780 (__si_type_info 0x004B2910, base VariableCameraBase).  Size
 * 0x180 (CameraView +0x1D00; HeliCamera's data start at +0x180). */
class AutomountCamera : public VariableCameraBase {
public:
    AutomountCamera();                      /* 0x00204160 type 5 */
    virtual ~AutomountCamera();             /* [0] +0x0C 0x002041B0 */
    virtual void applyView();               /* [2] +0x1C 0x00204728 the current matrix times `matrix' */
    virtual void virtual_8(CameraTargetInfo *target);       /* [8] +0x4C 0x002041D8 setup(target, 0) */
    virtual int  virtual_9();               /* [9] +0x54 0x002046A8 3 */
    virtual int  getEffect();              /* [10] +0x5C 0x002046B0 effect */
    virtual void setProjection(const CameraProjection *proj);  /* [11] +0x64 0x002046D0 the projection scaled by zoom */
    /* returns 1 (HeliCamera's calls it) */
    virtual int  update(RaceOrganization *org, RaceInput *input, CameraTargetInfo *target, CarProgress *progress);  /* [12] +0x6C 0x002043E0 */
    virtual int  getMount();                /* [13] +0x74 0x00204778 -1 */
    virtual int  isTargetDistEnable();      /* [14] +0x7C 0x00204770 0 */
    virtual int  getChangeLevel(RaceOrganization &org, int viewMode);              /* [15] +0x84 0x002046B8 inZone: 2, else 0 */
    virtual int  isNeedReset(RaceOrganization &org, CameraTargetInfo &target);  /* [16] +0x8C 0x002046C8 target.changed */
    virtual int  isEnableChanging();        /* [17] +0x94 0x00204768 1 */
    virtual int  isEnableTargetChange();    /* [18] +0x9C 0x00204760 1 */
    virtual int  isDrawBackMirror();        /* [20] +0xAC 0x00204758 0 */
    virtual f32  getFadeOutTime();          /* [21] +0xB4 0x00204748 0 */
    /* Set up (target and t are not used; HeliCamera::setupOnRecord calls it) */
    virtual void setup(CameraTargetInfo *target, f32 t);                       /* [23] +0xC4 0x00204208 */

    Random       rng;                       /* 0x100 seeded from the clock (gfx_perf_counter0) at set up */
    f32          offX;                      /* 0x104 the mount around the car */
    f32          offY;                      /* 0x108 3..7 up */
    f32          offZ;                      /* 0x10C */
    u8           unk_110[0xC];              /* 0x110 ? (not accessed) */
    f32          fov;                       /* 0x11C degrees, 55..85 */
    int          inZone;                    /* 0x120 the target is in a zone of mask 1 (getChangeLevel) */
    CameraWobble wobble[4];                 /* 0x124 the angles x, y, the position x, z */
    /* PC: 0x154..0x180 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x180, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_154, 0x2C)
};
EGCS_SIZE(AutomountCamera, 0x180);


#endif /* GAME_T402_AUTOMOUNT_CAMERA_H */
