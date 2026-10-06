/**
 * autotest.c -- boot-time runtime patches for unattended runs (docs/05, L3).
 *
 * RECOMP_SKIP_INTRO=1 drops the EA logo and the THX bumper before the title
 * runs. They are "movie screens" whose NotificationMessage only answers the
 * video-finished message (GameCube build: THXMovieScreen:: and
 * EAlogoBumperScreen::NotificationMessage both `if (a2 == -1013576007)` ->
 * HaltMoviePlayer=1 -> ChangeToNextBootFlowScreen); they never check a button,
 * so they cannot be skipped by hand and must play out (~10 s each).
 *
 * The boot flow is a table of screen-name pointers the game walks at boot to
 * build its screen list (Xbox: sub_000F05A0 walks 0x39FC60.. and 0x39FC80..,
 * skipping any entry whose string's first byte is 0). Two copies of the table
 * hold the EA logo and THX, so point both entries at a byte of the .data BSS
 * (always zero at run time): the walk then sees an empty name and skips the
 * screen. The two skipped movies collapse to the language-select/PSA screens,
 * which do answer button presses.
 *
 * This is a pure runtime memory patch (xbox_GetMemoryBase), not a gen-code
 * change: regenerating the lifted C leaves it in place.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <xbox/xboxrecomp.h>

static void autotest_skip_intro(void)
{
    static const uint32_t entries[] = {
        0x0039FC60u, 0x0039FC64u,   /* first table: EA logo, THX */
        0x0039FC84u, 0x0039FC88u,   /* second table: EA logo, THX */
    };
    const uint32_t zero = 0x003D8B08u;   /* .data BSS: zero at run time */
    uint8_t *mem;
    unsigned i;
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("RECOMP_SKIP_INTRO");
        on = e && *e == '1';
    }
    if (!on)
        return;
    mem = (uint8_t *)xbox_GetMemoryBase();
    if (!mem)
        return;
    for (i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        *(uint32_t *)(mem + entries[i]) = zero;
        fprintf(stderr, "[AUTOTEST] skip intro: table 0x%08X -> empty\n",
                entries[i]);
    }
}

/* Called from game_main() after the memory layout exists but before the title
 * runs -- the boot flow table is walked by the title right after entry. */
void autotest_init(void)
{
    autotest_skip_intro();
}
