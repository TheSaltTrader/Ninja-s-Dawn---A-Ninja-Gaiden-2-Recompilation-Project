"""Score captured intro frames for corruption, so variants can be compared
without a human looking at them.

The failure mode is vertical: adjacent ROWS differ wildly while adjacent
COLUMNS barely differ at all. A clean frame has those in the same ballpark
(the menu measures ~0.8 row vs ~1.1 col). So the headline number is the
row/col gradient ratio - 1.0 is clean, 30+ is the corrupted intro.

Frames that are essentially black, and the trailing menu frames, are excluded
so the score describes the video only.
"""
import glob, os, sys
import numpy as np
from PIL import Image

def score(pattern):
    rows = []
    for p in sorted(glob.glob(pattern)):
        a = np.asarray(Image.open(p).convert("RGB")).astype(np.int16)
        lum = a.mean(axis=2)
        ys, xs = np.where(lum > 12)
        if len(ys) < 5000:
            continue
        y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
        sub = lum[y0:y1+1, x0:x1+1]
        rd = float(np.abs(np.diff(sub, axis=0)).mean())
        cd = float(np.abs(np.diff(sub, axis=1)).mean())
        sat = float((a.max(axis=2) - a.min(axis=2))[y0:y1+1, x0:x1+1].mean())
        rows.append((os.path.basename(p), rd, cd, rd / max(cd, 1e-3), sat))
    if not rows:
        return None
    # the menu is clean and coloured; drop the tail so it cannot flatter a score
    vid = [r for r in rows if r[3] > 3.0]
    if not vid:
        vid = rows
    ratio = float(np.median([r[3] for r in vid]))
    rd = float(np.median([r[1] for r in vid]))
    sat = float(np.median([r[4] for r in vid]))
    return ratio, rd, sat, len(vid), len(rows)

if __name__ == "__main__":
    pat = sys.argv[1] if len(sys.argv) > 1 else "out/shots/*.png"
    r = score(pat)
    if r is None:
        print("no usable frames"); sys.exit(1)
    ratio, rd, sat, nvid, ntot = r
    print("ratio %7.2f   row|diff| %6.2f   sat %6.2f   (%d video / %d frames)"
          % (ratio, rd, sat, nvid, ntot))
