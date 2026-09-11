"""Which call inside sub_822F16D0's stuck region can block?

The worker has no early return between loc_822F1880 and the clear, so it cannot
leave the function without clearing the flag. It is therefore blocked inside a
call. This walks the call tree from that region, two levels deep, looking for
the kernel imports that can actually wait - which is a much smaller set than
"everything it calls".
"""
import glob
import os
import re

GEN = (r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
       r"\generated\default")

BLOCKING = re.compile(
    r"__imp__(KeWaitForSingleObject|KeWaitForMultipleObjects|"
    r"NtWaitForSingleObjectEx|NtWaitForMultipleObjectsEx|"
    r"NtSignalAndWaitForSingleObjectEx|KeDelayExecutionThread|"
    r"XMsgWait\w*|KeEnterCriticalRegion|RtlEnterCriticalSection\w*)")
CALL = re.compile(r"^\t(sub_[0-9A-F]{8})\(ctx, base\);", re.M)
DEF = re.compile(r"DEFINE_REX_FUNC\((\w+)\)")

# Index every function body once.
bodies = {}
for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    text = open(path, encoding="utf-8", errors="replace").read()
    parts = DEF.split(text)
    # parts = [pre, name1, body1, name2, body2, ...]
    for i in range(1, len(parts) - 1, 2):
        bodies[parts[i]] = parts[i + 1]
print("indexed %d functions" % len(bodies))

# The region of interest, taken from the worker's own body.
worker = bodies["sub_822F16D0"]
start = worker.index("loc_822F1880:")
end = worker.index("loc_822F21B0:")
region = worker[start:end]
direct = sorted(set(CALL.findall(region)))
print("direct calls in the stuck region: %d" % len(direct))

seen = set()
findings = []


def scan(name, depth, path):
    if depth > 2 or name in seen:
        return
    seen.add(name)
    body = bodies.get(name)
    if body is None:
        return
    for imp in set(BLOCKING.findall(body)):
        findings.append((" -> ".join(path + [name]), imp))
    for callee in set(CALL.findall(body)):
        scan(callee, depth + 1, path + [name])


for fn in direct:
    scan(fn, 1, ["sub_822F16D0"])

print("\n=== blocking imports reachable within 2 levels ===")
if not findings:
    print("  none - the worker does not reach a kernel wait from this region")
for chain, imp in sorted(set(findings)):
    print("  %-14s via %s" % (imp, chain))
