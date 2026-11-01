/*
 * usb_gamepad.c -- Xbox controllers, as the console's USB stack expects them.
 *
 * The standard requests follow the USB 1.1 specification; the rest is the
 * controller's own interface: class 0x58 subclass 0x42 (not HID, so there
 * is no report descriptor), the XID vendor requests XAPI uses to recognise
 * a controller, and a fixed 20-byte input report.
 *
 * Each emulated pad `dev` is driven by the host pad on the same port of the
 * xbox_input layer (on the Switch, the console's own controllers).
 */
#include "usb_gamepad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../input/xinput_xbox.h"

/* ── Descriptors ────────────────────────────────────────────────────────── */

static const uint8_t k_device_descriptor[18] = {
    18, 0x01,           /* length, DEVICE */
    0x10, 0x01,         /* USB 1.10 */
    0x00, 0x00, 0x00,   /* class, subclass, protocol: per interface */
    0x08,               /* endpoint 0 packet size */
    0x5E, 0x04,         /* vendor 0x045E (Microsoft) */
    0x89, 0x02,         /* product 0x0289 (Controller S) */
    0x21, 0x01,         /* device release */
    0x00, 0x00, 0x00,   /* no strings */
    0x01,               /* one configuration */
};

/* Configuration, interface and both interrupt endpoints, returned together
 * for GET_DESCRIPTOR(CONFIGURATION). */
static const uint8_t k_configuration_descriptor[32] = {
    9, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 50,      /* configuration, 100 mA */
    9, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,  /* interface 0x58/0x42 */
    7, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,              /* EP1 IN: reports */
    7, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04,              /* EP2 OUT: rumble */
};

/* XID: what kind of controller this is, and its report sizes. */
static const uint8_t k_xid_descriptor[16] = {
    0x10, 0x42,         /* length, XID */
    0x00, 0x01,         /* XID 1.00 */
    0x01, 0x02,         /* gamepad, Controller S */
    20, 6,              /* input and output report sizes */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,     /* no alternate products */
};

/* Capabilities: the reports with every supported field all ones (the id and
 * length bytes are values, not flags). */
static const uint8_t k_input_capabilities[20] = {
    0x00, 20, 0xFF, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t k_output_capabilities[6] = { 0x00, 6, 0xFF, 0xFF, 0xFF, 0xFF };

/* ── Device state ───────────────────────────────────────────────────────── */

static struct {
    uint8_t  address;           /* from SET_ADDRESS, 0 before */
    uint8_t  configuration;     /* from SET_CONFIGURATION */
    uint16_t rumble[2];         /* last speeds sent to the host pad */
    int      rumble_sent;
} g_pad[USB_GAMEPADS];

uint8_t usb_gamepad_address(int dev)
{
    return g_pad[dev].address;
}

void usb_gamepad_reset(int dev)
{
    g_pad[dev].address = 0;
    g_pad[dev].configuration = 0;
}

int usb_gamepad_connected(int dev)
{
    XBOX_INPUT_STATE probe;

    /* Pad 0 is always on the bus; the others are when a host pad is there. */
    return dev == 0 || xbox_InputGetState((DWORD)dev, &probe) == 0;
}

/* ── Control transfers ──────────────────────────────────────────────────── */

enum {
    GET_STATUS = 0x00, CLEAR_FEATURE = 0x01, SET_FEATURE = 0x03, SET_ADDRESS = 0x05,
    GET_DESCRIPTOR = 0x06, GET_CONFIGURATION = 0x08, SET_CONFIGURATION = 0x09,
    SET_INTERFACE = 0x0B,
};
enum { TYPE_STANDARD = 0, TYPE_CLASS = 1, TYPE_VENDOR = 2 };
enum { CLASS_GET_REPORT = 0x01, CLASS_SET_REPORT = 0x09 };
enum { XID_GET_CAPABILITIES = 0x01 };

/* Send at most what was asked for and what fits. */
static int reply(uint8_t *out, int max, const void *data, int size, uint16_t asked)
{
    int n = size;
    if (n > asked) n = asked;
    if (n > max)   n = max;
    if (n > 0)     memcpy(out, data, (size_t)n);
    return n;
}

static int standard_request(int dev, const UsbSetup *s, int device_to_host, uint8_t *out, int max)
{
    switch (s->bRequest) {
    case GET_DESCRIPTOR:
        switch (s->wValue >> 8) {
        case 0x01:
            return reply(out, max, k_device_descriptor, (int)sizeof k_device_descriptor, s->wLength);
        case 0x02:
            return reply(out, max, k_configuration_descriptor,
                         (int)sizeof k_configuration_descriptor, s->wLength);
        default:
            return -1;          /* no strings: a stall is the expected answer */
        }
    case SET_ADDRESS:
        g_pad[dev].address = (uint8_t)(s->wValue & 0x7F);
        return 0;
    case SET_CONFIGURATION:
        g_pad[dev].configuration = (uint8_t)s->wValue;
        return 0;
    case GET_CONFIGURATION:
        if (!device_to_host || max < 1)
            return -1;
        out[0] = g_pad[dev].configuration;
        return 1;
    case GET_STATUS:
        if (!device_to_host || max < 2)
            return -1;
        out[0] = out[1] = 0;    /* bus powered, no remote wakeup */
        return 2;
    case CLEAR_FEATURE:
    case SET_FEATURE:
    case SET_INTERFACE:
        return 0;
    default:
        return -1;
    }
}

int usb_gamepad_control(int dev, const UsbSetup *setup, uint8_t *out, int max)
{
    int device_to_host = (setup->bmRequestType & 0x80) != 0;
    int type = (setup->bmRequestType >> 5) & 3;

    if (type == TYPE_STANDARD)
        return standard_request(dev, setup, device_to_host, out, max);

    if (type == TYPE_CLASS) {
        /* XAPI asks for the input report over the control pipe too. A stall
         * there halts the endpoint and the pad is never polled again, so it
         * gets the same report the interrupt endpoint sends. */
        if (setup->bRequest == CLASS_GET_REPORT && device_to_host) {
            uint8_t report[20];
            int n = usb_gamepad_report(dev, report, (int)sizeof report);
            return n > 0 ? reply(out, max, report, n, setup->wLength) : -1;
        }
        /* The rumble report sent over the control pipe: its data stage goes
         * to usb_gamepad_output; this is the status stage. */
        if (setup->bRequest == CLASS_SET_REPORT && !device_to_host)
            return 0;
        return -1;
    }

    if (type == TYPE_VENDOR) {
        /* XID: without it XAPI enumerates the device and ignores it. */
        if (!device_to_host)
            return 0;
        if (setup->bRequest == GET_DESCRIPTOR && (setup->wValue >> 8) == 0x42)
            return reply(out, max, k_xid_descriptor, (int)sizeof k_xid_descriptor, setup->wLength);
        if (setup->bRequest == XID_GET_CAPABILITIES) {
            if ((setup->wValue >> 8) == 0x01)
                return reply(out, max, k_input_capabilities,
                             (int)sizeof k_input_capabilities, setup->wLength);
            if ((setup->wValue >> 8) == 0x02)
                return reply(out, max, k_output_capabilities,
                             (int)sizeof k_output_capabilities, setup->wLength);
        }
        return -1;
    }
    return -1;
}

/* ── The input report ───────────────────────────────────────────────────── */

/* RECOMP_PAD_PRESS=<mask>[,<period ms>[,<hold ms>]]: press the digital
 * buttons in <mask> (0x10 is Start) for <hold> ms every <period> ms, on pad
 * 0, to bring a title up with no controller (default 2000 / 250 ms). A
 * repeated edge, not a held level: a "press Start" screen waits for a
 * press, and when it starts reading is not knowable here. */
static uint8_t pressed_by_setting(void)
{
    static int state = -1;      /* -1 not read, 0 off, 1 on */
    static unsigned mask, period, hold;
    static unsigned long start;

    if (state < 0) {
        const char *v = getenv("RECOMP_PAD_PRESS");
        char *end;
        state = 0;
        if (v && *v) {
            mask = (unsigned)strtoul(v, &end, 0) & 0xFFu;
            period = *end == ',' ? (unsigned)strtoul(end + 1, &end, 0) : 2000u;
            hold = *end == ',' ? (unsigned)strtoul(end + 1, &end, 0) : 250u;
            if (!period)
                period = 2000u;
            if (!hold || hold >= period)
                hold = period / 4u;
            if (mask) {
                state = 1;
                start = (unsigned long)GetTickCount();
                fprintf(stderr, "  PAD: pressing 0x%02X for %u ms every %u ms (RECOMP_PAD_PRESS)\n",
                        mask, hold, period);
            }
        }
    }
    if (state != 1)
        return 0;
    return ((unsigned long)GetTickCount() - start) % period < hold ? (uint8_t)mask : 0;
}

/* RECOMP_INPUT_DIAG: what pad 0 reads, once a second. */
static void input_diagnostic(uint8_t pressed)
{
    static int on = -1;
    static unsigned long last, reports, pressed_reports;
    unsigned long now;
    XBOX_INPUT_STATE probe;
    DWORD rc;

    if (on < 0)
        on = getenv("RECOMP_INPUT_DIAG") != NULL;
    if (!on)
        return;
    reports++;
    pressed_reports += pressed != 0;
    now = (unsigned long)GetTickCount();
    if (now - last <= 1000)
        return;
    last = now;
    rc = xbox_InputGetState(0, &probe);
    fprintf(stderr, "  [INPUT] pad 0: state %lu, buttons 0x%04X, A/B/X/Y %02X %02X %02X %02X;"
                    " %lu reports, %lu with RECOMP_PAD_PRESS\n",
            (unsigned long)rc, rc == 0 ? probe.Gamepad.wButtons : 0,
            rc == 0 ? probe.Gamepad.bAnalogButtons[0] : 0, rc == 0 ? probe.Gamepad.bAnalogButtons[1] : 0,
            rc == 0 ? probe.Gamepad.bAnalogButtons[2] : 0, rc == 0 ? probe.Gamepad.bAnalogButtons[3] : 0,
            reports, pressed_reports);
}

static void put16(uint8_t *at, int16_t v)
{
    at[0] = (uint8_t)((uint16_t)v & 0xFF);
    at[1] = (uint8_t)((uint16_t)v >> 8);
}

/*
 * The 20-byte report:
 *   0       report id, 0          1       length, 20
 *   2       digital buttons       3       reserved
 *   4..11   A B X Y Black White, left and right triggers (0..255)
 *   12..19  left X, left Y, right X, right Y (signed 16-bit, little endian)
 * A pad with no host pad behind it is still on the bus: nothing pressed.
 */
int usb_gamepad_report(int dev, uint8_t *out, int max)
{
    XBOX_INPUT_STATE state;
    uint8_t pressed = dev == 0 ? pressed_by_setting() : 0;
    int i;

    if (max < 20)
        return 0;
    memset(out, 0, 20);
    out[1] = 20;
    if (dev == 0)
        input_diagnostic(pressed);
    out[2] = pressed;
    if (xbox_InputGetState((DWORD)dev, &state) != 0)
        return 20;

    out[2] |= (uint8_t)(state.Gamepad.wButtons & 0xFF);
    out[3] = (uint8_t)(state.Gamepad.wButtons >> 8);
    for (i = 0; i < 8; i++)
        out[4 + i] = state.Gamepad.bAnalogButtons[i];
    put16(&out[12], state.Gamepad.sThumbLX);
    put16(&out[14], state.Gamepad.sThumbLY);
    put16(&out[16], state.Gamepad.sThumbRX);
    put16(&out[18], state.Gamepad.sThumbRY);
    return 20;
}

/* ── The output report: rumble ──────────────────────────────────────────── */

/* Report id 0, length 6, then the left and right motor speeds as
 * little-endian words. The title sends the same speeds about every frame,
 * and on the Switch each change is a call to the hid service, so only
 * changes are passed on. RECOMP_RUMBLE_TRACE logs them. */
void usb_gamepad_output(int dev, const uint8_t *data, int len)
{
    static int trace = -1;
    XBOX_VIBRATION v;

    if (len < 6 || data[0] != 0x00 || data[1] < 6)
        return;
    v.wLeftMotorSpeed = (uint16_t)(data[2] | data[3] << 8);
    v.wRightMotorSpeed = (uint16_t)(data[4] | data[5] << 8);
    if (g_pad[dev].rumble_sent && v.wLeftMotorSpeed == g_pad[dev].rumble[0]
            && v.wRightMotorSpeed == g_pad[dev].rumble[1])
        return;
    g_pad[dev].rumble[0] = v.wLeftMotorSpeed;
    g_pad[dev].rumble[1] = v.wRightMotorSpeed;
    g_pad[dev].rumble_sent = 1;
    if (trace < 0)
        trace = getenv("RECOMP_RUMBLE_TRACE") != NULL;
    if (trace)
        fprintf(stderr, "  PAD%d: rumble %04X %04X\n", dev + 1, v.wLeftMotorSpeed, v.wRightMotorSpeed);
    xbox_InputSetState((DWORD)dev, &v);
}
