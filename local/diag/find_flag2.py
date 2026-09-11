"""Find every access to guest 0x84C39440 - the flag the chapter 12 -> 13
hand-off spins on.

The first attempt looked for a lis+addi landing exactly on the address and found
nothing, but the self-test showed 0x84C3943C being formed three times - four
bytes short. PowerPC does not usually materialise a field's address; it forms a
base and reaches the field with the load's own displacement. So this tracks the
register values produced by lis/addi/ori and then checks the DISPLACEMENT of
each following lwz/stw/lwzx/stwx against them.

Reads and writes are reported separately, because the question is not "who
touches this" but "who was supposed to set it and did not".
"""
import glob
import os
import re

GEN = (r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
       r"\generated\default")
TARGET = 0x84C250AC

LIS = re.compile(r"//\s*lis\s+(r\d+),\s*(-?\d+)")
ADDI = re.compile(r"//\s*(?:addi|ori)\s+(r\d+),(r\d+),\s*(-?\d+)")
MEM = re.compile(r"//\s*(lwz|stw|lhz|sth|lbz|stb)\s+(r\d+),(-?\d+)\((r\d+)\)")
FUNC = re.compile(r"DEFINE_REX_FUNC\((\w+)\)")

reads, writes = [], []

for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    base = os.path.basename(path)
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    func = "?"
    regs = {}                       # reg -> (value, line index)
    for i, line in enumerate(lines):
        m = FUNC.search(line)
        if m:
            func, regs = m.group(1), {}
            continue

        m = LIS.search(line)
        if m:
            regs[m.group(1)] = ((int(m.group(2)) << 16) & 0xFFFFFFFF, i)
            continue

        m = ADDI.search(line)
        if m:
            dst, src, imm = m.group(1), m.group(2), int(m.group(3))
            if src in regs and i - regs[src][1] <= 8:
                regs[dst] = (((regs[src][0] + imm) & 0xFFFFFFFF), i)
            else:
                regs.pop(dst, None)
            continue

        m = MEM.search(line)
        if m:
            op, _rt, disp, ra = m.group(1), m.group(2), int(m.group(3)), m.group(4)
            if ra in regs and i - regs[ra][1] <= 12:
                addr = (regs[ra][0] + disp) & 0xFFFFFFFF
                if addr == TARGET:
                    ctx = " | ".join(l.strip()[3:] for l in lines[max(0, i - 3):i + 4]
                                     if l.strip().startswith("//"))
                    rec = (base, i + 1, func, op, ctx)
                    (writes if op.startswith("st") else reads).append(rec)

print("=== accesses to 0x%08X ===" % TARGET)
print("reads:  %d" % len(reads))
print("writes: %d" % len(writes))

for label, group in (("WRITE", writes), ("READ", reads)):
    for b, ln, func, op, ctx in group:
        print("\n%-5s %s:%d  in %s   (%s)" % (label, b, ln, func, op))
        print("      %s" % ctx[:220])
