# M5 NG2 — where to intercept, and why it is not where Fable II intercepts

A decision record. The PM4 census (`M4_ng2_pm4_census.md`) changed the answer,
so the reasoning is written down rather than assumed.

## The question

A native path has to take the game's rendering somewhere and hand it to a
native renderer (Plume) instead of to the Xenos emulation. There are two
places to take it:

**(a) At the guest Direct3D call.** Hook `DrawIndexedVertices`, `SetShaders`,
`LoadConstants`, `SetRenderTarget`, `Resolve`. This is what the Fable II side
does, and it is the more thoroughly native of the two: the PM4 packets are
never built at all.

**(b) At the PM4 packet.** Let the guest's own Direct3D library build its
command stream as it always has, and drive the native renderer from the stream
inside the plugin, where `ExecutePacket` already sits.

## Why NG2 takes (b) first

Not because (b) is better in principle — it is not — but because for NG2 the
cost of (a) is currently unbounded and the cost of (b) is zero.

**(a) is blocked on identification, and the identification is hard here.**
Fable II's library has legible entry points; ours does not. Three weeks of
census, dirty-bit and transcript work have named the *shape* of NG2's calls
(a five-call state block, a draw-shaped cycle) without naming a single one of
them as `DrawIndexedVertices`. Worse, the one anchor the static scan gave us
was wrong: `sub_8373B060`, the only function carrying an immediate
`DRAW_INDX_2`, fires once in ten seconds against thousands of real draws.
Return addresses would break the tie and **cannot be obtained with this SDK** -
tested, both routes, recorded in `M4_ng2_frame_structure.md`. So (a) has no
credible completion date.

**(b) needs no identification at all.** Everything a native draw call requires
is already in the stream, and the census read all of it in a single run:

| the renderer needs | the packet gives |
|---|---|
| draw type, topology, index count | `VGT_DRAW_INITIATOR`, decoded |
| index buffer address, size, format | `VGT_DMA_BASE` / `VGT_DMA_SIZE` |
| vertex shader, pixel shader | the `IM_LOAD` address |
| shader constants | `LOAD_ALU_CONSTANT`, type-0 ranges |
| textures and vertex streams | fetch constants, 0x4800-0x4899 |
| all pipeline state | 152 control registers, all named |
| render targets | `RB_COLOR_INFO` / `RB_DEPTH_INFO` |
| resolves | `RB_COPY_*`, `RB_MODECONTROL.edram_mode` |
| present | `XE_SWAP`'s front-buffer pointer |

There is no missing input. There is no guest function left to identify.

**This is still native rendering.** "No emulation" is about how the pixels get
drawn - Plume and D3D12, the game's own geometry, its own translated shaders -
not about which side of the command stream we read the parameters from. Reading
a PM4 packet to learn "triangle strip, 1,197 indices, at this address" is what
a driver does; the Xenos emulation is what we are removing, and under (b) it is
just as removed.

## What (b) costs, honestly

The guest still builds PM4 packets that nothing consumes as packets. That is
real waste - the per-frame type-0 traffic alone is 4,000-15,000 packets - and
it is why (a) remains the destination. But it is waste in the *guest's* CPU
work, which is not where NG2's frame budget is tight, and it can be reclaimed
later by moving the interception point up without rewriting the renderer: the
renderer's inputs are the same either way, only their source changes.

So: **(b) now, (a) later as an optimisation**, and the renderer written against
an input struct rather than against the packets, so the move is a new front end
and not a rewrite.

## Order of work, driven by the census

1. **Auto-index point sprites first, not last.** 1,701 of 2,338 draws in a
   frame. A path built for indexed geometry and extended to auto-index later
   would be built for the minority case. Fable's renderer draws from the game's
   own IB/VB and has no auto-index path at all; copying it would reproduce 637
   of NG2's draws and miss every particle in the frame.
2. **Then indexed geometry**, which is one shape only: `kDMA` + 16-bit indices
   + `TRI_STRIP`. No 32-bit index path, no other topology.
3. **Shader translation keyed on the `IM_LOAD` address.** 63-67 distinct
   shaders per frame, in tight address blocks. XenosRecomp already translates
   126 of Fable's 132.
4. **Constants by DMA.** `SET_CONSTANT` is never used by this title; a device
   that implements it and not `LOAD_ALU_CONSTANT` renders nothing.
5. **Render targets with depth modelled separately from colour**, keyed on the
   depth base with its own ownership - carried from the Fable session's finding
   rather than retrofitted after it bites.
6. **Present the buffer `XE_SWAP` names**, not the last bound target. A frame
   is the resolved image (`EDRAM_AND_RESOLVES.md` §0).

## The first provable milestone

One frame, Plume window, NG2's own geometry: the 637 indexed triangle strips
with translated shaders and no textures. Recognisable scene silhouette is the
bar, matching what the Fable side called M4-d. Everything needed to attempt it
is in the census; nothing is blocked on further recon.

## Addendum: the container gap (2026-09-16, same day)

The interception decision above survives, but it has a cost that was not visible
when it was written, and it is worth stating next to the decision rather than
buried in a later note.

**The packet gives the microcode's ADDRESS, not the container that wraps it.**
XenosRecomp does not accept bare microcode: it scans for shader containers by
signature (`(flags & 0xFFFFFF00) == 0x102A1100`, sized by
`virtualSize + physicalSize`) and hands the container to the recompiler, because
the container carries the **constant table**. `IM_LOAD` names a physical address
in GPU memory; what sits there is raw control-flow microcode with zeros in front
of it.

Measured:

| scope | result |
|---|---|
| 22 MiB window around the frame's IM_LOAD addresses | 1 raw signature, 0 valid |
| whole 512 MiB physical space | 34 raw signatures, 0 valid |
| **guest virtual 0x80000000-0xA0000000** | **663 raw, 663 VALID** |

**RESOLVED, same evening. NG2 does use the XDK container format.** They are in
the guest VIRTUAL space, from 0x8200AC88 upward, with constant tables and
definition tables intact. They were never missing - the search was in the RAM
alias, which is where the microcode lives and the container does not.

The first virtual dump wrote a zero-byte file and the fix was the Fable
session's diagnosis: the title's heap is *allocated*, not reserved wholesale, so
a flat 512 MiB sweep crosses large unmapped holes. Page by page with a
`QueryProtect` check, writing unmapped pages as zeros so every file offset stays
its guest address: 14,976 pages mapped of 131,072, and every container found.

So the PM4 route's bill at the container is smaller than the paragraph below
says. It still does not get the container *from the packet* - but it does not
need the shader object either. A signature scan of the title's heap finds all
663, which keeps the route's independence from guest-side identification.

Next: XenosRecomp parses them and fails at the HLSL compile - undeclared
`b129`/`b137` bool constants and `iPosition0` - so NG2 needs its own
`shader_common.h` rather than Fable II's. Bounded, known work.

The Fable II side reads its containers at **shader object + 0x28** — it hooks
the guest call, so it has the object. The PM4 route never sees the object.

That is the honest statement of the trade, in the Fable session's words: *the
PM4 route buys independence from guest hooks and pays for it at the container;
the hooks route buys the container and pays for it in coverage — it sees half
the scene.* Neither route is free.

Three ways out, in the order they should be tried:

1. **Finish the search.** `TranslatePhysical` masks to `0x1FFFFFFF` and can only
   show the RAM alias; the shader object is allocated in the title's heap at a
   CPU virtual address. Until the virtual dump works, "NG2 has no containers" is
   a claim about the wrong address space.
2. **Synthesize the container.** The recompiler needs the header for sizes,
   flags and the table offsets; the packet supplies the dword count
   (`IM_LOAD`'s second word) and the type (its low two bits). A native path
   driven by PM4 does not need the constant table's *names* — it reads the ALU
   constants and fetch constants off the stream directly — so an empty constant
   table may be sufficient. Untested.
3. **Identify the shader object guest-side after all**, which is the part of the
   design this decision was meant to avoid. It would be a narrow exception
   rather than a return to full guest-call interception: one object, not a
   draw path.

## Addendum 2: every draw arrives inside an indirect buffer (2026-09-16)

Measured on both titles, by counting draws at indirect-buffer nesting depth > 0:

| | draws inside an indirect buffer |
|---|---|
| NG2, frame 1500 | **2,338 of 2,338 — 100%**, top level 0 |
| NG2, frame 1200 | 298 of 298 — 100% |
| Fable II, frame 6000 | 6,402 of 6,402 — 100% |

Two titles, two engines, both submitting every draw through chained command
segments. The ring never executes a draw at top level in either.

**This makes the interception decision right for a better reason than the one it
was made for.** The decision above chose the PM4 packet because NG2's Direct3D
entry points could not be identified with this SDK. The stronger reason is that
identifying them could never have been sufficient: a runtime that writes the
whole command stream as chained segments means hooking API wrappers is a
structurally partial view of the frame, and no amount of finding more wrappers
closes it. **The ring is the only complete source.** That is a property of the
Xbox 360 D3D9 runtime, not of either game, so it applies to the shared device
design and not just to NG2.

The Fable II side is moving its native path from drawing at hook exit to drawing
from the ring's own `DRAW_INDX` packets for the same reason — which makes
coverage 100% by construction rather than by enumeration.

### What is NOT established: command-buffer replay

The same run reports "204 of 210 buffers replayed unchanged, 0 rewritten"
against the previous frame, hashing each buffer in full. It is **not** recorded
as a finding, because the instrument's own self-check says the number cannot yet
be interpreted:

    hash check: 11 distinct hashes across 210 buffers, 0 untranslatable

The hash is not blind — it discriminates, and every buffer translated — but 210
buffers carrying only **eleven** distinct byte patterns is either a real
property (eleven command-buffer bodies repeated) or a length-decode fault
hashing a shared region of each. Until that is settled, "the command buffers are
replayed unchanged" is uninterpretable rather than true or false.

The self-check is the transferable part. An instrument that reports a null
result must be able to demonstrate it could have reported a different one; a
hash that returns a constant for every input reports "everything matched" and
means "I am blind". Counting distinct hashes per frame separates those for one
line of code.

## Addendum 3: NG2's shaders translate — 616 of 625 (2026-09-16)

The container gap closed and the pipeline ran. Using the Fable II side's
`translate_all.sh` + `fix_hlsl.py` unchanged, against 625 unique containers
extracted from the virtual-memory dump (`tools/native_gpu/extract_shader_containers.py`):

    translated 616 of 625 (9 recompiler crashes)
    dxc compiled 291, failed 325

That is a working shader path for NG2 on its first proper run, and it needed no
new tooling — the Fable header's `NGPU_BOOL` / `NGPU_LOOP` macros are exactly
what NG2's unnamed boolean constants (`b129`, `b137`) needed, which is why
running XenosRecomp directly had failed: the raw binary compiles inline and
never applies the fix-up pass.

**With a second NG2-specific pass (`tools/native_gpu/fix_hlsl_ng2.py`), every
shader that translates now compiles: 607 of 607, 0 failures.** 18 of the 625
containers crash the recompiler itself and produce no HLSL; that is the whole
remaining gap.

    XenosRecomp alone          0 compiled  (undeclared b129/b137 - no fix-up pass)
    + fix_hlsl.py            291 compiled, 325 failed
    + fix_hlsl_ng2.py        607 compiled,   0 failed

The pass does two things, both diagnosed from the error census rather than
guessed:

**Vertex inputs the body fetches but `main()` never declares** (960 references
to `iPosition0`, 224 to `iBinormal0`, 4 to `iDepth8`). XenosRecomp builds
`main()`'s input list from the container's vertex definition table; a shader
whose attributes come from explicit `vfetch` has no such table, so it emits a
`main()` taking only `SV_VertexID` and a body reading `iPosition0` anyway. **This
is the majority case for NG2 rather than a corner** - two thirds of its draws are
auto-index (§2), and an auto-index draw has no input layout to declare. The
shaders that failed to compile are largely the same population that makes NG2's
frame unlike Fable II's.

**GPRs past the declared count** (`r32`..`r36`, 344 references). The recompiler
sizes the register set from `SQ_PROGRAM_CNTL` and the microcode then uses more.

One trap worth recording because it produced a *worse* result than the failure
it fixed: matching the input identifiers as `i[A-Za-z]+\d+` also matches HLSL's
own vector types `int2`, `int3`, `int4`, so the first version declared
`in float4 int4 : NT4` and turned 2 compiling shaders into syntax errors. The
recompiler capitalises the usage, so the correct pattern requires an upper-case
character after the `i`. **A fix that introduces failures where there were none
is worse than the failures it removes**, and it was only visible because the
error census was re-read after the change rather than the total being compared.

Per the Fable II side, two things the runtime will need that this pass does not
supply: the sidecar's `computed` flag marks fetches indexed by the shader rather
than by vertex id - those cannot be input-assembler attributes at all and must
be bound as a buffer the shader indexes - and `declaredtype` must be checked
against the fetch format, because D3D12 refuses a pipeline whose input element
type class disagrees with the shader's declaration.

### What "607 of 607" is a rate OVER

Prompted by the Fable II session, which found that the shaders drawing two
thirds of *its* frame have **no D3D9 container at all** — loaded by `IM_LOAD`
from microcode blocks never wrapped in an API object, which is why sixty runs of
elimination could not find them. If NG2 were the same, a compile rate over
extracted containers would be a rate over the wrong denominator: the same shape
as their 91-99% coverage measured over hooked wrappers.

Checked by content rather than by address, because the two do not compare
directly — `IM_LOAD` names GPU **physical** addresses (0x1D..0x1F) while the
containers sit in the guest **virtual** heap (0x82xxxxxx), and the same shader
legitimately exists in both places. So: take the microcode the frame actually
asked for at each `IM_LOAD` address and look for those exact bytes inside the
extracted containers (`tools/native_gpu/check_shader_denominator.py`).

    distinct shaders the frame loaded (IM_LOAD)        76
    microcode found inside an extracted container      73
    NOT in any container                                3

**The first version of this check said 7, and 3 of those 7 were wrong** — found
by the Fable II session's follow-up warning that a container may point at an
*original* while the ring executes a *copy*. Probing only the first 64 bytes
missed three shaders whose bodies are in containers, including the busiest of
them all:

    VS 0x1D701000  x73   head missed, body found
    VS 0x1EFD7000   x6   head missed, body found
    VS 0x1EFD8000   x2   head missed, body found

So the coverage is **96%, not 91%**, and genuinely absent are three:
`VS 0x1DACB000` (x10), `VS 0x1EE30000` (x4), `VS 0x1F02F000` (x1).

### WITHDRAWN: "the microcode the GPU runs is a patched copy"

That claim was made here and is **wrong**. It came from diffing a ring copy
against a container anchored at the container's *region* start — but the
`Shader` struct carries a `physicalOffset`, and **539 of NG2's 625 containers
have `physicalOffset = 64`**. The code does not begin at the region start; it
begins 64 bytes in. So the diff compared 64 bytes of real code against 64 bytes
of preamble, and the "9 patched bytes inside vfetch operands" were an artefact
of the anchor. Caught by the Fable II session.

Anchoring correctly settles it in the other direction, and with positive
counter-evidence rather than an absence of evidence: **54 used shaders match a
container 100% exactly over 360-2048 bytes**, every one at `physicalOffset=64`
with the ring copy anchored at its own start. A patched copy would not be
byte-identical for two kilobytes. The ring copy *is* the container's code.

Worse, the specific diff that produced the claim was not merely mis-anchored — it
was **a diff against the wrong shader**. `VS 0x1D701000` matches no container
over any long span at either alignment; the 48-byte needle had hit a common
microcode prologue. So the nine "patched" bytes were code from one shader
compared against a preamble from another.

**Scope:** refuted for the 54 that have containers; unknowable for the 33 that
do not, since there is nothing to compare against.

### And the coverage figure was wrong twice

| claim | method | actual |
|---|---|---|
| 69 of 76 covered | 64-byte needle at offset 0 | too strict |
| 73 of 76 covered | 48-byte needles at several offsets | **too loose** |
| **54 of 87 covered** | >=256 bytes at >=98%, either anchor | measured |

A 48-byte needle collides on common microcode prologues — demonstrated: the
single match that produced the "patched copy" diff was a coincidence, and no
container matches that shader over any long span at either alignment.

So **real coverage is 62%, not 96%**, and the 33 without containers include the
two busiest pixel shaders in the frame:

    PS 0x1EB47000 x178    PS 0x1EB3F000 x167    PS 0x1D6FC040 x73
    VS 0x1D701000  x73    VS 0x1DACB000  x10    VS 0x1F000000   x9

**This changes the plan.** Microcode-only translation is not a tidy-up for three
stragglers; it is required for a third of the frame's shaders including the
heaviest. NG2 is much closer to Fable II's situation (41 of 54 vertex shaders
with no container) than the earlier numbers suggested. The Fable II side's
generator is at `tools/native_gpu/synth_xvu.py` and iterates offline against
microcode without a game run.

Two traps recorded with it, both from that side: a synthetic container must set
`physicalOffset` correctly or the recompiler starts 64 bytes early and runs 64
bytes long (6 of their 13 segfaulted that way); and padding microcode with zeros
raised their success rate 7/13 → 13/13 while *changing the decoded program* —
one shader gave 643 lines through its real container, 644 synthetic, 640 padded,
with different vertex inputs. A rising success rate is not evidence when what is
counted is "did it produce output".

**The first milestone still does not wait for it** — 54 shaders is enough to put
geometry on screen — but the frame will not be correct until the other 33 are
translated from microcode.

## Addendum 4: the input struct is complete (2026-09-17)

The last unlocated input was the geometry itself. A vertex fetch constant is two
dwords — type in the low 2 bits, a 30-bit address **in dwords**, then endian and
a 24-bit size in words — and 96 of them are overlaid on the same registers the
texture fetches use six dwords at a time. Decoded per draw, a real scene draw
now reads:

    #2 DRAW_INDX prim=TRI_STRIP indices=523
       IB guest=0x19738000 words=523 fmt=u16 endian=1
       VS=0x1DB62040 450 dw   PS=0x1DAF7040 123 dw
       RT colour base=0 fmt=3 | depth base=32 | pitch=320 msaa=0 | edram_mode=4
       depthctl=00700766 blend0=00010001 colorctl=87000004 mask=0000000F
       rast=00218002 progcntl=1031050A
       viewport scale=(128,-64,-1) offset=(128,64,1) scissor=..00800100
       vertex buffers: 11 slots hold a vertex fetch
                       [1]guest=0x003FE1FC 2624784B  [2]guest=0x003D0240 801304B
                       [5]guest=0x003D0000 2584B     [7]guest=0x0059E4FC 2626580B ...
       texture fetches: 10 of 32 slots typed kTexture

Topology, index buffer, both shaders, render targets, blend and depth state,
rasteriser, viewport, scissor, vertex buffers and textures — **every field a
native draw call takes, off the stream, with nothing left to identify.**

**What the slot count does NOT say.** It reports which slots *hold a well-formed
vertex fetch*, not which the draw *uses* — a slot the shader never reads can
hold stale state from an earlier draw, and one early draw showed a slot pointing
at guest address 0x000000FC, which is not a vertex buffer. Which slots are live
is decided by the shader's `vfetch` instructions and needs the translator.

There is a cheap discriminator in the meantime: **the slots that change between
draws are the ones being rebound.** Across consecutive draws slots 0 and 95
moved (0x0A4E1CBC → 0x0A4E1D10 → 0x08B800D0) while 4, 7 and 43 stayed fixed.
That separates live per-draw bindings from residue without waiting on shader
analysis.

### Recon is finished; the rest is a renderer

Nothing further is blocked on understanding NG2. What remains is building:

1. A Plume device and window inside the plugin (Plume is already built and has
   rendered on this machine).
2. Auto-index point sprites first — two thirds of the frame.
3. Indexed triangle strips, the one geometry shape.
4. The 607 compiled shaders, keyed on the `IM_LOAD` address, covering 73 of the
   76 a frame uses.
5. Textures, via the fetch constants above.

With the `computed` and `declaredtype` constraints from the Fable II side taken
as inputs rather than discovered later.

## Addendum 5: the 33 uncontained shaders are real programs, and most translate

`tools/native_gpu/cf_extent.py` (from the Fable II side) decodes a block's own
control flow and reports how far its program extends — answering the size
question from the shader's declarations instead of by trying lengths. Run
against the uncontained blocks:

    PS 0x1EB47000 x178  CF 24 bytes, program to 120  [Exec Alloc ExecEnd]
    PS 0x1EB3F000 x167  CF 24 bytes, program to 144  [Exec Alloc Exec ExecEnd]
    VS 0x1D701000  x73  CF 36 bytes, program to 168  [Exec Alloc Exec Exec Alloc ExecEnd]
    ...

**All ten sampled blocks carry well-formed Xenos programs** — clean control flow,
sensible extents of 84-168 bytes, a 24-byte CF section for pixel shaders and 36
for vertex. They are small, complete shaders, and `cf_extent` gives their exact
size, which is what a synthetic container needs.

### The sampler count, and a fix that survives its control

Synthetic containers built with the Fable II generator (`synth_xvu.py`,
`pad=0` — padding is a proven false fix there):

**XenosRecomp crashes NON-DETERMINISTICALLY** on identical input — same file,
same arguments, same binary, different outcome — found by the Fable II session.
So a single run's count establishes nothing, and the first version of this
section quoted "3 of 10 → 9 of 10" from one sample. Replaced by 10 runs per
shader per configuration:

| | 16 samplers | 32 samplers |
|---|---|---|
| **the 5 pixel shaders** | **0/10 each** | **10/10 each** |
| the 5 vertex shaders | 5, 8, 9, 9, 10 of 10 | 6, 7, 8, 8, 8 of 10 |
| total runs | 41/100 | 87/100 |

**The pixel-shader effect is deterministic**: every one fails ten times out of
ten at 16 samplers and succeeds ten times out of ten at 32. That is not a sample
from a random process. **The non-determinism is real but lives entirely in the
vertex shaders**, which fluctuate in both configurations.

Both things are true and neither cancels the other. The instrument that
separates them is per-shader repetition; an aggregate would not have, and the
41-vs-87 totals are only meaningful here because they are 100 samples a side.

5-of-5 by shader type is a far louder signal than a count, and a pixel shader is
where a `tfetch` above sampler slot 15 would live. The Fable II side had tested
more samplers on its own set and seen only membership shuffle — the hypothesis
was right, it was just invisible there.

**The recompiler's OUTPUT is non-deterministic too**, not just its crashes — the
Fable II session's finding, reproduced here. One unchanged file, 8 successful
runs:

    8 different file hashes          the output varies run to run
    statement count 12 every time    stable
    arithmetic, semantic names normalised away:  IDENTICAL all 8 runs

**The computation is invariant; every varying element is a semantic name or the
format decode that follows from it.** So the first version of this section, which
read a single `oDepth5` → `oBlendIndices10` difference as a sampler effect, was
reading run-to-run noise.

That same measurement makes *normalised arithmetic* a valid cross-configuration
instrument — it is stable across the flakiness where file hashes and counts are
not. Applied properly, 3 successful runs per configuration:

    1D701000   16 samplers: 2aa687bf ×3      32 samplers: 2aa687bf ×3
    1EE30000   16 samplers: 5a918740 ×3      32 samplers: 5a918740 ×3

Identical across configurations *and* across runs. **The sampler change is
semantically neutral for the computation** — the conclusion stands, now on
evidence that survives the non-determinism rather than evidence that never did.

**The "regression" was noise.** `VS 0x1EFD7000` looked like it worked at 16 and
crashed at 32; repeated, it is 9/10 and 7/10 — both inside the flaky band. It
was recorded as unexplained rather than rounded off, which is the only reason
re-measuring it was obvious.

A non-deterministic segfault on identical input is uninitialised memory or an
out-of-bounds read whose fault depends on heap layout: a XenosRecomp bug that no
container-level change will fix, only move. A debug build is the next step for
the vertex-shader faults.

### Two different bugs, each invisible in the other's population

The non-determinism and the pixel-shader crashes are **separate causes**, and
neither session could have found both.

**Cause 1 — the vertex ELEMENT table (Fable II session's find and fix).** A
`VertexShader` extends `Shader` with a vertex element array, and XenosRecomp
looks every vertex fetch up in it *by instruction address*, asserting when the
lookup misses. A synthetic header that stops at the 24-byte `Shader` struct
leaves the recompiler reading the constant table as the element array, indexed
by whatever `field18` happens to hold. That is the uninitialised read behind the
non-deterministic crashes *and* the varying output. Deriving the table from the
microcode — walk the control flow, the sequence bits mark which instructions are
fetches, a vertex fetch is opcode 0 in the low five bits — took their set from
78/104 attempts with 0 of 13 shaders stable, to **104/104 with 13 of 13
byte-identical across 8 runs**.

**Cause 2 — the sampler count (found here).** Declaring 32 samplers rather than
16 takes all five NG2 pixel shaders from 0/10 to 10/10, deterministically.

**The split is total, and it confirms cause 1 independently.** A pixel shader has
no vertex elements, so if the element table is the cause, pixel shader output
should already be deterministic. Measured, 5 successful runs each:

    all 5 PIXEL shaders    1 distinct output hash    DETERMINISTIC
    all 5 vertex shaders   5 distinct output hashes  VARIES

Five of five against five of five, split precisely on the predicted axis and on
no other — from a different engine and from the shader type the Fable II dump
path has never built. It also settles that the sampler result was never
contaminated by the element-table mechanism: those shaders are deterministic in
output *and* in crash behaviour, 0/10 and 10/10 with no flakiness at either end.

An all-vertex sample cannot find the sampler bug; a pixel-only sample cannot find
the element-table bug. Each session's population hid the other's cause.

### Still open

The **literal constants** — `c255 = (0, 1.0, 0.5, 0)` in a real translation,
zeros in a synthetic one — do come from the definition table, correctly
attributed but to the wrong symptom. A shader computing with 0.0 where it needs
1.0 is wrong in the way that renders rather than fails.

Semantic *names* are not recoverable and do not need to be: the native path binds
vertex buffers from the fetch constants and builds its input layout from the same
translation's sidecar, so the names only have to be self-consistent. That is why
varying names were harmless and the varying **format decode** was not.
