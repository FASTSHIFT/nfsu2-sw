#!/bin/bash
# NFSU2 (Xbox recomp) on an RK3566 board (lckfb TaishanPi, Ubuntu 20.04, Rockchip BSP 4.19,
# libmali g52 g2p0). Not part of the R36S build: a measurement harness only.
#
#   /userdata/nfs8/{nfsu2_recomp,game/,save/,run.sh}
#   ssh lckfb@HOST 'cd /userdata/nfs8 && setsid nohup ./run.sh [secs] > /dev/null 2>&1 &'
#   -> log.txt
#
# SDL KMSDRM needs the DRM master, so lightdm is stopped for the run and always
# started again (trap). X11 EGL does not work on this image (DRI2Connect returns
# an empty device name); the GBM path does (Mali-G52, GLES 3.2).
D=/userdata/nfs8
SECS=${1:-240}
cd "$D" || exit 1
G=/sys/class/devfreq/fde60000.gpu; DM=/sys/class/devfreq/dmc
OLDG=$(cat $G/governor); OLDD=$(cat $DM/governor)
# GOV=0 keeps the stock simple_ondemand / dmc_ondemand governors
if [ "${GOV:-1}" = 1 ]; then echo lckfb | sudo -S -p "" sh -c "echo performance > $G/governor; echo performance > $DM/governor"; fi
restore() {
  pactl suspend-sink 0 0 2>/dev/null; pactl suspend-sink 1 0 2>/dev/null   # give PulseAudio its sinks back
  echo lckfb | sudo -S -p "" sh -c "echo $OLDG > $G/governor; echo $OLDD > $DM/governor"
  echo lckfb | sudo -S -p "" systemctl start lightdm
}
trap restore EXIT
echo lckfb | sudo -S -p "" systemctl stop lightdm
sleep 2
# PulseAudio (user session) may hold the HDMI PCM: SDL's ALSA open then fails with
# "Device or resource busy". Suspend its sinks while the game has the device.
pactl suspend-sink 0 1 2>/dev/null; pactl suspend-sink 1 1 2>/dev/null
M=/usr/lib/aarch64-linux-gnu/mali
# The board's SDL (2.30 in /usr/local, 2.0.10 from Ubuntu) has no KMSDRM video driver.
# $D/lib/libSDL2-2.0.so.0 is SDL 2.0.10 built with KMSDRM (tools/rk3566/build_sdl.sh)
# plus SDL_KMSDRM_MODE=WxH (HDMI TVs boot into 4K30; the R36S panel is 640x480).
# The Mali libgbm/libEGL/libGLESv2 must come before Mesa's.
export LD_LIBRARY_PATH="$D/lib:$M${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_KMSDRM_MODE="${MODE:-640x480}"
export SDL_VIDEODRIVER=kmsdrm
export SDL_VIDEO_EGL_DRIVER=$M/libEGL.so.1
export SDL_VIDEO_GL_DRIVER=$M/libGLESv2.so.2
# HDMI audio is ALSA card 1 (rockchiphdmi); card 0 is the analogue rk809 codec.
# plughw converts rate/format if the TV wants something else. AUDIODEV=... overrides.
export SDL_AUDIODRIVER=alsa AUDIODEV="${AUDIODEV:-plughw:1,0}"
export NFSU2_GAME_DIR="$D/game" NFSU2_SAVE_DIR="$D/save"
# same knobs as the R36S launcher (Need for Speed Underground 2.sh)
# USB keyboard as player 1 (src/input/xinput_device.c): arrows = d-pad, Enter = A/START,
# Esc = BACK, W/S throttle/brake (RT/LT), A/D steer, Space = B, Ctrl = Y, Shift = WHITE, Q = BLACK.
# Needs read access to /dev/input/event* (the lckfb user is in the input group) and
# SDL built with libudev (the R36S launcher uses RECOMP_KEYBOARD=0 and the pad).
export RECOMP_KEYBOARD=1 RECOMP_PAD_LAYOUT=position RECOMP_AUDIO_BLOCKS=16
export RECOMP_GL_THREAD=1 RECOMP_FRAME_LAG=1 RECOMP_GIL_EAGER=1 RECOMP_GUEST_ONE_CORE=1
export RECOMP_KMS_PRESENT=1 RECOMP_GL_SURF_STATS=1 RECOMP_WIDESCREEN=0
export RECOMP_QUIET=1 RECOMP_FPS_LOG=1
# measurement: per-frame lines, the game mashes A itself until the race HUD is live
# MASH=1: measurement run, the game presses A/Start itself until the race HUD is live
export RECOMP_PERF_LOG=2
[ "${MASH:-0}" = 1 ] && export RECOMP_PAD_MASH=200,100,7
[ -f "$D/env.txt" ] && set -a && . "$D/env.txt" && set +a
{
  echo "[run] $(date +%T) md5 $(md5sum nfsu2_recomp | cut -c1-12) secs=$SECS"
  echo "[run] cpu0 $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  # per-thread CPU%, clocks and temperatures every 15 s (thr.sh sits next to this script)
  ( while sleep 15; do echo "[sample] $(date +%T) cpu0 $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) gpu $(cat $G/cur_freq) dmc $(cat $DM/cur_freq) soc $(cat /sys/class/thermal/thermal_zone0/temp) gpu_t $(cat /sys/class/thermal/thermal_zone1/temp)"; [ -x ./thr.sh ] && ./thr.sh 5 | head -6 | sed 's/^/[thr] /'; done ) & SAMPLER=$!
  # SIGKILL: the game ignores SIGTERM and a leftover instance skews every later run
  timeout -s KILL "$SECS" ./nfsu2_recomp
  kill $SAMPLER 2>/dev/null
  echo "[run] exit=$? $(date +%T)"
} > log.txt 2>&1
