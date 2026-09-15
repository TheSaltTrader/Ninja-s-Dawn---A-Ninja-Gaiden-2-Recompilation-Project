"""Turn the [ngpu] census lines of a game log into a report: per hooked
function its label, total calls, the peak and last 10-s call rates, the
argument samples, and whether r3 is the device (the most common r3 among
library entry points), a resource pointer, or a small value.
usage: census_report.py <log> [--out report.md]
"""
import re, sys
from collections import Counter, defaultdict

log = sys.argv[1]
out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
LINE = re.compile(r"\[ngpu\] sub_([0-9A-F]{8}) (.*?) calls=(\d+) \+(\d+)((?: \[[0-9A-F ]+\])*)$")
CENSUS = re.compile(r"\[ngpu\] census at ([0-9.]+) s")
funcs = {}
order = []
t = 0.0
for raw in open(log, "r", encoding="utf-8", errors="replace"):
    raw = raw.rstrip("\r\n")
    m = CENSUS.search(raw)
    if m:
        t = float(m.group(1)); continue
    m = LINE.search(raw)
    if not m:
        continue
    addr, label, total, delta, samples = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)
    f = funcs.get(addr)
    if f is None:
        f = funcs[addr] = {"label": label, "total": 0, "peak": 0, "last": 0, "samples": [], "first_t": t, "series": []}
        order.append(addr)
    f["total"] = total; f["last"] = delta; f["peak"] = max(f["peak"], delta)
    f["series"].append((t, delta))
    for s in re.findall(r"\[([0-9A-F ]+)\]", samples):
        f["samples"].append([int(x, 16) for x in s.split()])
# the device pointer = most common r3 among library entry points
r3s = Counter()
for addr, f in funcs.items():
    if f["label"].startswith("lib"):
        for s in f["samples"]:
            r3s[s[0]] += 1
device = r3s.most_common(1)[0][0] if r3s else 0
def kind(v):
    if v == device: return "DEV"
    if 0x40000000 <= v < 0xC0000000: return "ptr"
    if 0x82000000 <= v < 0x84000000: return "static"
    if v < 0x10000: return "small"
    return "?"
rows = []
for addr in sorted(order, key=lambda a: -funcs[a]["peak"]):
    f = funcs[addr]
    ex = f["samples"][0] if f["samples"] else []
    shape = " ".join(kind(v) for v in ex[:6])
    smp = " ".join(f"{v:08X}" for v in ex[:6])
    rows.append(f"| sub_{addr} | {f['label']} | {f['total']} | {f['peak']} | {f['last']} | {shape} | {smp} |")
hdr = (f"device pointer (most common r3 of library entry points): 0x{device:08X}\n\n"
       "| function | label | total calls | peak calls/10 s | last 10 s | arg kinds (r3..r8 of first sample) | first sample |\n"
       "| --- | --- | --- | --- | --- | --- | --- |")
text = hdr + "\n" + "\n".join(rows) + "\n"
never = 223 - len(funcs)
text += f"\nhooked functions never called in this run: {never}\n"
if out:
    open(out, "w", encoding="utf-8").write(text)
print(text)
