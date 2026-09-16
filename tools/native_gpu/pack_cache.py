#!/usr/bin/env python3
"""Pack translated Fable II shaders into the runtime's cache directory.

    pack_cache.py <translate_all out dir> <cache dir> [<ngpu_shaders dir>]

For every <name>_v.hlsl.layout / <name>_p.hlsl.layout whose DXIL compiled,
copies <dxil dir>/<name>.dxil to <cache>/<hash>_<v|p>.dxil and the sidecar
to <cache>/<hash>_<v|p>.layout, <hash> being the container's XXH3 the
sidecar's first line carries (the runtime hashes the live container the
same way). Existing entries are overwritten; nothing else is touched.

Also writes <cache>/<codehash>_<v|p>.code holding <hash>: the XXH3 of the
microcode block alone (the .xvu's bytes past the container, whose size is
the big-endian dword at +4). Fable's UI shader containers differ from run
to run while their code does not, so the runtime falls back to this alias
when the container hash misses. The .xvu files are looked up by name in
the ngpu_shaders dir (default: the Fable II build's).
"""
import os
import shutil
import sys

try:
    import xxhash
except ImportError:  # aliases are optional
    xxhash = None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    xvu_dir = sys.argv[3] if len(sys.argv) > 3 else r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/fable2recomp/out/build/win-amd64-Release/ngpu_shaders"
    os.makedirs(dst, exist_ok=True)
    n = skipped = aliases = 0
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
        if xxhash is not None:
            for cand in (name + ".xvu", name + ".var.xvu"):
                xvu = os.path.join(xvu_dir, cand)
                if os.path.exists(xvu):
                    b = open(xvu, "rb").read()
                    vsize = int.from_bytes(b[4:8], "big")
                    if 0 < vsize < len(b):
                        code = xxhash.xxh3_64_hexdigest(b[vsize:]).upper()
                        with open(os.path.join(dst, f"{code}_{kind}.code"), "w") as a:
                            a.write(h)
                        aliases += 1
                    break
    print(f"packed {n} shaders into {dst} ({skipped} without DXIL, {aliases} code aliases)")


if __name__ == "__main__":
    main()
