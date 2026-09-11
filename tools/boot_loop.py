"""Drive the run -> crash -> register -> rebuild loop automatically.

Every iteration launches ng2, waits for it to die or for the timeout, reads the
log, and if it died on an unregistered guest function, registers that function
and rebuilds. Stops when the game stops dying that way - either it survives the
timeout, or it fails for some other reason worth a human look.

    python tools/boot_loop.py --iterations 25 --timeout 30

Each iteration is an incremental build (codegen re-partitions only what
changed), so a pass costs well under a minute.
"""

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

FATAL_RE = re.compile(
    r"Call to invalid or unregistered function at guest address 0x([0-9A-Fa-f]{8})")
# Other terminal states worth stopping on rather than looping blindly.
OTHER_FATAL_RE = re.compile(r"\[critical\].*?\[FATAL\]\s*(.+)")


def run_build():
    r = subprocess.run(["cmd", "/c", os.path.join(HERE, "build.cmd"), "Release"],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        tail = "\n".join((r.stdout + r.stderr).splitlines()[-25:])
        print("BUILD FAILED:\n" + tail)
    return r.returncode == 0


def run_game(timeout, log_path):
    exe = os.path.join(ROOT, "out", "build", "win-amd64-Release", "ng2.exe")
    if os.path.exists(log_path):
        os.remove(log_path)
    p = subprocess.Popen([exe,
                          "--game_data_root", os.path.join(ROOT, "game"),
                          "--log_file", log_path,
                          "--log_level", "debug"])
    try:
        code = p.wait(timeout=timeout)
        survived = False
    except subprocess.TimeoutExpired:
        p.kill()          # by handle, never by window title
        p.wait()
        code, survived = None, True
    # The logger flushes on exit; give the file a moment to land.
    for _ in range(20):
        if os.path.exists(log_path):
            break
        time.sleep(0.1)
    log = open(log_path, encoding="utf-8", errors="replace").read() \
        if os.path.exists(log_path) else ""
    return code, survived, log


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iterations", type=int, default=20)
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="seconds to let the game run before calling it a survival")
    args = ap.parse_args()

    log_path = os.path.join(ROOT, "out", "boot_loop.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    registered = []

    for i in range(1, args.iterations + 1):
        print(f"\n=== iteration {i}/{args.iterations} ===", flush=True)
        if not run_build():
            return 1

        code, survived, log = run_game(args.timeout, log_path)
        lines = log.count("\n")

        if survived:
            print(f"SURVIVED {args.timeout:.0f}s ({lines} log lines) - "
                  f"no unregistered-function crash")
            break

        m = FATAL_RE.search(log)
        if not m:
            other = OTHER_FATAL_RE.search(log)
            print(f"exited {code} after {lines} log lines; "
                  f"not an unregistered-function crash")
            if other:
                print(f"  last fatal: {other.group(1).strip()}")
            else:
                print("  last lines:")
                for line in log.splitlines()[-6:]:
                    print("   ", line)
            break

        addr = m.group(1).upper()
        print(f"unregistered function 0x{addr} (after {lines} log lines)")
        r = subprocess.run([sys.executable, os.path.join(HERE, "add_function.py"),
                            "0x" + addr], cwd=ROOT, capture_output=True, text=True)
        print("  " + r.stdout.strip())
        if r.returncode != 0:
            print("  add_function made no progress - stopping to avoid a loop")
            break
        registered.append(addr)

    print(f"\nregistered {len(registered)} function(s) this run:")
    for a in registered:
        print(f"  0x{a}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
