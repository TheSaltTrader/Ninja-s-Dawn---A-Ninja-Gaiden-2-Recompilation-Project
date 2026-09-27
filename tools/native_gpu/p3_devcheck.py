#!/usr/bin/env python3
"""P3 stage 2 check, offline: does the XDK device object at a call's exit hold the register file the bridge drew with?

p3_guest.bin has, per recorded call, the device object (0x5000 bytes) at the call's exit and the call's cursor range;
p3_bridge.bin has, per bridge draw whose packet lies inside a recorded range, the packet address and the plugin's
register file (0x2000-0x23FF, 0x4000-0x4927) at that draw. With the register -> device-word map from p3_flush2.py
(--map, registers explained >= 95%), every draw inside a call's range is checked: for each mapped register, device
word == bridge register? Per register: draws checked, matches, mismatches. For registers the bridge used (non-zero)
that the map does not cover, the device is searched for the bridge's value at each draw (candidate offsets voted),
so the derived registers get their candidates too.

Usage: p3_devcheck.py --guest p3_guest.bin --bridge p3_bridge.bin --map p3_mapN_flush2.json [--base_shift 1]"""
import argparse, collections, json, struct, bisect

ap = argparse.ArgumentParser()
ap.add_argument("--guest", required=True)
ap.add_argument("--bridge", required=True)
ap.add_argument("--map", required=True)
ap.add_argument("--base_shift", type=int, default=1)
ap.add_argument("--json", default=None)
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
    calls.append((hook, (ci & PHYS) + 4 * args.base_shift, (co & PHYS) + 4 * args.base_shift, dev))
    off += 24 + DEV + n
calls.sort(key=lambda c: c[1])
starts = [c[1] for c in calls]
print("calls: %d" % len(calls))

b = open(args.bridge, "rb").read()
REC3 = 8 + 0x400 * 4 + 0x928 * 4   # format v3: {addr, bridge frame} + regs
REC2 = 4 + 0x400 * 4 + 0x928 * 4
v3 = len(b) % REC3 == 0 and len(b) % REC2 != 0
REC = REC3 if v3 else REC2
draws = []
for i in range(0, len(b) - REC + 1, REC):
    if v3:
        addr, frame = struct.unpack_from("<2I", b, i)
        base = i + 8
    else:
        (addr,) = struct.unpack_from("<I", b, i)
        frame = 0
        base = i + 4
    r2 = struct.unpack_from("<%dI" % 0x400, b, base)
    r4 = struct.unpack_from("<%dI" % 0x928, b, base + 0x400 * 4)
    draws.append((addr & PHYS, r2, r4, frame))
print("bridge draws: %d (format %s)" % (len(draws), "v3 with frame stamps" if v3 else "v2"))
if v3:
    # keep the earliest bridge frame that has draws (the recording frame's execution) and the one after it
    frames = collections.Counter(d[3] for d in draws)
    first = min(frames)
    print("bridge frames seen: %s" % ", ".join("%d:%d" % kv for kv in sorted(frames.items())[:8]))
    draws = [d for d in draws if d[3] <= first + 1]
    print("kept frames %d..%d: %d draws" % (first, first + 1, len(draws)))

rows = json.load(open(args.map))
mapped = {r["reg"]: r["dev"] for r in rows if r["nontrivial"] and r["dev"] is not None and r["agree"] >= 0.95}
print("mapped registers: %d" % len(mapped))


def regval(reg, r2, r4):
    if 0x2000 <= reg < 0x2400: return r2[reg - 0x2000]
    if 0x4000 <= reg < 0x4928: return r4[reg - 0x4000]
    return None


match = collections.Counter()
mism = collections.Counter()
checked_draws = 0
no_call = 0
cand = collections.defaultdict(collections.Counter)   # unmapped register -> device offset votes
used = collections.Counter()
TRIVIAL = {0, 1, 0xFFFFFFFF}
for addr, r2, r4, _fr in draws:
    k = bisect.bisect_right(starts, addr) - 1
    # the innermost call containing the packet: walk back over calls that start before it and end after it
    hit = None
    j = k
    while j >= 0 and calls[j][1] <= addr:
        if calls[j][2] > addr:
            if hit is None or (calls[j][2] - calls[j][1]) < (hit[2] - hit[1]): hit = calls[j]
        j -= 1
        if k - j > 64: break
    if hit is None:
        no_call += 1
        continue
    checked_draws += 1
    dev = hit[3]
    index = None
    for reg, o in mapped.items():
        v = regval(reg, r2, r4)
        if v is None: continue
        if dev[o // 4] == v: match[reg] += 1
        else: mism[reg] += 1
    # derived registers: what the bridge used that the map does not cover
    for reg in list(range(0x2000, 0x2400)) + list(range(0x4800, 0x4928)):
        if reg in mapped: continue
        v = regval(reg, r2, r4)
        if v is None or v in TRIVIAL: continue
        used[reg] += 1
        if index is None:
            index = collections.defaultdict(list)
            for jj, w in enumerate(dev): index[w].append(jj * 4)
        for o in index.get(v, ()): cand[reg][o] += 1

print("draws inside a recorded call: %d (no containing call: %d)" % (checked_draws, no_call))
good = [r for r in mapped if match[r] and not mism[r]]
bad = [r for r in mapped if mism[r]]
print("mapped registers always equal at the draw: %d; sometimes different: %d; never exercised: %d" %
      (len(good), len(bad), len([r for r in mapped if not match[r] and not mism[r]])))
print("mapped registers that differ (reg: matches/mismatches):")
print("  " + " ".join("%04X:%d/%d" % (r, match[r], mism[r]) for r in sorted(bad)[:60]))
print("unmapped registers the bridge used (non-trivial) with their best device-word candidate (share of draws):")
outj = []
for reg in sorted(used):
    o, c = cand[reg].most_common(1)[0] if cand[reg] else (None, 0)
    share = c / used[reg]
    print("  %04X used %5d  best %s %5.1f%%" % (reg, used[reg], ("+0x%04X" % o) if o is not None else " none ", 100 * share))
    outj.append({"reg": reg, "used": used[reg], "dev": o, "share": share})
if args.json:
    json.dump({"match": {("%04X" % r): match[r] for r in mapped}, "mismatch": {("%04X" % r): mism[r] for r in mapped},
               "unmapped": outj}, open(args.json, "w"), indent=0)
