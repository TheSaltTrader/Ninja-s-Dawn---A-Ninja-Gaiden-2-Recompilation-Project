"""Native-GPU spike, milestone M3: size the Direct3D 9 device surface of a
recompiled Xbox 360 game from its ReXGlue output, offline.

The 360's Direct3D is a static library linked into the XEX: its device
functions build PM4 packets into the ring buffer and call the Vd* kernel
entry points. In ReXGlue's generated C++ every guest function is a
`DEFINE_REX_FUNC(sub_XXXXXXXX)` block whose lines carry the original PPC
instruction as a comment (`// lis r11,-16384`), so the library can be found
without a disassembler:

  1. PM4 emitters: functions that materialise a type-3 packet header
     (0xC0xxxxxx with a known opcode) through a lis/ori pair.
  2. Kernel callers: functions that call a Vd* import.
  3. The library = the callee-closure of (1)+(2) restricted to functions that
     are reached only from... no - kept simple: (1)+(2) plus everything that
     calls them, split into "shared" (same instruction fingerprint in the
     other game = XDK library code) and "game-only" (the engine's use sites).
  4. Fingerprints: the mnemonic+register sequence of a function with absolute
     addresses masked, so the same library function matches across games at
     different addresses.

usage: recon.py <game_dir> [<other_game_dir>] [--out report.md]
"""
import os, re, sys, json, hashlib, glob
from collections import defaultdict, Counter

PM4_OPCODES = {0x48: "ME_INIT", 0x10: "NOP", 0x3f: "INDIRECT_BUFFER", 0x37: "INDIRECT_BUFFER_PFD",
               0x26: "WAIT_FOR_IDLE", 0x3c: "WAIT_REG_MEM", 0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE",
               0x5c: "WAIT_UNTIL_READ", 0x5d: "WAIT_IB_PFD_COMPLETE", 0x21: "REG_RMW", 0x3e: "REG_TO_MEM",
               0x3d: "MEM_WRITE", 0x4f: "MEM_WRITE_CNTR", 0x44: "COND_EXEC", 0x45: "COND_WRITE",
               0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL", 0x5a: "EVENT_WRITE_EXT",
               0x5b: "EVENT_WRITE_ZPD", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x34: "DRAW_INDX_BIN",
               0x35: "DRAW_INDX_2_BIN", 0x23: "VIZ_QUERY", 0x25: "SET_STATE", 0x2d: "SET_CONSTANT",
               0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x2f: "LOAD_ALU_CONSTANT", 0x27: "IM_LOAD",
               0x2b: "IM_LOAD_IMMEDIATE", 0x2e: "LOAD_CONSTANT_CONTEXT", 0x3b: "INVALIDATE_STATE",
               0x4a: "SET_SHADER_BASES", 0x4b: "SET_BIN_BASE_OFFSET", 0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT",
               0x5e: "CONTEXT_UPDATE", 0x54: "INTERRUPT", 0x64: "XE_SWAP", 0x2c: "IM_STORE",
               0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI", 0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI"}

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)")
INSN_RE = re.compile(r"^\t// ([a-z][a-z0-9.+-]*)\s*(.*)$")
CALL_RE = re.compile(r"^\t(sub_[0-9A-Fa-f]{8}|__imp__[A-Za-z0-9_]+)\(ctx, base\);")
ADDR_RE = re.compile(r"0x8[0-9a-fA-F]{7}\b")


def parse_game(game_dir):
    gen = os.path.join(game_dir, "generated", "default")
    files = sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp")))
    funcs = {}   # name -> dict(insns, calls, consts, fp)
    order = []
    for path in files:
        cur = None
        regs = {}  # reg -> high 16 from lis
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    cur = {"name": m.group(1), "insns": [], "calls": [], "consts": set(), "file": os.path.basename(path)}
                    funcs[cur["name"]] = cur
                    order.append(cur["name"])
                    regs = {}
                    continue
                if cur is None:
                    continue
                m = CALL_RE.match(line)
                if m:
                    cur["calls"].append(m.group(1))
                    continue
                m = INSN_RE.match(line)
                if not m:
                    continue
                mn, args = m.group(1), m.group(2).strip()
                cur["insns"].append((mn, args))
                # lis/ori|addi pairs -> 32-bit constants
                if mn == "lis":
                    mm = re.match(r"(r\d+),(-?\d+)", args)
                    if mm:
                        regs[mm.group(1)] = (int(mm.group(2)) & 0xFFFF) << 16
                elif mn in ("ori", "oris", "addi", "addic"):
                    mm = re.match(r"(r\d+),(r\d+),(-?\d+)", args)
                    if mm and mm.group(2) in regs and mm.group(1) == mm.group(2):
                        hi = regs.pop(mm.group(2))
                        lo = int(mm.group(3))
                        val = (hi | (lo & 0xFFFF)) if mn in ("ori",) else ((hi + lo) & 0xFFFFFFFF)
                        if mn == "oris":
                            val = hi | ((lo & 0xFFFF) << 16)
                        cur["consts"].add(val & 0xFFFFFFFF)
                elif mn in ("mr", "li", "lwz", "ld", "lbz", "lhz", "lfs", "lfd"):
                    mm = re.match(r"(r\d+),", args)
                    if mm:
                        regs.pop(mm.group(1), None)
    # fingerprints
    for name, fn in funcs.items():
        parts = []
        for mn, args in fn["insns"]:
            a = ADDR_RE.sub("A", args)
            a = re.sub(r"-?\d+", "I", a)  # every immediate (addresses, offsets, constants)
            parts.append(mn + " " + a)
        fn["fp"] = hashlib.md5("\n".join(parts).encode()).hexdigest()
        fn["len"] = len(fn["insns"])
    return funcs, order


def pm4_headers(consts):
    out = []
    for c in consts:
        if (c & 0xC0000000) == 0xC0000000 and (c & 0xFF) == 0:
            op = (c >> 8) & 0x7F
            if op in PM4_OPCODES:
                out.append((op, (c >> 16) & 0x3FFF))
    return out


def analyze(game_dir, other_dir=None, out_path=None):
    funcs, order = parse_game(game_dir)
    other = parse_game(other_dir)[0] if other_dir else {}
    other_fps = defaultdict(list)
    for n, fn in other.items():
        other_fps[fn["fp"]].append(n)
    callers = defaultdict(set)
    for n, fn in funcs.items():
        for c in fn["calls"]:
            callers[c].add(n)
    emitters = {}
    for n, fn in funcs.items():
        hdrs = pm4_headers(fn["consts"])
        if hdrs:
            emitters[n] = hdrs
    vd_callers = {n: [c for c in fn["calls"] if c.startswith("__imp__Vd")] for n, fn in funcs.items()
                  if any(c.startswith("__imp__Vd") for c in fn["calls"])}
    lib_seed = set(emitters) | set(vd_callers)
    # A static library is linked contiguously: the library is the address clusters
    # the seed functions fall in (gaps under 64 KB join a cluster; a cluster needs
    # at least 3 seeds), every function whose address lies inside a cluster is
    # library code, everything else is the engine.
    addrs = sorted(int(n[4:], 16) for n in lib_seed)
    clusters, cur = [], [addrs[0]] if addrs else []
    for a in addrs[1:]:
        if a - cur[-1] < 0x10000:
            cur.append(a)
        else:
            clusters.append(cur); cur = [a]
    if cur: clusters.append(cur)
    clusters = [c for c in clusters if len(c) >= 3]
    # The Direct3D library is the cluster(s) holding the Vd* kernel callers; other
    # seed clusters are the ENGINE's own inline PM4 (tiling, fences) and are
    # reported separately - a native port must intercept those too.
    vd_addrs = {int(n[4:], 16) for n in vd_callers}
    lib_clusters = [c for c in clusters if any(c[0] <= a <= c[-1] for a in vd_addrs)] or clusters
    engine_clusters = [c for c in clusters if c not in lib_clusters]
    ranges = [(c[0], c[-1]) for c in lib_clusters]
    engine_ranges = [(c[0], c[-1]) for c in engine_clusters]
    def in_lib(name):
        a = int(name[4:], 16)
        return any(lo <= a <= hi for lo, hi in ranges)
    lib = {n for n in funcs if in_lib(n)}
    # API surface = library functions called from non-library (engine) code
    api = {}
    for n in lib:
        eng = [p for p in callers.get(n, ()) if p not in lib]
        if eng:
            api[n] = eng
    shared = {n for n in funcs if other and funcs[n]["fp"] in other_fps and funcs[n]["len"] >= 12}
    lines = []
    g = os.path.basename(os.path.normpath(game_dir))
    lines.append(f"# Native GPU M3 recon - {g}\n")
    lines.append(f"functions: {len(funcs)}   shared-fingerprint with {os.path.basename(os.path.normpath(other_dir)) if other_dir else '-'}: {len(shared)}\n")
    lines.append("Direct3D library address range(s) (clusters holding the Vd* callers): " + ", ".join(f"{lo:08X}-{hi:08X}" for lo, hi in ranges) + chr(10))
    if engine_ranges:
        lines.append("engine-side PM4 emitter clusters (inline XDK code in game functions): " + ", ".join(f"{lo:08X}-{hi:08X}" for lo, hi in engine_ranges) + chr(10))
    eng_emit = [n for n in emitters if not in_lib(n)]
    lines.append(f"engine functions that emit PM4 themselves (outside the library): {len(eng_emit)}" + chr(10))
    lines.append(f"PM4 type-3 emitters: {len(emitters)}   Vd* callers: {len(vd_callers)}   library functions in range: {len(lib)} "
                 f"(shared {sum(1 for n in lib if n in shared)})   API surface (library functions called by engine code): {len(api)}\n")
    lines.append("\n## PM4 emitters (function, shared?, opcodes, #engine callers)\n")
    for n in sorted(emitters, key=lambda x: -len(callers.get(x, ()))):
        ops = ", ".join(sorted({PM4_OPCODES[o] for o, _ in emitters[n]}))
        lines.append(f"- {n}  {'shared' if n in shared else 'game-only'}  len {funcs[n]['len']}  [{ops}]  callers {len(callers.get(n, ()))} (engine {len(api.get(n, []))})")
    lines.append("\n## Vd* kernel callers\n")
    for n, vs in sorted(vd_callers.items()):
        lines.append(f"- {n}  {'shared' if n in shared else 'game-only'}  {', '.join(sorted(set(v[7:] for v in vs)))}  callers {len(callers.get(n, ()))}")
    lines.append("\n## API surface: library entry points called from engine code (sorted by call sites)\n")
    for n in sorted(api, key=lambda x: -len(api[x])):
        ops = ", ".join(sorted({PM4_OPCODES[o] for o, _ in emitters.get(n, [])})) or "-"
        lines.append(f"- {n}  {'shared' if n in shared else 'game-only'}  len {funcs[n]['len']}  pm4 [{ops}]  engine call sites {len(api[n])}")
    lines.append(f"\nTotal engine call sites into the library: {sum(len(v) for v in api.values())}\n")
    report = "\n".join(lines)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(report)
    data = {"functions": len(funcs), "shared": len(shared), "emitters": {n: [(PM4_OPCODES[o], c) for o, c in h] for n, h in emitters.items()},
            "vd_callers": vd_callers, "library": sorted(lib), "api": {n: sorted(v) for n, v in api.items()},
            "fp": {n: funcs[n]["fp"] for n in lib}}
    if out_path:
        with open(out_path.replace(".md", ".json"), "w") as f:
            json.dump(data, f)
    print(report[:6000])
    return data


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = None
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
        args = [a for a in args if a != out]
    analyze(args[0], args[1] if len(args) > 1 else None, out)
