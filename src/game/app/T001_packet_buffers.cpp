/* T001 - the double packet buffer g_0028F550 (two 0xA0000-byte .bss
 * buffers the render thread alternates between).  0x00101290..0x001012E0. */
#include "game/app/T001_packet_buffers.h"

/* PC (ROADMAP 18, no memory limits): 16 MB each, not 0xA0000 - the frame's
 * text, sprites, particles and vertex arrays grow down from the end, the
 * packet's records up from the start, unchecked (pgl_core.cpp
 * gfx_alloc_tail logs a crossing). */
enum { PACKET_BUFFER_SIZE = 0x1000000 };

/* The two buffers: file-local (.bss, emitted at the end of the unit in
 * this order).  PC: 16-byte aligned, as the PS2's quadword packets need
 * (the game stores whole quadwords into them; a u8 array is otherwise
 * only byte aligned off Windows). */
alignas(16) static u8 g_00353700[PACKET_BUFFER_SIZE];   /* 0x00353700 buf[0] */
alignas(16) static u8 g_003F3700[PACKET_BUFFER_SIZE];   /* 0x003F3700 buf[1] */

// FUNC 0x00101290 PacketBuffers::PacketBuffers
PacketBuffers::PacketBuffers()
{
    buf[0] = g_00353700;
    buf[1] = g_003F3700;
    size   = PACKET_BUFFER_SIZE;
    cur    = 0;
}

/* The object (.data 0x0028F550: egcs puts a public object that has a
 * constructor in .data, zero-filled, not in .bss). */
PacketBuffers g_0028F550;

/* Compiler-generated: egcs's static initialiser of g_0028F550 (.ctors entry
 * 0x0028F024), PacketBuffers() on it, emitted at the end of the unit. */
// FUNC 0x001012C0 T001_static_init (_GLOBAL_$I$__13PacketBuffers)
/* implicit: PacketBuffers (the constructor) */
