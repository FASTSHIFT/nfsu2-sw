#!/usr/bin/env python3
"""instancing_potential.py anatomy.json xml -- how many draws would remain if draws of the
same mesh (program, textures, blend state, index count, vertex count) in the same pass
were issued as one instanced draw. Assumes the (checked on three groups) premise that
equal counts mean equal geometry; ranges the estimate by also reporting the variant that
only merges *consecutive* repeats (what a renderer can do without reordering draws:
reordering opaque draws is safe with depth test, blended ones are not).
"""
import json, sys, collections
d = json.load(open(sys.argv[1]))["draws"]
def seg(i, x):
    if x["fbo"] != 366 or tuple(x["vp"]) != (640, 480): return "A passes (128/320 env+reflection)" if x["fbo"] != 593 else "F post-process"
    if i <= 1569: return "B world opaque"
    if i <= 1776: return "C world blended"
    if i <= 1933: return "D cars opaque"
    if i <= 2085: return "E translucent"
    return "G HUD"
def key(x): return (x["fbo"], x["prog"], tuple(x["tex"]), x["blend"], x["bsrc"], x["bdst"], x["dtest"], x["dwrite"], x["idx"], x["verts"])
rows = collections.defaultdict(lambda: [0, set(), 0, 0])
prev = None
for i, x in enumerate(d):
    s = seg(i, x); r = rows[s]; r[0] += 1; k = key(x); r[1].add(k)
    if k != prev: r[2] += 1          # consecutive repeats only
    prev = k
print("%-36s %6s | %15s %8s | %17s %8s" % ("segment", "draws", "same mesh (any)", "saved", "consecutive only", "saved"))
tot = [0, 0, 0]
for s in sorted(rows):
    n, ks, cons, _ = rows[s]
    print("%-36s %6d | %15d %7.0f%% | %17d %7.0f%%" % (s, n, len(ks), 100 * (1 - len(ks) / n), cons, 100 * (1 - cons / n)))
    tot[0] += n; tot[1] += len(ks); tot[2] += cons
print("%-36s %6d | %15d %7.0f%% | %17d %7.0f%%" % ("total", tot[0], tot[1], 100 * (1 - tot[1] / tot[0]), tot[2], 100 * (1 - tot[2] / tot[0])))
