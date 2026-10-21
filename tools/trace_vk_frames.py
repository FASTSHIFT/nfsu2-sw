#!/usr/bin/env python3
"""trace_vk_frames.py x.pftrace [...] -- per frame, who held the Vulkan build up (docs/13 §21).

A frame is one "flip" slice on the Vulkan recording thread (nv2a_vk.c, RECOMP_VK_THREAD=1),
flip start to flip start. For each frame: the recording thread's time in draws, clears, the
flip and "queue empty" (waiting for the executor); the executor's time decoding ("pb exec"),
idle and held back by the queue ("wait VK ..."); the KMS thread's fence wait (from the
frame's submit until the GPU has finished it), page-flip wait and copy.
Prints the medians and which thread was the limit how often:
the recording thread when it never waited for the queue, the executor when the recording
thread waited for it, ... Then the per-draw phases (trace_draw_phases.py) for scale.
Needs the `perfetto` pip package.
"""
import statistics as st
import sys
from perfetto.trace_processor import TraceProcessor

WAITS = ("idle", "queue empty", "wait GL", "wait VK", "GIL wait", "BlockOnFence", "frame fence",
         "page flip", "k:NtWait", "k:KeWait", "k:KeDelay", "apu idle", "vblank")


def thread_slices(tp, name_like):
    q = f"""
        select s.ts, s.dur, s.name, s.depth from slice s
        join thread_track tt on s.track_id = tt.id join thread t using(utid)
        where t.name like '{name_like}' order by s.ts"""
    return [(int(r.ts), int(r.dur), r.name, int(r.depth)) for r in tp.query(q)]


def overlap(sl, a, b, pred):
    tot = 0
    for ts, dur, name, depth in sl:
        if depth != 0 or not pred(name):
            continue
        lo, hi = max(ts, a), min(ts + dur, b)
        if hi > lo:
            tot += hi - lo
    return tot


def run(path):
    tp = TraceProcessor(trace=path)
    vk = thread_slices(tp, "Vulkan")
    ex = thread_slices(tp, "executor")
    km = thread_slices(tp, "KMS")
    flips = [ts for ts, dur, name, depth in vk if name == "flip" and depth == 0]
    print("== %s: %d frames" % (path.split("/")[-1], max(0, len(flips) - 1)))
    if len(flips) < 3:
        return
    cols = {k: [] for k in ("ms", "vk draw", "vk clear", "vk flip", "vk queue empty", "vk other",
                            "ex pb exec", "ex wait VK", "ex idle", "kms fence", "kms page flip",
                            "kms copy")}
    limit = {"recording thread": 0, "executor": 0}
    for a, b in zip(flips, flips[1:]):
        ms = (b - a) / 1e6
        d = lambda sl, p: overlap(sl, a, b, p) / 1e6
        row = {
            "ms": ms,
            "vk draw": d(vk, lambda n: n == "draw"),
            "vk clear": d(vk, lambda n: n == "clear"),
            "vk flip": d(vk, lambda n: n == "flip"),
            "vk queue empty": d(vk, lambda n: n == "queue empty"),
            "ex pb exec": d(ex, lambda n: n == "pb exec"),
            "ex wait VK": d(ex, lambda n: n.startswith("wait VK")),
            "ex idle": d(ex, lambda n: n == "idle"),
            "kms fence": d(km, lambda n: n == "frame fence"),
            "kms page flip": d(km, lambda n: n == "page flip"),
            "kms copy": d(km, lambda n: n == "KMS copy"),
        }
        row["vk other"] = ms - row["vk draw"] - row["vk clear"] - row["vk flip"] - row["vk queue empty"]
        for k, v in row.items():
            cols[k].append(v)
        limit["executor" if row["vk queue empty"] > 0.1 * ms else "recording thread"] += 1
    n = len(cols["ms"])
    print("  %-16s %8s %8s" % ("per frame (ms)", "median", "mean"))
    for k, v in cols.items():
        print("  %-16s %8.1f %8.1f" % (k, st.median(v), st.mean(v)))
    print("  limit: " + ", ".join("%s %d%%" % (k, 100 * v // n) for k, v in limit.items())
          + "  (executor = the recording thread waited for the queue > 10% of the frame)")
    rows = list(tp.query("""select name, count(*) n, sum(dur) tot, avg(dur) av from slice
                            where name like 'd:%' group by name order by tot desc"""))
    tot = sum(r.tot for r in rows) or 1
    if rows:
        print("  per-draw phases (sampled draws):")
        for r in rows:
            print("    %-22s n=%-6d mean %7.2f us  %5.1f%%" % (r.name, r.n, r.av / 1e3, 100.0 * r.tot / tot))
    for name in ("tex upload", "prog compile", "vkQueueSubmit", "dump BMP (RECOMP_GL_DUMP)"):
        r = next(iter(tp.query(f"select count(*) n, coalesce(sum(dur),0) tot from slice where name = '{name}'")))
        if r.n:
            print("  %-26s n=%-5d %.1f ms a frame" % (name, r.n, r.tot / 1e6 / n))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        run(p)
