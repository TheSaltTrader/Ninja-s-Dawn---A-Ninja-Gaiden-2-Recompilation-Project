"""Is the pack on disk the fixed AI output? Checks that need no game running.

  * when every .tex was written (a pack built by one run has one time window)
  * a sample of .tex files decoded and compared with a plain Lanczos 2x of
    the same texture's decoded dump: a faithful upscale correlates at ~0.99;
    the broken "-s 2" output (shifted, repeated, mostly black) correlated at
    ~0.2 and was far darker
  * a side-by-side sheet (dump nearest 2x | Lanczos 2x | pack) to look at

    python local/diag/verify_pack.py [--dir C:/ng2tex] [--count 6]
"""
import argparse
import glob
import os
import struct
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def read_tex(path):
    with open(path, "rb") as f:
        head = f.read(16)
        magic, version, w, h = head[:4], *struct.unpack("<III", head[4:16])
        if magic != b"NG2T":
            raise ValueError("bad magic %r" % magic)
        data = f.read(w * h * 4)
    return Image.frombytes("RGBA", (w, h), data), version


def corr(a, b):
    x = np.asarray(a.convert("L"), dtype=np.float64).ravel()
    y = np.asarray(b.convert("L"), dtype=np.float64).ravel()
    x -= x.mean(); y -= y.mean()
    d = np.sqrt((x * x).sum() * (y * y).sum())
    return float((x * y).sum() / d) if d > 0 else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="C:/ng2tex")
    ap.add_argument("--count", type=int, default=6)
    args = ap.parse_args()
    pack, dump = os.path.join(args.dir, "pack"), os.path.join(args.dir, "dump")

    texes = glob.glob(os.path.join(pack, "*.tex"))
    times = sorted(os.path.getmtime(t) for t in texes)
    fmt = lambda t: time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t))
    print("%d .tex files; written between %s and %s" % (len(texes), fmt(times[0]), fmt(times[-1])))
    # Histogram by half-hour, so a pack written by more than one run shows it.
    buckets = {}
    for t in times:
        k = time.strftime("%H:%M", time.localtime(t - (t % 1800)))
        buckets[k] = buckets.get(k, 0) + 1
    for k in sorted(buckets):
        print("  %s  %5d files" % (k, buckets[k]))
    print("manifest:", open(os.path.join(pack, "pack.txt")).read().strip().replace("\n", "  ")
          if os.path.isfile(os.path.join(pack, "pack.txt")) else "(none)")

    # Sample: art textures with a decoded dump PNG to compare against.
    picks = []
    for t in sorted(texes):
        tid = os.path.splitext(os.path.basename(t))[0]
        pngs = glob.glob(os.path.join(dump, tid + "_256x256_k_DXT*.png"))
        if pngs:
            picks.append((tid, t, pngs[0]))
        if len(picks) >= args.count:
            break
    rows, ok_all = [], True
    for tid, tpath, png in picks:
        src = Image.open(png).convert("RGBA")
        packed, version = read_tex(tpath)
        want = (src.width * 2, src.height * 2)
        lanczos = src.resize(want, Image.Resampling.LANCZOS)
        c = corr(packed, lanczos)
        mean_pack = float(np.asarray(packed.convert("L"), dtype=np.float32).mean())
        mean_lan = float(np.asarray(lanczos.convert("L"), dtype=np.float32).mean())
        ok = packed.size == want and version == 1 and c > 0.9 and abs(mean_pack - mean_lan) < 12.0
        ok_all &= ok
        print("  %s  pack %dx%d v%d  corr vs Lanczos %.3f  mean pack %.1f / lanczos %.1f  %s"
              % (tid, packed.width, packed.height, version, c, mean_pack, mean_lan,
                 "OK" if ok else "SUSPECT"))
        row = Image.new("RGBA", (want[0] * 3 + 8, want[1]), (30, 30, 30, 255))
        row.paste(src.resize(want, Image.Resampling.NEAREST), (0, 0))
        row.paste(lanczos, (want[0] + 4, 0))
        row.paste(packed, (want[0] * 2 + 8, 0))
        rows.append(row)
    if rows:
        sheet = Image.new("RGBA", (rows[0].width, sum(r.height + 4 for r in rows)), (30, 30, 30, 255))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y)); y += r.height + 4
        out = os.path.join(ROOT, "local", "diag", "captures", "pack_verify.png")
        sheet.save(out)
        print("sheet (dump nearest 2x | Lanczos 2x | PACK) ->", out)
    print("RESULT:", "PACK IS A FAITHFUL 2x OF THE DUMP" if ok_all else "PACK HAS SUSPECT TEXTURES")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
