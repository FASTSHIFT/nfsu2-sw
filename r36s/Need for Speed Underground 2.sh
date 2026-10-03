#!/bin/bash
# PORTMASTER: nfs8, Need for Speed Underground 2.sh
# NFSU2 (Xbox NTSC-U) static recompilation -- https://github.com/antoxa2584x/nfsu2-sw
# Port dir layout (/roms/ports/nfs8/):
#   nfsu2_recomp     the binary (r36s/build.sh)
#   game/            the extracted disc: default.xbe, NFSUNDER/, B3/, ... (UDATA = saves)
#   save/            Xbox partition images (created on first run)
#   log.txt          last run's log

PORTNAME="Need for Speed Underground 2"

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
get_controls

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY

GAMEDIR="/$directory/ports/nfs8"

if [[ ! -f "$GAMEDIR/game/default.xbe" ]]; then
  echo "Missing game files. Copy the extracted Xbox disc to roms/ports/nfs8/game." > $CUR_TTY
  sleep 5
  $ESUDO systemctl restart oga_events &
  printf "\033c" >> $CUR_TTY
  exit 1
fi

# --- Thermal management: stay under the 70C passive trip (AGENT.md §3.3) ---
# Unlike reVC the GPU is not pinned low: this title is CPU-bound and the
# GL thread needs every bit of the Mali it can get. Restored on exit.
sudo sh -c 'echo 1209600 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq; \
  echo 2048 > /sys/block/mmcblk0/queue/read_ahead_kb; \
  echo 10 > /proc/sys/vm/swappiness'
restore_thermal() {
  sudo sh -c 'echo 1512000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq; \
    echo 128 > /sys/block/mmcblk0/queue/read_ahead_kb; \
    echo 60 > /proc/sys/vm/swappiness'
}
trap restore_thermal EXIT

cd "$GAMEDIR"
mkdir -p save
# A build pushed while the game ran (the running binary cannot be replaced).
if [ -f nfsu2_recomp.new ]; then
  cp -f nfsu2_recomp nfsu2_recomp.prev.bak && mv -f nfsu2_recomp.new nfsu2_recomp
fi
$ESUDO chmod 666 /dev/uinput
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# Mali blob: KMSDRM + the vendor EGL/GLES (libEGL.so.1 is glvnd, AGENT.md §3.1).
export SDL_VIDEODRIVER=kmsdrm
export SDL_VIDEO_EGL_DRIVER=libEGL.so
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export NFSU2_GAME_DIR="$GAMEDIR/game"
export NFSU2_SAVE_DIR="$GAMEDIR/save"
# The pad drives the game directly through SDL; no keyboard stand-in.
export RECOMP_KEYBOARD=0
# Face buttons by position, like reVC's REVC_SNES_PAD: the R36S's bottom
# button (B) is Xbox A (confirm/accelerate menus), the right one (A) is Xbox
# B (back). =label to go by the printed letters instead.
export RECOMP_PAD_LAYOUT=position
# Audio: straight to ALSA (no PulseAudio here; SDL's pulse probe spams the log
# and stalls), through ~/.asoundrc's dmix. Deeper host queue: the A35 runs
# the APU behind the wall clock in busy scenes.
export SDL_AUDIODRIVER=alsa
export RECOMP_AUDIO_BLOCKS=16
# The Switch tuning (PERF_NOTES.md): GL calls on their own thread, the game
# builds frame N+1 while the GPU draws N, the main thread and the audio mixer
# get the guest lock first.
export RECOMP_GL_THREAD=1
export RECOMP_FRAME_LAG=1
export RECOMP_GIL_EAGER=1
# Which surfaces a frame draws into (one log line per 300 frames), to pick
# passes for RECOMP_GL_SKIP=<WxH>,... (docs/02 §7.2).
export RECOMP_GL_SURF_STATS=1
# Optional overrides without editing this file: nfs8/env.txt, KEY=VALUE lines.
[ -f "$GAMEDIR/env.txt" ] && set -a && . "$GAMEDIR/env.txt" && set +a
# 4:3 panel: tell the game the TV is 4:3 instead of letterboxing 16:9.
export RECOMP_WIDESCREEN=0
# Periodic kernel/GPU summaries: noise on an SD card log.
export RECOMP_QUIET=1
export RECOMP_FPS_LOG=1

# Which binary to run (A/B builds side by side, docs/03): NFSU2_BIN in env.txt.
BIN="${NFSU2_BIN:-nfsu2_recomp}"
[ -f "$BIN" ] || BIN=nfsu2_recomp
# gptokeyb only for the hotkey exit (the pad itself goes through SDL).
$GPTOKEYB "$BIN" &
{ echo "[launcher] binary $BIN md5 $(md5sum "$BIN" | cut -c1-12)"; ./"$BIN"; } 2>&1 | tee log.txt

restore_thermal
$ESUDO kill -9 $(pidof gptokeyb)
$ESUDO systemctl restart oga_events &
printf "\033c" >> $CUR_TTY
