#!/usr/bin/env bash
# Unattended run: tools/pc/run.sh <run-dir> <seconds> [ENV=VAL ...]
# Drives the menus with RECOMP_PAD_SCRIPT (MENU_PAD in env.sh), dumps every
# 300th frame as BMP, kills the game after <seconds>. Output in build/<run-dir>/.
# A is mashed (MASH, env.sh) until the game logs "[AUTOTEST] HUD live" -- the
# first update of the in-race HUD -- so the time to the race is whatever the
# menus take. HUD_STOP=<n>: end the run n s after that line instead of waiting
# out <seconds> (<seconds> stays the cap).
set -u
. "$(dirname "$0")/env.sh"
DIR="$BUILD/$1"; SECS="$2"; shift 2
rm -rf "$DIR"; mkdir -p "$DIR"; cd "$DIR"
# With the mash the timed MENU_PAD script is off: it has Start presses at
# 15/25/35/65 s, and a Start in the race is the pause menu (a PC run found
# every frame after 65 s paused). MASH=0 brings the script back.
MASH_ENV=(); PAD="$MENU_PAD"
[ "$MASH" != 0 ] && { MASH_ENV=(RECOMP_PAD_MASH="$MASH"); PAD=""; }
env "${GAME_ENV[@]}" RECOMP_GL_DUMP="$DIR/f,300" RECOMP_PAD_SCRIPT="$PAD" \
    "${MASH_ENV[@]}" "${PC_ENV[@]}" "$@" "$BIN" > run.log 2>&1 &
PID=$!
T0=$SECONDS; SEEN=
while [ $((SECONDS - T0)) -lt "$SECS" ]; do
    sleep 1
    if [ -z "$SEEN" ] && grep -q "AUTOTEST\] HUD live" run.log 2>/dev/null; then
        SEEN=$SECONDS
        echo "race reached after $((SEEN - T0)) s: $(grep -m1 'AUTOTEST\] HUD live' run.log)"
    fi
    [ -n "$SEEN" ] && [ -n "${HUD_STOP:-}" ] && [ $((SECONDS - SEEN)) -ge "$HUD_STOP" ] && break
done
[ -z "$SEEN" ] && [ "$MASH" != 0 ] && echo "HUD never went live in $SECS s (still in the menus?)"
kill -9 "$PID" 2>/dev/null
P=$(game_pid); [ -n "$P" ] && kill -9 "$P"
wait 2>/dev/null
grep -E "^\s*\[GL\] (OpenGL ES|.* / .* / )|shader failed|link failed|missing entry|\[CRASH\]|FATAL|HalReturn" run.log | head -20
grep -E "^\[fps\]" run.log | tr '\n' ' '; echo
ls "$DIR" | grep -c bmp
