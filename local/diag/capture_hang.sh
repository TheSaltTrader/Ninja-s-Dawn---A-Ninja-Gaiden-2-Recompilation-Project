#!/bin/bash
# Capture evidence from a hung ng2.exe WITHOUT touching it.
#
# -pv is a non-invasive attach: the debugger can read the target but cannot
# resume, kill, or otherwise disturb it. That matters here for two reasons -
# the session being captured is the one being tested, and killing this user's
# game session by accident is a mistake already made once in this project.
#
# Usage: capture_hang.sh <pid>
#
# What it collects, in order of usefulness:
#   1. every thread's stack, so the spinning thread is identifiable by name
#   2. the guest flag the main fibre waits on, read through the guest base
#   3. thread CPU times, which separate a spinning thread from a blocked one
set -u
PID="${1:-}"
if [ -z "$PID" ]; then echo "usage: capture_hang.sh <pid>" >&2; exit 2; fi

CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
OUT="/c/Users/renoi/AppData/Local/Temp/claude/C--users-renoi-claudecode/7c96f2bf-8164-49f3-b033-c53410743ffd/scratchpad/hang_$(date +%H%M%S).txt"

# Guest virtual base is 0x1_00000000, so guest 0x84C39440 is host 0x184C39440.
# Values are big-endian in guest memory; dd reverses nothing, so read the dword
# and byte-swap by eye - or use .formats on the swapped value.
FLAG_HOST="0x184C39440"

"$CDB" -pv -p "$PID" -c "
.printf \"=== all thread stacks ===\\n\";
~*k 40;
.printf \"\\n=== guest flag 0x84C39440 (host $FLAG_HOST), 8 dwords ===\\n\";
dd $FLAG_HOST L8;
.printf \"\\n=== thread times (spinning vs blocked) ===\\n\";
!runaway 7;
.printf \"\\n=== locks ===\\n\";
!locks;
qd
" > "$OUT" 2>&1

echo "captured to: $OUT"
echo "--- threads burning CPU ---"
sed -n '/thread times/,/locks/p' "$OUT" | head -25
