"""windiff2.py - the picture check for the native window against the game's own window, on a STATIC scene.

    python tools/native_gpu/windiff2.py --tag menu1 [--out D:/ng2_frameinterp/captures] [--pairs 1]
    python tools/native_gpu/windiff2.py --compare A.png B.png          (two saved client crops, any origin)

Captures one frame from each ng2.exe top-level window ("Ninja Gaiden II - native" and the game's
"Ninja Gaiden II  -  v...") through Windows Graphics Capture, as close together in time as two capture
sessions allow (the capture timestamps are printed - on a static scene the gap does not matter, on a
moving one it is the first suspect), saves both client areas as PNG, and prints Fable II's metric
(tools/windiff.py in the kit): per-channel mean |d| in 0..1 over both clients resized to 640x360, and
the fraction of pixels differing by more than 0.1 in any channel.

THE FLOOR COMES FIRST (the Fable rule): before any native-vs-plugin number means anything, run
--pairs 2 on the same static scene and read the native-vs-native and plugin-vs-plugin differences
across the two captures; a native-vs-plugin difference is only a finding if it clears that floor.
Fable II's floors were 0.0010 (both), against 0.0027-0.0035 native vs plugin.

The client area is taken from the window geometry (the caption height from GetClientRect vs the
window rect), never by detecting the caption by colour - Fable's crops misaligned on a white sky.
"""
import argparse
import ctypes
import os
import sys
import threading
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)

import numpy as np                    # noqa: E402
import win32gui                       # noqa: E402
import win32process                   # noqa: E402
from PIL import Image                 # noqa: E402
from windows_capture import WindowsCapture   # noqa: E402


def ng2_windows():
    found = {}

    def cb(h, _):
        if not win32gui.IsWindowVisible(h):
            return
        title = win32gui.GetWindowText(h)
        if not title:
            return
        _, pid = win32process.GetWindowThreadProcessId(h)
        try:
            import psutil
            name = psutil.Process(pid).name().lower()
        except Exception:
            name = ""
        if "ninja gaiden ii" in title.lower() and (name == "ng2.exe" or name == ""):
            found["native" if "native" in title.lower() else "game"] = h

    win32gui.EnumWindows(cb, None)
    return found


def client_rect_in_window(hwnd):
    """(left, top, width, height) of the client area within the window's own capture, in physical pixels."""
    l, t, r, b = win32gui.GetWindowRect(hwnd)
    cl, ct = win32gui.ClientToScreen(hwnd, (0, 0))
    cw = win32gui.GetClientRect(hwnd)[2]
    ch = win32gui.GetClientRect(hwnd)[3]
    return cl - l, ct - t, cw, ch


def grab(hwnd, label):
    """One frame from a window as an RGB numpy array, plus the wall-clock time it arrived."""
    result = {}
    done = threading.Event()
    cap = WindowsCapture(window_hwnd=hwnd, cursor_capture=False, draw_border=False)

    @cap.event
    def on_frame_arrived(frame, control):
        if "img" not in result:
            result["img"] = frame.frame_buffer[:, :, :3][:, :, ::-1].copy()   # BGRA -> RGB
            result["t"] = time.time()
            done.set()
            control.stop()

    @cap.event
    def on_closed():
        done.set()

    th = threading.Thread(target=cap.start, daemon=True)
    th.start()
    done.wait(5.0)
    if "img" not in result:
        raise RuntimeError("no frame from the %s window" % label)
    return result["img"], result["t"]


def client_crop(img, hwnd):
    l, t, w, h = client_rect_in_window(hwnd)
    return img[t:t + h, l:l + w]


def metric(a, b):
    ia = Image.fromarray(a).resize((640, 360), Image.BILINEAR)
    ib = Image.fromarray(b).resize((640, 360), Image.BILINEAR)
    n = np.asarray(ia, dtype=np.float32) / 255.0
    p = np.asarray(ib, dtype=np.float32) / 255.0
    d = np.abs(n - p)
    return d.mean(), (d.max(axis=2) > 0.1).mean(), n.mean(), p.mean()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="pair")
    ap.add_argument("--out", default="D:/ng2_frameinterp/captures")
    ap.add_argument("--pairs", type=int, default=1, help="capture this many native/game pairs, 2 s apart (floor: 2)")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    args = ap.parse_args()
    if args.compare:
        a = np.asarray(Image.open(args.compare[0]).convert("RGB"))
        b = np.asarray(Image.open(args.compare[1]).convert("RGB"))
        md, big, ma, mb = metric(a, b)
        print("mean|d| %.4f   >0.1: %.1f%%   A mean %.3f  B mean %.3f" % (md, 100 * big, ma, mb))
        return
    wins = ng2_windows()
    if "native" not in wins or "game" not in wins:
        print("need both windows; found: %s" % {k: hex(v) for k, v in wins.items()})
        sys.exit(2)
    os.makedirs(args.out, exist_ok=True)
    pairs = []
    for i in range(args.pairs):
        # Two capture sessions started back to back; the arrival times are printed.
        res = {}
        ths = [threading.Thread(target=lambda k=k, h=h: res.__setitem__(k, grab(h, k))) for k, h in wins.items()]
        for th in ths:
            th.start()
        for th in ths:
            th.join(8.0)
        if "native" not in res or "game" not in res:
            print("capture failed on pair %d" % i)
            sys.exit(3)
        n_img, n_t = res["native"]
        g_img, g_t = res["game"]
        n_c = client_crop(n_img, wins["native"])
        g_c = client_crop(g_img, wins["game"])
        n_path = os.path.join(args.out, "%s_%d_native.png" % (args.tag, i))
        g_path = os.path.join(args.out, "%s_%d_game.png" % (args.tag, i))
        Image.fromarray(n_c).save(n_path)
        Image.fromarray(g_c).save(g_path)
        md, big, mn, mg = metric(n_c, g_c)
        print("pair %d: native %dx%d @%.3f  game %dx%d @%.3f  gap %.0f ms | native vs game mean|d| %.4f  >0.1: %.1f%%  "
              "(native mean %.3f, game mean %.3f)" % (i, n_c.shape[1], n_c.shape[0], n_t, g_c.shape[1], g_c.shape[0], g_t,
                                                      abs(n_t - g_t) * 1000, md, 100 * big, mn, mg))
        pairs.append((n_c, g_c))
        if i + 1 < args.pairs:
            time.sleep(2.0)
    if len(pairs) >= 2:
        # The floors: each window against itself across captures. A native-vs-game difference below these is noise.
        for a in range(len(pairs) - 1):
            mdn, bign, _, _ = metric(pairs[a][0], pairs[a + 1][0])
            mdg, bigg, _, _ = metric(pairs[a][1], pairs[a + 1][1])
            print("floor %d->%d: native vs native mean|d| %.4f (>0.1: %.1f%%)   game vs game mean|d| %.4f (>0.1: %.1f%%)"
                  % (a, a + 1, mdn, 100 * bign, mdg, 100 * bigg))


if __name__ == "__main__":
    main()
