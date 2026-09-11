"""List the kernel imports a XEX uses, and where the guest calls them.

Every import is reached through a small thunk in the image. Knowing the thunk
address for an ordinal lets you find call sites with a `bl` scan, which is how
you work out what the guest expected an unimplemented kernel call to do.

    python tools/xex_imports.py                       # summary per library
    python tools/xex_imports.py --library xam         # every xam import
    python tools/xex_imports.py --ordinal 58046       # thunk + call sites
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_image import XexImage  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR_IMPORT_LIBRARIES = 0x000103FF


def parse_imports(xex_path):
    """-> {library_name: {ordinal: [record_addresses]}}"""
    d = open(xex_path, "rb").read()
    _flags, _pe, _res, _sec, opt_count = struct.unpack_from(">IIIII", d, 4)
    hdrs = {}
    off = 0x18
    for _ in range(opt_count):
        k, v = struct.unpack_from(">II", d, off)
        off += 8
        hdrs[k] = v
    base = hdrs[HDR_IMPORT_LIBRARIES]

    _size, string_table_size, library_count = struct.unpack_from(">III", d, base)
    names = [n.decode("latin1") for n in
             d[base + 12:base + 12 + string_table_size].split(b"\0") if n]

    libs = {}
    p = base + 12 + string_table_size
    for _ in range(library_count):
        block_size, = struct.unpack_from(">I", d, p)
        # 4 size + 20 digest + 4 id + 4 version + 4 version_min + 2 name_index
        # + 2 count, then count x uint32 record addresses.
        name_index, count = struct.unpack_from(">HH", d, p + 36)
        records = struct.unpack_from(f">{count}I", d, p + 40)
        name = names[name_index] if name_index < len(names) else f"?{name_index}"
        entries = libs.setdefault(name, {})
        for addr in records:
            entries.setdefault(addr, None)
        p += block_size
    return libs, names


def classify(img, libs):
    """Split each library's records into ordinal -> (thunk_addr, kind).

    A record word of (1 << 24) | ordinal marks a function thunk; (0 << 24)
    marks a variable slot patched at load time.
    """
    out = {}
    for lib, records in libs.items():
        table = {}
        for addr in sorted(records):
            if not img.contains(addr):
                continue
            value = img.word(addr)
            kind = value >> 24
            ordinal = value & 0xFFFF
            if kind == 1:
                # The thunk body sits just before the record in the image.
                table[ordinal] = ("thunk", addr)
            elif kind == 0:
                table.setdefault(ordinal, ("variable", addr))
        out[lib] = table
    return out


def find_calls(img, target):
    """Addresses of `bl target` in executable sections."""
    hits = []
    for name, va, size in img.sections:
        if name != ".text" and not name.startswith(".embsec"):
            continue
        for addr in range(va, va + (size & ~3), 4):
            w = struct.unpack_from(">I", img.data, img.offset(addr))[0]
            if (w >> 26) & 0x3F != 18 or not (w & 1) or (w >> 1) & 1:
                continue  # want bl, relative, not absolute
            li = (w >> 2) & 0xFFFFFF
            if li & 0x800000:
                li -= 0x1000000
            if addr + (li << 2) == target:
                hits.append(addr)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xex", default=os.path.join(ROOT, "assets", "default.xex"))
    ap.add_argument("--library")
    ap.add_argument("--ordinal", type=lambda s: int(s, 0))
    args = ap.parse_args()

    img = XexImage.load(args.xex)
    libs, _ = parse_imports(args.xex)
    tables = classify(img, libs)

    if args.ordinal is None:
        for lib, table in tables.items():
            funcs = sum(1 for k in table.values() if k[0] == "thunk")
            print(f"{lib}: {len(table)} imports ({funcs} functions)")
            if args.library and args.library in lib:
                for ordinal in sorted(table):
                    kind, addr = table[ordinal]
                    print(f"  ordinal {ordinal:>6} ({ordinal:#06x})  {kind:<8} @ 0x{addr:08X}")
        return 0

    for lib, table in tables.items():
        if args.ordinal not in table:
            continue
        kind, addr = table[args.ordinal]
        print(f"{lib} ordinal {args.ordinal}: {kind} record at 0x{addr:08X}")
        # The dispatch thunk is the code the guest actually calls; look for
        # a bl to each address in the neighbourhood of the record.
        for probe in (addr, addr - 0x10, addr - 0x14, addr - 0x18):
            calls = find_calls(img, probe)
            if calls:
                print(f"  callers of 0x{probe:08X}: {len(calls)}")
                for c in calls[:20]:
                    print(f"    0x{c:08X}")
                break
        else:
            print("  no direct bl call sites found (called through a pointer?)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
