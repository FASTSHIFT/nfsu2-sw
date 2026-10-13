# qrenderdoc --python rd_progress.py -- the colour target after chosen draws.
#
#   RDC=x.rdc EIDS=100,500,... FBO=366 OUTDIR=/tmp/prog DISPLAY=:0 qrenderdoc --python rd_progress.py
#
# Replays the frame once per EID in the list (a handful, not one per draw) and
# saves what the main colour target looks like at that point, so a stretch of
# draws can be looked at and named: the difference between two neighbouring
# pictures is what the draws between them drew.
import os, time, traceback

RDC = os.environ.get("RDC", "/home/vifex/workpath/r36s/trace/nfs8.rdc")
OUTDIR = os.environ.get("OUTDIR", "/tmp/prog")
EIDS = [int(x) for x in os.environ.get("EIDS", "").split(",") if x]
os.makedirs(OUTDIR, exist_ok=True)
log = open(OUTDIR + "/log.txt", "w")
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
    for e in EIDS:
        ctl.SetFrameEvent(e, True)
        outs = ctl.GetPipelineState().GetOutputTargets()
        if not outs:
            P("eid", e, "no target")
            continue
        ts = rd.TextureSave()
        ts.resourceId = outs[0].resource
        ts.destType = rd.FileType.PNG
        ts.mip = 0
        ts.alpha = rd.AlphaMapping.Discard
        ctl.SaveTexture(ts, "%s/eid_%06d.png" % (OUTDIR, e))
        P("saved", e)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
log.close()
os._exit(0)
