#!/usr/bin/env python3
"""P3: which XDK device word each register the draw-time flush writes comes from.

Input: p3_guest.bin format v2 (fable2_p2_census.cpp, FABLE2_P3MAP): per recorded call
  {u32 hook, cur_in, cur_out, cur2_in, cur2_out} + device[0x5000] (big-endian) + {u32 n} + the n bytes the call wrote.
The written bytes are decoded as PM4 (big-endian dwords): type 0 register writes, type 3 SET_CONSTANT into the
register / fetch / bool / loop spaces. For every (register, value) written, every device word holding that value at
the call's exit votes; per register the best word and its agreement (share of the register's writes it explains) are
reported. A word explains a write only when the value is not a trivial one (0, 1, 0xFFFFFFFF), which match anywhere.

Usage: p3_flush.py --guest p3_guest.bin [--json out.json]"""
import argparse, collections, json, struct

ap = argparse.ArgumentParser()
ap.add_argument("--guest", required=True)
ap.add_argument("--json", default=None)
args = ap.parse_args()

DEV = 0x5000
data = open(args.guest, "rb").read()
calls = []
off = 0
while off + 20 + DEV + 4 <= len(data):
    hook, ci, co, c2i, c2o = struct.unpack_from("<5I", data, off)
    dev = struct.unpack_from(">%dI" % (DEV // 4), data, off + 20)
    (n,) = struct.unpack_from("<I", data, off + 20 + DEV)
    body = data[off + 24 + DEV: off + 24 + DEV + n]
    calls.append((hook, dev, body))
    off += 24 + DEV + n
print("calls: %d (with written bytes: %d)" % (len(calls), sum(1 for c in calls if c[2])))

SPACE = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}
TRIVIAL = {0, 1, 0xFFFFFFFF}


def decode(body):
    """(register, value) pairs the packets write."""
    w = struct.unpack(">%dI" % (len(body) // 4), body[: len(body) // 4 * 4])
    i, out, unknown = 0, [], collections.Counter()
    while i < len(w):
        h = w[i]
        t = h >> 30
        if h == 0xFFFFFFFF:   # filler the library leaves in the reserved space (ranges start "FFFFFFFF 80000000 ...")
            i += 1
            continue
        if t == 0:
            base, cnt, one = h & 0x7FFF, ((h >> 16) & 0x3FFF) + 1, (h >> 15) & 1
            for k in range(cnt):
                if i + 1 + k < len(w):
                    out.append((base if one else base + k, w[i + 1 + k]))
            i += 1 + cnt
        elif t == 3:
            op, cnt = (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1
            if op == 0x2D and i + 1 < len(w):   # SET_CONSTANT
                d = w[i + 1]
                idx, typ = d & 0x7FF, (d >> 16) & 0xFF
                base = SPACE.get(typ)
                if base is not None:
                    for k in range(cnt - 1):
                        if i + 2 + k < len(w):
                            out.append((base + idx + k, w[i + 2 + k]))
            else:
                unknown[op] += 1
            i += 1 + cnt
        elif t == 2:
            i += 1
        else:   # type 1: two registers
            if i + 2 < len(w):
                out.append((h & 0x7FF, w[i + 1]))
                out.append(((h >> 11) & 0x7FF, w[i + 2]))
            i += 3
    return out, unknown


votes = collections.defaultdict(collections.Counter)
writes = collections.Counter()
nontrivial = collections.Counter()
ops = collections.Counter()
by_hook = collections.defaultdict(collections.Counter)
for hook, dev, body in calls:
    if not body:
        continue
    pairs, unk = decode(body)
    ops.update(unk)
    index = collections.defaultdict(list)
    for j, v in enumerate(dev):
        index[v].append(j * 4)
    for reg, val in pairs:
        writes[reg] += 1
        by_hook[hook][reg >> 8] += 1
        if val in TRIVIAL:
            continue
        nontrivial[reg] += 1
        for o in index.get(val, ()):
            votes[reg][o] += 1

print("registers written: %d, writes %d; other type-3 opcodes: %s" %
      (len(writes), sum(writes.values()), ", ".join("0x%02X:%d" % kv for kv in ops.most_common(10))))
rows = []
for reg in sorted(writes):
    nt = nontrivial[reg]
    if not nt:
        rows.append((reg, writes[reg], 0, None, 0.0))
        continue
    o, c = votes[reg].most_common(1)[0] if votes[reg] else (None, 0)
    rows.append((reg, writes[reg], nt, o, c / nt))
good = [r for r in rows if r[3] is not None and r[4] >= 0.95]
print("registers whose non-trivial writes one device word explains >= 95%%: %d of %d with non-trivial writes" %
      (len(good), sum(1 for r in rows if r[2])))
print("reg      writes  nontriv  best dev word  agreement")
for reg, wr, nt, o, a in rows:
    if nt:
        print("0x%04X %7d %8d   %s   %5.1f%%" % (reg, wr, nt, ("+0x%04X" % o) if o is not None else "  none ", 100 * a))
if args.json:
    json.dump([{"reg": r[0], "writes": r[1], "nontrivial": r[2], "dev": r[3], "agree": r[4]} for r in rows],
              open(args.json, "w"), indent=0)
