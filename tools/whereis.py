"""Locate a guest address: containing function, callers, disassembly.

The workhorse for "the log says 0x8385F4FC - what is that, and who reaches it?"

    python tools/whereis.py 0x8385F4FC            # function + callers
    python tools/whereis.py 0x8385F4FC --dis 40   # with disassembly
    python tools/whereis.py 0x8385F4FC --depth 3  # walk callers up 3 levels
"""

import argparse
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_image import XexImage, pdata_functions  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_call_index = None


def build_call_index(img):
    """guest target -> [call sites]. One linear pass over .text."""
    global _call_index
    if _call_index is not None:
        return _call_index
    index = {}
    for name, va, size in img.sections:
        if name != ".text" and not name.startswith(".embsec"):
            continue
        base = img.offset(va)
        for i in range(0, size & ~3, 4):
            w = struct.unpack_from(">I", img.data, base + i)[0]
            if (w >> 26) & 0x3F != 18 or not (w & 1) or (w >> 1) & 1:
                continue
            li = (w >> 2) & 0xFFFFFF
            if li & 0x800000:
                li -= 0x1000000
            addr = va + i
            index.setdefault(addr + (li << 2), []).append(addr)
    _call_index = index
    return index


def containing(pdata, addr):
    i = bisect.bisect_right(pdata, addr) - 1
    if i < 0:
        return None, None
    return pdata[i], (pdata[i + 1] if i + 1 < len(pdata) else None)


def disasm(img, start, count):
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    except ImportError:
        return
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    for addr in range(start, start + count * 4, 4):
        if not img.contains(addr):
            return
        w = img.word(addr)
        d = list(md.disasm(struct.pack(">I", w), addr))
        text = f"{d[0].mnemonic} {d[0].op_str}".strip() if d else f"<{w:08X}>"
        print(f"    0x{addr:08X}  {w:08X}  {text}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("address", type=lambda s: int(s, 16))
    ap.add_argument("--xex", default=os.path.join(ROOT, "assets", "default.xex"))
    ap.add_argument("--dis", type=int, default=0, help="instructions to disassemble")
    ap.add_argument("--depth", type=int, default=1, help="levels of callers to walk")
    ap.add_argument("--max-callers", type=int, default=12)
    args = ap.parse_args()

    img = XexImage.load(args.xex)
    pdata = pdata_functions(img)
    index = build_call_index(img)

    seen = set()
    frontier = [(args.address, 0)]
    while frontier:
        addr, depth = frontier.pop(0)
        if addr in seen or depth > args.depth:
            continue
        seen.add(addr)

        start, end = containing(pdata, addr)
        section = img.section_of(addr)
        indent = "  " * depth
        where = f"fn 0x{start:08X}" if start else "no pdata function"
        span = f" (0x{start:08X}..0x{end:08X}, +0x{addr - start:X})" if start and end else ""
        print(f"{indent}0x{addr:08X}  [{section}]  {where}{span}")

        if args.dis and depth == 0:
            disasm(img, addr, args.dis)

        callers = index.get(start or addr, [])
        print(f"{indent}  callers of 0x{(start or addr):08X}: {len(callers)}")
        for c in callers[:args.max_callers]:
            cs, _ = containing(pdata, c)
            print(f"{indent}    0x{c:08X}  in fn 0x{cs:08X}" if cs else
                  f"{indent}    0x{c:08X}")
            if depth < args.depth:
                frontier.append((c, depth + 1))
        if len(callers) > args.max_callers:
            print(f"{indent}    ... {len(callers) - args.max_callers} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
