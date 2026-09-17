"""Where do a shader's literal constants live?

The real translation ends with `float4 c255 = asfloat(uint4(0, 0x3F800000,
0x3F000000, 0))` - the shader's own 1.0 and 0.5 - and a synthetic container has
none, which is wrong in a way that renders rather than fails.

XenosRecomp reads them through the definition table:
    value = shaderData + virtualSize + definition->physicalOffset

So if physicalOffset lands inside the region IM_LOAD names, they are recoverable
for an uncontained shader. If it lands before it, they are not, and the question
becomes whether the uncontained shaders use any.
"""
import struct, os

REL = r"C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\fable2recomp\out\build\win-amd64-Release"
d = open(os.path.join(REL, "ngpu_jit", "2D96AFB187B6B7F6_v.xvu"), "rb").read()
flags, vsize, psize = struct.unpack(">III", d[0:12])
dtoff = struct.unpack(">I", d[0x14:0x18])[0]
shoff = struct.unpack(">I", d[0x18:0x1C])[0]
po, size = struct.unpack(">2I", d[shoff:shoff + 8])
print("container: virtualSize %d physicalSize %d  definitionTableOffset %d" % (vsize, psize, dtoff))
print("           code at physicalOffset %d, size %d  (so code occupies %d..%d of the physical region)"
      % (po, size, po, po + size))

# DefinitionTable: 5 dwords of header, then the definitions
defs_at = dtoff + 20
print("\nFloat4 definitions (registerIndex, count, physicalOffset):")
i = defs_at
while True:
    w = struct.unpack(">I", d[i:i + 4])[0]
    if w == 0:
        break
    reg, cnt = struct.unpack(">2H", d[i:i + 4])
    phys = struct.unpack(">I", d[i + 4:i + 8])[0]
    vals_at = vsize + phys
    n = (cnt + 3) // 4
    print("   c%-3d count %-3d physicalOffset %-4d -> file offset %d  %s"
          % (reg, cnt, phys, vals_at,
             "INSIDE the code region" if phys >= po else "BEFORE the code (in the preamble)"))
    for k in range(n):
        at = vals_at + k * 16
        v = struct.unpack(">4I", d[at:at + 16])
        print("      c%d = %s" % (reg + k, " ".join("%08X" % x for x in v)))
    i += 8
print("\n(the 64-byte preamble is at physical offset 0..63; the code starts at %d)" % po)
