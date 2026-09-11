"""Find strings in the guest image, with their guest addresses and xrefs.

`strings` on the XEX is useless (it is encrypted and compressed) and gives no
addresses. This works on the decrypted image and reports guest addresses, so a
hit can be fed straight to tools/xrefs.py to find the code that uses it.

    python tools/gstrings.py --pattern "\\.ng2"        # regex over the image
    python tools/gstrings.py --min 6 --section .data   # every string
    python tools/gstrings.py --pattern "game:" --xref  # plus who references it
"""

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_image import XexImage  # noqa: E402
import xrefs  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRINTABLE = re.compile(rb"[\x20-\x7e]")


def expand(data, pos, limit=120):
    """Widen a hit to the surrounding printable run (a C string)."""
    start = pos
    while start > 0 and 0x20 <= data[start - 1] < 0x7F and pos - start < limit:
        start -= 1
    end = pos
    while end < len(data) and 0x20 <= data[end] < 0x7F and end - pos < limit:
        end += 1
    return start, data[start:end].decode("latin1", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xex", default=os.path.join(ROOT, "assets", "default.xex"))
    ap.add_argument("--pattern", help="regex (bytes, case-insensitive)")
    ap.add_argument("--min", type=int, default=6, help="min length when listing all")
    ap.add_argument("--section", help="restrict to one section name")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--xref", action="store_true",
                    help="also report code referencing each hit")
    args = ap.parse_args()

    img = XexImage.load(args.xex)
    data = img.data

    ranges = []
    for name, va, size in img.sections:
        if args.section and name != args.section:
            continue
        ranges.append((name, img.offset(va), min(img.offset(va) + size, len(data))))

    if args.pattern:
        rx = re.compile(args.pattern.encode(), re.I)
    else:
        rx = re.compile(rb"[\x20-\x7e]{%d,}" % args.min)

    shown = 0
    for name, lo, hi in ranges:
        for m in rx.finditer(data, lo, hi):
            start, text = expand(data, m.start())
            va = img.image_base + start
            print(f"0x{va:08X} [{name}]  {text!r}")
            shown += 1
            if args.xref:
                hits = xrefs.scan(img, va, va + 1, True, True)
                if hits:
                    for addr, _w, mnem, is_store, _t in hits[:6]:
                        kind = "STORE" if is_store else "load "
                        print(f"      {kind} 0x{addr:08X} {mnem}")
                else:
                    print("      (no direct lis/addi reference - reached via a table?)")
            if shown >= args.limit:
                print(f"... stopping at {args.limit} (raise --limit)")
                return 0
    if not shown:
        print("no matches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
