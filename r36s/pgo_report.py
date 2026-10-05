#!/usr/bin/env python3
"""Per-function call counts from an NFSU2_PGO=gen run (docs/02).

  r36s/pgo_report.py PGO_DIR [BUILD_DIR] [GEN_DIR] > report.md

PGO_DIR    the .gcda files pulled from the device (/roms/ports/nfs8/pgo; names
           are the object paths with '/' turned into '#')
BUILD_DIR  the instrumented build with its .gcno notes (build-r36s-pgogen)
GEN_DIR    the lifted C, for the guest instruction count of each function

Runs aarch64-linux-gnu-gcov in nfsu2-r36s-cross:focal (the compiler that wrote
the notes) with --json-format, then ranks the lifted functions. Also writes
pgo_funcs.csv next to the report input for joining with perf samples.
"""
import csv, glob, gzip, json, os, re, shutil, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def stage(pgo, build, work):
    """gcov wants foo.gcda next to foo.gcno: copy both into one flat dir."""
    n = 0
    for gcda in glob.glob(os.path.join(pgo, "*.gcda")):
        rel = os.path.basename(gcda).replace("#", "/")
        rel = rel.split("/" + os.path.basename(build) + "/", 1)[-1]
        gcno = os.path.join(build, rel[:-5] + ".gcno")
        if not os.path.exists(gcno):
            continue
        base = os.path.join(work, rel.replace("/", "_")[:-5])
        shutil.copy(gcda, base + ".gcda")
        shutil.copy(gcno, base + ".gcno")
        n += 1
    return n


def run_gcov(work):
    cmd = ("cd /w && for f in *.gcda; do "
           "aarch64-linux-gnu-gcov --json-format -o /w \"${f%.gcda}.gcno\" >/dev/null 2>&1; done")
    subprocess.run(["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
                    "-v", f"{work}:/w", "nfsu2-r36s-cross:focal", "bash", "-c", cmd],
                   check=True)


def guest_sizes(gen):
    """sub_XXXXXXXX -> guest instruction count, from the lifter's comments."""
    sizes = {}
    pat = re.compile(r"^ \* (sub_[0-9A-F]{8})\n \* Original: .*?\((\d+) bytes, (\d+) insns\)", re.M)
    for f in glob.glob(os.path.join(gen, "recomp_*.c")):
        with open(f, errors="replace") as fh:
            for m in pat.finditer(fh.read()):
                sizes[m.group(1)] = int(m.group(3))
    return sizes


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pgo = os.path.abspath(sys.argv[1])
    build = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build-r36s-pgogen"))
    gen = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.path.join(REPO, "..", "NFS 8", "xbox", "gen"))

    work = tempfile.mkdtemp(prefix="pgo-", dir=os.path.join(REPO, build))
    if stage(pgo, build, work) == 0:
        sys.exit("no .gcda matched a .gcno in " + build)
    run_gcov(work)

    funcs = []
    for js in glob.glob(os.path.join(work, "*.gcov.json.gz")):
        with gzip.open(js, "rt") as fh:
            data = json.load(fh)
        for src in data.get("files", []):
            name = os.path.basename(src["file"])
            for fn in src.get("functions", []):
                funcs.append((fn["name"], name, fn["execution_count"],
                              fn.get("blocks", 0), fn.get("blocks_executed", 0)))
    shutil.rmtree(work, ignore_errors=True)

    sizes = guest_sizes(gen)
    lifted = [f for f in funcs if f[1].startswith("recomp_") and f[0].startswith("sub_")]
    lifted_names = {f[0] for f in lifted}
    total = len(lifted)
    ran = [f for f in lifted if f[2] > 0]
    calls = sum(f[2] for f in ran)
    ran.sort(key=lambda f: -f[2])

    with open(os.path.join(pgo, "pgo_funcs.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["func", "file", "calls", "guest_insns", "blocks", "blocks_executed"])
        for f in sorted(funcs, key=lambda f: -f[2]):
            w.writerow([f[0], f[1], f[2], sizes.get(f[0], ""), f[3], f[4]])

    # How many functions carry 50/90/99/99.9 % of all calls.
    cut, acc, k = {}, 0, 0
    for k, f in enumerate(ran, 1):
        acc += f[2]
        for p in (0.5, 0.9, 0.99, 0.999):
            if p not in cut and acc >= p * calls:
                cut[p] = k
    insns_all = sum(sizes.get(n, 0) for n in lifted_names)
    insns_ran = sum(sizes.get(f[0], 0) for f in ran)

    print(f"lifted functions: {total}, executed: {len(ran)} ({100.0 * len(ran) / total:.1f} %)")
    print(f"guest instructions in executed functions: {insns_ran} of {insns_all} "
          f"({100.0 * insns_ran / max(insns_all, 1):.1f} %)")
    print(f"calls: {calls}")
    for p in (0.5, 0.9, 0.99, 0.999):
        print(f"  {p * 100:g} % of calls in the top {cut.get(p, '-')} functions")
    for lo, hi in ((1, 10), (10, 1000), (1000, 10**5), (10**5, 10**7), (10**7, 10**30)):
        print(f"  called {lo}..{hi - 1}: {sum(1 for f in ran if lo <= f[2] < hi)}")
    print("\ntop 60 by calls (func, calls, guest insns, blocks run/total):")
    for f in ran[:60]:
        print(f"  {f[0]}  {f[2]:>12}  {sizes.get(f[0], '?'):>6}  {f[4]}/{f[3]}")


if __name__ == "__main__":
    main()
