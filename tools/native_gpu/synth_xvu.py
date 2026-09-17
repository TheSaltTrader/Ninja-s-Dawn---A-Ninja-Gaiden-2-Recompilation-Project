"""Synthetic XenosRecomp container, WITH a derived vertex element table.

The first version stopped at the 24-byte Shader struct. A vertex shader header
is a VertexShader, which extends it:

    struct VertexShader : Shader {
        field18; vertexElementCount; field20;
        vertexElementsAndInterpolators[];   // elements at [field18 + i],
    };                                      // interpolators after them
    struct VertexElement { address : 12; usage : 4; usageIndex : 4; };
    struct Interpolator  { usageIndex : 4; usage : 4; };

XenosRecomp looks every vertex fetch up in that array BY INSTRUCTION ADDRESS and
asserts when the lookup misses. Without it the recompiler reads the constant
table as the array - which is where the non-deterministic output and the random
segfaults came from: a program with correct arithmetic wired to random inputs.

The addresses are recoverable from the microcode (vfetch_scan), so the table can
be derived rather than guessed. The NAMES cannot be, and do not need to be: the
native path binds vertex buffers from fetch constants and builds its input
layout from this same translation's sidecar, so the semantics only have to be
unique and self-consistent.
"""
import struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vfetch_scan import scan

USAGE_TEXCOORD = 5


def build(microcode, is_vertex=True, pad=0, n_interpolators=8, preamble=None):
    """preamble: the 64 bytes that sit immediately BEFORE the code in guest
    memory. A real container keeps them at physical offset 0 with the code at
    64, and points a definition table at them: they are the shader's literal
    constants, c252..c255 for a vertex shader. Without them a shader that needs
    1.0 computes with 0.0, which renders rather than fails."""
    names = ["g_Consts"]
    names += ["g_Sampler%d" % i for i in range(16)]
    names += ["g_Bool%d" % i for i in range(32)]
    n = len(names)

    fetches = scan(microcode) if is_vertex else []
    n_elems = len(fetches)

    HEADER = 0x24
    # A definition table, when the literal constants are available.
    DEF_HEADER = 20            # DefinitionTable's five header dwords
    DEF_BYTES = DEF_HEADER + 8 + 4 + 4 if preamble else 0   # one Float4Definition, then both terminators
    shader_off = HEADER
    # A vertex shader header is Shader(24) + field18/count/field20(12) + the
    # element and interpolator array. A PIXEL shader header is its own struct -
    #   struct PixelShader : Shader { field18; outputs; interpolators[]; }
    # - and emitting only the 24-byte base left the recompiler reading the
    # constant table as its interpolator list, which produced HLSL referencing
    # inputs it never declared. The vertex bug, one population over.
    if is_vertex:
        shader_bytes = 24 + 12 + (n_elems + n_interpolators) * 4
    else:
        shader_bytes = 24 + 8 + n_interpolators * 4
    def_off = shader_off + shader_bytes
    ctc_off = def_off + DEF_BYTES
    ct_off = ctc_off + 4
    ct_size = 28
    info_off = ct_size
    info_bytes = n * 20

    name_at = []
    cursor = info_off + info_bytes
    for nm in names:
        name_at.append(cursor)
        cursor += len(nm) + 1

    vsize = (ct_off + cursor + 15) & ~15
    pre = bytes(preamble) if preamble else b""
    psize = len(pre) + len(microcode) + pad
    out = bytearray(vsize + psize)

    def p32(at, x):
        struct.pack_into(">I", out, at, x & 0xFFFFFFFF)

    def p16(at, x):
        struct.pack_into(">H", out, at, x & 0xFFFF)

    p32(0x00, 0x102A1100 | (1 if is_vertex else 0))
    p32(0x04, vsize)
    p32(0x08, psize)
    p32(0x0C, 0)
    p32(0x10, ctc_off)
    p32(0x14, def_off if preamble else 0)
    p32(0x18, shader_off)
    p32(0x1C, 0)
    p32(0x20, 0)

    p32(shader_off + 0x00, len(pre))               # physicalOffset: the code starts after the literals
    p32(shader_off + 0x04, len(microcode))         # size: the real program, never the padding
    p32(shader_off + 0x08, 0)
    p32(shader_off + 0x0C, 0)                      # svPos register 0
    p32(shader_off + 0x10, 0)
    p32(shader_off + 0x14, n_interpolators << 5)   # interpolatorInfo

    if is_vertex:
        p32(shader_off + 0x18, 0)                  # field18: elements start at [0]
        p32(shader_off + 0x1C, n_elems)            # vertexElementCount
        p32(shader_off + 0x20, n_interpolators)    # field20
        arr = shader_off + 0x24
        for i, addr in enumerate(fetches):
            # address:12 | usage:4 | usageIndex:4 - unique per element, which is
            # all the recompiler needs to declare each input once.
            p32(arr + i * 4, (addr & 0xFFF) | (USAGE_TEXCOORD << 12) | ((i & 0xF) << 16))
        for i in range(n_interpolators):
            # Interpolator: usageIndex:4 | usage:4 | reg:4. The reg is which
            # shader register it lands in; leaving it zero put every
            # interpolator in r0.
            p32(arr + (n_elems + i) * 4, (i & 0xF) | (USAGE_TEXCOORD << 4) | ((i & 0xF) << 8))
    else:
        p32(shader_off + 0x18, 0)                  # field18
        p32(shader_off + 0x1C, 1)                  # outputs: COLOR0
        arr = shader_off + 0x20
        # The pixel shader reads what the vertex shader writes, so the two
        # interpolator lists have to agree: TexCoord0..N on both sides.
        for i in range(n_interpolators):
            p32(arr + i * 4, (i & 0xF) | (USAGE_TEXCOORD << 4) | ((i & 0xF) << 8))

    p32(ctc_off, ct_size + info_bytes + (cursor - (info_off + info_bytes)))
    p32(ct_off + 0x00, ct_size)
    p32(ct_off + 0x04, 0)
    p32(ct_off + 0x08, 0)
    p32(ct_off + 0x0C, n)
    p32(ct_off + 0x10, info_off)
    p32(ct_off + 0x14, 0)
    p32(ct_off + 0x18, 0)

    for i in range(n):
        at = ct_off + info_off + i * 20
        if i == 0:
            rset, idx, cnt = 2, 0, (256 if is_vertex else 224)
        elif i <= 16:
            rset, idx, cnt = 3, i - 1, 1
        else:
            rset, idx, cnt = 0, i - 17, 1
        p32(at + 0x00, name_at[i])
        p16(at + 0x04, rset)
        p16(at + 0x06, idx)
        p16(at + 0x08, cnt)
        p16(at + 0x0A, 0)
        p32(at + 0x0C, 0)
        p32(at + 0x10, 0)

    for i, nm in enumerate(names):
        at = ct_off + name_at[i]
        out[at:at + len(nm)] = nm.encode()
        out[at + len(nm)] = 0

    if preamble:
        # DefinitionTable: five header dwords, then Float4Definitions
        # (registerIndex:16, count:16, physicalOffset:32) terminated by 0, then
        # Int4Definitions terminated by 0. count is in DWORDS.
        p16(def_off + DEF_HEADER + 0, 252 if is_vertex else 252 + 256)
        p16(def_off + DEF_HEADER + 2, len(pre) // 4)
        p32(def_off + DEF_HEADER + 4, 0)           # the literals sit at physical offset 0
        p32(def_off + DEF_HEADER + 8, 0)           # end of the Float4 list
        p32(def_off + DEF_HEADER + 12, 0)          # end of the Int4 list
        out[vsize:vsize + len(pre)] = pre

    out[vsize + len(pre):vsize + len(pre) + len(microcode)] = microcode
    return bytes(out)
