"""Photograph a running ng2.exe (by PID, never by title) and measure whether the
picture is pillarboxed: mean brightness of the outer columns against the middle.

    python local/diag/verify_pillarbox.py <pid> [out.png]

Prints the frame size, the left/centre/right means and a verdict. A 16:9 frame
inside a 3840x1600 surface leaves ~498 px of black each side, so the outer 10%
of columns should be near zero while the centre is not.
"""
import ctypes
import os
import sys
import time

# Before ANY win32 call - see ui_probe.py for why.
ctypes.windll.shcore.SetProcessDpiAwareness(2)

import numpy as np                    # noqa: E402
import win32gui                       # noqa: E402
import win32process                   # noqa: E402
from windows_capture import WindowsCapture   # noqa: E402


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
    pid = int(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "captures", "pillarbox_%d.png" % pid)
    hwnd = hwnd_for_pid(pid)
    if hwnd is None:
        print("no visible window for pid %d" % pid)
        return 1
    print("window 0x%X '%s'" % (hwnd, win32gui.GetWindowText(hwnd)))

    capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False, draw_border=False)
    state = {"frames": 0, "done": False}

    @capture.event
    def on_frame_arrived(frame, control):
        state["frames"] += 1
        if state["frames"] < 5:          # let the swap chain settle
            return
        buf = frame.frame_buffer          # H x W x 4, BGRA
        img = np.array(buf[:, :, :3], dtype=np.float32)
        h, w = img.shape[:2]
        lum = img.mean(axis=2)
        edge = max(8, int(w * 0.10))
        left = float(lum[:, :edge].mean())
        right = float(lum[:, w - edge:].mean())
        centre = float(lum[:, w // 2 - w // 6: w // 2 + w // 6].mean())
        top = float(lum[: max(8, int(h * 0.05)), :].mean())
        # Where do the bars end? First/last column whose mean exceeds a floor.
        col = lum.mean(axis=0)
        lit = np.where(col > 6.0)[0]
        first, last = (int(lit[0]), int(lit[-1])) if lit.size else (-1, -1)
        try:
            frame.save_as_image(out)
        except Exception as exc:          # noqa: BLE001
            print("save failed: %s" % exc)
        print("frame %dx%d  left %.1f  centre %.1f  right %.1f  top %.1f"
              % (w, h, left, centre, right, top))
        print("lit columns %d..%d of %d  (picture width %d, %.3f of frame)"
              % (first, last, w, last - first + 1, (last - first + 1) / float(w)))
        if centre > 12.0 and left < 4.0 and right < 4.0:
            print("VERDICT: PILLARBOXED - black bars both sides, picture in the middle")
        elif centre > 12.0:
            print("VERDICT: NOT pillarboxed - the picture reaches the edges")
        else:
            print("VERDICT: frame too dark to judge (centre %.1f) - try again later" % centre)
        print("saved %s" % out)
        state["done"] = True
        control.stop()

    @capture.event
    def on_closed():
        pass

    capture.start()
    return 0 if state["done"] else 2


if __name__ == "__main__":
    sys.exit(main())
