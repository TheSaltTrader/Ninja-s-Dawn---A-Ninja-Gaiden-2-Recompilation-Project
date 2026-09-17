#!/usr/bin/env python3
"""Lowest constant register a Xenos program reads, by decoding its microcode.

WHY THIS EXISTS. An uncontained shader has no definition table, so the size of
its literal block is not recorded anywhere - and the block is what holds the
shader's own constants. Get it wrong and the program computes with the wrong
numbers, which RENDERS rather than fails, the hardest defect in this project to
see. The rule validated over 625 containers is

    physicalOffset = (TOP - firstLiteralRegister) * 16

so the block size follows from the lowest constant register the program reads -
if that register can be recovered from the microcode alone. That is what this
does, and it is checkable: run it against containers, where the truth IS
recorded, and score it.

A first attempt scanned raw bytes for values in 248..255 sitting in operand
positions. It returned 128 for all nine dumped shaders including a 96-byte one -
a scan saturating at its own ceiling, not a measurement. Xenos microcode has to
be walked properly: the control-flow section says which 3-dword groups are ALU
instructions and which are fetches, and only an ALU instruction has constant
operands.

Relative, not absolute: the 8-bit operand field indexes the shader's own half of
the 512-entry constant file, so both stages give 0..255 here and the caller adds
256 for a pixel shader if it wants the absolute register.
"""
import struct

# ControlFlowOpcode values that carry an instruction block.
EXEC_OPCODES = frozenset((1, 2, 3, 4, 5, 6, 13, 14))


def _cf_pair(d0, d1, d2):
    """Xenos packs two 48-bit control-flow instructions into three dwords."""
    a = d0 | ((d1 & 0xFFFF) << 32)
    b = (d1 >> 16) | (d2 << 16)
    return a, b


def _cf_fields(cf):
    """(opcode, address, count, sequence) of a 48-bit control-flow word."""
    opcode = (cf >> 44) & 0xF
    address = cf & 0xFFF
    count = (cf >> 12) & 0x7
    sequence = (cf >> 16) & 0xFFFFFF
    return opcode, address, count, sequence


def constant_registers_read(microcode):
    """Every constant register an ALU instruction reads, 0..255 relative.

    Relative, not absolute: the 8-bit operand field indexes the shader's own
    half of the 512-entry constant file, so both stages give 0..255 here.
    """
    if len(microcode) < 12:
        return set()
    n = len(microcode) // 4
    dwords = struct.unpack(">%dI" % n, microcode[:n * 4])

    alu = set()
    limit = n
    i = 0
    # The control-flow section runs from the top until the first instruction an
    # exec block points at - CF words and ALU words share one array, so the
    # lowest exec target IS the end of the CF section.
    while i + 3 <= n and i < limit:
        for cf in _cf_pair(dwords[i], dwords[i + 1], dwords[i + 2]):
            opcode, address, count, sequence = _cf_fields(cf)
            if opcode not in EXEC_OPCODES:
                continue
            if address:
                limit = min(limit, address * 3)
            seq = sequence
            for k in range(count):
                if not (seq & 0x1):        # bit 0 of each pair: fetch, not ALU
                    alu.add(address + k)
                seq >>= 2
        i += 3

    out = set()
    for instr in alu:
        at = instr * 3
        if at + 3 > n:
            continue
        w2 = dwords[at + 2]
        # word 2, LSB first: src3_reg:8 src2_reg:8 src1_reg:8 vector_opc:5
        #                    src3_sel:1 src2_sel:1 src1_sel:1
        # sel == 1 is a temp register; sel == 0 is a CONSTANT.
        for reg_shift, sel_bit in ((16, 31), (8, 30), (0, 29)):
            if not ((w2 >> sel_bit) & 1):
                out.add((w2 >> reg_shift) & 0xFF)
    return out


def literal_bytes(microcode):
    """Size of the shader's literal block, in bytes. Exact on all 625 NG2
    containers; never under-predicts, which is the only unsafe direction.

    Two parts, with very different evidential weight:

    1. c255 IS READ if and only if the shader has a literal block. Perfectly
       separated over 625 containers - 541 of 541 with a block read it, 0 of 84
       without one did. Strong.

    2. The block covers the CONTIGUOUS run of constant registers ending at c255,
       rounded up to four float4s. Weak: NG2's blocks are 0 or 64 bytes with just
       TWO exceptions at 128, so a trivial "0 or 64" rule already scores 623/625
       and only those two containers separate the two rules. This one gets both,
       but two points is thin evidence and is recorded as such.

    The bound is a LOWER one either way - a shader reads a subset of its
    literals, never a superset (measured: the contiguous run equals the declared
    first register in only 21 of 541). Rounding up is therefore deliberate, and
    over-declaring is harmless because the block is anchored at the TOP of the
    constant file: the extra registers land on memory the program never reads,
    while the real literals keep their correct indices.
    """
    regs = constant_registers_read(microcode)
    if 255 not in regs:
        return 0
    lowest = 255
    while lowest - 1 in regs:
        lowest -= 1
    return ((256 - lowest + 3) // 4) * 4 * 16
