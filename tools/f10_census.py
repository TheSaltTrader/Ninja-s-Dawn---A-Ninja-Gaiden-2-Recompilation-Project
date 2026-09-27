#!/usr/bin/env python3
"""F10 settings census: does every row of the in-game settings screen reach the code that acts on it?

For each RowStart(...) row in src/ng2_menu.cpp: the Ng2Settings fields it edits, the cvars the executable derives
from those fields (the startup tuning list in src/ng2_tuning.h and the live push in ApplyLiveSettings), and for
each cvar: where the SDK defines it (plugin src/graphics, runtime src/ui + src/runtime), its lifecycle, every read
site with its enclosing function (an init-time read means a change needs a restart), whether the row's own text
claims "immediately" or carries the restart tag, and whether the user's session log shows the value pushed at
startup. Rows with no cvar are the executable's own (window, frame rate, pointer, texture tool) and are listed as
such - the census counts what it cannot trace rather than hiding it.

Usage: f10_census.py [--sdk D:/ng2_frameinterp/rexglue-v1025] [--log <ng2_NNN.log>]"""
import argparse
import os
import re
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--sdk", default=r"D:\ng2_frameinterp\rexglue-v1025")
ap.add_argument("--log", default=None)
args = ap.parse_args()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
menu = open(os.path.join(ROOT, "src", "ng2_menu.cpp"), encoding="utf-8", errors="replace").read()
tuning = open(os.path.join(ROOT, "src", "ng2_tuning.h"), encoding="utf-8", errors="replace").read()
app = open(os.path.join(ROOT, "src", "ng2_app.h"), encoding="utf-8", errors="replace").read()

# ---- 1. rows -------------------------------------------------------------------------------------------------
starts = list(re.finditer(r'RowStart\("([^"]+)"', menu))
rows = []
for i, m in enumerate(starts):
    end = starts[i + 1].start() if i + 1 < len(starts) else min(len(menu), m.end() + 4000)
    seg = menu[m.start():end]
    fields = sorted(set(re.findall(r'&s\.([a-z_0-9]+)', seg) + re.findall(r'\bs\.([a-z_0-9]+)\s*=[^=]', seg)))
    literals = " ".join(re.findall(r'"((?:[^"\\]|\\.)*)"', seg))
    claims = [w for w in ("immediately", "next frame", "next launch", "restart", "at startup", "next paint",
                          "per swap") if w in literals]
    rows.append(dict(label=m.group(1), fields=fields, tag="RestartTag()" in seg, claims=claims))

# ---- 2. field -> cvar ----------------------------------------------------------------------------------------
field_cvars = {}   # field -> set of (cvar, how)
for m in re.finditer(r'out\.push_back\(\{"([a-z_0-9]+)",\s*([^}]*)\}', tuning, re.S):
    cvar, expr = m.group(1), m.group(2)
    for f in re.findall(r'\bs\.([a-z_0-9]+)', expr):
        field_cvars.setdefault(f, set()).add((cvar, "startup tuning"))
    # constants (no field) are the tuning's own pins
for m in re.finditer(r'SetCvar\("([a-z_0-9]+)",\s*([^;]*)\);', menu):
    cvar, expr = m.group(1), m.group(2)
    for f in re.findall(r'\bs\.([a-z_0-9]+)', expr):
        field_cvars.setdefault(f, set()).add((cvar, "live (ApplyLiveSettings)"))
    if not re.findall(r'\bs\.([a-z_0-9]+)', expr):
        field_cvars.setdefault("(constant)", set()).add((cvar, "live (ApplyLiveSettings), constant"))
# ApplyLiveSettings computes ng2_fov_k from s.ultrawide and s.monitor
if "ng2_fov_k" in menu:
    field_cvars.setdefault("ultrawide", set()).add(("ng2_fov_k", "live (ApplyLiveSettings)"))

# ---- 3. SDK consumers ----------------------------------------------------------------------------------------
sdk_files = []
for sub in ("src", "include"):
    for root, _, files in os.walk(os.path.join(args.sdk, sub)):
        for f in files:
            if f.endswith((".cpp", ".cc", ".h")):
                sdk_files.append(os.path.join(root, f))
sdk_text = {}
for p in sdk_files:
    try:
        sdk_text[p] = open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        pass
INIT_WORDS = ("Initialize", "Init(", "Setup", "Create", "::D3D12", "::Presenter(", "Constructor", "Reset(", "Shutdown",
              "ParseSwap", "CommandProcessor::Initialize", "GraphicsSystem::Setup", "InitializeCommon",
              "GetConfigDrawResolutionScale", "FromCVar")   # BuildGuestOutputPaintConfigFromCVar: called once at init
# The executable's own sources: cvars it defines itself (ng2_video_mode) and the settings fields it consumes.
exe_text = {}
for root, _, files in os.walk(os.path.join(ROOT, "src")):
    for f in files:
        if f.endswith((".cpp", ".h")):
            p = os.path.join(root, f)
            try:
                exe_text[p] = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                pass


def exe_field_reads(field):
    n = 0
    for p, t in exe_text.items():
        base = os.path.basename(p)
        if base in ("ng2_menu.cpp", "ng2_settings.h", "ng2_tuning.h"):
            continue
        n += len(re.findall(r'(?:\.|->)' + re.escape(field) + r'\b', t))
    return n


def enclosing_function(text, pos):
    head = text[:pos]
    for line in reversed(head.splitlines()[-400:]):
        mm = re.match(r'^[A-Za-z_][\w:<>,\s\*&]*?\b([A-Za-z_]\w*(?:::[A-Za-z_~]\w*)+)\s*\(', line)
        if mm and not line.strip().startswith(("if", "for", "while", "return", "//")):
            return mm.group(1)
        mm = re.match(r'^(?:static |inline |void |bool |int |uint\d+_t |std::\w+ )+([A-Za-z_]\w*)\s*\(', line)
        if mm:
            return mm.group(1)
        # `ReturnType Name(` at column 0, any return type (GuestOutputPaintConfig BuildGuestOutputPaintConfigFromCVar()
        mm = re.match(r'^[A-Za-z_][\w:<>]*\s+\*?([A-Za-z_]\w*)\s*\(', line)
        if mm and mm.group(1) not in ("if", "for", "while", "switch", "return"):
            return mm.group(1)
    return "?"


def sdk_lookup(cvar):
    defs, reads = [], []
    for p, t in list(sdk_text.items()) + list(exe_text.items()):
        rel = (os.path.relpath(p, args.sdk) if p.startswith(args.sdk) else "exe:" + os.path.relpath(p, ROOT)).replace("\\", "/")
        for m in re.finditer(r'REXCVAR_DEFINE_\w+\(\s*' + re.escape(cvar) + r'\b', t):
            tail = t[m.end():m.end() + 800]
            lc = re.search(r'lifecycle\(rex::cvar::Lifecycle::(\w+)\)', tail)
            defs.append("%s (%s)" % (rel, lc.group(1) if lc else "default lifecycle"))
        for m in re.finditer(r'(REXCVAR_GET\(\s*' + re.escape(cvar) + r'\s*\)|Query<[^>]+>\("' + re.escape(cvar) +
                             r'"\)|GetFlagByName\("' + re.escape(cvar) + r'"\)|Plugin(?:Bool|Int|String|Double)\("' +
                             re.escape(cvar) + r'")', t):
            fn = enclosing_function(t, m.start())
            kind = "init" if any(w in fn for w in INIT_WORDS) else "use"
            reads.append("%s:%s [%s]" % (rel, fn, kind))
        for m in re.finditer(r'RegisterChangeCallback\("' + re.escape(cvar) + r'"', t):
            reads.append("%s:change callback [use]" % rel)
    return defs, sorted(set(reads))


# ---- 4. evidence from a session log -------------------------------------------------------------------------
pushed = {}
if args.log:
    for line in open(args.log, encoding="utf-8", errors="replace"):
        m = re.search(r"Tuning: ([a-z_0-9]+) = (\S*)", line)
        if m:
            pushed[m.group(1)] = m.group(2)

# ---- 5. report -----------------------------------------------------------------------------------------------
print("F10 SETTINGS CENSUS - %d rows in src/ng2_menu.cpp; SDK source %s%s" % (len(rows), args.sdk,
      ("; startup evidence from " + os.path.basename(args.log)) if args.log else ""))
untraced = 0
findings = []
for r in rows:
    cvars = set()
    for f in r["fields"]:
        cvars |= field_cvars.get(f, set())
    print("\n%s  fields=%s  tag=%s  text claims=%s" % (r["label"], ",".join(r["fields"]) or "-",
                                                   "restart" if r["tag"] else "-", ",".join(r["claims"]) or "-"))
    if not cvars:
        reads = {f: exe_field_reads(f) for f in r["fields"]}
        dead = [f for f, n in reads.items() if n == 0]
        if r["fields"] and not dead:
            print("   -> executable-side: read by the exe outside the menu at %s" %
                  ", ".join("%s x%d" % kv for kv in reads.items()))
        else:
            untraced += 1
            print("   -> NO READ SITE in the executable outside the menu for %s (nothing acts on it, or it is read "
                  "through a path this census cannot see)" % (", ".join(dead) or "(no field)"))
            if r["fields"]:
                findings.append("%s: field(s) %s written by the menu but read nowhere else" % (r["label"], ", ".join(dead)))
        continue
    for cvar, how in sorted(cvars):
        defs, reads = sdk_lookup(cvar)
        init_only = bool(reads) and all("[init]" in x for x in reads)
        live_reads = [x for x in reads if "[use]" in x]
        ev = ("pushed at startup = %s" % pushed[cvar]) if cvar in pushed else ("NOT pushed at startup" if args.log else "")
        print("   %-34s via %-32s %s" % (cvar, how, ev))
        if not defs:
            print("      DEFINED NOWHERE in the SDK source -> the value reaches nothing")
            findings.append("%s: cvar %s has no definition in the SDK" % (r["label"], cvar))
        for d in defs:
            print("      defined  %s" % d)
        for x in reads[:8]:
            print("      read     %s" % x)
        if len(reads) > 8:
            print("      ... %d more read sites" % (len(reads) - 8))
        if defs and not reads:
            print("      NO READ SITE -> defined but never consulted")
            findings.append("%s: cvar %s is defined but never read" % (r["label"], cvar))
        if init_only and "live" in how and not r["tag"]:
            findings.append("%s: %s is read at init only, but the row pushes it live with no restart tag" % (r["label"], cvar))
        if init_only and "immediately" in r["claims"]:
            findings.append("%s: text says 'immediately' but %s is read at init only" % (r["label"], cvar))
print("\n== SUMMARY: %d rows; %d without a GPU cvar (executable-side or untraced); findings: %d" % (len(rows), untraced, len(findings)))
for f in findings:
    print("  - " + f)
