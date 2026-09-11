"""Listener for the chapter 12 -> 13 hand-off (and anything else that stalls).

Runs OUTSIDE the game, read-only, by PID. What it does every 2 seconds:

  * reads the two hand-off flags straight out of guest memory
        0x84C39440  flag A - producer sets it, worker clears it
        0x84C39444  flag B - set by the main side after A clears
    and the chapter-12 site pointer at 0x84C23C48, and prints every transition
  * tails the newest log for chapter loads, texpack stage/warm lines, the
    chapter-12 guard, watchdog lines (minus the two known-benign waiters),
    ring faults, dirty-disc, errors
  * captures on a new watchdog line or on the log going quiet while the process
    lives. Flag A held for a long time is NOT a stall (it stays set for as long
    as a mode runs), so it is reported in the status line, never acted on.
    When the end-of-chapter screen sits there, capture by hand: `--capture`.

On a stall it takes a NON-INVASIVE cdb capture (all stacks, the flags, thread
CPU times), asks the running game to write its guest->host function table, and
writes an annotated copy of the capture where every `ng2+0xRVA` frame is named
as `sub_XXXXXXXX+off`. Everything lands in local/diag/captures/.

    python local/diag/watch_ch13.py              # waits for ng2.exe, then watches
    python local/diag/watch_ch13.py --capture    # capture right now, no waiting
    python local/diag/watch_ch13.py --resolve <capture.txt>   # re-annotate a capture

Read-only by construction: the process is opened with VM_READ|QUERY only, cdb
attaches with -pv (cannot resume, kill or disturb the target), and the one
write - CreateRemoteThread for the table dump - runs an exported function that
only writes a file. Nothing here can close the game.
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
import subprocess
import sys
import time
from datetime import datetime

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LOGDIR = os.path.join(ROOT, "out", "build", "win-amd64-Release", "logs")
CAPDIR = os.path.join(ROOT, "local", "diag", "captures")
FUNCTABLE = r"C:\ng2dump\functable.txt"
DUMP_TABLE_PY = os.path.join(ROOT, "tools", "dump_table_live.py")
CDB = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"

GUEST_BASE = 0x1_0000_0000          # host address of guest VA 0
FLAG_A = 0x84C39440
FLAG_B = 0x84C39444
CH12_SITE = 0x84C23C48

# The music bug, located 2026-09-06 09:35 from a live capture: both guest audio
# workers (sub_8374E740 / sub_8374E808, guest handles F80000DC / F80000E0) spin
# at 100% CPU in sub_8374F078, a rendezvous barrier. Each arriving thread
# writes 1 into its own byte of an 8-byte word (index = its hardware-thread
# number from r13+0x10C), then spins until the word equals a mask built from
# six per-hardware-thread slots of the audio engine object. The engine pointer
# is a global (`lwz r3,-17772(r30)`, r30 = 0x84C40000); the slots are at
# +308..+328 and the two alternating barrier words at +356 and +364
# (`addi r4,r31,356` / `addi r4,r11,356` with r11 = r31 + 8*(0|1)).
# A word that is non-zero and UNCHANGED for seconds is the livelock: normal
# rendezvous takes microseconds. The capture at that moment shows which byte
# is missing or stray, which is the fact the fix depends on.
AUDIO_ENGINE_PTR = 0x84C3BA94
AUDIO_SLOTS_OFF = 308
AUDIO_SLOT_COUNT = 6
AUDIO_BARRIER_OFFS = (356, 364)
AUDIO_STALL_SECONDS = 6.0
STALL_SECONDS = 15.0                # flag A held non-zero this long = hand-off stuck
QUIET_SECONDS = 45.0                # no log line this long while alive = freeze
# Title-screen parkers, documented benign: thread 8 on 40004BC4, thread 13 on
# F80000CC, and the short-lived threads each attract cycle creates (86, 90,
# 94... in log 044) that park ~30 s on 400FDC34 / 400FDCA4 and then go away.
# Every capture pauses the game for ~3 s, so a known-benign wait must not
# trigger one.
BENIGN_WAITS = ("40004BC4", "F80000CC", "400FDC34", "400FDCA4")
POLL = 2.0

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.CloseHandle.argtypes = [wt.HANDLE]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
READABLE = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD)]


def now():
    return datetime.now().strftime("%H:%M:%S")


def say(msg):
    print("[%s] %s" % (now(), msg), flush=True)


def find_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ng2.exe", "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines()[1:]:
        cells = [c.strip('"') for c in line.split('","')]
        if len(cells) > 1 and cells[0].lower() == "ng2.exe":
            return int(cells[1])
    return None


def newest_log(logdir=None):
    logdir = logdir or LOGDIR
    if not os.path.isdir(logdir):
        return None
    logs = [os.path.join(logdir, f) for f in os.listdir(logdir)
            if re.match(r"ng2_\d+\.log$", f)]
    return max(logs, key=os.path.getmtime) if logs else None


def logdir_for_pid(pid):
    """The logs folder beside the exe that is actually running - a release copy
    in ..\\Releases\\vX.Y.Z writes its logs there, not under the dev build."""
    try:
        h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not h:
            return LOGDIR
        buf = ctypes.create_unicode_buffer(1024)
        n = psapi.GetModuleFileNameExW(h, None, buf, 1024)
        k32.CloseHandle(h)
        if n:
            return os.path.join(os.path.dirname(buf.value), "logs")
    except Exception:
        pass
    return LOGDIR


class Guest:
    """Read-only view of guest memory through the virtual-base mapping."""

    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise OSError("OpenProcess failed: %d" % ctypes.get_last_error())

    def readable(self, host):
        mbi = MBI()
        if not k32.VirtualQueryEx(self.h, ctypes.c_void_p(host), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            return False
        return mbi.State == MEM_COMMIT and (mbi.Protect & READABLE) != 0 and not (mbi.Protect & 0x100)

    def read(self, guest_va, n):
        host = GUEST_BASE + guest_va
        if not self.readable(host):
            return None
        buf = (ctypes.c_ubyte * n)()
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(host), buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return bytes(buf)

    def u32be(self, guest_va):
        b = self.read(guest_va, 4)
        return struct.unpack(">I", b)[0] if b else None

    def audio_barrier(self):
        """(engine, slots[6], words[2] as 8-byte strings) or None."""
        engine = self.u32be(AUDIO_ENGINE_PTR)
        if not engine or engine < 0x10000:
            return None
        slots = [self.u32be(engine + AUDIO_SLOTS_OFF + 4 * i) for i in range(AUDIO_SLOT_COUNT)]
        words = [self.read(engine + off, 8) for off in AUDIO_BARRIER_OFFS]
        if any(s is None for s in slots) or any(w is None for w in words):
            return None
        return engine, slots, words

    def exe_base(self):
        mods = (ctypes.c_void_p * 8)()
        need = wt.DWORD()
        if not psapi.EnumProcessModules(self.h, ctypes.byref(mods), ctypes.sizeof(mods), ctypes.byref(need)):
            return None
        return mods[0]   # the main module is always first

    def close(self):
        k32.CloseHandle(self.h)


def load_functable(path=FUNCTABLE):
    """[(host, guest)] sorted by host, from dump_table_live's output."""
    rows = []
    with open(path) as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            h, g = line.split()
            rows.append((int(h, 16), int(g, 16)))
    rows.sort()
    return rows


def name_rva(table, base, rva):
    """Greatest table host address <= base+rva -> 'sub_GUEST+off'."""
    import bisect
    addr = base + rva
    i = bisect.bisect_right(table, (addr, 0xFFFFFFFF)) - 1
    if i < 0:
        return None
    host, guest = table[i]
    off = addr - host
    if off > 0x20000:      # too far from any function start to be inside it
        return None
    return "sub_%08X+0x%X" % (guest, off)


def base_from_capture(capture_path):
    """ng2.exe load address from the capture's own `lm m ng2` line, if present."""
    pat = re.compile(r"^([0-9a-fA-F`]{8,})\s+([0-9a-fA-F`]{8,})\s+ng2\b")
    with open(capture_path, errors="replace") as fh:
        for line in fh:
            m = pat.match(line)
            if m:
                return int(m.group(1).replace("`", ""), 16)
    return None


def annotate(capture_path, table, base=None):
    """Write <capture>.named.txt with every ng2+0xRVA frame named."""
    base = base_from_capture(capture_path) or base
    if base is None:
        raise ValueError("no ng2.exe base: capture has no `lm` line and none was given")
    pat = re.compile(r"\bng2\+0x([0-9a-fA-F]+)")
    out_path = capture_path.replace(".txt", ".named.txt")
    named = 0
    with open(capture_path, errors="replace") as src, open(out_path, "w") as dst:
        dst.write("# ng2.exe base %016X, table %s (%d entries)\n" % (base, FUNCTABLE, len(table)))
        for line in src:
            def sub(m):
                nonlocal named
                n = name_rva(table, base, int(m.group(1), 16))
                if n:
                    named += 1
                    return "%s [%s]" % (m.group(0), n)
                return m.group(0)
            dst.write(pat.sub(sub, line))
    return out_path, named


def cdb_capture(pid, why):
    os.makedirs(CAPDIR, exist_ok=True)
    out = os.path.join(CAPDIR, "ch13_%s_%s.txt" % (why, datetime.now().strftime("%H%M%S")))
    script = (
        '.printf "=== modules ===\\n"; lm m ng2; '
        '.printf "\\n=== all thread stacks ===\\n"; ~*k 40; '
        '.printf "\\n=== flags A/B at 0x84C39440 (host 0x184C39440), 8 dwords BE ===\\n"; dd 0x184C39440 L8; '
        '.printf "\\n=== chapter-12 site 0x84C23C48 ===\\n"; dd 0x184C23C48 L4; '
        '.printf "\\n=== thread times (spinning vs blocked) ===\\n"; !runaway 7; '
        '.printf "\\n=== locks ===\\n"; !locks; qd'
    )
    with open(out, "w") as fh:
        subprocess.run([CDB, "-pv", "-p", str(pid), "-c", script], stdout=fh, stderr=subprocess.STDOUT, timeout=300)
    return out


def dump_table():
    """Ask the running game for its live guest->host table (CreateRemoteThread)."""
    r = subprocess.run([sys.executable, DUMP_TABLE_PY], capture_output=True, text=True, timeout=60)
    ok = os.path.exists(FUNCTABLE) and (time.time() - os.path.getmtime(FUNCTABLE)) < 120
    return ok, (r.stdout + r.stderr).strip()


def full_capture(pid, guest, why):
    say(">>> CAPTURE (%s) - non-invasive attach, the game is paused for a few seconds" % why)
    try:
        cap = cdb_capture(pid, why)
        say("    stacks -> %s (%d bytes)" % (cap, os.path.getsize(cap)))
    except Exception as e:
        say("    capture FAILED: %s" % e)
        return
    ok, msg = dump_table()
    say("    table dump %s: %s" % ("ok" if ok else "FAILED", msg.replace("\n", " | ")[:200]))
    if ok:
        try:
            table = load_functable()
            base = guest.exe_base()
            named_path, n = annotate(cap, table, base)
            say("    named %d frames -> %s" % (n, named_path))
            summarise(named_path)
        except Exception as e:
            say("    annotate failed: %s" % e)


def summarise(named_path):
    """Top frame of every thread whose stack has a guest function, plus the runaway table."""
    txt = open(named_path, errors="replace").read()
    say("    --- guest threads (first named frame each) ---")
    cur = None
    shown = set()
    for line in txt.splitlines():
        m = re.match(r"^\.?\s*(\d+)\s+Id: [0-9a-f]+\.([0-9a-f]+) .*?(\"[^\"]*\")?\s*$", line)
        if m:
            cur = "%s %s %s" % (m.group(1), m.group(2), m.group(3) or "")
            continue
        m = re.search(r"\[(sub_[0-9A-F]+\+0x[0-9A-F]+)\]", line)
        if m and cur and cur not in shown:
            shown.add(cur)
            say("      thread %-28s %s" % (cur, m.group(1)))
    m = re.search(r"=== thread times.*?\n(.*?)\n=== locks", txt, re.S)
    if m:
        say("    --- CPU time per thread (top 8) ---")
        rows = [l for l in m.group(1).splitlines() if re.search(r"\d+:\d\d:\d\d", l)]
        for l in rows[:8]:
            say("      " + l.strip())


def fmt(v):
    return "----------" if v is None else "0x%08X" % v


def watch(pid):
    guest = Guest(pid)
    logdir = logdir_for_pid(pid)
    log = newest_log(logdir)
    say("watching pid %d, logs in %s, log %s" % (pid, logdir, os.path.basename(log) if log else "(none yet)"))
    say("flags: A=%s B=%s ch12site=%s" % (fmt(guest.u32be(FLAG_A)), fmt(guest.u32be(FLAG_B)), fmt(guest.u32be(CH12_SITE))))
    ab = guest.audio_barrier()
    if ab:
        say("audio engine at %s: slots %s words %s" % (fmt(ab[0]), [fmt(s) for s in ab[1]], [w.hex() for w in ab[2]]))
    else:
        say("audio engine pointer not readable yet (normal before the audio system starts)")
    barrier_seen = [(None, None), (None, None)]     # (value, since) per word
    barrier_reported = False

    pos = 0
    if log:
        pos = os.path.getsize(log)     # start from the current end; history is in the file
    last = {"A": None, "B": None, "S": None}
    a_nonzero_since = None
    stalled = False
    captures = {}
    last_status = time.time()
    last_fps = "?"
    chapter = "?"
    quiet_reported = False
    seen_waits = 0

    while True:
        time.sleep(POLL)
        if find_pid() != pid:
            say("PROCESS GONE (pid %d)" % pid)
            break

        # --- guest flags ---
        a, b, s = guest.u32be(FLAG_A), guest.u32be(FLAG_B), guest.u32be(CH12_SITE)
        for key, val in (("A", a), ("B", b), ("S", s)):
            if val != last[key]:
                say("flag %s: %s -> %s" % (key, fmt(last[key]), fmt(val)))
                last[key] = val
        # Flag A is NOT a short hand-off token. The 2026-09-06 07:07 capture
        # showed it held for 16s+ at a healthy 60 fps title screen: the main
        # thread sets it, then spins in sub_822F34B0 for as long as the worker
        # runs the current mode, and the worker clears it only when the mode
        # ends. So "held a long time" is the normal state of a running game,
        # not a stall - the old auto-capture on it paused the game for 2.7s at
        # boot and proved nothing. Track how long it has been held for the
        # status line; capture only on a watchdog line, log silence, or by hand
        # (`--capture` when the end-of-chapter screen sits there).
        if a:
            if a_nonzero_since is None:
                a_nonzero_since = time.time()
        else:
            a_nonzero_since = None

        # --- audio rendezvous barrier: non-zero and unchanged = livelock ---
        ab = guest.audio_barrier()
        if ab:
            engine, slots, words = ab
            expected = bytes([1 if s else 0 for s in slots]) + b"\x00\x00"
            for k, w in enumerate(words):
                val, since = barrier_seen[k]
                if w == b"\x00" * 8:
                    barrier_seen[k] = (None, None)
                    continue
                if w != val:
                    barrier_seen[k] = (w, time.time())
                    continue
                held = time.time() - since
                if held >= AUDIO_STALL_SECONDS and not barrier_reported:
                    barrier_reported = True
                    stray = [i for i in range(8) if w[i] and not expected[i]]
                    missing = [i for i in range(8) if expected[i] and not w[i]]
                    say("!!! AUDIO BARRIER STUCK %.0fs: word[%d]=%s expected=%s (slots %s) - "
                        "stray bytes %s, missing bytes %s"
                        % (held, k, w.hex(), expected.hex(), [fmt(s) for s in slots], stray, missing))
                    if "audio" not in captures:
                        captures["audio"] = time.time()
                        full_capture(pid, guest, "audio")
        # --- log tail ---
        new_log = newest_log(logdir)
        if new_log != log:
            log, pos = new_log, 0
            say("log switched to %s" % os.path.basename(log))
        if log:
            size = os.path.getsize(log)
            if size > pos:
                with open(log, errors="replace") as fh:
                    fh.seek(pos)
                    chunk = fh.read()
                    pos = fh.tell()
                for line in chunk.splitlines():
                    body = line[line.find("]", 30) + 1:] if "] [" in line else line
                    if "fps (" in line:
                        last_fps = line.split("[diag]")[-1].strip()[:60]
                        continue
                    if "is loading (from" in line:
                        chapter = line.split("chapter ")[1].split(" ")[0]
                        say("CHAPTER %s loading" % chapter)
                    elif "[texpack]" in line or "Patch: chapter-12" in line or "RINGDUMP" in line:
                        say("LOG " + line[line.find("] [t"):][:220])
                    elif "has been waiting" in line:
                        if not any(b in line for b in BENIGN_WAITS):
                            seen_waits += 1
                            say("WATCHDOG " + line[line.find("guest thread"):])
                            if "wait" not in captures:
                                captures["wait"] = time.time()
                                full_capture(pid, guest, "wait")
                    elif any(k in line for k in ("RINGBUFFER", "overflow", "DirtyDisc", "[error]", "FATAL", "Shutdown", "[critical]")):
                        say("LOG " + line[line.find("] [t"):][:220])
            gap = time.time() - os.path.getmtime(log)
            if gap >= QUIET_SECONDS:
                if not quiet_reported:
                    quiet_reported = True
                    say("LOG QUIET %.0fs while alive - possible freeze" % gap)
                    if "freeze" not in captures:
                        captures["freeze"] = time.time()
                        full_capture(pid, guest, "freeze")
            else:
                quiet_reported = False

        if time.time() - last_status >= 60:
            last_status = time.time()
            held = ("held %.0fs" % (time.time() - a_nonzero_since)) if a_nonzero_since else "clear"
            bar = ("barrier %s/%s" % (ab[2][0].hex(), ab[2][1].hex())) if ab else "barrier n/a"
            say("status: chapter %s | A=%s (%s) B=%s | %s | %s" % (chapter, fmt(a), held, fmt(b), bar, last_fps))
    guest.close()


def watch_forever(pid):
    """watch() with every exception written out - the 07:07 listener vanished
    after its first capture with an empty .err file, so anything that escapes
    the loop must leave a trace, and the watch resumes rather than ends."""
    while True:
        try:
            watch(pid)
            return
        except Exception:
            import traceback
            say("LISTENER EXCEPTION (resuming in 5s):")
            traceback.print_exc()
            sys.stdout.flush()
            time.sleep(5)
            if find_pid() != pid:
                say("PROCESS GONE (pid %d)" % pid)
                return


def main():
    args = sys.argv[1:]
    if args and args[0] == "--resolve":
        table = load_functable()
        base = None
        if len(args) > 2:
            base = int(args[2], 16)
        elif find_pid():
            base = Guest(find_pid()).exe_base()
        p, n = annotate(args[1], table, base)
        print("named %d frames -> %s" % (n, p))
        summarise(p)
        return 0
    pid = find_pid()
    if args and args[0] == "--capture":
        if not pid:
            print("no running ng2.exe")
            return 1
        full_capture(pid, Guest(pid), "manual")
        return 0
    while not pid:
        say("waiting for ng2.exe to start...")
        time.sleep(5)
        pid = find_pid()
    time.sleep(3)
    watch_forever(pid)
    return 0


if __name__ == "__main__":
    import faulthandler
    faulthandler.enable()          # a hard crash (ctypes) still leaves a traceback in .err
    sys.exit(main())
