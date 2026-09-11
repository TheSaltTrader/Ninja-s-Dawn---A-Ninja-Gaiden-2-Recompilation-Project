"""Read arbitrary guest words from a live game.

    python local/diag/peek.py <pid> ADDR[:u8|u16|u32|xN] ...

ADDR is hex (0x optional). Default width u32 big-endian; xN dumps N bytes.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from watch_ch13 import Guest  # noqa: E402


def main():
    pid = int(sys.argv[1])
    g = Guest(pid)
    try:
        for spec in sys.argv[2:]:
            addr, _, kind = spec.partition(":")
            va = int(addr, 16)
            kind = kind or "u32"
            if kind == "u8":
                v = g.read(va, 1)[0]
                print("0x%08X u8  = %d (0x%02X)" % (va, v, v))
            elif kind == "u16":
                v = struct.unpack(">H", g.read(va, 2))[0]
                print("0x%08X u16 = %d (0x%04X)" % (va, v, v))
            elif kind == "u32":
                v = g.u32be(va)
                print("0x%08X u32 = %d (0x%08X)" % (va, v, v))
            elif kind.startswith("x"):
                n = int(kind[1:])
                data = g.read(va, n)
                for off in range(0, n, 16):
                    chunk = data[off:off + 16]
                    words = " ".join("%08X" % w for w in struct.unpack(">%dI" % (len(chunk) // 4), chunk[:len(chunk) // 4 * 4]))
                    print("0x%08X: %s" % (va + off, words))
    finally:
        g.close()


if __name__ == "__main__":
    main()
