"""Find the XDK device's shadow-state setters that never touch the ring
buffer: every function that read-modify-writes the device's 64-bit dirty
flags (`ld rX,16(rD)` ... `std rX,16(rD)` with rD = r3 or a copy of it) -
SetTexture, SetStreamSource, SetIndices, SetVertexShader, SetPixelShader,
Set*ShaderConstant*, SetViewport, SetScissorRect, SetRenderTarget..., plus
the render/sampler-state setters already known from the dispatch tables.
Reports each with its instruction count, the other device offsets it writes
and the dirty bits it sets; writes a JSON for gen_trace_hooks.py --dirty.
usage: dirty_setters.py <game_dir> --out <json> [--known <tables.txt>]
"""
import glob, json, os, re, sys

game = sys.argv[1]
out = sys.argv[sys.argv.index("--out") + 1]
FUNC = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)")
INSN = re.compile(r"^\t// ([a-z][a-z0-9.+-]*)\s*(.*)$")
found = {}
cur = None
for path in sorted(glob.glob(os.path.join(game, "generated", "default", "*_recomp.*.cpp"))):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FUNC.match(line)
            if m:
                cur = m.group(1); alias = {"r3"}; n = 0; ld16 = False; std16 = False
                writes = set(); bits = set(); flag_regs = {}
                continue
            if cur is None:
                continue
            m = INSN.match(line)
            if not m:
                if line.startswith("}") and cur:
                    if ld16 and std16:
                        found[cur] = {"insns": n, "writes": sorted(writes), "bits": sorted(bits)}
                    cur = None
                continue
            mn, args = m.group(1), m.group(2).strip()
            n += 1
            if mn == "mr":
                mm = re.match(r"(r\d+),(r\d+)", args)
                if mm and mm.group(2) in alias: alias.add(mm.group(1))
                elif mm: alias.discard(mm.group(1))
            elif mn == "ld":
                mm = re.match(r"(r\d+),16\((r\d+)\)", args)
                if mm and mm.group(2) in alias:
                    ld16 = True; flag_regs[mm.group(1)] = 0
            elif mn in ("ori", "oris") and flag_regs:
                mm = re.match(r"(r\d+),(r\d+),(-?\d+)", args)
                if mm and mm.group(2) in flag_regs:
                    v = int(mm.group(3)) & 0xFFFF
                    if mn == "oris": v <<= 16
                    for b in range(64):
                        if v >> b & 1: bits.add(b)
                    flag_regs[mm.group(1)] = 1
            elif mn == "rldicr" and flag_regs:
                mm = re.match(r"(r\d+),(r\d+),(\d+),63", args)
                if mm and mm.group(2) not in flag_regs:
                    pass
            elif mn == "std":
                mm = re.match(r"(r\d+),16\((r\d+)\)", args)
                if mm and mm.group(2) in alias and mm.group(1) in flag_regs:
                    std16 = True
            if mn.startswith("st") and mn not in ("stwu",):
                mm = re.match(r"r\d+,(-?\d+)\((r\d+)\)", args)
                if mm and mm.group(2) in alias:
                    off = int(mm.group(1))
                    if off != 16: writes.add(off)
            # any write to an alias register kills the alias (except mr handled above)
            if mn not in ("mr",) and not mn.startswith("st") and not mn.startswith("cmp"):
                mm = re.match(r"(r\d+),", args)
                if mm and mm.group(1) in alias and mm.group(1) != "r3":
                    alias.discard(mm.group(1))
known = set()
if "--known" in sys.argv:
    for line in open(sys.argv[sys.argv.index("--known") + 1]):
        mm = re.search(r"-> sub_([0-9A-F]{8})", line)
        if mm: known.add("sub_" + mm.group(1))
rows = sorted(found.items())
print(f"{len(rows)} dirty-flag setters, {sum(1 for f, _ in rows if f in known)} already in the dispatch tables")
for f, d in rows:
    tag = "table" if f in known else "NEW"
    print(f"{f} {tag:5s} insns={d['insns']:4d} bits={d['bits']} writes={d['writes'][:8]}")
json.dump({"setters": {f: d for f, d in rows}, "known": sorted(known)}, open(out, "w"), indent=1)
