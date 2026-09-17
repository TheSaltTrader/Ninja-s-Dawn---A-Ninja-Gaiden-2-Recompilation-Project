"""Does the vfetch scan agree with a real container's own vertex element table?

The scan has to find exactly the fetch addresses the container declares, or the
synthesised table will miss a lookup the recompiler asserts on.
"""
import struct, sys, os, glob

SP = r"C:\Users\renoi\AppData\Local\Temp\claude\C--users-renoi-claudecode\7d093996-252d-49c9-a593-f1ba6e716609\scratchpad"
sys.path.insert(0, SP)
from vfetch_scan import scan

USAGE = {0: "Position", 1: "BlendWeight", 2: "BlendIndices", 3: "Normal", 4: "PointSize",
         5: "TexCoord", 6: "Tangent", 7: "Binormal", 8: "TessFactor", 9: "PositionT",
         10: "Color", 11: "Fog", 12: "Depth", 13: "Sample"}

real = os.path.join(r"C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\fable2recomp\out\build\win-amd64-Release\ngpu_jit",
                    "2D96AFB187B6B7F6_v.xvu")
d = open(real, "rb").read()
flags, vsize, psize = struct.unpack(">III", d[0:12])
shoff = struct.unpack(">I", d[0x18:0x1C])[0]
po, size = struct.unpack(">2I", d[shoff:shoff + 8])
field18, count, field20 = struct.unpack(">3I", d[shoff + 24:shoff + 36])
print("real container: shaderOffset %d physicalOffset %d size %d" % (shoff, po, size))
print("   field18 %d  vertexElementCount %d  field20 %d" % (field18, count, field20))

declared = []
for i in range(count):
    at = shoff + 36 + (field18 + i) * 4
    v = struct.unpack(">I", d[at:at + 4])[0]
    declared.append((v & 0xFFF, (v >> 12) & 0xF, (v >> 16) & 0xF))
print("   declared elements:")
for a, u, ui in declared:
    print("      address %3d  %s%d" % (a, USAGE.get(u, "?%d" % u), ui))

code = d[vsize + po: vsize + po + size]
found = scan(code)
print("\nscan found %d vertex fetches at addresses: %s" % (len(found), found))

want = sorted({a for a, _, _ in declared})
got = sorted(set(found))
print("declared addresses: %s" % want)
print("MATCH" if want == got else "MISMATCH  missing %s  extra %s"
      % ([a for a in want if a not in got], [a for a in got if a not in want]))
