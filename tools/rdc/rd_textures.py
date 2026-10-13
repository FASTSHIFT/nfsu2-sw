# qrenderdoc --python rd_textures.py -- save the textures a capture uses as PNG.
#
#   RDC=x.rdc NAMES=1,2,3 OUTDIR=/tmp/tex DISPLAY=:0 qrenderdoc --python rd_textures.py
#
# NAMES are the GL texture names from draw_anatomy.py (the number glBindTexture
# was called with). RenderDoc's own ResourceIds differ from GL names; they are
# matched through the resource name "Texture <glname>". One replay, no
# per-draw SetFrameEvent.
import os, time, traceback

RDC = os.environ.get("RDC", "/home/vifex/workpath/r36s/trace/nfs8.rdc")
OUTDIR = os.environ.get("OUTDIR", "/tmp/tex")
NAMES = set(int(x) for x in os.environ.get("NAMES", "").split(",") if x)
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
    names = {str(r.resourceId): r.name for r in ctl.GetResources()}
    P("sample names", list(names.values())[:8])
    saved = 0
    for t in ctl.GetTextures():
        nm = names.get(str(t.resourceId), "")
        digits = "".join(ch for ch in nm.split()[-1] if ch.isdigit()) if nm else ""
        glname = int(digits) if digits and nm.lower().startswith("texture") else None
        if NAMES and glname not in NAMES:
            continue
        if t.width < 4 or t.height < 4:
            continue
        ts = rd.TextureSave()
        ts.resourceId = t.resourceId
        ts.destType = rd.FileType.PNG
        ts.mip = 0
        ts.slice.sliceIndex = 0
        ts.alpha = rd.AlphaMapping.Discard
        ctl.SaveTexture(ts, "%s/tex_%s_%dx%d.png" % (OUTDIR, glname if glname is not None else t.resourceId, t.width, t.height))
        saved += 1
    P("saved", saved)
    ctl.Shutdown()
    cap.Shutdown()
except Exception:
    P(traceback.format_exc())
log.close()
os._exit(0)
