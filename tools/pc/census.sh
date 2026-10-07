#!/usr/bin/env bash
# Live draw census: tools/pc/census.sh [--poke VA:VAL] [--poke VA:VAL ...]
#   --poke 0x399498:0   hold a guest global at a value (flip live, no restart)
#   (no args)           just dump the current frame's draw census
# Reads build/live/run.log (live.sh) and prints the census line.
set -u
. "$(dirname "$0")/env.sh"
LOG="$BUILD/live/run.log"
KNOBS=/tmp/nfsu2.knobs

# Apply any --poke args to the knobs file (last one wins per VA).
: > "$KNOBS"
for a in "$@"; do
    case "$a" in
        --poke=*) va="${a#--poke=}"; va="${va%%:*}"; val="${a##*:}"
                  printf "POKE_VA=%s\nPOKE_VAL=%s\n" "$va" "$val" >> "$KNOBS" ;;
        --poke)   shift; va="$1"; va="${va%%:*}"; val="${va##*:}"
                  printf "POKE_VA=%s\nPOKE_VAL=%s\n" "$va" "$val" >> "$KNOBS" ;;
    esac
done

# Trigger the census, wait a moment, then show it.
printf "DUMP_DRAWS=1\n" >> "$KNOBS"
sleep 2
grep "draw census" "$LOG" | tail -1
echo "---"
grep -A3 "draw census" "$LOG" | tail -4