#!/usr/bin/env python3
"""
autotest.py -- host-side wrapper for the R36S unattended regression run.

The actual run happens ON the device (r36s/driver/autotest_driver.py), launched
detached via nohup so an ssh drop can't kill it mid-race. This script only does
what must happen on the host:

    1. (optionally) deploy the freshly built binary as nfsu2_recomp.new
    2. upload the driver, start it detached, poll the on-device report
    3. pull report + driver.log + game.log into build-r36s/autotest/<ts>/

Usage:
    r36s/autotest.py                      # deploy + full race run (regression)
    r36s/autotest.py --skip-deploy        # binary already deployed
    r36s/autotest.py --deploy-only        # deploy only
    r36s/autotest.py --launch             # driver launch mode (open + exit)
    r36s/autotest.py --sweep --set rd:EXP_RD=2 --set drop:EXP_DROPFRAG=1
                                          # ONE launch: switch live knobs per setting
    r36s/autotest.py --baseline           # enter a race, log per-frame draw
                                          # stats, print the steady state, quit
    r36s/autotest.py --host 192.168.3.71

Exit code 0 = PASS, 1 = FAIL. See docs/05 for the pipeline design and usage.

Env: R36S_PASS (default "ark"), R36S_HOST (default 192.168.3.71).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HOST = os.environ.get("R36S_HOST", "192.168.3.71")
PASS = os.environ.get("R36S_PASS", "ark")
PORT = "/roms/ports/nfs8"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_LOCAL = os.path.join(REPO, "build-r36s", "nfsu2_recomp")
DRIVER_LOCAL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "driver", "autotest_driver.py")
DRIVER_REMOTE = PORT + "/autotest_driver.py"
REPORT_REMOTE = PORT + "/autotest_report.json"
DRIVER_LOG_REMOTE = PORT + "/driver.log"
GAME_LOG_REMOTE = PORT + "/log.txt"


def ssh(args, timeout=60):
    cmd = ["sshpass", "-p", PASS, "ssh",
           "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=no",
           f"ark@{HOST}"] + list(args)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except FileNotFoundError:
        print("error: sshpass not found", file=sys.stderr)
        sys.exit(1)


def deploy():
    """scp the freshly built binary as .new; the ES script installs it on the
    next launch. gzip to survive flaky Wi-Fi, then gunzip + md5 on the device."""
    if not os.path.exists(BIN_LOCAL):
        print(f"error: no binary at {BIN_LOCAL}; build first "
              f"(sg docker -c 'bash r36s/build.sh')", file=sys.stderr)
        sys.exit(1)
    print(f"[deploy] {BIN_LOCAL} -> {HOST}:{PORT}/nfsu2_recomp.new")
    gz = BIN_LOCAL + ".gz"
    t0 = time.time()
    with open(gz, "wb") as f:
        subprocess.run(["gzip", "-kc", BIN_LOCAL], stdout=f, check=True)
    print(f"  gzip {os.path.getsize(gz)} bytes ({time.time()-t0:.1f}s)")
    local_md5 = subprocess.run(["md5sum", BIN_LOCAL], capture_output=True,
                               text=True).stdout.split()[0]
    t0 = time.time()
    rc = subprocess.run(
        ["sshpass", "-p", PASS, "scp", "-o", "ConnectTimeout=10",
         gz, f"ark@{HOST}:{PORT}/nfsu2_recomp.new.gz"],
        capture_output=True, text=True, timeout=300).returncode
    os.remove(gz)
    if rc != 0:
        print("error: scp failed", file=sys.stderr)
        return False
    print(f"  scp done ({time.time()-t0:.1f}s)")
    ssh([f"cd {PORT} && gunzip -f nfsu2_recomp.new.gz"], timeout=120)
    rc, dev_md5, _ = ssh([f"md5sum {PORT}/nfsu2_recomp.new"])
    dev_md5 = dev_md5.strip().split()[0]
    if dev_md5 != local_md5:
        print(f"error: md5 mismatch local={local_md5} dev={dev_md5}",
              file=sys.stderr)
        return False
    print(f"[deploy] ok, md5 {local_md5}")
    return True


def run_driver(mode, env=(), timeout=600):
    env = list(env)
    """Upload the driver, launch it DETACHED (nohup, so an ssh drop doesn't kill
    it mid-race), poll the on-device report file until it appears, then pull
    the report + driver log + game log back here. Returns the parsed report."""
    if not os.path.exists(DRIVER_LOCAL):
        print(f"error: no driver at {DRIVER_LOCAL}", file=sys.stderr)
        sys.exit(1)
    print(f"[driver] upload {DRIVER_LOCAL} -> {HOST}:{DRIVER_REMOTE}")
    rc = subprocess.run(
        ["sshpass", "-p", PASS, "scp", "-o", "ConnectTimeout=10",
         DRIVER_LOCAL, f"ark@{HOST}:{DRIVER_REMOTE}"],
        capture_output=True, text=True, timeout=60).returncode
    if rc != 0:
        print("error: driver upload failed", file=sys.stderr)
        return None

    # rm any stale report so we don't read last run's result, then start.
    rc, _, _ = ssh([f"rm -f '{REPORT_REMOTE}' && cd {PORT} && "
                    f"nohup python3 {DRIVER_REMOTE} {mode} {' '.join(env)} >/dev/null 2>&1 &"])
    print(f"[driver] started on {HOST} (detached), waiting for report ...")

    report = None
    end = time.time() + timeout
    while time.time() < end:
        rc, out, _ = ssh([f"cat '{REPORT_REMOTE}' 2>/dev/null"])
        if rc == 0 and out.strip():
            try:
                report = json.loads(out)
                break
            except json.JSONDecodeError:
                pass  # report still being written
        time.sleep(5)

    if report is None:
        print("error: no report after timeout", file=sys.stderr)
        return None
    return report


def collect(report):
    """Pull driver.log + game log + report into build-r36s/autotest/<ts>/."""
    ts = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(REPO, "build-r36s", "autotest", ts)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    for remote, local in [(DRIVER_LOG_REMOTE, "driver.log"),
                          (GAME_LOG_REMOTE, "game.log")]:
        rc = subprocess.run(
            ["sshpass", "-p", PASS, "scp", "-o", "ConnectTimeout=10",
             f"ark@{HOST}:{remote}", os.path.join(outdir, local)],
            capture_output=True, text=True, timeout=60).returncode
        if rc != 0:
            print(f"  (could not pull {local})")
    print(f"[autotest] artifacts in {outdir}")
    return outdir


def pull_shot(report, outdir):
    """The screen is judged on the device (driver: mean brightness of each
    dumped frame). Only a picture that is not black is worth looking at, so
    only then is the newest shot copied back, as PNG."""
    scr = report.get("screen")
    if not scr or not scr.get("last"):
        print("[screen] no frame dumped")
        return
    means = ", ".join(f"{s['mean']:.0f}" for s in scr["shots"] if s["mean"] is not None)
    if scr["black"]:
        print(f"[screen] BLACK (mean per shot: {means})")
        return
    bmp = os.path.join(outdir, scr["last"])
    rc = subprocess.run(["sshpass", "-p", PASS, "scp", "-o", "ConnectTimeout=10",
                         f"ark@{HOST}:{PORT}/shots/{scr['last']}", bmp],
                        capture_output=True, text=True, timeout=60).returncode
    if rc != 0:
        print("[screen] could not pull the shot")
        return
    try:
        from PIL import Image
        png = bmp[:-4] + ".png"
        Image.open(bmp).convert("RGB").save(png)
        os.remove(bmp)
        print(f"[screen] ok (mean per shot: {means}) -> {png}")
    except ImportError:
        print(f"[screen] ok -> {bmp} (no PIL here, left as BMP)")


def pull_traces(rows, outdir):
    """Copy every recording back and summarise it (who is busy, who waits)."""
    tdir = os.path.join(outdir, "xtrace")
    os.makedirs(tdir, exist_ok=True)
    got = []
    for r in rows:
        dst = os.path.join(tdir, os.path.basename(r["file"]))
        rc = subprocess.run(["sshpass", "-p", PASS, "scp", "-o", "ConnectTimeout=10",
                             f"ark@{HOST}:{r['file']}", dst], capture_output=True, timeout=180).returncode
        if rc == 0:
            got.append(dst)
        else:
            print(f"  (could not pull {r['file']})")
    print(f"[trace] {len(got)} recording(s) in {tdir}")
    if got:
        summ = os.path.join(REPO, "tools", "trace_summary.py")
        out = subprocess.run([sys.executable, summ] + got, capture_output=True, text=True, timeout=900)
        open(os.path.join(tdir, "summary.txt"), "w").write(out.stdout + out.stderr)
        print(out.stdout[-6000:])


def pull_sweep_shots(rows, outdir):
    """Copy the frame each window dumped (row["shot"]) back, and compare every
    setting's frame with the baseline window before it: the share of pixels
    that moved, in the road band and in the whole frame minus the HUD. Needs a
    static scene (the car waits on the grid). Writes <name>.png per window and
    sets r["d_road"] / r["d_all"]."""
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        print("[shots] PIL / numpy missing, no comparison")
        return
    shots = os.path.join(outdir, "shots")
    os.makedirs(shots, exist_ok=True)
    base = None
    for i, r in enumerate(rows):
        if not r.get("shot"):
            continue
        bmp = os.path.join(shots, f"{i:02d}_{r['name']}.bmp")
        rc = subprocess.run(["sshpass", "-p", PASS, "scp", "-o", "ConnectTimeout=10",
                             f"ark@{HOST}:{PORT}/shots/{r['shot']}", bmp],
                            capture_output=True, timeout=60).returncode
        if rc != 0:
            continue
        im = Image.open(bmp).convert("RGB")
        im.save(bmp[:-4] + ".png")
        os.remove(bmp)
        px = np.asarray(im).astype(int)
        if not r["spec"]:
            base = px
            continue
        if base is None or base.shape != px.shape:
            continue
        d = np.abs(px - base).sum(2) > 48
        h, w = d.shape
        d[: int(h * 0.30)] = False                  # sky / HUD timer area
        d[int(h * 0.62):, : int(w * 0.30)] = False  # minimap
        d[int(h * 0.62):, int(w * 0.70):] = False   # speedometer
        road = d[int(h * 0.62):int(h * 0.92)]
        r["d_road"] = 100.0 * road.mean()
        r["d_all"] = 100.0 * d[int(h * 0.30):].mean()


def print_sweep(rows):
    """One row per window. A setting's d_ms / d_gl / d_wait are against the mean
    of the two baselines next to it (drift cancels); `screen` is the mean
    brightness of a frame dumped during it (0 = black)."""
    print("\n[sweep] median per window; d_* = setting minus the mean of the baselines around it")
    print("  %-12s %7s %7s %7s %6s %7s | %7s %7s %7s %7s %6s  %s" % (
        "name", "ms", "gl", "wait", "dc", "calls", "d_ms", "d_gl", "d_wait", "d_calls", "screen", "C  cpu/gpu MHz  knobs"))
    for r in rows:
        sm = r["screen_mean"]
        d = ("%7.1f %7.1f %7.1f %7.0f" % (r["d_ms"], r["d_gl"], r["d_wait"], r["d_calls"])
             if "d_ms" in r else " " * 7 + " " * 8 + " " * 8 + " " * 8)
        pic = ("  road %5.1f%% frame %5.1f%%" % (r["d_road"], r["d_all"])) if "d_road" in r else ""
        print("  %-12s %7.1f %7.1f %7.1f %6.0f %7.0f | %s %6s  %.0f %s/%s  %s%s" % (
            r["name"], r["ms"], r["gl"], r["wait"], r["dc"], r["calls"], d,
            "-" if sm is None else f"{sm:.0f}", r.get("temp_c") or 0,
            r.get("cpu_mhz"), r.get("gpu_mhz"), r["spec"], pic))


def print_baseline(b):
    """The race steady state and the load -> race ramp, as a table."""
    st = b["steady"]
    print(f"\n[baseline] steady state, {st['frames']} frames of the race scene "
          f"(scene from frame {b['race_first_frame']}, sampled from {b['steady_first_frame']}, "
          "counted after HUD live)")
    for k in ("ms", "gl", "wait", "swap", "dc", "frag", "calls"):
        v = st[k]
        print(f"  {k:6s} p50 {v['p50']:9.1f}   min {v['min']:9.1f}   max {v['max']:9.1f}")
    print("[baseline] 30-frame medians after HUD live (ms gl wait dc frag calls)")
    for r in b["series"]:
        print(f"  +{r['start']:4d}  {r['ms']:6.1f} {r['gl']:6.1f} {r['wait']:6.1f} "
              f"{r['dc']:6.0f} {r['frag']:6.0f} {r['calls']:7.0f}")


def main():
    global HOST
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy-only", action="store_true")
    ap.add_argument("--skip-deploy", action="store_true")
    ap.add_argument("--launch", action="store_true",
                    help="driver launch mode (open + confirm + exit)")
    ap.add_argument("--baseline", action="store_true",
                    help="per-frame draw/GL stats of the race (RECOMP_PERF_LOG=2), "
                         "summarised on the device")
    ap.add_argument("--trace", action="store_true",
                    help="one launch, an xtrace recording per --set (plus a baseline first and last), "
                         "copied back and summarised by tools/trace_summary.py")
    ap.add_argument("--sweep", action="store_true",
                    help="one launch, then each --set applied live and sampled in place")
    ap.add_argument("--set", action="append", default=[], metavar="NAME:KNOB=V,KNOB=V",
                    help="a --sweep setting (knobs.h names, e.g. rd:EXP_RD=2,EXP_RDSEL=1)")
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL",
                    help="extra env.txt line for this run only (repeatable)")
    ap.add_argument("--host", default=HOST)
    args = ap.parse_args()
    HOST = args.host

    if args.deploy_only:
        deploy()
        return

    if not args.skip_deploy:
        if not deploy():
            print(json.dumps({"verdict": "FAIL: deploy"}))
            return
    else:
        print("[deploy] skipped (--skip-deploy)")

    mode = ("launch" if args.launch else "baseline" if args.baseline
            else "sweep" if args.sweep else "trace" if args.trace else "race")
    for sp in args.set:
        if not re.fullmatch(r"[a-z0-9_]+:([A-Z0-9_]+=\d+(,[A-Z0-9_]+=\d+)*)?", sp):
            sys.exit(f"error: bad --set {sp!r} (name:KNOB=V,KNOB=V)")
    for e in args.env:
        if not re.fullmatch(r"[A-Z0-9_]+=[\w.,:/+-]*", e):
            sys.exit(f"error: bad --env {e!r} (KEY=VAL, simple characters only)")
    report = run_driver(mode, list(args.env) + list(args.set), timeout=900 if mode == "sweep" else 600)
    if report is None:
        print(json.dumps({"verdict": "FAIL: no report"}))
        return

    print(json.dumps(report, indent=2))
    outdir = collect(report)
    if mode == "trace" and "trace" in report:
        pull_traces(report["trace"], outdir)
    if mode == "sweep" and "sweep" in report:
        pull_sweep_shots(report["sweep"], outdir)
        print_sweep(report["sweep"])
        pull_shot(report, outdir)
    if mode == "baseline" and "baseline" in report:
        print_baseline(report["baseline"])
        pull_shot(report, outdir)
    verdict = report.get("verdict", "UNKNOWN")
    if verdict == "PASS":
        print(f"[autotest] PASS ({outdir})")
        sys.exit(0)
    else:
        print(f"[autotest] {verdict} ({outdir})")
        sys.exit(1)


if __name__ == "__main__":
    main()
