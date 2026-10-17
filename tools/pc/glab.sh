#!/usr/bin/env bash
# Picture equivalence of a GL-layer change, inside ONE running game.
#   tools/pc/glab.sh <run-dir> [KNOB=optimised-off-value] [ENV=VAL ...]
#
# Two runs of the game never draw the same frame (the sim follows the wall
# clock), so frames cannot be compared across runs. Here the game runs once; after
# the race HUD is live a knob is flipped between its two values around frame
# dumps (knob DUMP_SEQ -> /tmp/glab_<n>.bmp), in the order  A B A B A B  (A = the
# optimised path, B = the knob set to 1, e.g. GL_LEGACY=1). tools/pc/glab_cmp.py then
# compares the same-mode pairs (the scene's own change between dumps: the control)
# with the A/B pairs. Equivalent = the A/B differences are no bigger than the control's
# (0 on a static scene).
#
# The knob changes are applied 2 s before each dump so no frame mixes both paths.
# The race is held still by not pressing anything after the HUD is live; the countdown
# grid is static apart from the camera, which is why the dumps are taken as close in time as
# the 1 s knob poll allows.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/env.sh"
DIR="$BUILD/$1"; KNOB="${2:-GL_LEGACY}"; shift; [ $# -gt 0 ] && shift
rm -rf "$DIR"; mkdir -p "$DIR"; cd "$DIR"
rm -f /tmp/glab_*.bmp
F=/tmp/nfsu2.knobs
: > "$F"
# FIXED_S=<secs>: no race -- idle at the boot/title screens for that long, then A/B there
# (a nearly static scene: the only case where the control pairs come out ~0 px).
if [ -n "${FIXED_S:-}" ]; then PADS=""; MSH="0"; else PADS="${PAUSE_AT:-60000}:start:300"; MSH="$MASH"; fi
env "${GAME_ENV[@]}" RECOMP_PAD_SCRIPT="$PADS" RECOMP_PAD_MASH="$MSH" "${PC_ENV[@]}" "$@" "$BIN" > run.log 2>&1 &
PID=$!
T0=$SECONDS
if [ -n "${FIXED_S:-}" ]; then
    while [ $((SECONDS - T0)) -lt "$FIXED_S" ]; do sleep 1; done
else
    while [ $((SECONDS - T0)) -lt 240 ]; do
        sleep 1
        grep -q "AUTOTEST\] HUD live" run.log 2>/dev/null && break
    done
    if ! grep -q "AUTOTEST\] HUD live" run.log 2>/dev/null; then
        echo "HUD never went live"; kill -9 "$PID" 2>/dev/null; exit 1
    fi
    echo "race reached after $((SECONDS - T0)) s"
    # the pad script presses Start once (PAUSE_AT ms after the first pad read)
    while [ $((SECONDS - T0)) -lt $((${PAUSE_AT:-60000} / 1000 + 12)) ]; do sleep 1; done
fi
n=0
for round in 1 2 3; do
    for v in 0 1; do
        n=$((n + 1))
        echo "$KNOB=$v" > "$F"; sleep 2.5          # apply, let a few frames go by
        echo "$KNOB=$v" > "$F"; echo "DUMP_SEQ=$n" >> "$F"; sleep 2.5
        echo "dump $n: $KNOB=$v"
    done
done
: > "$F"
kill -9 "$PID" 2>/dev/null
P=$(game_pid); [ -n "$P" ] && kill -9 "$P"
wait 2>/dev/null
cp /tmp/glab_*.bmp "$DIR"/ 2>/dev/null
python3 "$HERE/glab_cmp.py" "$DIR" 6
