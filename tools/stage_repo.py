"""Assemble exactly what may be published, and prove nothing else got in.

An ALLOWLIST, not a denylist. A denylist ships whatever nobody thought of, and
the things that must not be published here are the ones that look most like
ordinary build output:

  generated/   the translated PowerPC -> C++. This IS the derivative work.
  game/        the disc.
  out/         builds, including a 144 MB executable of translated game code.
  bugreport/evidence, docs/texpack   screenshots and decoded textures.
  tools/upscaler                     a third-party binary under its own licence.

So the rule is that a file is published only if a rule names it, and the sweep
at the end re-reads the staged tree looking for anything that smells like game
data regardless of how it got there.

    python tools/stage_repo.py                 # report
    python tools/stage_repo.py --out <dir>     # write the tree
"""

import argparse
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (path, recursive, extension filter or None)
ALLOW_FILES = [
    "README.md",
    "CHANGELOG.md",
    "CMakeLists.txt",
    "CMakePresets.json",
    "VERSION",
    "ng2_manifest.toml",
    "LICENSE",
]
ALLOW_TREES = [
    ("src", (".cpp", ".h")),
    ("config", (".toml",)),
    ("resources", (".in", ".ico", ".rc")),
    ("tools", (".py", ".json")),
    # .txt as well as .md: VECTOR_COVERAGE.txt was being dropped purely by
    # extension, which is not a decision anyone made. It is the investigation
    # that found the VMX128 v64-v127 defect, so it is one of the more useful
    # things in here. Anything else that lands in docs/ still has to clear the
    # sweep below.
    ("docs", (".md", ".txt")),
]

# Never, whatever a rule above might sweep up.
#
# `texpack` is named here because the docstring above has always claimed it was
# excluded while nothing actually excluded it: its two files happened to be a
# .png (denied by extension) and a .txt (not allowlisted), so the exclusion was
# an accident of file naming rather than a rule. Widening the docs filter to
# .txt exposed that immediately by staging an index of decoded game textures.
# A directory that must not be published has to be denied by name, so that it
# stays denied whatever is put in it.
DENY_NAMES = {"upscaler", "texpack"}
DENY_EXT = {".xex", ".wmv", ".ng2", ".bin", ".tex", ".dat", ".exe", ".dll",
            ".iso", ".png", ".jpg", ".rdc", ".pdb", ".obj", ".zip"}


def collect():
    picked, skipped = [], []
    for rel in ALLOW_FILES:
        p = os.path.join(ROOT, rel)
        if os.path.isfile(p):
            picked.append(rel)
        else:
            skipped.append((rel, "missing"))

    for tree, exts in ALLOW_TREES:
        base = os.path.join(ROOT, tree)
        if not os.path.isdir(base):
            skipped.append((tree, "missing"))
            continue
        for dirpath, dirnames, files in os.walk(base):
            dirnames[:] = [d for d in dirnames if d.lower() not in DENY_NAMES]
            for f in sorted(files):
                ext = os.path.splitext(f)[1].lower()
                if exts and ext not in exts:
                    continue
                if ext in DENY_EXT and ext not in exts:
                    continue
                rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
                picked.append(rel.replace("\\", "/"))
    return sorted(set(picked)), skipped


def personal_paths(paths):
    """Anything that would publish the author's machine along with the code.

    Run by hand once, this found nothing - which is exactly why it belongs in
    the tool. A check that only runs when somebody remembers it is a check that
    passes right up until the release nobody thought to run it for.
    """
    pat = re.compile(r"renoi|C:\\Users|C:/Users|ClaudeCode", re.I)
    bad = []
    for rel in paths:
        # This file carries the pattern as data, so it matches itself. Skipping
        # it by name rather than weakening the pattern: a search that has to
        # avoid describing what it looks for is a worse search.
        if os.path.basename(rel) == os.path.basename(__file__):
            continue
        try:
            text = open(os.path.join(ROOT, rel), encoding="utf-8",
                        errors="replace").read()
        except OSError:
            continue
        n = len(pat.findall(text))
        if n:
            bad.append((rel, "%d absolute/personal path reference(s)" % n))
    return bad


def suspicious(paths):
    """A second opinion, applied to what was actually chosen."""
    bad = []
    for rel in paths:
        low = rel.lower()
        base = os.path.basename(low)
        if "544307d5" in low:
            bad.append((rel, "title id in the path"))
            continue
        if os.path.splitext(base)[1] in DENY_EXT and not low.endswith((".ico", ".rc")):
            bad.append((rel, "extension that carries game data or binaries"))
            continue
        full = os.path.join(ROOT, rel)
        try:
            if os.path.getsize(full) > 4 * 1024 * 1024:
                bad.append((rel, "over 4 MB - source should not be"))
        except OSError:
            pass
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the staged tree here")
    args = ap.parse_args()

    picked, skipped = collect()
    total = 0
    by_top = {}
    for rel in picked:
        try:
            n = os.path.getsize(os.path.join(ROOT, rel))
        except OSError:
            n = 0
        total += n
        top = rel.split("/")[0]
        by_top[top] = by_top.get(top, [0, 0])
        by_top[top][0] += 1
        by_top[top][1] += n

    print("staged for publication: %d files, %.1f KB" % (len(picked), total / 1024.0))
    for top in sorted(by_top):
        n, b = by_top[top]
        print("  %-22s %4d files  %8.1f KB" % (top, n, b / 1024.0))
    if skipped:
        print("\nnot found (check before publishing):")
        for rel, why in skipped:
            print("  %-22s %s" % (rel, why))

    bad = suspicious(picked) + personal_paths(picked)
    print("")
    if bad:
        print("REFUSING: %d staged files look like they should not be published" % len(bad))
        for rel, why in bad:
            print("  %-50s %s" % (rel, why))
        return 1
    print("sweep: no game data, no binaries, no oversized blobs, no personal paths")

    if args.out:
        if os.path.exists(args.out):
            shutil.rmtree(args.out)
        for rel in picked:
            dest = os.path.join(args.out, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(os.path.join(ROOT, rel), dest)
        print("wrote %d files to %s" % (len(picked), args.out))
    else:
        print("(report only - pass --out <dir> to write the tree)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
