#!/usr/bin/env python3
"""P3 stage 2 - where each per-draw register's value comes from (NG2_P3SRC recording, one guest frame).

p3_fe.bin: every draw the front end decoded in the frame: {u32 packet_addr, issuer_ib_packet_addr, ordinal,
fe_frame} + regs 0x2000-0x23FF + 0x4000-0x4927 (the front end's register file at the draw - proven equal to the
plugin's, leg ng2_080).
p3_src.bin: every hooked call in the frame: {u32 hook, tid, cur_in, cur_out, r3..r10, nsamples} + nsamples x
{u32 arg_index, va, 256 bytes} (a sample behind each pointer-like argument, taken at the call's exit).
p3_guest.bin (NG2_P3MAP, same frame): the device object at the call's exit (joined by hook + cur_in).

Each draw is joined to the call whose cursor range [cur_in+4, cur_out+4) holds its packet - or, for a draw
reached through an INDIRECT_BUFFER (issuer != 0), the call whose range holds the IB packet - and then to that
call's enclosing calls (outer ranges), innermost first. For every register in the per-draw set the draw's value is
searched in each candidate call's arguments (immediate), samples (big-endian dwords) and device words; the vote
that explains the most draws wins. Usage: p3_sources.py --fe p3_fe.bin --src p3_src.bin [--guest p3_guest.bin]
[--regs 2200,4011-4019,...]"""
import argparse, collections, struct, bisect

ap = argparse.ArgumentParser()
ap.add_argument("--fe", required=True)
ap.add_argument("--src", required=True)
ap.add_argument("--guest", default=None)
ap.add_argument("--regs", default="21F9-21FC,2200,2203,2318,231B,4011-4019,4322-4326,443E,4443,4466,446B,4803-4805,"
                                  "481C,4822,4828,482E,4834,483A,4840,4846,484C,4852,4858,485E,4864,486A,4870,4876")
args = ap.parse_args()
PHYS = 0x1FFFFFFF


def regset(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a, 16), int(b, 16) + 1))
        else:
            out.append(int(part, 16))
    return out


REGS = regset(args.regs)

fe = open(args.fe, "rb").read()
REC = 16 + 0x400 * 4 + 0x928 * 4
draws = []
for i in range(0, len(fe) - REC + 1, REC):
    addr, issuer, ordn, fef = struct.unpack_from("<4I", fe, i)
    r2 = struct.unpack_from("<%dI" % 0x400, fe, i + 16)
    r4 = struct.unpack_from("<%dI" % 0x928, fe, i + 16 + 0x400 * 4)
    draws.append((addr & PHYS, issuer & PHYS, ordn, r2, r4))
print("front-end draws in the frame: %d (%d through an IB)" % (len(draws), sum(1 for d in draws if d[1])))

src = open(args.src, "rb").read()
calls = []
off = 0
while off + 52 <= len(src):
    hdr = struct.unpack_from("<13I", src, off)
    hook, tid, ci, co = hdr[0], hdr[1], hdr[2], hdr[3]
    a8 = hdr[4:12]
    n = hdr[12]
    off += 52
    samples = []
    for s in range(n):
        k, va = struct.unpack_from("<2I", src, off)
        data = src[off + 8: off + 8 + 256]
        samples.append((k, va, struct.unpack(">64I", data)))
        off += 8 + 256
    lo, hi = (ci & PHYS) + 4, (co & PHYS) + 4
    calls.append(dict(hook=hook, tid=tid, lo=lo, hi=hi, args=a8, samples=samples, ci=ci))
print("calls in the frame: %d (with a range: %d)" % (len(calls), sum(1 for c in calls if c["hi"] > c["lo"])))

devs = {}
if args.guest:
    g = open(args.guest, "rb").read()
    DEV = 0x5000
    o = 0
    while o + 24 + DEV <= len(g):
        hook, ci, co, c2i, c2o = struct.unpack_from("<5I", g, o)
        dev = struct.unpack_from(">%dI" % (DEV // 4), g, o + 20)
        (n,) = struct.unpack_from("<I", g, o + 20 + DEV)
        devs[(hook, ci)] = dev
        o += 24 + DEV + n
    print("device snapshots: %d" % len(devs))

ranged = sorted((c for c in calls if c["hi"] > c["lo"]), key=lambda c: c["lo"])


def containing(addr):
    out = [c for c in ranged if c["lo"] <= addr < c["hi"]]
    out.sort(key=lambda c: c["hi"] - c["lo"])   # innermost first
    return out


def regval(reg, r2, r4):
    if 0x2000 <= reg < 0x2400: return r2[reg - 0x2000]
    if 0x4000 <= reg < 0x4928: return r4[reg - 0x4000]
    return None


TRIVIAL = {0, 1, 0xFFFFFFFF}
votes = collections.defaultdict(collections.Counter)   # reg -> source -> draws explained
used = collections.Counter()
no_call = 0
for addr, issuer, ordn, r2, r4 in draws:
    key = issuer if issuer else addr
    cands = containing(key)
    if not cands:
        no_call += 1
        continue
    for reg in REGS:
        v = regval(reg, r2, r4)
        if v is None or v in TRIVIAL:
            continue
        used[reg] += 1
        found = set()
        for depth, c in enumerate(cands[:3]):
            tag = "hook%d@%d" % (c["hook"], depth)
            for k, a in enumerate(c["args"]):
                if a == v: found.add((tag, "r%d" % (3 + k)))
            for k, va, words in c["samples"]:
                for j, w in enumerate(words):
                    if w == v: found.add((tag, "[r%d+0x%X]" % (3 + k, j * 4)))
            dev = devs.get((c["hook"], c["ci"]))
            if dev is not None:
                for j, w in enumerate(dev):
                    if w == v: found.add((tag, "dev+0x%04X" % (j * 4)))

        for f in found:
            votes[reg][f] += 1

print("draws without a containing call: %d" % no_call)
print("reg     draws  best source (call@depth, where)              share   second")
for reg in REGS:
    if not used[reg]:
        continue
    top = votes[reg].most_common(2)
    if not top:
        print("0x%04X %6d  none" % (reg, used[reg]))
        continue
    (src1, n1) = top[0]
    second = ("%s %s %.0f%%" % (top[1][0][0], top[1][0][1], 100 * top[1][1] / used[reg])) if len(top) > 1 else ""
    print("0x%04X %6d  %-12s %-28s %5.1f%%   %s" % (reg, used[reg], src1[0], src1[1], 100 * n1 / used[reg], second))
