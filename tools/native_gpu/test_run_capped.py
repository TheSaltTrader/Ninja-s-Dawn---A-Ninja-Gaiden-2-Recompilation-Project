"""Controls for run_capped.py, INCLUDING the ones where the cap itself fails.

Both sessions validated the first version of the cap with four controls -
trivial exit, a chosen exit code, a sleep past the deadline, an allocation past
the cap - and all four passed while the cap had a bug that let two translators
reach 15 GB and 66 GB. They passed because every one of them exercised the
HAPPY PATH: each assumed the job object had been established, because when it
is established the cap works. "It caps correctly when capping works" is not a
test of a cap.

So half of this file injects failures into the mechanism itself. A safety
device needs a control for the case where the safety device fails, and that is
precisely the case neither of us covered.

    python test_run_capped.py
"""
import ctypes
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_capped as rc   # noqa: E402

PY = sys.executable
fails = []


def check(name, got, want):
    ok = got == want
    print("  %-52s %s (got %s, want %s)" % (name, "ok" if ok else "FAILED", got, want))
    if not ok:
        fails.append(name)


def run(secs, mb, code):
    return rc.main([str(secs), str(mb), "--", PY, "-c", code])


print("happy path - the mechanism works and the outcomes are distinguished")
check("exits 0", run(20, 512, "raise SystemExit(0)"), 0)
check("carries the child's own exit code", run(20, 512, "raise SystemExit(3)"), 3)
check("time cap", run(2, 512, "import time; time.sleep(30)"), rc.TIMED_OUT)
check("memory cap at 512 MB", run(60, 512, "x=bytearray(1024*1024*1024)"), rc.MEMORY_CAPPED)
check("memory cap at 4096 MB", run(90, 4096, "x=bytearray(8*1024*1024*1024)"),
      rc.MEMORY_CAPPED)
check("2 GB under a 4096 MB cap does NOT trip", run(90, 4096, "x=bytearray(2*1024*1024*1024)"), 0)

print()
print("THE MECHANISM ITSELF FAILS - the class of control that was missing")

# What must happen: refuse, with NO_CAP, and never let the child run. The
# strong assertion is the second one, so each case also proves the child
# produced no side effect - it writes a file if it ever executes.
probe = os.path.join(os.environ.get("TEMP", "."), "run_capped_probe.txt")
WRITES = "open(r'%s','w').write('the child ran')" % probe


def with_broken(attr, replacement, name):
    if os.path.exists(probe):
        os.remove(probe)
    saved = getattr(rc.k32, attr)
    setattr(rc.k32, attr, replacement)
    try:
        got = rc.main(["20", "512", "--", PY, "-c", WRITES])
    finally:
        setattr(rc.k32, attr, saved)
    check(name + " -> refuses", got, rc.NO_CAP)
    check(name + " -> the child never ran", os.path.exists(probe), False)


with_broken("CreateJobObjectW", lambda *a: None, "CreateJobObject fails")
with_broken("SetInformationJobObject", lambda *a: 0, "SetInformationJobObject fails")
with_broken("AssignProcessToJobObject", lambda *a: 0, "AssignProcessToJobObject fails")
with_broken("CreateIoCompletionPort", lambda *a: None, "CreateIoCompletionPort fails")

print()
print("the OS message id, which a happy-path control cannot check")
# 6 is MSG_NEW_PROCESS and fires for EVERY child; with 6 here the wrapper
# reported a memory breach for a command that exits immediately. The only way
# to catch that is to run something which cannot possibly breach the cap and
# assert it is NOT reported as capped - which the "exits 0" control above does
# only because this constant is right.
check("MSG_PROCESS_MEMORY_LIMIT is 9, not 6", rc.JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT, 9)

print()
print("ctypes restypes - an undeclared HANDLE truncates to 32 bits and the")
print("failure looks like a timeout, not like a broken declaration")
for fn in ("CreateJobObjectW", "OpenProcess", "OpenThread",
           "CreateToolhelp32Snapshot", "CreateIoCompletionPort"):
    check("%s.restype declared" % fn, getattr(rc.k32, fn).restype, ctypes.wintypes.HANDLE)

print()
print("struct layouts - a wrong one moves ProcessMemoryLimit somewhere harmless")
check("JOBOBJECT_BASIC_LIMIT_INFORMATION", ctypes.sizeof(rc.JOBOBJECT_BASIC_LIMIT_INFORMATION), 64)
check("JOBOBJECT_EXTENDED_LIMIT_INFORMATION",
      ctypes.sizeof(rc.JOBOBJECT_EXTENDED_LIMIT_INFORMATION), 144)

print()
if fails:
    print("%d CONTROL(S) FAILED: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
print("all controls passed, including the four where the cap itself fails")
