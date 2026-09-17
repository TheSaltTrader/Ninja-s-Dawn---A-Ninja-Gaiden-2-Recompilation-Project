"""Run a command under BOTH a time cap and a memory cap.

`timeout` bounds how long a process runs, not how much it takes with it, and
raising the time cap makes that worse. XenosRecomp on container 42E9FAB0_p
reached a 55 GB working set and took this machine from 93.6 GB to 0.5 GB free;
the other session lost two background watchers to the OS low-memory killer
while it ran, and every timing either of us recorded in that window is
worthless. At the old 15 s deadline it was reaped before it did real damage, so
fixing the deadline alone bought the runaway 105 more seconds to eat RAM.

A Windows job object makes the OS refuse the allocation instead: the child
dies, nothing else on the machine notices, and - the part `timeout` can never
give us - we learn WHICH containers do it, as a reproducible property of the
container rather than as whatever the machine happened to be doing.

    run_capped.py <seconds> <megabytes> -- <command> [args...]

Exit codes: 0 ok, 124 time cap, 125 memory cap, 126 crashed (an NTSTATUS-shaped
exit), otherwise the child's own.
"""
import ctypes
import ctypes.wintypes as w
import subprocess
import sys
import threading
import time

TIMED_OUT = 124
MEMORY_CAPPED = 125
# sys.exit truncates to 8 bits, so a Windows crash code (0xC0000005 and kin)
# comes out of this wrapper as an ordinary small number and reads as the child
# refusing its input. 0xC0000005 & 0xFF is 5. Carry the crash out as its own
# code instead - under plain `timeout` MSYS reported 139 for the same thing and
# that distinction must not be lost by moving to a job object.
CRASHED = 126
# Never run the command at all rather than run it with no limit.
NO_CAP = 127


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_longlong),
        ("PerJobUserTimeLimit", ctypes.c_longlong),
        ("LimitFlags", w.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", w.DWORD),
        ("Affinity", ctypes.POINTER(ctypes.c_ulong)),
        ("PriorityClass", w.DWORD),
        ("SchedulingClass", w.DWORD),
    ]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in
                ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                 "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
JobObjectExtendedLimitInformation = 9
JobObjectAssociateCompletionPortInformation = 7
# The OS says so itself. PeakProcessMemoryUsed is NOT the test: the allocation
# that breaches the cap is refused, so the peak stays BELOW it - measured, 1523
# MB of a 2048 MB cap on the container that wanted 55 GB. A peak-based guess
# would have called that a plain crash.
# 9, not 6. 6 is JOB_OBJECT_MSG_NEW_PROCESS, which the OS posts for every
# child - so with 6 here a `cmd /c exit 0` reported a memory breach. A
# smoke test on a command that cannot possibly breach the cap is what
# caught it; without one this would have marked the whole corpus capped.
JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT = 9


class JOBOBJECT_ASSOCIATE_COMPLETION_PORT(ctypes.Structure):
    _fields_ = [("CompletionKey", ctypes.c_void_p), ("CompletionPort", w.HANDLE)]

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
# EVERY handle-returning call needs its restype declared. ctypes defaults to
# c_int, which TRUNCATES a 64-bit HANDLE - and because Windows usually hands
# out small handle values it truncates harmlessly most of the time and then
# does not. Undeclared OpenThread was the intermittent one here: the resume
# silently failed, the child stayed suspended, and the run came back as a
# TIMEOUT - the exact misreading this whole file exists to stop.
for _fn in ("CreateJobObjectW", "OpenProcess", "OpenThread",
            "CreateToolhelp32Snapshot", "CreateIoCompletionPort"):
    getattr(k32, _fn).restype = w.HANDLE
k32.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
k32.ResumeThread.argtypes = [w.HANDLE]
k32.CloseHandle.argtypes = [w.HANDLE]
k32.TerminateProcess.argtypes = [w.HANDLE, ctypes.c_uint]
psapi = ctypes.WinDLL("psapi", use_last_error=True)
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD]


def main(argv):
    if "--" not in argv:
        sys.stderr.write(__doc__)
        return 2
    cut = argv.index("--")
    secs = float(argv[0])
    cap = int(argv[1]) * 1024 * 1024
    cmd = argv[cut + 1:]

    # EVERY STEP OF THE CAP IS CHECKED, and a child we could not cap is never
    # resumed. The first version checked none of them: if any call failed it
    # resumed the process anyway and ran it with no limit at all, which is the
    # one outcome this file exists to prevent - and during a 1,740-container
    # census something did let processes through to 15 and 66 GB while the same
    # cap held perfectly in isolation. A cap that can fail silently is not a cap.
    job = k32.CreateJobObjectW(None, None)
    port = None
    why = None
    if not job:
        why = "CreateJobObject failed (%d)" % ctypes.get_last_error()
    else:
        jl = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        jl.BasicLimitInformation.LimitFlags = (
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
        jl.ProcessMemoryLimit = cap
        if not k32.SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                           ctypes.byref(jl), ctypes.sizeof(jl)):
            why = "SetInformationJobObject(limits) failed (%d)" % ctypes.get_last_error()
        else:
            port = k32.CreateIoCompletionPort(w.HANDLE(-1), None, 0, 1)
            if not port:
                why = "CreateIoCompletionPort failed (%d)" % ctypes.get_last_error()
            else:
                ap = JOBOBJECT_ASSOCIATE_COMPLETION_PORT()
                ap.CompletionKey = ctypes.c_void_p(1)
                ap.CompletionPort = port
                if not k32.SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation,
                                                   ctypes.byref(ap), ctypes.sizeof(ap)):
                    why = "SetInformationJobObject(port) failed (%d)" % ctypes.get_last_error()
    if why:
        sys.stderr.write("*** REFUSING TO RUN UNCAPPED: %s\n" % why)
        if port:
            k32.CloseHandle(port)
        if job:
            k32.CloseHandle(job)
        return NO_CAP

    # Suspended, so the child is inside the capped job before it runs a single
    # instruction; anything it spawns inherits the job and the cap with it.
    CREATE_SUSPENDED = 0x00000004
    p = subprocess.Popen(cmd, creationflags=CREATE_SUSPENDED)
    h = k32.OpenProcess(0x1F0FFF, False, p.pid)   # PROCESS_ALL_ACCESS
    assigned = bool(h) and bool(k32.AssignProcessToJobObject(job, h))
    err = ctypes.get_last_error()
    if h:
        k32.CloseHandle(h)
    if not assigned:
        # Still suspended, so it has run nothing. Kill it rather than resume it.
        sys.stderr.write("*** REFUSING TO RUN UNCAPPED: could not put the child in "
                         "the capped job (%d) - killed it instead\n" % err)
        p.kill()
        k32.CloseHandle(port)
        k32.CloseHandle(job)
        return NO_CAP
    # Popen gives no thread handle, so resume every thread of the new process.
    th = k32.CreateToolhelp32Snapshot(0x00000004, p.pid)   # TH32CS_SNAPTHREAD

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                    ("th32OwnerProcessID", w.DWORD), ("tpBasePri", ctypes.c_long),
                    ("tpDeltaPri", ctypes.c_long), ("dwFlags", w.DWORD)]

    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(th, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == p.pid:
            ht = k32.OpenThread(0x0002, False, te.th32ThreadID)   # THREAD_SUSPEND_RESUME
            if ht:
                k32.ResumeThread(ht)
                k32.CloseHandle(ht)
        ok = k32.Thread32Next(th, ctypes.byref(te))
    k32.CloseHandle(th)

    # An INDEPENDENT watchdog, because the job object is one mechanism and one
    # mechanism that can fail silently is what let a translator reach 66 GB.
    # This samples what the child has actually committed and kills it itself.
    # If it ever fires, the job object did not do its job - which is a fact
    # worth knowing rather than a redundancy.
    watchdog_fired = [False]

    def watch():
        pmc = PROCESS_MEMORY_COUNTERS_EX()
        pmc.cb = ctypes.sizeof(pmc)
        hp = k32.OpenProcess(0x1000 | 0x0400 | 0x0001, False, p.pid)  # QUERY_LIMITED|QUERY|TERMINATE
        if not hp:
            return
        try:
            while p.poll() is None:
                if psapi.GetProcessMemoryInfo(hp, ctypes.byref(pmc), ctypes.sizeof(pmc)):
                    if pmc.PrivateUsage > cap:
                        watchdog_fired[0] = True
                        sys.stderr.write("*** WATCHDOG killed %s at %d MB committed - the JOB "
                                         "OBJECT DID NOT HOLD, which is itself a defect\n"
                                         % (cmd[0], pmc.PrivateUsage // (1024 * 1024)))
                        k32.TerminateProcess(hp, 1)
                        return
                time.sleep(0.25)
        finally:
            k32.CloseHandle(hp)

    wd = threading.Thread(target=watch, daemon=True)
    wd.start()

    timed_out = False
    try:
        rc = p.wait(timeout=secs)
    except subprocess.TimeoutExpired:
        timed_out = True
        p.kill()
        p.wait()      # do not leave it half-dead while the next one starts
        rc = TIMED_OUT
    wd.join(timeout=2)

    capped = watchdog_fired[0]
    if job:
        if port:
            # Drain the job's messages: the OS posted one if it refused an
            # allocation past the cap.
            msg, key, ovl = w.DWORD(), ctypes.c_void_p(), ctypes.c_void_p()
            while k32.GetQueuedCompletionStatus(port, ctypes.byref(msg), ctypes.byref(key),
                                                ctypes.byref(ovl), 0):
                if msg.value == JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT:
                    capped = True
            k32.CloseHandle(port)
        k32.CloseHandle(job)

    # The cap is tested BEFORE the crash code: a process killed for breaching
    # the cap dies with 0xC0000409 (its allocator fails and calls __fastfail),
    # so testing the crash first reports every capped run as a plain crash and
    # hides the one fact worth having. Measured: 42E9FAB0_p came back CRASHED
    # while the cap was working perfectly.
    if capped:
        sys.stderr.write("*** %s allocated past the %d MB cap and was killed - a "
                         "REPRODUCIBLE property of this input, not machine load\n"
                         % (cmd[0], cap // (1024 * 1024)))
        return MEMORY_CAPPED
    if not timed_out and rc >= 0xC0000000:
        sys.stderr.write("*** %s CRASHED (0x%08X) on this input - not a refusal\n" % (cmd[0], rc))
        return CRASHED
    return TIMED_OUT if timed_out else rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
