#!/usr/bin/env python3
"""P3 register map, NG2 variant: the plugin's packet boundaries drive the decode.

p3_flush.py decodes each recorded call's bytes by the PM4 grammar alone; on NG2 that loses sync in 990 of 1049
ranges (leg ng2_072): a call's range begins with the dword the library RESERVED at the cursor and fills later
(02000200 / 0BADF00D), and the draw-entry ranges carry inline data blocks. The plugin's parser executed the same
bytes and recorded every packet's address and header (p2_packets.bin, NG2_P2 on in the same leg), so here each
packet inside a call's range is decoded from the plugin's header, with its data read from the call's bytes at
(address - range start). LOAD_ALU_CONSTANT (from guest memory) is counted but not decoded.

Input: p3_guest.bin (format v2: {u32 hook, cur_in, cur_out, cur2_in, cur2_out} + device[0x5000] big-endian + {u32 n}
+ n bytes), p2_packets.bin ({magic, ver, rec_size, first} + records {u32 frame, addr, header, bin}), the guest
frame of the map and the plugin's frame offset tolerance (+-1: the two sides count frames differently).
Usage: p3_flush2.py --guest p3_guest.bin --packets p2_packets.bin --frame 6620 [--json out.json]"""
import argparse, collections, json, struct

ap = argparse.ArgumentParser()
ap.add_argument("--guest", required=True)
ap.add_argument("--packets", required=True)
ap.add_argument("--frame", type=int, required=True)
ap.add_argument("--json", default=None)
ap.add_argument("--base_shift", type=int, default=1, help="dwords between the range start (cur_in) and the first recorded byte: 1 for format v3 recordings ([cur_in+4, cur_out+4)), 0 for the first leg's file")
args = ap.parse_args()

DEV = 0x5000
PHYS = 0x1FFFFFFF
data = open(args.guest, "rb").read()
calls = []
off = 0
while off + 24 + DEV <= len(data):
    hook, ci, co, c2i, c2o = struct.unpack_from("<5I", data, off)
    dev = struct.unpack_from(">%dI" % (DEV // 4), data, off + 20)
    (n,) = struct.unpack_from("<I", data, off + 20 + DEV)
    body = data[off + 24 + DEV: off + 24 + DEV + n]
    calls.append((hook, ci, co, dev, body))
    off += 24 + DEV + n
print("calls: %d (with written bytes: %d)" % (len(calls), sum(1 for c in calls if c[4])))

p = open(args.packets, "rb").read()
magic, ver, rec_size, p_start = struct.unpack("<4I", p[:16])
pk = []   # (addr, header) for the frames around the map frame
lo_f, hi_f = args.frame - 1, args.frame + 1
for i in range(16, len(p) - 15, 16):
    frame, addr, header, binsel = struct.unpack_from("<4I", p, i)
    if lo_f <= frame <= hi_f and header != 0x7FFFFFFF:
        pk.append((addr & PHYS, header))
pk.sort()
addrs = [a for a, h in pk]
print("plugin packets in frames %d..%d: %d" % (lo_f, hi_f, len(pk)))
import bisect

SPACE = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}
TRIVIAL = {0, 1, 0xFFFFFFFF}
DRAW = {0x22, 0x36}


def decode_call(ci, co, body):
    """(register, value) pairs from the plugin's packets inside [ci, co), data from the call's bytes."""
    lo, hi = ci & PHYS, co & PHYS
    w = struct.unpack(">%dI" % (len(body) // 4), body[: len(body) // 4 * 4])
    out, ops, outside, seen = [], collections.Counter(), 0, 0
    a = bisect.bisect_left(addrs, lo)
    last_end = None
    while a < len(pk) and pk[a][0] < hi:
        addr, h = pk[a]
        a += 1
        if addr == last_end:   # the same packet recorded twice (a tiled or replayed execution): once is enough
            pass
        i = (addr - lo) // 4 - args.base_shift
        if i < 0:
            outside += 1
            continue
        if i >= len(w):
            outside += 1
            continue
        seen += 1
        t = h >> 30
        if t == 0:
            base, cnt, one = h & 0x7FFF, ((h >> 16) & 0x3FFF) + 1, (h >> 15) & 1
            for k in range(cnt):
                if i + 1 + k < len(w):
                    out.append((base if one else base + k, w[i + 1 + k]))
        elif t == 1:
            if i + 2 < len(w):
                out.append((h & 0x7FF, w[i + 1]))
                out.append(((h >> 11) & 0x7FF, w[i + 2]))
        elif t == 3:
            op, cnt = (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1
            ops[op] += 1
            if op == 0x2D and i + 1 < len(w):   # SET_CONSTANT
                d = w[i + 1]
                idx, typ = d & 0x7FF, (d >> 16) & 0xFF
                base = SPACE.get(typ)
                if base is not None:
                    for k in range(cnt - 1):
                        if i + 2 + k < len(w):
                            out.append((base + idx + k, w[i + 2 + k]))
        last_end = addr
    return out, ops, outside, seen


votes = collections.defaultdict(collections.Counter)
writes = collections.Counter()
nontrivial = collections.Counter()
ops_all = collections.Counter()
by_hook_pk = collections.Counter()
calls_no_pk = 0
outside_total = 0
for hook, ci, co, dev, body in calls:
    if not body:
        continue
    pairs, ops, outside, seen = decode_call(ci, co, body)
    ops_all.update(ops)
    by_hook_pk[hook] += seen
    outside_total += outside
    if not seen:
        calls_no_pk += 1
    index = collections.defaultdict(list)
    for j, v in enumerate(dev):
        index[v].append(j * 4)
    for reg, val in pairs:
        writes[reg] += 1
        if val in TRIVIAL:
            continue
        nontrivial[reg] += 1
        for o in index.get(val, ()):
            votes[reg][o] += 1

print("calls with no plugin packet inside their range: %d; packets whose address lies past the recorded bytes: %d" %
      (calls_no_pk, outside_total))
print("type-3 opcodes: %s" % ", ".join("0x%02X:%d" % kv for kv in ops_all.most_common(12)))
print("registers written: %d, writes %d" % (len(writes), sum(writes.values())))
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


def cls(r):
    if 0x4000 <= r < 0x4800: return "float"
    if 0x4800 <= r < 0x4900: return "fetch"
    if 0x4900 <= r < 0x4908: return "bool"
    if 0x4908 <= r < 0x4928: return "loop"
    if 0x2000 <= r < 0x2400: return "render"
    return "other(%04X)" % (r & 0xF000)


tot, gd, nt = collections.Counter(), collections.Counter(), collections.Counter()
for reg, wr, n, o, a in rows:
    c = cls(reg)
    tot[c] += 1
    if n: nt[c] += 1
    if n and o is not None and a >= 0.95: gd[c] += 1
for c in sorted(tot):
    print("  %-14s regs %5d nontrivial %5d explained %5d" % (c, tot[c], nt[c], gd[c]))
print("reg      writes  nontriv  best dev word  agreement")
for reg, wr, n, o, a in rows:
    if n:
        print("0x%04X %7d %8d   %s   %5.1f%%" % (reg, wr, n, ("+0x%04X" % o) if o is not None else "  none ", 100 * a))
if args.json:
    json.dump([{"reg": r[0], "writes": r[1], "nontrivial": r[2], "dev": r[3], "agree": r[4]} for r in rows],
              open(args.json, "w"), indent=0)
