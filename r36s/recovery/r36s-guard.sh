#!/bin/sh
# r36s-guard: keep the R36S recoverable without a power-button reset.
#
#  1. Heartbeat into the kernel log every 5 s (level crit: it reaches the
#     console, so ramoops keeps it across a reset). The last lines of a boot
#     that died show what still ran and when.
#  2. At start: the previous boot's console (pstore) is copied to
#     /roms/ports/nfs8/crashlogs/, so later boots do not overwrite it.
#  3. Game watchdog: a running nfsu2_recomp* that has presented no frame (no
#     new "[fps]" line in log.txt) for GAME_STALL_S is killed (by PID), so
#     EmulationStation comes back.
#  4. WiFi watchdog: link up (carrier) but the gateway does not answer for
#     WIFI_FAIL_S -> wlan0 down/up; still dead after as long again -> the
#     adapter is re-enumerated on USB. Never reboots for WiFi (out of range
#     is not a hang: then there is no carrier and nothing happens).
#
# Full freezes (kernel / GPU) are not this script's job: the hardware
# watchdog (fed by systemd, /etc/systemd/system.conf.d/90-r36s-watchdog.conf)
# and kernel.panic (/etc/sysctl.d/90-r36s-recover.conf) reset the device.
GAME_DIR=/roms/ports/nfs8
GAME_STALL_S=${GAME_STALL_S:-180}
WIFI_FAIL_S=${WIFI_FAIL_S:-120}
TICK=5

klog() { echo "<2>r36s-guard: $*" > /dev/kmsg; }

# 2. the previous boot's console
mkdir -p "$GAME_DIR/crashlogs"
# console-ramoops: the console up to the reset; dmesg-ramoops-*: a panic's log.
for f in /var/lib/systemd/pstore/*ramoops* /sys/fs/pstore/*ramoops*; do
    if [ -s "$f" ]; then
        sum=$(md5sum "$f" | cut -c1-8)
        ls "$GAME_DIR/crashlogs/"*"-$sum.txt" >/dev/null 2>&1 || \
            cp "$f" "$GAME_DIR/crashlogs/$(date +%Y%m%d-%H%M%S)-$(basename "$f")-$sum.txt"
    fi
done
ls -t "$GAME_DIR/crashlogs/"*.txt 2>/dev/null | tail -n +31 | xargs -r rm -f   # keep 30

klog "started (game stall ${GAME_STALL_S}s, wifi ${WIFI_FAIL_S}s)"
n=0; wifi_bad=0; wifi_step=0
last=$(date +%s); resumed=0
g_pid=""; g_fps=-1; g_seen=$last
while :; do
    n=$((n + 1))
    now=$(date +%s)
    # A gap between ticks is a suspend (auto_suspend.py) or the clock moving:
    # the game logged nothing while asleep, so its silence counts from here.
    if [ $((now - last)) -gt 30 ] || [ "$now" -lt "$last" ]; then
        resumed=$now; wifi_bad=0
        klog "clock gap of $((now - last))s (suspend?): watchdogs restart counting"
    fi
    last=$now
    game=$(pidof nfsu2_recomp nfsu2_recomp_vk | cut -d' ' -f1)

    # 1. heartbeat
    mem=$(awk '/MemAvailable/ {a=$2} /SwapFree/ {s=$2} END {printf "avail %dM swapfree %dM", a/1024, s/1024}' /proc/meminfo)
    st=""; [ -n "$game" ] && st=$(awk '{print $3}' /proc/$game/stat 2>/dev/null)
    klog "hb $n load $(cut -d' ' -f1-3 /proc/loadavg) $mem temp $(cat /sys/class/thermal/thermal_zone0/temp)" \
         "gpu $(cat /sys/class/misc/mali0/device/utilisation 2>/dev/null) usbirq $(awk '$1 == "19:" {print $2}' /proc/interrupts)" \
         "wlan $(awk '$1 == "wlan0:" {print $2 "/" $10}' /proc/net/dev) game ${game:-none}:$st"

    # 3. game watchdog. Progress = presented frames: the game logs "[fps]"
    # every 10 s from its flip (RECOMP_FPS_LOG=1, set by the launcher). The
    # log's mtime is not enough: a game stuck on a GPU fence keeps writing
    # its periodic "[wake]" lines. Checked every 30 s.
    if [ -z "$game" ]; then
        g_pid=""
    elif [ "$game" != "$g_pid" ]; then
        g_pid=$game; g_fps=-1; g_seen=$now           # a new launch: count from now
    fi
    if [ -n "$game" ] && [ -f "$GAME_DIR/log.txt" ] && [ $((n % 6)) -eq 0 ]; then
        c=$(grep -c '^\[fps\]' "$GAME_DIR/log.txt" 2>/dev/null || echo 0)
        if [ "$c" != "$g_fps" ]; then
            g_fps=$c; g_seen=$now
        fi
        mt=$g_seen
        [ "$mt" -lt "$resumed" ] && mt=$resumed      # ... or from the resume
        if [ $((now - mt)) -ge "$GAME_STALL_S" ]; then
            klog "game $game: no frame presented for $((now - mt))s ([fps] count $c) -- killing it"
            kill -9 "$game" 2>/dev/null
            k=$(pidof gptokeyb); [ -n "$k" ] && kill -9 $k 2>/dev/null
            sleep 3
            systemctl restart oga_events 2>/dev/null &
        fi
    fi

    # 4. wifi watchdog (every 15 s)
    if [ $((n % 3)) -eq 0 ]; then
        gw=$(ip route show default dev wlan0 2>/dev/null | awk '{print $3; exit}')
        if [ "$(cat /sys/class/net/wlan0/carrier 2>/dev/null)" = 1 ] && [ -n "$gw" ]; then
            if ping -c 1 -W 3 "$gw" >/dev/null 2>&1; then
                [ "$wifi_bad" -gt 0 ] && klog "wifi back after ${wifi_bad}s"
                wifi_bad=0; wifi_step=0
            else
                wifi_bad=$((wifi_bad + 15))
            fi
        else
            wifi_bad=0; wifi_step=0
        fi
        if [ "$wifi_bad" -ge "$WIFI_FAIL_S" ]; then
            if [ "$wifi_step" -eq 0 ]; then
                klog "wifi: gateway $gw silent ${wifi_bad}s with carrier -- wlan0 down/up"
                ip link set wlan0 down; sleep 2; ip link set wlan0 up
                wifi_step=1
            else
                dev=$(basename "$(readlink -f /sys/class/net/wlan0/device/..)" 2>/dev/null)
                klog "wifi: still dead -- re-enumerating USB device ${dev:-?}"
                if [ -n "$dev" ] && [ -w "/sys/bus/usb/devices/$dev/authorized" ]; then
                    echo 0 > "/sys/bus/usb/devices/$dev/authorized"; sleep 2
                    echo 1 > "/sys/bus/usb/devices/$dev/authorized"
                fi
                wifi_step=0
            fi
            wifi_bad=0
        fi
    fi
    sleep $TICK
done
