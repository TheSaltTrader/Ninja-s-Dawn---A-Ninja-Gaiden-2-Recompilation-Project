"""Find the code that touches guest 0x84C39440 - the flag the chapter 12 -> 13
hand-off spins on.

An earlier pass concluded the address was a runtime pointer because a grep for
"0x84C39440" found nothing. That was wrong: codegen writes `lis` immediates as
SIGNED DECIMAL, so 0x84C40000 appears as -2067529728 and the address is formed
by a following addi of -27584. Searching for the hex could never have matched.

A `lis` is paired with its addi/ori within a few lines, so this walks each file
and reports the pairs that actually land on the target.
"""
import glob
import os
import re

GEN = (r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
       r"\generated\default")
TARGET = 0x852AE454

LIS = re.compile(r"//\s*lis\s+(r\d+),\s*(-?\d+)")
ADDI = re.compile(r"//\s*addi\s+(r\d+),(r\d+),(-?\d+)")
ORI = re.compile(r"//\s*ori\s+(r\d+),(r\d+),(-?\d+)")
LWZ = re.compile(r"//\s*(lwz|stw|lwzx|stwx)\s+")
FUNC = re.compile(r"DEFINE_REX_FUNC\((\w+)\)")

hits = []
for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    func = "?"
    pending = {}          # reg -> (high value, line index)
    for i, line in enumerate(lines):
        m = FUNC.search(line)
        if m:
            func = m.group(1)
            pending.clear()
        m = LIS.search(line)
        if m:
            reg, imm = m.group(1), int(m.group(2))
            pending[reg] = ((imm << 16) & 0xFFFFFFFF, i)
            continue
        for rx in (ADDI, ORI):
            m = rx.search(line)
            if not m:
                continue
            dst, src, imm = m.group(1), m.group(2), int(m.group(3))
            if src in pending and i - pending[src][1] <= 6:
                base = pending[src][0]
                addr = (base + imm) & 0xFFFFFFFF
                if addr == TARGET:
                    # what happens next tells us read vs write
                    nxt = " | ".join(l.strip() for l in lines[i + 1:i + 5]
                                     if l.strip().startswith("//"))
                    hits.append((os.path.basename(path), i + 1, func, line.strip(), nxt))
                pending[dst] = (addr, i)

print("code sites forming 0x%08X: %d" % (TARGET, len(hits)))
for f, ln, func, line, nxt in hits:
    print("\n  %s:%d  in %s" % (f, ln, func))
    print("    %s" % line)
    print("    next: %s" % nxt[:150])
