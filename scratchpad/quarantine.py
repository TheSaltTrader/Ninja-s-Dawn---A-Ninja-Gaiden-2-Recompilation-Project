# Move pack textures aside (never delete) and bring them back, for bisecting a bad id.
# Usage:
#   python quarantine.py out <ids.txt> [<tag>]   move every id listed (first column) from pack -> quarantine/<tag>
#   python quarantine.py back [<tag>]            move everything in quarantine/<tag> back into the pack
#   python quarantine.py status                  counts per quarantine folder
import os, sys, shutil
PACK = "C:/ng2tex/pack"; Q = "C:/ng2tex/quarantine"

def ids_from(path):
    out = []
    for line in open(path):
        p = line.split()
        if p and len(p[0]) == 16: out.append(p[0])
    return out

cmd = sys.argv[1]
if cmd == "out":
    tag = sys.argv[3] if len(sys.argv) > 3 else "q"
    dst = os.path.join(Q, tag); os.makedirs(dst, exist_ok=True)
    moved = missing = 0
    for tid in ids_from(sys.argv[2]):
        src = os.path.join(PACK, tid + ".tex")
        if os.path.isfile(src):
            shutil.move(src, os.path.join(dst, tid + ".tex")); moved += 1
        else:
            missing += 1
    print("moved %d to %s (%d not in pack)" % (moved, dst, missing))
elif cmd == "back":
    tag = sys.argv[2] if len(sys.argv) > 2 else "q"
    src = os.path.join(Q, tag); n = 0
    for fn in os.listdir(src) if os.path.isdir(src) else []:
        if fn.endswith(".tex"):
            shutil.move(os.path.join(src, fn), os.path.join(PACK, fn)); n += 1
    print("restored %d from %s" % (n, src))
elif cmd == "status":
    if os.path.isdir(Q):
        for tag in sorted(os.listdir(Q)):
            print(tag, len([f for f in os.listdir(os.path.join(Q, tag)) if f.endswith(".tex")]))
    print("pack:", len([f for f in os.listdir(PACK) if f.endswith(".tex")]))
