#!/usr/bin/env python3
"""Is 607-of-607 a rate over the frame, or over a slice of it?

The Fable II session found that the shaders drawing two thirds of ITS frame have
no D3D9 container at all - they are loaded by IM_LOAD from bare microcode that
was never wrapped in an API object. If NG2 is the same, then extracting
containers from a memory dump finds only the shaders that HAVE containers, and a
compile rate over those is a rate over the wrong denominator: the same shape as
their 91-99% coverage over hooked wrappers.

The addresses do not compare directly. IM_LOAD names GPU PHYSICAL addresses
(0x1D..0x1F here); the containers were found in the guest VIRTUAL heap
(0x82xxxxxx). The same shader can legitimately exist twice - once inside its
container in the CPU heap, once as bare microcode DMA'd to GPU memory - so an
address mismatch proves nothing on its own.

So compare by CONTENT: take the microcode the frame actually asked for at each
IM_LOAD address, and look for those exact bytes inside the extracted containers.
A shader the frame uses whose microcode appears in no container is a shader the
translated set does not cover, whatever the compile rate says.
"""
import os
import re
import struct
import sys

dump = open(r"D:/ng2_frameinterp/shaders/phys_dump.bin", "rb").read()
xvu_dir = r"D:/ng2_frameinterp/shaders/xvu"
log = r"D:/ng2_frameinterp/pm4_census.log"

# --- the shaders the frame actually loaded, from the census -----------------
# lines look like:  [ngpu-pm4] shaders VS:1DB5F040x167 PS:1DAE9040x81 ...
used = {}
for line in open(log, encoding="utf-8", errors="replace"):
    if "[ngpu-pm4] shaders" not in line:
        continue
    for kind, addr, hits in re.findall(r"\b(VS|PS):([0-9A-F]{8})x(\d+)", line):
        a = int(addr, 16)
        used[a] = (kind, max(used.get(a, (None, 0))[1], int(hits)))
print("distinct shader addresses the frame loaded (IM_LOAD): %d" % len(used))

# --- the microcode inside every extracted container ------------------------
# Store a few distinctive windows per container rather than the whole blob, so a
# container whose microcode sits at an offset we did not model still matches.
container_bytes = []
for fn in os.listdir(xvu_dir):
    if not fn.endswith(".xvu"):
        continue
    container_bytes.append(open(os.path.join(xvu_dir, fn), "rb").read())
print("containers extracted: %d" % len(container_bytes))
blob = b"\x00".join(container_bytes)   # one haystack, separated so no false joins

# --- does each used shader's microcode appear in any container? ------------
hit = miss = unreadable = 0
misses = []
for a, (kind, hits) in sorted(used.items(), key=lambda kv: -kv[1][1]):
    off = a & 0x1FFFFFFF
    if off + 64 > len(dump):
        unreadable += 1
        continue
    # 64 bytes of real microcode is far too specific to collide by chance
    needle = dump[off:off + 64]
    if needle.count(0) == len(needle):
        unreadable += 1
        continue
    if needle in blob:
        hit += 1
    else:
        miss += 1
        if len(misses) < 12:
            misses.append((kind, a, hits))

print()
print("used shaders whose microcode IS inside an extracted container: %d" % hit)
print("used shaders NOT in any container:                             %d" % miss)
print("unreadable/zero at that address:                               %d" % unreadable)
if misses:
    print()
    print("busiest shaders with no container (kind, address, loads in a frame):")
    for kind, a, hits in misses:
        print("   %s 0x%08X  x%d" % (kind, a, hits))
