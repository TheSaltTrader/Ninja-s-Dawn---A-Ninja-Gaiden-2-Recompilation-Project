# Native GPU port — branch `native-gpu` (not pushed)

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
- [~] **M3** — this branch: recon tool + reports.
- [ ] M4 — hook the minimal set and push one hardcoded draw through guest → HLE → Plume.
- [ ] M5 — one real scene rendering natively (spike success criterion).
