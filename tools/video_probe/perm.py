"""Does some byte permutation of the dumped plane turn it back into video?

"Gross layout preserved, fine detail scrambled horizontally" is the signature of
a store whose byte order is wrong. On this target that has a specific shortlist:
a 4-byte word reversal (a 32-bit store byte-swapped), an 8-byte reversal, or a
16-byte reversal - the last being a VMX 128-bit store with the lanes the wrong
way round, which is exactly the big-endian lane trap this port has hit before.

Scored by Pearson correlation against ground truth, which is immune to the
brightness offsets that made the plain-difference match useless, and only over
frames with real contrast so a grey fade cannot win by being featureless.
"""
import numpy as np, glob, os

W, H = 320, 580
YS, CS = W * H, (W // 2) * (H // 2)
FS = YS + 2 * CS
VIDS = ("NinjaVI", "NinjaVI_Left", "NinjaVI_Right")

frames = []
for n in VIDS:
    a = np.memmap("gt/%s.yuv" % n, dtype=np.uint8, mode="r")
    nf = len(a) // FS
    Y = a[: nf * FS].reshape(nf, FS)[:, :YS].reshape(nf, H, W)
    for i in range(0, nf, 3):                     # every 3rd frame is plenty
        f = Y[i]
        s = f[::4, ::4].astype(np.float32)
        if s.std() > 40.0:                        # real content only
            frames.append((n, i, s))
print("ground-truth frames with contrast: %d" % len(frames))
G = np.stack([f[2].ravel() for f in frames])
G = (G - G.mean(axis=1, keepdims=True)) / (G.std(axis=1, keepdims=True) + 1e-6)

def rev(a, k):
    """Reverse every k-byte group along the row."""
    r = a.reshape(H, -1, k)[:, :, ::-1]
    return r.reshape(H, -1)

def swap2(a):
    return rev(a, 2)

TRANSFORMS = {
    "identity":            lambda a: a,
    "reverse 2-byte":      lambda a: rev(a, 2),
    "reverse 4-byte":      lambda a: rev(a, 4),
    "reverse 8-byte":      lambda a: rev(a, 8),
    "reverse 16-byte":     lambda a: rev(a, 16),
    "swap 4-byte halves":  lambda a: a.reshape(H, -1, 4)[:, :, [2, 3, 0, 1]].reshape(H, -1),
    "swap 8-byte halves":  lambda a: a.reshape(H, -1, 8)[:, :, [4,5,6,7,0,1,2,3]].reshape(H, -1),
}

for path in sorted(glob.glob(r"C:\ng2dump\raw_320x580_p512_*.bin")):
    full = np.fromfile(path, np.uint8).reshape(H, 512)
    print("=== %s" % os.path.basename(path))
    for name, fn in TRANSFORMS.items():
        t = fn(full)[:, :W].astype(np.float32)
        s = t[::4, ::4].ravel()
        s = (s - s.mean()) / (s.std() + 1e-6)
        c = G @ s / s.size
        i = int(c.argmax())
        print("    %-20s best r = %+.3f   (%s frame %d)"
              % (name, c[i], frames[i][0], frames[i][1]))
    print()
