# M4 NG2 — what the GPU actually receives

The guest-side census answers *which Direct3D entry points a frame uses*. It
cannot answer *what the GPU is given*, and for NG2 the two disagree in a way
that matters. This is the ground truth, read off the PM4 stream the plugin
executes, with `NGPU_PM4=1` on the `ng2-native-gpu` plugin branch.

## 0. Why this had to be measured, not inferred

The static M3 scan found exactly one function with an immediate `DRAW_INDX_2`
opcode, `sub_8373B060`. In ten seconds of 60 fps gameplay it fired **once**
(`calls=1 +0`) against thousands of real draws. NG2 builds its draw packets
with a computed opcode, so a static scan for immediates cannot see them — and a
census that only counts *calls* will report a draw path that does not draw.

Reading the ring is not subject to that error: a packet either is a draw or is
not.

## 1. One frame, by packet type

Chapter 1 in-engine scene, 60 fps, 2026-09-16. Steady across frames.

| | per frame |
|---|---:|
| type-0 (register range write) | 4,100–15,500 |
| type-1 | **0, always** |
| type-2 (no-op padding) | 2,300–6,600 |
| type-3 (command) | 1,500–8,100 |

**Type-1 is never used.** Not rare — zero, in every frame measured.

Type-3 opcodes in a representative frame:

    DRAW_INDX(22)=663   DRAW_INDX_2(36)=1675   IM_LOAD(27)=1250
    IM_LOAD_IMMEDIATE(2B)=181   LOAD_ALU_CONSTANT(2F)=873
    INDIRECT_BUFFER(3F)=210   INVALIDATE_STATE(3B)=112
    WAIT_REG_MEM(3C)=49   EVENT_WRITE(46)=54   EVENT_WRITE_SHD(58)=6
    SET_BIN_MASK_LO(60)=6  SET_BIN_MASK_HI/SELECT_LO/SELECT_HI=1 each
    REG_RMW(21)=1   INTERRUPT(54)=3   XE_SWAP(64)=1

**Never seen, in any frame of any run so far** — the packets a native NG2 device
does not have to implement:

    NOP  VIZ_QUERY  SET_CONSTANT  SET_CONSTANT2  SET_SHADER_CONSTANTS
    INDIRECT_BUFFER_PFD  MEM_WRITE  REG_TO_MEM  SET_BIN_MASK  SET_BIN_SELECT
    EVENT_WRITE_EXT  EVENT_WRITE_ZPD  CONTEXT_UPDATE

`SET_CONSTANT` being absent is the surprising one: NG2 sets **no** constants
inline in the command stream. Constants arrive as `LOAD_ALU_CONSTANT` (a DMA
from guest memory) and as bulk type-0 register-range writes.

## 2. Draws, decoded

Decoding `VGT_DRAW_INITIATOR` rather than dumping the dword, because a raw
dword invites the same misreading twice:

| | frame 1500 | frame 1800 |
|---|---:|---:|
| indexed, source = kDMA | 637 | 540 |
| non-indexed, source = kAutoIndex | 1,701 | 1,362 |
| indices issued | 763,073 | 656,731 |
| index format | **16-bit, 100%** | 16-bit, 100% |

Primitive types, frame 1500:

    POINT(1)=1632   TRI_STRIP(6)=637   RECT(8)=49   QUAD(13)=20

Read together these are one clean statement:

- **Every indexed draw is a 16-bit-index TRI_STRIP.** All the scene geometry is
  one shape: `kDMA` + `kInt16` + `TRI_STRIP`. There is no second geometry path
  to support.
- **Two thirds of the draws are POINT draws with no index buffer at all.** 1,632
  of them per frame. That is NG2's particle/effect system, and it is the
  majority of the draw calls while contributing almost none of the geometry.
- `RECT`=49 is the full-screen pass count — post, clears, resolve-adjacent work.
- 32-bit indices: never.

**This is where a native path copied from Fable II would break.** Fable's
renderer draws from the game's own IB/VB. That reproduces NG2's 637 indexed
draws and misses its 1,701 auto-index draws — two thirds of the frame, and
every particle in it. Auto-index needs its own path from the start.

## 3. Shaders come by pointer, and there are few of them

`IM_LOAD` (pointer into guest memory) outnumbers `IM_LOAD_IMMEDIATE` (microcode
embedded in the packet) about 7:1. So a shader's identity is **its guest
address, straight off the packet** — it does not have to be recovered from a
guest function's arguments, which is how the Fable side had to find it.

Distinct shader addresses loaded per frame: **63–67.** They sit in tight blocks:

    PS  0x1DACD040 - 0x1DAF8040      PS  0x1EB3D000 - 0x1EB4A040
    VS  0x1DB43040 - 0x1DB63040      VS  0x1EB21040 - 0x1EB22040
    plus a handful at 0x1D67-0x1D72, 0x1EE30000, 0x1EFD-0x1F02

Hit counts are steeply skewed: the top VS is loaded 144 times in one frame, the
long tail once. A shader cache keyed on that address is the whole mechanism.

## 4. The state surface is 166 registers, not "Xenos"

Registers written in one frame, by range:

| range | distinct | writes/frame |
|---|---:|---:|
| 0x4000–0x47FF ALU float constants | 2,016 (= 504 float4) | 72,368 |
| 0x4800–0x4899 fetch constants | 154 | 5,800 |
| 0x4900–0x492F bool/loop constants | 40 | 9,160 |
| 0x2000–0x2FFF control/state | 152 | 8,121 |
| below 0x2000 config/EDRAM | 14 | 317 |

**166 registers below 0x4000 is the complete state surface of this title**, and
every one of them is named in the SDK's own register table — none unknown.

The per-draw working set is much smaller still. Written more than 90 times in a
frame (i.e. plausibly per-draw or per-batch):

    VGT_DRAW_INITIATOR 577   VGT_INDX_OFFSET 319   RB_DEPTHCONTROL 297
    RB_HIZCONTROL 293   PA_SU_SC_MODE_CNTL 293   SQ_PROGRAM_CNTL 291
    SQ_CONTEXT_MISC 291   RB_BLENDCONTROL0 232   RB_COLORCONTROL 232
    RB_BLENDCONTROL1/2/3 231 each   VGT_DMA_BASE 209   VGT_DMA_SIZE 209
    SQ_INTERPOLATOR_CNTL 158   SQ_GPR_MANAGEMENT 117
    PA_SC_WINDOW_OFFSET/SCISSOR_TL/SCISSOR_BR 111 each   RB_MODECONTROL 96
    VGT_MAX/MIN_VTX_INDX 91   PA_CL_CLIP_CNTL 91   PA_CL_VTE_CNTL 91
    PA_SC_VIZ_QUERY 91   PA_SC_AA_MASK 91   COHER_DEST_BASE_7 91

Everything from `VGT_CURRENT_BIN_ID_MIN` down is written **once per frame** —
the initial state block after `INVALIDATE_STATE`, not per-draw state. That
includes all 24 user clip planes, all the tessellation registers, the fog
colours and the debug registers: present, set once, never touched again.

`RB_COPY_CONTROL`/`RB_COPY_DEST_INFO` at 67 and `RB_COPY_DEST_BASE`/`PITCH` at
34 per frame put the resolve count at roughly 34 per frame, which squares with
`RECT`=49 full-screen draws.

## 5. What this changes for the NG2 native path

1. **Auto-index draws are a first-class path, not an edge case** (§2). Design
   for them before anything else; they are the majority.
2. **One geometry format.** 16-bit indices, triangle strips, `kDMA`. No 32-bit
   index support needed, no other topology for scene geometry.
3. **Shader identity is the `IM_LOAD` address** (§3), available without any
   guest-side hook. ~65 shaders per frame.
4. **Constants arrive by DMA, never inline** (§1). `LOAD_ALU_CONSTANT` and bulk
   type-0 writes; `SET_CONSTANT` is never used. A device that implements
   `SET_CONSTANT` and not the DMA path renders nothing.
5. **Scope is 166 registers** (§4), all named, ~25 of them per-draw.
6. **Depth needs its own identity and its own ownership, keyed on the depth
   base** — carried over from the Fable session, which found eight distinct
   depth bases against a single colour base and a NativeRT that pairs one depth
   texture per colour target. NG2's three depth bases (0, 518, 720) hit the same
   wall the moment two passes share a depth buffer. Model depth separately from
   the start rather than retrofit it.

## 6. How to reproduce

Plugin branch `ng2-native-gpu` (rexglue worktree), `NGPU_PM4=1` in the
environment — **`Start-Process`, not WMI**, which drops the parent's env.
`NGPU_PM4_EVERY=<n>` sets the dump period in frames (default 300 = 5 s), so a
gameplay frame can be picked out of the log afterwards instead of guessing a
frame number in advance.

The census costs one already-loaded bool per packet when off.

**Log-line caveat:** the `src[...]` field in the raw log is mislabelled —
`xenos::SourceSelect` is `kDMA=0, kImmediate=1, kAutoIndex=2`, so the first slot
is kDMA and the third is kAutoIndex. The `indexed=`/`auto=` counts printed
alongside it are correct and were used for everything above.

## 7. Not tiled — the draw counts above are per frame

NG2 emits `SET_BIN_MASK_LO` six times a frame, which raises a question that
would invalidate every number here if the answer went the other way: **if the
frame is replayed once per EDRAM tile, the ring asks for each draw several times
while the game issues it once**, and the counts in §2 are inflated by the tile
count rather than being per-frame.

Measured directly, by bucketing every executed draw on the `(bin_mask,
bin_select)` pair in force and separately counting the draw packets the
predicate *rejects*:

    frame 1500  bins: 1 distinct (mask,select) pair [FFFFFFFFFFFFFFFF] = 2342
                predicated draws = 673   rejected = 0

One bucket, every draw in it, nothing rejected. **NG2 is not tiled.** 673 draws
carry the predicate bit and all 673 pass, because both mask and select are
all-ones — the title sets up for predication and never uses it, which is why the
`SET_BIN_MASK` emitters exist and fire without a single replay behind them.

The rejected-packet counter is what makes the bucket count trustworthy. A
rejected packet returns from `ExecutePacketType3` before any census sees it, so
an instrument that counts only what executes cannot distinguish a single-pass
frame from a tiled frame whose other passes were all rejected. Counting from
inside the predicate branch, before the early return, is the only place it can
be seen.

Credit where due: the doubt came from the Fable II session, which found the same
emitters in its own entry-point table and realised its headline coverage number
might be comparing per-tile ring draws against per-call API draws. The emitters
being present proves nothing either way — which is exactly why it needed
measuring rather than arguing.

**Bins are per title, and NG2's answer does not carry.** Run on Fable II, the
same instrument found the opposite: two `bin_select` values carrying **exactly
1455 draws each**, with `15555555` and `2AAAAAAA` as complementary halves of an
alternating bit pattern. That is a frame replayed per bin. A shared native
device that assumes a single bin is right for NG2 and silently wrong for Fable —
silently, because the error is a multiplier on counts rather than a crash.

**The instrument has to enumerate its own subjects.** As first written the
bucket table held 8 entries and dropped draws *with no counter* once full: the
Fable run lost 2,422 draws a frame, 36% of the frame, into that hole, so the
tool meant to settle the question could not observe enough of the frame to
settle it. It now carries an overflow counter, per-bucket index totals (two
passes over the same scene carry the same indices; a different pass of similar
draw count does not), and a reconciliation — bucketed + overflow must equal
`indexed + auto` from §2, and the line says `reconciles` or
`DOES NOT RECONCILE` rather than looking clean either way.

NG2's numbers were re-checked against that arithmetic: 2342 = 641 + 1701,
1903 = 541 + 1362, 3618 = 1111 + 2507, six frames, overflow zero. The verdict
stands — but it stood by luck, because the overflow it happened never to hit
would have been invisible.

Re-run on the corrected instrument rather than assumed to survive it:

    frame 1500 bins: 1 pair [FFFFFFFFFFFFFFFF] = 2339 draws / 762,952 idx
                     predicated=670  rejected=0  overflow=0
                     2339 draws bucketed of 2339 - reconciles

The claim now rests on a tool that reports its own coverage instead of one
checked by hand afterwards.
