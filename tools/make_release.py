"""Package a versioned, runnable release of ng2recomp into ..\\Releases.

A release is a folder anyone can unzip and run *provided they own the game*:
the recompiled executable, the SDK runtime and GPU plugin beside it, the
controller database, default settings, and instructions to drop their own disc
rip into game\\.

    python tools/make_release.py                 # cut the version in VERSION
    python tools/make_release.py --build         # rebuild first, then cut it
    python tools/make_release.py --version 0.2.0 --force

What it refuses to do, because each of these has produced a bad release before:

  * cut a version with no CHANGELOG entry - the notes are the release,
  * update a folder that is not already an install (--update),
  * cut a release missing a REQUIRED tool - v0.3.4 shipped without ffmpeg and
    every install it produced had broken videos,
  * cut a build older than the sources it claims to be built from,
  * overwrite an existing release folder without --force,
  * stage anything that looks like game data (see FORBIDDEN_SUFFIXES).

Nothing here is redistributable: ng2.exe contains the game's own code,
translated. The zip is for keeping versions straight on this machine and for
handing to someone who already owns Ninja Gaiden II - not for publishing.
"""

import argparse
import datetime
import glob
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import textwrap
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK_DIR = os.path.abspath(os.path.join(ROOT, "..", "RexBlue", "win-amd64"))
DEFAULT_RELEASES = os.path.abspath(os.path.join(ROOT, "..", "Releases"))
BUILD_DIR = os.path.join(ROOT, "out", "build", "win-amd64-Release")

# Copied verbatim from the build directory. Everything the executable needs at
# runtime other than the game itself.
PAYLOAD = [
    "ng2.exe",
    "rexruntime.dll",
    "rexgpu-xenos.dll",
    "gamecontrollerdb.txt",
]

# The Visual C++ runtime the executable and both SDK DLLs import. Windows does
# not ship it, so without these a machine that never installed the
# redistributable fails at start-up with a "DLL not found" box and nothing in
# the log. Microsoft permits shipping these files beside an application
# (app-local deployment). They are the only thing the game loads that is
# neither Windows itself nor this folder - checked by listing the modules of
# the running process (v1.0.6).
VCRT_DLLS = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
             "msvcp140_atomic_wait.dll"]
VCRT_GLOB = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                         "Microsoft Visual Studio", "2022", "*", "VC", "Redist", "MSVC",
                         "*", "x64", "Microsoft.VC143.CRT")

# Sources whose mtime must predate the executable, or the build is stale.
SOURCE_GLOBS = ["src", "config", "ng2_manifest.toml", "CMakeLists.txt"]

# Copied into the release's tools/. (name, required).
#
# These three ARE required, and the reason is worth stating: the app finds them
# by looking for tools/<name> beside the executable, and when one is missing the
# button that drives it fails with "missing from this install". Shipping none of
# them - which an earlier revision of this list did - leaves every texture
# control in the menu dead in a way that only shows up after install.
#
# The video encoder that used to be here is gone for good. Re-encoding existed
# only to work around the guest's video decoder producing garbage, and that was
# fixed at the source: the recompiler was localising VMX128 registers v64-v127
# into zero-initialised locals, so the codec's inverse-transform kernels read
# their vperm control vectors as zero. Seven functions in the whole title did
# that and all seven were the video codec. With that fixed, decoded frames match
# the source at r = 1.000, so a release ships no ffmpeg and no conversion pass.
TOOLS = [
    ("upscale_textures.py", True),   # dump -> pack, and the plain upscaler
    ("ai_upscale.py", True),         # imported by the above for --ai
    ("get_upscaler.py", True),       # downloads Real-ESRGAN on request
]

# What an install must already contain before --update will write into it. Any
# one of these is enough: a folder with a game rip, or with our executable in
# it, is an install. A folder with neither is somebody else's.
INSTALL_MARKERS = ["ng2.exe", "game", "ng2_settings.cfg"]

# Never touched by --update. Everything here is the player's, not ours: the disc
# rip, the DLC, the profile and saves, and the settings. "video" was on this
# list until the guest decoder was fixed and the folder stopped being written.
UPDATE_KEEPS = ["game", "dlc", "user", "logs", "cache",
                "ng2_settings.cfg", "ng2.toml"]


def update_install(folder, version):
    """Replace our files in an existing install, keeping the player's."""
    folder = os.path.abspath(folder)
    if not os.path.isdir(folder):
        die("no such folder: %s" % folder)
    if not any(os.path.exists(os.path.join(folder, m)) for m in INSTALL_MARKERS):
        die("%s does not look like an ng2 install (no %s).\n"
            "       Refusing to write an executable into it."
            % (folder, ", ".join(INSTALL_MARKERS)))

    print("=== Updating %s to v%s ===" % (folder, version))
    print("    keeping: %s" % ", ".join(UPDATE_KEEPS))
    for name in PAYLOAD:
        source = os.path.join(BUILD_DIR, name)
        if not os.path.isfile(source):
            die("%s is missing from the build - build first" % name)
        shutil.copy2(source, os.path.join(folder, name))
        print("  %-24s %s" % (name, human(os.path.getsize(source))))

    os.makedirs(os.path.join(folder, "tools"), exist_ok=True)
    for tool, required in TOOLS:
        source = find_tool(tool)
        if source is None:
            if required:
                die("%s is required and was not found in %s"
                    % (tool, " or ".join(TOOL_DIRS)))
            continue
        shutil.copy2(source, os.path.join(folder, "tools", tool))
        print("  tools/%-18s %s" % (tool, human(os.path.getsize(source))))

    with open(os.path.join(folder, "VERSION.txt"), "w") as f:
        f.write("ng2recomp v%s\n" % version)
    print("\n=== %s is now v%s ===" % (folder, version))
    print("Game data, DLC, saves and settings untouched.")


# Where a tool may live. The source tree first, then the build directory.
TOOL_DIRS = [os.path.join(ROOT, "tools"), os.path.join(BUILD_DIR, "tools")]


def sdk_dll_origin(name):
    """"stock" or "source-built", for the two SDK DLLs. Empty for anything else.

    The payload is copied VERBATIM from the build directory, and building ng2
    re-copies the stock DLLs over whatever was deployed there. So which SDK a
    release contains depends on what happened to be sitting in that folder when
    it was cut - which is how v0.5.3 shipped stock DLLs and a texture pack that
    could not possibly work. Saying it out loud costs one hash.
    """
    if name not in ("rexruntime.dll", "rexgpu-xenos.dll"):
        return ""
    stock = os.path.join(SDK_DIR, "bin", name)
    built = os.path.join(BUILD_DIR, name)
    if not os.path.isfile(stock):
        return "unknown (no stock copy to compare)"
    def digest(path):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    return "stock" if digest(stock) == digest(built) else "source-built"


def check_sdk_pair(dest):
    """Refuse to ship one stock DLL beside one source-built one.

    That combination does not fail loudly: the game exits during startup with no
    error and an empty log. It is the single worst way to get this wrong, and
    the only way to get it wrong by accident.
    """
    origins = {n: sdk_dll_origin(n) for n in ("rexruntime.dll", "rexgpu-xenos.dll")}
    kinds = set(origins.values())
    if len(kinds) > 1:
        die("SDK DLLs are mismatched (%s). A source-built plugin against a stock "
            "runtime makes the game exit at startup with no error. Deploy both "
            "from the same build before cutting."
            % ", ".join("%s=%s" % kv for kv in sorted(origins.items())))
    print("  SDK pair: both %s" % kinds.pop())


def find_tool(name):
    for directory in TOOL_DIRS:
        candidate = os.path.join(directory, name)
        if os.path.isfile(candidate):
            return candidate
    return None


# A release must never contain game data. Checked over the staged tree rather
# than trusted: assets/, game/ and DLC/ are gitignored, but a stray copy in the
# build directory would sail straight into the zip.
FORBIDDEN_SUFFIXES = (".xex", ".ng2", ".wmv", ".xma", ".bin", ".iso", ".dat")
FORBIDDEN_EXCEPTIONS = ("gamecontrollerdb.txt",)

SETTINGS_TEMPLATE = """\
# ng2 recompilation settings.
#
# You should not need this file: the setup screen on first run writes it, and
# F10 in game edits it. It is here so the settings are documented and can be
# edited by hand. The game rewrites it whenever you press Play or Save, so a
# hand edit survives only until then.
#
# Every NG2_* environment variable overrides the matching line for one run.

# Window size. The guest is told the display is this size, so it is the real
# render resolution, not an upscale. 640x480 to 7680x4320. Restart to apply.
window_width=1280
window_height=720
fullscreen=0

# Guest refresh rate, 30-144. The game paces its logic off this, so it is a
# game-speed setting, not just a cap; vsync must be on for it to pace properly.
fps=60
vsync=1

# True internal supersampling, 1-3. The guest's own framebuffer is rendered at
# this multiple and filtered back down. 2 quadruples the pixels the GPU draws
# and the emulated EDRAM has to hold. Restart to apply.
resolution_scale=1

# The Xenia community render-size patch. The title renders internally at
# 1120x584 and scales up; 1 raises that to 1280x720. Restart to apply.
internal_720p=0

# Forced anisotropic filtering. -1 leaves the game's own samplers alone,
# 0-4 force 1x/2x/4x/8x/16x. Restart to apply.
anisotropic=-1

# How the rendered image is resampled to the window. This build implements
# bilinear and nothing else; the two sharpness values below are kept only so
# the file round-trips, and are not sent to a build that lacks their settings.
present_effect=bilinear
cas_sharpness=0
fsr_sharpness=0.2

# Dither the output, and keep the game's aspect ratio instead of stretching.
present_dither=0
letterbox=1

# Where the game is. Empty means game\\ beside this file, dlc\\ for DLC.
game_path=
dlc_path=

# Set once you have pressed Play, so the setup screen stops interrupting.
# Hold Shift while launching to get it back.
configured=0
"""

README_TITLE = "ng2recomp v{version} - Ninja Gaiden II, statically recompiled for PC"

README_TEMPLATE = """\
{title}
{underline}

This is not an emulator. The game's PowerPC code was translated to C++ and
compiled into a native x86-64 executable; the ReXGlue SDK {sdk} supplies the
Xbox 360 kernel, filesystem, audio and GPU emulation around it.

REQUIRES YOUR OWN COPY OF THE GAME. No game code or data is included here.


Already have an older version installed?
----------------------------------------

Do NOT unzip this beside it and start again. A new release ships an empty
game\\ folder, so the only folder with your disc rip in it is the one you
installed first - and that is the one you will keep running, still on the old
executable, wondering why fixes have not arrived. (That is not hypothetical:
it produced three separate bug reports in one afternoon.)

Copy these over your existing install instead, keeping everything else:

    ng2.exe
    rexruntime.dll
    rexgpu-xenos.dll
    gamecontrollerdb.txt
    tools\\
Your game\\, dlc\\, user\\ and ng2_settings.cfg are untouched by that,
and the game will be on this version next time you start it.


Setting it up
-------------

1. Run ng2.exe. The first run opens a setup screen.

2. Point it at your game. Either:

     * "Choose folder..." - a folder that already holds your extracted disc,
       so that default.xex sits in it; or
     * "Choose disc image..." - your own .iso, which it extracts for you.
       About 6.7 GB and under a minute. The runtime mounts a folder rather
       than an image, which is why the copy is needed.

   It reads the title out of the disc, so it will tell you if you have
   pointed it at the wrong game.

3. Optional: point it at your DLC folder. The STFS packages are the
   extensionless files under  <content>\\544307D5\\00000002\\ - Fiend
   Challenges, Shadow Walker, Biometal Hayabusa and Mission Mode. They are
   installed on every launch; doing it twice is harmless.

4. Press Play.

The game/ and dlc/ folders beside ng2.exe are the defaults, so putting your
files there works without choosing anything.

The folder should end up looking like:

    ng2.exe
    rexruntime.dll
    rexgpu-xenos.dll
    gamecontrollerdb.txt
    ng2_settings.cfg
    game\\default.xex, game\\*.ng2, ...
    dlc\\<packages>            (optional)


Videos
------

They just play. Nothing to convert, nothing to install, no ffmpeg.

Earlier builds needed all of that. Every pre-rendered video came out
garbled, so the port re-encoded all 27 of them up front and drew its own
copy over the top. That was a workaround, and it could never fix the clips
inside the Techniques menu, which are a small inset in a scrolling page
rather than a full-screen video.

The cause was in the recompiler, not the game: VMX128 registers v64-v127
were being turned into zero-initialised local variables, so the video
codec's inverse-transform kernels read their permute control vectors as
zero and scrambled every block. Seven functions in the entire title did
that, and all seven were the video decoder - which is exactly why videos
were the only thing that looked wrong.

With that fixed, the game's own decoder is correct: decoded frames match
the source video at r = 1.000, with none of the clipped pixels the
corruption used to produce. The replacement path has been removed rather
than left switched off, so the only video setting left is "Skip intro
videos", which is a preference rather than a workaround.


Settings
--------

  F10   the settings menu, over the running game
  F4    every runtime setting, unfiltered
  F3    frame timing overlay
  `     console

The setup screen from step 1 comes back whenever you hold Shift while
launching. Settings that can change while the game runs do so immediately;
settings the window or the guest video mode were built from are shown greyed,
marked (restart), rather than accepted and quietly ignored.

What is there: window size and fullscreen, frame rate 30-144, V-Sync,
internal supersampling 1-3x, the 1280x720 internal render patch, anisotropic
filtering override, output filter, letterbox and dither.

Everything is saved in ng2_settings.cfg beside the executable, which is
documented and can be edited by hand. Any setting can also be overridden for a
single run with an environment variable - NG2_WIDTH, NG2_HEIGHT,
NG2_FULLSCREEN, NG2_FPS, NG2_SCALE, NG2_GAME, NG2_DLC.

Controllers work through SDL and gamecontrollerdb.txt (2,285 mappings) must
stay beside the executable. For keyboard control add  --mnk_mode=true  to the
command line.


Known issues
------------

{known_issues}


Provenance
----------

Built {built} on {host}.
See SHA256SUMS for file hashes and provenance.txt for exactly what went in.

ng2.exe contains Ninja Gaiden II's own code in translated form and no game data;
it is published on the project's GitHub releases page and does nothing without
your own copy of the game.
"""


def read_version(explicit):
    if explicit:
        version = explicit.strip().lstrip("v")
    else:
        path = os.path.join(ROOT, "VERSION")
        if not os.path.exists(path):
            die("no VERSION file and no --version given")
        version = open(path).read().strip().lstrip("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        die("version %r is not MAJOR.MINOR.PATCH" % version)
    return version


def changelog_section(version):
    """The body of the '## vX.Y.Z' section, and the one-line summary under it.

    A release with no notes is a release nobody can tell apart from the one
    before it, so a missing or empty section is fatal rather than a warning.
    """
    path = os.path.join(ROOT, "CHANGELOG.md")
    if not os.path.exists(path):
        die("CHANGELOG.md is missing - write the notes before cutting a release")
    text = open(path, encoding="utf-8").read()
    heading = re.compile(r"^## v%s\b.*$" % re.escape(version), re.M)
    m = heading.search(text)
    if not m:
        die("CHANGELOG.md has no '## v%s' section - write the notes first"
            % version)
    rest = text[m.end():]
    nxt = re.search(r"^## v", rest, re.M)
    body = (rest[:nxt.start()] if nxt else rest).strip()
    if not body:
        die("CHANGELOG.md section for v%s is empty" % version)
    return body


def iter_sources(paths):
    """Every source file under `paths`, as ROOT-relative posix paths."""
    for rel in paths:
        target = os.path.join(ROOT, rel)
        if os.path.isfile(target):
            yield rel.replace("\\", "/")
        for dirpath, dirnames, filenames in os.walk(target):
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            for name in filenames:
                full = os.path.join(dirpath, name)
                yield os.path.relpath(full, ROOT).replace("\\", "/")


def newest_source(paths):
    """(mtime, path) of the most recently modified source file."""
    newest = (0.0, None)
    for rel in paths:
        target = os.path.join(ROOT, rel)
        if os.path.isfile(target):
            newest = max(newest, (os.path.getmtime(target), target))
        for dirpath, dirnames, filenames in os.walk(target):
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            for name in filenames:
                full = os.path.join(dirpath, name)
                newest = max(newest, (os.path.getmtime(full), full))
    return newest


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0


def die(msg):
    print("ERROR: %s" % msg, file=sys.stderr)
    sys.exit(1)


def run_build():
    print("=== Building (tools/build.cmd Release) ===")
    rc = subprocess.call([os.path.join(ROOT, "tools", "build.cmd"), "Release"],
                         cwd=ROOT)
    if rc != 0:
        die("build failed (rc=%d) - not cutting a release from a broken build"
            % rc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None,
                    help="override the version in VERSION")
    ap.add_argument("--update", metavar="FOLDER", default=None,
                    help="apply this build over an existing install, keeping "
                         "its game/, dlc/, user/ and settings")
    ap.add_argument("--releases-dir", default=DEFAULT_RELEASES)
    ap.add_argument("--build", action="store_true",
                    help="run tools/build.cmd Release first")
    ap.add_argument("--force", action="store_true",
                    help="replace an existing release folder of this version")
    ap.add_argument("--no-zip", action="store_true")
    ap.add_argument("--allow-stale", action="store_true",
                    help="package a build older than the sources anyway")
    args = ap.parse_args()

    version = read_version(args.version)

    # --update is not a release: it writes this build over an install that is
    # already there. No changelog check, no version folder, no zip - the point
    # is to get the current executable in front of the person playing, without
    # asking them to reinstall 7 GB of disc rip to get it.
    if args.update:
        if args.build:
            run_build()
        update_install(args.update, version)
        return

    # Everything that can fail on human input fails here, before any work.
    notes = changelog_section(version)
    dest = os.path.join(args.releases_dir, "v" + version)
    if os.path.exists(dest) and not args.force:
        die("%s already exists - bump the version, or pass --force" % dest)

    if args.build:
        run_build()

    missing = [f for f in PAYLOAD if not os.path.exists(os.path.join(BUILD_DIR, f))]
    if missing:
        die("build output missing %s in %s - run tools/build.cmd Release"
            % (", ".join(missing), BUILD_DIR))

    exe = os.path.join(BUILD_DIR, "ng2.exe")
    src_mtime, src_path = newest_source(SOURCE_GLOBS)
    if src_mtime > os.path.getmtime(exe):
        msg = ("ng2.exe is older than %s - rebuild, or pass --allow-stale"
               % os.path.relpath(src_path, ROOT))
        if not args.allow_stale:
            die(msg)
        print("WARNING: %s" % msg)

    print("=== Staging v%s -> %s ===" % (version, dest))
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)

    for name in PAYLOAD:
        shutil.copy2(os.path.join(BUILD_DIR, name), os.path.join(dest, name))
        note = sdk_dll_origin(name)
        print("  %-24s %s%s" % (name, human(os.path.getsize(os.path.join(dest, name))),
                                ("   [%s]" % note) if note else ""))
    check_sdk_pair(dest)

    # The VC runtime, from the compiler's own redistributable set.
    vcrt_dirs = sorted(glob.glob(VCRT_GLOB))
    if not vcrt_dirs:
        die("Visual C++ redistributable folder not found under %s" % VCRT_GLOB)
    vcrt_dir = vcrt_dirs[-1]
    for name in VCRT_DLLS:
        source = os.path.join(vcrt_dir, name)
        if not os.path.isfile(source):
            die("%s is missing from %s" % (name, vcrt_dir))
        shutil.copy2(source, os.path.join(dest, name))
        print("  %-24s %s   [VC runtime]" % (name, human(os.path.getsize(source))))

    os.makedirs(os.path.join(dest, "tools"), exist_ok=True)
    for tool, required in TOOLS:
        source = find_tool(tool)
        if source is None:
            if required:
                die("%s is required in a release and was not found in %s.\n"
                    "       Without it the texture controls in the settings "
                    "menu fail with \"missing from this install\"."
                    % (tool, " or ".join(TOOL_DIRS)))
            print("  tools/%-18s (not found, skipped)" % tool)
            continue
        shutil.copy2(source, os.path.join(dest, "tools", tool))
        print("  tools/%-18s %s"
              % (tool, human(os.path.getsize(os.path.join(dest, "tools", tool)))))

    with open(os.path.join(dest, "ng2_settings.cfg"), "w", newline="\r\n") as f:
        f.write(SETTINGS_TEMPLATE)

    # An empty folder does not survive a zip, so give each one a note that
    # explains what belongs in it.
    for folder, text in (
        ("game", "Put your extracted Ninja Gaiden II disc here, so that\r\n"
                 "default.xex sits in this folder.\r\n"),
        ("dlc", "Optional. Put your STFS DLC packages here (the extensionless\r\n"
                "files from <content>\\544307D5\\00000002\\). They install on\r\n"
                "first run.\r\n"),
    ):
        os.makedirs(os.path.join(dest, folder), exist_ok=True)
        with open(os.path.join(dest, folder, "PUT_FILES_HERE.txt"), "w",
                  newline="\r\n") as f:
            f.write(text)

    # Wrapped to the width of the rest of the file. The entries are written as
    # single long strings, and pasting them in unwrapped shipped a README with
    # 300-character lines in the one section people actually read.
    if KNOWN_ISSUES:
        known = "\r\n\r\n".join(
            textwrap.fill(issue, width=76, initial_indent="  * ",
                          subsequent_indent="    ")
            for issue in KNOWN_ISSUES)
    else:
        known = "  none"
    title = README_TITLE.format(version=version)
    with open(os.path.join(dest, "README.txt"), "w", newline="\r\n") as f:
        f.write(README_TEMPLATE.format(
            title=title,
            underline="=" * len(title),
            sdk=sdk_version(),
            known_issues=known,
            built=datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
            host=platform.node(),
        ))

    with open(os.path.join(dest, "RELEASE_NOTES.md"), "w", encoding="utf-8",
              newline="\r\n") as f:
        f.write("# ng2recomp v%s\n\n%s\n" % (version, notes))

    write_provenance(os.path.join(dest, "provenance.txt"), version)

    # No game data, ever.
    for dirpath, _, filenames in os.walk(dest):
        for name in filenames:
            if name in FORBIDDEN_EXCEPTIONS:
                continue
            if name.lower().endswith(FORBIDDEN_SUFFIXES):
                die("staged %s looks like game data - refusing to package it"
                    % os.path.join(dirpath, name))

    staged = write_sums(dest)
    total = sum(os.path.getsize(p) for p in staged)
    print("  %d files, %s" % (len(staged), human(total)))

    if not args.no_zip:
        zip_path = os.path.join(args.releases_dir,
                                "ng2recomp-v%s-win-amd64.zip" % version)
        make_zip(dest, zip_path, version)

    print("\n=== v%s is in %s ===" % (version, dest))
    print("Attach the zip to the GitHub release; it holds translated game code and no game data.")
    return 0


def sdk_version():
    try:
        out = subprocess.check_output(
            [os.path.join(SDK_DIR, "bin", "rexglue.exe"), "--version"],
            text=True, stderr=subprocess.STDOUT, timeout=30)
        return out.strip().splitlines()[0]
    except Exception:
        return "unknown"


def write_provenance(path, version):
    """Exactly what went into this build, so a release can be reproduced."""
    lines = [
        "ng2recomp v%s" % version,
        "built      %s" % datetime.datetime.now().isoformat(timespec="seconds"),
        "host       %s (%s)" % (platform.node(), platform.platform()),
        "SDK        ReXGlue %s at %s" % (sdk_version(), SDK_DIR),
        "build dir  %s" % BUILD_DIR,
        "",
        "Inputs (sha256):",
    ]
    # Enumerated from SOURCE_GLOBS, never hand-listed. A typed list silently
    # stops covering files that are added later, and this file is the record
    # of what actually went into the build - the one place where "close
    # enough" is worthless.
    for rel in sorted(iter_sources(SOURCE_GLOBS)):
        lines.append("  %s  %s" % (sha256(os.path.join(ROOT, rel)), rel))
    with open(path, "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


def write_sums(dest):
    """SHA256SUMS over every staged file, in the format sha256sum -c expects."""
    staged = []
    for dirpath, dirnames, filenames in os.walk(dest):
        dirnames.sort()
        for name in sorted(filenames):
            if name == "SHA256SUMS":
                continue
            staged.append(os.path.join(dirpath, name))
    with open(os.path.join(dest, "SHA256SUMS"), "w", newline="\n") as f:
        for full in staged:
            rel = os.path.relpath(full, dest).replace("\\", "/")
            f.write("%s  %s\n" % (sha256(full), rel))
    staged.append(os.path.join(dest, "SHA256SUMS"))
    return staged


def make_zip(dest, zip_path, version):
    """Zip the staged folder, then verify the archive rather than assume it."""
    print("=== Zipping -> %s ===" % os.path.basename(zip_path))
    top = "ng2recomp-v%s" % version
    written = 0
    tmp = zip_path + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for dirpath, dirnames, filenames in os.walk(dest):
            dirnames.sort()
            for name in sorted(filenames):
                full = os.path.join(dirpath, name)
                arc = os.path.join(top, os.path.relpath(full, dest))
                z.write(full, arc)
                written += 1
    with zipfile.ZipFile(tmp) as z:
        bad = z.testzip()
        if bad:
            die("zip verification failed on %s" % bad)
        if len(z.namelist()) != written:
            die("zip holds %d entries, staged %d" % (len(z.namelist()), written))
    if os.path.exists(zip_path):
        os.remove(zip_path)
    os.replace(tmp, zip_path)
    digest = sha256(zip_path)
    with open(zip_path + ".sha256", "w", newline="\n") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(zip_path)))
    print("  %d entries, %s, verified" % (written, human(os.path.getsize(zip_path))))


# Shown in the release README. Kept next to the packaging rather than in the
# changelog because it describes the build being handed over, not the changes.
KNOWN_ISSUES = [
    "The transition from Chapter 12 to Chapter 13 does not progress: the game"
    " shows its end-of-chapter screen and never loads the next chapter. It is"
    " waiting on an internal flag its own code never clears, and issues no"
    " further disc reads. The same fault is Xenia's long-standing"
    " kernel-save-file-errors, open since 2018. Chapters 1-12 are unaffected.",
    "Chapter 12 runs at around 28 fps. It is playable, and the crash the"
    " community patch exists for is guarded against, but the frame rate is not"
    " fixed.",
    "The AI texture option needs Python and a 43 MB Real-ESRGAN download that"
    " this release deliberately does not bundle. Without them the plain"
    " upscaler still works and the buttons say what is missing.",
    "Chapter-loading videos cannot be skipped. The game treats a failed open"
    " of one as a bad disc and stops, so \"Skip intro videos\" deliberately"
    " leaves them alone.",
]


if __name__ == "__main__":
    sys.exit(main())
