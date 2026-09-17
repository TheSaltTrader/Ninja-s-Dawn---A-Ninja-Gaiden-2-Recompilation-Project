"""Find every vertex-fetch instruction in a Xenos shader's microcode.

XenosRecomp looks each vfetch up by its INSTRUCTION ADDRESS in a table the
VertexShader header carries:

    struct VertexShader : Shader {
        field18; vertexElementCount; field20;
        vertexElementsAndInterpolators[];   // elements at [field18 + i]
    };
    struct VertexElement { address : 12; usage : 4; usageIndex : 4; };

and asserts when the lookup misses. A synthetic container whose header stops at
the 24-byte Shader struct makes it read the constant table as that array, which
is where the non-deterministic semantics and the random crashes come from.

The addresses are recoverable: walk the control flow, and inside each exec block
the sequence bits say which instructions are fetches.
"""
import struct

EXEC_LIKE = {1, 2, 3, 4, 5, 6, 13, 14}
END_OPS = {2, 4, 6, 14}


def scan(code):
    """Instruction addresses of every vertex fetch, in program order."""
    n = len(code) // 4
    dw = struct.unpack(">%dI" % n, code[: n * 4])

    def cf_pairs():
        for i in range(0, n - 2, 3):
            d0, d1, d2 = dw[i], dw[i + 1], dw[i + 2]
            yield d0 | ((d1 & 0xFFFF) << 32)
            yield ((d1 >> 16) | ((d2 & 0xFFFF) << 16)) | ((d2 >> 16) << 32)

    cf_limit = len(code)
    fetches = []
    seen = set()
    for idx, v in enumerate(cf_pairs()):
        if (idx // 2) * 12 >= cf_limit:
            break
        op = (v >> 44) & 0xF
        if op in EXEC_LIKE:
            addr = v & 0xFFF
            count = (v >> 12) & 0x7
            sequence = (v >> 16) & 0xFFF
            if addr:
                cf_limit = min(cf_limit, addr * 12)
            for i in range(count):
                if sequence & 1:
                    at = (addr + i) * 3
                    if at + 2 < n:
                        # FetchOpcode::VertexFetch is 0, in the low 5 bits
                        if (dw[at] & 0x1F) == 0 and (addr + i) not in seen:
                            seen.add(addr + i)
                            fetches.append(addr + i)
                sequence >>= 2
        if op in END_OPS:
            break
    return fetches
