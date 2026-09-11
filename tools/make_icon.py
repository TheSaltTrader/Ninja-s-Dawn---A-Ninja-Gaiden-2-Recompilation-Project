"""Draw the port's icon: a katana over the kanji for ninja.

    python tools/make_icon.py                 # writes resources/ng2.ico (+ previews)
    python tools/make_icon.py --preview       # also writes the magnified check sheet

Windows takes the icon from THREE places and none implies the others:

  1. the icon resource compiled into ng2.exe  - Explorer, the desktop shortcut,
     Alt-Tab
  2. the window's own icon (WM_SETICON)       - the taskbar button while running
  3. an explicit AppUserModelID               - which taskbar group it joins

This script only makes the .ico. See resources/ng2.rc for (1) and
Ng2App::SetWindowIcon for (2) and (3).

Small sizes are drawn SIMPLIFIED rather than downsampled from the big one. A
16px LANCZOS reduction of a detailed blade is a grey smudge - the hamon, the
grip wrap and the thin outlines all land inside one pixel and average into mud.
Below 32px the blade is drawn thicker, the guard is a plain disc and the fine
lines are dropped entirely.
"""

import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

NIN = "\u5fcd"          # the kanji, kept out of the source as an escape so the
                        # file stays ASCII and no editor re-encodes it

# Yu Gothic Bold first: it is the heaviest Japanese face Windows ships, and
# weight is what survives being shrunk to 16 pixels.
FONT_CANDIDATES = [
    "C:/Windows/Fonts/YuGothB.ttc",
    "C:/Windows/Fonts/YuGothM.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
]

SIZES = [16, 24, 32, 48, 64, 128, 256]

RED = (200, 16, 46)
RED_DARK = (120, 8, 26)
STEEL_LIGHT = (222, 229, 239)
STEEL_MID = (150, 164, 184)
STEEL_EDGE = (52, 60, 74)
GRIP = (26, 28, 34)
GRIP_WRAP = (58, 62, 72)
GUARD = (72, 62, 40)
GUARD_LIGHT = (156, 132, 84)


def pick_font(size):
    for path in FONT_CANDIDATES:
        if not os.path.isfile(path):
            continue
        try:
            font = ImageFont.truetype(path, size)
            if font.getmask(NIN).getbbox() is not None:
                return font
        except OSError:
            continue
    return None


def draw_kanji(img, S, detail):
    """The kanji, filling most of the square, behind everything else."""
    # 0.80, not 0.86: at 0.86 the glyph's own side bearings put the outer
    # strokes hard against the canvas edge and the ICO frame clips them.
    font_px = int(S * 0.80)
    font = pick_font(font_px)
    if font is None:
        return False

    # Render on its own layer so it can be centred by its INK bounds. The glyph
    # box carries font-wide leading that is not symmetric, so centring on the
    # box leaves the character visibly high.
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.text((S // 2, S // 2), NIN, font=font, fill=RED + (255,), anchor="mm")

    box = layer.getbbox()
    if box:
        cx = (box[0] + box[2]) / 2.0
        cy = (box[1] + box[3]) / 2.0
        layer = layer.transform(
            (S, S), Image.AFFINE,
            (1, 0, cx - S / 2.0, 0, 1, cy - S / 2.0),
            resample=Image.BICUBIC)

    if detail:
        # A dark rim so the red still reads against a red or dark wallpaper.
        rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        rd = ImageDraw.Draw(rim)
        off = max(1, S // 128)
        for dx, dy in ((-off, 0), (off, 0), (0, -off), (0, off)):
            rd.text((S // 2 + dx, S // 2 + dy), NIN, font=font,
                    fill=RED_DARK + (255,), anchor="mm")
        if box:
            rim = rim.transform((S, S), Image.AFFINE,
                                (1, 0, cx - S / 2.0, 0, 1, cy - S / 2.0),
                                resample=Image.BICUBIC)
        img.alpha_composite(rim)
    img.alpha_composite(layer)
    return True


def blade_polygon(S, detail):
    """Katana from lower-left to upper-right, with the shallow curve (sori)
    that distinguishes a katana from a straight sword - at 16px it is the only
    thing that still says 'katana' rather than 'stick'."""
    ang = math.radians(38.0)
    ca, sa = math.cos(ang), math.sin(ang)

    # Along-blade axis, in units of the square.
    #
    # A katana's blade is roughly three times its handle. The first attempt had
    # them nearly equal, which reads as a rifle rather than a sword - the single
    # most damaging error in the drawing, and obvious only once it was rendered.
    start = -0.40           # where the handle butt sits
    guard = -0.22           # blade 0.84 long against a 0.18 handle
    tip = 0.62
    # Thicker when DETAILED, not thinner. This was inverted at first, so the
    # 256px icon - the one that gets looked at - had the faintest blade of all.
    # Length-to-width ~10:1. A real katana is nearer 20:1, which is too fine to
    # survive an icon, but 6:1 - where this started - reads as a machete.
    half = 0.042 if detail else 0.058
    # A katana's curve is SHALLOW. At 0.075 this drew a shamshir - the deep
    # crescent of a Persian sabre - which is a different weapon entirely.
    sori = 0.032 if detail else 0.026

    cx, cy = S * 0.50, S * 0.52

    def pt(t, off):
        # t along the blade, off perpendicular; the curve bows the spine.
        bow = sori * math.sin(math.pi * (t - start) / (tip - start))
        x = cx + (t * ca - (off + bow) * sa) * S
        y = cy - (t * sa + (off + bow) * ca) * S
        return (x, y)

    steps = 24 if detail else 10
    guard_t = guard

    spine, edge = [], []
    for i in range(steps + 1):
        t = guard_t + (tip - guard_t) * i / steps
        # taper to the point
        k = 1.0 - 0.82 * (i / steps) ** 5.0
        spine.append(pt(t, half * k))
        edge.append(pt(t, -half * k))
    return spine + list(reversed(edge)), pt, half, guard_t, start


def draw_katana(img, S, detail):
    d = ImageDraw.Draw(img)
    poly, pt, half, guard_t, start = blade_polygon(S, detail)

    # Blade body.
    d.polygon(poly, fill=STEEL_LIGHT)

    if detail:
        # Hamon: the temper line following the edge, and a shade along the
        # spine so the blade reads as a wedge rather than a flat strip.
        steps = 24
        tip = 0.62
        hamon = []
        for i in range(steps + 1):
            t = guard_t + (tip - guard_t) * i / steps
            k = 1.0 - 0.82 * (i / steps) ** 5.0
            hamon.append(pt(t, -half * k * 0.30))
        d.line(hamon, fill=STEEL_MID, width=max(1, S // 110))
        d.line([pt(guard_t, half * 0.98), pt(0.615, 0.0)],
               fill=STEEL_MID, width=max(1, S // 150))

    # Outline, so the blade separates from the red behind it.
    d.line(poly + [poly[0]], fill=STEEL_EDGE, width=max(1, S // 64))

    # Guard (tsuba) and handle (tsuka).
    gx, gy = pt(guard_t, 0.0)
    r = S * (0.050 if detail else 0.062)
    d.ellipse([gx - r, gy - r, gx + r, gy + r],
              fill=GUARD_LIGHT if detail else GUARD, outline=GUARD,
              width=max(1, S // 128))

    hw = S * (0.028 if detail else 0.036)
    ang = math.radians(38.0)
    ca, sa = math.cos(ang), math.sin(ang)
    hx, hy = pt(start, 0.0)
    perp = (-sa * hw, -ca * hw)
    grip = [(gx + perp[0], gy + perp[1]), (hx + perp[0], hy + perp[1]),
            (hx - perp[0], hy - perp[1]), (gx - perp[0], gy - perp[1])]
    d.polygon(grip, fill=GRIP)

    if detail:
        # Diamond wrap, drawn as crossing bands. Dropped below 32px, where the
        # bands land sub-pixel and just lighten the handle into grey.
        n = 5
        for i in range(1, n):
            f = i / float(n)
            ax = gx + (hx - gx) * f
            ay = gy + (hy - gy) * f
            d.line([(ax + perp[0], ay + perp[1]), (ax - perp[0], ay - perp[1])],
                   fill=GRIP_WRAP, width=max(1, S // 200))


def render(S, detail=None):
    if detail is None:
        detail = S >= 32
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if not draw_kanji(img, S, detail):
        print("WARNING: no Japanese font found - the icon will have no kanji")
    draw_katana(img, S, detail)
    return img


def check_sheet(path):
    """Magnify the small sizes on light AND dark. An icon judged only at 256px
    is judged at the one size nobody sees it."""
    sizes = [16, 24, 32, 48]
    zoom = 8
    pad = 12
    w = sum(s * zoom for s in sizes) + pad * (len(sizes) + 1)
    h = max(sizes) * zoom * 2 + pad * 3
    sheet = Image.new("RGB", (w, h), (128, 128, 128))
    for band, bg in enumerate([(245, 245, 245), (24, 24, 28)]):
        x = pad
        y = pad + band * (max(sizes) * zoom + pad)
        for s in sizes:
            tile = Image.new("RGB", (s * zoom, s * zoom), bg)
            big = render(s).resize((s * zoom, s * zoom), Image.NEAREST)
            tile.paste(big, (0, 0), big)
            sheet.paste(tile, (x, y))
            x += s * zoom + pad
    sheet.save(path)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None, help="path of the .ico to write")
    ap.add_argument("--preview", action="store_true",
                    help="also write the magnified small-size check sheet")
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    res = os.path.join(here, "resources")
    os.makedirs(res, exist_ok=True)
    out = args.out or os.path.join(res, "ng2.ico")

    frames = [render(s) for s in SIZES]
    # Pillow writes every requested size from the image it is given, so hand it
    # the largest and list the sizes; but that would DOWNSAMPLE and lose the
    # simplified small frames. Save the frames explicitly instead.
    frames[-1].save(out, format="ICO",
                    sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    print("wrote %s (%s)" % (out, ", ".join("%dx%d" % (s, s) for s in SIZES)))

    png = os.path.join(res, "ng2_256.png")
    frames[SIZES.index(256)].save(png)
    print("wrote %s" % png)

    if args.preview:
        p = check_sheet(os.path.join(res, "icon_check.png"))
        print("wrote %s  - magnified 16/24/32/48 on light and dark" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
