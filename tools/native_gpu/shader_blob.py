"""Inspect the shader dumps the draw dump writes (ngpu_shaders/<obj>_<v|p>.obj,
<data>_<v|p>.container, <code>_<v|p>.bin): find the XDK ShaderContainer
header (flags, virtualSize, physicalSize, fieldC, constantTableOffset,
definitionTableOffset, shaderOffset, ... - XenosRecomp's shader.h) inside the
object and container dumps, and report where the container starts relative
to the object, so the runtime dump can write exactly the container XenosRecomp
parses. Also prints the first words of the microcode.
usage: shader_blob.py <ngpu_shaders dir>
"""
import glob, os, struct, sys

d = sys.argv[1]
def be32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0] if o + 4 <= len(b) else None

def find_container(b):
    """Offsets in b where a plausible ShaderContainer header sits."""
    hits = []
    for o in range(0, len(b) - 36, 4):
        vs, ps, ct, dt, so = be32(b, o + 4), be32(b, o + 8), be32(b, o + 16), be32(b, o + 20), be32(b, o + 24)
        if not (0 < vs < 0x10000 and 0 < ps < 0x40000): continue
        if not (0 < ct < vs and 0 < dt < vs and 0 <= so < 0x40000): continue
        if not (ct <= dt): continue
        hits.append((o, vs, ps, ct, dt, so))
    return hits

objs = sorted(glob.glob(os.path.join(d, "*_[vp].obj")))
print(f"{len(objs)} shader objects, {len(glob.glob(os.path.join(d, '*.container')))} containers, {len(glob.glob(os.path.join(d, '*.bin')))} microcode blobs")
for path in objs[:12]:
    b = open(path, "rb").read()
    name = os.path.basename(path)
    words = " ".join(f"{be32(b, o):08X}" for o in range(0, 96, 4))
    print(f"\n{name}: obj words 0..23: {words}")
    print(f"  +24 data={be32(b, 24):08X} +32 base={be32(b, 32):08X} +64 hdroff={be32(b, 64):08X} +872..: " +
          " ".join(f"{be32(b, o):08X}" for o in range(872, 872 + 40, 4)))
    for o, vs, ps, ct, dt, so in find_container(b)[:4]:
        print(f"  container header candidate at obj+{o} (0x{o:X}): virtualSize {vs:#x} physicalSize {ps:#x} constTable +{ct:#x} defTable +{dt:#x} shaderOffset {so:#x}")
for path in sorted(glob.glob(os.path.join(d, "*_[vp].container")))[:6]:
    b = open(path, "rb").read()
    print(f"\n{os.path.basename(path)} ({len(b)} bytes): first words " + " ".join(f"{be32(b, o):08X}" for o in range(0, 40, 4)))
    for o, vs, ps, ct, dt, so in find_container(b)[:3]:
        print(f"  container header candidate at +{o} (0x{o:X}): virtualSize {vs:#x} physicalSize {ps:#x} constTable +{ct:#x} defTable +{dt:#x} shaderOffset {so:#x}")
for path in sorted(glob.glob(os.path.join(d, "*_[vp].bin")))[:4]:
    b = open(path, "rb").read()
    print(f"\n{os.path.basename(path)} ({len(b)} bytes): first words " + " ".join(f"{be32(b, o):08X}" for o in range(0, min(48, len(b)), 4)))
