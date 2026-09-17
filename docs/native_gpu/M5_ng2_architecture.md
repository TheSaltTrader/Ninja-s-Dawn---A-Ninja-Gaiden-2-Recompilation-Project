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

The 325 compile failures are concentrated, not scattered:

    215  use of undeclared identifier 'iPosition0'
     58  use of undeclared identifier 'r32'
     49  use of undeclared identifier 'iBinormal0'
      1  use of undeclared identifier 'iDepth8'

`iPosition0` and `iBinormal0` are one cause. In a failing shader, `main()`
declares **no vertex inputs at all** — only `SV_VertexID` — while the body reads
`iPosition0`. Those are shaders whose attributes come from explicit `vfetch`
instructions rather than a declared vertex layout, so XenosRecomp, which builds
its input list from the container's definition table, emits references it never
declared. The same family as the Fable side's local "duplicate vertex-input
declarations" change.

This is consistent with §2: NG2's draws are two thirds auto-index, and an
auto-index draw has no input layout to declare — the shader fetches what it
wants. So the shaders that fail to compile are likely the same population that
makes NG2's frame unlike Fable's.

`r32` is separate: a GPR past the declared register count.

Next, in order: declare the fetched attributes for the vfetch-only shaders, then
`r32`. 291 compiled shaders is already enough to attempt the first milestone,
since the frame's hot shaders are a small set (63-67 distinct per frame).
