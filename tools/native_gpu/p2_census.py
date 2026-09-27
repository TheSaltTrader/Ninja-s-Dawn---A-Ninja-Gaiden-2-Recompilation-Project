#!/usr/bin/env python3
"""P2 ATTRIBUTION CENSUS - the join (full-native plan, 2026-09-27). Format: docs/native_gpu/P2_CENSUS_FORMAT.md.

Inputs, both from one run with NG2_P2=<first frame>:<frames>:
  p2_guest.bin    the exe's call ranges: per hooked Direct3D entry point call, the device's ring write cursor at
                  entry and exit (two fields: +0x30 and +0x3484), frame by swap ENTRY (ng2_p2_census.cpp)
  p2_packets.bin  the plugin's packets: guest address, header word, bin select, frame by swap PACKET
  the run's log   for the hook labels? no - those come from src/native_gpu_trace.cpp (the hook table)
A packet is ATTRIBUTED to the call whose [cursor at entry, cursor at exit) range holds its address in the same
frame (or the neighbouring frame, since the two frame counters differ by the swap packet itself and the guest
writes ahead of the parser). Addresses are compared as PHYSICAL (the cursor is the CPU's 0xA/0xC/0xE0000000 view
of physical memory; the plugin records physical addresses): both & 0x1FFFFFFF.

Usage: p2_census.py --guest p2_guest.bin --packets p2_packets.bin [--trace src/native_gpu_trace.cpp]
                    [--json out.json] [--frames N] [--top 12]"""
import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--guest", required=True)
ap.add_argument("--packets", required=True)
ap.add_argument("--trace", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src",
                                                  "native_gpu_trace.cpp"))
ap.add_argument("--json", default=None)
ap.add_argument("--frames", type=int, default=0, help="report at most this many frames (0 = all)")
ap.add_argument("--top", type=int, default=12)
ap.add_argument("--field", choices=("auto", "cur", "cur2"), default="auto",
                help="which cursor field's ranges attribute (auto: the one that attributes more)")
ap.add_argument("--window", type=int, default=1,
                help="also look this many frames BACK for the producing call (the parser runs behind the guest)")
ap.add_argument("--ring", default="",
                help="the primary ring as BASE:SIZE (hex, from the plugin's 'InitializeRingBuffer ptr ... -> size' line): "
                     "packets in it with no producer are the library's kick-off writer, which bypasses the device cursor")
args = ap.parse_args()
ring_base, ring_size = 0, 0
if args.ring:
    rb, rs = args.ring.split(":")
    ring_base, ring_size = int(rb, 16) & 0x1FFFFFFF, int(rs, 16)
RING = 400000          # producer key: "PRIMARY RING (library kick-off writer)"

PHYS = 0x1FFFFFFF
OPCODE_NAMES = {0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x3B: "INVALIDATE_STATE",
                0x3C: "WAIT_REG_MEM", 0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x3F: "INDIRECT_BUFFER",
                0x2D: "LOAD_ALU_CONSTANT", 0x21: "REG_RMW", 0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI",
                0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI", 0x48: "ME_INIT", 0x30: "SET_CONSTANT",
                0x37: "XE_SWAP", 0x10: "NOP"}
DRAW_OPCODES = {0x22, 0x36}


def labels_from_trace(path):
    src = open(path, encoding="utf-8").read()
    out = []
    for m in re.finditer(r'\{0x([0-9A-F]{8}), "([^"]*)"', src):
        out.append(("sub_" + m.group(1), m.group(2)))
    return out


hooks = labels_from_trace(args.trace)

# ---- guest ranges -----------------------------------------------------------------------------------------------
g = open(args.guest, "rb").read()
magic, ver, rec_size, g_start, g_count, cur_off, cur2_off, n = struct.unpack("<8I", g[:32])
assert magic == 0x3250474E and rec_size == 28, (hex(magic), rec_size)
ranges = collections.defaultdict(list)   # frame -> [(lo, hi, hook, field)] physical, half-open
by_field = {"cur": 0, "cur2": 0}
calls_total = 0
for i in range(n):
    frame, tid, hook, flags, ci, co, c2i, c2o = struct.unpack_from("<IIHHIIII", g, 32 + i * rec_size)
    calls_total += 1
    for field, a, b in (("cur", ci, co), ("cur2", c2i, c2o)):
        if not a or not b or a == b:
            continue
        lo, hi = a & PHYS, b & PHYS
        if hi < lo:   # the buffer wrapped inside the call: two pieces (the wrap point is unknown; keep both open ends)
            ranges[frame].append((lo, 1 << 29, hook, field))
            ranges[frame].append((0, hi, hook, field))
        else:
            ranges[frame].append((lo, hi, hook, field))
        by_field[field] += 1
for f in ranges:
    ranges[f].sort()
INLINE = 100000        # producer key offset for "engine code writing right after this hooked call returned"
INLINE_BEFORE = 200000 # ... "engine code writing right before this hooked call" (register set-up before a draw)
IBVIA = 300000         # ... "executed from an indirect buffer issued by this hooked call" (library templates)
INLINE_GAP = 256       # bytes past a range's end (or ahead of a start) that still count as that call's inline
range_ends = {}        # frame -> ([end addresses sorted], [hook per end])
range_starts = {}      # frame -> ([start addresses sorted], [hook per start])
for f, rs in ranges.items():
    ends = sorted((hi, hook) for lo, hi, hook, fld in rs if fld == "cur")
    range_ends[f] = ([e for e, h in ends], [h for e, h in ends])
    starts = sorted((lo, hook) for lo, hi, hook, fld in rs if fld == "cur")
    range_starts[f] = ([s for s, h in starts], [h for s, h in starts])
# format version 2: the LAST WRITER of every 64-byte block any hooked call wrote since the first frame
blocks = {}
pos = 32 + n * rec_size
if ver >= 2 and pos + 8 <= len(g):
    bmagic, bcount = struct.unpack_from("<2I", g, pos)
    if bmagic == 0x314B4C42:
        for i in range(bcount):
            blk, bframe, bhook = struct.unpack_from("<3I", g, pos + 8 + i * 12)
            blocks[blk] = (bframe, bhook)
via_blocks = collections.Counter()   # hook -> packets attributed through the map (written before the window)

# ---- packets ----------------------------------------------------------------------------------------------------
p = open(args.packets, "rb").read()
magic, ver, rec_size, p_start = struct.unpack("<4I", p[:16])
assert magic == 0x3250474E and rec_size == 16, (hex(magic), rec_size)
packets = collections.defaultdict(list)   # frame -> [(addr, header, bin)]
for i in range(16, len(p), 16):
    frame, addr, header, binsel = struct.unpack_from("<4I", p, i)
    packets[frame].append((addr & PHYS, header, binsel))


def find(frame, addr, field):
    """The hook whose range holds addr in frame, or the neighbouring frames; None if none."""
    for f in [frame] + [frame - k for k in range(1, args.window + 1)] + [frame + 1]:
        rs = ranges.get(f)
        if not rs:
            continue
        # ranges sorted by lo: scan candidates with lo <= addr (bisect on lo)
        los = [r[0] for r in rs]
        k = bisect.bisect_right(los, addr)
        for j in range(k - 1, max(-1, k - 64), -1):
            lo, hi, hook, fld = rs[j]
            if fld != field:
                continue
            if lo <= addr < hi:
                return hook
    # ENGINE INLINE: written by the caller right after a hooked call returned (NG2 Chapter 1: 32 DRAW_INDX per
    # frame sit exactly 56 bytes past the end of a sub_8373BD50 range - the state flush before a draw - with the
    # draw packet built by engine code). Named after that call, in its own bucket (hook + INLINE). Checked before
    # the block map: the map's 64-byte blocks would otherwise absorb the inline tail into the call itself.
    for f in (frame, frame - 1, frame + 1):
        ends = range_ends.get(f)
        if not ends:
            continue
        k = bisect.bisect_right(ends[0], addr)
        if k and addr - ends[0][k - 1] <= INLINE_GAP:
            return INLINE + ends[1][k - 1]
    # No call in the window wrote it: the last hooked writer of its block, whenever that was (a command buffer the
    # game recorded once and replays every frame).
    w = blocks.get(addr >> 6)
    if w is not None:
        via_blocks[w[1]] += 1
        return w[1]
    return None


def census(field):
    per_frame = []
    tot = collections.Counter()
    tot_draws = collections.Counter()
    unattr_ops = collections.Counter()
    unattr_draw_ops = collections.Counter()
    frames = sorted(packets)
    if args.frames:
        frames = frames[:args.frames]
    for f in frames:
        c = collections.Counter()
        d = collections.Counter()
        t = collections.Counter()
        bins = collections.Counter()
        ib_ranges = []        # the most recent indirect buffers executed: (lo, hi, key of the issuing call)
        last_ib_key = -1      # the producer of the last INDIRECT_BUFFER packet seen
        for addr, header, binsel in packets[f]:
            if header == 0x7FFFFFFF:   # the plugin's marker: an indirect buffer [addr, addr + count*4) starts now
                ib_ranges.append((addr, addr + binsel * 4, last_ib_key))
                if len(ib_ranges) > 8:
                    ib_ranges.pop(0)
                continue
            ptype = header >> 30
            t[ptype] += 1
            is_draw = ptype == 3 and ((header >> 8) & 0x7F) in DRAW_OPCODES
            hook = find(f, addr, field)
            if hook is None:
                # Executed from an indirect buffer no hooked call wrote (the library's persistent templates, or a
                # buffer built by engine code): credited to the call that issued the INDIRECT_BUFFER packet.
                for lo, hi, ib_key in reversed(ib_ranges):
                    if lo <= addr < hi:
                        hook = (IBVIA + ib_key) if ib_key != -1 else None
                        break
            if hook is None:
                # Engine inline BEFORE a hooked call: the packet sits just ahead of a range start (register set-up
                # written by the caller before it calls the library's draw).
                starts = range_starts.get(f)
                if starts:
                    k = bisect.bisect_left(starts[0], addr)
                    if k < len(starts[0]) and starts[0][k] - addr <= INLINE_GAP:
                        hook = INLINE_BEFORE + starts[1][k]
            if hook is None and ring_size and ring_base <= addr < ring_base + ring_size:
                # The primary ring itself: the library's kick-off writes the INDIRECT_BUFFER calls and the swap
                # there through the ring's own write pointer, not the device cursor (NG2: 6 IB packets per frame).
                hook = RING
            key = hook if hook is not None else -1
            if ptype == 3 and ((header >> 8) & 0x7F) == 0x3F:
                last_ib_key = key if (key != -1 and key < INLINE) else -1
            c[key] += 1
            if is_draw:
                d[key] += 1
                bins[binsel] += 1
            if hook is None:
                op = ((header >> 8) & 0x7F) if ptype == 3 else (-1 - ptype)
                unattr_ops[op] += 1
                if is_draw:
                    unattr_draw_ops[op] += 1
        per_frame.append((f, c, d, t, bins))
        tot.update(c)
        tot_draws.update(d)
    return per_frame, tot, tot_draws, unattr_ops, unattr_draw_ops


results = {}
for field in (("cur", "cur2") if args.field == "auto" else (args.field,)):
    results[field] = census(field)
if args.field == "auto":
    best = max(results, key=lambda k: sum(v for h, v in results[k][1].items() if h != -1))
else:
    best = args.field
per_frame, tot, tot_draws, unattr_ops, unattr_draw_ops = results[best]


def name(h):
    if h == -1:
        return "UNATTRIBUTED"
    if h == RING:
        return "PRIMARY RING (library kick-off writer)"
    if h >= IBVIA:
        return "via INDIRECT_BUFFER issued by %s" % hooks[h - IBVIA][0]
    if h >= INLINE_BEFORE:
        return "ENGINE INLINE before %s" % hooks[h - INLINE_BEFORE][0]
    if h >= INLINE:
        return "ENGINE INLINE after %s" % hooks[h - INLINE][0]
    fn, label = hooks[h]
    return "%s %s" % (fn, label.split()[0] if label.startswith("Set") else label[:28])


npk = sum(tot.values())
ndr = sum(tot_draws.values())
un = tot.get(-1, 0)
und = tot_draws.get(-1, 0)
print("P2 CENSUS: %d frames (%d..%d), %d call ranges (%s field %s: %d usable, %s: %d), %d packets, %d draws" %
      (len(per_frame), per_frame[0][0] if per_frame else 0, per_frame[-1][0] if per_frame else 0, calls_total,
       "attributing by", best, by_field[best], "cur2" if best == "cur" else "cur",
       by_field["cur2" if best == "cur" else "cur"], npk, ndr))
print("METRIC 1  unattributed packets: %d of %d (%.1f%%)   unattributed draws: %d of %d (%.1f%%)" %
      (un, npk, 100.0 * un / max(1, npk), und, ndr, 100.0 * und / max(1, ndr)))
print("METRIC 2  draws not issued by a native front end: n/a (no front end yet; the bridge draws them all)")
if blocks:
    vb = sum(via_blocks.values())
    print("          attributed through the last-writer map (buffers recorded before the window and replayed): %d "
          "packets, top: %s" % (vb, ", ".join("%s=%d" % (hooks[h][0], v) for h, v in via_blocks.most_common(4))))
print()
print("by producer (whole run): packets  draws  producer")
for h, v in tot.most_common(args.top + 1):
    print("  %8d  %6d  %s" % (v, tot_draws.get(h, 0), name(h)))
print()
print("unattributed packets by type/opcode:")
for op, v in unattr_ops.most_common(12):
    nm = ("type-%d" % (-1 - op)) if op < 0 else ("%s(0x%02X)" % (OPCODE_NAMES.get(op, "op"), op))
    print("  %8d  %s%s" % (v, nm, "  <- DRAW %d" % unattr_draw_ops.get(op, 0) if op in DRAW_OPCODES else ""))
print()
print("per frame: frame  packets  draws  unattributed(packets/draws)  type0/2/3  bins(draws by bin select)")
for f, c, d, t, bins in per_frame[:60]:
    binstr = " ".join("%x:%d" % (b, v) for b, v in bins.most_common(5))
    print("  %6d  %7d  %5d  %6d/%-5d  %d/%d/%d  %s" % (f, sum(c.values()), sum(d.values()), c.get(-1, 0),
                                                       d.get(-1, 0), t.get(0, 0), t.get(2, 0), t.get(3, 0), binstr))
if args.json:
    out = {"frames": [{"frame": f, "packets": sum(c.values()), "draws": sum(d.values()),
                       "unattributed_packets": c.get(-1, 0), "unattributed_draws": d.get(-1, 0),
                       "by_producer": {name(h): v for h, v in c.items()},
                       "draws_by_bin_select": {hex(b): v for b, v in bins.items()}}
                      for f, c, d, t, bins in per_frame],
           "totals": {"packets": npk, "draws": ndr, "unattributed_packets": un, "unattributed_draws": und,
                      "by_producer": {name(h): v for h, v in tot.items()}, "field": best}}
    json.dump(out, open(args.json, "w"), indent=1)
    print("json ->", args.json)
