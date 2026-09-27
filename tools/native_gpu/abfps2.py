"""abfps2.py - the timing pair from NG2 game logs: baseline (plugin alone) vs offload (native backend is the GPU).

    python tools/native_gpu/abfps2.py <log> [<log> ...]        one column per leg, windows aligned by time since
                                                                the first [swap] line of the leg
    python tools/native_gpu/abfps2.py --latest 4                the newest N logs beside the exe

Reads every "[swap] N guest fps (S swaps in 5.0s)  interval ms: p50 A  p99 B  worst C  hitches H" window (the
plugin's line; under the DLL the vendored command processor prints an identical second copy, dropped as an exact
duplicate), the per-frame "[ngpu] GPU TIME" and "CPU COST" lines of the native backend, and the "[diag]" frame-ms
lines. Prints one row per 5 s window with time since the leg's first swap, and a summary over the windows from
+40 s on (the title + attract phase; before that the boot cutscene runs at its own 30 fps).

Read the numbers as PAIRS (baseline beside offload), never merged. fps deltas do not compose; ms do.
"""
import glob
import os
import re
import statistics
import sys

# The log stamps "[2026-09-26 17:33:13.896]": date, then time.
STAMP = r"\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)\.(\d+)\]"
SWAP = re.compile(STAMP + r".*\[swap\] ([0-9.]+) guest fps \((\d+) swaps in ([0-9.]+)s\)\s+interval ms: p50 ([0-9.]+)\s+p99 ([0-9.]+)\s+worst ([0-9.]+)\s+hitches (\d+)")
GPU = re.compile(STAMP + r".*\[ngpu\] GPU TIME: ([0-9.]+) ms of backend GPU work per frame")
CPU = re.compile(STAMP + r".*\[ngpu\] CPU COST per frame: plugin GPU thread \(PM4 parse \+ backend draws\) ([0-9.]+) ms, submit thread ([0-9.]+) ms")
MODE = re.compile(r"\[ngpu\] plugin gpu_offload_to_native = '([a-z0-9]+)'")
DLL = re.compile(r"ngpu_backend\.dll (loaded|started)")


def secs(hms, frac):
    h, m, s = (int(x) for x in hms.split(":"))
    return h * 3600 + m * 60 + s + float("0." + frac)


def parse(path):
    windows, gpu, cpu = [], {}, {}
    mode = "no native"
    last = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = SWAP.search(line)
        if m:
            key = m.group(0)[m.group(0).find("[swap]"):]
            if key == last:      # the vendored copy's duplicate report
                continue
            last = key
            windows.append((secs(m.group(1), m.group(2)), float(m.group(3)), int(m.group(4)), float(m.group(6)),
                            float(m.group(7)), float(m.group(8)), int(m.group(9))))
            continue
        m = GPU.search(line)
        if m:   # STAMP holds groups 1-2 (time, fraction); the value is group 3
            gpu[secs(m.group(1), m.group(2))] = float(m.group(3))
            continue
        m = CPU.search(line)
        if m:
            cpu[secs(m.group(1), m.group(2))] = (float(m.group(3)), float(m.group(4)))
            continue
        m = MODE.search(line)
        if m:
            mode = "offload" if m.group(1) in ("true", "1") else "lockstep"
        elif DLL.search(line) and mode == "no native":
            mode = "lockstep"
    return mode, windows, gpu, cpu


def nearest(table, t, tol=3.0):
    best = None
    for k, v in table.items():
        if abs(k - t) <= tol and (best is None or abs(k - t) < abs(best[0] - t)):
            best = (k, v)
    return best[1] if best else None


def main():
    args = sys.argv[1:]
    steady_from = 40.0
    if args and args[0] == "--from":   # the summary window's start, seconds after the first swap (gameplay: ~80)
        steady_from = float(args[1])
        args = args[2:]
    if args and args[0] == "--latest":
        n = int(args[1]) if len(args) > 1 else 4
        root = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                            "out", "build", "win-amd64-Release", "logs")
        args = sorted(glob.glob(os.path.join(root, "ng2_*.log")), key=os.path.getmtime)[-n:]
    if not args:
        print(__doc__)
        sys.exit(1)
    for path in args:
        mode, windows, gpu, cpu = parse(path)
        print("=== %s  (%s)  %d windows ===" % (os.path.basename(path), mode, len(windows)))
        if not windows:
            continue
        t0 = windows[0][0]
        print("   +s   fps  swaps  p50ms  p99ms worst hitch  gpu-ms  cpu-gpu-thread-ms  submit-ms")
        for (t, fps, swaps, p50, p99, worst, hitch) in windows:
            g = nearest(gpu, t)
            c = nearest(cpu, t)
            print("  %4.0f  %5.1f  %5d  %5.1f  %5.1f %5.1f %5d   %s   %s   %s" % (
                t - t0, fps, swaps, p50, p99, worst, hitch,
                ("%5.2f" % g) if g is not None else "  -  ",
                ("%5.2f" % c[0]) if c else "  -  ", ("%5.2f" % c[1]) if c else "  -  "))
        steady = [w for w in windows if w[0] - t0 >= steady_from]
        if steady:
            fps = [w[1] for w in steady]
            p50 = [w[3] for w in steady]
            p99 = [w[4] for w in steady]
            worst = [w[5] for w in steady]
            print("  from +%.0f s: n=%d  fps mean %.2f median %.2f min %.1f max %.1f spread %.1f | p50 ms median %.2f | "
                  "p99 ms median %.2f max %.1f | worst ms max %.1f | hitches %d" % (
                      steady_from, len(fps), statistics.mean(fps), statistics.median(fps), min(fps), max(fps), max(fps) - min(fps),
                      statistics.median(p50), statistics.median(p99), max(p99), max(worst), sum(w[6] for w in steady)))
            gs = [v for k, v in gpu.items() if k - t0 >= 40.0]
            cs = [v for k, v in cpu.items() if k - t0 >= 40.0]
            if gs:
                print("  native backend from +40 s: GPU ms/frame median %.2f max %.2f" % (statistics.median(gs), max(gs)))
            if cs:
                print("  native backend from +40 s: plugin GPU thread CPU ms/frame median %.2f max %.2f, submit thread median %.2f"
                      % (statistics.median([c[0] for c in cs]), max(c[0] for c in cs), statistics.median([c[1] for c in cs])))


if __name__ == "__main__":
    main()
