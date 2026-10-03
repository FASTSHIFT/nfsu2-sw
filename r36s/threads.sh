#!/bin/sh
# On the R36S, while the game runs: CPU % per thread over N seconds (default
# 10), plus clocks and temperature. Copy to the device and run as ark.
#   sh threads.sh [secs]
P=$(pidof nfsu2_recomp) || { echo "not running"; exit 1; }
S=${1:-10}
HZ=100
snap() { for t in /proc/$P/task/*; do
    echo "$(basename $t) $(awk '{print $14+$15}' $t/stat) $(cat $t/comm)"; done; }
snap > /tmp/th0; sleep "$S"; snap > /tmp/th1
echo "cpu MHz: $(($(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)/1000))" \
     " gpu MHz: $(($(cat /sys/class/devfreq/ff400000.gpu/cur_freq)/1000000))" \
     " temp: $(($(cat /sys/class/thermal/thermal_zone0/temp)/1000))C"
join /tmp/th0 /tmp/th1 | awk -v s="$S" -v hz=$HZ \
    '{ d = ($4 - $2) * 100 / (s * hz); if (d >= 1) printf "%6.1f%%  %s %s\n", d, $1, $3 }' | sort -rn | head -15
