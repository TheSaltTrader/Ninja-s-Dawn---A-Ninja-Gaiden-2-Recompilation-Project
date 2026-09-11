"""Find static references to a guest address range.

Answers "who reads or writes this global?" - the question that comes up
constantly once a fault points at an uninitialised pointer. Resolves the two
ways PowerPC code forms an absolute address:

    lis  rX, hi            ; then  lwz/stw  disp(rX)
    lis  rX, hi            ; addi rY, rX, lo   ; then  lwz/stw  disp(rY)

    python tools/xrefs.py 0x84C23C70                 # one address
    python tools/xrefs.py 0x84C23C70 0x84C24070      # a range
    python tools/xrefs.py 0x84C23C70 --stores        # writers only
"""

import argparse
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_image import XexImage, pdata_functions  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# D-form opcodes that carry a signed 16-bit displacement off a base register.
LOADS = {32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 40: "lhz", 41: "lhzu",
         42: "lha", 48: "lfs", 50: "lfd", 58: "ld"}
STORES = {36: "stw", 37: "stwu", 38: "stb", 39: "stbu", 44: "sth", 45: "sthu",
          52: "stfs", 54: "stfd", 62: "std"}


def signed16(v):
    return v - 0x10000 if v & 0x8000 else v


# "with update" forms also write back the base register rA.
_LOAD_UPDATE = {33, 35, 41, 43, 49, 51, 59}
_STORE_UPDATE = {37, 39, 45, 53, 55, 61}
_NO_GPR_WRITE = {10, 11, 16, 18, 19}  # compares and branches


def clobbers(word, reg):
    """Might this instruction overwrite `reg`?

    The operand roles differ by form, and getting them wrong breaks the scan in
    both directions: for a load, bits 16-20 is the *base*, so treating it as a
    destination discards valid matches; for `or`/`mr` and the immediate-logical
    ops it *is* the destination, so ignoring it invents them.

      loads   rT (21-25) written, rA (16-20) is the base (plus rA if "u" form)
      stores  nothing written    (plus rA if "u" form)
      other   either field may be the destination - assume both, conservatively
    """
    op = (word >> 26) & 0x3F
    rt = (word >> 21) & 0x1F
    ra = (word >> 16) & 0x1F
    if op in STORES:
        return op in _STORE_UPDATE and ra == reg
    if op in LOADS:
        return rt == reg or (op in _LOAD_UPDATE and ra == reg)
    if op in _NO_GPR_WRITE:
        return False
    return rt == reg or ra == reg


def scan(img, lo, hi, want_stores, want_loads):
    """-> list of (address, opcode_word, mnemonic, is_store, target)"""
    out = []
    for name, va, size in img.sections:
        if name != ".text" and not name.startswith(".embsec"):
            continue
        base = img.offset(va)
        end = min(base + size, len(img.data)) - 4
        # `i` is a file offset into the flat image, so an instruction's guest
        # address is image_base + i. (Adding the section va here instead would
        # double-count the section base and shift every result.)
        origin = img.image_base
        for i in range(base, end, 4):
            w = struct.unpack_from(">I", img.data, i)[0]
            # lis rT, imm   ==  addis rT, 0, imm
            if (w >> 26) & 0x3F != 15 or ((w >> 16) & 0x1F) != 0:
                continue
            upper = (w & 0xFFFF) << 16
            rt = (w >> 21) & 0x1F

            # Candidate bases: the lis result itself, plus any addi off it.
            # r1 (stack) and r13 (TLS) are never a static global base - an addi
            # that lands in one of those is a frame pointer, and matching stores
            # against it produces stack-relative false positives.
            # Each base carries the offset of the instruction that defines it,
            # so the scan starts *after* it - otherwise the addi looks like a
            # clobber of its own destination and every match is discarded.
            bases = [(rt, upper, 0)] if rt not in (1, 13) else []
            for j in range(4, 0x20, 4):
                if i + j >= end:
                    break
                w2 = struct.unpack_from(">I", img.data, i + j)[0]
                if (w2 >> 26) & 0x3F == 14 and ((w2 >> 16) & 0x1F) == rt:
                    ry = (w2 >> 21) & 0x1F
                    if ry not in (1, 13):
                        bases.append((ry,
                                      (upper + signed16(w2 & 0xFFFF)) & 0xFFFFFFFF,
                                      j))

            for reg, absbase, defined_at in bases:
                for k in range(defined_at + 4, defined_at + 4 + 0x40, 4):
                    if i + k >= end:
                        break
                    w3 = struct.unpack_from(">I", img.data, i + k)[0]
                    op = (w3 >> 26) & 0x3F
                    is_store = op in STORES
                    if not (is_store or op in LOADS):
                        if clobbers(w3, reg):
                            break
                        continue
                    if ((w3 >> 16) & 0x1F) != reg:
                        # Not our base, but it may still overwrite it (a load
                        # into reg ends the base's life).
                        if clobbers(w3, reg):
                            break
                        continue
                    tgt = (absbase + signed16(w3 & 0xFFFF)) & 0xFFFFFFFF
                    if lo <= tgt < hi:
                        if (is_store and want_stores) or (not is_store and want_loads):
                            mnem = (STORES if is_store else LOADS)[op]
                            out.append((origin + i + k, w3, mnem, is_store, tgt))
                    break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("start", type=lambda s: int(s, 16))
    ap.add_argument("end", nargs="?", type=lambda s: int(s, 16))
    ap.add_argument("--xex", default=os.path.join(ROOT, "assets", "default.xex"))
    ap.add_argument("--stores", action="store_true", help="writers only")
    ap.add_argument("--loads", action="store_true", help="readers only")
    args = ap.parse_args()

    lo = args.start
    hi = args.end if args.end else args.start + 4
    want_stores = args.stores or not args.loads
    want_loads = args.loads or not args.stores

    img = XexImage.load(args.xex)
    pdata = pdata_functions(img)

    def fn_of(a):
        i = bisect.bisect_right(pdata, a) - 1
        return pdata[i] if i >= 0 else 0

    hits = scan(img, lo, hi, want_stores, want_loads)
    stores = [h for h in hits if h[3]]
    loads = [h for h in hits if not h[3]]
    print(f"0x{lo:08X}..0x{hi:08X} [{img.section_of(lo)}]: "
          f"{len(stores)} stores, {len(loads)} loads")
    for addr, _w, mnem, is_store, tgt in sorted(hits, key=lambda h: h[0]):
        kind = "STORE" if is_store else "load "
        print(f"  {kind} 0x{addr:08X}  {mnem:<5} -> 0x{tgt:08X}   "
              f"(fn 0x{fn_of(addr):08X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
