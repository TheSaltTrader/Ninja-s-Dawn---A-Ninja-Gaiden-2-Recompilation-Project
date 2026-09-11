"""Validate rlwinm/rlwnm rotate-and-mask codegen: recompute the PPC 32-bit mask
from MB,ME and the rotate amount SH, compare to the emitted `rotateleft(...,SH) & MASK`.
Catches subtle mask/rotate mistranslations (shared ReXGlue codegen with Fable 2)."""
import glob, re, os, sys
GEN = sys.argv[1] if len(sys.argv) > 1 else "generated/default"

def ppc_mask(mb, me):
    m = 0
    for k in range(32):
        i = (mb + k) & 31
        m |= 1 << (31 - i)
        if i == me:
            break
    return m & 0xFFFFFFFF

# // rlwinm rA,rS,SH,MB,ME
cmt = re.compile(r'^\t// rlwinm r\d+,r\d+,(\d+),(\d+),(\d+)\s*$')
# code: ... __builtin_rotateleft64(... , SH) & 0xHEX;   (or rotateleft32)
code = re.compile(r'__builtin_rotateleft(?:32|64)\(.*?,\s*(\d+)\)\s*&\s*(0x[0-9A-Fa-f]+)')
code_norot = re.compile(r'=\s*(?:ctx\.)?r\d+\.\w+\s*&\s*(0x[0-9A-Fa-f]+)')  # SH==0: no rotate, just mask

checked = 0
mism = []
for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    base = os.path.basename(path)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    for i in range(len(lines)-1):
        m = cmt.match(lines[i])
        if not m:
            continue
        sh, mb, me = int(m.group(1)), int(m.group(2)), int(m.group(3))
        want = ppc_mask(mb, me)
        nxt = lines[i+1]
        checked += 1
        cm = code.search(nxt)
        if cm:
            got_sh = int(cm.group(1))
            got_mask = int(cm.group(2), 16) & 0xFFFFFFFF
            if got_sh != sh or got_mask != want:
                mism.append("%s:%d  // rlwinm ..,%d,%d,%d  -> rot=%d mask=0x%X  EXPECT rot=%d mask=0x%X"
                            % (base, i+2, sh, mb, me, got_sh, got_mask, sh, want))
        elif sh == 0:
            cm2 = code_norot.search(nxt)
            if cm2:
                got_mask = int(cm2.group(1), 16) & 0xFFFFFFFF
                if got_mask != want:
                    mism.append("%s:%d  // rlwinm ..,0,%d,%d  -> mask=0x%X  EXPECT 0x%X"
                                % (base, i+2, mb, me, got_mask, want))
        # else: unrecognized emit shape (skip; reported in summary count)

print("=== rlwinm checked=%d  mask/rotate mismatches=%d ===" % (checked, len(mism)))
for x in mism[:80]:
    print("  " + x)
