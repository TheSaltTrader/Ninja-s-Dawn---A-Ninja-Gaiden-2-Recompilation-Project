#!/usr/bin/env python3
"""Post-process one XenosRecomp HLSL file for the Fable II native path.

    fix_hlsl.py <file.hlsl> <file.hlsl.layout>

1. Unnamed boolean constants (`if (b132 != 0)`) become `NGPU_BOOL(132)` and
   loop counts (`i0.x`) become `NGPU_LOOP(0).x` - both read the per-draw
   shared constants (c34..c43) that fable2_shader_common.h declares. Plain
   `#define b132` would also rewrite the `register(b1, space4)` bindings.
2. An attribute fetched in a float format but declared by its usage name as
   uint4 (blend indices) is redeclared float4, in the HLSL and in the
   sidecar, so the runtime's input layout agrees with the shader.
3. 16-bit attributes: the runtime byte-swaps a whole stream 8-in-32, which
   leaves the input assembler reading each pair of halves backwards - it
   takes the low half as component 0 while the Xenos takes the guest's
   first, now high, half. A swizzle at the top of main puts them back.
4. Integer-valued 8/16-bit inputs (fetch formats 6, 25, 26 with the integer
   number format) that the recompiler declares as float4 are fed by the
   runtime as UNORM (D3D12 has no integer-to-float input conversion); the
   scale back to the integer values is inserted at the top of main().
"""
import os
import re
import sys

# recompiler usage name -> input variable name
USAGE_VAR = {"BLENDINDICES": "BlendIndices", "BLENDWEIGHT": "BlendWeight", "POSITION": "Position", "NORMAL": "Normal", "TEXCOORD": "TexCoord", "COLOR": "Color", "TANGENT": "Tangent", "BINORMAL": "Binormal"}
FLOAT_FORMATS = (31, 32, 36, 37, 38, 57)
HALF_FORMATS = (25, 26, 31, 32)


def read_layout(path):
    try:
        return open(path, encoding="utf-8").read().splitlines()
    except OSError:
        return []


def main():
    hlsl_path, layout_path = sys.argv[1], sys.argv[2]
    text = open(hlsl_path, encoding="utf-8").read()
    original = text

    text = re.sub(r"\(b(\d+) (!=|==) 0\)", r"(NGPU_BOOL(\1) \2 0)", text)
    text = re.sub(r"\bi(\d+)\.x\b", r"NGPU_LOOP(\1).x", text)

    # 2. a float-format fetch the recompiler typed uint4 by usage: the
    #    declaration follows the data, and the sidecar records the new type
    #    because the runtime checks it against the fetch format.
    layout = read_layout(layout_path)
    layout_changed = False
    for i, line in enumerate(layout):
        f = line.split()
        if len(f) < 13 or f[0] != "vfetch" or f[12] != "uint4":
            continue
        if int(f[6]) not in FLOAT_FORMATS:
            continue
        var = USAGE_VAR.get(f[1], f[1])
        text = text.replace("in uint4 i%s%s : %s%s" % (var, f[2], f[1], f[2]),
                            "in float4 i%s%s : %s%s" % (var, f[2], f[1], f[2]))
        f[12] = "float4"
        layout[i] = " ".join(f)
        layout_changed = True
    if layout_changed:
        open(layout_path, "w", encoding="utf-8", newline="\n").write("\n".join(layout) + "\n")

    inserts = []

    # 3. 16-bit attributes, halves back in the Xenos order. A computed fetch
    #    is decoded by ngpu_vload instead, so it is left alone.
    seen = set()
    for line in layout:
        f = line.split()
        if len(f) < 13 or f[0] != "vfetch" or f[12] == "1":
            continue
        sem, idx, fmt, computed = f[1], f[2], int(f[6]), f[-1]
        if computed == "1" or fmt not in HALF_FORMATS or (sem, idx) in seen:
            continue
        m = re.search(r"in (?:float4|uint4) (\w+) : " + re.escape(sem + idx) + r"\b", text)
        if not m:
            continue
        seen.add((sem, idx))
        swizzle = ".yxwz" if fmt in (26, 32) else ".yxzw"
        inserts.append("\t%s = %s%s;\n" % (m.group(1), m.group(1), swizzle))

    # 4. integer values fed as UNORM, scaled back
    for line in layout:
        f = line.split()
        # vfetch SEM idx stream off stride fmt num comp sgn swz mini type computed
        if len(f) < 13 or f[0] != "vfetch":
            continue
        sem, idx, fmt, num, typ = f[1], f[2], int(f[6]), int(f[7]), f[12]
        if num != 1 or typ != "float4" or fmt not in (6, 25, 26):
            continue
        m = re.search(r"in float4 (\w+) : " + re.escape(sem + idx) + r"\b", text)
        if not m:
            continue
        inserts.append("\t%s *= %s;\n" % (m.group(1), "255.0" if fmt == 6 else "65535.0"))

    if inserts:
        m = re.search(r"void main\(.*?\n\{\n", text, re.S)
        if m:
            text = text[: m.end()] + "".join(inserts) + text[m.end():]

    if text != original:
        open(hlsl_path, "w", encoding="utf-8", newline="\n").write(text)


if __name__ == "__main__":
    main()
