"""Build a synthetic XenosRecomp container around raw microcode.

A Python twin of synth::BuildContainer in native_gpu_present.cpp, so the header
can be iterated against the real recompiler offline instead of one game run at
a time. Keep the two in step: this is the one that gets experimented on.
"""
import struct, sys


def build(microcode: bytes, is_vertex: bool = True, pad: int = 0) -> bytes:
    names = ["g_Consts"]
    names += ["g_Sampler%d" % i for i in range(16)]
    names += ["g_Bool%d" % i for i in range(32)]
    n = len(names)

    HEADER = 0x24
    SHADER_STRUCT = 24
    shader_off = HEADER
    ctc_off = shader_off + SHADER_STRUCT
    ct_off = ctc_off + 4              # offsets inside the table are relative to here
    ct_size = 28
    info_off = ct_size
    info_bytes = n * 20

    name_at = []
    cursor = info_off + info_bytes
    for nm in names:
        name_at.append(cursor)
        cursor += len(nm) + 1

    vsize = (ct_off + cursor + 15) & ~15
    psize = len(microcode) + pad
    out = bytearray(vsize + psize)

    def p32(at, x):
        struct.pack_into('>I', out, at, x)

    def p16(at, x):
        struct.pack_into('>H', out, at, x)

    p32(0x00, 0x102A1100 | (1 if is_vertex else 0))
    p32(0x04, vsize)
    p32(0x08, psize)
    p32(0x0C, 0)
    p32(0x10, ctc_off)
    p32(0x14, 0)                      # no definition table
    p32(0x18, shader_off)
    p32(0x1C, 0)
    p32(0x20, 0)

    p32(shader_off + 0x00, 0)         # physicalOffset
    p32(shader_off + 0x04, psize)     # size
    p32(shader_off + 0x08, 0)
    p32(shader_off + 0x0C, 0)         # svPos register 0
    p32(shader_off + 0x10, 0)
    p32(shader_off + 0x14, 16 << 5)   # interpolator count 16

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
            rset, idx, cnt = 2, 0, (256 if is_vertex else 224)   # Float4
        elif i <= 16:
            rset, idx, cnt = 3, i - 1, 1                          # Sampler
        else:
            rset, idx, cnt = 0, i - 17, 1                         # Bool
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

    out[vsize:vsize + len(microcode)] = microcode
    return bytes(out)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    pad = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    is_vertex = (len(sys.argv) <= 4) or sys.argv[4] != "p"
    open(dst, "wb").write(build(open(src, "rb").read(), is_vertex, pad))
    print("wrote %s (pad %d, %s)" % (dst, pad, "vertex" if is_vertex else "pixel"))
