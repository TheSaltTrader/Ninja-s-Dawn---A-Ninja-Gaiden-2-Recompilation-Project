"""Extract files from an Xbox 360 GDF (XGD1/2/3) disc image.

The recompiler only needs default.xex, but the finished build needs every
asset file next to it, so this handles both:

    python extract_disc.py <iso> <outdir>                 # everything
    python extract_disc.py <iso> <outdir> --only default.xex
    python extract_disc.py <iso> <outdir> --list          # no writes

Written for this project because the usual GUI tools (xbox-image-browser,
wxPirs) are Windows-GUI-only and can't be driven from a script.
"""

import argparse
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"

# Partition base offsets by disc generation. XGD2 is the 2008-era format
# Ninja Gaiden II shipped on; the others are here so the script stays useful.
KNOWN_BASES = {
    0x00000000: "raw / XDKi",
    0x02080000: "XGD3",
    0x0FD90000: "XGD2",
    0x18300000: "XGD1",
}


def find_partition_base(f):
    """Return (base_offset, label). The volume descriptor lives at sector 32
    of the game partition, so probe base+0x10000 for the magic."""
    for base, label in KNOWN_BASES.items():
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base, label
    raise SystemExit("No GDF volume descriptor found - not an Xbox 360 disc image?")


def read_volume(f, base):
    f.seek(base + 32 * SECTOR)
    vd = f.read(SECTOR)
    root_sector, root_size = struct.unpack_from("<II", vd, 0x14)
    return root_sector, root_size


class Entry:
    __slots__ = ("name", "sector", "size", "attr", "path")

    def __init__(self, name, sector, size, attr, path):
        self.name = name
        self.sector = sector
        self.size = size
        self.attr = attr
        self.path = path

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)


def walk_directory(f, base, sector, size, prefix=""):
    """Walk one GDF directory table.

    Entries form a binary tree inside a flat table; left/right are 16-bit
    offsets in 4-byte units, and 0/0xFFFF terminate a branch. Recursion is
    breadth-first over subdirectories so deep trees don't blow the stack.
    """
    if size == 0:
        return []
    f.seek(base + sector * SECTOR)
    table = f.read(size)

    entries = []
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off * 4 + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr = struct.unpack_from("<HHIIB", table, off * 4)
        name_len = table[off * 4 + 13]
        raw = table[off * 4 + 14 : off * 4 + 14 + name_len]
        if not raw:
            continue
        name = raw.decode("latin1")
        entries.append(Entry(name, start, length, attr, prefix + name))
        for child in (left, right):
            if child and child != 0xFFFF:
                stack.append(child)

    # Recurse into subdirectories after the current table is fully parsed.
    out = []
    for e in entries:
        out.append(e)
        if e.is_dir:
            out.extend(walk_directory(f, base, e.sector, e.size, e.path + "/"))
    return out


def extract(f, base, entry, outdir):
    dest = os.path.join(outdir, entry.path.replace("/", os.sep))
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    f.seek(base + entry.sector * SECTOR)
    remaining = entry.size
    with open(dest, "wb") as out:
        while remaining:
            chunk = f.read(min(1 << 24, remaining))
            if not chunk:
                raise SystemExit(f"Unexpected EOF reading {entry.path}")
            out.write(chunk)
            remaining -= len(chunk)
    return dest


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:,.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("iso")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--only", action="append", default=None,
                    help="extract just this filename (repeatable, case-insensitive)")
    ap.add_argument("--list", action="store_true", help="list contents and exit")
    args = ap.parse_args()

    if not args.list and not args.outdir:
        ap.error("outdir is required unless --list is given")

    with open(args.iso, "rb") as f:
        base, label = find_partition_base(f)
        root_sector, root_size = read_volume(f, base)
        print(f"{label} image, partition base 0x{base:X}, root at sector {root_sector}")

        entries = walk_directory(f, base, root_sector, root_size)
        files = [e for e in entries if not e.is_dir]
        total = sum(e.size for e in files)
        print(f"{len(files)} files, {human(total)} total")

        if args.list:
            for e in sorted(files, key=lambda x: -x.size):
                print(f"  {e.path:<40} {e.size:>14,}")
            return

        wanted = None
        if args.only:
            wanted = {n.lower() for n in args.only}
            files = [e for e in files if e.name.lower() in wanted]
            missing = wanted - {e.name.lower() for e in files}
            if missing:
                raise SystemExit(f"Not on this disc: {', '.join(sorted(missing))}")

        os.makedirs(args.outdir, exist_ok=True)
        done = 0
        for i, e in enumerate(files, 1):
            extract(f, base, e, args.outdir)
            done += e.size
            pct = done / total * 100 if wanted is None else 100.0
            print(f"  [{i}/{len(files)}] {e.path} ({human(e.size)}) {pct:.0f}%", flush=True)
        print(f"Extracted {len(files)} files to {args.outdir}")


if __name__ == "__main__":
    sys.exit(main())
