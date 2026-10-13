#!/usr/bin/env python3
"""draw_anatomy.py -- what a captured frame is made of, from the XML call stream.

    renderdoccmd convert -f x.rdc -c xml -o x.xml        (once)
    qrenderdoc --python rd_counters.py  (RDC=x.rdc OUT=x_counters.json)   (once, ~30 s)
    python3 draw_anatomy.py x.xml x_counters.json [out.json]

GL state is tracked from the call stream, so no replay per draw. For every draw:
program, the textures bound on units 0-3 (GL names), index/vertex counts, FBO,
viewport, blend, depth test/write, plus the replay counters (samples passed =
pixels that survived depth; GPU duration is llvmpipe, relative only).
The JSON feeds instancing_potential.py.
"""
import json
import sys
import xml.etree.ElementTree as ET


def args(elem):
    d = {}
    for c in elem:
        n = c.get("name")
        if n is not None:
            d[n] = c.get("string") if c.get("string") is not None else (c.text or "")
    return d


def num(x, default=0):
    try:
        return int(x)
    except (TypeError, ValueError):
        try:
            return int(float(x))
        except (TypeError, ValueError):
            return default


def main(xml_path, counters_path, out_path):
    st = dict(prog=0, unit=0, tex=[0, 0, 0, 0], fbo=0, vp=(0, 0), blend=False,
              dtest=False, dwrite=True, cmask=True, bsrc="", bdst="")
    textures = {}          # GL name -> {w, h, fmt}
    draws = []
    n_calls = 0
    for ev, el in ET.iterparse(xml_path, events=("end",)):
        if el.tag != "chunk":
            continue
        n_calls += 1
        name = el.get("name")
        if name == "glUseProgram":
            st["prog"] = num(args(el).get("program"))
        elif name == "glActiveTexture":
            s = args(el).get("texture", "")
            if s.startswith("GL_TEXTURE") and s[10:].isdigit():
                st["unit"] = int(s[10:])
        elif name == "glBindTexture":
            a = args(el)
            if "2D" in a.get("target", "2D") and st["unit"] < 4:
                st["tex"][st["unit"]] = num(a.get("texture"))
        elif name in ("glTexImage2D", "glCompressedTexImage2D", "glTexStorage2D"):
            a = args(el)
            if num(a.get("level")) == 0:
                t = st["tex"][st["unit"]] if st["unit"] < 4 else 0
                textures[t] = dict(w=num(a.get("width")), h=num(a.get("height")),
                                   fmt=a.get("internalformat") or a.get("internalFormat") or "")
        elif name == "glBindFramebuffer":
            a = args(el)
            if "DRAW" in a.get("target", "") or a.get("target", "") == "GL_FRAMEBUFFER":
                st["fbo"] = num(a.get("framebuffer"))
        elif name == "glViewport":
            a = args(el)
            st["vp"] = (num(a.get("width")), num(a.get("height")))
        elif name in ("glEnable", "glDisable"):
            cap = args(el).get("cap", "")
            on = name == "glEnable"
            if cap == "GL_BLEND":
                st["blend"] = on
            elif cap == "GL_DEPTH_TEST":
                st["dtest"] = on
        elif name == "glDepthMask":
            st["dwrite"] = args(el).get("flag", "") in ("True", "1", "true")
        elif name == "glBlendFunc":
            a = args(el)
            st["bsrc"], st["bdst"] = a.get("sfactor", ""), a.get("dfactor", "")
        elif name in ("glDrawRangeElements", "glDrawElements", "glDrawArrays"):
            a = args(el)
            count = num(a.get("count"))
            verts = num(a.get("end")) + 1 if "end" in a else 0
            draws.append(dict(prog=st["prog"], tex=list(st["tex"]), idx=count, verts=verts,
                              fbo=st["fbo"], vp=list(st["vp"]), blend=st["blend"],
                              dtest=st["dtest"], dwrite=st["dwrite"],
                              bsrc=st["bsrc"] if st["blend"] else "",
                              bdst=st["bdst"] if st["blend"] else ""))
        el.clear()

    ctr = json.load(open(counters_path))
    cd = ctr["draws"]
    print("xml draws %d, replay draws %d, calls %d" % (len(draws), len(cd), n_calls))
    if len(draws) != len(cd):
        print("WARNING: draw counts differ; joining by order up to the shorter")
    for d, c in zip(draws, cd):
        d["eid"] = c["eid"]
        d["pixels"] = c.get("Samples Passed", 0)
        d["gpu_us"] = c.get("GPU Duration", 0.0) * 1e6
    json.dump(dict(draws=draws, textures=textures), open(out_path, "w"))
    print("wrote", out_path)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "/tmp/anatomy.json")
