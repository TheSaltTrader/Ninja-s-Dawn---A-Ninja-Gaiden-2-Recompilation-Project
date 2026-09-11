"""Read the stage-end flow state machine (sub_82441450) gates from a live game.

    python local/diag/peek_flow.py <pid>

Prints the flow struct at 0x83B14A00 and every global the state-7/8 code
tests before it writes the sequencer request word 0x84C250AC.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from watch_ch13 import Guest  # noqa: E402

FLOW = 0x83B14A00
ITEMS = [
    ("flow.state      [0x83B14A00]", FLOW + 0),
    ("flow.mode       [0x83B14A04]", FLOW + 4),
    ("flow.countdown  [0x83B14A08]", FLOW + 8),
    ("flow.saveflag   [0x83B14A0C]", FLOW + 12),
    ("game mode word  [0x84C25070]", 0x84C25070),
    ("seq request     [0x84C250AC]", 0x84C250AC),
    ("gate A ==3      [0x8534B714]", 0x8534B714),
    ("gate B ==0      [0x85032160]", 0x85032160),
    ("gate C idx<16   [0x85309850]", 0x85309850),
    ("gate D ==0      [0x8534F5FC]", 0x8534F5FC),
    ("0x84C23C00 (==1 -> dialog)  ", 0x84C23C00),
    ("0x85040758+8                ", 0x85040758 + 8),
]


def main():
    pid = int(sys.argv[1])
    g = Guest(pid)
    try:
        for label, va in ITEMS:
            v = g.u32be(va)
            print("%s = %d (0x%08X)" % (label, v, v))
        idx = g.u32be(0x85309850)
        if 0 <= idx < 16:
            v = g.u32be(0x85309798 + idx * 4)
            print("gate C table[%d] [0x%08X] = %d  (0/4/11 pass)" % (idx, 0x85309798 + idx * 4, v))
        print("gate C table:", [g.u32be(0x85309798 + i * 4) for i in range(16)])
        p = g.u32be(0x84C3B9E4)
        print("player ptr [0x84C3B9E4] = 0x%08X" % p)
        if p:
            print("  byte +97 =", g.read(p + 97, 1)[0])
        print("stage id half [0x85418668] =", struct.unpack(">H", g.read(0x85418668, 2))[0])
        print("stage byte [0x84B5030A] =", g.read(0x84B5030A, 1)[0])
        # 0x8534B1F0 object: dump first 0x540 words that are non-zero near +1316
        print("0x8534B1F0+1300..1340:", [g.u32be(0x8534B1F0 + o) for o in range(1300, 1344, 4)])
        print("0x8534F5F0+0..32:", [g.u32be(0x8534F5F0 + o) for o in range(0, 32, 4)])
    finally:
        g.close()


if __name__ == "__main__":
    main()
