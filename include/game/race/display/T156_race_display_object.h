/* T156_race_display_object.h - RaceDisplayObjectBase, the base of the race
 * display's elements, and helpers of the race display (translation unit
 * T156, unit u053), 0x00196490..0x00196910.
 *
 * An element of the head-up display (RaceDisplay, T154): an image pack (the
 * race display's race/<country>/display.imgs, T506) and one image of it
 * (setImage looks a name up), a rectangle (location and size, kept as
 * 16-bit), an alignment per axis (0 left / top, 1 right / bottom, 2 centred:
 * the default; + 4 the wide screen shift; T158 race_align_pos) and an
 * enabled flag (off by default).  Derived:
 * the T157 parts, RaceMiniMap and RaceMeterBase (T154) and the meters of
 * T160..T163.  RaceDisplay::virtual_3 sets the 2D render state
 * (race_display_setup_2d) and calls the draw slot of every enabled element:
 * draw, or drawMeter(buf) for the meters.
 *
 * T156 also has RaceSplitDisplayBase::setTargetDisplay (0x00196750, the
 * Lupo 2 name; u053 only, not declared here).
 *
 * Names: u053 (the owner) and the Lupo 2 symbols (getImagePack ..
 * isEnabled, setAlignment, setSize, setLocation). */
#ifndef GAME_T156_RACE_DISPLAY_OBJECT_H
#define GAME_T156_RACE_DISPLAY_OBJECT_H

#include "game/game.h"

struct ImagePack;                           /* T506_image_pack.h (pointers only) */
struct Tex1Image;                           /* G001_flare.h */
class GSBuffer;                             /* T514_gs_buffer.h (references only) */
class type_info;

/* The typeinfo function (vtable entry 0), declared before the class, for
 * GCC 14 only: egcs generates it itself (in T156, with the vtable).  An
 * earlier extern "C" declaration is the one egcs uses, and then it never
 * defines the function (u053 matching pass); a later one it rejects. */

/* vtable 0x0032E798 (10 slots), typeinfo __tf21RaceDisplayObjectBase
 * 0x001968D0 (user, no bases).  0x13 bytes of data, vptr +0x14; 0x18 bytes
 * (the derived classes' data start at +0x18). */
class RaceDisplayObjectBase {
public:
    /* No image; location and size 0 (virtual calls), alignment 2 / 2, off. */
    RaceDisplayObjectBase();                                /* 0x00196490 */
    /* Calls RaceDisplayObjectBase::virtual_2 directly (not through the
     * vtable). */
    virtual ~RaceDisplayObjectBase();                       /* [0] +0x0C 0x00196510 */
    virtual int  virtual_1();                               /* [1] +0x14 0x00196560 returns 0 */
    virtual void virtual_2();                               /* [2] +0x1C 0x00196568 empty */
    virtual void update();                               /* [3] +0x24 0x00196570 empty: the per-frame update
                                                               (RaceDisplay::virtual_2) */
    virtual void draw();                               /* [4] +0x2C 0x00196578 empty: the draw
                                                               (RaceDisplay::virtual_3) */
    /* The meters' draw (RaceDisplay::virtual_3 passes its GSBuffer on; no
     * implementation reads it).  Here: draw(). */
    virtual void drawMeter(GSBuffer &buf);                  /* [5] +0x34 0x00196580 */
    /* draw() / drawMeter(buf) when enabled.  No callers (RaceDisplay::
     * virtual_3 has them expanded, its isEnabled calls kept).  In-class
     * inline, declared before the setters (u053 data pass): T156 emits its
     * inline members at the end in reverse declaration order, and retail
     * has them after setLocation (0x00196840 / 0x00196890, before the
     * __tf; the frame info's FDE order).  isEnabled is called, not
     * integrated, in their bodies. */
    void drawIfEnabled()                                    /* 0x00196890 */
    {
        if (isEnabled())
            draw();
    }
    void drawIfEnabled(GSBuffer &buf)                       /* 0x00196840 */
    {
        if (isEnabled())
            drawMeter(buf);
    }
    /* The four setters are inline (u053 matching pass); T156 emits their
     * copies for the vtable, nothing calls those directly.  Evidence: the
     * constructor's setRegion(0, 0, 0, 0) is integrated with its two calls
     * virtual, as retail has them (egcs calls a constructor's own virtuals
     * directly, but in the integrated body `this' is setRegion's); T161's
     * tachometers integrate them on their member objects (bar.setRegion(...)
     * etc. are exact); T157's RaceEventDisplay constructor gets retail's EH
     * regions only with them inline.  The sizes are stored as s16. */
    virtual void setLocation(int x, int y)                  /* [6] +0x3C 0x00196830 */
    {
        this->x = x;
        this->y = y;
    }
    virtual void setSize(int w, int h)                      /* [7] +0x44 0x00196820 */
    {
        this->w = w;
        this->h = h;
    }
    virtual void setAlignment(u8 alignX, u8 alignY)         /* [8] +0x4C 0x00196810 */
    {
        this->alignX = alignX;
        this->alignY = alignY;
    }
    virtual void setRegion(int x, int y, int w, int h)      /* [9] +0x54 0x001967A8 */
    {
        setLocation(x, y);
        setSize(w, h);
    }

    /* image = the named image of the pack (image_pack_find), when there is
     * a pack. */
    void setImage(const char *name);                        /* 0x001965A8 */
    ImagePack *getImagePack() const;                        /* 0x00196758 */
    void setImagePack(ImagePack *pack);                     /* 0x00196760 */
    u8   getAlignY() const;                                 /* 0x00196768 */
    u8   getAlignX() const;                                 /* 0x00196770 */
    int  getHeight() const;                                 /* 0x00196778 */
    int  getWidth() const;                                  /* 0x00196780 */
    int  getY() const;                                      /* 0x00196788 */
    int  getX() const;                                      /* 0x00196790 */
    bool isEnabled() const;                                 /* 0x00196798 enabled as it is (lbu only:
                                                               a bool bit-field needs no sltu) */
    void setEnabled(bool on);                               /* 0x001967A0 no callers (the users
                                                               store enabled themselves) */

    ImagePack *imagePack;                   /* 0x00 (u051 / u054 void *; u051 / u052 images) */
    Tex1Image *image;                       /* 0x04 the image (image_pack_find's result; u053 int image,
                                               u051 / u052 imageIndex, u054 RaceImage *) */
    s16        x;                           /* 0x08 */
    s16        y;                           /* 0x0A */
    s16        w;                           /* 0x0C */
    s16        h;                           /* 0x0E */
    u8         alignX;                      /* 0x10 (u051 / u052 alignH) */
    u8         alignY;                      /* 0x11 (u051 / u052 alignV) */
    /* an 8-bit bool bit-field (egcs' bool is 4 bytes; u052's proposal in
     * ref/matching/u053/pending.md, applied by u054): retail's stores of it
     * do not alias other loads, as a u8 (char) store would (RaceDisplay::
     * layoutMeters, RaceEventDisplay::addEvent) */
    bool       enabled : 8;                 /* 0x12 */
    /*  vptr                                   0x14 */
    /* PC: 0x17..0x18 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x18, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_17, 0x1)
};
EGCS_SIZE(RaceDisplayObjectBase, 0x18);

/* The helpers of the race display (no `this'). */
/* pgl_ortho(left, right, bottom, top, -1, 1) (G001), the integers converted
 * to float.  No callers. */
void race_display_set_ortho(int left, int top, int right, int bottom);  /* 0x001965E0 */
/* The render state of the race display's 2D drawing: colour 0x80FFFFFF
 * (opaque white), PGL_CAP_BLEND on, FOG / DEPTH_TEST / ALPHA_TEST /
 * DEST_ALPHA off, blend (Cs - Cd) * As + Cd (0x44), MMIN / MMAG linear, the
 * system font g_002F1968 left aligned at size 1.  RaceDisplay::virtual_3
 * (twice). */
void race_display_setup_2d(void);                                       /* 0x00196630 */
/* The x shift race_align_posf (T158) gives an alignment value on the wide
 * screen (g_002F424C): align 4 (left + shift) -106.66667, 5 (right +
 * shift) +106.66667, else 0 (the whole value is compared).  No callers
 * (T157's displays expand it).  (u053 first pass: race_display_split_offset_x;
 * the argument is an alignment, not a display.) */
f32  race_align_shift_x(int align);                                     /* 0x001966D0 */
/* T158's race_align_pos / race_align_posf (the arguments pass through).  No
 * callers. */
int  race_display_align(int pos, int size, int align);                  /* 0x00196710 */
f32  race_display_alignf(int align, f32 pos, f32 size);                 /* 0x00196730 */


#endif /* GAME_T156_RACE_DISPLAY_OBJECT_H */
