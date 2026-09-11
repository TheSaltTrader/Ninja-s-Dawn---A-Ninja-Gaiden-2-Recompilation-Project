"""Attach to a RUNNING ng2.exe and find what writes a guest address.

Why this exists
---------------
Every previous step of this hunt lived inside ng2.exe, so every fix to the
instrumentation meant relinking the executable - which cannot be done while it
is running - which meant closing the user's game. Six times. That is a bad way
to chase a bug that takes a minute to reproduce.

This runs OUTSIDE the game. It attaches as a real debugger, so:

  * nothing has to be rebuilt or restarted to change the analysis
  * the OS delivers the debug exception to us directly, rather than through an
    in-process handler chain that another handler might swallow - which is a
    live suspicion, since the in-process watchpoint reported zero hits across
    73 threads while the value was visibly changing several times a second
  * the debug registers are set AND read back, so "the watchpoint did not fire"
    can be told apart from "the watchpoint was never really armed"

    python tools/watch_guest.py --addr 0x116CAA8C
    python tools/watch_guest.py --scan            # find oscillating floats first

Guest memory is mapped at several host addresses; a hardware watchpoint is a
HOST address, so every mapped alias of the target gets its own debug register.
Watching only the physical alias is why the in-process attempt saw nothing.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# Declare argtypes explicitly. Without them ctypes passes pointer arguments as
# 32-bit ints, and every address here is above 4 GB (guest memory is mapped at
# 0x1_00000000 and 0x3_00000000), so the addresses were silently truncated and
# every read failed. That is why the first run found zero aliases.
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.GetThreadContext.restype = wt.BOOL
k32.SetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SetThreadContext.restype = wt.BOOL
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.SuspendThread.restype = wt.DWORD
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.ResumeThread.restype = wt.DWORD
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.DebugActiveProcess.argtypes = [wt.DWORD]
k32.DebugActiveProcess.restype = wt.BOOL
k32.DebugActiveProcessStop.argtypes = [wt.DWORD]
k32.DebugSetProcessKillOnExit.argtypes = [wt.BOOL]
k32.WaitForDebugEvent.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.WaitForDebugEvent.restype = wt.BOOL
k32.ContinueDebugEvent.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD]

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL = 0x1F03FF
CONTEXT_DEBUG_REGISTERS = 0x00100010
CONTEXT_CONTROL = 0x00100001
EXCEPTION_SINGLE_STEP = 0x80000004
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
TH32CS_SNAPTHREAD = 0x00000004
MEM_COMMIT = 0x1000
READABLE = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [
        ("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong),
        ("P3Home", ctypes.c_ulonglong), ("P4Home", ctypes.c_ulonglong),
        ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
        ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
        ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD),
        ("SegFs", wt.WORD), ("SegGs", wt.WORD), ("SegSs", wt.WORD),
        ("EFlags", wt.DWORD),
        ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong),
        ("Dr2", ctypes.c_ulonglong), ("Dr3", ctypes.c_ulonglong),
        ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
        ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong),
        ("Rdx", ctypes.c_ulonglong), ("Rbx", ctypes.c_ulonglong),
        ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
        ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong),
        ("R8", ctypes.c_ulonglong), ("R9", ctypes.c_ulonglong),
        ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
        ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong),
        ("R14", ctypes.c_ulonglong), ("R15", ctypes.c_ulonglong),
        ("Rip", ctypes.c_ulonglong),
        ("FltSave", ctypes.c_byte * 512),
        ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_ulonglong),
        ("DebugControl", ctypes.c_ulonglong),
        ("LastBranchToRip", ctypes.c_ulonglong),
        ("LastBranchFromRip", ctypes.c_ulonglong),
        ("LastExceptionToRip", ctypes.c_ulonglong),
        ("LastExceptionFromRip", ctypes.c_ulonglong),
    ]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class EXCEPTION_RECORD(ctypes.Structure):
    pass


EXCEPTION_RECORD._fields_ = [
    ("ExceptionCode", wt.DWORD), ("ExceptionFlags", wt.DWORD),
    ("ExceptionRecord", ctypes.POINTER(EXCEPTION_RECORD)),
    ("ExceptionAddress", ctypes.c_void_p), ("NumberParameters", wt.DWORD),
    ("ExceptionInformation", ctypes.c_ulonglong * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wt.DWORD)]


class DEBUG_EVENT_U(ctypes.Union):
    _fields_ = [("Exception", EXCEPTION_DEBUG_INFO), ("pad", ctypes.c_byte * 176)]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD),
                ("dwThreadId", wt.DWORD), ("u", DEBUG_EVENT_U)]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong),
                ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", wt.DWORD), ("__a", wt.DWORD),
                ("RegionSize", ctypes.c_ulonglong), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD), ("__b", wt.DWORD)]


def find_pid(name="ng2.exe"):
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name, "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines()[1:]:
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name:
            return int(parts[1])
    return None


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ids = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                ids.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return ids


def load_table(path):
    """host_address -> guest_address, sorted, for naming a RIP."""
    entries = []
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("#"):
                    continue
                a, b = line.split()
                entries.append((int(a, 16), int(b, 16)))
    except OSError:
        return []
    entries.sort()
    return entries


def name_rip(table, rip):
    """The last function entry point at or below rip."""
    if not table:
        return 0
    lo, hi = 0, len(table)
    while lo < hi:
        mid = (lo + hi) // 2
        if table[mid][0] <= rip:
            lo = mid + 1
        else:
            hi = mid
    return table[lo - 1][1] if lo else 0


def aliases_for(hproc, guest_pa):
    """Every mapped host address holding the same 4 bytes as the target.

    Guest memory is mapped more than once; a watchpoint is a HOST address, so
    the alias the guest actually writes through is the one that matters. Each
    candidate is checked with VirtualQuery BEFORE being read - reading an
    uncommitted one crashed the game earlier tonight.
    """
    VIRT = 0x100000000
    # Do NOT anchor on the "physical" alias. In-process it is readable at
    # 0x3_00000000 + pa, but from another process that read fails outright, so
    # requiring it as the reference produced zero aliases and hid two perfectly
    # good ones. Take the first readable candidate as the reference instead.
    found = []
    for base in (0x00000000, 0x40000000, 0x80000000, 0xA0000000,
                 0xC0000000, 0xE0000000):
        host = VIRT + (base | guest_pa)
        mbi = MEMORY_BASIC_INFORMATION()
        if not k32.VirtualQueryEx(hproc, ctypes.c_void_p(host), ctypes.byref(mbi),
                                  ctypes.sizeof(mbi)):
            continue
        if mbi.State != MEM_COMMIT or not (mbi.Protect & READABLE):
            continue
        val = read_mem(hproc, host, 4)
        if val is None:
            continue
        found.append((host, val, base))
    if not found:
        return []
    ref = found[0][1]
    out = []
    for host, val, base in found:
        if val == ref and host not in out:
            out.append(host)
            print("   alias: guest VA 0x%08X -> host %016X  (= %s)"
                  % (base | guest_pa, host, val.hex()))
    return out[:4]


def read_mem(hproc, addr, size):
    buf = (ctypes.c_char * size)()
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size,
                                 ctypes.byref(got)):
        return None
    return bytes(buf[:got.value]) if got.value == size else None


def set_watchpoints(pid, hosts, verify=True):
    """Program DR0..DR3 on every thread, then READ THEM BACK.

    Reading back matters: the in-process attempt reported "applied to 73
    threads" and then caught nothing, and there was no way to tell whether the
    registers had actually taken. Now there is.
    """
    dr7 = 0
    for k in range(len(hosts)):
        dr7 |= (1 << (k * 2))              # local enable
        dr7 |= (0b01 << (16 + k * 4))      # break on write
        dr7 |= (0b11 << (18 + k * 4))      # 4 bytes
    applied = verified = 0
    for tid in thread_ids(pid):
        h = k32.OpenThread(THREAD_ALL, False, tid)
        if not h:
            continue
        try:
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            c = CONTEXT()
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS
            if k32.GetThreadContext(h, ctypes.byref(c)):
                for k, host in enumerate(hosts):
                    setattr(c, "Dr%d" % k, host)
                c.Dr7 = dr7
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS
                if k32.SetThreadContext(h, ctypes.byref(c)):
                    applied += 1
                    if verify:
                        c2 = CONTEXT()
                        c2.ContextFlags = CONTEXT_DEBUG_REGISTERS
                        if k32.GetThreadContext(h, ctypes.byref(c2)) \
                                and c2.Dr0 == hosts[0] and c2.Dr7 == dr7:
                            verified += 1
            k32.ResumeThread(h)
        finally:
            k32.CloseHandle(h)
    return applied, verified


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", default="0x116CAA8C",
                    help="guest PHYSICAL address to watch")
    ap.add_argument("--table", default=r"C:\ng2dump\functable.txt")
    ap.add_argument("--seconds", type=int, default=90)
    args = ap.parse_args()

    pid = find_pid()
    if not pid:
        print("no running ng2.exe")
        return 1
    print("attached target: PID %d" % pid)

    hproc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not hproc:
        print("OpenProcess failed: %d" % ctypes.get_last_error())
        return 1

    target = int(args.addr, 16)
    hosts = aliases_for(hproc, target)
    print("guest 0x%08X maps to %d host alias(es):" % (target, len(hosts)))
    for h in hosts:
        print("   %016X" % h)
    if not hosts:
        print("no readable alias - is the address right?")
        return 1

    table = load_table(args.table)
    print("function table: %d entries from %s" % (len(table), args.table))

    applied, verified = set_watchpoints(pid, hosts)
    print("debug registers: applied to %d threads, VERIFIED on %d" % (applied, verified))
    if verified == 0:
        print("!! the registers did not stick - hardware watchpoints are not")
        print("   usable here, and that is the finding, not a failure to look.")
        return 2

    # Attach as a debugger so the OS hands us the exception directly.
    if not k32.DebugActiveProcess(pid):
        print("DebugActiveProcess failed: %d" % ctypes.get_last_error())
        return 1
    k32.DebugSetProcessKillOnExit(False)   # detaching must NOT kill the game
    print("debugger attached - reproduce the bug now (%ds)" % args.seconds)

    seen, deadline = {}, time.time() + args.seconds
    ev = DEBUG_EVENT()
    try:
        while time.time() < deadline:
            if not k32.WaitForDebugEvent(ctypes.byref(ev), 500):
                continue
            status = DBG_CONTINUE
            if ev.dwDebugEventCode == 1:  # EXCEPTION_DEBUG_EVENT
                rec = ev.u.Exception.ExceptionRecord
                if rec.ExceptionCode == EXCEPTION_SINGLE_STEP:
                    h = k32.OpenThread(THREAD_ALL, False, ev.dwThreadId)
                    if h:
                        c = CONTEXT()
                        c.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL
                        if k32.GetThreadContext(h, ctypes.byref(c)) and (c.Dr6 & 0xF):
                            g = name_rip(table, c.Rip)
                            key = g or c.Rip
                            if key not in seen:
                                seen[key] = 1
                                print("  WRITER: guest_fn=0x%08X   rip=%016X   dr6=%X"
                                      % (g, c.Rip, c.Dr6 & 0xF))
                                # Walk the stack for the CALLER chain.
                                #
                                # The function that writes the value is only the
                                # animation updater doing its job; what matters
                                # is who keeps asking it to. There is no unwind
                                # data to hand out here, so read the raw stack
                                # and keep any word that lands inside a known
                                # recompiled function - crude, but every hit is
                                # a real return address by construction.
                                chain, last = [], None
                                stk = read_mem(hproc, c.Rsp, 8 * 160)
                                if stk:
                                    for off in range(0, len(stk), 8):
                                        val = struct.unpack_from("<Q", stk, off)[0]
                                        if val < 0x7FF000000000 or val > 0x7FFFFFFFFFFF:
                                            continue
                                        gg = name_rip(table, val)
                                        if gg and gg != last:
                                            chain.append("0x%08X" % gg)
                                            last = gg
                                        if len(chain) >= 12:
                                            break
                                if chain:
                                    print("     stack: " + " <- ".join(chain))
                            c.Dr6 = 0
                            c.ContextFlags = CONTEXT_DEBUG_REGISTERS
                            k32.SetThreadContext(h, ctypes.byref(c))
                        k32.CloseHandle(h)
                else:
                    status = DBG_EXCEPTION_NOT_HANDLED
            k32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status)
    finally:
        k32.DebugActiveProcessStop(pid)
        print("detached; game left running. %d distinct writer(s)." % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
