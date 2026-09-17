"""Does a derived vertex element table remove the crashes AND the non-determinism?

Both symptoms came from the same missing structure, so both have to go, and the
test has to be repeated per shader - a single run cannot tell a fix from a lucky
draw of the very process being fixed.
"""
import os, sys, struct, glob, subprocess, hashlib

SP = r"C:\Users\renoi\AppData\Local\Temp\claude\C--users-renoi-claudecode\7d093996-252d-49c9-a593-f1ba6e716609\scratchpad"
SRC = r"C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\fable2recomp\out\build\win-amd64-Release\ngpu_synth"
RECOMP = r"C:\Users\renoi\ClaudeCode\NativeGPU\build\xenosrecomp\XenosRecomp\XenosRecomp.exe"
HEADER = r"C:\Users\renoi\ClaudeCode\NativeGPU\fable2_shader_common.h"
N = int(os.environ.get("REPS", "8"))

sys.path.insert(0, SP)
import synth_xvu          # v1: no element table
import synth_xvu2         # v2: derived element table

work = os.path.join(SP, "ctl", "v2test")
os.makedirs(work, exist_ok=True)

print("shader                   v1 ok   v1 distinct    v2 ok   v2 distinct")
tot = {1: 0, 2: 0}
stable = {1: 0, 2: 0}
for f in sorted(glob.glob(os.path.join(SRC, "*.xvu"))):
    name = os.path.basename(f)[:-4]
    b = open(f, "rb").read()
    _, v, p = struct.unpack(">III", b[0:12])
    code = b[v:v + p]
    row = {}
    for ver, mod in ((1, synth_xvu), (2, synth_xvu2)):
        xvu = os.path.join(work, "%s_v%d.xvu" % (name, ver))
        out = os.path.join(work, "%s_v%d.hlsl" % (name, ver))
        open(xvu, "wb").write(mod.build(code, True, 0))
        ok, hashes = 0, set()
        for _ in range(N):
            if os.path.exists(out):
                os.remove(out)
            try:
                subprocess.run([RECOMP, xvu, out, HEADER], timeout=60,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except Exception:
                pass
            if os.path.exists(out) and os.path.getsize(out) > 0:
                ok += 1
                hashes.add(hashlib.md5(open(out, "rb").read()).hexdigest()[:8])
        row[ver] = (ok, len(hashes))
        tot[ver] += ok
        if ok == N and len(hashes) == 1:
            stable[ver] += 1
    print("%-24s %2d/%d %8d %9d/%d %8d"
          % (name, row[1][0], N, row[1][1], row[2][0], N, row[2][1]))

n = len(glob.glob(os.path.join(SRC, "*.xvu")))
print()
print("v1 (no element table): %d/%d attempts, %d of %d shaders fully stable"
      % (tot[1], n * N, stable[1], n))
print("v2 (derived table):    %d/%d attempts, %d of %d shaders fully stable"
      % (tot[2], n * N, stable[2], n))
