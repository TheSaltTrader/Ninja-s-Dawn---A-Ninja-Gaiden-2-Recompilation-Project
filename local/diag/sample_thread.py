"""Sample one thread's instruction pointer many times and name the guest
functions it is in - a poor man's profiler that needs no debugger.

    python local/diag/sample_thread.py <pid> <tid-hex> [--seconds 10] [--hz 50]

Each sample suspends the thread for a few microseconds (SuspendThread +
GetThreadContext + ResumeThread), reads RIP, and maps it to sub_XXXXXXXX via
the live function table (C:\\ng2dump\\functable.txt, written by
tools/dump_table_live.py; run watch_ch13.py --capture first if it is stale).
It also walks the stack's return addresses conservatively (scans the top of
the stack for values inside ng2.exe) so callers show up too. The output is a
histogram: which functions the thread spends its time in, and how the
distribution differs from a healthy run is what names a stuck state machine.
"""
import argparse
import bisect
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time
from collections import Counter

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

THREAD_ALL = 0x1F03FF
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
CONTEXT_AMD64 = 0x00100000
CONTEXT_CONTROL = CONTEXT_AMD64 | 0x1
CONTEXT_INTEGER = CONTEXT_AMD64 | 0x2


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [
        ("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong), ("P3Home", ctypes.c_ulonglong),
        ("P4Home", ctypes.c_ulonglong), ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
        ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
        ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
        ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
        ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong), ("Dr2", ctypes.c_ulonglong),
        ("Dr3", ctypes.c_ulonglong), ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
        ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong), ("Rdx", ctypes.c_ulonglong),
        ("Rbx", ctypes.c_ulonglong), ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
        ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong), ("R8", ctypes.c_ulonglong),
        ("R9", ctypes.c_ulonglong), ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
        ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong), ("R14", ctypes.c_ulonglong),
        ("R15", ctypes.c_ulonglong), ("Rip", ctypes.c_ulonglong),
        ("FltSave", ctypes.c_ubyte * 512),
        ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_ulonglong),
        ("DebugControl", ctypes.c_ulonglong), ("LastBranchToRip", ctypes.c_ulonglong),
        ("LastBranchFromRip", ctypes.c_ulonglong), ("LastExceptionToRip", ctypes.c_ulonglong),
        ("LastExceptionFromRip", ctypes.c_ulonglong),
    ]


k32.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wt.DWORD), ("EntryPoint", ctypes.c_void_p)]


def load_table(path=r"C:\ng2dump\functable.txt"):
    rows = []
    with open(path) as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            h, g = line.split()
            rows.append((int(h, 16), int(g, 16)))
    rows.sort()
    return rows


def name(table, addr):
    i = bisect.bisect_right(table, (addr, 0xFFFFFFFF)) - 1
    if i < 0:
        return None
    host, guest = table[i]
    off = addr - host
    if off > 0x20000:
        return None
    return "sub_%08X" % guest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("tid", help="thread id, hex")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--hz", type=float, default=50.0)
    ap.add_argument("--stack-words", type=int, default=48)
    args = ap.parse_args()
    tid = int(args.tid, 16)

    hp = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, args.pid)
    ht = k32.OpenThread(THREAD_ALL, False, tid)
    if not hp or not ht:
        print("open failed: process %s thread %s" % (bool(hp), bool(ht)))
        return 1
    mods = (ctypes.c_void_p * 8)()
    need = wt.DWORD()
    psapi.EnumProcessModules(hp, ctypes.byref(mods), ctypes.sizeof(mods), ctypes.byref(need))
    mi = MODULEINFO()
    psapi.GetModuleInformation(hp, mods[0], ctypes.byref(mi), ctypes.sizeof(mi))
    exe_lo, exe_hi = mi.lpBaseOfDll, mi.lpBaseOfDll + mi.SizeOfImage
    table = load_table()
    print("ng2.exe %016X-%016X, %d table entries" % (exe_lo, exe_hi, len(table)))

    top = Counter()
    anywhere = Counter()
    chains = Counter()
    n = 0
    period = 1.0 / args.hz
    end = time.time() + args.seconds
    ctx = CONTEXT()
    while time.time() < end:
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER
        if k32.SuspendThread(ht) == 0xFFFFFFFF:
            break
        ok = k32.GetThreadContext(ht, ctypes.byref(ctx))
        rip, rsp = ctx.Rip, ctx.Rsp
        # Read the top of the stack while suspended, so the words are consistent.
        buf = (ctypes.c_ulonglong * args.stack_words)()
        got = ctypes.c_size_t()
        k32.ReadProcessMemory(hp, ctypes.c_void_p(rsp), buf, ctypes.sizeof(buf), ctypes.byref(got))
        k32.ResumeThread(ht)
        if not ok:
            continue
        n += 1
        fn = name(table, rip) if exe_lo <= rip < exe_hi else ("host:%016X" % rip)
        top[fn or ("ng2+%X" % (rip - exe_lo))] += 1
        seen = [fn] if fn else []
        for i in range(got.value // 8):
            v = buf[i]
            if exe_lo <= v < exe_hi:
                f2 = name(table, v)
                if f2 and f2 not in seen:
                    seen.append(f2)
        for f2 in seen:
            anywhere[f2] += 1
        chains[" < ".join(seen[:6])] += 1
        time.sleep(period)

    print("%d samples" % n)
    print("--- top-of-stack (where the thread IS) ---")
    for f, c in top.most_common(12):
        print("  %5.1f%%  %s" % (100.0 * c / n, f))
    print("--- on the stack anywhere (callers included) ---")
    for f, c in anywhere.most_common(20):
        print("  %5.1f%%  %s" % (100.0 * c / n, f))
    print("--- most common chains (top < callers) ---")
    for ch, c in chains.most_common(8):
        print("  %5.1f%%  %s" % (100.0 * c / n, ch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
