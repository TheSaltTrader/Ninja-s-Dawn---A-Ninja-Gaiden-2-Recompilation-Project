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
import re
import sys


def main():
    hlsl_path, layout_path = sys.argv[1], sys.argv[2]
    text = open(hlsl_path, encoding="utf-8").read()
    original = text

    text = re.sub(r"\(b(\d+) (!=|==) 0\)", r"(NGPU_BOOL(\1) \2 0)", text)
    text = re.sub(r"\bi(\d+)\.x\b", r"NGPU_LOOP(\1).x", text)

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
