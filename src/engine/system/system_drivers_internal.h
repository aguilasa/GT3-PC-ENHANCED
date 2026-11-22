/* system_drivers_internal.h - unit u123 of the GT3 main executable, 0x00265468..0x00268ED8.
 *
 * The middle of the no-FDE stretch G002 (plain C, no exceptions).  The
 * unit's first two functions (0x00265468..0x00265530, the IPU image
 * decoder's fromIPU DMA and IPU INTC handlers) are in u122's
 * ipu_image.c: one object with the decoder
 * (ref/matching/u122/notes.md, "Merge (G002 ipu image)").  Files:
 *   usbd.c       EE side of Polyphony's USB device manager: the
 *                          "PUSB" SIF RPC server the IOP driver reports
 *                          attach / detach / error events to, a vblank-woken
 *                          dispatcher thread, class-driver registration
 *   pjoy.c       "PJOY" SIF RPC client: the USB joystick (wheel)
 *                          class driver (attach states, device info, input
 *                          state per port, queued force-feedback commands),
 *                          Logitech classic force-feedback command builders,
 *                          and the USB class / logical-port mapper
 *   mcard_io.c   blocking libmc wrappers under one lock (bootstrap
 *                          src/mcard_io.cpp, 9 functions shared)
 *   scf_lock.c   two libscf clock conversions under a lock
 *   sound_driver.c      the start of the sound sequencer / SPU2 voice
 *                          manager (continued in u124): voice handle
 *                          operations, the engine-sound crossfader (ENGN
 *                          banks), bank relocation (ENGN / INST / SEQG),
 *                          note -> pitch, sequence players, note-on
 *
 * The canonical header engine/media/G002_mpeg_frameq.h declares what other units
 * call and the sound types G002 shares (SndVoice, SndVoiceBank,
 * SndVoiceParam, SndRamp, InstZone, InstProgram, SeqTrack, Sequence,
 * SeqChannel, SeqSystem, their globals).  game/sound/T468_gtsound.h has the
 * engine-sound voice (GtSoundVoice, EngineLayer: the first pass's
 * EngineSound), engine/pdi/T520_pdispu2.h the SPU2 state block and its setters.
 * This header keeps what only the unit uses: the layouts of the types G002
 * forward-declares for it, the USB / PJOY records, the unit's own data and its
 * internal functions.
 */
#ifndef U123_H
#define U123_H

#include "types.h"
#include <stdarg.h>                     /* va_list (snd_set_reverb) */
#include "eekernel.h"                   /* threads, semaphores, INTC handlers, SemaParam / ThreadParam */
#include "sif.h"                        /* sceSif* RPC types, sceSifRpcLoop */
#include "libmc.h"                      /* sceMc* calls and results */
#include "libcdvd.h"                    /* sceCdCLOCK */
#include "engine/media/G002_mpeg_frameq.h"      /* own translation unit */
/* _gp with a size, declared before G000's unsized declaration (as app_internal.h,
 * file_loader_internal.h, mpegif_internal.h): egcs then counts it as small data (-G8) and loads &_gp
 * with one `la' kept in source order, as retail's usbd_init has it. */
#ifdef __cplusplus
extern "C" char _gp[4];                 /* 0x003579F0, the linkage of G000's declaration */
#else
extern char _gp[4];                     /* 0x003579F0 */
#endif
#include "runtime/startup/G000_collect2_ctors.h"   /* _gp */
#include "sdk/libcdvd/G004_libcdvd.h"          /* sceMcWrite, sceMcSync, sceScfGet*fromRTC (core addresses) */
#include "runtime/newlib/G007_stdio.h"            /* sceSifBindRpc, wait_vblank_start, memset, kernel calls */
#include "game/sound/T468_gtsound.h"          /* GtSoundVoice, EngineLayer (C part) */
#include "engine/pdi/T520_pdispu2.h"          /* SpuEffectAttr, SpuState, spu_state_* */
#include "engine/pdi/T528_sif_call_rpc.h"     /* sif_call_rpc */
#include "engine/pdistd/T532_heap.h"             /* malloc, free */

#ifdef __cplusplus
extern "C" {
#endif

#define U123_SYNC() __asm__ volatile("sync")
#define U123_EI()   EI()  /* PC: ei */

/* The lists of the USB manager and the class mapper are BSD <sys/queue.h>
 * tail queues (TAILQ: next + address of the previous next pointer; STAILQ:
 * next only), with the macros' statement order and their `do { } while (0)'
 * (the loop notes keep retail's store order: usbd_init, usbd_register_driver).
 * Here a head is a pair of fields (first, last = address of the last next
 * pointer) and an entry the fields `next' / `prevp'. */
#define UQ_INIT(first, last) do {                                       \
        (first) = NULL;                                                 \
        (last) = &(first);                                              \
    } while (0)
#define TQ_INSERT_TAIL(last, elm) do {                                  \
        (elm)->next = NULL;                                             \
        (elm)->prevp = (last);                                          \
        *(last) = (elm);                                                \
        (last) = &(elm)->next;                                          \
    } while (0)
#define TQ_REMOVE(last, elm) do {                                       \
        if ((elm)->next != NULL)                                        \
            (elm)->next->prevp = (elm)->prevp;                          \
        else                                                            \
            (last) = (elm)->prevp;                                      \
        *(elm)->prevp = (elm)->next;                                    \
    } while (0)
#define TQ_INSERT_HEAD(first, last, elm) do {                           \
        if (((elm)->next = (first)) != NULL)                            \
            (first)->prevp = &(elm)->next;                              \
        else                                                            \
            (last) = &(elm)->next;                                      \
        (first) = (elm);                                                \
        (elm)->prevp = &(first);                                        \
    } while (0)
#define TQ_INSERT_BEFORE(listelm, elm) do {                             \
        (elm)->prevp = (listelm)->prevp;                                \
        (elm)->next = (listelm);                                        \
        *(listelm)->prevp = (elm);                                      \
        (listelm)->prevp = &(elm)->next;                                \
    } while (0)
#define STQ_INSERT_TAIL(last, elm) do {                                 \
        (elm)->next = NULL;                                             \
        *(last) = (elm);                                                \
        (last) = &(elm)->next;                                          \
    } while (0)

/* ======================================================================== */
/* USB device manager, "PUSB" (usbd.c)                             */
/* ======================================================================== */

#define USBD_RPC_ID         0x42535550u     /* 'PUSB' */
#define USBD_NUM_EVENTS     5
#define USBD_NUM_CLASSES    3
#define USBD_CLASS_PJOY     2               /* the only class built (the wheels) */
#define USBD_STACK_SIZE     0x1000

/* Event types: the RPC fno of the IOP driver's call. */
enum {
    USBD_EV_ATTACH = 1,
    USBD_EV_DETACH = 2,
    USBD_EV_ERROR  = 3
};

/* Event payload as the class handlers see it (UsbEvent + 0x18). */
typedef struct UsbEventData {
    int  id;                    /* 0x00 RPC word +4: device id / port index */
    int  unk_04;                /* 0x04 */
    u64  key;                   /* 0x08 RPC bytes 8..15 read big-endian: devices are kept sorted by it;
                                   bits 56-57 pick the logical port in mode 1 (? the USB location
                                   path, byte 0 = the root hub port) */
} UsbEventData;                 /* 0x10 */

typedef int  (*UsbClassFn)(void *arg, int type, UsbEventData *data);
struct UsbDriver;
typedef void (*UsbConnectFn)(struct UsbDriver *drv, int type, UsbEventData *data);
typedef void (*UsbPollFn)(struct UsbDriver *drv);

/* Header of a class driver: only these three words are read by the
 * manager (the PJOY client starts with its sceSifClientData and server id). */
typedef struct UsbDriver {
    u8   unk_00[0x2C];          /* 0x00 the driver's own (Pjoy: cd, server_id) */
    int  cls;                   /* 0x2C class index 0..2 */
    UsbConnectFn connect;       /* 0x30 called with (drv, event type, data) */
    UsbPollFn    poll;          /* 0x34 called every dispatcher pass */
} UsbDriver;

/* IOP event, queued by the RPC server and delivered by the dispatcher. */
typedef struct UsbEvent {
    int  in_use;                /* 0x00 */
    UsbClassFn fn;              /* 0x04 class handler */
    void *arg;                  /* 0x08 its argument */
    int  pending_cls;           /* 0x0C class with no handler yet (0 = resolved) */
    int  cls;                   /* 0x10 */
    int  type;                  /* 0x14 USBD_EV_* */
    UsbEventData data;          /* 0x18 */
    struct UsbEvent *next;      /* 0x28 */
    int  unk_2C;                /* 0x2C */
} UsbEvent;                     /* 0x30 */

typedef struct UsbConn {        /* malloc(0x14) */
    int  cls;                   /* 0x00 */
    UsbConnectFn fn;            /* 0x04 drv->connect */
    UsbDriver *drv;             /* 0x08 */
    struct UsbConn *next;       /* 0x0C */
    struct UsbConn **prevp;     /* 0x10 */
} UsbConn;

typedef struct UsbPoll {        /* malloc(0x10) */
    UsbPollFn fn;               /* 0x00 drv->poll */
    UsbDriver *drv;             /* 0x04 */
    struct UsbPoll *next;       /* 0x08 */
    struct UsbPoll **prevp;     /* 0x0C */
} UsbPoll;

/* The manager @0x004A7640.  Its first bytes are the RPC receive buffer
 * (sceSifRegisterRpc buff = the manager itself). */
typedef struct UsbMgr {
    int  msg_cls;               /* 0x000 RPC data: class index */
    int  msg_word;              /* 0x004 RPC data: event data word */
    u8   msg_bytes[8];          /* 0x008 RPC data: big-endian 64-bit key */
    u8   unk_010[0x30];         /* 0x010 */
    sceSifQueueData qd;         /* 0x040 */
    sceSifServeData sd;         /* 0x058 */
    int  thread_id;             /* 0x09C GetThreadId() of usbd_init's caller */
    int  intc_id;               /* 0x0A0 vblank handler id */
    int  ndrivers;              /* 0x0A4 */
    int  new_events;            /* 0x0A8 */
    UsbClassFn handler[USBD_NUM_CLASSES];   /* 0x0AC */
    void *handler_arg[USBD_NUM_CLASSES];    /* 0x0B8 */
    /* The queue heads are spelled `struct X *' / `struct X **' as the entries'
     * links (BSD TAILQ_HEAD): egcs gives every pointer type node its own
     * alias set, and retail reloads tqe_prev after a store to the tail. */
    struct UsbConn  *conn_head; /* 0x0C4 */
    struct UsbConn **conn_tail; /* 0x0C8 */
    struct UsbPoll  *poll_head; /* 0x0CC */
    struct UsbPoll **poll_tail; /* 0x0D0 */
    struct UsbEvent  *ev_head;  /* 0x0D4 */
    struct UsbEvent **ev_tail;  /* 0x0D8 */
    int  unk_0DC;               /* 0x0DC */
    UsbEvent events[USBD_NUM_EVENTS];       /* 0x0E0 */
} UsbMgr;                       /* 0x1D0 (thread stacks follow at 0x004A7840) */

/* usbd.c's statics: g_usbd (0x004A7640), g_usbd_rpc_stack
 * (0x004A7840), g_usbd_dispatch_stack (0x004A8840).  It defines the words
 * below too (.sdata). */
extern int g_usbd_started;                  /* 0x00353170 gp-0x4880 */
extern int g_usbd_wake_sema;                /* 0x00353174 gp-0x487C vblank -> dispatcher (max 1, init 0) */
extern int g_usbd_lock_sema;                /* 0x00353178 gp-0x4878 dispatcher busy lock (max 1, init 1) */
extern int g_usbd_dispatch_tid;             /* 0x0035317C gp-0x4874 */
extern int g_usbd_rpc_tid;                  /* 0x00353180 gp-0x4870 */
/* 0x00345AA8 { NULL, "attach", "detach", "error" }: the initialiser of
 * usbd_rpc_event's local `names' (an anonymous .rodata table). */

UsbEvent  *usbd_event_alloc(UsbMgr *m);                 /* 0x00265530 */
void       usbd_event_free(UsbMgr *m, UsbEvent *ev);    /* 0x002655A8 */
void       usbd_exit(void);                             /* 0x00265798 */
int        usbd_vblank_handler(int cause, void *arg, void *addr); /* 0x00265808 */
void       usbd_dispatch_thread(void *arg);             /* 0x00265850 */
void       usbd_rpc_thread(void *arg);                  /* 0x00265880 */
void      *usbd_rpc_event(unsigned int fno, void *data, int size); /* 0x002658E8 */
void       usbd_dispatch(void);                         /* 0x002659E8 */
UsbClassFn usbd_register_driver(UsbDriver *drv, UsbClassFn handler, void *arg); /* 0x00265B30 */
void       usbd_unregister_driver(UsbDriver *drv);      /* 0x00265C40 */

/* ======================================================================== */
/* PJOY client and USB class mapper (pjoy.c)                       */
/* ======================================================================== */

#define PJOY_RPC_ID         0x594F4A50u     /* 'PJOY' */
#define PJOY_NUM_DEVICES    4
#define PJOY_ALL_DEVICES    (-1)            /* port of the attach-state query */
#define PJOY_SEND_SIZE      0x20
#define PJOY_RECV_SIZE      0x80

/* RPC function numbers (also PjoyMsg.fno). */
enum {
    PJOY_FNO_ATTACH = 1,        /* the attach state of the 4 devices */
    PJOY_FNO_INFO   = 2,        /* device info (PjoyInfo) */
    PJOY_FNO_STATE  = 3,        /* input state (PjoyState) */
    PJOY_FNO_FF     = 4         /* one force-feedback command, no reply */
};

/* Results of the public calls; PJOY_BUSY is also the status of a query
 * made while the previous RPC is in flight. */
enum {
    PJOY_OK           = 0,
    PJOY_NOT_ATTACHED = 2,
    PJOY_NO_DEVICE    = 3,      /* no device on that logical port */
    PJOY_BUSY         = 5,      /* lock taken / RPC in flight */
    PJOY_QUEUE_FULL   = 7       /* 4 commands queued, or one for that device already */
};

/* Logitech classic force-feedback protocol: byte 0 = force slot mask << 4 |
 * command, byte 1 = force type (download commands) or a parameter. */
enum {
    FF_CMD_DOWNLOAD            = 0x0,
    FF_CMD_DOWNLOAD_PLAY       = 0x1,
    FF_CMD_PLAY                = 0x2,
    FF_CMD_STOP                = 0x3,
    FF_CMD_DEFAULT_SPRING_ON   = 0x4,
    FF_CMD_DEFAULT_SPRING_OFF  = 0x5,
    FF_CMD_SET_LED             = 0xA,   /* (? from the protocol, no caller in GT3) */
    FF_CMD_REFRESH             = 0xC,   /* (? idem) */
    FF_CMD_FIXED_TIME_LOOP     = 0xD,   /* (? idem) */
    FF_CMD_SET_DEFAULT_SPRING  = 0xE,   /* K1, K2, clip in bytes 2..4 */
    FF_CMD_SET_DEAD_BAND       = 0xF    /* (? idem) */
};
enum {
    FF_TYPE_CONSTANT      = 0,
    FF_TYPE_SPRING        = 1,
    FF_TYPE_DAMPER        = 2,
    FF_TYPE_AUTO_CENTER   = 3,
    FF_TYPE_SAWTOOTH_UP   = 4,
    FF_TYPE_SAWTOOTH_DOWN = 5,
    FF_TYPE_TRAPEZOID     = 6,
    FF_TYPE_RECTANGLE     = 7,
    FF_TYPE_VARIABLE      = 8,
    FF_TYPE_RAMP          = 9
};
#define FF_ALL_SLOTS    0xF0            /* the slot mask of all four forces */

/* FfCmd (one force-feedback command): engine/media/G002_mpeg_frameq.h. */

/* Send buffer (0x004A9BC0); every int is stored with swl/swr. */
typedef struct __attribute__((packed)) PjoyMsg {
    int   port;                 /* 0x00 device index, PJOY_ALL_DEVICES */
    int   fno;                  /* 0x04 PJOY_FNO_* */
    u8    unk_08[8];            /* 0x08 */
    FfCmd cmd;                  /* 0x10 */
    u8    unk_18[8];            /* 0x18 */
} PjoyMsg;                      /* 0x20 */

typedef struct PjoyCmd {
    int   port;                 /* 0x00 device index, -1 = sent */
    FfCmd cmd;                  /* 0x04 */
} PjoyCmd;                      /* 0x0C */

/* PjoyState (fno 3) and PjoyInfo (fno 2): engine/media/G002_mpeg_frameq.h. */

typedef struct PjoyAttach { int v[PJOY_NUM_DEVICES]; } PjoyAttach;  /* fno 1 reply, copied as one block */

struct UsbClass;

/* The client @0x004A9840; its head is a UsbDriver. */
typedef struct Pjoy {
    sceSifClientData cd;        /* 0x000 */
    u32   server_id;            /* 0x028 'PJOY' */
    int   cls;                  /* 0x02C USBD_CLASS_PJOY (UsbDriver.cls) */
    UsbConnectFn connect;       /* 0x030 pjoy_connect */
    UsbPollFn    poll;          /* 0x034 pjoy_poll */
    struct UsbClass *port_map;  /* 0x038 */
    PjoyMsg *send;              /* 0x03C g_pjoy_send_buf */
    u8      *recv;              /* 0x040 g_pjoy_recv_buf: +0 status, +8 payload */
    PjoyCmd  cmds[4];           /* 0x044 queued FF commands */
    int   ncmds;                /* 0x074 */
    int   status[PJOY_NUM_DEVICES];     /* 0x078 last info / state query result */
    PjoyState state[PJOY_NUM_DEVICES];  /* 0x088 */
    PjoyInfo  info[PJOY_NUM_DEVICES];   /* 0x168 */
    int   attached[PJOY_NUM_DEVICES];   /* 0x328 1 = attached */
    int   sema;                 /* 0x338 */
    int   inited;               /* 0x33C */
    int   event_count[PJOY_NUM_DEVICES];/* 0x340 events seen per device */
} Pjoy;                         /* 0x350 */

/* A device of a class, kept sorted by key. */
typedef struct UsbDev {
    int  in_use;                /* 0x00 */
    int  unk_04;                /* 0x04 */
    UsbEventData data;          /* 0x08 copied from the attach event */
    struct UsbDev *next;        /* 0x18 */
    struct UsbDev **prevp;      /* 0x1C */
} UsbDev;                       /* 0x20 */

/* A class's device list (BSD TAILQ_HEAD; the class code reaches it through
 * a pointer, retail's `cls + 0x24' in a saved register). */
typedef struct UsbDevList {
    struct UsbDev  *first;      /* 0x00 */
    struct UsbDev **last;       /* 0x04 address of the last next pointer */
} UsbDevList;

/* Logical port mapping modes. */
enum {
    USBCLS_MAP_BY_ID   = 0,     /* port = device id */
    USBCLS_MAP_BY_SIDE = 1      /* by key bits 56-57: 2 -> port 0, 1 -> port 1 */
};
#define USBCLS_SIDE_MASK    0x0300000000000000ull
#define USBCLS_SIDE_0       0x0200000000000000ull   /* -> logical port 0 */
#define USBCLS_SIDE_1       0x0100000000000000ull   /* -> logical port 1 */

/* Logical port mapper of a class: 3 @0x004A9C80 (only class 2 is built). */
typedef struct UsbClass {
    int  mode;                  /* 0x00 USBCLS_MAP_* */
    int  map[4];                /* 0x04 logical port -> device id, -1 = none */
    int  changed[4];            /* 0x14 set on every rebuild, cleared by lookup */
    UsbDevList list;            /* 0x24 the devices, sorted by key */
    int  unk_2C;                /* 0x2C */
    UsbDev devs[4];             /* 0x30 */
    UsbDriver *drv;             /* 0xB0 */
    UsbClassFn reg;             /* 0xB4 usbd_register_driver result */
    int  inited;                /* 0xB8 */
    int  sema;                  /* 0xBC */
} UsbClass;                     /* 0xC0 */

/* pjoy.c's statics: g_pjoy (0x004A9840), g_pjoy_send_buf
 * (0x004A9BC0), g_pjoy_recv_buf (0x004A9C00), g_usb_classes (0x004A9C80),
 * g_pjoy_flush_forward (.sbss 0x003532DC).  It defines the two below too. */
extern const struct SemaParam g_pjoy_sema_param;  /* 0x00345AB8 { max 1, init 1 } */
extern const struct SemaParam g_usbcls_sema_param;/* 0x00345AD0 { max 1, init 1 } */

int     pjoy_bind(Pjoy *pj);                            /* 0x002655B0 */
Pjoy   *pjoy_init(void);                                /* 0x00265D80 */
void    pjoy_set_port_map(struct UsbClass *cls);        /* 0x00265E40 */
int     pjoy_send_command(int port, const FfCmd *cmd);  /* 0x00265FC8 */
void    pjoy_connect(Pjoy *pj, int type, UsbEventData *data); /* 0x00266220 */
int     pjoy_flush_command(Pjoy *pj, FfCmd *buf, int i);/* 0x00266430 */
void    pjoy_poll(Pjoy *pj);                            /* 0x002664C0 */
u8     *pjoy_rpc_query(Pjoy *pj, int port, int *status, int fno); /* 0x00266698 */
void    pjoy_rpc_command(Pjoy *pj, int port, const FfCmd *cmd, int *status); /* 0x00266750 */

int     ff_send(int port, FfCmd *cmd);                  /* 0x00266810 */
int     ff_send_refresh(int port, FfCmd *cmd);          /* 0x00266858 */
int     ff_set_default_spring(int port, int slots, int k1, int k2, int clip); /* 0x00266880 */
int     ff_play_stop(int port, int slots, int play);    /* 0x002668B0 */
int     ff_default_spring(int port, int slots, int on); /* 0x002668E8 */
int     ff_set_dead_band(int port, int on);             /* 0x00266920 */
int     ff_fixed_time_loop(int port, int on);           /* 0x00266948 */
int     ff_set_led(int port, int leds);                 /* 0x00266970 */
FfCmd  *ff_build_spring(FfCmd *c, int slots, int d1, int d2, int k1, int k2,
                        int s1, int s2, int clip);      /* 0x002669C0 */
FfCmd  *ff_build_damper(FfCmd *c, int slots, int k1, int k2, int s1, int s2); /* 0x00266A00 */
FfCmd  *ff_build_autocenter(FfCmd *c, int slots, int p2, int p3, int p4); /* 0x00266A28 */
FfCmd  *ff_build_sawtooth_up(FfCmd *c, int slots, int p2, int p3, int p4, int p5, int p6); /* 0x00266A48 */
FfCmd  *ff_build_sawtooth_down(FfCmd *c, int slots, int p2, int p3, int p4, int p5, int p6); /* 0x00266A78 */
FfCmd  *ff_build_trapezoid(FfCmd *c, int slots, int p2, int p3, int p4, int p5, int p6, int p7); /* 0x00266AA8 */
FfCmd  *ff_build_rectangle(FfCmd *c, int slots, int p2, int p3, int p4, int p5, int p6); /* 0x00266AD8 */
FfCmd  *ff_build_variable(FfCmd *c, int slots, int p2, int p3, int p4, int p5, int p6,
                          int p7, int p8, int p9);      /* 0x00266B00 */
FfCmd  *ff_build_ramp(FfCmd *c, int slots, int p2, int p3, int p4, int p5, int p6); /* 0x00266B48 */

int       usbcls_event(UsbClass *cls, u32 type, UsbEventData *data); /* 0x00266B78 */
int       usbcls_slot(int *map, int n);                 /* 0x00266CF0 */
void      usbcls_rebuild_map(UsbClass *cls);            /* 0x00266CF8 */
UsbClass *usbcls_get(int idx);                          /* 0x00266E98 */
void      usbcls_close(int idx);                        /* 0x00266F50 */
void      usbcls_set_mode(UsbClass *cls, int mode);     /* 0x00266FC0 */
int       usbcls_lookup(UsbClass *cls, int port);       /* 0x00267008 */
UsbDev   *usbcls_dev_alloc(UsbClass *cls);              /* 0x00267068 */
void      usbcls_dev_free(UsbClass *cls, UsbDev *dev);  /* 0x002670A8 */

/* ======================================================================== */
/* Memory card (mcard_io.c), clock (scf_lock.c)          */
/* ======================================================================== */
extern int g_mc_lock_sema;                  /* 0x003531A0 gp-0x4850 (.sdata, -1 until mc_init_once) */
extern int g_scf_lock_sema;                 /* 0x003531A4 gp-0x484C (.sdata, -1 until scf_lock_init) */

#define MC_SYNC_WAIT    0                   /* sceMcSync mode: wait (non-zero: poll once) */

void mc_sync_wait(int *cmd, int *result);   /* 0x002678E8 */

/* ======================================================================== */
/* Sound (sound_driver.c; u124 has the rest)                              */
/* ======================================================================== */

/* SndVoice +0x08, EngineLayer +0x04: volL / volR are read and written as one
 * word, volL | volR << 16 (SND_HALF_PAIR, G002_mpeg_frameq.h). */

#define SND_DIRTY_PITCH     0x1             /* SndVoice.dirty */
#define SND_DIRTY_VOLUME    0x2
#define SND_NUM_VOICES      SND_VOICES_TOTAL    /* PC: the voice pool, 46 on the PS2 (G002_mpeg_frameq.h) */
#define SND_VOL_FULL        0x3FFF          /* volumes: 0x4000 = 1.0 */

/* The ENGN / INST / SEQG bank layouts and their magics: G002_mpeg_frameq.h. */

#define ENGINE_MIN_RPM      200     /* engine_update: lower rpm counts as this */
#define ENGINE_ADSR         0x3000F /* the engine voices' ADSR (adsr1 0xF, adsr2 3) */
#define ENGINE_PITCH_UNIT   0x1000  /* GtSoundVoice.pitch: 1.0 */
#define ENGINE_FADE_FULL    0x4000  /* crossfade factor 1.0 */
#define ENGINE_SLEW_VOL     0x1000  /* max volume change per update */


#define SEQ_NUM_CHANNELS    47
#define SEMITONE            0x100       /* notes and pitches: 8.8 semitones */
#define OCTAVE              0xC00
#define SEQ_TICKS_PER_SEC   0x1C9C3800  /* 480,000,000: Sequence.frameStep = this / frame rate */

/* sceSdEffectAttr modes that take extra arguments (libsd SD_REV_MODE_*). */
#define SD_REV_MODE_OFF     0
#define SD_REV_MODE_ECHO    7           /* delay, feedback */
#define SD_REV_MODE_DELAY   8           /* delay */

extern s16 g_snd_sine[0x1400];          /* 0x002FFE98 4096 * sin(2 pi i / 4096), 1.25 periods (.data): the crossfade uses 0..0x400 */
extern const u16 g_snd_pitch_octave[384]; /* 0x00345AE8 0x4000 * 2^(i / 384): one octave in 1/32 semitones */

SndVoice *snd_voice_lookup(s8 *handle);                         /* 0x00267CE0 */
void      engine_layer_pitch(GtSoundVoice *es, int i, int rpm, int smooth);  /* 0x00267D18 */
void      engine_layer_volume(GtSoundVoice *es, int i, int rpm, int smooth); /* 0x00267D98 */
void      seq_link(Sequence *seq);                              /* 0x00268848 */
void      snd_stop_all_if_ready(void);                          /* 0x002688D8 */
void      snd_stop_all_voices(void);                            /* 0x002688F8 */

#ifndef __cplusplus
_Static_assert(sizeof(UsbEventData) == 0x10, "UsbEventData");
_Static_assert(sizeof(UsbEvent) == 0x30, "UsbEvent");
_Static_assert(sizeof(UsbMgr) == 0x1D0, "UsbMgr");
_Static_assert(sizeof(PjoyMsg) == 0x20, "PjoyMsg");
_Static_assert(sizeof(PjoyCmd) == 0x0C, "PjoyCmd");
_Static_assert(sizeof(PjoyState) == 0x38, "PjoyState");
_Static_assert(sizeof(PjoyInfo) == 0x70, "PjoyInfo");
_Static_assert(sizeof(Pjoy) == 0x350, "Pjoy");
_Static_assert(sizeof(UsbDev) == 0x20, "UsbDev");
_Static_assert(sizeof(UsbClass) == 0xC0, "UsbClass");
#endif

#ifdef __cplusplus
}
#endif

#endif /* U123_H */
