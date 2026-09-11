"""Drive the settings UI with scripted mouse clicks and photograph the result.

play_probe.py sends keys, which is enough for the game itself (it only wants
Start). The settings menu is ImGui: tabs, combos and buttons are all mouse
targets, so "does the Graphics tab render" is not answerable without a pointer.

Clicks are REAL system input (SetCursorPos + mouse_event). PostMessage was
tried first and does nothing: SDL3's Windows backend reads the pointer from
raw input and ignores synthesised window messages, so the clicks vanished and
the screenshots looked exactly like a UI that had not been touched - a silent
failure worth knowing about. Because the cursor really moves, every click
re-checks that our window is the foreground one and is skipped otherwise, so
it can never click into whatever the user is doing.

The two hard rules from capture_intro.py apply here unchanged:

  * the window is resolved from the PID we launched, never by title, and
  * the process is killed by PID, never by window title.

COORDINATES ARE READ OFF THE SCREENSHOT. Windows Graphics Capture photographs
the whole window including its border and title bar, so a point measured in a
shot is offset from the client-area coordinate the click needs. The tool
measures that offset itself (ClientToScreen against GetWindowRect) and
converts, so what you type is what you clicked on.

    python tools/ui_probe.py --seconds 30 --shots 3,10,20 \
        --clicks 5:120,80 12:250,80 --outdir out/ui
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

# THIS HAS TO HAPPEN BEFORE ANY WIN32 CALL, and it is the whole reason the
# first version of this tool "worked" while every click landed 25% away from
# where it was aimed.
#
# Windows Graphics Capture hands back PHYSICAL pixels (a 1280x720 window on a
# 125% display photographs as 1600x900). A DPI-unaware process, though, gets
# every Win32 coordinate virtualised into logical units - GetWindowRect,
# ClientToScreen and SetCursorPos all silently in a different space from the
# screenshot the coordinates were measured on. Clicks then land at 1.25x the
# intended offset, hit some other control, and nothing announces the mismatch.
#
# It cost a rebuild of the game to find, because the obvious suspect was the
# app: ImGui really does run in a 1280x720 logical space here. It is correct.
# The probe was not.
ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PER_MONITOR_AWARE_V2


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


def client_offset(hwnd):
    """(dx, dy) to subtract from a screenshot point to get a client point."""
    wl, wt, _, _ = win32gui.GetWindowRect(hwnd)
    cl, ct = win32gui.ClientToScreen(hwnd, (0, 0))
    return cl - wl, ct - wt


def force_foreground(hwnd):
    """Windows refuses SetForegroundWindow from a process that does not own the
    foreground. Attaching to the current foreground thread's input queue lifts
    that restriction for the duration. Lifted verbatim from play_probe.py."""
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return True
    user32.SystemParametersInfoW(0x2001, 0, ctypes.c_void_p(0), 0)
    fg_thread = user32.GetWindowThreadProcessId(fg, None)
    our_thread = kernel32.GetCurrentThreadId()
    attached = fg_thread and fg_thread != our_thread and         user32.AttachThreadInput(our_thread, fg_thread, True)
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


def click(hwnd, x, y):
    """One left click at a client coordinate.

    PostMessage(WM_LBUTTON*) does NOT work here: SDL3's Windows backend takes
    the pointer from raw input and ignores synthesised window messages, so the
    clicks land nowhere and the screenshot looks like nothing happened. Real
    system-level input is the only thing ImGui sees.

    That means the cursor genuinely moves, so the same guard play_probe.py uses
    applies: the foreground window is re-checked immediately before every
    click, and a click is skipped rather than typed into somebody else's
    window.
    """
    if win32gui.GetForegroundWindow() != hwnd:
        force_foreground(hwnd)
    if win32gui.GetForegroundWindow() != hwnd:
        return False
    sx, sy = win32gui.ClientToScreen(hwnd, (x, y))
    win32api.SetCursorPos((sx, sy))
    time.sleep(0.20)  # ImGui reacts to the move before the press
    win32api.mouse_event(win32con.MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.10)
    win32api.mouse_event(win32con.MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.20)
    return True


def wheel(hwnd, x, y, notches):
    """Turn the mouse wheel over a client coordinate.

    ImGui scrolls whatever window is under the pointer, so the position
    matters as much as the direction - a turn over the footer scrolls nothing.
    """
    if win32gui.GetForegroundWindow() != hwnd:
        force_foreground(hwnd)
    if win32gui.GetForegroundWindow() != hwnd:
        return False
    sx, sy = win32gui.ClientToScreen(hwnd, (x, y))
    win32api.SetCursorPos((sx, sy))
    time.sleep(0.15)
    for _ in range(abs(notches)):
        win32api.mouse_event(win32con.MOUSEEVENTF_WHEEL, 0, 0,
                             120 if notches > 0 else -120, 0)
        time.sleep(0.06)
    time.sleep(0.20)
    return True


def press(hwnd, vk, shift=False):
    """One key press, under the same foreground guard as a click.

    `shift` is needed because several of the runtime's default binds are
    modifier combinations - the d-pad is Shift+Arrow - and a bare arrow does
    nothing at all in menus that navigate with it.
    """
    if win32gui.GetForegroundWindow() != hwnd:
        force_foreground(hwnd)
    if win32gui.GetForegroundWindow() != hwnd:
        return False
    scan = win32api.MapVirtualKey(vk, 0)
    if shift:
        win32api.keybd_event(win32con.VK_SHIFT,
                             win32api.MapVirtualKey(win32con.VK_SHIFT, 0), 0, 0)
        time.sleep(0.04)
    win32api.keybd_event(vk, scan, 0, 0)
    time.sleep(0.12)
    win32api.keybd_event(vk, scan, win32con.KEYEVENTF_KEYUP, 0)
    if shift:
        time.sleep(0.04)
        win32api.keybd_event(win32con.VK_SHIFT,
                             win32api.MapVirtualKey(win32con.VK_SHIFT, 0),
                             win32con.KEYEVENTF_KEYUP, 0)
    time.sleep(0.15)
    return True


VK = {"F%d" % n: win32con.VK_F1 + n - 1 for n in range(1, 13)}
# Every letter and digit, so a keybind like keybind_x=L can be driven by name.
VK.update({c: ord(c) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"})
VK.update({"RETURN": win32con.VK_RETURN, "SPACE": win32con.VK_SPACE,
           "ESCAPE": win32con.VK_ESCAPE, "TAB": win32con.VK_TAB,
           "UP": win32con.VK_UP, "DOWN": win32con.VK_DOWN,
           "LEFT": win32con.VK_LEFT, "RIGHT": win32con.VK_RIGHT,
           "BACKSPACE": win32con.VK_BACK, "DELETE": win32con.VK_DELETE,
           "HOME": win32con.VK_HOME, "END": win32con.VK_END,
           "BACKTICK": 0xC0, "SEMICOLON": 0xBA, "QUOTE": 0xDE,
           "X": 0x58, "Z": 0x5A, "W": 0x57, "S": 0x53})


def parse_clicks(items):
    out = []
    for item in items:
        at, _, coords = item.partition(":")
        x, _, y = coords.partition(",")
        out.append((float(at), int(x), int(y)))
    return sorted(out)


def parse_wheels(items):
    """'12:400,300,-6' -> [(12.0, 400, 300, -6)], negative scrolls down."""
    out = []
    for item in items:
        at, _, rest = item.partition(":")
        x, y, n = rest.split(",")
        out.append((float(at), int(x), int(y), int(n)))
    return sorted(out)


def parse_keys(spec):
    """'60:RETURN,70:SHIFT+LEFT' -> [(60.0, 'RETURN', vk, shift), ...]"""
    out = []
    for item in filter(None, (s.strip() for s in spec.split(","))):
        at, _, name = item.partition(":")
        name = name.strip().upper()
        shift = name.startswith("SHIFT+")
        if shift:
            name = name[len("SHIFT+"):]
        if name not in VK:
            raise SystemExit("unknown key %r" % name)
        out.append((float(at), ("SHIFT+" if shift else "") + name, VK[name],
                    shift))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--shots", default="",
                    help="comma separated seconds at which to capture")
    ap.add_argument("--interval", type=float, default=0.0,
                    help="capture every N seconds instead of --shots")
    ap.add_argument("--clicks", nargs="*", default=[],
                    help="<seconds>:<x>,<y>, measured on a screenshot")
    ap.add_argument("--keys", default="",
                    help="comma separated <seconds>:<KEY> presses")
    ap.add_argument("--wheel", nargs="*", default=[],
                    help="<seconds>:<x>,<y>,<notches>; negative scrolls down")
    ap.add_argument("--outdir", default="out/ui")
    ap.add_argument("--tag", default="ui")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--game-args", nargs=argparse.REMAINDER, default=[])
    args = ap.parse_args()

    outdir = os.path.join(ROOT, args.outdir)
    os.makedirs(outdir, exist_ok=True)
    clicks = parse_clicks(args.clicks)
    wheels = parse_wheels(args.wheel)
    keys = parse_keys(args.keys)
    shots = [float(s) for s in args.shots.split(",") if s.strip()]
    if args.interval > 0:
        shots = [args.interval * i
                 for i in range(1, int(args.seconds / args.interval) + 1)]

    cmd = [args.exe or EXE] + list(args.game_args)
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
    dx, dy = client_offset(hwnd)
    print("window 0x%X '%s'  client offset %d,%d"
          % (hwnd, win32gui.GetWindowText(hwnd), dx, dy))

    capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False,
                             draw_border=False)
    t0 = time.time()
    state = {"n": 0, "want": False, "stop": t0 + args.seconds}

    @capture.event
    def on_frame_arrived(frame, control):
        if time.time() >= state["stop"]:
            control.stop()
            return
        if not state["want"]:
            return
        state["want"] = False
        state["n"] += 1
        path = os.path.join(outdir, "%s_%02d_%03ds.png"
                            % (args.tag, state["n"], int(time.time() - t0)))
        try:
            frame.save_as_image(path)
            print("  %5.1fs  %s  (%dx%d)" % (time.time() - t0,
                                             os.path.basename(path),
                                             frame.width, frame.height))
        except Exception as exc:  # a dropped frame must not kill the run
            print("  save failed: %s" % exc)

    @capture.event
    def on_closed():
        print("capture closed")

    def driver():
        pending = list(clicks)
        pending_keys = list(keys)
        pending_wheels = list(wheels)
        want_shots = list(shots)
        while time.time() < state["stop"]:
            now = time.time() - t0
            while pending_keys and pending_keys[0][0] <= now:
                _, name, vk, shift = pending_keys.pop(0)
                ok = press(hwnd, vk, shift)
                print("  %5.1fs  KEY %-8s %s"
                      % (time.time() - t0, name, "" if ok else "SKIPPED"))
            while pending and pending[0][0] <= now:
                _, x, y = pending.pop(0)
                ok = click(hwnd, x - dx, y - dy)
                print("  %5.1fs  CLICK %d,%d %s"
                      % (time.time() - t0, x, y, "" if ok else "SKIPPED"))
            while pending_wheels and pending_wheels[0][0] <= now:
                _, x, y, n = pending_wheels.pop(0)
                ok = wheel(hwnd, x - dx, y - dy, n)
                print("  %5.1fs  WHEEL %d,%d %+d %s"
                      % (time.time() - t0, x, y, n, "" if ok else "SKIPPED"))
            while want_shots and want_shots[0] <= now:
                want_shots.pop(0)
                state["want"] = True
            time.sleep(0.05)

    threading.Thread(target=driver, daemon=True).start()

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
