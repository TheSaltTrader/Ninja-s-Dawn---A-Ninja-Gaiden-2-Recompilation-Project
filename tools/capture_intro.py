"""Launch ng2 and photograph its window on a timer, without a human watching.

Why this exists: the intro video can only be judged visually, and the two
obvious approaches are both wrong here.

  * PrintWindow cannot read a D3D12 swapchain - it returns a blank or stale
    surface for this window.
  * Grabbing the screen *region* under the window photographs whatever is
    actually on top of it, which is not necessarily this game and may be
    anything at all. That has already gone wrong once on this machine.

Windows Graphics Capture is the correct API: it captures a specific window's
composited output, including D3D12, and because the capture is bound to an
HWND it cannot pick up another window's pixels.

The HWND is resolved from the PID we launched, never by matching a window
title - a title match can land on somebody else's window.

    python tools/capture_intro.py --seconds 30 --interval 2 --outdir out/shots
"""

import argparse
import os
import subprocess
import sys
import time

import win32gui
import win32process
from windows_capture import WindowsCapture

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "out", "build", "win-amd64-Release", "ng2.exe")


def hwnd_for_pid(pid):
    """Top-level visible window owned by exactly this PID."""
    found = []

    def cb(h, _):
        if not win32gui.IsWindowVisible(h):
            return
        _, wpid = win32process.GetWindowThreadProcessId(h)
        if wpid == pid and win32gui.GetWindowText(h):
            found.append(h)

    win32gui.EnumWindows(cb, None)
    return found[0] if found else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--outdir", default="out/shots")
    ap.add_argument("--tag", default="shot")
    ap.add_argument("--exe", default=None,
                    help="run this executable instead of ng2 (e.g. xenia_canary.exe)")
    ap.add_argument("--cwd", default=None)
    ap.add_argument("--no-default-args", action="store_true")
    ap.add_argument("--game-args", nargs=argparse.REMAINDER, default=[])
    args = ap.parse_args()

    outdir = os.path.join(ROOT, args.outdir)
    os.makedirs(outdir, exist_ok=True)

    exe = args.exe or EXE
    base = [] if args.no_default_args else ["--game_data_root", "game"]
    cmd = [exe] + base + list(args.game_args)
    proc = subprocess.Popen(cmd, cwd=(args.cwd or ROOT))
    print("launched pid %d" % proc.pid)

    hwnd = None
    deadline = time.time() + 30
    while time.time() < deadline and hwnd is None:
        if proc.poll() is not None:
            print("process exited early rc=%s" % proc.returncode)
            return 1
        hwnd = hwnd_for_pid(proc.pid)
        if hwnd is None:
            time.sleep(0.25)
    if hwnd is None:
        proc.kill()
        print("no window appeared for pid %d" % proc.pid)
        return 1
    print("window 0x%X '%s'" % (hwnd, win32gui.GetWindowText(hwnd)))

    capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False,
                             draw_border=False)
    state = {"n": 0, "next": time.time(), "stop": time.time() + args.seconds}

    @capture.event
    def on_frame_arrived(frame, control):
        now = time.time()
        if now >= state["stop"]:
            control.stop()
            return
        if now < state["next"]:
            return
        state["next"] = now + args.interval
        state["n"] += 1
        path = os.path.join(outdir, "%s_%02d.png" % (args.tag, state["n"]))
        try:
            frame.save_as_image(path)
            print("  %5.1fs  %s  (%dx%d)" %
                  (now - (state["stop"] - args.seconds), os.path.basename(path),
                   frame.width, frame.height))
        except Exception as exc:  # a dropped frame must not kill the run
            print("  save failed: %s" % exc)

    @capture.event
    def on_closed():
        print("capture closed")

    try:
        capture.start()
    finally:
        # Always stop the process we started, by PID. Never by window title.
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        print("stopped pid %d; %d frames in %s" % (proc.pid, state["n"], outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
