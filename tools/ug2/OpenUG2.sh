#!/bin/bash
# PORTMASTER: openug2, OpenUG2.sh
# OpenUG2 (clean-room NFSU2 engine, https://github.com/whoismept/OpenUG2) on R36S.
# 目录布局 (/roms/ports/openug2/):
#   nfsu2             二进制 (ug2-r36s/build.sh, aarch64 GLES2)
#   nfsu2-pc-data/    NFSU2 PC 版数据 (CARS/ TRACKS/ GLOBAL/ FRONTEND/ ...)
#   args.txt          可选: 额外命令行参数(一行), 例如 --car RX7
#   env.txt           可选: VIEW_DIST=300 (绘制距离米数, 默认 400; 常驻场景随之缩小, 省内存)
#   log.txt           上次运行日志
PORTNAME="OpenUG2"
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
GAMEDIR="/$directory/ports/openug2"
DATA="$GAMEDIR/nfsu2-pc-data"
if [[ ! -d "$DATA/TRACKS" || ! -f "$GAMEDIR/nfsu2" ]]; then
  echo "Missing nfsu2 binary or nfsu2-pc-data/ (CARS TRACKS GLOBAL ...) in roms/ports/openug2." > $CUR_TTY
  sleep 5
  $ESUDO systemctl restart oga_events &
  printf "\033c" >> $CUR_TTY
  exit 1
fi
# --- 与 nfs8 同样的温控/IO 设置，退出时恢复 ---
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
$ESUDO chmod 666 /dev/uinput
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# Mali blob: KMSDRM + vendor EGL/GLES (与 nfs8 相同)
export SDL_VIDEODRIVER=kmsdrm
export SDL_VIDEO_EGL_DRIVER=libEGL.so
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_AUDIODRIVER=alsa
# 可选覆盖: openug2/env.txt (KEY=VALUE 行)
[ -f "$GAMEDIR/env.txt" ] && set -a && . "$GAMEDIR/env.txt" && set +a
# 额外参数: openug2/args.txt (一行)
EXTRA=""
[ -f "$GAMEDIR/args.txt" ] && EXTRA="$(head -n1 "$GAMEDIR/args.txt")"
# gptokeyb 只负责热键退出(Select+Start); 手柄由 SDL 直接读取
$GPTOKEYB "nfsu2" &
{
  echo "[launcher] $(date +%T) nfsu2 md5 $(md5sum nfsu2 | cut -c1-12) args: --track STREAML4RA --resolution 640x480 --view-dist ${VIEW_DIST:-400} $EXTRA"
  echo "[launcher] MemAvailable before: $(awk '/MemAvailable/{print $2}' /proc/meminfo) kB"
  # ALSA underrun 在加载期会刷屏，过滤掉免得日志爆 SD 卡
  ./nfsu2 "$DATA" --track STREAML4RA --resolution 640x480 --view-dist ${VIEW_DIST:-400} $EXTRA 2>&1 | grep --line-buffered -v "snd_pcm_recover"
} > log.txt 2>&1
restore_thermal
$ESUDO kill -9 $(pidof gptokeyb)
$ESUDO systemctl restart oga_events &
printf "\033c" >> $CUR_TTY
