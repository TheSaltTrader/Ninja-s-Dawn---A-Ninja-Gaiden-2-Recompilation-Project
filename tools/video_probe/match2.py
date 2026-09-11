"""Match the dumped planes against ground truth with a ROBUST metric.

The first attempt used mean absolute difference and found nothing. That was the
wrong tool: if a quarter of the pixels are corrupt, the mean is dominated by
them and buries a real match. The median ignores a corrupted minority entirely,
so if 60% of the picture is right the median difference collapses to near zero
for the correct frame and stays high for every other one.

The chance baseline - the same score between two unrelated real frames - is
computed alongside, because a number is only evidence next to what it would be
if there were nothing there.
"""
import numpy as np, glob, os

W, H = 320, 580
YS, CS = W * H, (W // 2) * (H // 2)
FS = YS + 2 * CS
VIDS = ("NinjaVI", "NinjaVI_Left", "NinjaVI_Right")

mm, Y = {}, {}
for n in VIDS:
    a = np.memmap("gt/%s.yuv" % n, dtype=np.uint8, mode="r")
    nf = len(a) // FS
    mm[n] = a[: nf * FS].reshape(nf, FS)
    Y[n] = mm[n][:, :YS].reshape(nf, H, W)

rng = np.random.default_rng(0)
base = []
for n in VIDS:
    nf = Y[n].shape[0]
    for _ in range(40):
        i, j = rng.integers(0, nf, 2)
        if abs(int(i) - int(j)) < 30:
            continue
        d = np.abs(Y[n][i].astype(np.int16) - Y[n][j].astype(np.int16))
        base.append((np.median(d), d.mean()))
base = np.array(base)
print("chance baseline, two unrelated real frames:  median %.1f   mean %.1f"
      % (base[:, 0].mean(), base[:, 1].mean()))
print()

for path in sorted(glob.glob(r"C:\ng2dump\raw_320x580_p512_*.bin")):
    plane = np.fromfile(path, np.uint8).reshape(H, 512)[:, :W].astype(np.int16)
    print("=== %s" % os.path.basename(path))
    best = None
    for n in VIDS:
        v = Y[n]
        # Coarse pass on 8x-downsampled frames using the median, then refine.
        small_p = plane[::4, ::4]
        d = np.median(np.abs(v[:, ::4, ::4].astype(np.int16) - small_p), axis=(1, 2))
        order = np.argsort(d)[:5]
        for i in order:
            full = np.abs(v[i].astype(np.int16) - plane)
            med = float(np.median(full))
            frac = float((full <= 2).mean())
            if best is None or med < best[0]:
                best = (med, n, int(i), frac, float(full.mean()))
    med, n, i, frac, mean = best
    print("    best: %-14s frame %4d   MEDIAN diff %5.1f   mean %5.1f   "
          "pixels within 2: %.1f%%" % (n, i, med, mean, 100 * frac))
    print()
