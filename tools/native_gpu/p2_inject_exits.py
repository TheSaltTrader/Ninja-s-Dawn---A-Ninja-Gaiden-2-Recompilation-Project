#!/usr/bin/env python3
"""P2 census: inject the EXIT hook into the generated code of every hooked Direct3D entry point.

The entry hooks (config/hooks/native_gpu_trace.toml) were emitted by the codegen at each function's first
instruction; the census also needs the exit, and 55 of the 108 functions have no `blr` (they leave by tail calls), so
the reliable exit point is every `return;` of the recompiled body. This script inserts `ng2_p2_exit(<hook index>);`
before each `return;` inside `DEFINE_REX_FUNC(sub_XXXXXXXX)` for the 108 addresses of src/native_gpu_trace.cpp
(the index is that table's order, the one ng2::p2::Enter receives), and one `extern "C" void ng2_p2_exit(int);`
per touched file. Idempotent; --revert removes both again. The generated tree is untracked (pinned from the main
repo), so this is a build-local change: run it before building a census exe, --revert before a release build.

Usage: p2_inject_exits.py [--revert] [--root D:/ng2_frameinterp/ng2recomp-worktree]"""
import glob
import os
import re
import sys

root = r"D:/ng2_frameinterp/ng2recomp-worktree"
if "--root" in sys.argv:
    root = sys.argv[sys.argv.index("--root") + 1]
revert = "--revert" in sys.argv
DECL = 'extern "C" void ng2_p2_exit(int);   // P2 census exit hook (tools/native_gpu/p2_inject_exits.py)\n'

trace = open(os.path.join(root, "src", "native_gpu_trace.cpp"), encoding="utf-8").read()
addrs = [int(m.group(1), 16) for m in re.finditer(r"\{0x([0-9A-F]{8}), \"", trace)]
index = {a: i for i, a in enumerate(addrs)}
assert len(addrs) == 108, len(addrs)

touched_files = 0
inserted = 0
removed = 0
found = set()
for path in sorted(glob.glob(os.path.join(root, "generated", "default", "ng2_recomp.*.cpp"))):
    t = open(path, encoding="utf-8", errors="strict").read()
    orig = t
    if revert:
        t2 = re.sub(r"\tng2_p2_exit\(\d+\);   // P2\n", "", t)
        removed += t.count("ng2_p2_exit(") - t2.count("ng2_p2_exit(")
        t2 = t2.replace(DECL, "")
        t = t2
    else:
        for a in addrs:
            key = "DEFINE_REX_FUNC(sub_%08X) {" % a
            i = t.find(key)
            if i < 0:
                continue
            found.add(a)
            j = t.find("\n}\n", i)
            body = t[i:j]
            if "ng2_p2_exit(" in body:
                continue
            marker = "\tng2_p2_exit(%d);   // P2\n" % index[a]
            new_body = body.replace("\treturn;\n", marker + "\treturn;\n")
            inserted += new_body.count("ng2_p2_exit(")
            t = t[:i] + new_body + t[j:]
        if "ng2_p2_exit(" in t and DECL not in t:
            # after the file's include line
            k = t.find("\n", t.find("#include")) + 1
            t = t[:k] + DECL + t[k:]
    if t != orig:
        open(path, "w", encoding="utf-8", newline="\n").write(t)
        touched_files += 1

if revert:
    print("reverted: %d exit calls removed from %d files" % (removed, touched_files))
else:
    missing = [hex(a) for a in addrs if a not in found]
    print("inserted %d exit calls into %d files; functions found %d of %d%s" %
          (inserted, touched_files, len(found), len(addrs), ("; MISSING " + ", ".join(missing)) if missing else ""))
    if missing:
        sys.exit(2)
