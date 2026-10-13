# qrenderdoc --python rd_meshhash.py -- hash the vertex and index bytes each draw reads.
#
#   RDC=x.rdc OUT=/tmp/x_mesh.json DISPLAY=:0 qrenderdoc --python rd_meshhash.py
#
# The capture records the draws with their vertex/index buffers, but only the
# replay can hand back the bytes. Per draw: action.indexOffset / vertexOffset,
# the bound index buffer and the vertex buffers, and an md5 of the index values
# rebased to the lowest one plus an md5 of the vertex bytes the draw touches
# (min..max index). Two draws with the same hashes are the same mesh. Done with
# GetBufferData on the buffers once each and slicing, not by stepping the
# replay to every draw.
import hashlib, json, os, struct, time, traceback

RDC = os.environ.get("RDC", "/home/vifex/workpath/r36s/trace/nfs8.rdc")
OUT = os.environ.get("OUT", "/tmp/rd_mesh.json")
log = open(OUT + ".log", "w")
t0 = time.time()


def P(*a):
    log.write("%6.1fs " % (time.time() - t0) + " ".join(str(x) for x in a) + "\n")
    log.flush()


try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile()
    cap.OpenFile(RDC, "", None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    P("replay", res)
    draws = []

    def walk(acts):
        for a in acts:
            if a.flags & rd.ActionFlags.Drawcall:
                draws.append(a)
            walk(a.children)
    walk(ctl.GetRootActions())
    P("draws", len(draws))

    # The vertex/index buffers are streamed: the same buffer holds different bytes
    # at different times. So the bytes have to be read at the draw, which is what
    # SetFrameEvent is for -- only for the draws asked for, not all of them.
    want = set(int(x) for x in os.environ.get("EIDS", "").split(",") if x)
    out = []
    for a in draws:
        if want and a.eventId not in want:
            continue
        ctl.SetFrameEvent(a.eventId, False)
        st = ctl.GetPipelineState()
        ib = st.GetIBuffer()
        vbs = st.GetVBuffers()
        rec = dict(eid=a.eventId, idx=a.numIndices, base=a.baseVertex, ioff=a.indexOffset)
        try:
            raw = ctl.GetBufferData(ib.resourceId, ib.byteOffset + a.indexOffset * ib.byteStride, a.numIndices * ib.byteStride)
            fmt = {1: "B", 2: "H", 4: "I"}[ib.byteStride]
            ix = struct.unpack("<%d%s" % (a.numIndices, fmt), raw)
            lo, hi = min(ix), max(ix)
            rec["ihash"] = hashlib.md5(struct.pack("<%dI" % len(ix), *[i - lo for i in ix])).hexdigest()[:12]
            vattrs = st.GetVertexInputs()
            vh = hashlib.md5()
            for vb in vbs:
                if vb.resourceId == rd.ResourceId.Null():
                    continue
                stride = vb.byteStride or 1
                data = ctl.GetBufferData(vb.resourceId, vb.byteOffset + (lo + a.baseVertex) * stride, (hi - lo + 1) * stride)
                vh.update(data)
            rec["vhash"] = vh.hexdigest()[:12]
            rec["nverts"] = hi - lo + 1
        except Exception as e:
            rec["err"] = repr(e)[:120]
        out.append(rec)
        if len(out) % 20 == 0:
            P("done", len(out), rec)
    json.dump(out, open(OUT, "w"))
    P("wrote", OUT, len(out))
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
log.close()
os._exit(0)
