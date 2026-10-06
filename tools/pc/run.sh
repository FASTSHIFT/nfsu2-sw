#!/usr/bin/env bash
# Unattended run: tools/pc/run.sh <run-dir> <seconds> [ENV=VAL ...]
# Drives the menus with RECOMP_PAD_SCRIPT (MENU_PAD in env.sh), dumps every
# 300th frame as BMP, kills the game after <seconds>. Output in build/<run-dir>/.
set -u
. "$(dirname "$0")/env.sh"
DIR="$BUILD/$1"; SECS="$2"; shift 2
rm -rf "$DIR"; mkdir -p "$DIR"; cd "$DIR"
env "${GAME_ENV[@]}" RECOMP_GL_DUMP="$DIR/f,300" RECOMP_PAD_SCRIPT="$MENU_PAD" \
    "${PC_ENV[@]}" "$@" "$BIN" > run.log 2>&1 &
PID=$!
sleep "$SECS"
kill -9 "$PID" 2>/dev/null
P=$(game_pid); [ -n "$P" ] && kill -9 "$P"
wait 2>/dev/null
grep -E "^\s*\[GL\] (OpenGL ES|.* / .* / )|shader failed|link failed|missing entry|\[CRASH\]|FATAL|HalReturn" run.log | head -20
grep -E "^\[fps\]" run.log | tr '\n' ' '; echo
ls "$DIR" | grep -c bmp
