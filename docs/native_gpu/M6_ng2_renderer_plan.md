# M6 NG2 — building the renderer

Recon is finished (`M5_ng2_architecture.md`). Nothing below is blocked on
understanding NG2; every input a draw takes is measured and available. This is
the build order, with the constraints already discovered attached to the step
they constrain, so they are not rediscovered.

## What is already in hand

| | where |
|---|---|
| Plume (D3D12 backend), built and proven to render on this PC | `ClaudeCode/NativeGPU/build/plume/plume.lib` |
| XenosRecomp, built | `ClaudeCode/NativeGPU/build/xenosrecomp/.../XenosRecomp.exe` |
| 607 NG2 shaders compiled to DXIL | `D:/ng2_frameinterp/shaders/out/dxil/` |
| the shader pipeline | `tools/native_gpu/extract_shader_containers.py` → `translate_all.sh` → `fix_hlsl.py` → `fix_hlsl_ng2.py` |
| a complete per-draw description from the ring | `ngpu_pm4::Describe`, plugin branch `ng2-native-gpu` |

## Order, and why this order

**1a. DONE — the plugin hands every draw over.** `rex_gpu_set_draw_callback`,
a plain C export beside the existing plugin ABI (rexglue `ng2-native-gpu`
d08c465), fired from `ExecutePacketType3`'s draw branch after the tiling
predicate. `GpuDrawRecord` carries `struct_size` first, the draw initiator, the
index buffer, both shader addresses with sizes and immediate flags, the register
file, and the predication state. Game-agnostic, so it belongs in the shared
plugin.

Shader-load tracking was split out of the census as part of this: it now runs
whenever *either* consumer is active, because a draw handed over with stale
shader addresses is worse than one not handed over at all. The hand-off itself is
gated on the callback pointer alone — a renderer must never depend on a
diagnostic being enabled.

Verified with `NGPU_DRAW_SELFTEST`, an internal counting consumer whose draw and
index counts must equal the census's own for the same frame — two independent
paths through the same packets:

    2,347 draws / 764,123 indices handed over vs census 2,347 / 764,123 — reconciles

at a steady 60 fps. The self-test earned its place on its first run by reporting
0 draws against 156: `Emit()` runs at the next packet boundary, *after*
`FrameEnd` resets the live counters, so it was reading post-reset zeros. Every
other counter here is snapshotted for that exact reason.

**1b. A Plume device and window — in the EXE, driven from the plugin.**

*Corrected after writing this file.* The obvious reading of "the renderer lives
on the GPU worker thread" is to put it inside the plugin, and that is wrong:
`rexgpu-xenos.dll` is the **shared** SDK component both titles load, and a
game-specific renderer does not belong in it. The Fable II side links Plume into
its own exe, which is right for that reason even though their renderer runs on
the wrong thread for the other one.

Both constraints are satisfiable at once, and the Fable II session has already
built the mechanism:

- the **plugin** exports a game-agnostic draw callback
  (`RexNgpuSetDrawCallback`, resolved by `GetProcAddress` on a plain C name,
  with a `size` field first so a plugin and an exe built apart mismatch loudly
  rather than reading garbage), fired from `ExecutePacketType3`'s draw branch
  after the tiling predicate;
- **ng2.exe** links Plume and implements the callback.

The callback arrives on the GPU worker thread and NG2's renderer does its work
there synchronously, so the renderer is in the game where it belongs and on the
thread the draws arrive on. This is the arrangement that makes the Fable II
side's 8 KB-per-draw queueing problem not arise — it was never solved, it is
avoided.

A shadow window beside the real one, so the plugin's own output stays untouched
and the two can be compared frame to frame. Nothing renders yet — this step
succeeds when a cleared window appears and the game still runs at 60 fps.

    NGPU_PLUME_DIR  C:/Users/renoi/ClaudeCode/NativeGPU/reference/plume
    NGPU_PLUME_LIB  C:/Users/renoi/ClaudeCode/NativeGPU/build/plume/plume.lib

**2. Auto-index point sprites.** *First*, because they are 1,701 of 2,338 draws
(§2). A path built for indexed geometry and extended to auto-index afterwards
would be built for the minority case, and it is the specific mistake copying
Fable II's renderer would produce — theirs draws from the game's own IB/VB and
has no auto-index path at all.

**3. Indexed triangle strips.** One shape only: `kDMA` + 16-bit + `TRI_STRIP`.
No 32-bit index path is needed for NG2 (not seen in any sampled scene — a
scene-bounded claim, not a title-wide one; Fable has 83 a frame). Index buffer
address, size, format and endianness all come off the draw packet.

**4. Shaders, keyed on the `IM_LOAD` address.** 63-67 distinct per frame, 73 of
76 covered by the 607 compiled.

> **Constraint: the GPU runs a PATCHED copy.** The executed microcode differs
> from the container's in ~9 of the first 64 bytes, inside instructions, where a
> `vfetch` encodes its operands. A translated container copy is therefore not
> the program the GPU ran. NG2's native path supplies bindings from the fetch
> constants independently, so this *may* be harmless here — but it must be
> confirmed, not assumed, before a shader is trusted.

**5. Vertex buffers, from the fetch constants.** Two dwords per slot: type in
the low 2 bits, 30-bit address **in dwords**, endian, 24-bit size in words.

> **Constraint: a populated slot is not a used slot.** Stale state persists in
> slots the shader never reads (one draw pointed a slot at guest `0x000000FC`).
> Which are live is decided by the shader's `vfetch` instructions. Until the
> translator supplies that, the slots that *change between draws* are the ones
> being rebound.

> **Constraint, from the Fable II side:** the sidecar's `computed` flag marks a
> fetch indexed by the shader rather than by vertex id — those cannot be
> input-assembler attributes at all and must be bound as a buffer the shader
> indexes. And `declaredtype` has to agree with the fetch format, or D3D12
> refuses the pipeline outright.

**6. Textures, from the texture fetch constants.** Six dwords per slot. Decode
verified field-by-field against the SDK's `xe_gpu_texture_fetch_t`:
format `dword_1` bits 0-5, endianness bits 6-7, `base_address` bits 12-31
(`>> 12`, so the byte address is `base << 12`), width/height 13 bits each in
`dword_2` for 2D.

**7. Render targets and resolves.** Decode `RB_COLOR_INFO` / `RB_DEPTH_INFO` as
**tile** bases, model EDRAM as a periodic 2048-tile space with the depth
half-tile swap, and present the guest buffer `XE_SWAP` names rather than the
last bound target (`EDRAM_AND_RESOLVES.md`).

> **Constraint: depth needs its own identity and ownership, keyed on the depth
> base.** From the Fable II session, which pairs one depth texture per colour
> target and found eight depth bases against one colour base. NG2's depth bases
> are **scene-dependent** (0/518/720 in Chapter 7, base 32 in a Chapter 1
> cutscene), so the wall is closer here than a fixed set would suggest.

## Things that are settled and must not be re-litigated

- **Where the renderer runs: the GPU worker thread. Where it LIVES: ng2.exe.**
  Those are different questions and conflating them puts a game-specific
  renderer inside the shared plugin. 100% of draws arrive inside indirect
  buffers, so there is no guest-thread draw path to synchronise with and nothing
  needs queueing across threads — the plugin hands the draw over on the worker
  thread and the exe's renderer handles it there.
- **NG2 is not tiled.** One `(bin_mask, bin_select)` bucket carries every draw,
  nothing rejected. Draw counts are per frame. (Fable II *is* three-bin tiled —
  bins are per title and this does not transfer.)
- **Constants arrive by DMA**, never `SET_CONSTANT`.

## Watch the screen, not the log, for a freeze

**User instruction, 2026-09-17: keep AI Vision on the game while it runs and
catch a lock early rather than losing time.** It is also the faster signal: the
log takes 30 s of watchdog before it admits anything, plus the polling interval
on top, and this branch's plugin hangs the Chapter 1 intro reliably.

A running game never goes static, so *stability is the freeze*:

    eye_raise(target = "pid:<pid>")          # once, makes it the default target
    eye_wait(stable_ms = 4000, timeout_s = 110)

Returning on the stable condition means frozen; timing out means alive. For a
long unattended stretch, `eye_watch(action="start")` before the run and
`eye_changes` after gives the same answer with a timeline attached.

MEASURED 2026-09-17, and it is better than it looks on paper. Across four runs
the alive case timed out at 60 s, 115 s and 240 s without ever going stable,
and the frozen case returned in **3.2 s and 10.1 s**. In the run that froze in a
CUTSCENE - the case where a held camera shot ought to fool a pixel detector -
vision called it at 06:59:10 while the draw counter did not visibly flatline
until 06:59:16. **It caught the onset six seconds before the log could**, because
a frame that has stopped advancing is stable long before the last in-flight
draws finish landing. It was right, and it was the fastest signal available.

Do not read a `[swap]` frame rate as a health check. NG2 swings 60 -> 30 -> 60
in normal play, and the working plugin and the broken one produced traces that
are indistinguishable by eye:

    shipped (ran 240 s):  60.3  30.1  60.0  60.0  59.4  58.6  57.2  30.0 ...
    mine    (froze):      60.3  60.0  60.0  59.4  58.4  60.0  40.5  30.0 ...

The authoritative confirmation is a COUNTER THAT STOPS ADVANCING - swap reports,
or the exe-side draw totals - never the rate itself.

## Known hazards when running

- The plugin built from this branch hangs NG2's Chapter 1 intro; it is a build
  configuration difference, not the census (`M5_ng2_census_hang.md`) and not the
  draw hand-off — re-confirmed 2026-09-17 with the same binary and
  `NG2_NATIVE_GPU` unset, zero callback lines in the log, identical stall on
  guest thread 17 / Event `F800003C`. **Do not drive into Chapter 1.** Reach
  gameplay another way, and watch the screen for the lock (above).
- `Start-Process`, never WMI — WMI drops the environment and every env-gated
  probe silently does nothing.
- Build the plugin from a shell that has run `vcvars64.bat`, with
  `C:\Program Files\LLVM\bin` on PATH, or clang falls back to MSVC 19.33 and
  refuses the 19.44 PCH. Reconfigure with `cmake --preset win-amd64` — a
  hand-rolled `-S/-B` drops the preset's `-march=x86-64-v2` and breaks every
  SSSE3 intrinsic in `src/core/memory.cpp`.
- Coordinate the machine through `~/.game-test-lock`, and restore the shipped
  plugin (`.sdkbak`) after every run.

## The correctness oracle, from the start

Every instrument built during the shader work measured whether a translation
**succeeded** or was **stable**. None measured whether it was **correct** — a
shader synthesised with NaN literal constants translates, stays byte-identical
across ten runs, carries its definition table, and renders nonsense. It passed
every check that existed.

**The renderer will walk into exactly this.** "It draws" and "it draws the same
every frame" are the same two questions wearing different clothes, and a
renderer can pass both while drawing the wrong thing.

The oracle here is unusually good and should be wired in before anything appears
on screen rather than after: **the plugin renders the same frame from the same
packets.** So for any draw the native path handles, the emulated result is
available for comparison — per draw, per render target, or per frame. Concretely:

- the plugin already reports its own per-frame draw and index totals, and the
  hand-off reconciles against them (M6 step 1a);
- the same comparison extends to geometry: for a given draw, the native path and
  the plugin should issue the same primitive count from the same index buffer;
- and ultimately to pixels — the plugin's resolved frame against the native one.

A native frame that renders something plausible is not evidence. A native frame
that matches the plugin's is.

## The milestone

One frame of NG2's own geometry in the Plume window, recognisable silhouette,
translated shaders, no textures — matching what the Fable II side called M4-d.
