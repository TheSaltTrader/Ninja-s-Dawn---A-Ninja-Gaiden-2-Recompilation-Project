#!/usr/bin/env python3
"""Stutter comparison across legs on the same route: per 5-second [swap] window, aligned by seconds since the log's
first line, the worst frame, the hitch count and the new instrumentation (texture creations and their ms, texture-
load CPU ms, pack read ms). Also the sum over the route and the ten worst frames from the [hitch] lines.
Usage: hitch_compare.py LABEL=path/to/ng2_NNN.log [LABEL=... ...]"""
import re
import sys

SWAP = re.compile(r"^\[(\S+) (\S+)\].*\[swap\] ([\d.]+) guest fps \((\d+) swaps in ([\d.]+)s\)  interval ms: p50 ([\d.]+)  "
                  r"p99 ([\d.]+)  worst ([\d.]+)  hitches (\d+)(?:.*?\| window: creates (\d+) \(([\d.]+) ms\), "
                  r"tex-load cpu ([\d.]+) ms, pack read ([\d.]+) ms)?")
HITCH = re.compile(r"^\[(\S+) (\S+)\].*\[hitch\] ([\d.]+) ms frame: (\d+) textures \((\d+) KB\), uploads (\d+) KB.*?draws (\d+)"
                   r"(?:, creates (\d+) \(([\d.]+) ms\), tex-load cpu ([\d.]+) ms, pack (\d+) \(read ([\d.]+) ms\))?")


def secs(t):
    h, m, s = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def parse(path):
    t0 = None
    wins = []
    hitches = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("["):
            continue
        if t0 is None:
            t0 = secs(line[12:24])
        m = SWAP.match(line)
        if m:
            g = m.groups()
            wins.append(dict(t=secs(g[1]) - t0, fps=float(g[2]), p50=float(g[5]), p99=float(g[6]), worst=float(g[7]),
                             hitches=int(g[8]), creates=int(g[9] or 0), create_ms=float(g[10] or 0),
                             load_ms=float(g[11] or 0), pack_ms=float(g[12] or 0)))
            continue
        m = HITCH.match(line)
        if m:
            g = m.groups()
            hitches.append(dict(t=secs(g[1]) - t0, ms=float(g[2]), tex=int(g[3]), kb=int(g[4]), up=int(g[5]),
                                creates=int(g[7] or 0), create_ms=float(g[8] or 0), load_ms=float(g[9] or 0),
                                pack=int(g[10] or 0), pack_ms=float(g[11] or 0)))
    return wins, hitches


legs = [a.split("=", 1) for a in sys.argv[1:]]
data = {label: parse(path) for label, path in legs}
print("%-12s %7s %7s %8s %8s %9s %9s %9s" % ("leg", "windows", "hitches", "worst", ">100ms", "create ms", "load ms", "pack ms"))
for label, (w, h) in data.items():
    print("%-12s %7d %7d %8.0f %8d %9.0f %9.0f %9.0f" % (label, len(w), sum(x["hitches"] for x in w),
                                                     max((x["worst"] for x in w), default=0),
                                                     sum(1 for x in w if x["worst"] > 100),
                                                     sum(x["create_ms"] for x in w), sum(x["load_ms"] for x in w),
                                                     sum(x["pack_ms"] for x in w)))
print()
print("per window (seconds since start): worst ms / hitches / load ms per leg")
n = max(len(w) for w, _ in data.values())
labels = list(data.keys())
print("%6s " % "t" + " ".join("%-22s" % l for l in labels))
for i in range(n):
    row = []
    t = None
    for l in labels:
        w = data[l][0]
        if i < len(w):
            t = t or w[i]["t"]
            row.append("%5.0f/%2d/%5.0f        " % (w[i]["worst"], w[i]["hitches"], w[i]["load_ms"]))
        else:
            row.append("%-22s" % "-")
    print("%6.0f " % (t or 0) + " ".join(row))
print()
for l in labels:
    h = sorted(data[l][1], key=lambda x: -x["ms"])[:6]
    print("%s: worst frames" % l)
    for x in h:
        print("   t=%5.0f %5.0f ms  tex %3d (%6d KB) up %6d KB  creates %3d (%5.1f ms)  load %6.1f ms  pack %3d (%5.1f ms)"
              % (x["t"], x["ms"], x["tex"], x["kb"], x["up"], x["creates"], x["create_ms"], x["load_ms"], x["pack"], x["pack_ms"]))
