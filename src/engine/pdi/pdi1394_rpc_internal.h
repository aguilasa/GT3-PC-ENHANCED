/* pdi1394_rpc_internal.h - unit u127 of the GT3 main executable, 0x0026E310..0x0026F538.
 *
 * The whole no-FDE stretch G003 (plain C, no exceptions), two modules:
 *   pdistd_misc.c   thread_create_retry, then the small pdistd helpers
 *                        shared with the bootstrap's pdistd_misc.cpp
 *                        (global lock, break stubs, printf_to_deci2)
 *   pdi1394_rpc.c   the C core of the EE side of the i.LINK (IEEE 1394)
 *                        driver: SIF RPC clients of the IOP servers 'PFIM'
 *                        (control) and 'PFIS' (frame exchange), an EE RPC
 *                        server 'PFWE' the IOP calls back (status block,
 *                        incoming 8-byte messages), a vblank timeout wait,
 *                        and 8 message queues (4.4BSD TAILQs over a 32-node
 *                        pool).  The C++ wrapper class pdi1394 follows it
 *                        (T541, vtable 0x003477F0); T473 (link battle
 *                        session) and the menus are the users.
 *
 * The canonical header engine/pdi/G003_pdi1394_rpc.h declares what other units
 * call (and Pdi1394Msg, Guid1394, Pdi1394Status, Pdi1394Callback); this
 * header keeps what only the unit uses: the queue and context layouts, the
 * file-static data and the static helpers.
 */
#ifndef U127_H
#define U127_H

#include "engine/pdi/G003_pdi1394_rpc.h"      /* own translation unit */
#include "eekernel.h"                   /* ThreadParam, SemaParam, INTC_VBLANK_S, ExitHandler */
#include "runtime/newlib/G007_stdio.h"            /* libkernl stubs, SIF RPC, memcpy / memset,
                                         * wait_vblank_start, sce_printf_deci2 (core addresses) */
#include "engine/media/G002_mpeg_frameq.h"      /* create_mutex_sema, yield_low_priority, signal_sema_helper */
/* _gp with a size, declared before G000's unsized declaration: egcs then
 * counts it as small data (-G8) and loads &_gp with one `la' (GAS: lui /
 * addiu) kept in source order, as retail does in thread_create_retry and
 * pdi1394_init (app_internal.h does the same; not in G000 itself, where a sized
 * declaration costs u121 / u123: ref/matching/u127/pending.md). */
#ifdef __cplusplus
extern "C" char _gp[4];                         /* 0x003579F0, the linkage of G000's declaration */
#else
extern char _gp[4];                             /* 0x003579F0 */
#endif
#include "runtime/startup/G000_collect2_ctors.h"   /* _gp */

/* ======================================================================== */
/* pdistd_misc.c                                                       */
/* ======================================================================== */

/* s_globalLockSema (0x003531D8, .sdata, = -1) is a file static of
 * pdistd_misc.c (as the bootstrap's pdistd_misc.cpp has it). */

/* ======================================================================== */
/* pdi1394_rpc.c                                                       */
/* ======================================================================== */

#define PDI1394_MAGIC_OK    0xAC1DCAFEu     /* ctx guard word; first word of a good reply */
#define PDI1394_MAGIC_BAD   0xDEADBEAFu     /* ctx guard word; first word of a failed reply */

#define PDI1394_RPC_CTRL    0x4D494650u     /* 'PFIM' in memory: IOP control server */
#define PDI1394_RPC_DATA    0x53494650u     /* 'PFIS' in memory: IOP frame-exchange server */
#define PDI1394_RPC_EE      0x45574650u     /* 'PFWE' in memory: the EE server (IOP -> EE) */

/* fno values of the EE server 'PFWE'. */
#define PDI1394_FNO_STATUS  0xFFFFFFFFu     /* data = a new status block */
#define PDI1394_FNO_2       0xFFFFFFFEu     /* a case label of its own, nothing done (then the callback) */
#define PDI1394_FNO_3       0xFFFFFFFDu     /* the same */
#define PDI1394_FNO_MSG     0xFFFFFFFCu     /* data = Pdi1394MsgPacket */

/* Data of a PDI1394_FNO_MSG call: the channel is read with lwl/lwr, so the
 * block has no alignment (? packed in the original too). */
typedef struct __attribute__((packed)) Pdi1394MsgPacket {
    int        chan;                        /* 0x00 queue 0..7 */
    Pdi1394Msg msg;                         /* 0x04 */
} Pdi1394MsgPacket;                         /* 0x0C */

/* Queue node: a BSD TAILQ element.  The links are `struct Pdi1394MsgNode *'
 * in the node and in the head alike (TAILQ_ENTRY / TAILQ_HEAD spell
 * `struct type *'): egcs gives the typedef's pointer type an alias set of
 * its own, and with `Pdi1394MsgNode *' in the head the scheduler moved
 * `*head->last = elm' over `elm->next = NULL' (pdi1394_msg_init). */
typedef struct Pdi1394MsgNode {
    Pdi1394Msg              msg;            /* 0x00 */
    struct Pdi1394MsgNode  *next;           /* 0x08 tqe_next */
    struct Pdi1394MsgNode **prev;           /* 0x0C tqe_prev */
} Pdi1394MsgNode;                           /* 0x10 */

/* TAILQ_HEAD */
typedef struct Pdi1394MsgList {
    struct Pdi1394MsgNode  *first;          /* 0x00 tqh_first */
    struct Pdi1394MsgNode **last;           /* 0x04 tqh_last */
} Pdi1394MsgList;                           /* 0x08 */

#define PDI1394_NCHAN       8
#define PDI1394_NNODES      32
#define PDI1394_CHAN_MAX    4               /* a push to a full channel drops its oldest */

typedef struct Pdi1394MsgQueues {           /* @0x004AB268 */
    Pdi1394MsgList freeList;                /* 0x00 unused nodes */
    Pdi1394MsgList chan[PDI1394_NCHAN];     /* 0x08 */
    int            count[PDI1394_NCHAN];    /* 0x48 */
} Pdi1394MsgQueues;                         /* 0x68 */

typedef struct Pdi1394Ctx {                 /* @0x004AB140 */
    u32               magic0;               /* 0x000 PDI1394_MAGIC_OK */
    void             *rpcRecvBuf;           /* 0x004 = s_pdiRecvBuf, buffer of the EE server */
    void             *rpcSendBuf;           /* 0x008 = s_pdiSendBuf, send buffer of pdi1394_send_msg */
    sceSifQueueData   queue;                /* 0x00C */
    sceSifServeData   serve;                /* 0x024 'PFWE' */
    sceSifClientData  cdCtrl;               /* 0x068 'PFIM' */
    sceSifClientData  cdData;               /* 0x090 'PFIS' */
    Pdi1394Status     status;               /* 0x0B8 */
    void             *lastSend;             /* 0x104 = s_pdiLastSend, copy of the last frame sent */
    void             *lastRecv;             /* 0x108 = s_pdiLastRecv, copy of the last frame received */
    Pdi1394Callback   callback;             /* 0x10C called after every other EE-server call */
    void             *cbArg;                /* 0x110 */
    int               waitThread;           /* 0x114 thread in pdi1394_wait_event, -1 none */
    int               timedOut;             /* 0x118 set by the vblank handler */
    int               sema;                 /* 0x11C lock of all of this */
    u32               magic1;               /* 0x120 PDI1394_MAGIC_BAD */
} Pdi1394Ctx;                               /* 0x124 */

_Static_assert(sizeof(Pdi1394MsgPacket) == 0x0C, "Pdi1394MsgPacket");
_Static_assert(sizeof(Pdi1394MsgNode) == 0x10, "Pdi1394MsgNode");
_Static_assert(sizeof(Pdi1394MsgQueues) == 0x68, "Pdi1394MsgQueues");
_Static_assert(sizeof(Pdi1394Ctx) == 0x124, "Pdi1394Ctx");

/* The module's data are file statics of pdi1394_rpc.c (defined there,
 * with their addresses): s_pdiSemaParam (.rodata 0x003477D8),
 * s_pdiVblankCount (.sbss 0x003532E4), and the .bss 0x004A9F00..0x004AB4D0:
 * s_pdiServerStack, s_pdiSendBuf, s_pdiRecvBuf, s_pdiLastSend,
 * s_pdiLastRecv, s_pdi, s_pdiQueues, s_pdiNodes. */

/* Static helpers (no caller outside the unit; some none at all). */
void  pdi1394_server_thread(void *context);                         /* 0x0026E4C8 */
void *pdi1394_rpc_server(unsigned int fno, void *data, int size);   /* 0x0026E5C0 */
void  pdi1394_msg_peek_last_locked(int chan, Pdi1394Msg *out);      /* 0x0026E780 */
void  pdi1394_msg_clear_locked(int chan);                           /* 0x0026E7D8 */
void  pdi1394_msg_remove_if_locked(int chan,
                                   int (*pred)(void *arg, Pdi1394Msg *msg),
                                   void *arg);                      /* 0x0026E820 */
void  pdi1394_clear_callback(void);                                 /* 0x0026ED10 */
void  pdi1394_wake_waiter(void);                                    /* 0x0026ED48 */
int   pdi1394_vblank_handler(int cause);                            /* 0x0026EF38 */
void  pdi1394_msg_push(int chan, const Pdi1394Msg *msg);            /* 0x0026F060 */
int   pdi1394_msg_find(int chan, Pdi1394Msg *out, int type);        /* 0x0026F130 */
int   pdi1394_msg_peek_last(int chan, Pdi1394Msg *out);             /* 0x0026F1D8 */
int   pdi1394_msg_take(int chan, Pdi1394Msg *out, int type);        /* 0x0026F228 */
int   pdi1394_msg_pop(int chan, Pdi1394Msg *out);                   /* 0x0026F340 */
void  pdi1394_msg_clear(int chan);                                  /* 0x0026F3D8 */
void  pdi1394_msg_remove_if(int chan,
                            int (*pred)(void *arg, Pdi1394Msg *msg),
                            void *arg);                             /* 0x0026F468 */

#endif /* U127_H */
