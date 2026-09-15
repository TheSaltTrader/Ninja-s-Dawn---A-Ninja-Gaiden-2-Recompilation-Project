"""M4 step 1: a feature table for the library entry points of a game, to name
them by behaviour. For each entry point (from the M3 JSON): instruction count,
argument registers read before written (r3..r10, f1..f8), device-struct
offsets read/written through r3 (the device is always the first argument of a
D3DDevice_* function), the PM4 opcodes it materialises, the library callees,
the number of engine call sites, and whether it returns something in r3.
usage: entry_points.py <game_dir> <M3 json> --out table.md
"""
import os, re, sys, glob, json

game, m3 = sys.argv[1], sys.argv[2]
out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
d = json.load(open(m3))
api = d["api"]; lib = set(d["library"]); emit = d["emitters"]
want = set(api) | set(d["vd_callers"])
FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)")
INSN_RE = re.compile(r"^\t// ([a-z][a-z0-9.+-]*)\s*(.*)$")
CALL_RE = re.compile(r"^\t(sub_[0-9A-Fa-f]{8}|__imp__[A-Za-z0-9_]+)\(ctx, base\);")
feats = {}
for path in sorted(glob.glob(os.path.join(game, "generated", "default", "*_recomp.*.cpp"))):
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FUNC_RE.match(line)
            if m:
                cur = m.group(1) if m.group(1) in want else None
                if cur:
                    feats[cur] = {"n": 0, "args": set(), "written": set(), "dev_r": set(), "dev_w": set(),
                                  "calls": [], "r3_alias": {"r3"}, "stack": 0, "fargs": set()}
                continue
            if not cur:
                continue
            fe = feats[cur]
            m = CALL_RE.match(line)
            if m:
                fe["calls"].append(m.group(1)); continue
            m = INSN_RE.match(line)
            if not m:
                continue
            mn, args = m.group(1), m.group(2).strip()
            fe["n"] += 1
            regs = re.findall(r"\b(r\d+|f\d+)\b", args)
            if not regs:
                continue
            dst, srcs = regs[0], regs[1:]
            if mn in ("stw", "std", "stb", "sth", "stfs", "stfd", "stwu", "stwx", "stwcx."):
                srcs = regs  # stores read every register
                dst = None
            if mn == "stwu":
                mm = re.match(r"r1,(-?\d+)\(r1\)", args)
                if mm: fe["stack"] = -int(mm.group(1))
            for s_ in srcs:
                if s_ not in fe["written"] and re.match(r"r([3-9]|10)$|f[1-8]$", s_):
                    (fe["fargs"] if s_[0] == "f" else fe["args"]).add(s_)
            mm = re.match(r"(?:\w+),(-?\d+)\((r\d+)\)", args)
            if mm and mm.group(2) in fe["r3_alias"]:
                off = int(mm.group(1))
                (fe["dev_w"] if mn.startswith("st") else fe["dev_r"]).add(off)
            if mn == "mr" and len(regs) == 2 and regs[1] in fe["r3_alias"]:
                fe["r3_alias"].add(regs[0])
            if dst and mn not in ("cmpw", "cmplw", "cmpwi", "cmplwi", "cmpd", "cmpld", "cmpdi", "cmpldi"):
                fe["written"].add(dst)
                if dst in fe["r3_alias"] and mn != "mr":
                    fe["r3_alias"].discard(dst)
rows = []
for n in sorted(want, key=lambda x: -len(api.get(x, []))):
    fe = feats.get(n)
    if not fe:
        continue
    ops = ", ".join(sorted({o for o, _ in emit.get(n, [])})) or "-"
    a = sorted(fe["args"], key=lambda r: int(r[1:])); fa = sorted(fe["fargs"])
    devr = sorted(fe["dev_r"])[:10]; devw = sorted(fe["dev_w"])[:10]
    lib_calls = [c for c in fe["calls"] if c in lib]
    imp = [c[7:] for c in fe["calls"] if c.startswith("__imp__")]
    rows.append(f"| {n} | {len(api.get(n, []))} | {fe['n']} | {' '.join(a)}{(' ' + ' '.join(fa)) if fa else ''} | "
                f"{' '.join(str(o) for o in devr)} | {' '.join(str(o) for o in devw)} | {ops} | "
                f"{' '.join(sorted(set(lib_calls)))[:60]} | {' '.join(sorted(set(imp)))} |")
hdr = ("| function | engine sites | insns | args read | dev offsets read (r3) | dev offsets written | PM4 | library callees | imports |\n"
       "| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
text = hdr + "\n" + "\n".join(rows) + "\n"
if out:
    open(out, "w", encoding="utf-8").write(text)
print(text[:5000])
