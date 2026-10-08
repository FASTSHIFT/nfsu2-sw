#!/usr/bin/env bash
# A/B runs of the race baseline on the R36S: r36s/exp.sh [-d] NAME[:KEY=VAL[,KEY=VAL]] ...
#
#   R36S_HOST=192.168.10.241 r36s/exp.sh -d base rdctl:RECOMP_GL_EXP_RD=1 rd:RECOMP_GL_EXP_RD=2
#
# Each argument is one `autotest.py --baseline` run (the game mashes A into a
# race, 420 frames, hotkey exit) with the given extra env.txt lines. -d deploys
# the binary before the first run. Prints one table row per run: the steady
# state (last 100 frames, median) from build-r36s/autotest/<ts>/report.json.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEPLOY=1; [ "${1:-}" = "-d" ] && { DEPLOY=0; shift; }
printf "%-10s %7s %7s %7s %6s %6s %7s  %s\n" run ms gl wait dc frag calls report
for spec in "$@"; do
    name="${spec%%:*}"; envs=()
    if [ "$spec" != "$name" ]; then
        IFS=, read -ra kv <<< "${spec#*:}"
        for e in "${kv[@]}"; do envs+=(--env "$e"); done
    fi
    skip=(--skip-deploy); [ "$DEPLOY" = 0 ] && { skip=(); DEPLOY=1; }
    out=$(python3 "$HERE/autotest.py" --baseline "${skip[@]}" "${envs[@]}" 2>&1)
    dir=$(echo "$out" | grep -o "artifacts in .*" | sed 's/artifacts in //')
    if [ -z "$dir" ] || [ ! -f "$dir/report.json" ]; then
        echo "$name: FAILED"; echo "$out" | tail -5; continue
    fi
    python3 - "$name" "$dir" <<'EOF'
import json, sys
name, d = sys.argv[1:3]
r = json.load(open(d + "/report.json"))
b = r.get("baseline")
if not b:
    print(f"{name}: {r.get('verdict')}")
else:
    s = b["steady"]
    print("%-10s %7.1f %7.1f %7.1f %6.0f %6.0f %7.0f  %s" % (
        name, *(s[k]["p50"] for k in ("ms", "gl", "wait", "dc", "frag", "calls")), d))
EOF
done
