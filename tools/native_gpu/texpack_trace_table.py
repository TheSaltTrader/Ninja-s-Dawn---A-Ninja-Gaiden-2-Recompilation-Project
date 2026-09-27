#!/usr/bin/env python3
"""Per-reason table of the texture pack's resolve-at-load decisions from one or more run logs
(FABLE2_TEXPACK_TRACE=1 lines: "[texpack-trace] <id> WxH hash H prev P -> <what> <file>"), plus the coarse
counters ("N upscaled textures resolved at load", "hashed files indexed", "left alone"), plus the two homes of the
replaced counter if the PACK COUNTERS line exists. Usage: texpack_trace_table.py LOG [LOG...]"""
import re
import sys
from collections import Counter

RX = re.compile(r"^\[([^\]]+)\].*\[texpack-trace\] ([0-9A-F]{16}) (\d+)x(\d+) hash ([0-9A-F]{8}) prev ([0-9A-F]{8}) -> (\S+) ([0-9A-F]{16})")

for path in sys.argv[1:]:
    what = Counter()
    ids_replaced = set()
    files_replaced = set()
    first_replace = None
    last_ts = None
    sizes = Counter()
    lines = 0
    index_ts = None
    resolved_last = None
    left_alone_last = None
    home_lines = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RX.match(line)
            if m:
                lines += 1
                ts, tid, w, h, hsh, prev, kind, fid = m.groups()
                what[kind] += 1
                last_ts = ts
                if kind.startswith("replace"):
                    ids_replaced.add(tid)
                    files_replaced.add(fid)
                    sizes["%sx%s" % (w, h)] += 1
                    if first_replace is None:
                        first_replace = ts
                continue
            if "hashed files indexed" in line and index_ts is None:
                index_ts = line[1:24]
            mm = re.search(r"(\d+) upscaled textures resolved at load", line)
            if mm:
                resolved_last = int(mm.group(1))
            mm = re.search(r"(\d+) texture loads left alone", line)
            if mm:
                left_alone_last = int(mm.group(1))
            if "PACK COUNTERS" in line:
                home_lines.append(line.strip()[1:24] + " " + line.split("PACK COUNTERS:", 1)[1].strip())
    print("=== %s" % path)
    print("  trace lines: %d (cap 40000)   index at: %s   resolved-at-load counter last: %s   left-alone last: %s"
          % (lines, index_ts, resolved_last, left_alone_last))
    for k, v in sorted(what.items(), key=lambda kv: -kv[1]):
        print("  %-22s %7d" % (k, v))
    print("  distinct ids replaced: %d   distinct pack files served: %d   first replace: %s   last trace: %s"
          % (len(ids_replaced), len(files_replaced), first_replace, last_ts))
    top = sorted(sizes.items(), key=lambda kv: -kv[1])[:6]
    if top:
        print("  replaced sizes (top): " + ", ".join("%s x%d" % (s, n) for s, n in top))
    for hl in home_lines[-4:]:
        print("  homes: " + hl)
