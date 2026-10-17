#!/bin/bash
# 设备端测试运行器(放在 /roms/ports/openug2/)。停掉 EmulationStation 释放 DRM，
# 跑完(或超时)后一定把它拉起来。用法: device_test.sh <日志名> <nfsu2 参数...>
# 例:  device_test.sh shot --track STREAML4RA --car HUMMER --resolution 640x480 --shot /tmp/ug2.png --frames 1
D=/roms/ports/openug2
NAME=${1:-run}; shift
LOG=$D/$NAME.log
restore() { sudo systemctl start emulationstation; }
trap restore EXIT
sudo systemctl stop emulationstation
sleep 1
cd "$D"
export SDL_VIDEODRIVER=kmsdrm SDL_VIDEO_EGL_DRIVER=libEGL.so SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_AUDIODRIVER=alsa
{
  echo "[test] $(date +%T) args: $*"
  echo "[test] mem before: $(grep -E 'MemAvailable' /proc/meminfo)"
  ./nfsu2 "$D/nfsu2-pc-data" "$@" &
  P=$!
  # 每 2 秒记录峰值/当前内存，进程结束或 ${TMAX:-300}s 超时
  T0=$SECONDS
  while kill -0 $P 2>/dev/null; do
    echo "[mem] t=$((SECONDS-T0))s $(grep -E 'VmHWM|VmRSS' /proc/$P/status | tr -s ' \n' ' ')avail=$(awk '/MemAvailable/{print $2}' /proc/meminfo)kB"
    [ $((SECONDS-T0)) -gt ${TMAX:-300} ] && { echo "[test] timeout, killing"; kill $P; break; }
    sleep 2
  done
  wait $P; echo "[test] exit=$? after $((SECONDS-T0))s"
} > "$LOG" 2>&1
