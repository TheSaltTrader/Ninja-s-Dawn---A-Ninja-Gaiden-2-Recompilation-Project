"""M4 step 1: find the Direct3D device's function-pointer tables in a recompiled
game. The XDK device struct carries a table of SetRenderState_* implementations
(indexed by D3DRENDERSTATETYPE) and one of SetSamplerState_* implementations
(indexed by D3DSAMPLERSTATETYPE); the inline state setters dispatch through
them, and the device constructor fills them with code addresses:

    lis   rA, hi(func)      // lis rA,-32071  (0x82B9)
    addi  rA, rA, lo(func)
    stw   rA, OFF(rDEV)

So a library function that stores many code addresses at consecutive offsets
of one base register is the constructor, the offsets give the table layout and
the targets are the setter implementations - named by their index.

usage: device_tables.py <game_dir> <lib_lo_hex> <lib_hi_hex>
"""
import os, re, sys, glob
from collections import defaultdict

game, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)")
INSN_RE = re.compile(r"^\t// ([a-z][a-z0-9.+-]*)\s*(.*)$")

results = []  # (func, base_reg, [(offset, target)])
for path in sorted(glob.glob(os.path.join(game, "generated", "default", "*_recomp.*.cpp"))):
    cur = None
    regs = {}
    stores = defaultdict(list)
    def flush():
        if cur is None:
            return
        for base, lst in stores.items():
            code = [(off, t) for off, t in lst if 0x82000000 <= t < 0x84000000]
            if len(code) >= 8:
                results.append((cur, base, sorted(code)))
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FUNC_RE.match(line)
            if m:
                flush()
                cur = m.group(1); regs = {}; stores = defaultdict(list)
                a = int(cur[4:], 16)
                if not (lo <= a <= hi):
                    cur = None
                continue
            if cur is None:
                continue
            m = INSN_RE.match(line)
            if not m:
                continue
            mn, args = m.group(1), m.group(2).strip()
            if mn == "lis":
                mm = re.match(r"(r\d+),(-?\d+)", args)
                if mm: regs[mm.group(1)] = ((int(mm.group(2)) & 0xFFFF) << 16, None)
            elif mn == "addi":
                mm = re.match(r"(r\d+),(r\d+),(-?\d+)", args)
                if mm and mm.group(2) in regs and regs[mm.group(2)][1] is None:
                    hi16 = regs[mm.group(2)][0]
                    regs[mm.group(1)] = ((hi16 + int(mm.group(3))) & 0xFFFFFFFF, True)
            elif mn == "ori":
                mm = re.match(r"(r\d+),(r\d+),(-?\d+)", args)
                if mm and mm.group(2) in regs and regs[mm.group(2)][1] is None:
                    regs[mm.group(1)] = ((regs[mm.group(2)][0] | (int(mm.group(3)) & 0xFFFF)), True)
            elif mn in ("stw", "std"):
                mm = re.match(r"(r\d+),(-?\d+)\((r\d+)\)", args)
                if mm and mm.group(1) in regs and regs[mm.group(1)][1]:
                    stores[mm.group(3)].append((int(mm.group(2)), regs[mm.group(1)][0]))
            elif mn in ("mr", "lwz", "ld", "li"):
                mm = re.match(r"(r\d+),", args)
                if mm: regs.pop(mm.group(1), None)
    flush()

results.sort(key=lambda r: -len(r[2]))
for func, base, code in results[:6]:
    offs = [o for o, _ in code]
    print(f"{func}: {len(code)} code addresses stored via {base}, offsets {offs[0]}..{offs[-1]}")
    # contiguous runs of 4-byte slots = tables
    run = [code[0]]
    runs = []
    for prev, nxt in zip(code, code[1:]):
        if nxt[0] - prev[0] == 4:
            run.append(nxt)
        else:
            runs.append(run); run = [nxt]
    runs.append(run)
    for r in runs:
        if len(r) >= 4:
            print(f"  table at +{r[0][0]} (0x{r[0][0]:X}): {len(r)} entries, index -> function:")
            for i, (o, t) in enumerate(r):
                print(f"    [{i:3d}] +0x{o:X} -> sub_{t:08X}")
