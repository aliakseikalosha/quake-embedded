#!/usr/bin/env python3
"""Compare two golden files written by `hostcheck` (HGOLD=...): identical picture on every frame?

The first frames of demo1 are skipped: they show start-up state that is not fully
initialised in the engine and differs between builds of identical source."""
import sys


def load(path):
    frames = {}
    for line in open(path):
        t = line.split()
        if len(t) >= 3 and t[1].isdigit() and not (t[0] == "demo1" and int(t[1]) < 8):
            frames[(t[0], int(t[1]))] = " ".join(t[2:])
    return frames


a, b = load(sys.argv[1]), load(sys.argv[2])
bad = sorted(k for k in a if a[k] != b.get(k))
if not bad and len(a) == len(b):
    print(f"IDENTICAL: {len(a)} frames")
    sys.exit(0)
print(f"DIFFERENT: {len(bad)} of {len(a)} frames differ (frames {len(a)} vs {len(b)}); first: {bad[:3]}")
sys.exit(1)
