"""Regenerate the pack textures written by the broken 2x AI pass.

On 2026-09-06 07:45-07:47 the pack tool ran Real-ESRGAN x4plus with "-s 2",
which garbles the output (see CHANGELOG v1.0.1), and wrote 240 blended .tex
files before it was stopped. This finds every .tex in the pack modified in
that window (or after --since), rebuilds each from its decoded dump PNG with
the FIXED pipeline (tools/ai_upscale.py at the model's native 4x, resized to
--scale, blended at --strength) and overwrites it. Uses the same write_tex and
blend the tool uses, so the result is byte-for-byte what a full re-run would
produce for those ids.

    python local/diag/repair_ai_textures.py --dir C:/ng2tex --since "2026-09-06 07:40"
"""
import argparse
import datetime as dt
import glob
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from PIL import Image                     # noqa: E402
import ai_upscale                         # noqa: E402
import upscale_textures                   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="C:/ng2tex")
    ap.add_argument("--since", default="2026-09-06 07:40")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--strength", type=float, default=1.0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    pack = os.path.join(args.dir, "pack")
    dump = os.path.join(args.dir, "dump")
    since = dt.datetime.strptime(args.since, "%Y-%m-%d %H:%M").timestamp()
    victims = sorted(f for f in glob.glob(os.path.join(pack, "*.tex"))
                     if os.path.getmtime(f) >= since)
    print("%d .tex files in the pack modified since %s" % (len(victims), args.since))
    if not victims:
        return 0

    exe = ai_upscale.find_upscaler(args.dir)
    if not exe:
        print("FAIL: upscaler executable not found")
        return 1

    images, missing = [], []
    for f in victims:
        tid = os.path.splitext(os.path.basename(f))[0]
        pngs = glob.glob(os.path.join(dump, tid + "_*.png"))
        if not pngs:
            missing.append(tid)
            continue
        images.append((tid, Image.open(pngs[0]).convert("RGBA")))
    print("%d have a decoded source, %d do not (left untouched)" % (len(images), len(missing)))
    if missing:
        print("  missing:", " ".join(missing[:10]), "..." if len(missing) > 10 else "")
    if args.dry_run:
        return 0

    t0 = time.time()
    got = ai_upscale.upscale_many(exe, images, args.scale, gpu=0)
    print("AI pass: %d textures in %.0fs" % (len(images), time.time() - t0))
    written = 0
    for tid, src in images:
        out = ai_upscale.blend(got[tid], src, args.strength)
        upscale_textures.write_tex(os.path.join(pack, tid + ".tex"), out)
        written += 1
        if written % 40 == 0:
            print("  written %d / %d" % (written, len(images)), flush=True)
    print("DONE rewrote %d textures with the fixed pipeline (scale %d, strength %.2f)"
          % (written, args.scale, args.strength))
    return 0


if __name__ == "__main__":
    sys.exit(main())
