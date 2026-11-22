/* T130_race_ps2_base.h - RacePS2Base, RaceBGMPS2, RaceEntryPS2, the BGM
 * marker data (translation unit T130, unit u044), 0x0017CF30..0x0017E6C0;
 * RacePS2Base's rendering half is T131 (u045), declared here with the class.
 *
 * RacePS2Base is the PS2 layer between RaceBase (T129) and the race modes
 * (RaceBasic, T132, and everything built on it).  It holds the per-race
 * effect managers (ReplayBlur, FlareClass, RaceSmoke, ParticleManager,
 * RigidBodyManager), the error view, the race BGM (RaceBGMPS2), the race
 * ambient sound, the rear-view mirror rectangle and the pad input (RaceInput,
 * attached to the pad manager as slot 0x15), plus two malloc'ed weather
 * buffers (G001).  Its overrides wrap RaceBase's: virtual_8 (start) wires the
 * effect managers into the course and copies the music play list from the
 * game options, virtual_28 / 29 run the managers and the cars' sounds each
 * frame, virtual_35 (end) writes the play list back; render (T131) draws the
 * race view.  virtual_41 is the race loop the game modes call (start, sleep
 * until the race thread signals the end, the screen transition, end; again
 * while endCode == 4).
 *
 * RaceBGMPS2 plays the race music: event 0x14 with code 0x14 takes the next
 * song of the play list (or, in the demo, BGM group 0x22), other codes start
 * track -1 of that group.  Every frame it follows 32 channels of beat
 * markers of the current song: markerTime[ch] is the time to the next
 * marker, markerHit[ch] says that a marker passed this frame (the replay
 * cameras cut on them: CameraManager's changeRequest / nextMarkerTime, T383,
 * through RaceOrganization::getBGM).
 *
 * Every member is held by value, so egcs constructs and destroys them in
 * member order, as the original does (RaceSmoke after FlareClass,
 * RaceAmbientSound after RaceBGMPS2; destroyed in reverse).
 */
#ifndef GAME_T130_RACE_PS2_BASE_H
#define GAME_T130_RACE_PS2_BASE_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/race/race_cars.h"            /* PC: PC_RACE_CARS_MAX */
#include "game/race/core/T121_race_entry.h"           /* base RaceEntryBase */
#include "game/race/core/T129_race_base.h"            /* base RaceBase */
#include "game/sound/T175_race_bgm_base.h"        /* base RaceBGMBase */
#include "game/save/T430_music_play_list.h"      /* MusicPlayList (held by value) */
#include "game/effects/T182_replay_blur.h"          /* ReplayBlur (held by value) */
#include "game/effects/T159_flare.h"                /* FlareClass (held by value) */
#include "game/effects/T173_race_smoke.h"           /* RaceSmoke (held by value) */
#include "game/effects/T165_particle_manager.h"     /* ParticleManager (held by value) */
#include "game/car/physics/T167_rigid_body.h"           /* RigidBodyManager (held by value) */
#include "game/race/display/T185_error_event_view.h"     /* ErrorEventView (held by value) */
#include "game/sound/T177_race_ambient_sound.h"   /* RaceAmbientSound (held by value) */
#include "game/race/core/T413_race_input.h"           /* RaceInput (held by value) */

class type_info;
class GSBuffer;                             /* T514 (pointers / references only: forward declarations) */
class RaceCourse;                           /* T153 */
struct BgmTrack;                            /* T466 */
struct Weather;                             /* G001 */
struct WeatherDrop;                         /* G001 */
struct GadgetShape;                         /* u057 */
struct CarBoxList;                          /* T167 */

/* The typeinfo functions (vtable entry 0).  GCC 14 only: egcs declares
 * __tf<mangled> itself once a class is complete and emits it with the
 * vtable; an earlier declaration would be the one it uses, and then it emits
 * none (rule 8). */

/* ------------------------------------------------------------------------ */
/* MirrorView                                                               */
/* ------------------------------------------------------------------------ */

/* The rear-view mirror's rectangle and zoom (RacePS2Base +0xC6F4; T131
 * drawView sets 200, 20, 240, 64 and 1.3).  ? name (u045).  Size 0x14.
 * Defined before the #pragma interface below: T130 has no out-of-line copy
 * of set(). */
struct MirrorView {
    /* (? name) Inline in the original: every retail site (u018
     * ShortmenuLicenseRender::render 0x00134774, u038 0x0016CCC8, u044
     * setMirrorRect 0x0017E4E0, u045 drawView 0x0017E9E4, u046 0x00183EFC /
     * 0x00184060) computes &mirror (ori 0xC6F4; addu) and stores through it
     * at 0x0..0x10, zoom first.  The parameter order (zoom first) and the
     * store order make u018's render and u044's setMirrorRect exact (u018
     * notes). */
    void set(f32 zoom_, f32 x_, f32 y_, f32 w_, f32 h_)
    {
        x = x_;
        y = y_;
        w = w_;
        h = h_;
        zoom = zoom_;
    }

    f32 x;                                  /* 0x00 */
    f32 y;                                  /* 0x04 */
    f32 w;                                  /* 0x08 */
    f32 h;                                  /* 0x0C */
    f32 zoom;                               /* 0x10 */
};
EGCS_SIZE(MirrorView, 0x14);


/* ------------------------------------------------------------------------ */
/* RaceEntryPS2                                                             */
/* ------------------------------------------------------------------------ */

/* The entry list of the PS2 race modes: a RaceEntryBase with seven inline
 * car slots.  vtable 0x00327E78 (5 slots: [0] its own).  typeinfo
 * __tf12RaceEntryPS2 0x0017E670 (si, base RaceEntryBase).  vptr +0x1C
 * (RaceEntryBase's).  Size 0x3C (operator delete(this, 0x3C)). */
class RaceEntryPS2 : public RaceEntryBase {
public:
    static void *operator new(size_t size);             /* 0x0017CF30 the race pool, 16-byte aligned */
    static void  operator delete(void *p, size_t size); /* 0x0017CF58 */
    RaceEntryPS2();                                     /* 0x0017CF80 capacity 7, cars = slots */
    virtual ~RaceEntryPS2();                            /* [0] +0x0C 0x0017CFC8 */

    RaceEntryCar *slots[PC_RACE_CARS_MAX];  /* 0x20 RaceEntryBase::cars points here (PC: was 7,
                                               game/race/race_cars.h) */
};
EGCS_SIZE(RaceEntryPS2, 0x3C);

/* ------------------------------------------------------------------------ */
/* RaceBGMPS2                                                               */
/* ------------------------------------------------------------------------ */

/* vtable 0x00327E30 (8 slots, all its own).  typeinfo __tf10RaceBGMPS2
 * 0x0017E620 (si, base RaceBGMBase).  vptr +4 (RaceBGMBase's).  Size 0x9B8
 * (RacePS2Base +0xBCF0; the base of RaceLicenseBGM, T144).  The slots keep
 * RaceBGMBase's names (T175, u058). */
class RaceBGMPS2 : public RaceBGMBase {
public:
    static void *operator new(size_t size);             /* 0x0017DC50 the race pool */
    static void  operator delete(void *p, size_t size); /* 0x0017DC78 */
    RaceBGMPS2();                                       /* 0x0017DCA0 no song, resetMarkers(1.0) */
    virtual ~RaceBGMPS2();                              /* [0] +0x0C 0x0017DD28 */
    /* The update: the markers of the song playing (the stream position,
     * pdibgm_get_status, T521), the next song of the play list when one
     * ended. */
    virtual void update();                              /* [1] +0x14 0x0017DDF0 */
    virtual f32  getMarkerTime(int ch);                 /* [2] +0x1C 0x0017E0B8 markerTime[ch] */
    virtual int  getMarkerHit(int ch);                  /* [3] +0x24 0x0017E0C8 markerHit[ch] */
    /* Play: code 0x14 the next song of the play list (the demo: group
     * 0x22), else track -1 of group `code'. */
    virtual void play(int code, int arg);               /* [4] +0x2C 0x0017E160 */
    virtual void pause();                               /* [5] +0x34 0x0017E3C0 */
    virtual void resume();                              /* [6] +0x3C 0x0017E3E0 */
    virtual void stop();                                /* [7] +0x44 0x0017E400 */

    int  isDemoWithoutPreview();            /* 0x0017DD80 */
    void resetMarkers(f32 volume);          /* 0x0017DDB0 */
    f32  getVolume();                       /* 0x0017E0D8 */
    void playTrack(int index, int arg, f32 volume);     /* 0x0017E348 */
    /* Inline (defined after the class; T130's #pragma implementation copies
     * follow __tf11RacePS2Base).  RacePS2Base::virtual_8 / virtual_35 use
     * setPlayList, commitPlayList and getPlayList2. */
    void setVolume(f32 v);                  /* 0x0017E568 */
    void commitPlayList();                  /* 0x0017E570 playlist2 = playlist */
    MusicPlayList *getPlayList2();          /* 0x0017E5C8 */
    void setPlayList(const MusicPlayList &pl);  /* 0x0017E5D0 */

    /*  RaceBGMBase                            0x000 (org, vptr +4) */
    BgmTrack     *song;                     /* 0x008 the track playing (its markers), NULL none */
    int           songId;                   /* 0x00C the stream id seen last frame */
    int           markerIndex[32];          /* 0x010 the next marker of each channel */
    f32           markerTime[32];           /* 0x090 seconds to it */
    int           markerHit[32];            /* 0x110 a marker passed this frame */
    f32           volume;                   /* 0x190 */
    /*  0x194: alignment */
    MusicPlayList playlist;                 /* 0x198 */
    MusicPlayList playlist2;                /* 0x5A8 the one that is played and written back */
};
EGCS_SIZE(RaceBGMPS2, 0x9B8);

/* The accessors: inline, defined after the class in reverse address order
 * (egcs emits the #pragma implementation copies 0x0017E568..0x0017E5D0 in
 * reverse definition order). */
inline void RaceBGMPS2::setPlayList(const MusicPlayList &pl) { playlist = pl; }   /* 0x0017E5D0 */
inline MusicPlayList *RaceBGMPS2::getPlayList2() { return &playlist2; }          /* 0x0017E5C8 */
inline void RaceBGMPS2::commitPlayList() { playlist2 = playlist; }               /* 0x0017E570 */
inline void RaceBGMPS2::setVolume(f32 v) { volume = v; }                         /* 0x0017E568 */

/* ------------------------------------------------------------------------ */
/* RacePS2Base                                                              */
/* ------------------------------------------------------------------------ */

/* vtable 0x00327C68 (56 slots: [0] [2] [3] [8] [27] [28] [29] [31] [33]
 * [35] [36] [37] its own, 41..55 new; [2] and 44..55 are T131's).
 * typeinfo __tf11RacePS2Base 0x0017E518 (si, base RaceBase).  vptr +0
 * (GameObject's).  Size 0xC880 (operator delete(this, 0xC880); RaceBasic's
 * data start there); constructed in place by the race modes. */
class RacePS2Base : public RaceBase {
public:
    static void *operator new(size_t size);             /* 0x0017D020 the race pool (no callers) */
    static void  operator delete(void *p, size_t size); /* 0x0017D048 */
    /* The members; the input attached to the pad manager (slot 0x15), the
     * two weather buffers, bgm = &bgmPS2, race_menu_heap_push (T197). */
    RacePS2Base();                                      /* 0x0017D070 */
    virtual ~RacePS2Base();                             /* [ 0] +0x0C  0x0017D2B8 */
    /* GameObject::render (T131): while renderEnabled, drawView, 45, 46. */
    virtual void render(GSBuffer &buf);                 /* [ 2] +0x1C  0x0017E6C0 */
    /* GameObject slot 3: the course data's course_apply_clut_anims; with two entries,
     * car 1's deferred model (applyPendingModel). */
    virtual void virtual_3();                           /* [ 3] +0x24  0x0017DBE8 */
    /* RaceBase's, then the course gets the smoke, rigid bodies and
     * particles; the weather, the error view, the play lists. */
    virtual void virtual_8(int replay);                 /* [ 8] +0x4C  0x0017D580 */
    /* After virtual_33 (endRequested): endRace(), 0; while bit 29 of the
     * input word: 0; else the error view's frame and RaceBase's. */
    virtual int  virtual_27();                          /* [27] +0xE4  0x0017D8E8 */
    virtual void virtual_28(f32 dt);                    /* [28] +0xEC  0x0017D960 the managers; the afterimage fades */
    virtual void virtual_29(f32 dt);                    /* [29] +0xF4  0x0017DA58 the cars' sounds, sparks and lamps */
    virtual void virtual_31();                          /* [31] +0x104 0x0017DBA8 empty */
    virtual void virtual_33();                          /* [33] +0x114 0x0017DBB0 RaceBase's, endRequested = 1 */
    virtual void virtual_35();                          /* [35] +0x124 0x0017D7E0 the end: the play list written back */
    virtual void startSounds();                         /* [36] +0x12C 0x0017D888 RaceBase's, the ambient sound started */
    virtual void stopSounds();                          /* [37] +0x134 0x0017D8B8 RaceBase's, the ambient sound stopped */
    /* Runs one race to its end (11 callers): virtual_8(replay), virtual_43,
     * the render thread, virtual_38 until signalled, the transition,
     * virtual_35; with retry again while endCode == 4 (the pause menu's
     * retry, RaceBasic::virtual_30). */
    virtual void virtual_41(int replay, int retry);     /* [41] +0x154 0x0017D3D8 */
    virtual int  virtual_42();                          /* [42] +0x15C 0x0017D4C8 restarts the race when signalled */
    virtual void virtual_43();                          /* [43] +0x164 0x0017D7D8 empty: before the race runs */
    /* T131 (u045's definitions; slot names u045's, virtual_44 / 47..55
     * before the sweep): */
    virtual void drawView(GSBuffer *buf);               /* [44] +0x16C 0x0017E740 the race view into buf */
    virtual void virtual_45(GSBuffer *buf);             /* [45] +0x174 0x0017EFC8 empty: the 2D overlays */
    virtual void virtual_46(GSBuffer *buf);             /* [46] +0x17C 0x0017EFD0 the error view */
    virtual void drawSubView(GSBuffer *buf, int sub, f32 zoom);    /* [47] +0x184 0x0017F040 a view into a sub-rectangle */
    virtual void drawCourse();                          /* [48] +0x18C 0x0017F610 the course, main vision list */
    /* The reflection on the road: the cars mirrored in the ground
     * (drawCarReflections), their lamps' reflections (drawLampReflections),
     * the buffer blurred and blended over the screen (u045; not the
     * shadows). */
    virtual void drawReflections(GSBuffer *screen, GSBuffer *envBuf);  /* [49] +0x194 0x0017FB68 */
    /* The cars, farthest first: lod the CarModel LOD (-1 by distance), tyreBlur
     * the tyres' motion blur; the shadows per light pass (u045). */
    virtual void drawCars(GSBuffer *envBuf, int lod, int tyreBlur, f32 t);    /* [50] +0x19C 0x0017F730 */
    virtual void drawCarReflections(GSBuffer *envBuf, f32 t);  /* [51] +0x1A4 0x0017FFE8 the cars mirrored into the reflection buffer */
    virtual void drawLampReflections(f32 t);            /* [52] +0x1AC 0x00180218 the lamps' reflections (FlareClass::beginReflections ...) */
    virtual void drawMirror(GSBuffer *screen, int sub); /* [53] +0x1B4 0x00180330 the rear-view mirror */
    virtual void addCarLamps(f32 t);                    /* [54] +0x1BC 0x001808E8 the cars' lights into the flare */
    virtual void addCourseLights();                     /* [55] +0x1C4 0x001809E8 the course lights into the flare */

    /* Inline (defined after the class; T130's #pragma implementation copies
     * have no callers).  virtual_8 takes the smoke, particles and rigid
     * bodies through getSmoke / getParticles / getRigidBodies. */
    int  getEndRequested();                 /* 0x0017E458 (u044 getField97C0) */
    FlareClass       *getFlare();           /* 0x0017E468 */
    RigidBodyManager *getRigidBodies();     /* 0x0017E478 */
    ParticleManager  *getParticles();       /* 0x0017E488 */
    RaceSmoke        *getSmoke();           /* 0x0017E498 (u044 getObjBC30) */
    void setAfterimageTime(f32 t);          /* 0x0017E4A8 (u044 setTimeScale) */
    /* A w x h rectangle centred on (cx, cy) (signed halving) and the zoom
     * (MirrorView::set; u038's LoggerRaceReplay::renderRace expands it). */
    void setMirrorRect(int cx, int cy, int w, int h, f32 zoom);    /* 0x0017E4B8 (u044 setViewRect) */
    /* T131: the frame captured into three planes; afterimage =
     * max(1 - (1/60) / afterimageTime, 0).  The enhanced engine: and the
     * field mode it was taken in (D1). */
    void captureAfterimage(GSBuffer *buf);  /* 0x00180AE0 */
    /* T131: while afterimage > 0 the planes drawn over buf, alpha
     * afterimage^3.  The enhanced engine (D1): not in background mode, and
     * shifted half a line when the field differs from the capture's. */
    void drawAfterimage(GSBuffer *buf);     /* 0x00180BF8 */

    /*  RaceBase                               0x0000 (vptr +0) */
    int              endRequested;          /* 0x97C0 1 after virtual_33: the next virtual_27 ends the race */
    Weather         *weather;               /* 0x97C4 malloc(0x40) (weather_init / update / draw, G001) */
    WeatherDrop     *weatherDrops;          /* 0x97C8 malloc(0x6400) */
    ReplayBlur       blur;                  /* 0x97CC (u044 T182Object) */
    FlareClass       flare;                 /* 0x9830 */
    RaceSmoke        smoke;                 /* 0xBC30 RaceCourse::smoke points here */
    ParticleManager  particles;             /* 0xBC90 */
    RigidBodyManager rigidBodies;           /* 0xBCA4 */
    u8               unk_BCBC[4];           /* 0xBCBC  ? (RigidBodyManager is 0x18: h28) */
    int              field_BCC0;            /* 0xBCC0 0 */
    f32              afterimage;            /* 0xBCC4 the afterimage's strength, 1 .. 0 (u044 timer) */
    f32              afterimageTime;        /* 0xBCC8 its duration, s (u044 timeScale) */
    u8               frameBits;             /* 0xBCCC (x << 1) | 1 every drawn frame: 0 = the first */
    /* The enhanced engine (race_logic.md D1): the GS field mode the
     * afterimage was captured in (gs_get_field_mode, PGL_FIELD_*), in GT3's
     * alignment byte (Lupo 2 +0x1BF68). */
    u8               afterimageField;       /* 0xBCCD */
    /*  0xBCCE: alignment */
    ErrorEventView   errorView;             /* 0xBCD0 */
    RaceBGMPS2       bgmPS2;                /* 0xBCF0 RaceBase::bgm points here (u044 ps2Bgm) */
    RaceAmbientSound ambientSound;          /* 0xC6A8 */
    MirrorView       mirror;                /* 0xC6F4 (u044 / u046 viewRect[5]) */
    RaceInput        input;                 /* 0xC708 the pad manager's slot 0x15 */
    /*  0xC850 .. 0xC87F: alignment */
    /* PC: 0x204E0..0x20500 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x20500, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_204E0, 0x20)
};
EGCS_SIZE(RacePS2Base, 0xC880);

/* The accessors: inline, defined after the class in reverse address order
 * (egcs emits the #pragma implementation copies 0x0017E458..0x0017E4B8 in
 * reverse definition order). */
inline void RacePS2Base::setMirrorRect(int cx, int cy, int w, int h, f32 zoom)   /* 0x0017E4B8 */
{
    mirror.set(zoom, (f32)(cx - w / 2), (f32)(cy - h / 2), (f32)w, (f32)h);
}
inline void RacePS2Base::setAfterimageTime(f32 t) { afterimageTime = t; }       /* 0x0017E4A8 */
inline RaceSmoke *RacePS2Base::getSmoke() { return &smoke; }                     /* 0x0017E498 */
inline ParticleManager *RacePS2Base::getParticles() { return &particles; }       /* 0x0017E488 */
inline RigidBodyManager *RacePS2Base::getRigidBodies() { return &rigidBodies; }  /* 0x0017E478 */
inline FlareClass *RacePS2Base::getFlare() { return &flare; }                    /* 0x0017E468 */
inline int RacePS2Base::getEndRequested() { return endRequested; }               /* 0x0017E458 */

/* ------------------------------------------------------------------------ */
/* Data                                                                     */
/* ------------------------------------------------------------------------ */

/* (RacePS2Base::virtual_8 sets RigidBodyManager's statics g_00352174 /
 * g_00352178 / g_0035217C through T167's inline SetGadgetShapes /
 * SetCarBoxes / SetCourse;
 * __ti11RaceBGMBase, which the plan put here, is in T175_race_bgm_base.h.) */


#endif /* GAME_T130_RACE_PS2_BASE_H */
