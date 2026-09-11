"""Open the F10 settings overlay of a running ng2.exe, photograph it, scroll
down to the Textures section and photograph again, then close it.

    python local/diag/shoot_menu.py <pid>

Real input (ui_probe's helpers), so the game must be the foreground window;
every key/scroll is skipped otherwise and says so. Frames land in
local/diag/captures/menu_<n>.png. Never launches or kills anything.
"""
import ctypes
import os
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
CAPDIR = os.path.join(ROOT, "local", "diag", "captures")

import win32con                      # noqa: E402
import win32gui                      # noqa: E402
from windows_capture import WindowsCapture   # noqa: E402
import ui_probe                      # noqa: E402


def snap(hwnd, path):
    """One frame of the window to `path`."""
    capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False, draw_border=False)
    state = {"n": 0, "ok": False}

    @capture.event
    def on_frame_arrived(frame, control):
        state["n"] += 1
        if state["n"] < 4:
            return
        try:
            frame.save_as_image(path)
            state["ok"] = True
        except Exception as exc:          # noqa: BLE001
            print("save failed:", exc)
        control.stop()

    @capture.event
    def on_closed():
        pass

    capture.start()
    return state["ok"]


def main():
    pid = int(sys.argv[1])
    hwnd = ui_probe.hwnd_for_pid(pid)
    if hwnd is None:
        print("no window for pid", pid)
        return 1
    if not ui_probe.force_foreground(hwnd):
        print("could not bring the game to the foreground - nothing sent")
        return 2
    os.makedirs(CAPDIR, exist_ok=True)
    print("window 0x%X '%s'" % (hwnd, win32gui.GetWindowText(hwnd)))

    l, t, r, b = win32gui.GetWindowRect(hwnd)
    cx, cy = (l + r) // 2, (t + b) // 2

    ok = ui_probe.press(hwnd, win32con.VK_F10)
    print("F10 open:", "sent" if ok else "SKIPPED")
    time.sleep(1.5)
    p1 = os.path.join(CAPDIR, "menu_1_top.png")
    print("shot 1:", snap(hwnd, p1), p1)

    for i in range(2, 4):
        ok = ui_probe.wheel(hwnd, cx, cy, -12)
        print("scroll down:", "sent" if ok else "SKIPPED")
        time.sleep(0.8)
        p = os.path.join(CAPDIR, "menu_%d_scrolled.png" % i)
        print("shot %d:" % i, snap(hwnd, p), p)

    ok = ui_probe.press(hwnd, win32con.VK_F10)
    print("F10 close:", "sent" if ok else "SKIPPED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
