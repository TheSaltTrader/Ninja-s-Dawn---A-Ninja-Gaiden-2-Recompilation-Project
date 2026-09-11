"""Chapter 12 -> 13: resolve which critical sections the stuck worker enters,
and find who else holds them.

The worker sub_822F16D0 is blocked inside a call, and the only indefinite
blockers in its stuck region are NtWaitForSingleObjectEx and
RtlEnterCriticalSection (eight chains). If the worker enters a critical section
that the SPINNING side (sub_822F34B0 or a callee it runs while spinning) holds,
that is a classic hold-and-wait deadlock -- findable statically, no playthrough
needed.

Method, same as cracked the music bug:
  1. Walk the worker's stuck region; collect every RtlEnterCriticalSection
     reachable within a few call levels, with the chain that reaches it.
  2. At each leaf call, resolve the critical-section guest address -- the CS is
     r3, formed by lis/addi/ori in the few instructions before the call.
  3. Do the same for the spinner side (sub_822F34B0 and the functions it calls
     before and during its spin).
  4. Any CS address that appears on BOTH sides is a suspect: the worker waits on
     a lock the spinner may be holding while it waits for the worker.

CS addresses that cannot be resolved statically (loaded from memory, passed in)
are reported as "dynamic" -- they are not cleared, only un-provable here.
"""
import glob
import os
import re

GEN = (r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
       r"\generated\default")

DEF = re.compile(r"DEFINE_REX_FUNC\((\w+)\)")
CALL = re.compile(r"^\t(sub_[0-9A-F]{8})\(ctx, base\);", re.M)
ENTER = re.compile(r"__imp__RtlEnterCriticalSection\(ctx, base\)")
LIS = re.compile(r"//\s*lis\s+(r\d+),\s*(-?\d+)")
ADDI = re.compile(r"//\s*(?:addi|ori)\s+(r\d+),(r\d+),\s*(-?\d+)")
MR = re.compile(r"//\s*mr\s+(r\d+),(r\d+)")

# ---- index every function body once -------------------------------------
bodies = {}
for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    text = open(path, encoding="utf-8", errors="replace").read()
    parts = DEF.split(text)
    for i in range(1, len(parts) - 1, 2):
        bodies[parts[i]] = parts[i + 1]


def resolve_r3_before(lines, call_idx):
    """Best-effort value of r3 at the instruction index call_idx.

    Walks backward a short window tracking register formation. Returns an int
    address or None if r3's value is not statically determinable in-window.
    """
    regs = {}          # reg -> value
    # Walk forward through a window ending at the call, so later writes win.
    start = max(0, call_idx - 40)
    for i in range(start, call_idx):
        line = lines[i]
        m = LIS.search(line)
        if m:
            regs[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = ADDI.search(line)
        if m:
            dst, src, imm = m.group(1), m.group(2), int(m.group(3))
            if src in regs:
                regs[dst] = (regs[src] + imm) & 0xFFFFFFFF
            else:
                regs.pop(dst, None)
            continue
        m = MR.search(line)
        if m:
            dst, src = m.group(1), m.group(2)
            if src in regs:
                regs[dst] = regs[src]
            else:
                regs.pop(dst, None)
    return regs.get("r3")


def enters_in(name):
    """List of (cs_addr_or_None) for each RtlEnterCriticalSection call in name."""
    body = bodies.get(name)
    if not body:
        return []
    lines = body.split("\n")
    out = []
    for i, line in enumerate(lines):
        if ENTER.search(line):
            out.append(resolve_r3_before(lines, i))
    return out


def collect(root, max_depth=3):
    """Every (chain, cs_addr) for RtlEnterCriticalSection reachable from root."""
    found = []
    seen = set()

    def walk(name, depth, chain):
        if depth > max_depth or name in seen:
            return
        seen.add(name)
        for cs in enters_in(name):
            found.append((chain + [name], cs))
        for callee in sorted(set(CALL.findall(bodies.get(name, "")))):
            walk(callee, depth + 1, chain + [name])

    walk(root, 0, [])
    return found


def region_calls(name, start_label, end_label):
    """Direct sub_ calls between two labels in a function body."""
    body = bodies.get(name, "")
    s = body.find(start_label)
    e = body.find(end_label)
    if s < 0:
        return []
    seg = body[s:e if e > s else len(body)]
    return sorted(set(CALL.findall(seg)))


def fmt(cs):
    return "0x%08X" % cs if cs is not None else "dynamic"


# ---- worker side: sub_822F16D0's stuck region ---------------------------
print("=" * 66)
print("WORKER  sub_822F16D0  (stuck region loc_822F1880 .. loc_822F21B0)")
print("=" * 66)
worker_calls = region_calls("sub_822F16D0", "loc_822F1880:", "loc_822F21B0:")
worker_cs = {}
for c in worker_calls:
    for chain, cs in collect(c):
        worker_cs.setdefault(cs, []).append(" -> ".join(chain))
for cs in sorted(worker_cs, key=lambda x: (x is None, x)):
    print("\n  CS %s   via %d chain(s), e.g.:" % (fmt(cs), len(worker_cs[cs])))
    print("     %s" % worker_cs[cs][0])

# ---- spinner side: sub_822F34B0 -----------------------------------------
print("\n" + "=" * 66)
print("SPINNER  sub_822F34B0  (whole function + callees)")
print("=" * 66)
spin_cs = {}
for chain, cs in collect("sub_822F34B0"):
    spin_cs.setdefault(cs, []).append(" -> ".join(chain))
for cs in sorted(spin_cs, key=lambda x: (x is None, x)):
    print("  CS %s   via %s" % (fmt(cs), spin_cs[cs][0]))

# ---- the answer ---------------------------------------------------------
print("\n" + "=" * 66)
print("SHARED critical sections (worker waits on one the spinner may hold):")
print("=" * 66)
shared = [cs for cs in worker_cs if cs is not None and cs in spin_cs]
if not shared:
    print("  none resolved statically.")
    wd = sorted(x for x in worker_cs if x is None)
    print("  worker has %d dynamic (un-resolvable) CS enters; spinner %d."
          % (len([x for x in worker_cs if x is None]),
             len([x for x in spin_cs if x is None])))
for cs in shared:
    print("  *** 0x%08X entered by BOTH sides -- deadlock suspect ***" % cs)
