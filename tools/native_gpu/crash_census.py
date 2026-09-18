#!/usr/bin/env python3
"""Census the translator's CRASH lottery, and look for a correlate.

The crash defect is the one thing on this project with no mechanism after three
days. Two interventions at opposite ends of the fetch-element lookup have now
failed to move it - NG2 gave a container a dense table that cannot miss and the
rate did not change; the sibling project made the miss REFUSE and the rate did
not change either. So it is not the lookup, from either side.

The gate that translates the corpus already learns the crash rate on every run
and THROWS IT AWAY: it retries a crash (correctly - a crash is not an output
disagreement) and records only the final verdict. Three days of data discarded
because the instrument's job was to survive the defect rather than measure it.

This records the outcome of every individual run, so a correlate can be looked
for instead of argued about.

DESIGN, from this project's own accumulated failures:

  - A CANARY FIRST. The patched binary needs dxcompiler.dll/dxil.dll/fmt.dll
    beside it; run from elsewhere it exits 0 writing nothing, and a census then
    reports "everything crashes" - a beautifully consistent, entirely fictional
    table. Both sessions have now hit this.

  - PRODUCED IS COUNTED SEPARATELY FROM DISTINCT, because a checker that counts
    zero outputs as zero distinct hashes reports "produced nothing" and
    "produced the same thing" identically.

  - EVERY RUN GETS ITS OWN OUTPUT PATH. A shared one measures the state as much
    as the subject.

  - THE SAMPLE IS RANDOM ACROSS THE WHOLE CORPUS with a fixed seed, never the
    first N. NG2's containers are ordered by guest address and address carries
    shader kind: a contiguous 40-container window of this corpus returns
    anywhere between 0% and 100% on a property that is 44% overall.

  - CRASHES ARE DISTINGUISHED FROM CAPS. A process killed for breaching a
    memory cap dies with 0xC0000409, the same code as an assertion failure, so
    the cap must be classified BEFORE the generic crash or every capped run
    reads as a crash. That already cost this project a wrong conclusion once.
"""
import hashlib
import os
import random
import subprocess
import sys

FIX = (r"C:\Users\renoi\AppData\Local\Temp\claude\C--users-renoi-claudecode"
       r"\7d093996-252d-49c9-a593-f1ba6e716609\scratchpad\XenosRecomp.fetchfix.exe")
DLLDIR = r"C:\Users\renoi\ClaudeCode\NativeGPU\build\xenosrecomp\XenosRecomp"
HEADER = r"C:\Users\renoi\ClaudeCode\NativeGPU\fable2_shader_common.h"
CAPPER = r"D:\ng2_frameinterp\ng2recomp-worktree\tools\native_gpu\run_capped.py"
CANARY = "ng2_8200AC88.xvu"


def run_once(container, out_path):
    """Return (classification, sha256-or-None)."""
    cmd = [sys.executable, CAPPER, "120", "8192", "--",
           FIX, container, out_path, HEADER]
    try:
        p = subprocess.run(cmd, cwd=DLLDIR, capture_output=True, timeout=180)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        return "TIMEOUT", None
    # Classify the specific causes BEFORE the generic one.
    if rc == 125:
        return "MEMCAP", None
    if rc == 124:
        return "TIMECAP", None
    if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
        h = hashlib.sha256(open(out_path, "rb").read()).hexdigest()[:16]
        return "OK", h
    if rc == 0:
        # Exited cleanly and wrote nothing - the environment is wrong, not the
        # shader. Named separately so it can never be counted as a crash.
        return "SILENT", None
    return "CRASH", None


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else r"D:\ng2_frameinterp\shaders\xvu"
    n_sample = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    runs = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    tmp = r"D:\ng2_frameinterp\work\crash_census_tmp"
    os.makedirs(tmp, exist_ok=True)

    if not os.path.exists(FIX):
        print("patched binary missing:", FIX)
        return 1

    # CANARY. If this container does not translate, nothing below means anything.
    cpath = os.path.join(src, CANARY)
    if os.path.exists(cpath):
        cls, _ = run_once(cpath, os.path.join(tmp, "canary.hlsl"))
        if cls in ("SILENT", "TIMEOUT"):
            print(f"REFUSING: canary returned {cls} - the binary cannot run here")
            return 1
        print(f"canary: {cls}")
    else:
        print("REFUSING: canary container not found")
        return 1

    names = sorted(f for f in os.listdir(src) if f.endswith(".xvu"))
    random.seed(20260917)
    sample = sorted(random.sample(names, min(n_sample, len(names))))

    print(f"sample {len(sample)} of {len(names)} containers, {runs} runs each\n")
    rows = []
    for name in sample:
        outcomes = []
        hashes = set()
        for r in range(runs):
            out = os.path.join(tmp, f"{name[:-4]}.{r}.hlsl")
            for stale in (out, out + ".layout"):
                if os.path.exists(stale):
                    os.remove(stale)
            cls, h = run_once(os.path.join(src, name), out)
            outcomes.append(cls)
            if h:
                hashes.add(h)
            for stale in (out, out + ".layout"):
                if os.path.exists(stale):
                    os.remove(stale)
        produced = sum(1 for o in outcomes if o == "OK")
        crashed = sum(1 for o in outcomes if o == "CRASH")
        rows.append((name, produced, crashed, len(hashes), outcomes))
        print(f"{name:28s} produced {produced}/{runs}  crashed {crashed}  "
              f"distinct {len(hashes)}  {','.join(outcomes)}")

    total_runs = len(rows) * runs
    total_crash = sum(r[2] for r in rows)
    always = sum(1 for r in rows if r[1] == runs)
    never = sum(1 for r in rows if r[1] == 0)
    mixed = len(rows) - always - never
    unstable = sum(1 for r in rows if r[3] > 1)
    print()
    print(f"runs                     : {total_runs}")
    print(f"  CRASHED                : {total_crash}  ({100.0*total_crash/total_runs:.1f}%)")
    print(f"containers               : {len(rows)}")
    print(f"  produced every run     : {always}")
    print(f"  produced on SOME runs  : {mixed}   <- the lottery")
    print(f"  never produced         : {never}")
    print(f"  MORE THAN ONE OUTPUT   : {unstable}   <- instability, distinct from crashing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
