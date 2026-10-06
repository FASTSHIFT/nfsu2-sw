#!/usr/bin/env bash
# Live tunables (xboxrecomp/src/platform/knobs.h), re-read by the game every second:
#   tools/pc/knob.sh KEY=VAL ...   replace the file (no args: back to defaults)
#   tools/pc/knob.sh -s            the file, recent [knob]/[wake]/[fps] lines
# Keys: SPIN_WAKE_EVERY SPIN_YIELD_EVERY SPIN_YIELD_IDLE HW_SLEEP_US GIL_WAIT_MS FPS_CAP
. "$(dirname "$0")/env.sh"
F=/tmp/nfsu2.knobs
L="$BUILD/live/run.log"
if [ "${1:-}" = "-s" ]; then
    echo "--- $F"; cat "$F" 2>/dev/null
    [ -f "$L" ] && { grep -E "^\[knob\]" "$L" | tail -5; grep -E "^\[wake\]" "$L" | tail -3; grep -E "^\[fps\]" "$L" | tail -2; }
    exit 0
fi
: > "$F"
for kv in "$@"; do echo "$kv" >> "$F"; done
echo "--- $F"; cat "$F"
