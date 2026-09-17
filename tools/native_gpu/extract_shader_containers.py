#!/usr/bin/env python3
"""Extract NG2's Xenos shader containers from the guest-memory dump.

XenosRecomp's own scanner finds containers in a file, but the translate_all.sh
pipeline wants one container per .xvu file so it can name, fix and compile each
one separately. So do the same scan and split.

A container is identified exactly as the recompiler does:
    (flags & 0xFFFFFF00) == 0x102A1100, field1C == 0, field20 == 0,
    size = virtualSize + physicalSize
and the TYPE is the low bit, inverted: isPixelShader = (flags & 1) == 0.
Names carry the guest address so a translated shader can be tied back to the
IM_LOAD that asked for it, and pixel shaders take the _p suffix translate_all.sh
keys its dxc profile off.

Usage: extract_ng2_shaders.py <dump.bin> <out dir> [base_guest_addr_hex]
"""
import os
import struct
import sys

dump_path, out_dir = sys.argv[1], sys.argv[2]
base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80000000

os.makedirs(out_dir, exist_ok=True)
data = open(dump_path, "rb").read()
print("dump %s: %d bytes, base guest 0x%08X" % (dump_path, len(data), base))

found = vs = ps = 0
i = 0
seen = set()
while True:
    i = data.find(b"\x10\x2a\x11", i)
    if i < 0:
        break
    if i % 4:
        i += 1
        continue
    if i + 36 > len(data):
        break
    flags, vsz, psz, f0C, ctab, dtab, soff, f1C, f20 = struct.unpack_from(">9I", data, i)
    if (flags & 0xFFFFFF00) != 0x102A1100 or f1C or f20:
        i += 1
        continue
    size = vsz + psz
    if size == 0 or i + size > len(data):
        i += 1
        continue
    blob = data[i:i + size]
    # Identical containers appear at several addresses (the same shader loaded
    # more than once); translating duplicates wastes time and muddles the counts.
    h = hash(blob)
    if h in seen:
        i += size
        continue
    seen.add(h)
    is_pixel = (flags & 1) == 0
    name = "ng2_%08X%s.xvu" % (base + i, "_p" if is_pixel else "")
    with open(os.path.join(out_dir, name), "wb") as f:
        f.write(blob)
    found += 1
    ps += is_pixel
    vs += not is_pixel
    i += size

print("extracted %d unique containers: %d vertex, %d pixel -> %s" % (found, vs, ps, out_dir))
