#!/usr/bin/env bash
# Interactive run: tools/pc/live.sh [ENV=VAL ...]   (no pad script, no timeout)
# Log: build/live/run.log. While it runs: tools/pc/knob.sh to tune, -s to look.
set -u
. "$(dirname "$0")/env.sh"
mkdir -p "$BUILD/live"; cd "$BUILD/live"
: > /tmp/nfsu2.knobs
exec env "${GAME_ENV[@]}" "${PC_ENV[@]}" "$@" "$BIN" > run.log 2>&1
