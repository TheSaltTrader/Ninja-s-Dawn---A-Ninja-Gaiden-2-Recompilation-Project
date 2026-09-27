#!/usr/bin/env python3
"""P5 side-effect census: what the plugin's command processor did (p5_plugin.bin) against what the guest-thread
front end would do (p5_fe.bin) for the same packets. Records {u32 frame, kind, addr, value}.

Per kind: counts per side, and the ordered (addr, value) sequences compared after aligning the two streams on
their first common element (the two sides count frames differently: guest swap entry vs swap packet). Reports
exact-equal kinds, and for the rest the first differences. Read-pointer write-backs (kind 11) are compared as the
set of values (the plugin writes once per parse pass, the front end once per kick).
Usage: p5_compare.py p5_plugin.bin p5_fe.bin"""
import collections, struct, sys

KIND = {1: "MEM_WRITE", 2: "COND_WRITE mem", 3: "COND_WRITE reg", 4: "EVENT_WRITE_SHD value", 5: "EVENT_WRITE_SHD counter",
        6: "EVENT_WRITE_EXT", 7: "EVENT_WRITE_ZPD", 8: "REG_TO_MEM", 9: "INTERRUPT", 10: "REG_RMW",
        11: "read-pointer write-back", 12: "XE_SWAP", 13: "WAIT_REG_MEM", 14: "WAIT unsatisfied at decode"}


def load(path):
    b = open(path, "rb").read()
    return [struct.unpack_from("<4I", b, i) for i in range(0, len(b) - 15, 16)]


pl, fe = load(sys.argv[1]), load(sys.argv[2])
print("plugin side effects: %d, front end: %d" % (len(pl), len(fe)))
fr_pl = sorted(set(r[0] for r in pl)); fr_fe = sorted(set(r[0] for r in fe))
print("frames: plugin %s..%s, front end %s..%s" % (fr_pl[0] if fr_pl else "-", fr_pl[-1] if fr_pl else "-",
                                                  fr_fe[0] if fr_fe else "-", fr_fe[-1] if fr_fe else "-"))


def seq(recs, k):
    return [(a, v) for (f, kk, a, v) in recs if kk == k]


def align(a, b):
    """Trim the leading elements of the stream that starts earlier, and both tails to a common length."""
    if not a or not b: return a, b
    head = b[: min(8, len(b))]
    for off in range(0, min(len(a), 4000)):
        if a[off: off + len(head)] == head:
            a = a[off:]; break
    else:
        head = a[: min(8, len(a))]
        for off in range(0, min(len(b), 4000)):
            if b[off: off + len(head)] == head:
                b = b[off:]; break
    n = min(len(a), len(b))
    return a[:n], b[:n]


kinds = sorted(set(r[1] for r in pl) | set(r[1] for r in fe))
print("%-26s %9s %9s  %s" % ("kind", "plugin", "front end", "verdict"))
for k in kinds:
    a, b = seq(pl, k), seq(fe, k)
    name = KIND.get(k, str(k))
    if k == 11:
        va, vb = set(v for _, v in a), set(v for _, v in b)
        print("%-26s %9d %9d  values: plugin-only %d, front-end-only %d, common %d" % (name, len(a), len(b), len(va - vb), len(vb - va), len(va & vb)))
        continue
    if not a or not b:
        print("%-26s %9d %9d  %s" % (name, len(a), len(b), "ONE SIDE EMPTY"))
        continue
    aa, bb = align(a, b)
    diffs = [(i, x, y) for i, (x, y) in enumerate(zip(aa, bb)) if x != y]
    verdict = ("EQUAL over %d aligned" % len(aa)) if not diffs else ("%d of %d aligned differ" % (len(diffs), len(aa)))
    print("%-26s %9d %9d  %s" % (name, len(a), len(b), verdict))
    for i, x, y in diffs[:4]:
        print("      #%d plugin addr %08X value %08X | front end addr %08X value %08X" % (i, x[0], x[1], y[0], y[1]))
