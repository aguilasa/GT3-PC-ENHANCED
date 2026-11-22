/* T397_variable_camera_base.h - VariableCameraBase (translation unit T397,
 * unit u088), 0x001FE768..0x001FED38.
 *
 * The base of the race cameras: OnBoardCamera (T378; DesignerCamera T398
 * derives from it), PasslineCamera (T399; StartingCamera T400), AutomaticCamera
 * (T401), AutomountCamera (T402; HeliCamera T404), LoggerCamera (T403),
 * MatrixCamera (T405) and ZoomupCamera (T406).  The CameraManager (T383) keeps
 * one of each per view (CameraView +0x300 .. +0x2480) and runs the view's
 * camera through updateFrame every frame.
 *
 * Slot names (u088, u090, u091; CameraBase's applyProjection / applyView,
 * T410): applyView (slot 2), getEffect (10), setProjection (11), update
 * (12); the first pass had virtual_<slot>.
 *
 * The camera values (gamma, zoom, position, rotate, target length) keep
 * HISTORY = 2 entries: [0] this frame, [1] the previous one.  updateFrame
 * shifts them, counts the giveup timers down, runs the update (slot 12), then
 * takes the eye position from the inverse of the view matrix; when the update
 * returns 0 (the first frame after a set up) the history is overwritten with
 * the new values, so nothing is interpolated from the old view.
 */
#ifndef GAME_T397_VARIABLE_CAMERA_BASE_H
#define GAME_T397_VARIABLE_CAMERA_BASE_H

#include "game/game.h"
#include "game/camera/T410_camera_base.h"          /* base CameraBase */

/* A camera 4-vector (VariableCameraBase's position and rotate history).
 * 16-aligned: updateFrame copies the history entries as whole structs, with
 * ld / sd pairs (a 4-aligned record would be ldl / ldr; u083).  The
 * conversion to f32 * (v[0..3], the VU0 calls) is how the code indexes it:
 * the constructor's element stores through it keep one pointer per array
 * (retail 0x001FE768), written as members they share one.  Defined before
 * the #pragma interface below, so T397 emits no copy of the operator. */
struct alignas(16) CameraVec4 {
    f32 x;                                  /* 0x0 */
    f32 y;                                  /* 0x4 */
    f32 z;                                  /* 0x8 */
    f32 w;                                  /* 0xC */
    operator f32 *() { return &x; }
};
EGCS_SIZE(CameraVec4, 0x10);


class CameraProjection;                     /* T383 (pointers only) */
class CameraTargetInfo;                     /* T377 */
class CarProgress;                          /* T408 (no canonical header): CameraManager +0x278 */
class RaceCourse;                           /* T153 */
class RaceInput;                            /* T413 */
class RaceOrganization;                     /* T125 */

/* vtable 0x0033DF18 (23 slots: [1], [6], [7] CameraBase's; [8], [13]..[18],
 * [20], [21] __pure_virtual).  typeinfo __tf18VariableCameraBase 0x001FECE8
 * (__si_type_info 0x004B28B0, base CameraBase).  vptr +0x00 (CameraBase has
 * no data).
 *
 * The view matrix is 64-byte aligned: nothing reads or writes +0x08..+0x3F,
 * and every camera's size and place is a multiple of 0x40 - VariableCameraBase
 * 0x100 (data to 0xF8), OnBoardCamera 0x180 (data to 0x164), PasslineCamera
 * 0x140 (data to 0x10C), DesignerCamera 0x14C0 (data to 0x148C); in
 * CameraView the onboard camera is at +0x300 after a pointer at +0x2D4. */
class VariableCameraBase : public CameraBase {
public:
    enum { HISTORY = 2 };

    /* trueType and subCounter are not set (the derived constructors call
     * the inline setType()). */
    VariableCameraBase();                                   /* 0x001FE768 */
    virtual ~VariableCameraBase();                          /* [0] +0x0C 0x001FE800 */
    virtual void applyView();                               /* [2] +0x1C 0x001FEA88 the view: the current matrix times `matrix' */
    virtual f32  getPositionX();                            /* [3] +0x24 0x001FEBF8 position[0][0] */
    virtual f32  getPositionY();                            /* [4] +0x2C 0x001FEBF0 */
    virtual f32  getPositionZ();                            /* [5] +0x34 0x001FEBE8 */
    ;                                       /* (keeps the address above off the pure slot below) */
    virtual void virtual_8(CameraTargetInfo *target) = 0;   /* [8] +0x4C pure: set up on the target */
    virtual int  virtual_9();                               /* [9] +0x54 0x001FEA28 3 (? how the followed car is drawn) */
    virtual int  getEffect();                               /* [10] +0x5C 0x001FEA30 effect, sign-extended */
    virtual void setProjection(const CameraProjection *proj); /* [11] +0x64 0x001FEA38 the projection (pgl_frustum), setLodBias(1.428148) */
    /* The update: prints "---- not defined  camera update" here.  0: the
     * first frame after a set up (no interpolation from the last frame). */
    virtual int  update(RaceOrganization *org, RaceInput *input, CameraTargetInfo *target, CarProgress *progress);  /* [12] +0x6C 0x001FE828 */
    ;
    virtual int  getMount() = 0;                            /* [13] +0x74 pure: the car mount, -1: none */
    virtual int  isTargetDistEnable() = 0;                  /* [14] +0x7C pure */
    /* viewMode: the player's view mode (CameraManager::canCut passes it;
     * HeliCamera tests 0 / 3; u091).  0 no change, 1 a change may come, 2
     * change now (u091 CHANGE_LEVEL_NONE / SOFT / NOW). */
    virtual int  getChangeLevel(RaceOrganization &org, int viewMode) = 0;          /* [15] +0x84 pure */
    virtual int  isNeedReset(RaceOrganization &org, CameraTargetInfo &target) = 0; /* [16] +0x8C pure */
    virtual int  isEnableChanging() = 0;                    /* [17] +0x94 pure */
    virtual int  isEnableTargetChange() = 0;                /* [18] +0x9C pure */
    virtual int  isRangeOver();                             /* [19] +0xA4 0x001FEB00 0 */
    ;
    virtual int  isDrawBackMirror() = 0;                    /* [20] +0xAC pure */
    virtual f32  getFadeOutTime() = 0;                      /* [21] +0xB4 pure */
    virtual int  disableCrossOver();                        /* [22] +0xBC 0x001FEAF8 0 */
    /* Slots 23 / 24 are added class by class, with their own arguments:
     * setup [23] (the set up with arguments: Passline / Starting /
     * Automatic / Automount (Heli) / Matrix / Zoomup), applyMountView [23]
     * (OnBoard / Designer), setupOnRecord [24] (Designer / Heli: the set up
     * on a course camera record). */

    /* ? name.  The frame update (CameraManager T383 / T385, T409): the
     * arguments go to update unchanged; the eye position is computed
     * with interrupts off. */
    void  updateFrame(RaceOrganization *org, RaceInput *input, CameraTargetInfo *target, CarProgress *progress); /* 0x001FE850 */
    /* The texture LOD bias for the zoom: gfx_tex_param(PGL_TEXPARAM_LOD_BIAS,
     * (-log2(zoom) - ln 2) * 256), GsContext.lodBias (G001), added to TEX1.K;
     * every camera's setProjection calls it.  (First pass F_001FEAA8.) */
    void  setLodBias(f32 zoom);                             /* 0x001FEAA8 */
    f32   getBankForcibly();                                /* 0x001FEB08 */
    void  setBankForcibly(f32 bank);                        /* 0x001FEB10 */
    int   getGiveupTime() const;                            /* 0x001FEB18 */
    void  setGiveupTime(int t);                             /* 0x001FEB20 (? name) */
    int   getGiveupMode() const;                            /* 0x001FEB28 */
    int   getGiveupState() const;                           /* 0x001FEB30 giveupState (F_001FEB30) */
    void  setGiveup(int state, int mode);                   /* 0x001FEB38 giveupState = state, giveupMode = mode (F_001FEB38) */
    int   getKeepTime() const;                              /* 0x001FEB48 the +0xF4 word (no callers) */
    f32   getTargetLength(int i);                           /* 0x001FEB50 */
    f32   getTargetLength();                                /* 0x001FEB60 */
    const f32 (*getMatrix() const)[4];                      /* 0x001FEB68 */
    f32   getRotateZ(int i);                                /* 0x001FEB70 */
    f32   getRotateY(int i);                                /* 0x001FEB80 */
    f32   getRotateX(int i);                                /* 0x001FEB90 */
    f32   getRotateZ();                                     /* 0x001FEBA0 */
    f32   getRotateY();                                     /* 0x001FEBA8 */
    f32   getRotateX();                                     /* 0x001FEBB0 */
    f32   getPositionZ(int i);                              /* 0x001FEBB8 */
    f32   getPositionY(int i);                              /* 0x001FEBC8 */
    f32   getPositionX(int i);                              /* 0x001FEBD8 */
    f32   getGamma(int i);                                  /* 0x001FEC00 */
    void  setGamma(f32 g);                                  /* 0x001FEC10 */
    int   getDoppler();                                     /* 0x001FEC18 */
    void  setDoppler(bool on);                              /* 0x001FEC20 (setDoppler__18VariableCameraBaseb) */
    void  setEffect(int e);                                 /* 0x001FEC28 */
    int   getTrueType();                                    /* 0x001FEC30 */
    int   getType();                                        /* 0x001FEC38 */
    void  setTrueType(int t);                               /* 0x001FEC40 */
    /* The derived constructors expand it (u091). */
    void  setType(int t);                                   /* 0x001FEC48 type and trueType */
    void  setTargetLength(f32 l);                           /* 0x001FEC58 */
    void  setPositionZ(f32 v);                              /* 0x001FEC60 */
    void  setPositionY(f32 v);                              /* 0x001FEC68 */
    void  setPositionX(f32 v);                              /* 0x001FEC70 */
    void  setRotateZ(f32 v);                                /* 0x001FEC78 */
    void  setRotateY(f32 v);                                /* 0x001FEC80 */
    void  setRotateX(f32 v);                                /* 0x001FEC88 */
    void  setZoom(f32 z);                                   /* 0x001FEC90 */
    f32   getZoom(int i);                                   /* 0x001FEC98 */
    void  updateSubCounter();                               /* 0x001FECA8 */
    void  updateCounter();                                  /* 0x001FECB8 */
    int   getSubCounter();                                  /* 0x001FECC8 */
    int   getCounter();                                     /* 0x001FECD0 */
    void  setSubCounter(int n);                             /* 0x001FECD8 */
    void  setCounter(int n);                                /* 0x001FECE0 */

    /*  vptr                                       0x00 */
    s8    type;                             /* 0x04 the camera kind (getType; the constructors: 0
                                               onboard / logger, 1 designer / passline, 3 starting,
                                               4 automatic, 5 automount / matrix / zoom-up, 6 heli) */
    s8    trueType;                         /* 0x05 getTrueType (setType sets both) */
    s16   effect;                           /* 0x06 setEffect, getEffect (bits 0x2000.. 0x8000: the
                                               course record's effects) */
    /*  0x08 .. 0x3F: alignment */
    alignas(64) f32   matrix[4][4];  /* 0x40 the view matrix (getMatrix) */
    f32   gamma[HISTORY];                   /* 0x80 */
    f32   zoom[HISTORY];                    /* 0x88 1 / tan(half the field of view) */
    CameraVec4 position[HISTORY];           /* 0x90 the eye (x, y, z, 1), from the inverse view */
    CameraVec4 rotate[HISTORY];             /* 0xB0 the angles x, y, z */
    f32   targetLength[HISTORY];            /* 0xD0 the distance to the target */
    int   doppler;                          /* 0xD8 bool; updateFrame sets it, the CameraManager clears
                                               it when the view's camera changes, OnBoardCamera::update on
                                               every view jump (mount change, look-back; u089): no doppler for
                                               one frame after a cut */
    int   counter;                          /* 0xDC 0 until the first update after a set up */
    int   subCounter;                       /* 0xE0 */
    f32   bankForcibly;                     /* 0xE4 */
    int   giveupState;                      /* 0xE8 getGiveupState / setGiveup */
    int   giveupMode;                       /* 0xEC frames, counts down in updateFrame */
    int   giveupTime;                       /* 0xF0 frames, counts down in updateFrame */
    union {
        int         keepTime;               /* 0xF4 getKeepTime (lupo2m name; no callers) */
        RaceCourse *course;                 /* 0xF4 the course of the course cameras (T398 and
                                               T399 store it, read it back) */
    };
    /* The enhanced engine (Lupo 2, cameras_sound.md C5), in GT3's 64-byte
     * alignment after the data (the size and every derived camera's layout
     * stay as they were): the TV camera's zoom factor, 1..4, which the
     * zoom button raises in view mode 0 (CameraManager::updateMode0, T388);
     * DesignerCamera::update multiplies its zoom by it and divides the
     * dampers' range by it.  1.0 by the constructor (L2 0x00215750; Lupo 2
     * +0xE8, which moved its later fields up by 4; linkonce get / set L2
     * 0x002D08F8 / 0x002D0900). */
    f32   zoomMult;                         /* 0xF8 */
    /* PC: 0xFC is tail padding; on GCC / Clang i386 a member there keeps a
     * derived camera's data at +0x100 (MatrixCamera::rng). */
    PC_TAIL_PAD(pcTail_FC, 4)

    f32   getZoomMult() { return zoomMult; }
    void  setZoomMult(f32 m) { zoomMult = m; }
};
EGCS_SIZE(VariableCameraBase, 0x100);

/* The accessors (and the virtual getPosition*(), isRangeOver,
 * disableCrossOver) are inline: none has a caller, the users expand them.
 * Defined in the reverse order of their addresses: egcs emits the #pragma
 * implementation copies in reverse definition order. */
inline void VariableCameraBase::setCounter(int n) { counter = n; }  /* 0x001FECE0 */
inline void VariableCameraBase::setSubCounter(int n) { subCounter = n; }  /* 0x001FECD8 */
inline int VariableCameraBase::getCounter() { return counter; }  /* 0x001FECD0 */
inline int VariableCameraBase::getSubCounter() { return subCounter; }  /* 0x001FECC8 */
inline void VariableCameraBase::updateCounter() { counter = counter + 1; }  /* 0x001FECB8 */
inline void VariableCameraBase::updateSubCounter() { subCounter = subCounter + 1; }  /* 0x001FECA8 */
inline f32 VariableCameraBase::getZoom(int i) { return zoom[i]; }  /* 0x001FEC98 */
inline void VariableCameraBase::setZoom(f32 z) { zoom[0] = z; }  /* 0x001FEC90 */
inline void VariableCameraBase::setRotateX(f32 v) { rotate[0][0] = v; }  /* 0x001FEC88 */
inline void VariableCameraBase::setRotateY(f32 v) { rotate[0][1] = v; }  /* 0x001FEC80 */
inline void VariableCameraBase::setRotateZ(f32 v) { rotate[0][2] = v; }  /* 0x001FEC78 */
inline void VariableCameraBase::setPositionX(f32 v) { position[0][0] = v; }  /* 0x001FEC70 */
inline void VariableCameraBase::setPositionY(f32 v) { position[0][1] = v; }  /* 0x001FEC68 */
inline void VariableCameraBase::setPositionZ(f32 v) { position[0][2] = v; }  /* 0x001FEC60 */
inline void VariableCameraBase::setTargetLength(f32 l) { targetLength[0] = l; }  /* 0x001FEC58 */
inline void VariableCameraBase::setType(int t) { type = (s8)t; trueType = (s8)t; }  /* 0x001FEC48 */
inline void VariableCameraBase::setTrueType(int t) { trueType = (s8)t; }  /* 0x001FEC40 */
inline int VariableCameraBase::getType() { return type; }  /* 0x001FEC38 */
inline int VariableCameraBase::getTrueType() { return trueType; }  /* 0x001FEC30 */
inline void VariableCameraBase::setEffect(int e) { effect = (s16)e; }  /* 0x001FEC28 */
inline void VariableCameraBase::setDoppler(bool on) { doppler = on; }  /* 0x001FEC20 */
inline int VariableCameraBase::getDoppler() { return doppler; }  /* 0x001FEC18 */
inline void VariableCameraBase::setGamma(f32 g) { gamma[0] = g; }  /* 0x001FEC10 */
inline f32 VariableCameraBase::getGamma(int i) { return gamma[i]; }  /* 0x001FEC00 */
inline f32 VariableCameraBase::getPositionX() { return position[0][0]; }  /* 0x001FEBF8 */
inline f32 VariableCameraBase::getPositionY() { return position[0][1]; }  /* 0x001FEBF0 */
inline f32 VariableCameraBase::getPositionZ() { return position[0][2]; }  /* 0x001FEBE8 */
inline f32 VariableCameraBase::getPositionX(int i) { return position[i][0]; }  /* 0x001FEBD8 */
inline f32 VariableCameraBase::getPositionY(int i) { return position[i][1]; }  /* 0x001FEBC8 */
inline f32 VariableCameraBase::getPositionZ(int i) { return position[i][2]; }  /* 0x001FEBB8 */
inline f32 VariableCameraBase::getRotateX() { return rotate[0][0]; }  /* 0x001FEBB0 */
inline f32 VariableCameraBase::getRotateY() { return rotate[0][1]; }  /* 0x001FEBA8 */
inline f32 VariableCameraBase::getRotateZ() { return rotate[0][2]; }  /* 0x001FEBA0 */
inline f32 VariableCameraBase::getRotateX(int i) { return rotate[i][0]; }  /* 0x001FEB90 */
inline f32 VariableCameraBase::getRotateY(int i) { return rotate[i][1]; }  /* 0x001FEB80 */
inline f32 VariableCameraBase::getRotateZ(int i) { return rotate[i][2]; }  /* 0x001FEB70 */
inline const f32 (*VariableCameraBase::getMatrix() const)[4] { return matrix; }  /* 0x001FEB68 */
inline f32 VariableCameraBase::getTargetLength() { return targetLength[0]; }  /* 0x001FEB60 */
inline f32 VariableCameraBase::getTargetLength(int i) { return targetLength[i]; }  /* 0x001FEB50 */
inline int VariableCameraBase::getKeepTime() const { return keepTime; }  /* 0x001FEB48 */
/* 0x001FEB38 */
inline void VariableCameraBase::setGiveup(int state, int mode)
{
    giveupState = state;
    giveupMode = mode;
}
inline int VariableCameraBase::getGiveupState() const { return giveupState; }  /* 0x001FEB30 */
inline int VariableCameraBase::getGiveupMode() const { return giveupMode; }  /* 0x001FEB28 */
inline void VariableCameraBase::setGiveupTime(int t) { giveupTime = t; }  /* 0x001FEB20 */
inline int VariableCameraBase::getGiveupTime() const { return giveupTime; }  /* 0x001FEB18 */
inline void VariableCameraBase::setBankForcibly(f32 bank) { bankForcibly = bank; }  /* 0x001FEB10 */
inline f32 VariableCameraBase::getBankForcibly() { return bankForcibly; }  /* 0x001FEB08 */
inline int VariableCameraBase::isRangeOver() { return 0; }  /* 0x001FEB00 */
inline int VariableCameraBase::disableCrossOver() { return 0; }  /* 0x001FEAF8 */


#endif /* GAME_T397_VARIABLE_CAMERA_BASE_H */
