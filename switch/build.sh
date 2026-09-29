#!/usr/bin/env bash
# Build the NFSU2 (Xbox) recompilation as a Switch homebrew NRO and stage an
# SD-card layout:
#
#   <SD>/switch/nfsu2x/nfsu2x.nro
#   <SD>/switch/nfsu2x/game/        the extracted disc: default.xbe, NFSUNDER/, ...
#   <SD>/switch/nfsu2x/save/        created on first run
#   <SD>/switch/nfsu2x/nfsu2x_env.txt   optional KEY=VALUE runtime switches
#   <SD>/switch/nfsu2x/nfsu2x_log.txt   written when no nxlink host is listening
#
# The game reads the unpacked disc files directly -- never the ISO.
#
# Environment:
#   DEVKITPRO        default /opt/devkitpro (switch-dev, switch-sdl2, switch-mesa)
#   XBOXRECOMP_DIR   toolkit (default: the vendored xboxrecomp/)
#   NFSU2_GEN_DIR    lifted C from tools/regen.sh (default /root/nfsu2x/gen)
#   NFSU2_GAME_SRC   extracted disc to stage (default /root/nfsu2x/game)
#   SD_ROOT          staging SD root (default <repo>/switch_sd)
#   BUILD_DIR        default /root/nfsu2x/build-switch
#   JOBS             parallel compile jobs (default: nproc, capped by memory)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
TK="${XBOXRECOMP_DIR:-$REPO/xboxrecomp}"
GEN="${NFSU2_GEN_DIR:-/root/nfsu2x/gen}"
GAME="${NFSU2_GAME_SRC:-/root/nfsu2x/game}"
SD="${SD_ROOT:-$REPO/switch_sd}"
BUILD="${BUILD_DIR:-/root/nfsu2x/build-switch}"

fail() { echo "error: $*" >&2; exit 1; }
[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || fail "devkitPro not found at $DEVKITPRO"
[ -f "$DEVKITPRO/portlibs/switch/include/SDL2/SDL.h" ] || fail "switch-sdl2 missing"
[ -f "$GEN/recomp_funcs.h" ] || fail "no generated code in $GEN (run tools/regen.sh)"
[ -f "$GAME/default.xbe" ] || fail "no extracted disc in $GAME"

if [ -z "${JOBS:-}" ]; then
    mem_mb=$(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1024 ))
    JOBS=$(( mem_mb / 1500 )); [ "$JOBS" -lt 1 ] && JOBS=1
    [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
fi

cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_DIR="$TK" -DNFSU2_GEN_DIR="$GEN" \
    -DNFSU2_SWITCH_ICON="$REPO/assets/icon.jpg" >/dev/null
ninja -C "$BUILD" -j"$JOBS"

DEST="$SD/switch/nfsu2x"
mkdir -p "$DEST/game"
cp "$BUILD/nfsu2_recomp.nro" "$DEST/nfsu2x.nro"
# The disc image is 2.6 GB: copy only what changed.
rsync -a --size-only "$GAME/" "$DEST/game/" --exclude '*_analysis.json'
echo
echo "staged $DEST"
echo "  nfsu2x.nro $(stat -c %s "$DEST/nfsu2x.nro") bytes, game/ $(du -sh "$DEST/game" | cut -f1)"
echo "  copy <SD>/switch/nfsu2x/ to the card (or point Eden's sdmc at $SD),"
echo "  and start it from hbmenu with title takeover (hold R on a game) for full RAM."
