# Native GPU port — branch `native-gpu` (not pushed)

> **Status 2026-09-28: done by another route, and shipped.** NG2 did not take
> the Direct3D 9 / Plume route this plan describes. It kept the command stream
> and moved the SDK's own Direct3D 12 backend into the executable - vendored
> through the Fable II migration kit, see `src/native_gpu_xlat/ORIGIN.txt` -
> first beside the plugin (v1.1.0), then without it: `rexgpu-xenos.dll` is gone
> since v1.1.1, the draw recording has its own thread since v1.1.2, and v1.1.5
> is current. The branch is `native-gpu-ng2`, and `main` follows it. The current
> design is in `docs/ARCHITECTURE.md` ("The graphics path"), the defects in
> `docs/ISSUES_AND_FIXES.md` N1-N15. What follows is the original plan, kept as
> the record of the M3/M4 reconnaissance, which is still accurate.

Goal: replace the Xenia-derived Xenos emulation (`rexgpu-xenos.dll`, PM4 ring
buffer interpreted at the packet level) with a **native renderer**: intercept the
game at the **Direct3D 9 device API** it was written against and drive a modern
render HAL (Plume, MIT) with the game's own shaders translated offline
(XenosRecomp, MIT). The proof that this works is hedge-dev's UnleashedRecomp
(GPL-3.0: read as a blueprint only, never copied). The feasibility spike and the
prior decision to bank it live in `C:/Users/renoi/ClaudeCode/NativeGPU/SPIKE_PLAN.md`;
this branch resumes it at **M3**.

Why it matters: in Fable II's town the frame rate is bound by the GPU command
thread interpreting ~4,000 draws of PM4 per frame plus the readback/upload
bookkeeping the emulation needs; a native device layer submits those draws
directly and needs none of the guest-memory coherency machinery.

## M3 — size the Direct3D 9 surface (this milestone)

`tools/native_gpu/recon.py <game> [<other game>] --out docs/native_gpu/M3_<game>.md`
reads ReXGlue's generated `*_recomp.*.cpp` (every guest function carries its PPC
disassembly as comments) and reports, offline:

1. **PM4 emitters** — functions that build a type-3 packet header (`0xC0xxxxxx`
   with a known opcode) via `lis`/`ori`: the D3D library's ring-buffer writers.
2. **Vd\* callers** — functions that call the Xenos kernel entry points.
3. **The library closure** and the **API surface**: library functions called
   from engine code, with the number of call sites — the number that sizes the
   per-game HLE.
4. **Cross-game fingerprints** (mnemonic + registers, addresses masked): the
   XDK library is the same static code in both games, so functions matched
   across NG2 and Fable II are library, not engine, and one HLE serves both.

Facts established so far:
- NG2 and Fable II reference the identical 14 `Vd*` kernel functions → the same
  XDK Direct3D library is linked into both.
- The reference's device surface is 42 hooked functions + 15 stubs
  (CreateDevice, Lock/Unlock texture/vertex/index buffers, Get*Desc, Present,
  GetBackBuffer, Create texture/vertex buffer/index buffer/surface, StretchRect,
  SetRenderTarget, SetDepthStencilSurface, Clear, SetViewport, SetTexture,
  SetScissorRect, DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP,
  Create/SetVertexDeclaration, Create/SetVertexShader, SetStreamSource,
  SetIndices, Create/SetPixelShader, D3DXFill*), with SetRenderState /
  SetSamplerState / shader constants reached through a command queue
  (`RenderCommandType`). That is the template of what to find here.

Results: see `M3_ng2.md` / `M3_fable2.md` (generated).

Other tools in `tools/native_gpu/` (all read a RUNNING game through
ReadProcessMemory; guest = host - 0x100000000): `scan_tables.py` finds
function-pointer tables (`--funcs <game>` = exact recompiled addresses),
`find_word.py` finds who holds a value (device pointer globals), `peek.py`
hex-dumps guest memory, `entry_points.py` builds the per-entry-point feature
table from the recompiled code, `gen_trace_hooks.py` generates the call-census
tracer (hook TOML + C++) for a game project.

## Route decision (after M3)

- **Route A** — native backend inside ReXGlue: keep its recompilation and
  runtime, hook the D3D9 device functions by guest address (ReXGlue's hook
  system), translate shaders offline, render through Plume. Preferred if the
  API surface is tractable: everything else (kernel, audio, input, saves,
  updater, the two shipped games) keeps working.
- **Route B** — recompile under XenonRecomp and build the bridge as
  UnleashedRecomp did. Abandons ReXGlue's runtime; only if Route A's hooking
  proves impossible.

## Milestones

- [x] M0–M2 (spike): recon, Plume builds and renders here (`NativeGPU/spike/m1b_clear`), XenosRecomp recon.
- [x] **M3** — this branch: recon tool + reports (`M3_FINDINGS.md`).
- [~] M4 — Fable II first (`M4_fable2.md`): device found at guest 0x44142480
  with its D3DRS/D3DSAMP dispatch tables; call-census tracer over 223 hooks
  (fable2recomp branch `native-gpu`, also unpushed); then Present/Clear/one
  draw through Plume.
- [ ] M5 — one real scene rendering natively (spike success criterion).
