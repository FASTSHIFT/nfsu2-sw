#!/usr/bin/env python3
"""trace_summary.py x.pftrace [...] -- who is busy in an xtrace recording (docs/04 3).

For every thread: time covered by its top-level slices, split into work and wait,
as a share of the recording, then its biggest slices by total time (name, count,
total ms, average us). `idle`, `queue empty`, `wait GL (...)`, `GIL wait`,
`BlockOnFence` and `frame fence` are waiting, the rest is work. The main thread's
`frame` slices give the frame time.

Uses perfetto's trace processor (pip package `perfetto`, downloads its shell once).
"""
import sys
from perfetto.trace_processor import TraceProcessor

WAIT_PREFIX = ("idle", "queue empty", "wait GL", "wait VK", "GIL wait", "BlockOnFence", "frame fence",
               "blit fence", "vblank", "page flip", "k:NtWait", "k:KeWait", "k:KeDelay", "apu idle")


def is_wait(name):
    return any(name.startswith(p) for p in WAIT_PREFIX)


def summarize(path, top=6):
    tp = TraceProcessor(trace=path)
    span = next(iter(tp.query("select min(ts) a, max(ts+dur) b from slice")))
    t0, t1 = int(span.a), int(span.b)
    total_ms = (t1 - t0) / 1e6
    print("== %s: %.2f s" % (path.split("/")[-1], total_ms / 1e3))
    threads = list(tp.query("""
        select utid, coalesce(name, 'tid '||tid) as name from thread
        where utid in (select distinct utid from thread_track where id in (select track_id from slice))
    """))
    rows = []
    for th in threads:
        q = list(tp.query("""
            select s.name as name, count(*) as n, sum(s.dur) as dur
            from slice s join thread_track tt on s.track_id = tt.id
            where tt.utid = %d and s.depth = 0 group by s.name order by dur desc
        """ % th.utid))
        if not q:
            continue
        work = wait = 0
        for r in q:
            if is_wait(r.name):
                wait += int(r.dur)
            else:
                work += int(r.dur)
        rows.append((th.name, work / 1e6, wait / 1e6, q))
    # Guest code itself is not instrumented: on `game main` and the guest workers
    # the time no slice covers is the lifted game code running.
    print("%-18s %9s %9s %10s %7s" % ("thread", "work ms", "wait ms", "uncovered", "busy %"))
    for name, work, wait, q in sorted(rows, key=lambda r: -r[1]):
        unc = max(0.0, total_ms - work - wait)
        guest = name.startswith(("game main", "guest"))
        print("%-18s %9.0f %9.0f %10.0f %6.0f%%%s" % (
            name[:18], work, wait, unc, 100.0 * (work + (unc if guest else 0)) / total_ms,
            "   <- uncovered = guest code" if guest and unc > 0.2 * total_ms else ""))
    for name, work, wait, q in sorted(rows, key=lambda r: -r[1])[:7]:
        print("\n  %s -- top slices (depth 0)" % name)
        for r in q[:top]:
            print("    %-30s n=%-6d total %8.1f ms  avg %8.1f us%s" % (
                r.name[:30], r.n, r.dur / 1e6, r.dur / r.n / 1e3, "   (wait)" if is_wait(r.name) else ""))
    d = sorted(r.dur / 1e6 for r in tp.query("select dur from slice where name = 'frame'"))
    if len(d) > 2:
        print("\n  'frame' slices: %d, p50 %.1f ms, min %.1f, max %.1f" % (len(d), d[len(d) // 2], d[0], d[-1]))
    tp.close()


if __name__ == "__main__":
    for p in sys.argv[1:]:
        summarize(p)
        print()
