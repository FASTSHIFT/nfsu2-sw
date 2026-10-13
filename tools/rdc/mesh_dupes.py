#!/usr/bin/env python3
"""mesh_dupes.py x.xml -- do draws that look alike differ only in their vertex constants?

Draws are grouped by (program, tex0, tex1, index count, vertex count). For the biggest
groups it reports which rows of the 192-row vertex-constant block c[] (glUniform4fv,
location 0, 768 floats) take more than one value inside the group. The hash check that
the vertex and index bytes are really equal is rd_meshhash.py (it needs a replay).
"""
import collections
import sys
import xml.etree.ElementTree as ET


def read_draws(path):
    cur = dict(prog=0, tex=[0, 0, 0, 0], unit=0)
    c0 = None
    draws = []
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag != "chunk":
            continue
        n = el.get("name")
        if n == "glUseProgram":
            for c in el:
                if c.get("name") == "program":
                    cur["prog"] = int(c.text or 0)
        elif n == "glActiveTexture":
            for c in el:
                s = c.get("string") or ""
                if s.startswith("GL_TEXTURE") and s[10:].isdigit():
                    cur["unit"] = int(s[10:])
        elif n == "glBindTexture":
            for c in el:
                if c.get("name") == "texture" and cur["unit"] < 4:
                    cur["tex"][cur["unit"]] = int(c.text or 0)
        elif n == "glUniform4fv":
            loc = vals = None
            for c in el:
                if c.get("name") == "location":
                    loc = int(c.text or 0)
                if c.get("name") == "values":          # an <array> of <float>
                    vals = [float(v.text) for v in c if v.tag == "float"]
            if loc == 0 and vals and len(vals) == 768:
                c0 = vals
        elif n in ("glDrawRangeElements", "glDrawElements"):
            a = {c.get("name"): (c.text or "") for c in el}
            draws.append(dict(key=(cur["prog"], cur["tex"][0], cur["tex"][1],
                                   int(a.get("count", 0)), int(a.get("end", 0)) + 1),
                              c=c0))
        el.clear()
    return draws


def main(path, top=4):
    draws = read_draws(path)
    grp = collections.defaultdict(list)
    for d in draws:
        grp[d["key"]].append(d)
    print("draws %d, distinct (prog, tex0, tex1, idx, verts): %d" % (len(draws), len(grp)))
    for key, v in sorted(grp.items(), key=lambda kv: -len(kv[1]))[:top]:
        v = [d for d in v if d["c"]]
        rows = {}
        for r in range(192):
            vals = {tuple(d["c"][r * 4:r * 4 + 4]) for d in v}
            if len(vals) > 1:
                rows[r] = len(vals)
        print("group prog %d tex (%d,%d) idx %d verts %d: %d draws, %d of 192 rows differ" % (
            key[0], key[1], key[2], key[3], key[4], len(v), len(rows)))
        print("   rows (distinct values):", sorted(rows.items()))


if __name__ == "__main__":
    main(sys.argv[1])
