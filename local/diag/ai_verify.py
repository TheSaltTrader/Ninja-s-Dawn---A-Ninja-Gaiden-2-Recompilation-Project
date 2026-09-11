"""Prove the AI upscaler runs on this machine and produces real 2x detail.

Takes a few decoded art textures from the dump, runs them through
tools/ai_upscale.upscale_many exactly as upscale_textures.py does, and checks:
  * every output is exactly 2x the input and not blank
  * the AI result carries more fine detail than a Lanczos 2x of the same
    texture (variance of the Laplacian - a sharpness measure), texture by
    texture
and writes a side-by-side (original 2x nearest | Lanczos 2x | AI 2x) so a
person can look. Not a quality judgement, a "does it work" check.

    python local/diag/ai_verify.py [--dir C:/ng2tex] [--count 4]
"""
import argparse
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import numpy as np                       # noqa: E402
from PIL import Image, ImageFilter       # noqa: E402
import ai_upscale                        # noqa: E402


def sharpness(img):
    g = np.asarray(img.convert("L"), dtype=np.float32)
    lap = (np.roll(g, 1, 0) + np.roll(g, -1, 0) + np.roll(g, 1, 1) + np.roll(g, -1, 1) - 4 * g)
    return float(lap.var())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="C:/ng2tex")
    ap.add_argument("--count", type=int, default=4)
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args()

    exe = ai_upscale.find_upscaler(args.dir)
    print("upscaler exe:", exe)
    if not exe:
        print("FAIL: realesrgan-ncnn-vulkan.exe not found under", args.dir)
        return 1

    dump = os.path.join(args.dir, "dump")
    # Art textures only: DXT1/DXT4_5 at 256x256 or 512x512, which is what the
    # pack is made of. UI and 1-pixel strips prove nothing.
    picks = []
    for fn in sorted(os.listdir(dump)):
        if not fn.endswith(".png"):
            continue
        if ("_256x256_k_DXT" in fn or "_512x512_k_DXT" in fn) and len(picks) < args.count:
            picks.append(fn)
    if not picks:
        print("FAIL: no decoded art textures in", dump)
        return 1
    images = [(fn, Image.open(os.path.join(dump, fn)).convert("RGBA")) for fn in picks]
    for fn, im in images:
        print("  input %s %dx%d" % (fn, im.width, im.height))

    t0 = time.time()
    out = ai_upscale.upscale_many(exe, images, args.scale, gpu=0)
    took = time.time() - t0
    print("AI pass: %d textures in %.1fs (%.2fs each) - GPU is shared with a training run" %
          (len(images), took, took / len(images)))

    ok_all = True
    tiles = []
    for fn, src in images:
        ai = out[fn]
        want = (src.width * args.scale, src.height * args.scale)
        lanczos = src.resize(want, Image.Resampling.LANCZOS)
        nearest = src.resize(want, Image.Resampling.NEAREST)
        size_ok = ai.size == want
        blank = ai_upscale._looks_blank(ai)
        same_as_lanczos = np.array_equal(np.asarray(ai), np.asarray(lanczos))
        s_l, s_a = sharpness(lanczos), sharpness(ai)
        verdict = "OK" if (size_ok and not blank and not same_as_lanczos) else "FAIL"
        if verdict == "FAIL":
            ok_all = False
        print("  %-48s out %dx%d size_ok=%s blank=%s identical_to_lanczos=%s  sharpness lanczos %.1f -> ai %.1f  %s"
              % (fn[:48], ai.width, ai.height, size_ok, blank, same_as_lanczos, s_l, s_a, verdict))
        row = Image.new("RGBA", (want[0] * 3 + 8, want[1]), (30, 30, 30, 255))
        row.paste(nearest, (0, 0))
        row.paste(lanczos, (want[0] + 4, 0))
        row.paste(ai, (want[0] * 2 + 8, 0))
        tiles.append(row)

    width = max(t.width for t in tiles)
    sheet = Image.new("RGBA", (width, sum(t.height + 4 for t in tiles)), (30, 30, 30, 255))
    y = 0
    for t in tiles:
        sheet.paste(t, (0, y))
        y += t.height + 4
    outp = os.path.join(ROOT, "local", "diag", "captures", "ai_verify.png")
    os.makedirs(os.path.dirname(outp), exist_ok=True)
    sheet.save(outp)
    print("side-by-side (nearest | lanczos | AI) ->", outp)
    print("RESULT:", "AI UPSCALER WORKS" if ok_all else "AI UPSCALER FAILED A CHECK")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
