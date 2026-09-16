#!/usr/bin/env python3
"""Pack translated Fable II shaders into the runtime's cache directory.

    pack_cache.py <translate_all out dir> <cache dir>

For every <name>_v.hlsl.layout / <name>_p.hlsl.layout whose DXIL compiled,
copies <dxil dir>/<name>.dxil to <cache>/<hash>_<v|p>.dxil and the sidecar
to <cache>/<hash>_<v|p>.layout, <hash> being the container's XXH3 the
sidecar's first line carries (the runtime hashes the live container the
same way). Existing entries are overwritten; nothing else is touched.
"""
import os
import shutil
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    n = skipped = 0
    for f in sorted(os.listdir(os.path.join(src, "hlsl"))):
        if not f.endswith(".hlsl.layout"):
            continue
        name = f[: -len(".hlsl.layout")]
        dxil = os.path.join(src, "dxil", name + ".dxil")
        if not os.path.exists(dxil) or os.path.getsize(dxil) == 0:
            skipped += 1
            continue
        lines = open(os.path.join(src, "hlsl", f), encoding="utf-8").read().splitlines()
        h = lines[0].split()[1]
        kind = "p" if name.endswith("_p") else "v"
        shutil.copyfile(dxil, os.path.join(dst, f"{h}_{kind}.dxil"))
        shutil.copyfile(os.path.join(src, "hlsl", f), os.path.join(dst, f"{h}_{kind}.layout"))
        n += 1
    print(f"packed {n} shaders into {dst} ({skipped} without DXIL)")


if __name__ == "__main__":
    main()
