#!/usr/bin/env bash
# RenderDoc capture: tools/pc/rdoc.sh <name> [ENV=VAL ...]
# Same env as run.sh (llvmpipe + spin throttles under VMware, pad script to a
# race), launched under renderdoccmd. Press F12 (or PrtScr) in the game window
# to capture; captures go to trace/<name>_frameN.rdc.
# Example: tools/pc/rdoc.sh lowq RECOMP_POKE=0x3CC5E8:0
set -u
. "$(dirname "$0")/env.sh"
NAME="$1"; shift
mkdir -p "$ROOT/../../trace" "$BUILD/rdoc-$NAME"
cd "$BUILD/rdoc-$NAME"
exec env "${GAME_ENV[@]}" RECOMP_PAD_SCRIPT="$MENU_PAD" "${PC_ENV[@]}" "$@" \
    renderdoccmd capture -w -c "$(cd "$ROOT/../.." && pwd)/trace/$NAME" "$BIN"
