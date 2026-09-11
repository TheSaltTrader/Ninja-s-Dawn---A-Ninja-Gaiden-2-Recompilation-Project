"""Census: every place the runtime can block a guest thread, and whether the
watchdog can see it.

Subjects are taken from the SOURCE - every call into a primitive that can wait
indefinitely - not from a list typed here. That is the whole point: the first
watchdog covered one wait entry out of five and its silence was then used as
evidence, which is how a blind spot becomes a conclusion.

A call is covered if a WaitMark is in scope at it, which for these files means
one appears within the enclosing function before the call.
"""
import os
import re

SDK = r"C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\rexglue-src"
FILES = [
    r"src\kernel\xboxkrnl\xboxkrnl_threading.cpp",
    r"src\kernel\xboxkrnl\xboxkrnl_rtl.cpp",
]

# Primitives that can block a guest thread until something else acts.
BLOCKERS = re.compile(
    r"\b(xeKeWaitForSingleObject|XObject::WaitMultiple|XObject::SignalAndWait|"
    r"object->Wait|rex::thread::Wait\b)\s*\(")
FUNC_START = re.compile(r"^[A-Za-z_][\w:<>,\s\*&]*\b(\w+)\s*\([^;]*$")

rows = []
for rel in FILES:
    path = os.path.join(SDK, rel)
    lines = open(path, encoding="utf-8").read().split("\n")
    func, func_line, marked = "?", 0, False
    depth = 0
    for i, line in enumerate(lines):
        stripped = line.strip()
        if depth == 0:
            m = FUNC_START.match(line)
            if m and not stripped.startswith("//"):
                func, func_line, marked = m.group(1), i + 1, False
        if "WaitMark mark(" in line:
            marked = True
        m = BLOCKERS.search(line)
        if m and not stripped.startswith("//") and not stripped.startswith("*"):
            rows.append((os.path.basename(rel), i + 1, func, m.group(1), marked))
        depth += line.count("{") - line.count("}")
        if depth < 0:
            depth = 0

covered = [r for r in rows if r[4]]
bare = [r for r in rows if not r[4]]

print("=== blocking call sites in the guest kernel ===")
print("%d sites: %d marked, %d bare\n" % (len(rows), len(covered), len(bare)))
for f, ln, func, prim, ok in rows:
    print("  %-5s %-28s %-34s %s:%d" %
          ("OK" if ok else "BARE", prim, func, f, ln))
