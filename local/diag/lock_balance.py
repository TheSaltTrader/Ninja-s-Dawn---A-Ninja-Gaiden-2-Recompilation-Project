"""Is every critical-section acquire released on every path?

sub_822F3A88 runs immediately before the main side starts spinning for the
worker. If any path through it enters a critical section and returns without
leaving, the spinner holds that lock for the whole spin - and if the worker
needs it, neither side can ever move. That is the deadlock shape the evidence
points at: one thread at 100%, one at 0%, and no kernel wait on the spinner.

Counting Enter against Leave per function is a coarse test - it cannot follow
branches - but an imbalance is a fact worth having, and a function with more
enters than leaves is exactly where to look next.
"""
import glob
import os
import re

GEN = (r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
       r"\generated\default")
DEF = re.compile(r"DEFINE_REX_FUNC\((\w+)\)")
CALL = re.compile(r"^\t(sub_[0-9A-F]{8})\(ctx, base\);", re.M)

bodies = {}
for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    text = open(path, encoding="utf-8", errors="replace").read()
    parts = DEF.split(text)
    for i in range(1, len(parts) - 1, 2):
        bodies[parts[i]] = parts[i + 1]


def counts(name):
    b = bodies.get(name, "")
    return (b.count("__imp__RtlEnterCriticalSection"),
            b.count("__imp__RtlLeaveCriticalSection"),
            b.count("__imp__RtlTryEnterCriticalSection"),
            b.count("return;"))


# The functions on the path into the spin, and the worker's own entry.
seen = set()
order = []


def walk(name, depth=0):
    if depth > 3 or name in seen:
        return
    seen.add(name)
    order.append((depth, name))
    for c in sorted(set(CALL.findall(bodies.get(name, "")))):
        walk(c, depth + 1)


for root in ("sub_822F3A88", "sub_822F35A0", "sub_822F3708"):
    walk(root)

print("=== enter/leave balance on the pre-spin path ===")
print("%-14s %6s %6s %6s %8s" % ("function", "enter", "leave", "try", "returns"))
flagged = []
for depth, name in order:
    e, l, t, r = counts(name)
    if e or l or t:
        mark = "" if e == l else "   <-- UNBALANCED"
        if e != l:
            flagged.append((name, e, l))
        print("%-14s %6d %6d %6d %8d%s" % ("  " * depth + name, e, l, t, r, mark))

print("\nfunctions where enters != leaves: %d" % len(flagged))
for name, e, l in flagged:
    print("  %s: %d enter, %d leave" % (name, e, l))
