#!/usr/bin/env python3
"""glab_cmp.py <dir> <n> -- compare glab_1..n.bmp: odd = mode 0, even = mode 1 (glab.sh).

Prints, for every pair, the number of pixels that differ at all and the biggest channel
difference. Pairs of the same mode (1-3, 3-5, 2-4, 4-6) are the control: how much the
scene moves between dumps by itself. Pairs of different modes (1-2, 2-3, ...) are the
test. Exit status 0 if no different-mode pair differs more than the worst control pair.
"""
import sys
import numpy as np
from PIL import Image


def load(d, k):
    return np.asarray(Image.open("%s/glab_%d.bmp" % (d, k)).convert("RGB"), dtype=np.int16)


def diff(a, b):
    if a.shape != b.shape:
        return None
    d = np.abs(a - b).max(axis=2)
    return int((d > 0).sum()), int(d.max()), a.shape[0] * a.shape[1]


def main():
    d, n = sys.argv[1], int(sys.argv[2])
    im = {k: load(d, k) for k in range(1, n + 1)}
    ctrl, test = [], []
    for i in range(1, n + 1):
        for j in range(i + 1, n + 1):
            if (j - i) > 2:
                continue
            r = diff(im[i], im[j])
            if r is None:
                print("size differs: %d vs %d" % (i, j)); continue
            kind = "control" if (i % 2) == (j % 2) else "A/B"
            (ctrl if kind == "control" else test).append((i, j, r))
            print("%-8s %d-%d: %7d of %d pixels differ, max channel diff %3d" % (kind, i, j, r[0], r[2], r[1]))
    worst_ctrl = max((r[0] for _, _, r in ctrl), default=0)
    worst_test = max((r[0] for _, _, r in test), default=0)
    print("worst control %d px, worst A/B %d px" % (worst_ctrl, worst_test))
    if worst_test <= worst_ctrl:
        print("OK: the A/B pairs differ no more than the scene does by itself")
        return 0
    print("CHECK: A/B differs more than the control")
    return 1


if __name__ == "__main__":
    sys.exit(main())
