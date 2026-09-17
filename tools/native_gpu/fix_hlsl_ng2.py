#!/usr/bin/env python3
"""Second post-processing pass for NG2's XenosRecomp HLSL. Run AFTER fix_hlsl.py.

    fix_hlsl_ng2.py <file.hlsl>

Kept separate from fix_hlsl.py deliberately: that file is the Fable II side's and
is shared, and these two fixes address a population NG2 has and Fable largely
does not. Nothing here changes a shader that already compiles.

1. VERTEX INPUTS THE BODY FETCHES BUT main() NEVER DECLARES.
   XenosRecomp builds main()'s input list from the container's vertex definition
   table. A shader whose attributes come from explicit `vfetch` instructions has
   no such table, so the recompiler emits a main() taking only SV_VertexID and a
   body that reads `iPosition0` anyway - 960 references across NG2's set, plus
   iBinormal0 and iDepth8.

   This is the majority case for NG2 rather than a corner: two thirds of its
   draws are auto-index (M4_ng2_pm4_census.md section 2), and an auto-index draw
   has no input layout to declare - the shader fetches what it wants. So the
   shaders that fail to compile are largely the same population that makes NG2's
   frame unlike Fable II's, which is why this pass is needed here and was not
   there.

   The fix declares each referenced-but-undeclared input as a float4 with the
   semantic its name implies (iPosition0 -> POSITION0), matching the form the
   recompiler emits when it DOES have a table. The runtime binds these from the
   draw's fetch constants either way.

2. GPRs PAST THE DECLARED COUNT (r32..r36).
   The recompiler sizes the register array from SQ_PROGRAM_CNTL's register count
   and the microcode then uses more. Widening the declaration is safe - unused
   registers cost nothing - and is strictly better than the shader not compiling.
   Reported, not silent, because a shader needing more GPRs than its own program
   control declares is worth knowing about.
"""
import re
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8", errors="replace").read()
orig = src

# --- 1. undeclared vertex inputs -------------------------------------------
# The recompiler names inputs i<Usage><N> with the usage CAPITALISED, so the
# character after the i must be upper case. Matching `i[A-Za-z]+\d+` instead
# also matches HLSL's own vector types - int2, int3, int4 - and declaring
# `in float4 int4 : NT4` is a syntax error, which is exactly what it produced
# on the first run.
IDENT = r"\bi[A-Z][A-Za-z]*\d+\b"
declared = set(re.findall(r"\bin\s+float4\s+(" + IDENT[2:-2] + r")\s*:", src))
declared |= set(re.findall(r"\bin\s+\w+\s+(i[A-Z][A-Za-z]*\d+)\s*:", src))
used = set(re.findall(IDENT, src))
# iVertexId/iInstanceId are system values the recompiler declares its own way.
missing = sorted(u for u in used - declared
                 if not u.startswith(("iVertexId", "iInstanceId", "iIndex")))

if missing:
    m = re.search(r"void main\(\n", src)
    if m:
        # Existing vk::location bindings decide the next free slot.
        locs = [int(x) for x in re.findall(r"vk::location\((\d+)\)", src)]
        nxt = (max(locs) + 1) if locs else 0
        lines = []
        for name in missing:
            usage = re.match(r"i([A-Za-z]+)(\d+)", name)
            sem = usage.group(1).upper() + usage.group(2)
            lines.append("\t[[vk::location(%d)]] in float4 %s : %s,\n" % (nxt, name, sem))
            nxt += 1
        src = src[:m.end()] + "".join(lines) + src[m.end():]

# --- 2. GPRs past the declared count ---------------------------------------
rmax = max([int(x) for x in re.findall(r"\br(\d+)\b\.", src)] or [0])
mdecl = re.search(r"float4 r\[(\d+)\];", src)
widened = None
if mdecl and rmax >= int(mdecl.group(1)):
    widened = (int(mdecl.group(1)), rmax + 1)
    src = src.replace(mdecl.group(0), "float4 r[%d];" % (rmax + 1), 1)
else:
    # The usual form is one initialised declaration per register:
    #     float4 r0 = float4(float(iVertexId), 0.0, 0.0, 0.0);
    #     float4 r1 = 0.0;
    # so the new ones are appended after the LAST of those, where they are in
    # scope for the body and initialised the same way.
    decl_regs = set(int(x) for x in re.findall(r"\bfloat4 r(\d+)\s*=", src))
    used_regs = set(int(x) for x in re.findall(r"\br(\d+)\s*[.\[]", src))
    extra = sorted(used_regs - decl_regs)
    if extra:
        last = None
        for m2 in re.finditer(r"\n\tfloat4 r\d+\s*=[^;]*;", src):
            last = m2
        if last:
            widened = (len(decl_regs), len(decl_regs) + len(extra))
            add = "".join("\n\tfloat4 r%d = 0.0;" % r for r in extra)
            src = src[:last.end()] + add + src[last.end():]

if src != orig:
    open(path, "w", encoding="utf-8", newline="").write(src)
    bits = []
    if missing:
        bits.append("declared %d fetched input(s): %s" % (len(missing), ", ".join(missing)))
    if widened:
        bits.append("widened GPRs %d -> %d" % widened)
    print("%s: %s" % (path, "; ".join(bits)))
