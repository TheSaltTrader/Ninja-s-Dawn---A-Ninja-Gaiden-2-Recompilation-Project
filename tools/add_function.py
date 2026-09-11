"""Register a guest function the analyzer missed.

The analyzer takes function bounds from .pdata and fills the gaps. MSVC emits
no .pdata for small helpers with no prologue - `this`-adjusting thunks,
interlocked helpers, one-line virtual overrides - so when one sits in the
padding after a real function it gets absorbed into that function's body. A
call reaching it then dies at runtime with:

    [FATAL] Call to invalid or unregistered function at guest address 0x...

These are only discoverable by running, because the callers reach them through
function-pointer tables the static pass cannot follow.

    python tools/add_function.py 0x83846050 [0x... ...]

Appends an entry (with the disassembled body as a comment) to
config/functions.toml, skipping addresses already present.
"""

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_image import XexImage, pdata_functions  # noqa: E402

BLR = 0x4E800020
BCTR = 0x4E800420
MAX_SPAN = 0x4000


def _branch_target(addr, w):
    """Target of a b/bc instruction, or None if it isn't one."""
    op = (w >> 26) & 0x3F
    if op == 18:  # b / bl / ba / bla
        li = (w >> 2) & 0xFFFFFF
        if li & 0x800000:
            li -= 0x1000000
        li <<= 2
        return li if (w >> 1) & 1 else addr + li
    if op == 16:  # bc
        bd = (w >> 2) & 0x3FFF
        if bd & 0x2000:
            bd -= 0x4000
        bd <<= 2
        return bd if (w >> 1) & 1 else addr + bd
    return None


def function_extent(img, start):
    """Walk forward to the end of the function beginning at `start`.

    Terminates on blr/bctr/unconditional-b, but only once past every forward
    branch target seen so far - otherwise a loop back-edge or a jump to a
    shared epilogue would cut the function short.
    """
    furthest = start
    addr = start
    while addr - start < MAX_SPAN:
        w = img.word(addr)
        target = _branch_target(addr, w)
        if target is not None and start < target < start + MAX_SPAN:
            furthest = max(furthest, target)

        op = (w >> 26) & 0x3F
        unconditional_b = op == 18 and not (w & 1)
        if (w in (BLR, BCTR) or unconditional_b) and addr >= furthest:
            return addr + 4 - start
        addr += 4
    raise SystemExit(f"0x{start:08X}: no terminator within 0x{MAX_SPAN:X} bytes")


def describe(img, start, size):
    """Short disassembly of the body, for the TOML comment."""
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    except ImportError:
        return []
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    lines = []
    for addr in range(start, start + size, 4):
        w = img.word(addr)
        d = list(md.disasm(struct.pack(">I", w), addr))
        text = f"{d[0].mnemonic} {d[0].op_str}".strip() if d else f"<{w:08X}>"
        lines.append(f"#   0x{addr:08X}  {text}")
        if len(lines) >= 12:
            lines.append("#   ...")
            break
    return lines


def existing_addresses(path):
    if not os.path.exists(path):
        return set()
    text = open(path, encoding="utf-8").read()
    return {int(m, 16) for m in re.findall(r"^\s*0x([0-9A-Fa-f]+)\s*=", text, re.M)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addresses", nargs="+", help="guest addresses, e.g. 0x83846050")
    ap.add_argument("--xex", default="assets/default.xex")
    ap.add_argument("--toml", default="config/functions.toml")
    args = ap.parse_args()

    img = XexImage.load(args.xex)
    pdata = pdata_functions(img)
    known = existing_addresses(args.toml)

    added = []
    for raw in args.addresses:
        addr = int(raw, 16) if raw.lower().startswith("0x") else int(raw, 16)
        if addr in known:
            print(f"0x{addr:08X}: already registered, skipping")
            continue
        if not img.contains(addr):
            print(f"0x{addr:08X}: outside the image, skipping")
            continue

        size = function_extent(img, addr)
        section = img.section_of(addr) or "?"

        import bisect
        i = bisect.bisect_right(pdata, addr) - 1
        owner = pdata[i] if i >= 0 else 0
        note = (f"absorbed into pdata fn 0x{owner:08X}"
                if owner and owner != addr else "no pdata entry")

        with open(args.toml, "a", encoding="utf-8") as f:
            f.write(f"\n# {section}, {note}.\n")
            for line in describe(img, addr, size):
                f.write(line + "\n")
            f.write(f'0x{addr:08X} = {{ name = "sub_{addr:08X}_missed", '
                    f"size = 0x{size:X} }}\n")
        known.add(addr)
        added.append((addr, size))
        print(f"0x{addr:08X}: registered, size 0x{size:X} ({note})")

    return 0 if added else 1


if __name__ == "__main__":
    sys.exit(main())
