"""Digest a verbose (debug) game log: count message SHAPES per time window.

    python local/diag/log_digest.py <log> --from "13:24:32" [--window 30] [--top 25]

Shape = the message with timestamps, thread ids, hex numbers and decimals
replaced by placeholders, so 60 identical calls a second collapse to one row
with a count. The point is to see what the game asks the runtime for in a
stuck state, against what it asked for while things were moving.
"""
import argparse
import collections
import re
import sys

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\.(\d+)\] \[(\w+)\] \[(\w+)\] \[t(\d+)\] (.*)$")
NORM = [
    (re.compile(r"0x[0-9A-Fa-f]+"), "0xN"),
    (re.compile(r"\b[0-9A-Fa-f]{8}\b"), "HEX8"),
    (re.compile(r"\b\d+\.\d+\b"), "F"),
    (re.compile(r"\b\d+\b"), "N"),
]


def shape(msg):
    s = msg
    for rx, rep in NORM:
        s = rx.sub(rep, s)
    return s[:110]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="start", required=True, help="HH:MM:SS")
    ap.add_argument("--to", dest="end", default="23:59:59")
    ap.add_argument("--window", type=int, default=30, help="seconds per bucket")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--skip-fps", action="store_true", default=True)
    args = ap.parse_args()

    def secs(t):
        h, m, s = t.split(":")
        return int(h) * 3600 + int(m) * 60 + int(s)

    t0, t1 = secs(args.start), secs(args.end)
    buckets = collections.OrderedDict()
    levels = collections.Counter()
    with open(args.log, errors="replace") as fh:
        for line in fh:
            m = TS.match(line.rstrip("\n"))
            if not m:
                continue
            t = secs(m.group(2))
            if t < t0 or t > t1:
                continue
            level, cat, msg = m.group(4), m.group(5), m.group(7)
            if "fps (" in msg or "PROGRESS" in msg:
                continue
            b = (t - t0) // args.window
            key = "%s/%s %s" % (level, cat, shape(msg))
            buckets.setdefault(b, collections.Counter())[key] += 1
            levels[level] += 1
    print("levels:", dict(levels))
    for b, c in buckets.items():
        lo = t0 + b * args.window
        print("\n=== +%ds (%02d:%02d:%02d) - %d lines, %d shapes ===" % (
            b * args.window, lo // 3600, lo % 3600 // 60, lo % 60, sum(c.values()), len(c)))
        for k, n in c.most_common(args.top):
            print("  %6d  %s" % (n, k))
    return 0


if __name__ == "__main__":
    sys.exit(main())
