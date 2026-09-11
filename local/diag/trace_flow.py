"""Trace the stage-end / transition state words of a live game.

    python local/diag/trace_flow.py <pid> [--hz 30] [--seconds 0] [--out FILE]

Polls the words that drive the post-boss "advance to next chapter" flow and
prints a line ONLY when one of them changes, with wall-clock time and the guest
frame counter. Reads memory only (ReadProcessMemory) - no thread suspension, so
it is safe to leave running through a whole playthrough. --seconds 0 = forever.

The point: catch the exact transition where the game leaves gameplay mode for
the stage-end flow, runs the save/continue prompts, and (in the bug) falls back
to gameplay mode 3 without ever emitting the sequencer advance request.
"""
import argparse
import ctypes
import struct
import sys
import time

GUEST_BASE = 0x1_0000_0000
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# label -> (guest VA, width)
FIELDS = [
    ("mode", 0x84C25070, 4),      # 0x84C25070: 3=in-game, 128=stage-end flow, 131/132=quit
    ("gametype", 0x84C25074, 4),  # story vs other; gates the state-5 advance branch
    ("req", 0x84C250AC, 4),       # sequencer transition request (0 = none)
    ("flowstate", 0x83B14A00, 4), # sub_82441450 state 0..10
    ("flowsave", 0x83B14A0C, 4),  # save flag the flow computes the request from
    ("substate", 0x84AED970, 4),  # sequencer state-4 substate
    ("seqstate", 0x842DC325, 1),  # sequencer state byte (4 = in-game)
    ("seqnext", 0x842DC326, 1),
    ("site0_560", 0x83CDF018, 4), # first site descriptor +560
]
FRAME_VA = 0x84C3BF40


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("--hz", type=float, default=30)
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    h = k32.OpenProcess(0x0410, False, args.pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        print("OpenProcess failed:", ctypes.get_last_error())
        return 1

    def rd(va, width):
        buf = (ctypes.c_ubyte * width)()
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(GUEST_BASE + va), buf, width, ctypes.byref(got)) or got.value != width:
            return None
        return int.from_bytes(bytes(buf), "big")

    out = open(args.out, "w", buffering=1) if args.out else None

    def emit(line):
        print(line)
        if out:
            out.write(line + "\n")

    emit("# tracing pid %d fields: %s" % (args.pid, ", ".join(f[0] for f in FIELDS)))
    last = {}
    period = 1.0 / args.hz
    t_end = time.time() + args.seconds if args.seconds else None
    try:
        while True:
            now = time.time()
            cur = {name: rd(va, w) for name, va, w in FIELDS}
            frame = rd(FRAME_VA, 4)
            changed = [k for k in cur if cur[k] != last.get(k, object())]
            if last and changed:
                stamp = time.strftime("%H:%M:%S") + (".%03d" % int((now % 1) * 1000))
                parts = []
                for k in changed:
                    ov = last.get(k)
                    nv = cur[k]
                    parts.append("%s %s->%s" % (
                        k,
                        "None" if ov is None else "0x%X" % ov,
                        "None" if nv is None else "0x%X" % nv))
                emit("[%s f%s] %s" % (stamp, frame, "  ".join(parts)))
            elif not last:
                stamp = time.strftime("%H:%M:%S")
                emit("[%s f%s] init: %s" % (stamp, frame, "  ".join(
                    "%s=%s" % (k, "None" if cur[k] is None else "0x%X" % cur[k]) for k in cur)))
            last = cur
            if t_end and now >= t_end:
                break
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()
        k32.CloseHandle(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
