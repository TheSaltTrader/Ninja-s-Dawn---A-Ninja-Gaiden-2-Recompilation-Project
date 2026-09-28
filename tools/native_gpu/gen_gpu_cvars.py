"""Generate src/ng2_gpu_cvars.cpp: the exe's own registration of every cvar rexgpu-xenos.dll used to define.

Removing the DLL (user direction 2026-09-27) leaves ~130 GPU cvars unregistered: config values are dropped, menu rows
go dead and by-name readers (the vendored Plugin* helpers, the runtime presenter's ng2_uw_mode) fall back silently -
gpu_offload_to_native falling back to false turns guest-memory ownership off. This copies each REXCVAR_DEFINE_*
statement VERBATIM (name, default, category, description, .allowed/.lifecycle chains) from the plugin sources of the
fork, so the registry holds exactly what the DLL registered.

Names the exe already DEFINES as accessor-only storage (`T& FLAGS_x_storage_() { static T s = ...Plugin*("x", d);
return s; }`, vendored flags.cpp and friends) are rewritten in place into the REXCVAR_DEFINE, so there is one symbol
and it is live. Names the exe defines itself with REXCVAR_DEFINE already are skipped.

Usage: gen_gpu_cvars.py [--check]   (--check: report only, write nothing)
"""
import os
import re
import sys

FORK = r"D:\ng2_frameinterp\ng2-rexglue\src\graphics"
EXE = r"D:\ng2_frameinterp\ng2recomp-worktree\src"
OUT = os.path.join(EXE, "ng2_gpu_cvars.cpp")
check = "--check" in sys.argv


def statements(text):
    """Yield (name, statement) for each REXCVAR_DEFINE_* in text, up to its ';' at paren depth 0."""
    for m in re.finditer(r"REXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|STRING)\(\s*(\w+)", text):
        i, depth, in_str, esc = m.start(), 0, False, False
        j = i
        while j < len(text):
            c = text[j]
            if not in_str and text.startswith("//", j):   # a line comment inside the chain (may hold a ';')
                j = text.find("\n", j)
                if j < 0:
                    j = len(text)
                continue
            if in_str:
                if esc:
                    esc = False
                elif c == "\\":
                    esc = True
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == ";" and depth == 0:
                break
            j += 1
        yield m.group(2), text[i:j + 1]


fork = {}
for root, _, files in os.walk(FORK):
    if os.sep + "vulkan" in root:
        continue   # the Vulkan backend is not NG2's; its cvars never reach the D3D12 path
    for f in files:
        if f.endswith(".cpp"):
            p = os.path.join(root, f)
            for name, st in statements(open(p, encoding="utf-8", errors="replace").read()):
                if name in fork and fork[name][1] != st:
                    print("WARNING: %s defined twice in the fork (%s, %s)" % (name, fork[name][0], p))
                fork.setdefault(name, (p, st))

# The exe's own definitions: REXCVAR_DEFINE (skip) and accessor-only storage (rewrite in place).
exe_defined, accessors = set(), {}
acc_re = re.compile(r"^[^\n]*?\b[\w:]+&\s+FLAGS_(\w+)_storage_\(\)\s*\{[^\n]*\n", re.M)
for root, _, files in os.walk(EXE):
    for f in files:
        if not f.endswith((".cpp", ".h")) or f == "ng2_gpu_cvars.cpp":
            continue
        p = os.path.join(root, f)
        t = open(p, encoding="utf-8", errors="replace").read()
        for name, _ in statements(t):
            exe_defined.add(name)
        for m in acc_re.finditer(t):
            if m.group(1) in fork and "Plugin" in m.group(0):
                accessors.setdefault(p, []).append((m.group(1), m.group(0)))
            elif m.group(1) in fork:
                print("NOTE: %s has a non-Plugin storage definition in %s - left alone, not registered" % (m.group(1), p))
                exe_defined.add(m.group(1))

in_place = {n for lst in accessors.values() for n, _ in lst}
new = [n for n in sorted(fork) if n not in exe_defined and n not in in_place]
print("fork cvars (D3D12/common): %d; already exe-defined: %d; rewritten in place: %d; new in %s: %d" % (
    len(fork), len(exe_defined & set(fork)), len(in_place), os.path.basename(OUT), len(new)))
if check:
    for p, lst in accessors.items():
        print("  in place %s: %s" % (os.path.relpath(p, EXE), ", ".join(n for n, _ in lst)))
    sys.exit(0)

for p, lst in accessors.items():
    s = open(p, encoding="utf-8", newline="").read()
    crlf = "\r\n" in s
    for name, line in lst:
        old = line.replace("\n", "\r\n") if crlf else line
        st = fork[name][1].replace("\r\n", "\n")
        rep = "// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)\n" + st + "\n"
        if crlf:
            rep = rep.replace("\n", "\r\n")
        assert s.count(old) == 1, (p, name)
        s = s.replace(old, rep)
    if "#include <rex/cvar.h>" not in s:
        s = ("#include <rex/cvar.h>\r\n" if crlf else "#include <rex/cvar.h>\n") + s
    open(p, "w", encoding="utf-8", newline="").write(s)
    print("rewrote %d accessors in %s" % (len(lst), os.path.relpath(p, EXE)))

with open(OUT, "w", encoding="utf-8", newline="\n") as o:
    o.write("// GENERATED by tools/native_gpu/gen_gpu_cvars.py - do not edit by hand.\n")
    o.write("// The GPU cvars rexgpu-xenos.dll used to register, copied verbatim from the fork's plugin sources\n")
    o.write("// (D:/ng2_frameinterp/ng2-rexglue src/graphics, Vulkan excluded) so the exe owns them now the DLL is gone.\n")
    o.write("#include <cstdint>\n#include <string>\n#include <rex/cvar.h>\n\n")
    for n in new:
        o.write("// from %s\n%s\n\n" % (os.path.relpath(fork[n][0], FORK).replace("\\", "/"), fork[n][1].replace("\r\n", "\n")))
print("wrote %s (%d cvars)" % (OUT, len(new)))
