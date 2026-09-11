"""Photograph a running ng2.exe (by PID) every --interval seconds for --seconds.

    python local/diag/shots_by_pid.py <pid> --seconds 25 --interval 0.4 --tag warm

Frames land in local/diag/captures/<tag>_NN_<t>.png. Each line printed gives
the frame's mean brightness and the mean of the bottom strip, so a frame with
an overlay on an otherwise black boot screen stands out in the listing without
opening every file. Never launches or kills anything.
"""
import argparse
import ctypes
import os
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)

import numpy as np                    # noqa: E402
import win32gui                       # noqa: E402
import win32process                   # noqa: E402
from windows_capture import WindowsCapture   # noqa: E402

CAPDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")


def hwnd_for_pid(pid):
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
    ap.add_argument("pid", type=int)
    ap.add_argument("--seconds", type=float, default=25.0)
    ap.add_argument("--interval", type=float, default=0.4)
    ap.add_argument("--tag", default="shot")
    args = ap.parse_args()

    hwnd = None
    deadline = time.time() + 30
    while hwnd is None and time.time() < deadline:
        hwnd = hwnd_for_pid(args.pid)
        if hwnd is None:
            time.sleep(0.25)
    if hwnd is None:
        print("no window for pid %d" % args.pid)
        return 1
    print("window 0x%X '%s'" % (hwnd, win32gui.GetWindowText(hwnd)))
    os.makedirs(CAPDIR, exist_ok=True)

    capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False, draw_border=False)
    t0 = time.time()
    state = {"n": 0, "next": t0, "stop": t0 + args.seconds}

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
        t = now - t0
        path = os.path.join(CAPDIR, "%s_%02d_%04.1fs.png" % (args.tag, state["n"], t))
        try:
            buf = frame.frame_buffer
            lum = np.asarray(buf[:, :, :3], dtype=np.float32).mean(axis=2)
            h = lum.shape[0]
            whole = float(lum.mean())
            bottom = float(lum[int(h * 0.80):, :].mean())
            middle = float(lum[int(h * 0.40):int(h * 0.60), :].mean())
            frame.save_as_image(path)
            print("  %5.1fs  %-28s mean %5.1f  middle %5.1f  bottom %5.1f"
                  % (t, os.path.basename(path), whole, middle, bottom), flush=True)
        except Exception as exc:          # noqa: BLE001
            print("  save failed: %s" % exc)

    @capture.event
    def on_closed():
        pass

    capture.start()
    print("%d frames" % state["n"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
