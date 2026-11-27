/* usb_pc.cpp - the USB device manager and the PJOY wheel client
 * (usbd.c 0x00265530.., pjoy.c 0x00265D80.. in the PS2 program).
 *
 * On the PS2 these talked to Polyphony's pdiusb IOP module, which drove
 * the USB steering wheels (Logitech GT Force and friends): their state
 * came over RPC and force-feedback commands went back.  Here PJOY ports 0
 * and 1 are the PC's wheels (platform/wheel.h: Wheel 1 / 2, each a Logitech
 * Driving Force, 0x46D:0xC294, mapped from an SDL joystick), connected while
 * their device is; with none the manager reports no device, as a PS2 with
 * nothing in its USB sockets did, and the wheel ports (T479) stay
 * disconnected.  PC (2026-11-20, user: "Force feedback for the virtual
 * wheels, currently none is implemented"): the force-feedback commands are
 * decoded (the Logitech classic protocol, pjoy.c's builders) and played on
 * the PC wheel (platform/wheel.h force / spring, platform/wheel_ffb.h): a
 * constant force - T479's update sends one every tick, the race's steering
 * force in its third slot (RaceInput::sendVibration, T336), the others none
 * - and the default spring the enhanced engine sets up and switches (T479
 * setupSpring / enableSpring: on in the menus, off in a race).  Before,
 * a connected wheel took the commands and dropped them. */
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/system/system_drivers_internal.h"
#include "platform/log.h"
#include "platform/wheel.h"

#include <string.h>

/* The Driving Force's USB ids (engine/input/control_internal.h). */
enum { PC_WHEEL_VENDOR = 0x046D, PC_WHEEL_PRODUCT = 0xC294 };

/* A wheel on logical `port'. */
static int pc_wheel_connected(int port)
{
    return wheel::connected(port);
}

// FUNC 0x00265610 usbd_init
int usbd_init(int priority)
{
    (void)priority;
    return 0;
}

// FUNC 0x00265770 usbjoy_start
void usbjoy_start(void)
{
}

// FUNC 0x00265E50 pjoy_get_event_count
/* Events seen for the device on logical `port', -1 if none is mapped. */
int pjoy_get_event_count(int port)
{
    (void)port;
    return -1;
}

// FUNC 0x00265EA0 pjoy_read_state
/* The wheel as the Driving Force reports it: the 10-bit wheel, the pedals
 * at 0xFF released (0 pressed fully), 12 buttons, the hat (8 none). */
int pjoy_read_state(int port, PjoyState *out)
{
    wheel::State w;
    if (!wheel::read(port, &w))
        return PJOY_NO_DEVICE;
    memset(out, 0, sizeof(*out));
    out->x = w.x;
    out->slider[0] = 0xFF - w.gas;
    out->slider[1] = 0xFF - w.brake;
    out->pov[0] = w.hat;
    for (int i = 0; i < 12; i++)
        out->buttons[i] = w.buttons[i];
    return PJOY_OK;
}

// FUNC 0x002660E0 pjoy_get_info
int pjoy_get_info(int port, PjoyInfo *out)
{
    if (!pc_wheel_connected(port))
        return PJOY_NO_DEVICE;
    memset(out, 0, sizeof(*out));
    out->vendor = PC_WHEEL_VENDOR;
    out->product = PC_WHEEL_PRODUCT;
    return PJOY_OK;
}

/* PC (2026-11-20): the force feedback, decoded.  A constant force's level
 * byte: 0x7F (FF_NONE, control_internal.h) and 0x80 none, below them a push
 * to the right, above to the left (T479 autoCentre turns a wheel right of
 * centre - its 8-bit position above 0x80 - with that position as the level:
 * the bytes above 0x80 push left; T336's centring terms agree), 0 / 0xFF
 * the most.  The slots the command names add up. */
static float pc_ff_level(int b)
{
    if (b == 0x7F || b == 0x80)
        return 0.0f;
    return (float)(0x80 - b) / 127.0f;
}

/* The default spring of each port: on, its K (0..7) and clip (0..255) as
 * Set Default Spring last gave them (GT3's 3, 3, 0x1E until then). */
static struct {
    int on, k, clip;
} g_pcSpring[2] = {{0, 3, 0x1E}, {0, 3, 0x1E}};
static int g_pcFfLogged;                    /* the force types not played, said once each (bits) */

static void pc_ff_spring(int port)
{
    if (port < 0 || port > 1)
        return;
    wheel::spring(port, g_pcSpring[port].on != 0, (float)g_pcSpring[port].k / 7.0f,
                  (float)g_pcSpring[port].clip / 255.0f);
}

// FUNC 0x00266830 ff_send_download_play
int ff_send_download_play(int port, FfCmd *cmd)
{
    if (!pc_wheel_connected(port))
        return PJOY_NO_DEVICE;
    const int slots = cmd->b[0] & FF_ALL_SLOTS, type = cmd->b[1];
    if (type == FF_TYPE_CONSTANT) {
        float f = 0.0f;
        for (int i = 0; i < 4; i++)
            if (slots & (0x10 << i))
                f += pc_ff_level(cmd->b[2 + i]);
        wheel::force(port, f);
    } else if (type < 16 && !(g_pcFfLogged & (1 << type))) {
        g_pcFfLogged |= 1 << type;
        pclog::write("wheel: force feedback: force type %d not played (only GT3's constant force and default "
                     "spring are)", type);
    }
    return PJOY_OK;
}

// FUNC 0x00266880 ff_set_default_spring
int ff_set_default_spring(int port, int slots, int k1, int k2, int clip)
{
    (void)slots;
    if (!pc_wheel_connected(port))
        return PJOY_NO_DEVICE;
    g_pcSpring[port].k = (k1 > k2 ? k1 : k2) & 7;
    g_pcSpring[port].clip = clip & 0xFF;
    pc_ff_spring(port);
    return PJOY_OK;
}

// FUNC 0x002668E8 ff_default_spring
int ff_default_spring(int port, int slots, int on)
{
    if (!pc_wheel_connected(port))
        return PJOY_NO_DEVICE;
    (void)slots;                            /* (GT3's: all four off, then on in 0x20 and switched there) */
    g_pcSpring[port].on = on == 1;
    pc_ff_spring(port);
    return PJOY_OK;
}

/* The command builders as pjoy.c's (the bytes ff_send_download_play reads). */
// FUNC 0x00266998 ff_build_constant
FfCmd *ff_build_constant(FfCmd *c, int f0, int f1, int f2, int f3)
{
    memset(c, 0, sizeof(*c));
    c->b[0] = FF_ALL_SLOTS | FF_CMD_DOWNLOAD;
    c->b[1] = FF_TYPE_CONSTANT;
    c->b[2] = (u8)f0;
    c->b[3] = (u8)f1;
    c->b[4] = (u8)f2;
    c->b[5] = (u8)f3;
    return c;
}

// FUNC 0x00266A00 ff_build_damper
FfCmd *ff_build_damper(FfCmd *c, int slots, int k1, int k2, int s1, int s2)
{
    memset(c, 0, sizeof(*c));
    c->b[0] = (u8)slots;
    c->b[1] = FF_TYPE_DAMPER;
    c->b[2] = (u8)k1;
    c->b[3] = (u8)s1;
    c->b[4] = (u8)k2;
    c->b[5] = (u8)s2;
    return c;
}
