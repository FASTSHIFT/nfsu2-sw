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
    r36s/autotest.py --host 192.168.3.71

Exit code 0 = PASS, 1 = FAIL. See docs/05 for the pipeline design and usage.

Env: R36S_PASS (default "ark"), R36S_HOST (default 192.168.3.71).
"""
import argparse
import json
import os
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


def run_driver(mode, timeout=600):
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
                    f"nohup python3 {DRIVER_REMOTE} {mode} >/dev/null 2>&1 &"])
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


def main():
    global HOST
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy-only", action="store_true")
    ap.add_argument("--skip-deploy", action="store_true")
    ap.add_argument("--launch", action="store_true",
                    help="driver launch mode (open + confirm + exit)")
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

    mode = "launch" if args.launch else "race"
    report = run_driver(mode)
    if report is None:
        print(json.dumps({"verdict": "FAIL: no report"}))
        return

    print(json.dumps(report, indent=2))
    outdir = collect(report)
    verdict = report.get("verdict", "UNKNOWN")
    if verdict == "PASS":
        print(f"[autotest] PASS ({outdir})")
        sys.exit(0)
    else:
        print(f"[autotest] {verdict} ({outdir})")
        sys.exit(1)


if __name__ == "__main__":
    main()
