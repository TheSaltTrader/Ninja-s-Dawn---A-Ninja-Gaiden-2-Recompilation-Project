"""Rebuild the pack from already-decoded dump PNGs, without the raw guest bytes.

    python tools/repack_from_decoded.py --dir C:/ng2tex --scale 4

The normal route is upscale_textures.py, which decodes tex_<id>.bin and writes
the pack in one pass. This exists because the decoded PNGs outlive the raw dump:
they carry the id, the size and the format in their filenames, which is
everything the pack filter needs. Losing the .bin files therefore does not mean
replaying the game.

It also keeps the two paths honest by importing the SAME pack_reason() and
upscaler that upscale_textures.py uses - a second copy of the filter would drift
and quietly pack things the real one rejects.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upscale_textures as U  # noqa: E402

import ai_upscale  # noqa: E402

from PIL import Image  # noqa: E402

NAME_RE = re.compile(r"^([0-9A-Fa-f]{16})_(\d+)x(\d+)_(.+)\.png$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--model", default="RealESRGAN_x4plus")
    ap.add_argument("--no-upscale", action="store_true")
    ap.add_argument("--include-ui", action="store_true")
    ap.add_argument("--ai", action="store_true",
                    help="use Real-ESRGAN (needs tools/get_upscaler.py first)")
    ap.add_argument("--ai-strength", type=float, default=0.75,
                    help="1.0 = the model alone, 0.0 = a plain resize")
    ap.add_argument("--gpu", type=int, default=0)
    args = ap.parse_args()

    dump = os.path.join(args.dir, "dump")
    pack = os.path.join(args.dir, "pack")
    os.makedirs(pack, exist_ok=True)

    by_name = {v: k for k, v in U.FMT_NAMES.items()}
    entries = []
    for fn in sorted(os.listdir(dump)):
        m = NAME_RE.match(fn)
        if not m:
            continue
        tid, w, h, fmt_name = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        fmt = by_name.get(fmt_name)
        if fmt is None:
            continue
        entries.append((tid, w, h, fmt, os.path.join(dump, fn)))

    skips = {}
    chosen = []
    for tid, w, h, fmt, path in entries:
        reason = U.pack_reason(w, h, fmt)
        if reason and not args.include_ui:
            skips[reason] = skips.get(reason, 0) + 1
            continue
        chosen.append((tid, path))

    total = len(chosen)
    packed = 0
    started = time.time()

    if args.ai:
        exe = ai_upscale.find_upscaler(args.dir)
        if not exe:
            print("FAILED the AI upscaler is not installed - run tools/get_upscaler.py")
            return 1
        print("AI upscaler: %s (strength %.2f)" % (exe, args.ai_strength))
        # In chunks so progress is visible and a device loss cannot take the
        # whole run down with it.
        step = ai_upscale.CHUNK
        for base in range(0, total, step):
            part = chosen[base:base + step]
            srcs = {tid: Image.open(p).convert("RGBA") for tid, p in part}
            got = ai_upscale.upscale_many(exe, list(srcs.items()), args.scale,
                                          gpu=args.gpu)
            for tid, _p in part:
                img = ai_upscale.blend(got[tid], srcs[tid], args.ai_strength)
                U.write_tex(os.path.join(pack, "%s.tex" % tid), img)
                packed += 1
                print("PROGRESS %d %d %s" % (packed, total, tid), flush=True)
    else:
        up = None if args.no_upscale else U.make_upscaler(args.model, args.scale)
        for i, (tid, path) in enumerate(chosen, 1):
            print("PROGRESS %d %d %s" % (i, total, tid), flush=True)
            img = Image.open(path).convert("RGBA")
            if up:
                img = up(img)
            U.write_tex(os.path.join(pack, "%s.tex" % tid), img)
            packed += 1

    for why, n in sorted(skips.items(), key=lambda kv: -kv[1]):
        print("  not packed - %-28s %d" % (why, n))
    print("DONE packed=%d of %d decoded in %.1fs"
          % (packed, total, time.time() - started))
    return 0


if __name__ == "__main__":
    sys.exit(main())
