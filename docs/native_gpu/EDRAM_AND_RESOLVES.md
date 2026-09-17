# EDRAM, render targets and resolves: how the plugin models them

What a native renderer has to reproduce, and where the plugin approximates
rather than emulates. Sources are the shared tree `rexglue-src/`. Inference is
marked; everything else is read off the code.

File-location correction for anyone searching: there is no
`src/graphics/render_target_cache.*` and no `draw_util.*`. The API-independent
cache is `pipeline/render_target/cache.{h,cpp}`, the helpers are
`util/draw.{h,cpp}` (namespace `draw_util`), the backend is
`d3d12/render_target_cache.cpp`.

## 0. The headline: a frame is the RESOLVED image, not the live tile

`XE_SWAP` (command_processor.cpp:1131-1136) reads a **guest address**:

    frontbuffer_ptr    = reader->ReadAndSwap<uint32_t>();
    frontbuffer_width  = reader->ReadAndSwap<uint32_t>();
    frontbuffer_height = reader->ReadAndSwap<uint32_t>();
    IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

No render-target handle appears in the packet. The front buffer is a guest
buffer, which is what a resolve produces. **The player sees a resolved copy, so
anything drawn into the EDRAM tile after that resolve never reaches the screen.**

A native path that presents "the last bound target" will composite late passes -
impostor bakes, post steps, second-precision passes - over a scene that should
have been frozen at resolve time. This one fact explained three separate open
bugs on the Fable side.

## 1. EDRAM addressing

    tile       = 80 x 16 samples of 32bpp = 5120 bytes
    EDRAM      = 2048 tiles = 10 MiB exactly
    addressing = 11 bits, PERIODIC - tile 2048 is tile 0

From `xenos.h:411-421`. Consequences:

- **Format size does not change geometry.** A 64bpp surface takes twice the
  tiles, not wider ones.
- **Depth tiles have their 40x16 halves swapped** relative to colour
  (`xenos.h:263-268`). Games rely on it: they write depth/stencil values by
  drawing to a depth buffer's memory through a colour target.
- **Ranges wrap.** A target may run past tile 2048 and continue at 0; every
  range operation is split at the boundary (`cache.cpp:1381-1388`).

Arithmetic (`xenos.h:423-431`, `cache.cpp:631-656`):

    pitch_tiles  = ceil(pitch_pixels << (msaa>=4x) / 80) << is_64bpp
    length_tiles = ceil(height_samples / 16) * pitch_tiles << is_64bpp

Worked: 1280x720 32bpp 1x -> pitch 16 tiles, 45 rows, **720 tiles = 3.52 MiB**,
35% of EDRAM. At 4x MSAA the same surface needs 2880 tiles and **does not fit**,
which is why predicated tiling exists.

**Host render target height is not the guest's height.** Targets are allocated
at the maximum height fitting the period, `ceil(2048/pitch_tiles)` rows
(`cache.cpp:793-813`). A 16-tile pitch gives a 2048-pixel-tall texture whatever
the game asked for.

## 2. Register decode - the trap that cost a day

`RB_COLOR_INFO` (`registers.h:735-751`) is NOT a base:

    color_base     bits 0:10   in TILES
    (bit 11 split off and discarded - 11-bit periodic addressing)
    color_format   bits 16:19
    color_exp_bias bits 20:25

Two values in Fable's census, `0x00030000` and `0x000C0000`, look like distinct
bases and are not. Both decode to **base 0**, formats 3 (`k_2_10_10_10_FLOAT`,
7e3 HDR) and 12 (`k_2_10_10_10_FLOAT_AS_16_16_16_16`) - **the same surface at
two blending precisions**. `GetStorageColorFormat` (`xenos.h:353-362`) folds 12
to 3, and the key stores the storage format, commented "Ignoring the blending
precision and sRGB" (`cache.h:234`). The plugin treats them as ONE target.

Same trap for `RB_DEPTH_INFO` (`registers.h:834-848`): `depth_base` bits 0:10,
`depth_format` bit 16.

Census lines print these with `%08X`, so raw values read like addresses.
**Decode before believing.**

## 3. Render target identity and overlap

The key is one packed `uint32_t` of five fields (`cache.h:225-236`): base_tiles,
pitch_tiles_at_32bpp, msaa_samples, is_depth, resource_format. Height is absent
(a function of pitch). Guest format is absent (storage format only).

**Overlap is never tested pairwise.** A `std::map<uint32_t, OwnershipRange>`
keyed on absolute tile index covers all 2048 tiles with no gaps
(`cache.h:560-624`). When a draw needs a range, `ChangeOwnership`
(`cache.cpp:1243-1389`) walks it and, for every sub-range not already owned by
the destination, emits a `Transfer` and re-stamps the owner.

A transfer is a **copy, not an invalidate**: tiles are re-drawn through a pixel
shader converting the source to the guest bit pattern and reinterpreting it in
the destination format, via typeless resources and `_UINT` views so NaNs survive
(`d3d12/render_target_cache.cpp:1436-1501`). Eight modes exist, including
colour-to-depth and depth-to-colour.

Depth carries a **second owner** per guest depth format: float24 is emulated as
float32, and aliasing float24 with unorm24 through one float32 buffer loses
precision (`cache.h:569-600`).

## 4. Resolves end to end

A resolve is a draw with `RB_MODECONTROL.edram_mode == kCopy` (6), intercepted
at the top of `IssueDraw` before any shader work
(`d3d12/command_processor.cpp:2862-2866`). It is never rasterised.

**The rectangle comes from vertex fetch constant 0, written by the CPU**
(`util/draw.cpp:787-805`, carrying `// D3D9 HACK`). Exactly 3 vertices x 2 floats
or it is rejected. The host reads those vertices from guest memory directly; the
vertex shader never runs. This is why a replayed or deferred resolve reads a
stale rectangle - the CPU has moved on.

Destination: `RB_COPY_DEST_BASE/PITCH/INFO` (0x2319-0x231B). Bytes land in
**Xenos 32x32-tiled layout** with the destination endian applied, written into
shared memory at a guest address (`draw.cpp:945-1010`). A linear blit samples as
garbage.

`RB_COPY_CONTROL` (`registers.h:855-863`): `copy_src_select` 0:2 (>=4 means
depth), `copy_sample_select` 4:6, `color_clear_enable` bit 8,
`depth_clear_enable` bit 9, `copy_command` 20:21. **A value of 0 is valid and
meaningful** - colour RT0, sample 0, no clears, `kRaw` - and is what an ordinary
scene resolve looks like. It is also what "never written" looks like, so confirm
against a plausible non-zero `RB_COPY_DEST_BASE` before trusting it.

Copy and clear are not exclusive; one resolve can do both.

Execution is a **compute dispatch**, not `CopyTextureRegion`: nine shader
variants (fast when bitwise-equivalent, full when converting), preceded on the
host-render-target path by a dump of the owning targets into the EDRAM scratch
buffer (`d3d12/render_target_cache.cpp:1208-1303`). Afterwards
`MarkRangeAsResolved` -> `RangeWrittenByGpu` invalidates any cached host texture
over that guest range, which is how the resolved image becomes samplable.

**There is no render-target-to-texture shortcut anywhere.** An impostor bake is
a normal resolve to a guest address; the billboards sample it as an ordinary
guest texture keyed on the physical page.

## 5. Repeated resolves to one destination: a ring, not serialisation

The impostor pool resolves the same destination several times a frame
(`d3d12/command_processor.cpp:3864-3872`). The plugin does NOT serialise. Per
destination it keeps a ring of readback slots; when a resolve targets a slot
whose copy has not landed it **grows the ring** and takes a fresh slot, blocking
only when the ring is full. Before any slot is reused its pending copy is
**landed into guest memory**.

The invariant is **land-before-reuse**, not ordering. A native path holding
resolves in a GPU texture rather than guest memory owes the same invariant in a
different currency: do not overwrite the texture while a draw that sampled the
previous contents is still pending.

Two gates: this runs only with `d3d12_readback_resolve` in `kSome` mode and
`readback_resolve_keep_superseded` true. **`d3d12_readback_resolve` defaults to
FALSE**, in which case resolved bytes never reach guest CPU memory at all, only
the GPU-side shared-memory buffer.

## 6. Stencil

Packed with depth, 24:8, in the same 32-bit word, inside tiles whose 40x16
halves are swapped. On D3D12 it is one depth-stencil resource with two SRVs
(`d3d12/render_target_cache.cpp:1503-1552`). When `SV_StencilRef` is
unavailable, ownership transfer writes stencil **one bit at a time, eight draws
per rectangle**, using the stencil test itself as the write mechanism
(`:4540-4550`). A native path wants `PSSpecifiedStencilRefSupported` or it
inherits that.

## 7. The approximations to match or knowingly diverge from

Ordered by how much they can change pixels. All documented in the code.

1. **Host textures instead of EDRAM.** Self-described as "may be irreparably
   inaccurate, completely at the mercy of the host API's fixed-function
   output-merger" (`cache.h:38-45`).
2. **Render target height is an estimate.** Falls back to CPU-executing the
   vertex shader for unclipped draws, and that is OFF by default, so an
   unclipped draw claims up to all of EDRAM
   (`draw_extent_estimator.cpp:27-47`). The largest single heuristic here.
3. **MRT ranges clamped to the minimum inter-base distance**, justified by one
   game (`cache.cpp:600-624`).
4. **Blending-precision format variants collapsed** (the `_AS_` suffixes).
5. **16_16 / 16_16_16_16 range truncated** into SNORM16, undone in the resolve
   by an exponent-bias bump the code calls "a hack" (`draw.cpp:1068-1077`).
6. **8_8_8_8_GAMMA stored as UNORM16**; neither that nor sRGB matches the Xenos
   piecewise curve.
7. **float24 depth emulated as float32**, with double ownership tracking.
8. **Two bound targets sharing a base: all but the lowest slot dropped**
   (`cache.cpp:504-536`).
9. **Resolve rectangle read from vf0 on a D3D9 assumption**, hard-rejecting
   anything not 3x2 floats.
10. **Resolve rectangles force-aligned outward to 8 pixels** "for safety",
    writing up to 7 extra guest pixels per edge (`draw.cpp:844-853`).
11. **Resolves go dump-then-copy**; the direct path is a stub.
12. **Base bit 11 discarded** - wrap at 2048 tiles, do not honour a 12-bit base.
13. **2x MSAA may be emulated as 4x** with guessed sample positions.
14. **Resolution scaling is integer-only.**
15. **(ReXGlue-specific.)** The readback layer is timing-approximate by
    construction, and its default leaves guest RAM stale.

## 8. What NG2's native path must therefore do

- Present the guest buffer named by `XE_SWAP`, not the last bound target.
- Write resolve output in 32x32-tiled guest layout with the destination endian,
  at `RB_COPY_DEST_BASE`, and invalidate any cached texture over that range.
- Decode `RB_COLOR_INFO` / `RB_DEPTH_INFO` rather than keying on the raw value.
- Model EDRAM as a periodic 2048-tile space with the depth half-tile swap.
- Honour land-before-reuse for repeated resolves to one destination.
- Decide explicitly whether guest CPU memory is a channel at all; the plugin's
  default says no.
