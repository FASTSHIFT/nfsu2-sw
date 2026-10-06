#!/usr/bin/env bash
# Same-menu A/B: tools/pc/gpuab.sh <tag> [ENV=VAL ...]   (~75 s, unattended)
# Runs tools/pc/run.sh, samples the 55-67 s window (menus with MENU_PAD):
# menu fps, share of time the GL thread waits on a GPU fence, and the
# desktop's wake-up latency (1 ms sleep overshoot). Appends to build/gpuab.txt.
# Example: tools/pc/gpuab.sh svga PC_GPU=1 ; tools/pc/gpuab.sh nopersist RECOMP_GL_PERSIST=0
set -u
. "$(dirname "$0")/env.sh"
TAG="$1"; shift
bash "$(dirname "$0")/run.sh" "gab-$TAG" 75 "$@" > /dev/null 2>&1 &
RP=$!
sleep 55
P=$(game_pid)
python3 - 12 > /tmp/gab-lat <<'EOF' &
import sys, time
end = time.monotonic() + float(sys.argv[1]); d = []
while time.monotonic() < end:
    t = time.monotonic(); time.sleep(0.001); d.append((time.monotonic() - t - 0.001) * 1e6)
d.sort(); n = len(d)
print(f"lat p50={d[n//2]:.0f} p99={d[int(n*.99)]:.0f}us")
EOF
LP=$!; FW=0; TOT=0
for i in $(seq 1 100); do
    for t in /proc/$P/task/*; do
        [ "$(cat "$t/wchan" 2>/dev/null)" = dma_fence_default_wait ] && FW=$((FW + 1))
    done
    TOT=$((TOT + 1)); sleep 0.1
done
wait $LP; wait $RP 2>/dev/null
FPS=$(grep -E "^\[fps\]" "$BUILD/gab-$TAG/run.log" | sed -n '5,7p' | awk '{s+=$2;n++}END{if(n)printf "%.1f", s/n}')
printf "%-10s fps(menu)=%-5s fence_wait=%3d%%  %s  [%s]\n" "$TAG" "$FPS" $((FW * 100 / TOT)) \
    "$(cat /tmp/gab-lat)" "$*" | tee -a "$BUILD/gpuab.txt"
rm -f /tmp/gab-lat
