#!/bin/bash
# per-thread CPU% of the game over a window: thr.sh <secs>
P=$(pgrep -x nfsu2_recomp | head -1); [ -n "$P" ] || exit 1
W=${1:-10}; HZ=$(getconf CLK_TCK)
declare -A a
for t in /proc/$P/task/*; do id=${t##*/}; a[$id]=$(awk '{print $14+$15}' $t/stat); done
sleep $W
for t in /proc/$P/task/*; do id=${t##*/}; n=$(cat $t/comm); b=$(awk '{print $14+$15}' $t/stat); d=$((b-${a[$id]:-0})); printf "%6.1f%%  %s (tid %s) cpu%s\n" $(echo "$d*100/$HZ/$W" | bc -l) "$n" $id $(awk '{print $39}' $t/stat); done | sort -rn | head -10
