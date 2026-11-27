/* T479 - USBSteeringPort: a USB steering wheel.  0x00230388..0x00231190.
 *
 * Devices: Logitech (vendor 0x46D) GT Force 0xC293 (0xC295 is taken as
 * 0xC293) and 0xC294, and vendor 0x6F8 product 4.  A new Logitech wheel
 * first runs an auto-centring sequence with its force feedback (autoCentre)
 * before it counts as connected.  update reads the driver's report
 * (pjoy_read_state) every frame, with the GT Force's calibration from the
 * game options while the pedal map is on, sends the force values
 * (ff_build_constant / ff_send_download_play) and resets them; translate
 * maps the wheel's buttons, its hat and quick steering movements to GT3's
 * buttons and gives channels 0-2 = wheel, pedal 1, pedal 2.  Control USB
 * types: 1 GT Force, 3 0xC294, 2 0x6F8 wheel, 7 anything else. */
#include "engine/input/control_internal.h"
#include "engine/input/T479_usb_steering_port.h"
#include "game/save/T426_controller_config.h"    /* ControllerConfig::getSteerMin .. getValid */
#include "game/save/T429_game_status.h"          /* getGameStatus, GameStatus::options */
#include "game/enhanced.h"                  /* ENH_INPUT_WHEEL_MENU_STEER; the menus' ENH_MENU_WHEEL_CALIBRATION */

/* pjoy.c's force-feedback builders the enhanced engine uses (backend.md B2;
 * G002_mpeg_frameq.h declares only the two GT3 called from here). */
extern "C" {
int    ff_default_spring(int port, int slots, int on);                  /* 0x002668E8 */
int    ff_set_default_spring(int port, int slots, int k1, int k2, int clip); /* 0x00266880 */
FfCmd *ff_build_damper(FfCmd *c, int slots, int k1, int k2, int s1, int s2); /* 0x00266A00 */
}

/* (? name) The calibration of (port, wheel) applied to the report: the
 * wheel to 0..0x3FF around the stored centre, the pedals to 0..0xFF.  No
 * check of a zero range (the division traps, break 7).  One variable for
 * the three values (retail's s0). */
// FUNC 0x00230388 usbApplyCalibration
/* (? name) 0..max.  An inline (u105 matching): its parameter copy and its
 * result keep retail's registers, the limit already in a register when it
 * is compared. */
static inline int usbClamp(int v, int max)
{
    return (v < 0) ? 0 : ((v > max) ? max : v);
}

void usbApplyCalibration(ControllerConfig *cfg, int port, int wheel, PjoyState *report)
{
    if (cfg->getValid(port, wheel) == 0)
        return;

    int min    = cfg->getSteerMin(port, wheel);
    int range  = (int)cfg->getSteerMax(port, wheel) - min;
    int v      = report->x;
    int offset = (int)cfg->getSteerCentre(port, wheel) - 0x200;    /* the centre's offset from 0x200 */
    v = (((v - offset) - min) << 10) / range;
    report->x = usbClamp(v, 0x3FF);

    min    = cfg->getAxis1Min(port, wheel);
    range  = (int)cfg->getAxis1Max(port, wheel) - min;
    v = report->slider[0];
    v = ((v - min) << 8) / range;
    report->slider[0] = usbClamp(v, 0xFF);

    min    = cfg->getAxis2Min(port, wheel);
    range  = (int)cfg->getAxis2Max(port, wheel) - min;
    v = report->slider[1];
    v = ((v - min) << 8) / range;
    report->slider[1] = usbClamp(v, 0xFF);
}

/* (? name) The GT Force's 6 buttons as raw bits (translate maps 1 / 8 to
 * up / down, 4 start, 2 select, 0x20 decide, 0x10 cancel). */
// FUNC 0x00230538 usbGTForceButtons
u32 usbGTForceButtons(const u8 *buttons)
{
    u32 raw = 0;                            /* (`buttons[0] != 0' gives other registers) */
    if (buttons[0]) raw |= 0x1;
    if (buttons[2]) raw |= 0x2;
    if (buttons[4]) raw |= 0x4;
    if (buttons[1]) raw |= 0x8;
    if (buttons[3]) raw |= 0x10;
    if (buttons[5]) raw |= 0x20;
    return raw;
}

/* The 0xC294 wheel's 12 buttons as raw bits (the first word of T479's
 * .rodata, before the jump tables). */
const u32 g_00341AB0[12] = {                /* 0x00341AB0 */
    0x800, 0x1000, 0x2000, 0x400, 8, 1, 0x10, 2, 0x8000, 0x4000, 0x20, 4
};

/* (? name) The 0xC294 wheel: 12 buttons through the table g_00341AB0
 * {0x800, 0x1000, 0x2000, 0x400, 8, 1, 0x10, 2, 0x8000, 0x4000, 0x20, 4},
 * then the hat (0 = up, clockwise; other values: none). */
// FUNC 0x00230580 usbC294Buttons
u32 usbC294Buttons(const u8 *buttons, const int *pov)
{
    u32 raw = 0;
    for (int i = 0; i < 12; i++) {
        if (buttons[i] != 0)
            raw |= g_00341AB0[i];
    }
    u32 hat = *pov;                         /* read here: its range check stays after the loop */
    switch (hat) {                          /* table 0x00341AE0 */
    case 0: raw |= WHEEL_HAT_UP;                    break;
    case 1: raw |= WHEEL_HAT_UP | WHEEL_HAT_RIGHT;   break;
    case 2: raw |= WHEEL_HAT_RIGHT;                 break;
    case 3: raw |= WHEEL_HAT_DOWN | WHEEL_HAT_RIGHT; break;
    case 4: raw |= WHEEL_HAT_DOWN;                  break;
    case 5: raw |= WHEEL_HAT_DOWN | WHEEL_HAT_LEFT;  break;
    case 6: raw |= WHEEL_HAT_LEFT;                  break;
    case 7: raw |= WHEEL_HAT_UP | WHEEL_HAT_LEFT;    break;
    }
    return raw;
}

/* (? name) Logitech wheels as GT3 directions: left when the wheel is at
 * 0x13F or less or swings left across 0x1D1 fast, right at 0x2C1 or more or
 * swinging right across 0x230 fast (the speed, per second at 60 frames,
 * beyond 0x1A5).  pov is not used. */
// FUNC 0x00230628 usb46DSteerButtons
u32 usb46DSteerButtons(const int *pov, int x, int prevX)
{
    int speed = (x - prevX) * 60;
    u32 buttons = 0;
    if (prevX >= 0x1D1 && x < 0x1D1) {     /* as the right turn below (`buttons = ... ? 0 : */
        if (!(-0x1A5 < speed))              /* CONTROL_LEFT' gives movn instead of movz) */
            buttons |= CONTROL_LEFT;
    }
    if (!(0x13F < x))
        buttons |= CONTROL_LEFT;
    if (prevX < 0x230 && x >= 0x230) {
        if (!(speed < 0x1A5))
            buttons |= CONTROL_RIGHT;
    }
    return (x < 0x2C1) ? buttons : (buttons | CONTROL_RIGHT);
}

/* (? name) The 0x6F8 wheel's 10 buttons as raw bits, then the hat (as
 * usbC294Buttons). */
// FUNC 0x00230698 usb6F8Buttons
u32 usb6F8Buttons(const u8 *buttons, const int *pov)
{
    u32 raw = buttons[1] != 0;
    u32 hat = *pov;
    if (buttons[3]) raw |= 0x8000;
    if (buttons[0]) raw |= 0x8;
    if (buttons[2]) raw |= 0x4000;
    if (buttons[4]) raw |= 0x4;
    if (buttons[5]) raw |= 0x20;
    if (buttons[6]) raw |= 0x400;
    if (buttons[7]) raw |= 0x2000;
    if (buttons[8]) raw |= 0x800;
    if (buttons[9]) raw |= 0x1000;
    switch (hat) {                          /* table 0x00341B00 */
    case 0: raw |= WHEEL_HAT_UP;                    break;
    case 1: raw |= WHEEL_HAT_UP | WHEEL_HAT_RIGHT;   break;
    case 2: raw |= WHEEL_HAT_RIGHT;                 break;
    case 3: raw |= WHEEL_HAT_DOWN | WHEEL_HAT_RIGHT; break;
    case 4: raw |= WHEEL_HAT_DOWN;                  break;
    case 5: raw |= WHEEL_HAT_DOWN | WHEEL_HAT_LEFT;  break;
    case 6: raw |= WHEEL_HAT_LEFT;                  break;
    case 7: raw |= WHEEL_HAT_UP | WHEEL_HAT_LEFT;    break;
    }
    return raw;
}

/* (? name) As usb46DSteerButtons with the 0x6F8 wheel's limits (0x11F /
 * 0x2E1, crossings 0x1A1 / 0x260, speed 0x385). */
// FUNC 0x00230778 usb6F8SteerButtons
u32 usb6F8SteerButtons(int x, int prevX)
{
    int speed = (x - prevX) * 60;
    u32 buttons = 0;
    if (prevX >= 0x1A1 && x < 0x1A1) {
        if (!(-0x385 < speed))
            buttons |= CONTROL_LEFT;
    }
    if (!(0x11F < x))
        buttons |= CONTROL_LEFT;
    if (prevX < 0x260 && x >= 0x260) {
        if (!(speed < 0x385))
            buttons |= CONTROL_RIGHT;
    }
    return (x < 0x2E1) ? buttons : (buttons | CONTROL_RIGHT);
}

/* The enhanced engine (backend.md B3, ENH_INPUT_WHEEL_MENU_STEER; Lupo 2 L2
 * 0x0023F348): a wheel's left / right for the menus from its position
 * alone, outside a dead zone around the centre 0x200 (GT3: fixed limits and
 * a quick-swing test, above).  Caller: translate. */
u32 usbWheelSteerButtons(int x, int deadZone)
{
    u32 buttons = (x < 0x200 - deadZone) ? CONTROL_LEFT : 0;

    if (0x200 + deadZone < x)
        buttons |= CONTROL_RIGHT;
    return buttons;
}

/* PC (2026-11-10, user: with the wheel, the tuning menu's sliders' "L3 maps
 * correctly but R3 doesn't do anything"): the 0xC294 wheel's raw word
 * (usbC294Buttons) as a PS pad's (PADRAW_*) for the menus
 * (MenuControl::emulatePressure, T214) and the time trial's SELECT (the
 * ghost, RaceSolitaire::virtual_28, T139).  Retail's menus read any device's
 * raw word as a pad's, so the wheel's buttons came out as others: L3 (4) as
 * L1, R3 (0x20) as circle, L1 (1) as L2, L2 (2) as R2, R2 (0x10) as
 * triangle, triangle (0x400) as R3, the hat's up / down / right as cross /
 * square / L3 (the rest as bits the menus do not read).  Here each is its
 * own, but for the wheel's L3 / R3, which are the menus' L1 / R1 (the
 * sliders' ten steps, the garage's long jumps): its L1 / R1 paddles are
 * already the menus' up / down (translate: 1 / 8), and a direction held
 * keeps the shoulder buttons out (MenuControl::updateKeys). */
u32 usbC294MenuRaw(u32 raw)
{
    static const struct {
        u32 wheel, pad;
    } kMenuRaw[] = {
        {C294RAW_CROSS, PADRAW_CROSS},       {C294RAW_SQUARE, PADRAW_SQUARE},
        {C294RAW_CIRCLE, PADRAW_CIRCLE},     {C294RAW_TRIANGLE, PADRAW_TRIANGLE},
        {C294RAW_L2, PADRAW_L2},             {C294RAW_R2, PADRAW_R2},
        {C294RAW_L3, PADRAW_L1},             {C294RAW_R3, PADRAW_R1},
        {C294RAW_SELECT, PADRAW_SELECT},     {C294RAW_START, PADRAW_START},
        {WHEEL_HAT_UP, PADRAW_UP},           {WHEEL_HAT_DOWN, PADRAW_DOWN},
        {WHEEL_HAT_LEFT, PADRAW_LEFT},       {WHEEL_HAT_RIGHT, PADRAW_RIGHT},
    };
    u32 pad = 0;
    for (const auto &m : kMenuRaw) {
        if (raw & m.wheel)
            pad |= m.pad;
    }
    return pad;
}

/* Callers: PadManager::init (device 0 / 1), IEEE1394Port::init (0). */
// FUNC 0x002307F0 USBSteeringPort::init
void USBSteeringPort::init(int usbPort)
{
    status = PORT_NONE;
    this->usbPort = usbPort;
    clearReport();
    resetCentring();
    actCount = 4;
    /* PC (2026-11-20): the four forces at FF_NONE from the start - left 0
     * (never set before the first stopActuators), the first update after
     * the wheel connected sent 0 in all four slots, each the most force
     * (four times it summed): with the force feedback played, a jolt to one
     * side at each connect (the replay harness's first command, +4.03) */
    stopActuators();
    /* The enhanced engine (backend.md B3): a Logitech wheel's menu dead
     * zone until onConnect sets the device's (an i.LINK node's port, whose
     * report comes from the link frames, never connects locally) */
    steerDeadZone = 0x90;
    steerStep = (0x200 - 0x90) / 7;
}

/* 0xC295 is taken as the GT Force (if-converted: movn).  The stores in
 * retail's order. */
// FUNC 0x00230830 USBSteeringPort::onConnect
void USBSteeringPort::onConnect(u16 vendor, u16 product)
{
    if (product == USB_PRODUCT_C295)
        product = USB_PRODUCT_GTFORCE;
    st.id.vendor  = vendor;
    st.connected  = 1;
    st.id.product = product;
    /* The enhanced engine (backend.md B3; Lupo 2 L2 0x0023F4B8): the menus'
     * dead zone of the wheel and the size of one repeat level (read only
     * with ENH_INPUT_WHEEL_MENU_STEER) */
    if (vendor == USB_VENDOR_LOGITECH)
        steerDeadZone = 0x90;
    else if (vendor == USB_VENDOR_6F8)
        steerDeadZone = 0xC0;
    else
        steerDeadZone = 0;
    steerStep = (0x200 - steerDeadZone) / 7;
}

// FUNC 0x00230858 USBSteeringPort::onDisconnect
void USBSteeringPort::onDisconnect()
{
    st.connected = 0;
    resetCentring();
}

/* Not st.connected.  One counter for the three loops (check_dbra_loop
 * reverses them). */
// FUNC 0x00230878 USBSteeringPort::clearReport
void USBSteeringPort::clearReport()
{
    st.z  = 0;
    st.y  = 0;
    st.x  = 0;
    st.rz = 0;
    st.ry = 0;
    st.rx = 0;
    prevX = 0;
    int i;
    for (i = 0; i < 2; i++)
        st.slider[i] = 0;
    for (i = 0; i < 1; i++)
        st.pov[i] = 0;
    for (i = 0; i < 16; i++)
        st.buttons[i] = 0;
    st.id.vendor  = 0;
    st.id.product = 0;
    report        = 0;
}

/* centred first: egcs then stores centreState first, as retail. */
// FUNC 0x00230928 USBSteeringPort::resetCentring
void USBSteeringPort::resetCentring()
{
    centred     = 0;
    centreState = 0;
    springSetup = 0;                        /* the enhanced engine: the spring set-up again (B2; L2 0x0023F5A0) */
}

/* The enhanced engine (backend.md B2; Lupo 2 L2 0x0023F5B8): one step a
 * frame while a Logitech wheel connects, each taken when the driver accepts
 * the command - the default spring off in all four slots (0xF0), set to
 * K1 = K2 = 3 with clip 0x1E, on in slot 0x20 (step 3: done).  From then
 * on enableSpring switches it: a weak centring in the menus and in pause,
 * none while racing (RaceInput::setMode). */
void USBSteeringPort::setupSpring()
{
    switch (springSetup) {
    case 0:
        if (ff_default_spring(usbPort, 0xF0, 0) == 0)
            springSetup = 1;
        break;
    case 1:
        if (ff_set_default_spring(usbPort, 0xF0, 3, 3, 0x1E) == 0)
            springSetup = 2;
        break;
    case 2:
        if (ff_default_spring(usbPort, 0x20, 1) == 0) {
            springSetup = 3;
            printf("[enhanced] wheel %d: default spring set up (on)\n", usbPort);
        }
        break;
    }
}

/* The enhanced engine (backend.md B2; Lupo 2 L2 0x0023F680): the wheel's
 * own centring spring (slot 0x20) on / off. */
void USBSteeringPort::enableSpring(bool on)
{
    ff_default_spring(usbPort, 0x20, on != 0);
}

/* The enhanced engine (Lupo 2 L2 0x0023F7F8; no caller there either): a
 * damper force of strength k in slot 0x80, downloaded and played. */
void USBSteeringPort::setDamper(int k, int unused)
{
    FfCmd cmd;

    ff_send_download_play(usbPort, ff_build_damper(&cmd, 0x80, k, k, 0, 0));
}

/* The enhanced engine (backend.md B3; Lupo 2 L2 0x0023FFD8): how far the
 * connected wheel is turned beyond the menus' dead zone, as a repeat level
 * 1..8 (Control::getRepeat adds it in eighths of a frame); 0 inside the
 * dead zone, disconnected, or the Driving Force Pro (no menu steering). */
int USBSteeringPort::getSteerLevel()
{
    int off;

    if (st.connected == 0)
        return 0;
    off = st.x - 0x200;
    if (off < 0)
        off = -off;
    off -= steerDeadZone;
    if (st.id.product == USB_PRODUCT_DFP || off < 0)
        return 0;
    return ((off / steerStep) & 7) + 1;
}

/* (? name) The centring sequence of a Logitech wheel, one step a frame,
 * with a constant force on all four values: 0 follow the position until the
 * wheel is near the centre, 1 force 0 for 12 frames (centreLeft = x), 2
 * back to the centre, 3 force 0xFF for 12 frames (centreRight = x), 4 back
 * to the centre: done (centred).  An unknown state sends force 0.  abs()
 * is the builtin (bltzl; negu).  The command's address in a pointer is an
 * exact form, not the original: retail keeps &cmd in $t0 from the start
 * (`&cmd' at the call: 95.6%). */
// FUNC 0x00230938 USBSteeringPort::autoCentre
void USBSteeringPort::autoCentre(const PjoyState *r)
{
    FfCmd cmd;
    FfCmd *p = &cmd;                        /* (the exact form, see above) */
    int force = 0;
    int pos = r->x >> 2;                    /* the wheel's 10 bits as 8 */

    switch ((u32)centreState) {
    case 0:
        force = pos;
        if (__builtin_abs(force - FF_NONE) < 0x20) {
            centreCount = 0;
            centreState = 1;
        }
        break;
    case 1:
        force = FF_LEFT;
        if (centreCount++ >= 0xB) {
            centreCount = 0;
            centreState = 2;
        }
        centreLeft = r->x;
        break;
    case 2:
        force = pos;
        if (__builtin_abs(force - FF_NONE) < 0x20)
            centreState = 3;
        break;
    case 3:
        force = FF_RIGHT;
        if (centreCount++ >= 0xB) {
            centreCount = 0;
            centreState = 4;
        }
        centreRight = r->x;
        break;
    case 4:
        force = pos;
        if (__builtin_abs(force - FF_NONE) < 0x20) {
            centreState = 0;
            centred     = 1;
        }
        break;
    }
    ff_send_download_play(usbPort, ff_build_constant(p, force, force, force, force));
}

/* (? name) Once a frame (PadManager::update).  A removed device
 * disconnects; a new one connects once its id is known (a Logitech wheel
 * after its centring sequence; the 0x6F8 wheel needs none).  The driver's
 * result in a switch: case PJOY_STATE_BUSY (5) gives retail's compare tree
 * (`== 3', `> 3', `== 0').  One counter for the copy loops. */
// FUNC 0x00230A90 USBSteeringPort::update
void USBSteeringPort::update()
{
    PjoyState state;
    PjoyInfo  info;

    switch (pjoy_read_state(usbPort, &state)) {
    case PJOY_STATE_REMOVED:
        if (status == PORT_CONNECTED) {
            status = PORT_NONE;
            onDisconnect();
        }
        return;
    case PJOY_STATE_OK:
        break;
    case PJOY_STATE_BUSY:
        return;
    default:
        return;
    }

    if (status == PORT_NONE) {
        pjoy_get_info(usbPort, &info);
        if (info.vendor == USB_VENDOR_6F8) {
            centred = 1;
        } else if (info.vendor == USB_VENDOR_LOGITECH) {
            /* The enhanced engine (Lupo 2 L2 0x0023FA88): first the default
             * spring's set-up (backend.md B2), and the Driving Force Pro
             * centres itself (B1) */
            if (springSetup != 3) {
                setupSpring();
                return;
            }
            if (info.product == USB_PRODUCT_DFP) {
                centred = 1;
                printf("[enhanced] wheel %d: Driving Force Pro (USB type 4)\n", usbPort);
            } else if (centred == 0) {
                /* PC (2026-11-20): the PC's wheel (engine/input/usb_pc.cpp: a
                 * Driving Force, 0xC294, built from the player's) needs no
                 * centring sequence - its steering is mapped and calibrated
                 * already, and centreLeft / centreRight are read nowhere - and
                 * with its force feedback played on the real wheel now, the
                 * sequence's full forces swung it to both locks at each
                 * connect.  autoCentre stays for the record. */
                centred = 1;
            }
        }
        onConnect(info.vendor, info.product);
    }
    status = PORT_CONNECTED;

    Control unused;                         /* (? constructed and destroyed, never used) */
    if (g_00352CE0 != 0 && st.id.product == USB_PRODUCT_GTFORCE) {
        usbApplyCalibration(&getGameStatus()->options.config,     /* GameStatus +0x30570 */
                            usbPort != 0, CONFIG_WHEEL_GTFORCE, &state);
    }
#if ENH_MENU_WHEEL_CALIBRATION
    /* The enhanced engine (backend.md B4 c with menus_b.md 11, the menus'
     * switch): the 0xC294's calibration as well (Lupo 2 L2 0x0023FB20) */
    else if (g_00352CE0 != 0 && st.id.product == USB_PRODUCT_C294) {
        usbApplyCalibration(&getGameStatus()->options.config,
                            usbPort != 0, CONFIG_WHEEL_C294, &state);
    }
#endif
    prevX = st.x;
    st.x  = state.x;
    st.y  = state.y;
    st.z  = state.z;
    st.rx = state.rx;
    st.ry = state.ry;
    st.rz = state.rz;
    int i;
    for (i = 0; i < 2; i++)
        st.slider[i] = state.slider[i];
    for (i = 0; i < 1; i++)
        st.pov[i] = state.pov[i];
    for (i = 0; i < 16; i++)
        st.buttons[i] = state.buttons[i];
    report = state.unk_34;
    if (actCount > 0) {
        FfCmd cmd;
        ff_send_download_play(usbPort, ff_build_constant(&cmd, act[0], act[1], act[2], act[3]));
        stopActuators();
    }
}

/* Index: USBSteeringPort::virtual_1.  Disconnected: type, buttons and
 * channels 0 (the mask is left as it is). */
// FUNC 0x00230D10 USBSteeringPort::translate
void USBSteeringPort::translate(Control &c)
{
    c.status = status;
    if (status == PORT_NONE) {
        c.type = 0;
        c.curButtons.buttons = 0;
        for (int ch = CHAN_COUNT - 1; ch >= 0; ch--)
            c.channels[ch] = 0;
        return;
    }
    if (status != PORT_CONNECTED)
        return;

    c.type = USB_WHEEL_OTHER << USB_TYPE_SHIFT;
    u32 raw = 0;
    u32 buttons = 0;
    if (st.id.vendor == USB_VENDOR_LOGITECH
        && (u16)(st.id.product - USB_PRODUCT_GTFORCE) < 2) {     /* 0xC293, 0xC294 */
        if (st.id.product == USB_PRODUCT_GTFORCE) {
            c.type = USB_WHEEL_GTFORCE << USB_TYPE_SHIFT;
            raw = usbGTForceButtons(st.buttons);
        } else {
            c.type = USB_WHEEL_C294 << USB_TYPE_SHIFT;
            raw = usbC294Buttons(st.buttons, st.pov);
        }
#if ENH_INPUT_WHEEL_MENU_STEER
        buttons = usbWheelSteerButtons(st.x, steerDeadZone);     /* Lupo 2 (L2 0x0023FE20) */
#else
        buttons = usb46DSteerButtons(st.pov, st.x, prevX);
#endif
    } else if (st.id.vendor == USB_VENDOR_LOGITECH && st.id.product == USB_PRODUCT_DFP) {
        /* The enhanced engine (backend.md B1; Lupo 2 L2 0x0023FDB8): the
         * Driving Force Pro, USB type 4, the 0xC294's buttons with four of
         * them moved (1 -> 4, 8 -> 0x20, 2 -> 1, 0x10 -> 8), no menu
         * steering from the wheel */
        u32 b;

        c.type = USB_WHEEL_DFP << USB_TYPE_SHIFT;
        b = usbC294Buttons(st.buttons, st.pov);
        raw = b & ~0x1C;
        if (b & 0x1)  raw |= 0x4;
        if (b & 0x8)  raw |= 0x20;
        if (b & 0x2)  raw |= 0x1;
        if (b & 0x10) raw |= 0x8;
    } else if (st.id.vendor == USB_VENDOR_6F8 && st.id.product == USB_PRODUCT_6F8_WHEEL) {
        c.type = USB_WHEEL_6F8 << USB_TYPE_SHIFT;
        raw = usb6F8Buttons(st.buttons, st.pov);
#if ENH_INPUT_WHEEL_MENU_STEER
        buttons = usbWheelSteerButtons(st.x, steerDeadZone);     /* Lupo 2 (L2 0x0023FE60) */
#else
        buttons = usb6F8SteerButtons(st.x, prevX);
#endif
    }
    if (g_00352CE0 != 0) {
        /* the pedal map: a pedal pressed far enough is a button (the
         * pedals rest at 0xFF) */
        if (!(0x3F < st.slider[0])) buttons |= CONTROL_DECIDE;
        if (!(0x3F < st.slider[1])) buttons |= CONTROL_CANCEL;
    }
    if (raw & (WHEEL_HAT_UP | 0x1))   buttons |= CONTROL_UP;
    if (raw & (WHEEL_HAT_DOWN | 0x8)) buttons |= CONTROL_DOWN;
    if (raw & WHEEL_HAT_LEFT)         buttons |= CONTROL_LEFT;
    if (raw & WHEEL_HAT_RIGHT)        buttons |= CONTROL_RIGHT;
    if (raw & 0x4000)                 buttons |= CONTROL_START;
    if (raw & 0x8000)                 buttons |= CONTROL_SELECT;
    if (raw & (0x2000 | 0x800))       buttons |= CONTROL_DECIDE;
    if (raw & (0x1000 | 0x400))       buttons |= CONTROL_CANCEL;
    if (st.id.product == USB_PRODUCT_GTFORCE) {
        /* the GT Force's own bits */
        if (raw & 0x4)  buttons |= CONTROL_START;
        if (raw & 0x2)  buttons |= CONTROL_SELECT;
        if (raw & 0x20) buttons |= CONTROL_DECIDE;
        if (raw & 0x10) buttons |= CONTROL_CANCEL;
    }
    c.curButtons.raw[0]  = raw;
    c.curButtons.buttons = buttons;
    c.channels[CHAN_WHEEL]  = st.x;
    c.channels[CHAN_PEDAL1] = st.slider[0];
    c.channels[CHAN_PEDAL2] = st.slider[1];
}

/* Index: USBSteeringPort::virtual_2. */
// FUNC 0x00230F50 USBSteeringPort::translateSync
void USBSteeringPort::translateSync(Control &c)
{
}

/* Index: USBSteeringPort::virtual_3.  Only channel 2 (the force) is kept. */
// FUNC 0x00230F58 USBSteeringPort::setActuator
void USBSteeringPort::setActuator(int ch, int value)
{
    if (ch == 2)
        act[2] = value;
}

/* Index: USBSteeringPort::virtual_4.  FF_NONE (0x7F) = no force. */
// FUNC 0x00230F70 USBSteeringPort::stopActuators
void USBSteeringPort::stopActuators()
{
    for (int i = 0; i < actCount; i++)
        act[i] = FF_NONE;
}

/* Index: USBSteeringPort::virtual_5. */
// FUNC 0x00230FA8 USBSteeringPort::isReady
int USBSteeringPort::isReady()
{
    return 1;
}

/* (? name) Into a link frame: +0x20..+0x5B (0x3C bytes).  memcpy: the
 * frame is not aligned (retail's last word is lwl / lwr too). */
// FUNC 0x00230FB0 USBSteeringPort::save
u8 *USBSteeringPort::save(u8 *buf)
{
    memcpy(buf, &st, sizeof(USBPortState));
    return buf + sizeof(USBPortState);
}

/* (? name) From a link frame (memcpy, as save); the status follows
 * st.connected. */
// FUNC 0x00231038 USBSteeringPort::load
const u8 *USBSteeringPort::load(const u8 *buf)
{
    memcpy(&st, buf, sizeof(USBPortState));
    status = (ControlStatus)(st.connected != 0);
    return buf + sizeof(USBPortState);
}

/* The accessors are inline in engine/input/T479_usb_steering_port.h (no callers);
 * egcs emits these copies with the vtable, the last-declared first.  They
 * are Lupo 2's linkonce getX .. getReport (0x002D2CC0..0x002D2D08) in the
 * reverse order.  index.txt's lupo2 / lupo2m names for 0x002310D8..
 * 0x00231110 are shifted by one (it has getX at 0x00231110, which returns
 * y): getX must return +0x24, the wheel the report's first word goes to
 * (and getButtons / getPov / getSlider the three arrays). */
// FUNC 0x002310D0 USBSteeringPort::getReport
/* inline: report */

// FUNC 0x002310D8 USBSteeringPort::getButtons
/* inline: st.buttons (index.txt: USBSteeringPort::getPov, shifted) */

// FUNC 0x002310E0 USBSteeringPort::getPov
/* inline: st.pov */

// FUNC 0x002310E8 USBSteeringPort::getSlider
/* inline: st.slider */

// FUNC 0x002310F0 USBSteeringPort::getRZ
/* inline: st.rz (index.txt: getRY, shifted) */

// FUNC 0x002310F8 USBSteeringPort::getRY
/* inline: st.ry (index.txt: getRX, shifted) */

// FUNC 0x00231100 USBSteeringPort::getRX
/* inline: st.rx (index.txt: getZ, shifted) */

// FUNC 0x00231108 USBSteeringPort::getZ
/* inline: st.z (index.txt: getY, shifted) */

// FUNC 0x00231110 USBSteeringPort::getY
/* inline: st.y (index.txt: getX, shifted) */

// FUNC 0x00231118 USBSteeringPort::getX
/* inline: st.x */


// FUNC 0x00231170 USBSteeringPort::~USBSteeringPort
/* implicit: ~PortBase; Index: USBSteeringPort::virtual_0.  Implicit
 * (engine/input/T479_usb_steering_port.h declares none); egcs emits this copy for
 * the vtable: only PortBase::~PortBase with the caller's flags. */
