#!/usr/bin/env python3
"""List the vertex-fetch instructions of dumped Fable II shader containers.

    fetch_rows.py <file.xvu|dir> [more files]   (dirs: every *_v.var.xvu)

Every 4-byte-aligned 12-byte row whose first dword has opcode 0 (vertex
fetch) with the must-be-one bit set and a plausible format is printed as
  const k(sel) -> stream  fmt  stride(dw)  offset(dw)  mini  dst
so the layouts the runtime scanner (PositionFromShader) does not resolve can
be compared with the ones it does.
"""
import os
import struct
import sys

FMT = {6: "8_8_8_8", 7: "2_10_10_10", 16: "10_11_11", 17: "11_11_10", 25: "16_16", 26: "16_16_16_16",
       31: "16_16_FLOAT", 32: "16_16_16_16_FLOAT", 33: "32", 34: "32_32", 35: "32_32_32_32",
       36: "32_FLOAT", 37: "32_32_FLOAT", 38: "32_32_32_32_FLOAT", 57: "32_32_32_FLOAT"}


def rows(path):
    data = open(path, "rb").read()
    out = []
    for off in range(0, len(data) - 12, 4):
        d0, d1, d2 = struct.unpack_from(">III", data, off)
        if d0 & 0x1F or not (d0 >> 19) & 1:
            continue
        fmt = (d1 >> 16) & 0x3F
        if fmt not in FMT:
            continue
        stride = d2 & 0xFF
        offset = (d2 >> 8) & 0x7FFFFF
        mini = (d1 >> 30) & 1
        ci = (d0 >> 20) & 0x1F
        sel = (d0 >> 25) & 3
        dst = (d0 >> 12) & 0x3F
        k = ci * 3 + sel
        if stride > 64 or offset > 64:
            continue
        out.append((off, ci, sel, 95 - k, fmt, stride, offset, mini, dst))
    return out


def main():
    files = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            files += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith("_v.var.xvu"))
        else:
            files.append(a)
    for f in files:
        print(os.path.basename(f))
        for off, ci, sel, stream, fmt, stride, offset, mini, dst in rows(f):
            print(f"  @{off:5d} const {ci:2d}({sel}) stream {stream:3d} fmt {fmt:2d} {FMT[fmt]:18s} stride {stride:2d} offset {offset:2d}"
                  f"{' mini' if mini else '     '} dst r{dst}")


if __name__ == "__main__":
    main()
