/*
 * xinput_nx.c -- the Switch's controllers as Xbox gamepads (libnx pad API).
 *
 * BUTTONS. The face buttons go by their printed labels, so the game's
 * on-screen prompts match the buttons pressed: Switch A is Xbox A. With
 * RECOMP_PAD_LAYOUT=position they go by place instead (A<->B, X<->Y).
 *
 *   Xbox            Switch
 *   A B X Y         A B X Y           (by label; by position: B A Y X)
 *   White / Black   L / R
 *   LT / RT         ZL / ZR           (all or nothing: 0 or 255)
 *   Start / Back    + / -
 *   sticks, stick clicks and the D-pad as they are
 *
 * PLAYERS. Two players, made from three slots: handheld, No1 and No2.
 * Without the controller applet, a controller that wakes up takes the
 * lowest free slot, so Joy-Cons detached from a console played handheld
 * land in No1. So: when something is in No2, player 1 is handheld + No1 and
 * player 2 is No2 (or No2 alone is player 1); otherwise handheld and No1,
 * both present, are players 1 and 2; otherwise whatever is there is
 * player 1. A slot shared by one player adds its buttons, and takes the
 * stick pushed further. "[NX] pads:" in the log names each change.
 *
 * SINGLE JOY-CONS. RECOMP_NX_JOYCON=single makes each detached Joy-Con a
 * player of its own, held sideways: the face buttons by Nintendo's A B X Y
 * places (right, bottom, top, left), SL / SR as LT / RT, the Joy-Con's
 * + or - as Start, and the stick turned a quarter with the controller
 * (RECOMP_NX_JOYCON_ROTATE=0: unturned, if the system already does it).
 *
 * RUMBLE. The Xbox's heavy left motor drives the low band (160 Hz) and its
 * light right motor the high band (320 Hz), on every controller of the
 * player. RECOMP_RUMBLE=0 turns it off.
 *
 * In a file of its own: <switch.h> and the runtime's Win32 vocabulary share
 * names.
 */
#ifdef __SWITCH__
#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xinput_nx.h"

enum { PLAYERS = 2, SLOTS = 3 };
enum { SLOT_HANDHELD = 1u << 0, SLOT_NO1 = 1u << 1, SLOT_NO2 = 1u << 2 };

static const struct {
    HidNpadIdType id;
    const char   *name;
} k_slots[SLOTS] = {
    { HidNpadIdType_Handheld, "handheld" },
    { HidNpadIdType_No1,      "No1" },
    { HidNpadIdType_No2,      "No2" },
};

static struct {
    Mutex    lock;
    int      started;
    int      by_position;       /* RECOMP_PAD_LAYOUT=position */
    int      turn_sticks;       /* RECOMP_NX_JOYCON_ROTATE, default on */
    PadState pads[SLOTS];
    unsigned logged[PLAYERS];   /* the assignment last written to the log */
} g_in = { .turn_sticks = 1, .logged = { ~0u, ~0u } };

static int setting_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && strcmp(v, value) == 0;
}

/* Lock held. */
static void input_start(void)
{
    int i;

    g_in.by_position = setting_is("RECOMP_PAD_LAYOUT", "position");
    g_in.turn_sticks = !setting_is("RECOMP_NX_JOYCON_ROTATE", "0");
    padConfigureInput(PLAYERS, HidNpadStyleSet_NpadStandard);
    if (setting_is("RECOMP_NX_JOYCON", "single")) {
        hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
        hidSetNpadJoyAssignmentModeSingleByDefault(HidNpadIdType_No1);
        hidSetNpadJoyAssignmentModeSingleByDefault(HidNpadIdType_No2);
    }
    for (i = 0; i < SLOTS; i++)
        padInitialize(&g_in.pads[i], k_slots[i].id);
    g_in.started = 1;
}

/* Which slots make up each player right now (see the top of the file).
 * Lock held. */
static void assign_players(unsigned players[PLAYERS])
{
    unsigned present = 0;
    int i;

    for (i = 0; i < SLOTS; i++)
        if (hidGetNpadStyleSet(k_slots[i].id))
            present |= 1u << i;

    if (present & SLOT_NO2) {
        unsigned first = present & (SLOT_HANDHELD | SLOT_NO1);
        players[0] = first ? first : SLOT_NO2;
        players[1] = first ? SLOT_NO2 : 0;
    } else if ((present & (SLOT_HANDHELD | SLOT_NO1)) == (SLOT_HANDHELD | SLOT_NO1)) {
        players[0] = SLOT_HANDHELD;
        players[1] = SLOT_NO1;
    } else {
        players[0] = present;
        players[1] = 0;
    }

    if (players[0] != g_in.logged[0] || players[1] != g_in.logged[1]) {
        char line[96];
        int len = snprintf(line, sizeof line, "  [NX] pads:");
        int p;
        for (p = 0; p < PLAYERS; p++) {
            len += snprintf(line + len, sizeof line - (size_t)len, " player %d =", p + 1);
            if (!players[p])
                len += snprintf(line + len, sizeof line - (size_t)len, " none");
            for (i = 0; i < SLOTS; i++)
                if (players[p] & (1u << i))
                    len += snprintf(line + len, sizeof line - (size_t)len, " %s", k_slots[i].name);
            if (p + 1 < PLAYERS)
                len += snprintf(line + len, sizeof line - (size_t)len, ";");
        }
        fprintf(stderr, "%s\n", line);
        g_in.logged[0] = players[0];
        g_in.logged[1] = players[1];
    }
}

/* A button-to-button translation. */
typedef struct {
    u64 from, to;
} ButtonMap;

/* A left Joy-Con turned anticlockwise: its D-pad Down is on the right. */
static const ButtonMap k_left_sideways[] = {
    { HidNpadButton_Down,   HidNpadButton_A },
    { HidNpadButton_Left,   HidNpadButton_B },
    { HidNpadButton_Right,  HidNpadButton_X },
    { HidNpadButton_Up,     HidNpadButton_Y },
    { HidNpadButton_LeftSL, HidNpadButton_ZL },
    { HidNpadButton_LeftSR, HidNpadButton_ZR },
    { HidNpadButton_Minus,  HidNpadButton_Plus },
    { HidNpadButton_StickL, HidNpadButton_StickL },
};

/* A right Joy-Con turned clockwise: its X is on the right. */
static const ButtonMap k_right_sideways[] = {
    { HidNpadButton_X,       HidNpadButton_A },
    { HidNpadButton_A,       HidNpadButton_B },
    { HidNpadButton_Y,       HidNpadButton_X },
    { HidNpadButton_B,       HidNpadButton_Y },
    { HidNpadButton_RightSL, HidNpadButton_ZL },
    { HidNpadButton_RightSR, HidNpadButton_ZR },
    { HidNpadButton_Plus,    HidNpadButton_Plus },
    { HidNpadButton_StickR,  HidNpadButton_StickL },
};

static u64 translate(u64 held, const ButtonMap *map, size_t n)
{
    u64 out = 0;
    size_t i;
    for (i = 0; i < n; i++)
        if (held & map[i].from)
            out |= map[i].to;
    return out;
}

static s32 clamp_axis(s32 v)
{
    return v < -32767 ? -32767 : v;
}

/* A lone Joy-Con held sideways: its buttons as a standard pad's and its
 * stick, turned with it, as the left stick. */
static void sideways(u32 style, u64 *held, HidAnalogStickState *left, HidAnalogStickState *right)
{
    HidAnalogStickState s;
    s32 x, y;

    if (style & HidNpadStyleTag_NpadJoyLeft) {
        s = *left;
        *held = translate(*held, k_left_sideways,
                          sizeof k_left_sideways / sizeof k_left_sideways[0]);
        x = g_in.turn_sticks ? -s.y : s.x;
        y = g_in.turn_sticks ?  s.x : s.y;
    } else {
        s = *right;
        *held = translate(*held, k_right_sideways,
                          sizeof k_right_sideways / sizeof k_right_sideways[0]);
        x = g_in.turn_sticks ?  s.y : s.x;
        y = g_in.turn_sticks ? -s.x : s.y;
    }
    left->x = clamp_axis(x);
    left->y = clamp_axis(y);
    right->x = right->y = 0;
}

static int deflection(HidAnalogStickState s)
{
    return abs(s.x) + abs(s.y);
}

static int is_lone_joycon(u32 style)
{
    const u32 full = HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld
                   | HidNpadStyleTag_NpadJoyDual;
    const u32 half = HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight;
    return !(style & full) && (style & half);
}

/* The Xbox's digital buttons, as XINPUT_GAMEPAD wButtons. */
static const struct {
    u64      nx;
    uint16_t xbox;
} k_digital[] = {
    { HidNpadButton_Up,     0x0001 },
    { HidNpadButton_Down,   0x0002 },
    { HidNpadButton_Left,   0x0004 },
    { HidNpadButton_Right,  0x0008 },
    { HidNpadButton_Plus,   0x0010 },   /* Start */
    { HidNpadButton_Minus,  0x0020 },   /* Back */
    { HidNpadButton_StickL, 0x0040 },
    { HidNpadButton_StickR, 0x0080 },
};

int xbox_nx_pad_read(unsigned port, uint16_t *digital, uint8_t analog[8], int16_t thumbs[4])
{
    HidAnalogStickState left = { 0 }, right = { 0 };
    unsigned players[PLAYERS];
    u64 held = 0;
    size_t k;
    int i;

    if (port >= PLAYERS)
        return 0;
    mutexLock(&g_in.lock);
    if (!g_in.started)
        input_start();
    assign_players(players);
    for (i = 0; i < SLOTS; i++) {
        PadState *pad = &g_in.pads[i];
        HidAnalogStickState sl, sr;
        u64 bits;
        u32 style;

        if (!(players[port] & (1u << i)))
            continue;
        padUpdate(pad);
        bits = padGetButtons(pad);
        sl = padGetStickPos(pad, 0);
        sr = padGetStickPos(pad, 1);
        style = padGetStyleSet(pad);
        if (is_lone_joycon(style))
            sideways(style, &bits, &sl, &sr);
        held |= bits;
        if (deflection(sl) > deflection(left))
            left = sl;
        if (deflection(sr) > deflection(right))
            right = sr;
    }
    mutexUnlock(&g_in.lock);

    if (!players[port])
        return 0;

    *digital = 0;
    for (k = 0; k < sizeof k_digital / sizeof k_digital[0]; k++)
        if (held & k_digital[k].nx)
            *digital |= k_digital[k].xbox;

    {
        /* Face buttons by label, or by place. */
        u64 a = g_in.by_position ? HidNpadButton_B : HidNpadButton_A;
        u64 b = g_in.by_position ? HidNpadButton_A : HidNpadButton_B;
        u64 x = g_in.by_position ? HidNpadButton_Y : HidNpadButton_X;
        u64 y = g_in.by_position ? HidNpadButton_X : HidNpadButton_Y;
        analog[NX_XBOX_A] = (held & a) ? 255 : 0;
        analog[NX_XBOX_B] = (held & b) ? 255 : 0;
        analog[NX_XBOX_X] = (held & x) ? 255 : 0;
        analog[NX_XBOX_Y] = (held & y) ? 255 : 0;
    }
    analog[NX_XBOX_BLACK] = (held & HidNpadButton_R)  ? 255 : 0;
    analog[NX_XBOX_WHITE] = (held & HidNpadButton_L)  ? 255 : 0;
    analog[NX_XBOX_LT]    = (held & HidNpadButton_ZL) ? 255 : 0;
    analog[NX_XBOX_RT]    = (held & HidNpadButton_ZR) ? 255 : 0;

    /* Both use +-32767 with +y up. */
    thumbs[0] = (int16_t)left.x;
    thumbs[1] = (int16_t)left.y;
    thumbs[2] = (int16_t)right.x;
    thumbs[3] = (int16_t)right.y;
    return 1;
}

/* ── Rumble ─────────────────────────────────────────────────────────────── */

/* A slot's vibration devices, looked up again when its controller style
 * changes (a Joy-Con pair swapped for a Pro Controller, say). */
static struct {
    u32                      style;     /* the style they were made for, 0 none */
    s32                      count;
    HidVibrationDeviceHandle devices[2];
} g_rumble[SLOTS];

/* The style to vibrate in slot i, and how many devices it has. */
static u32 rumble_style(int i, s32 *devices)
{
    u32 styles = hidGetNpadStyleSet(k_slots[i].id);

    *devices = 2;
    if (k_slots[i].id == HidNpadIdType_Handheld)
        return styles & HidNpadStyleTag_NpadHandheld;
    if (styles & HidNpadStyleTag_NpadFullKey)
        return HidNpadStyleTag_NpadFullKey;
    if (styles & HidNpadStyleTag_NpadJoyDual)
        return HidNpadStyleTag_NpadJoyDual;
    *devices = 1;
    if (styles & HidNpadStyleTag_NpadJoyLeft)
        return HidNpadStyleTag_NpadJoyLeft;
    if (styles & HidNpadStyleTag_NpadJoyRight)
        return HidNpadStyleTag_NpadJoyRight;
    return 0;
}

static s32 rumble_devices(int i)
{
    s32 count;
    u32 style = rumble_style(i, &count);

    if (style != g_rumble[i].style) {
        g_rumble[i].style = style;
        g_rumble[i].count = 0;
        if (style && R_SUCCEEDED(hidInitializeVibrationDevices(g_rumble[i].devices, count,
                                                               k_slots[i].id, (HidNpadStyleTag)style)))
            g_rumble[i].count = count;
    }
    return g_rumble[i].count;
}

void xbox_nx_pad_rumble(unsigned port, uint16_t left, uint16_t right)
{
    static int off = -1;
    HidVibrationDeviceHandle devices[2 * SLOTS];
    HidVibrationValue values[2 * SLOTS];
    HidVibrationValue v;
    unsigned players[PLAYERS];
    s32 n = 0, k;
    int i;

    if (port >= PLAYERS)
        return;
    if (off < 0)
        off = setting_is("RECOMP_RUMBLE", "0");
    if (off)
        return;

    mutexLock(&g_in.lock);
    if (!g_in.started) {        /* no pad read yet: nothing to shake */
        mutexUnlock(&g_in.lock);
        return;
    }
    assign_players(players);
    v.amp_low   = (float)left / 65535.0f;
    v.freq_low  = 160.0f;
    v.amp_high  = (float)right / 65535.0f;
    v.freq_high = 320.0f;
    for (i = 0; i < SLOTS; i++) {
        s32 count;
        if (!(players[port] & (1u << i)))
            continue;
        count = rumble_devices(i);
        for (k = 0; k < count; k++) {
            devices[n] = g_rumble[i].devices[k];
            values[n] = v;
            n++;
        }
    }
    if (n)
        hidSendVibrationValues(devices, values, n);
    mutexUnlock(&g_in.lock);
}
#endif
