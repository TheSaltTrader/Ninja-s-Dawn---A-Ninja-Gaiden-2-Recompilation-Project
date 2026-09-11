"""Report how completely the decoder filled a dumped Y plane."""
import sys
import numpy as np

path = sys.argv[1]
label = sys.argv[2] if len(sys.argv) > 2 else path
try:
    d = np.frombuffer(open(path, 'rb').read(), dtype=np.uint8)
except OSError:
    print("  %-46s NO DUMP" % label); sys.exit(0)

P    = int(sys.argv[3]) if len(sys.argv) > 3 else 512
W    = int(sys.argv[4]) if len(sys.argv) > 4 else 320
REAL = int(sys.argv[5]) if len(sys.argv) > 5 else 580
rows = len(d) // P
img = d[:rows * P].reshape(rows, P)[:, :W]
nz = (img != 0).mean(axis=1)
blank = nz < 0.02
body = blank[:REAL]                      # ignore the padding rows past 580
pat = "".join("X" if not b else "." for b in body[:32])
print("  %-46s written=%5.1f%%  %s" % (label, 100 * (~body).mean(), pat))
