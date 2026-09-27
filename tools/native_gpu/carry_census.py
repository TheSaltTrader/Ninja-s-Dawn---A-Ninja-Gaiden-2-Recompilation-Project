#!/usr/bin/env python3
"""Carry-forward census: does a built NG2 folder still carry every feature
the port shipped through v1.0.24?  (docs/native_gpu/CARRY_FORWARD_LEDGER.md)

    python tools/native_gpu/carry_census.py <folder> [--generated <dir>] [--hooks <patches.toml>]
    python tools/native_gpu/carry_census.py --compare <baseline-folder> <candidate-folder>

Presence is judged by CONTENT (a string inside the file the exe loads), never
by where a file came from - v1.0.22 shipped without ultrawide because the
packaging check compared origins.  A DLL whose CONTROL string is absent is
reported as unreadable, not as "every feature missing".  Items that have no
string proof are printed as UNMEASURED and counted, so what this census cannot
see is part of its output.

ngpu_backend.dll (the native backend, 2026-09-26) is OPTIONAL: absent, its
rows read UNMEASURED and the census still passes (the in-exe copy runs);
present, it must carry NG2's plugin-side features, because under offload it
is the only GPU and a build of it from a tree without them loses ultrawide,
the fades and the texture pack exactly as v1.0.22 lost ultrawide.

Exit status: 0 = every measurable item present; 1 = something MISSING;
2 = a required file could not be read.
"""
import argparse
import os
import sys

# ---------------------------------------------------------------------------
# The ledger.  One row per feature: (item, file, needle, note).
# file: 'exe' | 'runtime' | 'plugin' | 'dll' | 'generated' | 'hooks' | None (unmeasured)
# ---------------------------------------------------------------------------
FILES = {
    "exe": "ng2.exe",
    "runtime": "rexruntime.dll",
    "plugin": "rexgpu-xenos.dll",
    "dll": "ngpu_backend.dll",   # optional
}
OPTIONAL = {"dll"}

# A control string every build of that file carries.  Verified against the
# shipped v1.0.24 binaries (and the kit's DLL) on 2026-09-26.
CONTROLS = {
    "exe": b"video_mode_explicit",   # the cvar the app sets by name (v1.0.1)
    "runtime": b"rex_gpu_create",
    "plugin": b"rex_gpu_create",
    "dll": b"NgpuBackendAbiVersion",
}

LEDGER = [
    # 1.1 texture pack + AI upscale
    ("texpack: replacement + index",        "plugin",  b"[texpack]",           "texture_cache.cpp"),
    ("texpack: per-stage warm",             "plugin",  b"warmed stage",        "v1.0.0 warm + bar"),
    ("texpack: mip compute pass",           "plugin",  b"texpack_mip",         "texpack_mip.cs.hlsl"),
    ("texpack: pack path cvar",             "plugin",  b"texture_pack_path",   "cvar the tuning sets"),
    ("upscale: Real-ESRGAN path",           "exe",     b"Real-ESRGAN",         "ng2_textool / menu"),
    ("upscale: only-missing",               "exe",     b"only-missing",        "v1.0.2"),
    # 1.2 ultrawide
    ("ultrawide: projection widen k",       "plugin",  b"ng2_fov_k",           "command_processor.cpp"),
    ("ultrawide: mode cvar (plugin side)",  "plugin",  b"ng2_uw_mode",         "written at swap"),
    ("ultrawide: pause-flag detector",      "plugin",  b"NG2_UW_NOPAUSE",      "v1.0.19"),
    ("ultrawide: mode cvar (presenter)",    "runtime", b"ng2_uw_mode",         "the half v1.0.22 lost"),
    ("ultrawide: video_mode_explicit",      "runtime", b"video_mode_explicit", "v1.0.1"),
    ("ultrawide: app ApplyFov",             "exe",     b"ng2_fov_k",           "ng2_app.h"),
    # 1.3 fades
    ("fades: solid-2d gate",                "plugin",  b"solid2d",             "v1.0.20"),
    # 1.4 / 1.5 SDK fixes
    ("sdk: stuck-wait watchdog",            "runtime", b"watchdog",            "xboxkrnl_threading.cpp"),
    ("sdk: ring re-init fix (both pointers)", "plugin", b"pointers reset",     "InitializeRingBuffer; the v1.0.0 attract-freeze fix"),
    ("sdk: ring dump on bad packet",        "plugin",  b"RINGDUMP",            "command_processor.cpp"),
    ("sdk: clear_memory_page_state path",   "plugin",  b"clear_memory_page_state", "NG2 is the only title on it"),
    ("sdk: upload copy pool present",       "plugin",  b"shared_memory_upload_threads", "the POOL, not the fix"),
    ("sdk: upload copy pool RACE FIX",      None,      None,                   "no string exists; MEASURED behaviourally 2026-09-26: 9 legs of 150-220 s on the fork pair with clear_memory_page_state on, 0 freezes (the unfixed pool froze 2/2 at ~13 swaps)"),
    ("sdk: db16cyc -> rex_spin_yield",      "generated", b"rex_spin_yield",    "inlined; only the generated source shows it"),
    # 2 guest hooks (exe strings) - RenderSize names live only in the TOML
    ("hook: ng2PatchChapter12",             "exe",     b"ng2PatchChapter12",   "0x82834C78"),
    ("hook: ng2PatchSkipVideos",            "exe",     b"ng2PatchSkipVideos",  "0x8380EB9C"),
    ("hook: ng2AudioBarrierFix",            "exe",     b"ng2AudioBarrierFix",  "0x8374F164"),
    ("hook: ng2ChapterAwardFix",            "exe",     b"ng2ChapterAwardFix",  "0x8242D7B4"),
    ("hook: ng2PatchRenderSize1",           "hooks",   b"ng2PatchRenderSize1", "TOML only"),
    ("hook: ng2PatchRenderSize2",           "hooks",   b"ng2PatchRenderSize2", "TOML only"),
    # 1.4 60 fps
    ("60fps: guest fps at 60, correct speed", None,    None,                   "behavioural; MEASURED 2026-09-26: title 60.0 in every leg, four-leg pair identical to the plugin-alone baseline; Chapter 1 gameplay pair in the handover"),
    ("60fps: no rate above 60 offered",     None,      None,                   "lodestone census (green on the alt branch 2026-09-26, fps clamped, vsync forced) + README; not a binary string"),
    # 6 the native backend (the transplant): the exe's side, and the DLL when present
    ("native: exe binds the RexNgpu ABI",   "exe",     b"RexNgpuSetDrawCallback", "ng2_ngpu_bridge.cpp"),
    ("native: exe presenter half",          "exe",     b"[ngpu-window]",       "ng2_ngpu_window.cpp"),
    ("native dll: ultrawide widen k",       "dll",     b"ng2_fov_k",           "the DLL's IssueSwap / UpdateBindings"),
    ("native dll: ultrawide mode",          "dll",     b"ng2_uw_mode",         "written at its swap"),
    ("native dll: fades solid-2d gate",     "dll",     b"solid2d",             "v1.0.20 in the DLL"),
    ("native dll: texpack replacement",     "dll",     b"[texpack]",           "texture_cache in the DLL"),
    ("native dll: texpack mip pass",        "dll",     b"texpack_mip",         "in the DLL"),
    ("native dll: settings across the boundary", "dll", b"NgpuBackendSetSetting", "ng2_fov_k in, ng2_uw_mode out"),
]

# Every ledger string, for --compare (a candidate that lost any of these is
# reported even if the item above is not the one that names it).
COMPARE_STRINGS = sorted({row[2] for row in LEDGER if row[2]})


def read(path):
    with open(path, "rb") as f:
        return f.read()


def census(folder, generated=None, hooks=None):
    blobs = {}
    unreadable = []
    absent_optional = []
    for key, name in FILES.items():
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            if key in OPTIONAL:
                absent_optional.append(name)
            else:
                unreadable.append("%s: missing from %s" % (name, folder))
            continue
        blob = read(path)
        if CONTROLS[key] not in blob:
            unreadable.append("%s: control string %r absent - cannot report on its features"
                              % (name, CONTROLS[key].decode()))
            continue
        blobs[key] = blob
    if generated and os.path.isdir(generated):
        hits = 0
        for root, _, files in os.walk(generated):
            for fn in files:
                if fn.endswith((".cpp", ".h")):
                    if b"rex_spin_yield" in read(os.path.join(root, fn)):
                        hits += 1
        blobs["generated"] = b"rex_spin_yield" if hits else b""
        blobs["generated_hits"] = hits
    if hooks and os.path.isfile(hooks):
        blobs["hooks"] = read(hooks)

    present, missing, unmeasured = [], [], []
    for item, where, needle, note in LEDGER:
        if where is None:
            unmeasured.append((item, note))
        elif where not in blobs:
            what = FILES.get(where, where)
            unmeasured.append((item, "%s not readable here%s" % (what, " (optional, absent)" if what in absent_optional else "")))
        elif needle in blobs[where]:
            present.append((item, note))
        else:
            missing.append((item, "%s lacks %r (%s)" % (FILES.get(where, where), needle.decode(), note)))

    print("carry census: %s" % folder)
    for key in ("exe", "runtime", "plugin", "dll"):
        if key in blobs:
            print("  read %-16s %10d bytes" % (FILES[key], len(blobs[key])))
    for name in absent_optional:
        print("  absent %-14s (optional - the in-exe copy of the backend runs)" % name)
    if "generated_hits" in blobs:
        print("  generated: %d file(s) carry rex_spin_yield" % blobs["generated_hits"])
    for line in unreadable:
        print("  UNREADABLE  %s" % line)
    for item, note in present:
        print("  PRESENT     %-40s %s" % (item, note))
    for item, why in missing:
        print("  MISSING     %-40s %s" % (item, why))
    for item, why in unmeasured:
        print("  UNMEASURED  %-40s %s" % (item, why))
    print("  %d present, %d missing, %d unmeasured, %d unreadable file(s) of %d ledger rows"
          % (len(present), len(missing), len(unmeasured), len(unreadable), len(LEDGER)))
    if unreadable:
        return 2
    return 1 if missing else 0


def compare(baseline, candidate):
    rc = 0
    for key, name in FILES.items():
        a = os.path.join(baseline, name)
        b = os.path.join(candidate, name)
        if not (os.path.isfile(a) and os.path.isfile(b)):
            print("  %-16s cannot compare (missing on one side)%s" % (name, " - optional" if key in OPTIONAL else ""))
            if key not in OPTIONAL:
                rc = 2
            continue
        ba, bb = read(a), read(b)
        lost = [s.decode() for s in COMPARE_STRINGS if s in ba and s not in bb]
        gained = [s.decode() for s in COMPARE_STRINGS if s not in ba and s in bb]
        print("  %-16s baseline %10d B  candidate %10d B  lost %d  gained %d"
              % (name, len(ba), len(bb), len(lost), len(gained)))
        for s in lost:
            print("      LOST   %s" % s)
        for s in gained:
            print("      gained %s" % s)
        if lost:
            rc = max(rc, 1)
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?", help="folder holding ng2.exe, rexruntime.dll, rexgpu-xenos.dll (and ngpu_backend.dll)")
    ap.add_argument("--generated", help="the generated/ source dir (proves rex_spin_yield)")
    ap.add_argument("--hooks", help="config/hooks/patches.toml (proves the TOML-only hook names)")
    ap.add_argument("--compare", nargs=2, metavar=("BASELINE", "CANDIDATE"),
                    help="list every ledger string the candidate folder lost against the baseline")
    args = ap.parse_args()
    if args.compare:
        print("carry compare: %s -> %s" % tuple(args.compare))
        sys.exit(compare(*args.compare))
    if not args.folder:
        ap.error("a folder is required (or --compare)")
    sys.exit(census(args.folder, args.generated, args.hooks))


if __name__ == "__main__":
    main()
