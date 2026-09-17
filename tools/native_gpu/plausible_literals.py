#!/usr/bin/env python3
"""Are the 64 bytes before a shader's code its literal constants, or someone else's?

The Fable II session found that a real container keeps a shader's literal
constants in the physical region immediately BEFORE the program, and that the
same bytes sit before the block IM_LOAD names for a shader with no container -
so they can be read across. That is validated on NG2: of 53 used shaders matched
to a container, 51 have the container's preamble byte-identical in GPU memory
and none differ.

But it is only true where the container declares physicalOffset > 0, and on NG2
that is not universal:

    vertex containers   physicalOffset  0: 31   64: 378   128: 2
    pixel  containers   physicalOffset  0: 53   64: 161

84 of 625 have NO preamble. For those, the 64 bytes before the code belong to
whatever the allocator put there - and handing them over as literal constants
produces a shader that computes with garbage. Deterministic and wrong, which is
the failure mode reading the literals was supposed to close.

An uncontained shader has no physicalOffset to consult, so the bytes have to
speak for themselves. Real literal blocks are IEEE floats or zero; the rejects
measured on NG2 look like microcode operands (00002104, 0000210D) or unrelated
data (900DED58, AF0D9BFC).

This is a HEURISTIC, not a decode, and it is deliberately conservative: a
rejected preamble costs the shader its literals (which is where it already was),
while a wrongly accepted one silently corrupts every constant it uses.
"""
import struct


def plausible(preamble: bytes) -> bool:
    if not preamble or len(preamble) != 64:
        return False
    dw = struct.unpack(">16I", preamble)
    for v in dw:
        if v == 0:
            continue
        f = struct.unpack(">f", struct.pack(">I", v))[0]
        if f != f or f in (float("inf"), float("-inf")):   # NaN / Inf
            return False
        a = abs(f)
        # Shader literals are ordinary numbers - 1.0, 0.5, 4.0, 16.0, 0.25.
        # Denormals and astronomically large values are not constants, they are
        # whatever happened to be in memory.
        if a < 1e-6 or a > 1e6:
            return False
    return True


if __name__ == "__main__":
    import sys
    b = open(sys.argv[1], "rb").read()[:64]
    print("plausible" if plausible(b) else "REJECT")
