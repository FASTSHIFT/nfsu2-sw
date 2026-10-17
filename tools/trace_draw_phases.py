#!/usr/bin/env python3
"""trace_draw_phases.py x.pftrace [...] -- where one draw's CPU time goes in the GL thread.

nv2a_gl.c marks the phases of 1 draw in 16 (XTRACE_DRAW_MARK): "d:surface+program",
"d:textures", "d:uniforms", "d:state", "d:vertices", "d:indices", "d:glDrawElements".
Each is a span from the previous mark, so the averages add up to the sampled draw's
time. Prints count, mean us and the share per phase, and the frame-level GL spans
for scale. Needs the `perfetto` pip package (like tools/trace_summary.py).
"""
import sys
from perfetto.trace_processor import TraceProcessor


def phases(path):
    tp = TraceProcessor(trace=path)
    rows = list(tp.query("""
        select name, count(*) n, sum(dur) tot, avg(dur) av
        from slice where name like 'd:%' group by name order by tot desc
    """))
    print("== %s" % path.split("/")[-1])
    if not rows:
        print("   no d:* spans (no draw sampling in this recording)")
        return
    total = sum(int(r.tot) for r in rows)
    n_draw = max(int(r.n) for r in rows)
    print("   %-20s %8s %10s %7s" % ("phase", "count", "mean us", "share"))
    for r in rows:
        print("   %-20s %8d %10.2f %6.1f%%" % (r.name, r.n, r.av / 1e3, 100.0 * r.tot / total))
    print("   %-20s %8d %10.2f" % ("sum of phases", n_draw, total / n_draw / 1e3))
    # Draw counters written with each sampled draw.
    for c in tp.query("""
        select t.name name, avg(c.value) av, count(*) n from counter c
        join counter_track t on c.track_id = t.id where t.name like 'draw %' group by t.name
    """):
        print("   %-20s mean %.1f (n %d)" % (c.name, c.av, c.n))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        phases(p)
