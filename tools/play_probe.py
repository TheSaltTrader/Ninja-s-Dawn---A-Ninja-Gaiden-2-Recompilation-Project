"""Launch ng2, photograph its window, and drive it with scripted key presses.

This is capture_intro.py plus input: the title screen cannot be got past
without pressing Start, so "does the game actually play?" is not answerable by
capture alone.

The same two rules as capture_intro.py apply and are not negotiable:

  * the window is resolved from the PID we launched, never by title, and
  * the process is killed by PID, never by window title.

Input is sent one of two ways:

  --method post   PostMessage(WM_KEYDOWN/WM_KEYUP) straight to the window.
                  Does not steal focus, so it cannot type into whatever the
                  user is doing. SDL's win32 backend may ignore keys while the
                  window is not focused, in which case nothing happens.
  --method input  SetForegroundWindow + SendInput. Real system-level input, so
                  it always works - but it types into the *foreground* window,
                  so every send re-checks that the foreground window is still
                  ours and skips the key otherwise.

    python tools/play_probe.py --seconds 180 --interval 3 \
        --keys 60:RETURN,66:SPACE,72:3,78:X,84:Z,90:A --outdir out/probe
"""

import argparse
import ctypes
import os
import subprocess
import sys
import threading
import time

import win32api
import win32con
import win32gui
import win32process
from windows_capture import WindowsCapture

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "out", "build", "win-amd64-Release", "ng2.exe")

# Virtual-key codes for the names accepted in --keys.
VK = {
    "RETURN": win32con.VK_RETURN,
    "SPACE": win32con.VK_SPACE,
    "ESCAPE": win32con.VK_ESCAPE,
    "UP": win32con.VK_UP,
    "DOWN": win32con.VK_DOWN,
    "LEFT": win32con.VK_LEFT,
    "RIGHT": win32con.VK_RIGHT,
    "LCONTROL": win32con.VK_CONTROL,
    "LSHIFT": win32con.VK_SHIFT,
    "BACK": win32con.VK_BACK,
    "TAB": win32con.VK_TAB,
}
for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789":
    VK.setdefault(_c, ord(_c))
for _n in range(1, 13):  # F1-F12: the SDK binds its overlays to these
    VK["F%d" % _n] = win32con.VK_F1 + _n - 1
VK["BACKTICK"] = 0xC0  # VK_OEM_3, the console overlay's default bind
VK["TILDE"] = 0xC0


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


def parse_keys(spec):
    """'60:RETURN,66:SPACE' -> [(60.0, 'RETURN', vk), ...] sorted by time."""
    out = []
    for item in filter(None, (s.strip() for s in spec.split(","))):
        at, _, name = item.partition(":")
        name = name.strip().upper()
        if name not in VK:
            raise SystemExit("unknown key %r (known: %s)"
                             % (name, ",".join(sorted(VK))))
        out.append((float(at), name, VK[name]))
    return sorted(out)


def send_post(hwnd, vk, hold):
    scan = win32api.MapVirtualKey(vk, 0)
    lp_down = 1 | (scan << 16)
    lp_up = lp_down | (1 << 30) | (1 << 31)
    win32api.PostMessage(hwnd, win32con.WM_KEYDOWN, vk, lp_down)
    time.sleep(hold)
    win32api.PostMessage(hwnd, win32con.WM_KEYUP, vk, lp_up)
    return True


def force_foreground(hwnd):
    """Windows refuses SetForegroundWindow from a process that does not own the
    foreground. Attaching to the current foreground thread's input queue lifts
    that restriction for the duration."""
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return True
    # Two more things are needed beyond AttachThreadInput: the foreground lock
    # timeout has to be zero, and the shell only releases the foreground after
    # it has seen a keystroke - a bare Alt press is the usual way to give it
    # one. Without both, SetForegroundWindow returns success and does nothing.
    user32.SystemParametersInfoW(0x2001, 0, ctypes.c_void_p(0), 0)
    fg_thread = user32.GetWindowThreadProcessId(fg, None)
    our_thread = kernel32.GetCurrentThreadId()
    attached = fg_thread and fg_thread != our_thread and \
        user32.AttachThreadInput(our_thread, fg_thread, True)
    try:
        win32api.keybd_event(win32con.VK_MENU, 0, 0, 0)
        win32api.keybd_event(win32con.VK_MENU, 0, win32con.KEYEVENTF_KEYUP, 0)
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(our_thread, fg_thread, False)
    time.sleep(0.25)
    return user32.GetForegroundWindow() == hwnd


def send_input(hwnd, vk, hold):
    """System-level input - only ever while our window is the foreground one."""
    if win32gui.GetForegroundWindow() != hwnd:
        force_foreground(hwnd)
    if win32gui.GetForegroundWindow() != hwnd:
        return False  # somebody else owns the keyboard; do not type into it
    scan = win32api.MapVirtualKey(vk, 0)
    win32api.keybd_event(vk, scan, 0, 0)
    time.sleep(hold)
    win32api.keybd_event(vk, scan, win32con.KEYEVENTF_KEYUP, 0)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--interval", type=float, default=3.0)
    ap.add_argument("--outdir", default="out/probe")
    ap.add_argument("--tag", default="probe")
    ap.add_argument("--keys", default="",
                    help="comma separated <seconds>:<KEY> presses")
    ap.add_argument("--hold", type=float, default=0.12,
                    help="how long each key is held down")
    ap.add_argument("--method", choices=("post", "input"), default="post")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--game-args", nargs=argparse.REMAINDER, default=[])
    args = ap.parse_args()

    outdir = os.path.join(ROOT, args.outdir)
    os.makedirs(outdir, exist_ok=True)
    schedule = parse_keys(args.keys)

    cmd = [args.exe or EXE, "--game_data_root", "game"] + list(args.game_args)
    proc = subprocess.Popen(cmd, cwd=ROOT)
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

    t0 = time.time()
    sender = send_post if args.method == "post" else send_input
    log = []

    def key_thread():
        for at, name, vk in schedule:
            wait = t0 + at - time.time()
            if wait > 0:
                time.sleep(wait)
            if proc.poll() is not None:
                return
            ok = sender(hwnd, vk, args.hold)
            line = "  %5.1fs  KEY %-8s %s" % (time.time() - t0, name,
                                              "sent" if ok else "SKIPPED")
            print(line)
            log.append(line)

    threading.Thread(target=key_thread, daemon=True).start()

    capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False,
                             draw_border=False)
    state = {"n": 0, "next": t0, "stop": t0 + args.seconds}

    @capture.event
    def on_frame_arrived(frame, control):
        now = time.time()
        if now >= state["stop"] or proc.poll() is not None:
            control.stop()
            return
        if now < state["next"]:
            return
        state["next"] = now + args.interval
        state["n"] += 1
        path = os.path.join(outdir, "%s_%03d_%03ds.png"
                            % (args.tag, state["n"], int(now - t0)))
        try:
            frame.save_as_image(path)
            print("  %5.1fs  %s  (%dx%d)"
                  % (now - t0, os.path.basename(path), frame.width,
                     frame.height))
        except Exception as exc:  # a dropped frame must not kill the run
            print("  save failed: %s" % exc)

    @capture.event
    def on_closed():
        print("capture closed")

    try:
        capture.start()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        print("stopped pid %d; %d frames in %s" % (proc.pid, state["n"], outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
