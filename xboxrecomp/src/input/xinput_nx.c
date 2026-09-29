/*
 * xinput_nx.c -- the Switch's own controllers as the Xbox gamepad.
 *
 * Read with libnx's pad API rather than SDL. The face buttons go by label
 * by default -- Switch A is Xbox A -- because that is what the title's
 * on-screen prompts name: a positional layout makes "(B) Back" the Switch's
 * A button, and a player pressing B in a modal box (NFSU2's Help, opened
 * with Y) sees the game ignore them. RECOMP_PAD_LAYOUT=position (settable in
 * nfsu2x_env.txt) swaps to the Xbox positions instead: A<->B, X<->Y. Kept in
 * its own file because <switch.h> and the Win32 vocabulary the rest of the
 * runtime uses define some of the same names.
 *
 *   Xbox            Switch (default)   Switch (position)
 *   A               A                  B
 *   B               B                  A
 *   X               X                  Y
 *   Y               Y                  X
 *   White / Black   L / R
 *   LT / RT         ZL / ZR   (digital on the Switch: 0 or 255)
 *   Start / Back    + / -
 *   sticks, clicks, D-pad as they are
 *
 * Handheld mode and the first player's controller (Joy-Con pair or Pro
 * Controller) both drive port 0, and both get the title's rumble
 * (xbox_nx_pad_rumble): the Xbox's left, heavy motor as the low band, its
 * right, light motor as the high band, on both sides of the pad.
 */
#ifdef __SWITCH__
#include <switch.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xinput_nx.h"

static PadState s_pad;
static int      s_ready;
static Mutex    s_lock;
static int      s_positional;

int xbox_nx_pad_read(unsigned port, uint16_t *digital, uint8_t analog[8],
                     int16_t thumbs[4])
{
    u64 b;
    HidAnalogStickState l, r;

    if (port != 0)
        return 0;
    mutexLock(&s_lock);
    if (!s_ready) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&s_pad);
        {
            const char *layout = getenv("RECOMP_PAD_LAYOUT");
            s_positional = layout && strcmp(layout, "position") == 0;
        }
        s_ready = 1;
    }
    padUpdate(&s_pad);
    b = padGetButtons(&s_pad);
    l = padGetStickPos(&s_pad, 0);
    r = padGetStickPos(&s_pad, 1);
    mutexUnlock(&s_lock);

    if (!padIsConnected(&s_pad))
        return 0;

    *digital = 0;
    if (b & HidNpadButton_Up)     *digital |= 0x0001;
    if (b & HidNpadButton_Down)   *digital |= 0x0002;
    if (b & HidNpadButton_Left)   *digital |= 0x0004;
    if (b & HidNpadButton_Right)  *digital |= 0x0008;
    if (b & HidNpadButton_Plus)   *digital |= 0x0010;   /* Start */
    if (b & HidNpadButton_Minus)  *digital |= 0x0020;   /* Back */
    if (b & HidNpadButton_StickL) *digital |= 0x0040;
    if (b & HidNpadButton_StickR) *digital |= 0x0080;

    memset(analog, 0, 8);
    if (s_positional) {
        analog[NX_XBOX_A] = (b & HidNpadButton_B)  ? 255 : 0;
        analog[NX_XBOX_B] = (b & HidNpadButton_A)  ? 255 : 0;
        analog[NX_XBOX_X] = (b & HidNpadButton_Y)  ? 255 : 0;
        analog[NX_XBOX_Y] = (b & HidNpadButton_X)  ? 255 : 0;
    } else {
        analog[NX_XBOX_A] = (b & HidNpadButton_A)  ? 255 : 0;
        analog[NX_XBOX_B] = (b & HidNpadButton_B)  ? 255 : 0;
        analog[NX_XBOX_X] = (b & HidNpadButton_X)  ? 255 : 0;
        analog[NX_XBOX_Y] = (b & HidNpadButton_Y)  ? 255 : 0;
    }
    analog[NX_XBOX_BLACK] = (b & HidNpadButton_R)  ? 255 : 0;
    analog[NX_XBOX_WHITE] = (b & HidNpadButton_L)  ? 255 : 0;
    analog[NX_XBOX_LT]    = (b & HidNpadButton_ZL) ? 255 : 0;
    analog[NX_XBOX_RT]    = (b & HidNpadButton_ZR) ? 255 : 0;

    /* libnx sticks are +-32767 with +y up, the Xbox's convention too. */
    thumbs[0] = (int16_t)l.x;
    thumbs[1] = (int16_t)l.y;
    thumbs[2] = (int16_t)r.x;
    thumbs[3] = (int16_t)r.y;
    return 1;
}

/* Vibration handles for one input source, re-fetched when its style changes
 * (Joy-Con pair swapped for a Pro Controller, say). */
typedef struct {
    HidNpadIdType            id;
    u32                      style;     /* HidNpadStyleTag, 0 = none */
    s32                      count;
    HidVibrationDeviceHandle handles[2];
} NxRumbleTarget;

static int nx_rumble_refresh(NxRumbleTarget *t, int active)
{
    u32 styles = active ? hidGetNpadStyleSet(t->id) : 0;
    u32 style = 0;
    s32 count = 2;

    if (t->id == HidNpadIdType_Handheld) {
        if (styles & HidNpadStyleTag_NpadHandheld) style = HidNpadStyleTag_NpadHandheld;
    } else if (styles & HidNpadStyleTag_NpadFullKey) {
        style = HidNpadStyleTag_NpadFullKey;
    } else if (styles & HidNpadStyleTag_NpadJoyDual) {
        style = HidNpadStyleTag_NpadJoyDual;
    } else if (styles & HidNpadStyleTag_NpadJoyLeft) {
        style = HidNpadStyleTag_NpadJoyLeft;  count = 1;
    } else if (styles & HidNpadStyleTag_NpadJoyRight) {
        style = HidNpadStyleTag_NpadJoyRight; count = 1;
    }
    if (style != t->style) {
        t->style = style;
        t->count = 0;
        if (style && R_SUCCEEDED(hidInitializeVibrationDevices(t->handles,
                                        count, t->id, (HidNpadStyleTag)style)))
            t->count = count;
    }
    return t->count;
}

void xbox_nx_pad_rumble(unsigned port, uint16_t left, uint16_t right)
{
    static NxRumbleTarget targets[2] = {
        { HidNpadIdType_Handheld, 0, 0, {{0}} },
        { HidNpadIdType_No1,      0, 0, {{0}} },
    };
    static int disabled = -1;
    HidVibrationDeviceHandle handles[4];
    HidVibrationValue values[4];
    HidVibrationValue v;
    int active[2];
    s32 n = 0;
    int i, k;

    if (port != 0)
        return;
    if (disabled < 0) {
        const char *e = getenv("RECOMP_RUMBLE");
        disabled = e && strcmp(e, "0") == 0;
    }
    if (disabled)
        return;

    mutexLock(&s_lock);
    if (!s_ready) {                       /* no pad read yet: nothing to shake */
        mutexUnlock(&s_lock);
        return;
    }
    active[0] = padIsHandheld(&s_pad);
    active[1] = padIsNpadActive(&s_pad, HidNpadIdType_No1);

    /* Standard HD rumble resonances: 160 Hz low band, 320 Hz high band. */
    v.amp_low   = (float)left  / 65535.0f;
    v.freq_low  = 160.0f;
    v.amp_high  = (float)right / 65535.0f;
    v.freq_high = 320.0f;
    for (i = 0; i < 2; i++) {
        int c = nx_rumble_refresh(&targets[i], active[i]);
        for (k = 0; k < c; k++) {
            handles[n] = targets[i].handles[k];
            values[n] = v;
            n++;
        }
    }
    if (n)
        hidSendVibrationValues(handles, values, n);
    mutexUnlock(&s_lock);
}
#endif
