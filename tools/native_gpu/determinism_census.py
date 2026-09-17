#!/usr/bin/env python3
"""Does XenosRecomp produce the SAME HLSL for the same container, every time?

The sibling project found a container that translated to SIX DIFFERENT HLSL
outputs across six runs of the same input - not reordering, but different
instructions, different vertex streams, constant registers past the end of the
pixel bank. If that happens here, then every "N of 625 translate" figure this
project has quoted is a statement about a per-run lottery rather than about the
shaders, and the manifest is whatever the last roll produced.

There is a specific reason to suspect it on MY corpus too, independent of
theirs. A full re-translation reported 28 recompiler failures; re-running the
same containers idle recovered 22, and raising the deadline recovered 7 more. I
attributed all of that to machine load, and load does explain a deadline being
missed - but non-determinism explains the same numbers just as well, and I never
separated them. This does.

STABLE means K runs produced byte-identical HLSL. UNSTABLE means they did not,
and that is a defect in the translator or its input, never a property of the
shader. FAILED means the recompiler did not produce output at all; a shader that
fails every time is a different thing from one that fails sometimes, so the
count of DISTINCT outcomes is reported rather than a pass rate.

Usage: determinism_census.py <container dir> [runs] [sample]
       sample = 0 means every container.
"""
import hashlib
import os
import random
import subprocess
import sys
import tempfile

RECOMP = r"C:\Users\renoi\ClaudeCode\NativeGPU\build\xenosrecomp\XenosRecomp\XenosRecomp.exe"
HEADER = r"C:\Users\renoi\ClaudeCode\NativeGPU\fable2_shader_common.h"


def translate_once(src, out_path):
    """Return the sha1 of the HLSL, or None if the recompiler produced nothing."""
    if os.path.exists(out_path):
        os.remove(out_path)
    # THROUGH THE CAP, NOT A BARE TIMEOUT. This census ran 240 UNCAPPED
    # invocations of a translator known to reach 41 GB on some inputs, while I
    # was telling the sibling session to grep for exactly this. A time cap
    # bounds how long a process runs, not how much it takes with it, and a
    # control suite can only test the wrapper a call site actually reaches - so
    # a bypass is invisible to every control and only a grep finds it.
    capped = os.path.join(os.path.dirname(os.path.abspath(__file__)), "run_capped.py")
    try:
        subprocess.run([sys.executable, capped, "120", "4096", "--",
                        RECOMP, src, out_path, HEADER], timeout=180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        return None
    if not os.path.exists(out_path) or os.path.getsize(out_path) == 0:
        return None
    with open(out_path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else r"D:/ng2_frameinterp/shaders/xvu"
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    sample = int(sys.argv[3]) if len(sys.argv) > 3 else 0

    names = sorted(f for f in os.listdir(root) if f.endswith(".xvu"))
    if sample and sample < len(names):
        random.seed(1)                      # a fixed seed, so the run is repeatable
        names = sorted(random.sample(names, sample))

    stable = unstable = always_failed = sometimes_failed = 0
    unstable_names, flaky_names = [], []
    tmp = tempfile.mkdtemp(prefix="detcensus_")

    for i, name in enumerate(names):
        src = os.path.join(root, name)
        hashes = []
        for r in range(runs):
            hashes.append(translate_once(src, os.path.join(tmp, "%s.%d.hlsl" % (name, r))))
        distinct = set(hashes)
        nones = sum(1 for h in hashes if h is None)
        if nones == runs:
            always_failed += 1
        elif nones:
            sometimes_failed += 1
            flaky_names.append(name)
        elif len(distinct) == 1:
            stable += 1
        else:
            unstable += 1
            unstable_names.append((name, len(distinct)))
        if (i + 1) % 25 == 0:
            print("  ... %d/%d" % (i + 1, len(names)), flush=True)

    total = len(names)
    print()
    print("containers       : %d, %d runs each" % (total, runs))
    print("  STABLE         : %d  (byte-identical every run)" % stable)
    print("  UNSTABLE       : %d  (different output between runs)" % unstable)
    print("  always failed  : %d  (a real property of the shader)" % always_failed)
    print("  SOMETIMES failed: %d (a lottery, not a property)" % sometimes_failed)
    for n, d in unstable_names[:10]:
        print("     UNSTABLE %-26s %d distinct outputs" % (n, d))
    for n in flaky_names[:10]:
        print("     FLAKY    %s" % n)
    return 0 if (unstable == 0 and sometimes_failed == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
