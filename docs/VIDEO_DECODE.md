# Why every pre-rendered video was garbled

**FIXED in v0.5.0.** The cause was in the **recompiler**, not the game, the video
files, the GPU plugin, or the codec.

`BuilderContext::v()` in the SDK's codegen turned VMX128 registers **v64-v127**
into zero-initialised local variables whenever `non_volatile_as_local` was set,
with no check for whether a function reads such a register *before* it writes
one. A function handed a value in one of them read **zero**.

Exactly **seven** functions in the whole 42,504-function title do that, and all
seven are the video codec. Two of them are its inverse-transform kernels, which
receive their `vperm` control vectors in `v69` and `v72`. Both arrived as zero,
so both permutes did the same thing, the 8x8 transpose duplicated one row over
another, every block's DC was split evenly between vertical coefficients 0 and
1, and a flat block came out as a gradient.

That is the whole reason videos were the only thing in the game that looked
wrong.

```cpp
// src/codegen/builders/context.cpp   -- the fix
if ((cfg.nonArgumentRegistersAsLocalVariables && (index >= 32 && index <= 63)) ||
    (localizeNonVolatiles() && (index >= 14 && index <= 31))) {   // was: || (index >= 64 && index <= 127)
```

v14-v31 stay localised - that is what stops a callee corrupting its caller, and
this port needs it. v32-v63 are volatile and unaffected either way.

## What actually found it

Four confident hypotheses died first. What worked was refusing to trust any
conclusion that wasn't measured, and building a test whose correct answer was
known in advance.

**The reproducer** - `tools/video_probe/ramp_320x580_intra.wmv`. A static
greyscale vertical ramp: every row one flat grey, 0 at the top to 255 at the
bottom, encoded to the intro panel's exact profile and forced to intra frames so
every frame carries real coefficients. ffmpeg decodes it back with every row's
spread at **0.00**, so any deviation is the game's.

That one clip separates every failure mode:

| what breaks | how the ramp shows it |
|---|---|
| pitch / stride / tiling / plane addressing | rows stop being flat |
| horizontal transform | rows stop being flat |
| **vertical transform** | **rows stay flat, row VALUES go wrong** |
| chroma | U/V stop being a flat 128 |

It came back with rows perfectly flat and their values wrong - so layout was
eliminated outright and the vertical pass was the only thing left.

**The profiler.** Eight guessed candidate functions had already come back with
zero calls, so guessing was abandoned for sampling: a thread that parks every
other thread in turn, reads its instruction pointer and lets it go. 98.6% of all
samples sat in `db16cyc` spin loops; filtering those out named the real kernels
in one run. The plane-copy thread turned out to spend 96% of its time *waiting* -
it never decoded anything, which is why every earlier trap caught only `memset`
and `memcpy`.

**Then the arithmetic.** Hooking the kernel's entry and its single `blr` gave a
known input and its output together:

```
IN  (coefficients):  455  0  0  0  0  0  0  0     <- DC only
OUT (should be flat):
  row 0: 281   row 2: 211   row 4:  80   row 6: -30
  row 1: 271   row 3: 161   row 5:  30   row 7: -40
```

A forward DCT of that column gave `k0 = 340.83`, `k1 = 340.73` - the DC split
exactly evenly between two coefficients. A hook between the two passes then
caught it in the act: `v1` and `v5` both held 1285 where only one should have
been non-zero.

## Verified, not eyeballed

| | before | after |
|---|---|---|
| DC-only block decodes to | `281 271 211 161 80 30 -30 -40` | **flat, all 8 rows equal** |
| correlation with source video | r = 0.18 - 0.25 | **r = 1.000** (mean 0.938) |
| pixels clipped to 0 | 3 - 10% | **0.0%** |
| pixels at 255 | 4 - 5% | **0.0%** |

Ground truth has exactly 0.0% of each, and now so do we. Each decoded plane also
matches a *sensibly ordered* frame - Right at 20/40/60, Left at 8/28/46/68,
Centre at 12/32/54 - advancing monotonically, as playback does.

## What this cost, and what it ruled out

Recorded because each was expensive and is now closed:

| Ruled out | By |
|---|---|
| The video files | Re-encoded to a byte-matching profile with Microsoft's own encoder. Still wrong. `game/NinjaVI.wmv` is md5-identical to the untouched original. |
| An SDK or kernel decoder | The game imports no media APIs - xam.xex (65) and xboxkrnl.exe (173), nothing else. |
| A missing opcode | Zero `REX_UNIMPLEMENTED` sites across all 553 generated files. |
| The GPU / DXVA path | Instrumented the resolve path: the only resolve destinations in a whole run are the two 1280x720 framebuffers. During intro playback the GPU binds **only** the k_8 video planes - no `k_16_16_16_16` residual surface, no detile pass, no decode pipeline. The `1120x584 k_16_16_16_16_FLOAT` resolve that looked incriminating is the **scene's HDR buffer**, and appears only once the menu loads. |
| Pitch, stride, tiling, plane addressing | The ramp's rows come back with spread `0.00`. A stride error cannot leave a row flat. |
| Byte-order errors in the stores | 2-, 4-, 8- and 16-byte reversals and half-swaps all scored *identically* against ground truth (r = 0.18-0.25). No permutation recovers the picture. |
| The 52 untested vector builders | The codec's hot instructions are all in the **tested** set. Separately verified by hand: `vupkhsh`/`vupklsh`, `vmrghh`/`vmrglh`/`vmrghw`/`vmrglw`, `vpkswss`, `vpkshus`, `vsel`, `vspltish`, `vcmpequh`, `stvewx`, `lvx`/`stvx`, the `sllv`/`srlv`/`srav` epi16 emulations, and the `slw`/`srw`/`sraw`/`sld`/`srd`/`srad` count>=32 rule. All correct. |

## Things that cost time and would again

* **Every `ng2` rebuild re-copies the stock SDK DLLs** from `RexBlue` into the
  build folder, silently reverting any instrumented plugin or runtime. Redeploy
  after every build or the probes go quiet and lie to you.
* **Sampling "every Nth plane bind" aliases** against the 27-plane cycle: a
  stride sharing a factor with 27 lands on one plane in three forever. One run
  produced 36 chroma planes and no luma at all. Count each kind separately.
* **A static test clip produces no residuals**, so the transform is never called
  (measured: 0 calls, against 2,015,645 on real video). Force intra frames.
* **A gap in the emitted address sequence is not a dropped instruction.** The
  compiler interleaves the last two rounding shifts with the transpose network,
  which made pass 1 look asymmetric and nearly produced a fifth wrong root
  cause. Check the binary before believing it.
* **Mean absolute difference is the wrong metric** for a partly-corrupt image -
  the corrupt minority dominates it and buries a real match. And a low score
  against a near-flat frame is meaningless: always compute the chance baseline.

## The workaround this replaced

The port used to re-encode all 27 videos at install time and draw its own copy
over the guest. That is gone in v0.5.0: no `ffmpeg.exe` (97 MB) in releases, no
`prepare_videos.py`, no ~340 MB of derived files, and `video_mode` defaults to 0.

It also never could fix the **Techniques menu** clips, which are a small inset
inside a scrolling page - the overlay paints the whole viewport, so replacing one
covered the scroll, the text and the button prompt. Those work now.
