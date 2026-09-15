# M4 design — the native backend's shape, from the census numbers (2026-09-15)

What the two censuses and the draw dump make concrete about a Direct3D-9-level
native renderer for Fable II (and, by the same XDK, NG2).

## 1. The hook set is ~20 functions, all already located

| role | Fable II TU1 | per frame (market) |
| --- | --- | --- |
| Present | `sub_82BA34D8` | 1 |
| DrawIndexedVertices / DrawVertices / DrawVerticesUP / (fan variant) | `sub_8221DFC0` / `sub_8221C3E8` / `sub_82217DB8` / `sub_82242668` | 1,900 / 335 / 115 / 100 |
| SetShader (vertex \| pixel by type bit) | `sub_82221858` | 1,500 |
| LoadShaderConstants (ALU constants from guest memory) | `sub_82221B90` (+ `sub_8222BFA0`) | 2,450 (+240) |
| SetRenderTarget / SetDepthStencilSurface | `sub_822192E8` | 580 |
| Resolve (EDRAM → texture) | `sub_82196750` → kicks `sub_82B9EEE0`/`sub_82B9F038` | 12 |
| Clear-type rectangle draws | `sub_821EFAC0`, `sub_82205F68` | 32 + 15 |
| SetPredication (tiling) | `sub_822655F0`, `sub_82221740` | 4,300 + 1,100 → **no-ops natively** |
| INDIRECT_BUFFER (command-buffer replay) | `sub_8219CD68` (+ `sub_822866E0`) | 14 |
| render/sampler state | the 121 table entries (generic dispatch) | ~10,000 |
| SetTexture / SetStreamSource / SetIndices | shadow-only writers of device+0x480.. and +0x3094 — not yet named; **not needed as hooks**: the draw reads their result from the device |
| resource create / lock / unlock | the `sub_82B9Dxxx` pool family — needed only for the upload-at-unlock optimisation, not for correctness |

Everything a draw needs is in the device struct at draw time (map in
`M4_fable2.md`): 32 fetch constants (+0x480), the index buffer object
(+0x3094), the shadow registers (+0x2880.., +0x2934..), float constants
(+0x780/+0x1780) plus the constants the last LOAD packets point at in guest
memory, and the two shader objects last passed to SetShader.

## 2. Why the per-draw cost can drop

Today: the game thread writes ~15,000 PM4 packets per frame (state, fetch,
constants, predication ×2 per draw, draws), the plugin's command thread
parses them, tracks every guest page the GPU may read or write (the readback
and upload machinery that produced the v0.2.10–0.2.12 work), and the game
thread spins in `sub_82BA1FA8` waiting for it (1.3–2.2 M polls per 10 s).

Natively, per draw: read ~40 words from the device, look up (VB address,
stride, IB address, shader pair, state block hash) in caches, record one
draw into a Plume command list. Tiling disappears (one pass, no predication,
no bin masks), Resolve becomes a copy or nothing, and vertex/index/texture
data is uploaded once per (address, size, content hash) rather than watched.

## 3. Bring-up path (shadow render)

Link Plume into `fable2recomp` and render into a **second window** while
the emulated path keeps drawing the real one; the hooks stay `void`
(fall-through) until the native frame is right, then flip to
`return_on_true` per function.

1. Present hook → Plume swap chain present of a cleared frame (M4-a).
2. Draw hook → for `DI` draws with a known vertex layout (one shader pair
   chosen from the dump), fetch the VB/IB bytes from guest memory, upload,
   draw with a hand-written HLSL pair (M4-b: "one hardcoded draw").
3. Shader pipeline: the `S` lines give every shader object and its
   microcode address; dump the microcode to files, translate offline
   (XenosRecomp, or the SDK's own Xenos→DXBC translator if it can be called
   from the app), load by hash at runtime (M5 groundwork).
4. State: the shadow register blocks are Xenos registers; the plugin's
   register→D3D12 translation (`rexgpu-xenos`) is the reference for
   blend/depth/raster/sampler decoding and can be ported field by field.

## 4. Risks named by the data

- **LOAD_ALU_CONSTANT from guest memory** (2,450/frame): the constants are
  not in the device, so the native path reads them from the table's guest
  address at call time — cheap, but the engine may rewrite that memory
  between the call and the draw; the dump's `ct` field plus a content hash
  will show whether it does.
- **Command buffers** (`INDIRECT_BUFFER`, 14/frame): the draws inside them
  were recorded earlier through the same entry points (the XDK's recording
  mode swaps the device's dispatch tables — the second table at device+0x224
  is probably exactly that), so the hooks see them at record time; the
  native side must record too and replay on `sub_8219CD68`.
- **GPU-written textures the CPU reads** (the readback problem): natively a
  Resolve target that the game later Locks must be read back; the census
  will show how many resolves are followed by locks.
- Shaders with dynamic constant indexing and integer constants are the
  known XenosRecomp gaps; count them in the shader dump before choosing the
  translator.
