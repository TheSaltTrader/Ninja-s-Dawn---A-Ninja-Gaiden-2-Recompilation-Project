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
