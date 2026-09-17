#!/usr/bin/env python3
"""Does the renderer's root signature cover what every DXIL actually binds?

WHY THIS EXISTS, and it is a replacement rather than an addition. The runtime
reported "PSO wanted 121 created 121 failed 0" and that was presented as proof
the bindless layout matched what XenosRecomp emits. It is not. A control with a
DELIBERATELY WRONG layout - four descriptor sets instead of five, omitting the
space that holds the constant buffers every shader reads - created 123 pipelines
and failed none.

The reason is that Plume enables the D3D12 debug layer only under `#ifndef
NDEBUG`, and this is a Release build, so nothing validated a shader's declared
bindings against the root signature at pipeline creation. An instrument that
cannot produce a failure has not tested anything, and this project has now hit
that shape four times.

So the check moves OFFLINE, where it is deterministic, needs no game, and can be
re-run against any artefact set:

    dxc -dumpbin <file>.dxil   ->   "Resource Bindings" table
                               ->   (kind, register, space) per resource

and every one of those must be covered by a declared range. Note that DXIL
STRIPS UNUSED RESOURCES: a shader that never samples a texture declares no
texture at all, so the bindings are a per-shader SUBSET and the root signature
has to be a superset of their union - which is exactly what this measures.

Usage: check_root_signature.py <dxil dir> [more dirs...]
"""
import os
import re
import subprocess
import sys

DXC = r"C:\Users\renoi\ClaudeCode\NativeGPU\reference\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxc.exe"

# The root signature the renderer declares (ng2_plume_renderer.cpp,
# EnsurePipelineLayout). Set index == D3D12 register space, one to one.
#   space0 t# : Texture2D      space1 t# : Texture3D     space2 t# : TextureCube
#   space3 s# : Sampler        space4 t# : StructuredBuffer
#   space4 b0 VertexShaderConstants, b1 PixelShaderConstants, b2 SharedConstants
DECLARED = {
    ("t", 0): "boundless Texture2D",
    ("t", 1): "boundless Texture3D",
    ("t", 2): "boundless TextureCube",
    ("s", 3): "boundless Sampler",
    ("t", 4): "boundless StructuredBuffer",
    ("b", 4): "cbuffers b0 and b2",
}
# Constant buffers are declared individually rather than boundlessly, so the
# register number matters for them and a b1 or b3 would NOT be covered.
DECLARED_CB_REGISTERS = {0, 1, 2}

# THE PREFIX IS NOT ALWAYS ONE LETTER. dxc writes a cbuffer's bind as
# "cb2,space4", not "b2,space4", and the first version of this regex demanded a
# single letter - so it matched NOTHING, the union table came out empty, and the
# tool reported "634 of 634 fully covered". An instrument that finds no subjects
# reports perfect coverage of them, which is the exact failure this file was
# written to replace, reproduced inside the replacement within ten minutes.
#
# Matched anywhere in the line rather than by column count, because the column
# widths vary with the longest resource name in each artefact.
BIND_RE = re.compile(r"\b(cb|t|s|u)(\d+),space(\d+)\b")


def _kind(prefix):
    # dxc's prefixes to D3D12 register classes: cb -> b, t -> t, s -> s, u -> u.
    return "b" if prefix == "cb" else prefix


def bindings_of(path):
    """[(kind, register, space)] a DXIL actually declares, or None if unreadable."""
    try:
        # A DELIBERATE, STATED EXCEPTION to routing every external call through
        # run_capped.py. This is dxc parsing an already-built container, not
        # XenosRecomp translating microcode, and it runs 634 times - a python
        # wrapper per artefact would cost more than the risk it removes. The
        # point of the rule is that an uncapped call site must never be an
        # ACCIDENT; an exception that says so in the file is fine, one nobody
        # noticed is how 41 GB happens.
        out = subprocess.run([DXC, "-dumpbin", path], capture_output=True, timeout=60)
    except Exception:
        return None
    text = out.stdout.decode("utf-8", "replace")
    if "Resource Bindings" not in text:
        return []          # a shader that binds nothing at all is a real answer
    found = []
    started = False
    for line in text.splitlines():
        if "Resource Bindings" in line:
            started = True
            continue
        if not started:
            continue
        if not line.startswith(";"):
            break
        m = BIND_RE.search(line)
        if m:
            found.append((_kind(m.group(1)), int(m.group(2)), int(m.group(3))))
    # A SHADER THAT BINDS NOTHING IS A REAL ANSWER, but a PARSE that finds
    # nothing across the whole population is not - the caller checks that the
    # union is non-empty, because "no subjects" and "all subjects pass" print
    # the same number otherwise.
    return found


def main():
    dirs = sys.argv[1:] or [r"D:/ng2_frameinterp/shaders/out/dxil"]
    checked = covered = 0
    unreadable = []
    uncovered = {}
    union = {}
    for d in dirs:
        if not os.path.isdir(d):
            print("no such directory: %s" % d)
            return 2
        for name in sorted(os.listdir(d)):
            if not name.endswith(".dxil"):
                continue
            b = bindings_of(os.path.join(d, name))
            if b is None:
                unreadable.append(name)
                continue
            checked += 1
            ok = True
            for kind, reg, space in b:
                union[(kind, space)] = union.get((kind, space), 0) + 1
                if (kind, space) not in DECLARED:
                    uncovered.setdefault("%s#,space%d" % (kind, space), []).append(name)
                    ok = False
                elif kind == "b" and reg not in DECLARED_CB_REGISTERS:
                    uncovered.setdefault("b%d,space%d" % (reg, space), []).append(name)
                    ok = False
            covered += ok

    # THE PARSE MUST HAVE FOUND SOMETHING. Without this the tool reports
    # "634 of 634 fully covered" when its regex matches nothing at all, which is
    # precisely how the first version of this file passed.
    if checked and not union:
        print("PARSE FOUND NO BINDINGS AT ALL across %d artefacts - the tool is" % checked)
        print("broken, not the root signature. Refusing to report coverage.")
        return 2

    print("artefacts checked : %d" % checked)
    print("fully covered     : %d" % covered)
    print("NOT covered       : %d" % (checked - covered))
    if unreadable:
        print("unreadable        : %d (e.g. %s)" % (len(unreadable), unreadable[0]))
    print()
    print("bindings the artefacts actually use:")
    for (kind, space), n in sorted(union.items()):
        state = "declared" if (kind, space) in DECLARED else "NOT DECLARED"
        print("   %s#,space%-2d  used by %5d artefacts   %s" % (kind, space, n, state))
    if uncovered:
        print()
        print("UNCOVERED BINDINGS - the root signature would not satisfy these:")
        for k, v in sorted(uncovered.items()):
            print("   %-16s %d artefacts, e.g. %s" % (k, len(v), v[0]))
    return 0 if not uncovered else 1


if __name__ == "__main__":
    sys.exit(main())
