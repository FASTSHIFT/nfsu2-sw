/* xinput_nx.h -- Switch pad reader (see xinput_nx.c). Plain C types only. */
#ifndef XINPUT_NX_H
#define XINPUT_NX_H

#include <stdint.h>

/* Indices into the Xbox analog-button array (XBOX_BUTTON_* order). */
enum { NX_XBOX_A, NX_XBOX_B, NX_XBOX_X, NX_XBOX_Y,
       NX_XBOX_BLACK, NX_XBOX_WHITE, NX_XBOX_LT, NX_XBOX_RT };

/* Fill the Xbox digital mask, the eight analog buttons and the four stick
 * axes (LX, LY, RX, RY) for `port`. Returns 0 when nothing is connected. */
int xbox_nx_pad_read(unsigned port, uint16_t *digital, uint8_t analog[8],
                     int16_t thumbs[4]);

#endif
