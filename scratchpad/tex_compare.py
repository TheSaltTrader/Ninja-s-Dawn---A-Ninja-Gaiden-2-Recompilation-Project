# Side-by-side of a texture's ORIGINAL decode (dump PNG) and its PACK version (.tex).
# Usage: python tex_compare.py <id> [<id> ...]   -> writes scratchpad/cmp_<id>.png + prints stats
import sys, os, glob, struct
import numpy as np
from PIL import Image

DUMP = "C:/ng2tex/dump"; PACK = "C:/ng2tex/pack"
OUT = os.path.dirname(os.path.abspath(__file__))

def read_tex(path):
    with open(path, "rb") as f:
        hdr = f.read(16)
        magic, ver, w, h = struct.unpack("<4sIII", hdr)
        assert magic == b"NG2T", magic
        return Image.frombytes("RGBA", (w, h), f.read(w * h * 4))

def stats(im):
    a = np.asarray(im.convert("RGBA"), dtype=np.float32)
    rgb = a[..., :3]; al = a[..., 3]
    return "mean RGB=(%.0f,%.0f,%.0f) alpha mean=%.0f min=%.0f max=%.0f" % (
        rgb[..., 0].mean(), rgb[..., 1].mean(), rgb[..., 2].mean(), al.mean(), al.min(), al.max())

for tid in sys.argv[1:]:
    src = glob.glob(os.path.join(DUMP, tid + "_*.png"))
    tex = os.path.join(PACK, tid + ".tex")
    if not src:
        print(tid, "no decoded PNG in dump"); continue
    a = Image.open(src[0]).convert("RGBA")
    print(tid, os.path.basename(src[0]), "orig:", stats(a))
    if os.path.isfile(tex):
        b = read_tex(tex)
        print("   pack %dx%d:" % b.size, stats(b))
        b_small = b.resize(a.size, Image.Resampling.LANCZOS)
    else:
        print("   (not in pack)"); b_small = None
    # Sheet: original | pack (downsized) | original alpha | pack alpha, each on a checker
    cell = max(a.size); cell = min(cell, 512)
    def fit(im):
        im = im.copy(); im.thumbnail((cell, cell)); return im
    def checker(im):
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        px = bg.load()
        for y in range(0, im.size[1], 16):
            for x in range(0, im.size[0], 16):
                if ((x // 16) + (y // 16)) % 2:
                    for yy in range(y, min(y + 16, im.size[1])):
                        for xx in range(x, min(x + 16, im.size[0])):
                            px[xx, yy] = (180, 180, 180, 255)
        bg.alpha_composite(im); return bg
    panels = [checker(fit(a)), fit(a.getchannel("A")).convert("RGBA")]
    if b_small is not None:
        panels.insert(1, checker(fit(b_small))); panels.append(fit(b_small.getchannel("A")).convert("RGBA"))
    sheet = Image.new("RGBA", (sum(p.size[0] + 8 for p in panels), max(p.size[1] for p in panels)), (30, 30, 30, 255))
    x = 0
    for p in panels:
        sheet.paste(p, (x, 0)); x += p.size[0] + 8
    out = os.path.join(OUT, "cmp_%s.png" % tid)
    sheet.convert("RGB").save(out); print("   ->", out)
