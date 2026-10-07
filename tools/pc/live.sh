#!/usr/bin/env bash
# Interactive run: tools/pc/live.sh [--auto] [ENV=VAL ...]
#   --auto   drive into a race with a dense A-press script (no pad needed),
#            then keep running so knobs can be flipped live.
#   (default) no pad script, no timeout -- you drive.
# Log: build/live/run.log. While it runs: tools/pc/knob.sh to tune, -s to look.
set -u
. "$(dirname "$0")/env.sh"
AUTO=0
for a in "$@"; do
    [ "$a" = "--auto" ] && AUTO=1 && shift && break
done
mkdir -p "$BUILD/live"; cd "$BUILD/live"
: > /tmp/nfsu2.knobs
if [ "$AUTO" = 1 ]; then
    # Dense A presses (1/s for 60 s) drive title -> menus -> race; the game
    # keeps running afterwards so knobs (POKE_VA/POKE_VAL/DUMP_DRAWS) can be
    # flipped live. Times from the first pad read, like MENU_PAD.
    PAD=$(python3 -c "print(','.join(f'{1000+i*1000}:a:150' for i in range(60)))")
    exec env "${GAME_ENV[@]}" RECOMP_PAD_SCRIPT="$PAD" "${PC_ENV[@]}" "$@" "$BIN" > run.log 2>&1
fi
exec env "${GAME_ENV[@]}" "${PC_ENV[@]}" "$@" "$BIN" > run.log 2>&1
