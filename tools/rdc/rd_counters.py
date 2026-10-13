# qrenderdoc --python rd_counters.py -- per-draw GPU counters and texture sizes of
# one capture, from ONE replay (no SetFrameEvent per draw: that replays the frame
# up to each event, 2400 times for a race frame).
#
#   RDC=x.rdc OUT=/tmp/x_counters.json DISPLAY=:0 qrenderdoc --python rd_counters.py
#
# Draws come out in capture order, so draw N here is draw N in the XML dump of
# the same capture (renderdoccmd convert -c xml), which carries the state.
import json, os, time, traceback

RDC = os.environ.get("RDC", "/home/vifex/workpath/r36s/trace/nfs8.rdc")
OUT = os.environ.get("OUT", "/tmp/rd_counters.json")
log = open(OUT + ".log", "w")
t0 = time.time()


def P(*a):
    log.write("%6.1fs " % (time.time() - t0) + " ".join(str(x) for x in a) + "\n")
    log.flush()


def rid(x):
    s = str(x)
    return int(s.split("::")[-1]) if s.split("::")[-1].isdigit() else s


try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile()
    cap.OpenFile(RDC, "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    P("replay open", res)

    names = {rid(r.resourceId): r.name for r in ctl.GetResources()}
    tex = {}
    for t in ctl.GetTextures():
        k = rid(t.resourceId)
        tex[k] = dict(w=t.width, h=t.height, mips=t.mips, fmt=t.format.Name(),
                      name=names.get(k, ""), glid=None)

    draws = []

    def walk(acts):
        for a in acts:
            if a.flags & rd.ActionFlags.Drawcall:
                draws.append(a)
            walk(a.children)
    walk(ctl.GetRootActions())
    P("draws", len(draws))

    want = {}
    for c in ctl.EnumerateCounters():
        d = ctl.DescribeCounter(c)
        if c in (rd.GPUCounter.SamplesPassed, rd.GPUCounter.EventGPUDuration,
                 rd.GPUCounter.PSInvocations):
            want[c] = d.name
    cval = {}
    for r in ctl.FetchCounters(list(want.keys())):
        d = ctl.DescribeCounter(r.counter)
        v = r.value.d if d.resultType == rd.CompType.Float else r.value.u64
        cval.setdefault(r.eventId, {})[want[r.counter]] = v
    P("counters fetched", list(want.values()))

    out = []
    for a in draws:
        rec = dict(eid=a.eventId, idx=a.numIndices, name=a.GetName(ctl.GetStructuredFile()))
        rec.update(cval.get(a.eventId, {}))
        out.append(rec)
    json.dump(dict(draws=out, textures=tex), open(OUT, "w"))
    P("wrote", OUT, len(out))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
log.close()
os._exit(0)
