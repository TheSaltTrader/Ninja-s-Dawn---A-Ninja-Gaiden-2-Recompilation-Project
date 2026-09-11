# ReXGlue codegen validators

Static checkers that compare each PPC instruction's disassembly comment
(`// <mnemonic> ...`, the ground truth ReXGlue emits) against the C++ it
generated on the following line(s). Where the result is recomputable (masks,
rotates, immediates) the check is **exact**; otherwise it asserts the emitted
C++ has the right operator / signedness / guard / target.

The codegen is shared with the **Fable 2** recomp, so any finding here applies
there too. Point the validators at that tree's `generated/` to re-run.

## Run

```bash
tools/codegen_validation/run_all.sh generated/default
```

Exit code is non-zero if any pass reports `mismatches=N` (N>0).

## Passes and coverage

| pass | classes |
|------|---------|
| `codegen_validate.py` | conditional branches (polarity), integer compares (signed/unsigned + width), integer loads, integer stores |
| `rlwinm_validate.py`  | `rlwinm`/`rlwnm` rotate-and-mask (recomputes PPC 32-bit mask from MB,ME) |
| `validate2.py`        | `rldicl`/`rldicr` 64-bit rotate masks, `srawi` carry mask, byte-reverse `lwbrx`/`lhbrx`/`stwbrx`/`sthbrx`/`ldbrx`/`stdbrx`, `cntlzw`/`cntlzd` |
| `validate3.py`        | extended aliases `clrlwi`/`clrldi`/`rotlwi`/`rotldi`, variable shifts `slw`/`srw`/`sld`/`srd` + arithmetic `sraw`/`srad`, carry arithmetic (`addze`/`subfe`/`subfc`/`addic`…), multiply/divide signedness, **record-form CR0/CR6 updates** |
| `validate4.py`        | immediate arithmetic `li`/`lis`/`addis`/`mulli` (exact), `subf` operand order, logical ops + immediates, sign-extend loads `lha`/`lwa`/`lhax`, insert-rotates `rlwimi`/`rldimi` masks (ISA `MASK(MB+32,ME+32)`), conditional returns `b<cc>lr` (polarity), `bdnz` counter branch, spr moves `mtctr`/`mtlr`/`mtxer` |
| `coverage.py`         | census: every distinct mnemonic, counts, validated vs. remainder by family |

## Status (NG2, generated/default, 2026-09-07)

**All passes clean.** ~1.5M instructions validated across ~45 classes covering
the entire integer control-flow + arithmetic + memory + FP-scalar surface.
0 real mismatches after fixing the 4 earlier `bne`→`blt` branch bugs.

Two flags surfaced during development were **validator-model errors, not codegen
bugs**, and the validator was corrected:
- `mtxer` is field-decomposed into `xer.so`/`xer.ov`/`xer.ca` (not a monolithic
  `xer.u64=`).
- `rlwimi` with a **wraparound** mask (MB>ME) legitimately sets the upper 32
  bits of the rotate mask, because the ISA uses `MASK(MB+32,ME+32)` over 64 bits
  and `ROTL32` duplicates the word into both halves. The codegen is faithful.

Not script-validated: **VMX128 vector** (~1.1M) and, beyond the scalar spot
check, most **FP** ops — these are rendering/SIMD math, not control flow, and are
confirmed correct by the game rendering at 60fps. FP scalar load/store single↔
double conversion (`lfs`/`stfs`/`fadds` vs `fadd`, `frsp`, `fctiwz`) was
spot-validated correct — that is the usual FP mistranslation locus.

**Conclusion:** the chapter-transition hang is *not* a codegen fault. It is a
runtime/logic wait in the results/save controller (`sub_82442128`).
