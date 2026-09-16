#!/usr/bin/env python3
"""Post-process one XenosRecomp HLSL file for the Fable II native path.

    fix_hlsl.py <file.hlsl> <file.hlsl.layout>

1. Unnamed boolean constants (`if (b132 != 0)`) become `NGPU_BOOL(132)` and
   loop counts (`i0.x`) become `NGPU_LOOP(0).x` - both read the per-draw
   shared constants (c34..c43) that fable2_shader_common.h declares. Plain
   `#define b132` would also rewrite the `register(b1, space4)` bindings.
2. Integer-valued 8/16-bit inputs (fetch formats 6, 25, 26 with the integer
   number format) that the recompiler declares as float4 are fed by the
   runtime as UNORM (D3D12 has no integer-to-float input conversion); the
   scale back to the integer values is inserted at the top of main().
"""
import os
import re
import sys

# recompiler usage name -> input variable name
USAGE_VAR = {"BLENDINDICES": "BlendIndices", "BLENDWEIGHT": "BlendWeight", "POSITION": "Position", "NORMAL": "Normal", "TEXCOORD": "TexCoord", "COLOR": "Color", "TANGENT": "Tangent", "BINORMAL": "Binormal"}


def main():
    hlsl_path, layout_path = sys.argv[1], sys.argv[2]
    text = open(hlsl_path, encoding="utf-8").read()
    original = text

    text = re.sub(r"\(b(\d+) (!=|==) 0\)", r"(NGPU_BOOL(\1) \2 0)", text)
    text = re.sub(r"\bi(\d+)\.x\b", r"NGPU_LOOP(\1).x", text)

    # A fetch whose format is a float one, declared by the recompiler as
    # uint4 because of its usage (BLENDINDICES): the shader uses the values as
    # floats, so the declaration follows the data.
    for line in open(layout_path, encoding="utf-8").read().splitlines() if os.path.exists(layout_path) else []:
        f = line.split()
        if len(f) < 13 or f[0] != "vfetch" or f[12] != "uint4":
            continue
        if int(f[6]) not in (31, 32, 36, 37, 38, 57):
            continue
        text = text.replace("in uint4 i%s%s : %s%s" % (USAGE_VAR.get(f[1], f[1]), f[2], f[1], f[2]),
                            "in float4 i%s%s : %s%s" % (USAGE_VAR.get(f[1], f[1]), f[2], f[1], f[2]))

    scales = []
    try:
        layout = open(layout_path, encoding="utf-8").read().splitlines()
    except OSError:
        layout = []
    for line in layout:
        f = line.split()
        # vfetch SEM idx stream off stride fmt num comp sgn swz mini type
        if len(f) < 13 or f[0] != "vfetch":
            continue
        sem, idx, fmt, num, typ = f[1], f[2], int(f[6]), int(f[7]), f[12]
        if num != 1 or typ != "float4" or fmt not in (6, 25, 26):
            continue
        m = re.search(r"in float4 (\w+) : " + re.escape(sem) + idx + r"\b", text)
        if not m:
            continue
        scales.append((m.group(1), 255.0 if fmt == 6 else 65535.0))
    if scales:
        m = re.search(r"void main\(.*?\n\{\n", text, re.S)
        if m:
            ins = "".join(f"\t{var} *= {scale};\n" for var, scale in scales)
            text = text[: m.end()] + ins + text[m.end():]

    if text != original:
        open(hlsl_path, "w", encoding="utf-8", newline="\n").write(text)


if __name__ == "__main__":
    main()
