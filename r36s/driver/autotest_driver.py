#!/usr/bin/env python3
"""
autotest_driver.py -- run ON the R36S device: drive a full unattended NFSU2 run.

Runs locally (no ssh, no cross-boundary quoting), so pgrep/grep/evdev behave
exactly as typed. The host only uploads this file and runs it:

    python3 autotest_driver.py            # full run (deploy handled by host)
    python3 autotest_driver.py launch     # only: open the game, confirm, exit
    python3 autotest_driver.py race       # open + drive into a race + sample
    python3 autotest_driver.py baseline   # same entry, then log per-frame draw
                                          # stats (RECOMP_PERF_LOG=2), summarise
                                          # the race frames and quit by hotkey

Reach detection is the game's own log: src/autotest.c wraps FEngHud::Update
(sub_00100360, once per frame while the race HUD exists) and prints

    [AUTOTEST] HUD live: FEngHud=0x... Player=0x... update#1 t=NN.N s

on its first call. The HUD is built with the Player for a race and does not
exist in the front end, so the line is "the tachometer is now being drawn".
Until it shows up the game mashes A (RECOMP_PAD_MASH, set through env.txt
for the run); the driver only waits for the line.

Button codes (odroidgo3-joypad / event2, R36S remap; see dArkOSen dtb sw19/sw22):
    A (confirm) = 305 (BTN_EAST, right)   B (back) = 304 (BTN_SOUTH, bottom)
    SELECT = 704   START = 705            exit = SELECT+START together (gptokeyb)
    RT (accelerate) = 313 (BTN_TR2)
"""
import evdev
import json
import os
import re
import subprocess
import sys
import time

PORT = "/roms/ports/nfs8"
GAME_LOG = PORT + "/log.txt"
DRIVER_LOG = PORT + "/driver.log"
REPORT_FILE = PORT + "/autotest_report.json"
ES_LOG = "/home/ark/.emulationstation/es_log.txt"
EVDEV = "/dev/input/event2"

BTN_A = 305       # right face button
BTN_B = 304       # bottom face button
BTN_SELECT = 704
BTN_START = 705
BTN_RT = 313      # accelerate (right trigger)

# The menus are driven by the game itself: RECOMP_PAD_MASH (usb_gamepad.c)
# presses A -- every 7th press Start -- inside the process, the same spec as
# tools/pc/env.sh, until the HUD line shows up. The driver used to tap A over
# evdev from here (5/s, 70 ms): python's own latency, and a press shorter than
# a menu frame (~10 fps on the device) can fall between two pad polls and
# never be seen. Hold 100 ms of a 200 ms period: every poll sees an edge.
PAD_MASH = "200,100,7"
HUD_POLL_S = 1.0

# Printed by src/autotest.c (see the module docstring).
HUD_LIVE = "[AUTOTEST] HUD live"
HUD_HOOK_OFF = "FEngHud hook OFF"


def log(msg):
    line = f"[driver] {msg}"
    print(line, flush=True)
    # Persist every step to DRIVER_LOG so a driver death mid-run (or an ssh
    # disconnect that kills the foreground command) can still be diagnosed.
    try:
        with open(DRIVER_LOG, "a") as f:
            f.write(time.strftime("%H:%M:%S ") + line + "\n")
    except OSError:
        pass


def die(msg):
    report = {"verdict": "FAIL", "error": msg}
    print(json.dumps(report, indent=2))
    try:
        with open(REPORT_FILE, "w") as f:
            json.dump(report, f, indent=2)
    except OSError:
        pass
    sys.exit(1)


def sh(cmd, timeout=30):
    """Run a command locally on the device."""
    try:
        p = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout.strip()
    except subprocess.TimeoutExpired:
        return -1, ""


def game_pid():
    # -x: exact process-name match. A `pgrep -f nfsu2_recomp` would also match
    # the very shell running the pgrep (its command line contains the pattern),
    # so it always "found" a game that was never launched.
    rc, out = sh("pgrep -x nfsu2_recomp | head -1")
    return out if rc == 0 and out.isdigit() else None


def es_running():
    # comm is cut to 15 characters ("emulationstatio"); `pgrep -x` with the
    # full name never matches, and piping into head hid that.
    rc, out = sh("pgrep -x emulationstatio | head -1")
    return out.isdigit()


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def tail_file(path, n=20):
    rc, out = sh(f"tail -n {n} '{path}'")
    return out if rc == 0 else ""


def btn(code, value, hold_ms=0):
    """Inject one button transition on event2."""
    try:
        d = evdev.InputDevice(EVDEV)
        d.write(evdev.ecodes.EV_KEY, code, value)
        d.write(evdev.ecodes.EV_SYN, 0, 0)
        if hold_ms:
            time.sleep(hold_ms / 1000.0)
    except OSError as e:
        log(f"evdev write failed: {e}")


def press(code, hold_ms=150):
    btn(code, 1)
    time.sleep(hold_ms / 1000.0)
    btn(code, 0)


ENV_FILE = PORT + "/env.txt"
ENV_BAK = PORT + "/env.txt.autotest-bak"


def env_apply(lines):
    """Add KEY=VALUE lines to env.txt for this run only. The launcher sources
    env.txt on every start; the original is kept in env.txt.autotest-bak and
    put back by env_restore() (and at the start of the next run, if this one
    died before it could)."""
    env_restore()
    orig = read_file(ENV_FILE)
    with open(ENV_BAK, "w") as f:
        f.write(orig)
    with open(ENV_FILE, "w") as f:
        f.write(orig.rstrip("\n") + "\n# autotest driver (this run only)\n"
                + "\n".join(lines) + "\n")
    log("env.txt += " + " ".join(lines))


def env_restore():
    if os.path.exists(ENV_BAK):
        os.replace(ENV_BAK, ENV_FILE)
        log("env.txt restored")


def exit_game():
    """SELECT+START together -> gptokeyb kills the game cleanly."""
    log("SELECT+START to exit")
    d = evdev.InputDevice(EVDEV)
    d.write(evdev.ecodes.EV_KEY, BTN_SELECT, 1)
    d.write(evdev.ecodes.EV_KEY, BTN_START, 1)
    d.write(evdev.ecodes.EV_SYN, 0, 0)
    time.sleep(0.6)
    d.write(evdev.ecodes.EV_KEY, BTN_SELECT, 0)
    d.write(evdev.ecodes.EV_KEY, BTN_START, 0)
    d.write(evdev.ecodes.EV_SYN, 0, 0)


def exit_game_sure(timeout=60):
    """Send SELECT+START and confirm the game actually dies; retry if not."""
    if not game_pid():
        log("game already gone")
        return True
    for attempt in range(5):
        exit_game()
        end = time.time() + 15
        while time.time() < end:
            if not game_pid():
                log("game exited cleanly")
                return True
            time.sleep(1)
        log(f"exit attempt {attempt + 1}: game still alive, retrying")
    log("WARN: game did not exit after 5 SELECT+START attempts")
    return False


def log_has(text):
    """True if the game log contains `text` (fixed string)."""
    rc, _ = sh(f"grep -qF '{text}' '{GAME_LOG}'")
    return rc == 0


def first_frame_seen():
    return sh(f"grep -q '\\[fps\\]' '{GAME_LOG}'")[0] == 0


def es_launched_nfs2():
    rc, out = sh(f"grep -A1 'Attempting to launch game' '{ES_LOG}' | tail -6")
    return "Underground" in out


def read_temp():
    rc, out = sh("cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null")
    if rc == 0 and out:
        vals = [int(x) for x in out.split() if x.isdigit()]
        return max(vals) / 1000.0 if vals else None
    return None


def read_freq():
    rc, out = sh("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null")
    return int(out) // 1000 if rc == 0 and out.isdigit() else None


def wait_game(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        pid = game_pid()
        if pid:
            return pid
        time.sleep(1)
    return None


def do_launch():
    """Open the game with one A press; confirm via ES log it was NFS2."""
    # Clear the game log before launching: the HUD line of a previous run
    # would otherwise be taken for this run's and report "reached race" even
    # when the game never started this time.
    sh(f"rm -f '{GAME_LOG}'")
    log("pressing A to launch")
    press(BTN_A, 200)
    pid = wait_game(60)
    if not pid:
        die("game did not appear after A press")
    log(f"game running (pid {pid})")
    if not es_launched_nfs2():
        log("WARN: ES log does not confirm NFS2 launch (cursor misplaced?)")
    return pid


def do_race(pid, timeout=240, hold_rt=True):
    """Wait for the game to log that the race HUD is live (it is mashing A
    itself, see PAD_MASH), then (hold_rt) hold RT so the car moves."""
    log("waiting for the first frame ...")
    end = time.time() + timeout
    while time.time() < end:
        if not game_pid():
            die("game process gone")
        if first_frame_seen():
            break
        time.sleep(1)
    else:
        log("timeout waiting for first frame")
        return False

    if log_has(HUD_HOOK_OFF):
        die("this build cannot see the race HUD (gen lifted without the "
            "sub_00100360 wrap) -- regen and rebuild")

    log(f"frames started -- game is mashing A ({PAD_MASH}); waiting for '{HUD_LIVE}'")
    while time.time() < end:
        if not game_pid():
            die("game process gone")
        rc, line = sh(f"grep -F '{HUD_LIVE}' '{GAME_LOG}' | head -1")
        if rc == 0 and line:
            log(f"reached race: {line}")
            if hold_rt:
                log("holding RT (accelerate) 8 s ...")
                btn(BTN_RT, 1, 8000)
                btn(BTN_RT, 0)
            return True
        time.sleep(HUD_POLL_S)
    log("timeout waiting for race")
    return False


FRAME_RE = re.compile(r"^\[frame\] (\d+) (.*)$")
BASE_FIELDS = ("ms", "gl", "wait", "swap", "dc", "frag", "calls")
BASE_FRAMES = 420      # frames after "HUD live" to wait for (race starts ~+220)
BASE_STEADY = 100      # the last this many frames are the steady-state sample


def frames_after_hud():
    """[frame] lines (RECOMP_PERF_LOG=2) written after the HUD-live line."""
    out, seen = [], False
    for line in read_file(GAME_LOG).splitlines():
        if HUD_LIVE in line:
            seen = True
            continue
        m = FRAME_RE.match(line) if seen else None
        if m:
            d = dict(re.findall(r"(\w+)=([\d.]+)", m.group(2)))
            try:
                out.append({k: float(d[k]) for k in BASE_FIELDS})
            except KeyError:
                pass
    return out


def summarise(frames):
    """Median / min / max per field, over `frames`."""
    res = {"frames": len(frames)}
    for k in BASE_FIELDS:
        v = [f[k] for f in frames]
        res[k] = {"p50": quantile(v, 0.5), "min": min(v), "max": max(v)} if v else None
    return res


def do_baseline(timeout=300):
    """Wait for BASE_FRAMES frames after the HUD line, then summarise: a
    series (30-frame windows, to show the load -> pre-race -> race ramp) and
    the steady state (the last BASE_STEADY frames)."""
    end = time.time() + timeout
    n = 0
    while time.time() < end:
        if not game_pid():
            die("game process gone while sampling")
        n = len(frames_after_hud())
        if n >= BASE_FRAMES:
            break
        time.sleep(2)
    fr = frames_after_hud()
    log(f"{len(fr)} frames after HUD live")
    if len(fr) < BASE_FRAMES:
        return None
    return {
        "series": [dict(start=i, **{k: quantile([f[k] for f in fr[i:i + 30]], 0.5)
                                    for k in BASE_FIELDS})
                   for i in range(0, len(fr) - 29, 30)],
        "steady": summarise(fr[-BASE_STEADY:]),
        "steady_first_frame": len(fr) - BASE_STEADY,
    }


def sample(seconds=10):
    fps, temps, freqs = [], [], []
    end = time.time() + seconds
    while time.time() < end:
        rc, out = sh(f"grep '\\[fps\\]' '{GAME_LOG}' | tail -1")
        if rc == 0 and out:
            m = re.search(r"\[fps\] ([0-9.]+) fps", out)
            if m:
                fps.append(float(m.group(1)))
        t = read_temp()
        if t:
            temps.append(t)
        f = read_freq()
        if f:
            freqs.append(f)
        time.sleep(1)
    return fps, temps, freqs


def quantile(v, q):
    if not v:
        return None
    return sorted(v)[int(len(v) * q)]


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "race"
    timing = {}
    t0 = time.time()

    def mark(name):
        timing[name] = round(time.time() - t0, 1)

    # Fresh driver log each run (do_launch also clears GAME_LOG).
    sh(f"rm -f '{DRIVER_LOG}'")

    # preconditions
    if game_pid():
        log("WARN: game already running, exiting it first")
        if not exit_game_sure():
            die("game still running after SELECT+START exit")
    if not es_running():
        die("emulationstation not running")
    log("preconditions ok (ES up, no game)")
    mark("preconditions")

    extra = []
    if mode != "launch":
        extra.append(f"RECOMP_PAD_MASH={PAD_MASH}")
    if mode == "baseline":
        # One [frame] line per presented frame: draws, fragment draws, GL
        # calls, GL / wait / swap ms. The game's own switches stay as in env.txt.
        extra.append("RECOMP_PERF_LOG=2")
    if extra:
        env_apply(extra)
    else:
        env_restore()
    try:
        run(mode, timing, t0, mark)
    finally:
        env_restore()


def run(mode, timing, t0, mark):
    pid = do_launch()
    mark("launch")

    report = {"host": "local", "pid": pid,
              "time": time.strftime("%Y-%m-%d %H:%M:%S")}

    try:
        if mode == "launch":
            # [fps] prints every 10 s of wall clock, but its timer starts on the
            # first presented frame (~5-6 s after launch: black-screen bring-up).
            # 20 s guarantees at least one [fps] line has been emitted.
            time.sleep(20)
            rc, out = sh(f"grep -c '\\[fps\\]' '{GAME_LOG}'")
            report["verdict"] = "LAUNCHED"
            report["fps_lines"] = out
            exit_game_sure()
            mark("exit")
        elif mode == "baseline":
            if not do_race(pid, hold_rt=False):
                report["verdict"] = "FAIL: did not reach race"
            else:
                mark("reach_race")
                base = do_baseline()
                mark("baseline")
                if base is None:
                    report["verdict"] = "FAIL: too few frames after HUD live"
                else:
                    report["baseline"] = base
                    report["temp_c"] = read_temp()
                    report["cpu_mhz"] = read_freq()
                    report["verdict"] = "PASS"
            exit_game_sure()
            mark("exit")
        else:
            if not do_race(pid):
                report["verdict"] = "FAIL: did not reach race"
                exit_game_sure()
                mark("exit")
            else:
                mark("reach_race")
                log("sampling steady state ...")
                fps, temps, freqs = sample(25)
                report["fps"] = {
                    "count": len(fps),
                    "p50": quantile(fps, 0.50),
                    "p10": quantile(fps, 0.10),
                    "p1": quantile(fps, 0.01),
                }
                report["temp_c"] = {"max": max(temps) if temps else None}
                report["freq_mhz"] = {"min": min(freqs) if freqs else None,
                                      "max": max(freqs) if freqs else None}
                report["verdict"] = "PASS"
                mark("sample")
                exit_game_sure()
                mark("exit")
    except Exception as e:
        log(f"EXCEPTION: {e!r}")
        report["verdict"] = f"EXCEPTION: {e!r}"
        try:
            exit_game_sure()
        except Exception:
            pass

    report["timing"] = timing
    report["total_s"] = round(time.time() - t0, 1)
    # The machine-readable result goes to its own file, so the host can pull
    # it without parsing away [driver] log lines mixed into stdout.
    try:
        with open(REPORT_FILE, "w") as f:
            json.dump(report, f, indent=2)
        log(f"report written to {REPORT_FILE}")
    except OSError as e:
        log(f"WARN: could not write report: {e}")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
