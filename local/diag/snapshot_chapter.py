"""Snapshot the chapter/area identity of a live game, for diffing 12 vs 13.

    python local/diag/snapshot_chapter.py <pid> [--out FILE]

Dumps a labelled, diff-friendly set of the fields that identify which chapter
and area is loaded: the engine mode words, the area pointer 0x84C23C48 and the
object it points to (name string included), the chapter/stage id bytes, the
sequencer state, and the site table. Run it once with chapter 13 loaded
normally, once on the stuck chapter-12 mist, and diff the two files - the
differences are what the broken 12->13 transition should have set.
"""
import argparse
import ctypes
import struct
import sys

GUEST_BASE = 0x1_0000_0000
k32 = ctypes.WinDLL("kernel32", use_last_error=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    h = k32.OpenProcess(0x0410, False, args.pid)
    if not h:
        print("OpenProcess failed:", ctypes.get_last_error())
        return 1

    def read(va, n):
        buf = (ctypes.c_ubyte * n)()
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(GUEST_BASE + va), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return bytes(buf)

    def u32(va):
        b = read(va, 4)
        return None if b is None else int.from_bytes(b, "big")

    lines = []

    def p(s):
        lines.append(s)

    p("mode      0x84C25070 = %s" % hexn(u32(0x84C25070)))
    p("gametype  0x84C25074 = %s" % hexn(u32(0x84C25074)))
    p("req       0x84C250AC = %s" % hexn(u32(0x84C250AC)))
    p("substate  0x84AED970 = %s" % hexn(u32(0x84AED970)))
    seq = read(0x842DC324, 4)
    p("seq bytes 0x842DC324 = %s" % (seq.hex() if seq else "None"))
    flow = read(0x83B14A00, 16)
    p("flow      0x83B14A00 = %s" % (flow.hex() if flow else "None"))

    # chapter / stage id candidates
    p("chapid?   0x85418660 = %s" % (fmt(read(0x85418660, 8))))
    p("stageblk  0x84C23C70 = %s" % (fmt(read(0x84C23C70, 16))))

    # engine struct area-pointer neighbourhood
    p("engarea   0x84C23C18 = %s" % (fmt(read(0x84C23C18, 64))))

    # the area pointer and the object it names
    areap = u32(0x84C23C48)
    p("areaptr   0x84C23C48 = %s" % hexn(areap))
    if areap and 0x80000000 <= areap <= 0xFFFFFFFF:
        obj = read(areap, 128)
        p("  areaobj @0x%08X:" % areap)
        if obj:
            for off in range(0, 128, 16):
                chunk = obj[off:off + 16]
                words = " ".join("%08X" % w for w in struct.unpack(">4I", chunk))
                ascii_ = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
                p("    +%03X  %s  |%s|" % (off, words, ascii_))
        else:
            p("    <unreadable - pointer not resident>")

    # site table first entries +560
    for i in range(4):
        base = 0x83CDEDE8 + i * 584
        p("site[%d]+560 0x%08X = %s" % (i, base + 560, hexn(u32(base + 560))))

    out = "\n".join(lines)
    print(out)
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(out + "\n")
    k32.CloseHandle(h)
    return 0


def hexn(v):
    return "None" if v is None else "0x%08X (%d)" % (v, v)


def fmt(b):
    if b is None:
        return "None"
    return b.hex()


if __name__ == "__main__":
    sys.exit(main())
