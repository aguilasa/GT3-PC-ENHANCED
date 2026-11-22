/* T378_onboard_camera.h - OnBoardCamera and CameraShake (translation unit
 * T378, unit u089), 0x001FF788..0x00201418.
 *
 * The cameras mounted on the followed car.  A car file lists its camera
 * mounts (0x40 bytes each: a point, three rotations in degrees, a projection
 * scale and flags).  Mounts 0 and 1 are the two fixed views (a car file with
 * fewer than two mounts uses built-in ones); mount 0 sways with the wheels and
 * the speed, the other mounts get a random head shake (two CameraShakes) that
 * grows with the speed.  The mount buttons of the RaceInput step through the
 * mounts with flag bit 3, look-back turns the view round.  getChangeLevel
 * reports a mount whose point is off the runway or inside another car's
 * collision box (isViewBlocked).  DesignerCamera (T398) derives from it.
 */
#ifndef GAME_T378_ONBOARD_CAMERA_H
#define GAME_T378_ONBOARD_CAMERA_H

#include "game/game.h"
#include "game/camera/T397_variable_camera_base.h" /* base VariableCameraBase */
#include "game/camera/T380_camera_spring.h"        /* CameraSpring (held by value) */
#include "engine/pdistd/T531_random.h"               /* Random (held by value) */

class CameraProjection;                     /* T383 (pointers only) */
class CameraTargetInfo;                     /* T377 */
class CarProgress;                          /* T408 */
class RaceEntryCar;                         /* T121 */
class RaceInput;                            /* T413 */
class RaceOrganization;                     /* T125 */
struct PcCustomSource;                      /* PC: game/app/pc_customcam.h */

/* ? name.  A random head shake, 0xC bytes: a spring (stiffness gain,
 * damping 10, 60 steps a second) pulled to a target that jumps to a new
 * random value (-128..128) with probability 0.4 a frame.  Also ZoomupCamera's
 * pitch / yaw wobble (T406). */
struct CameraShake {
    void reset();                           /* 0x001FF788 all 0 */
    void pickTarget(Random *rng);           /* 0x001FF798 */
    f32  getValue();                        /* 0x001FF818 pos / 128 clamped to -1..1 */
    void update(Random *rng, f32 gain);     /* 0x001FF860 pickTarget, one spring step */

    f32 pos;                                /* 0x0 */
    f32 target;                             /* 0x4 */
    f32 vel;                                /* 0x8 */
};
EGCS_SIZE(CameraShake, 0xC);

/* vtable 0x0033E268 (24 slots: [1], [6], [7] CameraBase's, [3]..[5], [19],
 * [22] VariableCameraBase's; [23] new).  typeinfo __tf13OnBoardCamera
 * 0x002013C8 (__si_type_info 0x004B28C0, base VariableCameraBase).  Size
 * 0x180 (CameraView +0x300; DesignerCamera's data start at +0x180). */
class OnBoardCamera : public VariableCameraBase {
public:
    OnBoardCamera();                        /* 0x001FF8E8 type 0, fov 60 */
    virtual ~OnBoardCamera();               /* [0] +0x0C 0x001FF948 */
    virtual void applyView();               /* [2] +0x1C 0x002004B0 the current matrix times `matrix' */
    virtual void virtual_8(CameraTargetInfo *target);       /* [8] +0x4C 0x001FF970 the target's car, mount 0 */
    virtual int  virtual_9();               /* [9] +0x54 0x002003E8 mount 0: 0, mount 1: 2, else bit 1 of the mount's flags */
    virtual int  getEffect();              /* [10] +0x5C 0x00200450 0 */
    virtual void setProjection(const CameraProjection *proj);  /* [11] +0x64 0x00200458 the projection scaled by the mount's */
    /* Springs, shake and sway from the car's state and the target's speed;
     * the mount buttons; look-back; then the view (applyMountView) into
     * `matrix'.  0 on the first update. */
    virtual int  update(RaceOrganization *org, RaceInput *input, CameraTargetInfo *target, CarProgress *progress);  /* [12] +0x6C 0x001FFA70 */
    virtual int  getMount();                /* [13] +0x74 0x002013A0 */
    virtual int  isTargetDistEnable();      /* [14] +0x7C 0x00201398 0 */
    virtual int  getChangeLevel(RaceOrganization &org, int viewMode);              /* [15] +0x84 0x002010F8 2: isViewBlocked */
    virtual int  isNeedReset(RaceOrganization &org, CameraTargetInfo &target);  /* [16] +0x8C 0x00201390 0 */
    virtual int  isEnableChanging();        /* [17] +0x94 0x00201388 1 */
    virtual int  isEnableTargetChange();    /* [18] +0x9C 0x00201380 1 */
    virtual int  isDrawBackMirror();        /* [20] +0xAC 0x00201378 drawMirror */
    virtual f32  getFadeOutTime();          /* [21] +0xB4 0x00201368 0 */
    virtual void applyMountView(RaceEntryCar *car);         /* [23] +0xC4 0x00200038 the view of the mount on car (current matrix) */

    void setMount(int m);                   /* 0x002004D0 setMountChecked with a car */
    void setBasicMount(int m);              /* 0x00200500 m < 2, else mount 0 */
    int  getMountCount();                   /* 0x00200520 */
    int  getMountDirection(int i);          /* 0x00200538 a direction mask (1 front, 2 rear, 4 / 8 the sides) */
    /* The mounts (from 2) facing entry `target' as seen from this car, with
     * any of anyFlags (or anyFlags 0) and all of allFlags; at most max into
     * out; returns the count. */
    int  findMountsToward(RaceOrganization *org, int target, int *out, int max, int anyFlags, int allFlags); /* 0x002005E8 */
    int  findMountsByDirection(int dirMask, int *out, int max, int anyFlags, int allFlags);              /* 0x00200858 */
    /* mounts 5 and up: the mount's point is off the runway or inside
     * another entry's collision box */
    int  isViewBlocked(RaceOrganization *org);                      /* 0x002009F0 */
    int  isInsideCar(RaceOrganization *org, RaceEntryCar *other);   /* 0x00200E08 mount 0 only */
    void setMountChecked(int m);            /* 0x00201120 mount = m, stepMount(0, 0) */
    /* from mount + step (wrapping), the first mount with any of flags (0:
     * any); mount 0 when none */
    void stepMount(int step, int flags);    /* 0x00201150 */
    void findMountDown(int start, int anyFlags, int allFlags);      /* 0x00201260 */
    void setMountChangeable(int on);        /* 0x002013A8 */
    int  isMountChangeable();               /* 0x002013B0 */
    RaceEntryCar *getCar();                 /* 0x002013B8 */
    void setFov(f32 deg);                   /* 0x002013C0 */
    /* PC: the view button's extra cameras (game/app/pc_detail.h): the next
     * of bumper, chase, cockpit, the car's own mounts; the mount to keep in
     * GameOptions::onboardMount; the cockpit view is shown (mount 0 with the
     * car drawn: RacePS2Base::drawCars) */
    void pcNextView();
    int  pcSavedMount();
    /* PC (2026-11-08): the camera's car is its view's target car (update,
     * first): a target changed without virtual_8 - the split screen's
     * second view, set to car 1 after CameraManager::init attached every
     * view to car 0 - re-attaches it, its springs and shakes started again
     * from that car's state, its mount kept when the car has it. */
    void pcFollowTarget(CameraTargetInfo *target);
    /* 0, or the cockpit view's slot: 1, or 2 a second driver's (the cockpit
     * camera, game/app/pc_headcam.h) */
    int  pcIsCockpit() { return mount == 0 ? pcCockpit : 0; }
    /* PC (2026-11-14): the user's own cameras (game/app/pc_customcam.h).
     * pcSetCustom: custom camera `index' of the car in view (0: none, the
     * mount kept), the mount its type's - 0 onboard, 1 chase: what it is to
     * everything else; pcDescribeView: the view shown as a custom camera
     * would give it (a new camera's start). */
    void pcSetCustom(int index);
    void pcDescribeView(PcCustomSource *out);

    int           mount;                    /* 0x100 the car's camera mount */
    f32           fov;                      /* 0x104 degrees (60) */
    /* 0x108 reverse: 1 looking back; 0x109 mountFlag2: bit 1 of the mount's
     * flags; 0x10A changeable: the mount buttons work; 0x10B drawMirror:
     * isDrawBackMirror (mount 0, not looking back).  8-bit fields of an
     * unsigned int (bit-fields), as CarGetControl's (T061): their sb stores
     * are not char accesses, so later loads of other objects pass them
     * (OnBoardCamera::update 0x001FFA70 and findMountDown 0x00201260 are
     * exact only so); the loads stay lbu. */
    unsigned int  reverse : 8;              /* (no offsetof for a bit-field) */
    unsigned int  mountFlag2 : 8;           /* (no offsetof for a bit-field) */
    unsigned int  changeable : 8;           /* (no offsetof for a bit-field) */
    unsigned int  drawMirror : 8;           /* (no offsetof for a bit-field) */
    CameraSpring  springX;                  /* 0x10C added to the car's x (cockpit mounts) */
    CameraSpring  springY;                  /* 0x118 added to the car's y */
    Random        rng;                      /* 0x124 the shakes' and sways' */
    CameraShake   shakeX;                   /* 0x128 rotate x (cockpit mounts) */
    CameraShake   shakeY;                   /* 0x134 rotate y x 0.8 */
    CameraSpring  swayY;                    /* 0x140 rotate y (mount 0) */
    CameraSpring  swayX;                    /* 0x14C rotate x (mount 0) */
    f32           speedFactor;              /* 0x158 speed / 120 km/h, clamped (mount 0) */
    RaceEntryCar *car;                      /* 0x15C */
    int           carIndex;                 /* 0x160 */
    int           pcCockpit;                /* 0x164 PC: the cockpit view (pcNextView): 1, or 2 a second driver's */
    int           pcCustom;                 /* 0x168 PC (2026-11-14): the custom camera in view: its number among the
                                               car's (pc_customcam.h), 0 none; mount is then 0 or 1 by its type */
    /*  0x16C .. 0x17F: alignment (64) */
    /* PC: 0x16C..0x180 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x180, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_16C, 0x14)
};
EGCS_SIZE(OnBoardCamera, 0x180);


#endif /* GAME_T378_ONBOARD_CAMERA_H */
