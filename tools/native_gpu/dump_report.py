"""Summarise a native-GPU draw dump (ngpu_dump_<n>.txt from
fable2recomp/src/native_gpu_dump.cpp): per frame the draw count by kind and
primitive, the distinct object keys (vertex fetch slots' word 0, index
buffer word 0, vs, ps), how many keys reappear in the next frame (the
object match rate the NG2 frame-interpolation work measures), the fetch
slots in use and the shader pairs.
usage: dump_report.py <ngpu_dump.txt> [--out report.md]
"""
import re, sys
from collections import Counter, defaultdict

path = sys.argv[1]
out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
frames = defaultdict(list)   # frame -> [draw dict]
kinds = Counter(); prims = Counter(); slots = Counter(); pairs = Counter()
D = re.compile(r"^D f(\d+) (\w+) prim=(\d+) base=(\d+) start=(\d+) count=(\d+) ib=([0-9A-F]+):([0-9A-F]+)/([0-9A-F]+) vs=([0-9A-F]+) ps=([0-9A-F]+) ct=([0-9A-F]+)\+([0-9A-F]+):(\d+) rt=([0-9A-F]+):(\d+) pred=([0-9A-F]+)(.*)$")
for line in open(path, encoding="utf-8", errors="replace"):
    m = D.match(line.rstrip())
    if not m:
        continue
    f = int(m.group(1))
    fcs = {}
    for a, words in re.findall(r" fc(\d+)=([0-9A-F/]+)", m.group(18)):
        fcs[int(a)] = tuple(words.split("/"))
    d = {"kind": m.group(2), "prim": int(m.group(3)), "count": int(m.group(6)), "ib": m.group(8),
         "vs": m.group(10), "ps": m.group(11), "ct": m.group(12), "rt": m.group(15), "fc": fcs}
    # object key: the vertex-fetch constants = every 2-word (address|type 3, size) pair in the
    # 192-word fetch block whose type bits are 3 (slot words are read in pairs), + ib + shaders;
    # texture fetches = the 6-word slots whose word 0 has type bits 2.
    vfetch, tfetch = [], []
    for i, w in sorted(fcs.items()):
        if int(w[0], 16) & 3 == 2:
            tfetch.append((i, w[1] if len(w) > 1 else w[0]))
        for k in range(0, len(w) - 1, 2):
            if int(w[k], 16) & 3 == 3:
                vfetch.append((i * 6 + k, w[k]))
    vfetch = tuple(vfetch); tfetch = tuple(tfetch)
    # Fable II streams some vertex data through a per-frame ring (the stream 0..2
    # pairs in slot 31 change address every frame), so the stable mesh identity
    # is the index buffer's physical address; "key" = (ib, vs, ps), "key_tex" adds
    # every vertex fetch pair (which shows how much of the geometry is per-frame).
    d["key"] = (d["ib"], d["vs"], d["ps"])
    d["key_tex"] = (vfetch, d["ib"], d["vs"], d["ps"])
    frames[f].append(d)
    kinds[d["kind"]] += 1; prims[d["prim"]] += 1
    for i in fcs: slots[i] += 1
    pairs[(d["vs"], d["ps"])] += 1
lines = [f"# draw dump report: {path}", "",
         f"frames: {len(frames)}  draws: {sum(len(v) for v in frames.values())}  kinds: {dict(kinds)}  prims: {dict(sorted(prims.items()))}",
         f"fetch slots used (slot: draws): {dict(sorted(slots.items()))}",
         f"distinct shader pairs: {len(pairs)}  (top: {pairs.most_common(5)})", ""]
fs = sorted(frames)
lines.append("| frame | draws | distinct keys (ib,vs,ps) | keys also in next frame | match % | keys with every vertex fetch pair | match % |")
lines.append("| --- | --- | --- | --- | --- | --- | --- |")
for a, b in zip(fs, fs[1:]):
    ka = {d["key"] for d in frames[a]}; kb = {d["key"] for d in frames[b]}
    ta = {d["key_tex"] for d in frames[a]}; tb = {d["key_tex"] for d in frames[b]}
    lines.append(f"| {a} | {len(frames[a])} | {len(ka)} | {len(ka & kb)} | {100 * len(ka & kb) // max(1, len(ka))} | {len(ta)} | {100 * len(ta & tb) // max(1, len(ta))} |")
if fs:
    lines.append(f"| {fs[-1]} | {len(frames[fs[-1]])} | {len({d['key'] for d in frames[fs[-1]]})} | - | - | - | - |")
# duplicate keys within a frame (the same object drawn twice: tiling or multipass)
if fs:
    c = Counter(d["key"] for d in frames[fs[0]])
    dup = sum(1 for k, n in c.items() if n > 1)
    lines.append("")
    lines.append(f"frame {fs[0]}: {dup} keys drawn more than once (max {max(c.values()) if c else 0} times); "
                 f"draws without any vertex fetch slot: {sum(1 for d in frames[fs[0]] if not d['key'][0])}")
text = "\n".join(lines) + "\n"
if out:
    open(out, "w", encoding="utf-8").write(text)
print(text)
