#!/usr/bin/env python3
"""Summarise a prof.csv written by a PD_PROFILE build (see scripts/pd-bench.sh).

    scripts/pd-report.py bench-results/a.csv               per-demo summary
    scripts/pd-report.py bench-results/a.csv b.csv         ... and b compared with a

Times are per frame in milliseconds (game code only); "wall" fps includes what the system does
between frames. Sections nest: scan contains dsurf, which contains cache/spans/zspan/other, and so on.
The finer sections and counters are only present in builds made with -DPD_PROFILE_FINE=ON."""
import collections
import statistics as st
import sys

TOP = ["input", "server", "client", "setup", "world", "bent", "scan", "ent", "view", "part",
       "upscale", "hud", "vid", "snd", "pal"]
FINE = [("world", ["wmark", "wefrag", "wsurfs", "face"]), ("scan", ["se_ins", "se_gen", "se_rem", "se_step", "dsurf"]),
        ("dsurf", ["cache", "spans", "zspan", "other", "grad"]), ("cache", ["scalloc", "light", "blocks"]),
        ("server", ["sv_run", "sv_phys", "sv_send", "qc"]), ("ent", ["alias", "atrans", "apoly", "lpt", "abbox"])]


def load(path):
    hdr, rows, bench = None, [], []
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("H,"):
            hdr = line.split(",")[1:]
        elif line.startswith("P,") and hdr:
            v = line.split(",")[1:]
            if len(v) == len(hdr):
                try:
                    rows.append(dict(zip(hdr, map(int, v))))
                except ValueError:
                    pass
        elif line.startswith("BENCH,"):
            p = line.split(",")
            bench.append((int(p[1]), p[2], p[3] if len(p) > 3 else ""))
        elif line.startswith("L,") and " fps" in line and "frames" in line:
            print("  game says:", line.split(",", 2)[2].strip())
    return [r for r in rows if r["period"] > 0], sorted(bench)


def split(rows, bench):
    demos, cur, ev = collections.OrderedDict(), None, list(bench)
    for r in rows:
        while ev and ev[0][0] <= r["ms"]:
            _, kind, name = ev.pop(0)
            cur = name if kind == "start" else None
        if cur:
            demos.setdefault(cur, []).append(r)
    return demos or {"all": rows}


def mean(rs, k):
    return sum(r[k] for r in rs) / len(rs) / 1000.0


def summarize(name, rs):
    n = len(rs)
    work = sum(r["frame"] for r in rs) / 1e6
    wall = sum(r["period"] for r in rs) / 1e6
    per = sorted(r["period"] / 1000 for r in rs)
    pct = lambda p: per[min(n - 1, int(n * p / 100))]
    print(f"\n### {name}: {n} frames | {1000 * work / n:.1f} ms/frame of game work | {n / wall:.1f} fps wall "
          f"| frame p50 {pct(50):.0f} p95 {pct(95):.0f} max {per[-1]:.0f} ms")
    tot = sum(r["frame"] for r in rs)
    print("   " + "  ".join(f"{k}={mean(rs, k):.1f}({100 * sum(r[k] for r in rs) / tot:.0f}%)" for k in TOP if mean(rs, k) >= 0.05))
    for parent, kids in FINE:
        vals = [(k, mean(rs, k)) for k in kids if k in rs[0] and mean(rs, k) > 0]
        if vals:
            print(f"   {parent} ({mean(rs, parent):.1f}): " + "  ".join(f"{k}={v:.1f}" for k, v in vals))


def compare(name, a, b):
    print(f"\n### {name}: second file vs first (ms/frame)")
    fa, fb = mean(a, "frame"), mean(b, "frame")
    print(f"   total {fa:.1f} -> {fb:.1f}  ({100 * (fb - fa) / fa:+.1f}%)")
    print("   " + "  ".join(f"{k} {mean(a, k):.1f}->{mean(b, k):.1f}" for k in TOP if max(mean(a, k), mean(b, k)) >= 0.3))


def main():
    files = sys.argv[1:3]
    if not files:
        sys.exit(__doc__)
    data = []
    for f in files:
        print(f"== {f}")
        rows, bench = load(f)
        data.append(split(rows, bench))
        summarize("TOTAL", [r for d in data[-1].values() for r in d])
        for name, rs in data[-1].items():
            summarize(name, rs)
    if len(data) == 2:
        for name in data[0]:
            if name in data[1]:
                compare(name, data[0][name], data[1][name])


if __name__ == "__main__":
    main()
