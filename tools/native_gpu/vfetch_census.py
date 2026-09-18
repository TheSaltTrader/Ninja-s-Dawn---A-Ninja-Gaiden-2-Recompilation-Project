#!/usr/bin/env python3
"""Count vfetch_full vs vfetch_mini across the container corpus, offline.

The runtime census reads const_index and stride from EVERY fetch instruction,
but ucode.h says both are "Applicable only to vfetch_full (the address from
vfetch_full is reused in vfetch_mini)". So every mini has been naming a fetch
slot at random and pairing a random stride with it.

This says how big that is WITHOUT a game run - the containers are on disk, the
control flow is decodable, and the answer bounds how much of the runtime's
"24% of draws want more bytes than the buffer declares" this can explain.

    vertex fetch instruction, 3 dwords:
      dword 0  opcode:5 src_reg:6 src_reg_am:1 dst_reg:6 dst_reg_am:1
               must_be_one:1 const_index:5 const_index_sel:2
      dword 1  dst_swiz:12 ... exp_adjust:6 IS_MINI_FETCH:1(bit30) is_predicated:1
      dword 2  stride:8 offset:23 pred_condition:1

A mini is not an error and not rare by design: it is how the hardware fetches
several attributes from one already-computed address. The defect is reading its
fields, not its existence.
"""
import os
import sys
import struct
sys_path_hack = None
from vfetch_scan import scan


def be32(b, i):
    return struct.unpack_from(">I", b, i * 4)[0]


def code_region(path):
    """Return (code_bytes, is_pixel) or None.

    Lifted verbatim from make_shader_manifest.py rather than re-derived. My
    first attempt read the physical offset straight from dword 2 and reported
    625 of 625 unparseable - the offset is reached through a POINTER at 0x18,
    and the code's length is stored beside it. Copying the parser that already
    reads this corpus correctly is the whole fix.
    """
    d = open(path, "rb").read()
    if len(d) < 0x24:
        return None
    flags, vsize, psize = struct.unpack_from(">3I", d, 0)
    if (flags & 0xFFFFFF00) != 0x102A1100:
        return None
    shoff = struct.unpack_from(">I", d, 0x18)[0]
    if shoff + 8 > len(d):
        return None
    po, size = struct.unpack_from(">2I", d, shoff)
    at = vsize + po
    if at + size > len(d) or size == 0:
        return None
    return d[at:at + size], (flags & 1) == 0


def walk(code):
    """Yield (instruction address, is_mini) for every vertex fetch.

    DELEGATES TO vfetch_scan.scan, which is the scanner the synthesis path has
    used all along. My own first version omitted the END_OPS break that scan
    has, so it walked past the end of the control flow and decoded instruction
    data as further CF pairs. That invented fetches in slots 0-9 and 48-53 and
    inflated the mini count.

    The tell was ground truth, not review: the generated HLSL contains only
    NGPU_STREAM(0) and NGPU_STREAM(1), so the only fetch slots in play are 94
    and 95. A census claiming eighteen distinct slots was describing its own
    walker.
    """
    for at in scan(code):
        w1 = be32(code, at * 3 + 1)
        yield at * 3, bool((w1 >> 30) & 1)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else r"D:\ng2_frameinterp\shaders\xvu"
    full = mini = 0
    files = progs_with_mini = progs_with_fetch = 0
    unparseable = 0
    for name in sorted(os.listdir(d)):
        if not name.endswith(".xvu"):
            continue
        files += 1
        r = code_region(os.path.join(d, name))
        if r is None:
            unparseable += 1
            continue
        code, is_pixel = r
        if is_pixel:
            continue
        f = m = 0
        for _, is_mini in walk(code):
            if is_mini:
                m += 1
            else:
                f += 1
        full += f
        mini += m
        if f or m:
            progs_with_fetch += 1
        if m:
            progs_with_mini += 1

    total = full + mini
    print(f"containers scanned      : {files}  (unparseable {unparseable})")
    print(f"vertex programs w/ fetch: {progs_with_fetch}")
    print(f"  of those, with a MINI : {progs_with_mini}")
    print(f"fetch instructions      : {total}")
    print(f"  vfetch_full           : {full}")
    print(f"  vfetch_MINI           : {mini}"
          + (f"   ({100.0 * mini / total:.1f}%)" if total else ""))
    if total and mini:
        print()
        print("Every mini was previously read for its const_index and stride, which")
        print("belong to the preceding full. Each one named a fetch slot at random.")
    elif total:
        print()
        print("No minis in this corpus - so the mini defect cannot explain the")
        print("runtime's 24%, and the cause is elsewhere.")


if __name__ == "__main__":
    sys.exit(main() or 0)
