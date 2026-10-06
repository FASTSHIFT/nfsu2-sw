#!/usr/bin/env bash
# Per-thread CPU and page faults of the running game: tools/pc/thr.sh [secs]
. "$(dirname "$0")/env.sh"
P=$(game_pid); S=${1:-3}
[ -z "$P" ] && { echo "game not running"; exit 1; }
snap() {
    for t in /proc/$P/task/*; do
        st=$(cat "$t/stat" 2>/dev/null) || continue
        n=$(tr ' ' _ < "$t/comm")
        set -- ${st#*) }          # after ')': state ... minflt=$8 majflt=$10 utime=$12 stime=$13
        echo "${t##*/} $n $8 ${10} ${12} ${13}"
    done
}
snap > /tmp/thr0; sleep "$S"; snap > /tmp/thr1
awk -v s="$S" 'NR==FNR{f[$1]=$3;m[$1]=$4;u[$1]=$5;k[$1]=$6;next}
  ($1 in f){df=($3-f[$1])/s; dm=($4-m[$1])/s; du=($5-u[$1])/s; dk=($6-k[$1])/s;
  if (df+du+dk>2) printf "%-14s user%%=%-4d sys%%=%-4d minflt/s=%-7d majflt/s=%d\n",$2,du,dk,df,dm}' /tmp/thr0 /tmp/thr1
rm -f /tmp/thr0 /tmp/thr1
