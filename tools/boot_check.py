"""Boot the game N times and report, per run, how far it actually got.

Bring-up questions ("does it reach the menu?") were being answered from single
runs, and this title does not behave the same way twice: the GPU command
processor sometimes fails mid-intro and the picture freezes while the guest
carries on, so one good run proves nothing. This runs the same build several
times and prints a table.

What each column means:

  opens     highest [diag] open # reached. 34 = the three intro videos have
            been opened; 38 = the menu content archives (cmn/char/rtm) have,
            which is the front end loading.
  ring      "PRIMARY RINGBUFFER: Failed to execute packet" count. Non-zero
            means the command stream was misparsed; rendering usually stops
            there even though the guest keeps running.
  badreg    CommandProcessor::WriteRegister out-of-bounds warnings.
  fps       first / last fps sample. A last sample far above 60 means vsync
            stopped pacing, i.e. the GPU stopped consuming frames.
  verdict   MENU if the menu archives opened and the ring buffer survived.

    python tools/boot_check.py --runs 3 --seconds 45
"""

import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "out", "build", "win-amd64-Release")
EXE = os.path.join(BUILD, "ng2.exe")

OPEN_RE = re.compile(r"\[diag\] open #(\d+): (.+)")
FPS_RE = re.compile(r"\[diag\] ([\d.]+) fps")
RING_RE = re.compile(r"PRIMARY RINGBUFFER: Failed")
BADREG_RE = re.compile(r"WriteRegister index out of bounds")
# The four archives the front end loads once the intro is done with.
MENU_FILES = ("cmn.ng2", "char.ng2", "other_rtm.ng2", "rtm.ng2")


def analyse(log_path):
    opens, fps, files = 0, [], set()
    ring = badreg = 0
    try:
        text = open(log_path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    for line in text.splitlines():
        m = OPEN_RE.search(line)
        if m:
            opens = max(opens, int(m.group(1)))
            files.add(os.path.basename(m.group(2).strip()))
        m = FPS_RE.search(line)
        if m:
            fps.append(float(m.group(1)))
        if RING_RE.search(line):
            ring += 1
        if BADREG_RE.search(line):
            badreg += 1
    return {
        "opens": opens,
        "files": files,
        "fps": fps,
        "ring": ring,
        "badreg": badreg,
        "menu": all(f in files for f in MENU_FILES),
        "lines": len(text.splitlines()),
    }


def one_run(index, seconds, extra, outdir):
    log = os.path.join(outdir, "boot_%02d.log" % index)
    if os.path.exists(log):
        os.remove(log)
    cmd = [EXE, "--game_data_root", os.path.join(ROOT, "game"),
           "--log_file", log, "--log_level", "info"] + extra
    # Run from the build directory: the runtime resolves gamecontrollerdb.txt
    # against the working directory, so launching from elsewhere silently
    # drops every controller mapping.
    proc = subprocess.Popen(cmd, cwd=BUILD)
    t0 = time.time()
    while time.time() - t0 < seconds:
        if proc.poll() is not None:
            break
        time.sleep(0.5)
    exited = proc.poll()
    if exited is None:
        proc.kill()          # always by PID, never by window title
        proc.wait(timeout=15)
    return log, exited, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=45.0)
    ap.add_argument("--outdir", default="out/bootcheck")
    ap.add_argument("--game-args", nargs=argparse.REMAINDER, default=[])
    args = ap.parse_args()

    outdir = os.path.join(ROOT, args.outdir)
    os.makedirs(outdir, exist_ok=True)
    if not os.path.exists(EXE):
        print("ERROR: %s not found - run tools/build.cmd first" % EXE)
        return 1

    rows = []
    for i in range(1, args.runs + 1):
        print("run %d/%d ..." % (i, args.runs), flush=True)
        log, exited, took = one_run(i, args.seconds, list(args.game_args), outdir)
        r = analyse(log) or {}
        r.update(run=i, exited=exited, took=took, log=log)
        rows.append(r)

    print("\n run  opens  menu  ring  badreg  fps first/last   exit   verdict")
    print(" ---  -----  ----  ----  ------  --------------   ----   -------")
    for r in rows:
        fps = r.get("fps") or [0.0]
        verdict = ("MENU" if r.get("menu") and not r.get("ring")
                   else "WEDGED" if r.get("ring")
                   else "INTRO" if r.get("opens", 0) >= 31
                   else "EARLY")
        print(" %3d  %5d  %4s  %4d  %6d  %6.1f/%6.1f   %4s   %s" % (
            r["run"], r.get("opens", 0), "yes" if r.get("menu") else "no",
            r.get("ring", 0), r.get("badreg", 0), fps[0], fps[-1],
            "-" if r["exited"] is None else str(r["exited"]), verdict))

    menus = sum(1 for r in rows if r.get("menu") and not r.get("ring"))
    print("\n%d/%d runs reached the menu with the ring buffer intact"
          % (menus, len(rows)))
    print("logs in %s" % outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
