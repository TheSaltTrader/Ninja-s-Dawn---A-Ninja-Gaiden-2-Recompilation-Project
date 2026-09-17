"""How far does a Xenos shader's own control flow say its program extends?

The recompiler walks control-flow instructions bounded by shader->size, then
decodes the ALU/fetch instructions each exec block points at. If a block is
shorter than the control flow describes, that second pass reads past the end -
which is what a segfault with no malformed packet to blame looks like.

This answers the length question from the shader's OWN declarations instead of
by trying padding or trimming and watching a success count move.
"""
import struct, sys, glob, os

EXEC_LIKE = {1, 2, 3, 4, 5, 6, 13, 14}   # Exec..CondExecPredCleanEnd
NAMES = {0: "Nop", 1: "Exec", 2: "ExecEnd", 3: "CondExec", 4: "CondExecEnd",
         5: "CondExecPred", 6: "CondExecPredEnd", 7: "LoopStart", 8: "LoopEnd",
         9: "CondCall", 10: "Return", 11: "CondJmp", 12: "Alloc",
         13: "CondExecPredClean", 14: "CondExecPredCleanEnd", 15: "MarkVsFetchDone"}


def cf_instructions(code):
    """Yield the 48-bit control-flow instructions, two per 12 bytes."""
    n = len(code) // 4
    dw = struct.unpack(">%dI" % n, code[: n * 4])
    for i in range(0, n - 2, 3):
        d0, d1, d2 = dw[i], dw[i + 1], dw[i + 2]
        yield (i // 3) * 12, d0 | ((d1 & 0xFFFF) << 32)
        yield (i // 3) * 12, ((d1 >> 16) | ((d2 & 0xFFFF) << 16)) | ((d2 >> 16) << 32)


def extent(code):
    """(cf_bytes, max_extent_bytes, opcodes seen) from the shader's own CF."""
    cf_limit = len(code)
    max_end = 0
    seen = []
    for at, v in cf_instructions(code):
        if at >= cf_limit:
            break
        op = (v >> 44) & 0xF
        seen.append(NAMES.get(op, str(op)))
        if op in EXEC_LIKE:
            addr = v & 0xFFF
            cnt = (v >> 12) & 0x7
            if addr:
                cf_limit = min(cf_limit, addr * 12)
            max_end = max(max_end, (addr + cnt) * 12)
        if op in (2, 4, 6, 14):        # an End terminates the control flow
            break
    return cf_limit, max_end, seen


if __name__ == "__main__":
    files = sys.argv[1:] or sorted(glob.glob("*.xvu"))
    for f in files:
        b = open(f, "rb").read()
        _, v, p = struct.unpack(">III", b[0:12])
        code = b[v:v + p]
        cf, end, seen = extent(code)
        flag = "OK   " if end <= len(code) else "SHORT"
        print("%s %-24s block %5d bytes, CF section %4d, program extends to %5d  (%+d)  [%s]"
              % (flag, os.path.basename(f), len(code), cf, end, len(code) - end,
                 " ".join(seen[:6])))
