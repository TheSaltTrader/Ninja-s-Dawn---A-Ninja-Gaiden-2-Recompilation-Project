# M4 — Fable II first: the device, its dispatch tables, and the tracer (2026-09-15)

Order changed by the user from "NG2 first" to **Fable II first** (its town is
the case that hurts). Everything here was read from the running TU1 build
(v0.2.12) with the tools in `tools/native_gpu/` and from the recompiled code.

## The Direct3D device (guest 0x44142480)

`scan_tables.py` looks for runs of exact recompiled-function addresses in a
running game's guest memory (`--funcs` builds the address set from
`generated/default`, so vtables — which point at data — do not match). In
Fable II the only heap-resident run that is not a jump table is at
**0x441424C0: 242 code pointers**, which is the XDK device's dispatch area:

| device offset | guest | content |
| --- | --- | --- |
| +0x00..0x2F | 44142480 | flags/counters (0x13A0, 0x001B89E8, 8, 0x80000001 ...) |
| +0x30 / +0x34 / +0x38 | 441424B0 | ring-buffer write pointer, ?, limit (`FF71E81C FF724444 FF7243A4`) — the offsets 48/56 every draw entry point reads and writes |
| **+0x40** | **441424C0** | **`SetRenderState` dispatch table, 0x65 = 101 entries, index = D3DRS value / 4**; entries 0..9 = the invalid-state stub `sub_82B96E78`, entry 10 (`D3DRS_ZENABLE` = 40) = `sub_82286D78` |
| **+0x1D4** | **44142654** | **`SetSamplerState` dispatch table, 0x14 = 20 entries** (`D3DSAMP_ADDRESSU` .. `POINTBORDERENABLE`) |
| +0x224.. | 441426A4 | 10 x `sub_828FD4A0` (the command-buffer-recording stub?) then ~110 more setters in `sub_82B976xx..82B979xx` — a second table (deferred/recording variants) |
| ... up to +0x5DB8 | | the largest offset an entry point reads through r3 (23992); the reference's `sizeof(GuestDevice) == 0x5E00` matches |

The allocation header before it (0x44142450: magic `ABCDEF12`, size 0x5E90)
confirms the 0x5E00-byte device plus a header.

**Globals holding the device pointer** (`find_word.py fable2 44142480 82000000 84000000`):
`0x83360364` (XDK static, `g_pDevice`), `0x834970E8`, `0x834A600C` (engine).
Everything else that holds it is a heap object (renderer classes).

This is the same layout UnleashedRecomp's `GuestDevice` documents for Sonic
Unleashed's XDK (`setRenderStateFunctions[0x65]` at +0x40,
`setSamplerStateFunctions[0x14]` at +0x1D4), so the device-level route is
confirmed for a *different* XDK build: the table indices are the D3DRS/D3DSAMP
enums and the entries are the functions to hook.

## Where the state setters live — the "engine-side PM4 emitters" resolved

The 121 table entries resolve to **112 distinct functions**: the library's
own defaults in `0x82B96E78..0x82B98170` (just below the Vd-caller cluster)
and **inline XDK instances scattered through the engine's address space**
(`0x82188750`, `0x821A8B88`, ..., `0x822C0968`, `0x82286D78` = ZENABLE). Those
are the 360 XDK's `D3DINLINE` state setters, instantiated once per translation
unit that used them and folded by the linker — which is exactly what M3 saw as
"33 engine functions that emit PM4 themselves". They are XDK code, not engine
code: M3's `M3_FINDINGS.md` reading ("Fable's engine has Direct3D inlined")
was right about the cause and wrong to call them engine emitters. Hooking a
setter by address catches both call paths (direct `bl` to the inline instance
and the generic `SetRenderState` dispatch through the table), so the HLE needs
no table patching.

## The 111 library entry points — feature table

`entry_points.py` (`M4_fable2_entries.md`) lists, per entry point: engine
call sites, instruction count, argument registers read before written, the
device offsets read/written through r3, PM4 opcodes, library callees and
kernel imports. Already named from it:

| function | role | evidence |
| --- | --- | --- |
| `sub_82BA6990` | `Direct3D_CreateDevice` (1 site) | `VdInitializeEngines`, `ExGetXConfigSetting`, `VdIsHSIOTrainingSucceeded`, `RtlInitializeCriticalSection`, calls the ring init |
| `sub_82BA2830` | device/ring init | `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, `MmGetPhysicalAddress` |
| `sub_82BA6C18` | device reset/shutdown path (3 sites) | `VdShutdownEngines`, `KeSetEvent`, `ExRegisterTitleTerminateNotification` |
| `sub_82BA34D8` | **Present** (3 sites, 457 insns) | `VdSwap`, `VdPersistDisplay`, `VdGetSystemCommandBuffer`, `VdSetDisplayMode`, `REG_RMW` |
| `sub_82B9EEE0`, `sub_82B9F038` | **draw kick** (read only r3; ring +0x30/+0x38) | `DRAW_INDX_2`, `INVALIDATE_STATE`, `IM_LOAD_IMMEDIATE`: the parameters are already in the device — the typed `Draw*` entry points are the callers that store them |
| `sub_82BA1D40` | `SetPredication` / tiling bin mask (21 sites, r3 r4 r5) | `SET_BIN_MASK_HI/LO`, device +0x2AB4.. |
| `sub_82BA2F68` | fence/wait (3 sites) | `WAIT_REG_MEM` |
| `sub_82BAE440` / `sub_82BAEA88` | display mode / scaler setup | `VdQueryVideoMode`, `VdInitializeScalerCommandBuffer` |
| `sub_82BA2218` / `sub_82BA2200` | ref-count add/release on the device (+0x3C) | 10 sites, calls the shutdown path when it hits 0 |
| `sub_82B9D400`, `sub_82B9DD48`, `sub_82B9D750`, `sub_82B9D990`, `sub_82B9DCB8`, `sub_82B9DC60`, `sub_82B9D4C0`, `sub_82B9D540` | resource-pool functions (device +0x4DB0..0x4DE4 = 19888..19940: a free-list/ring of resource headers) | the Lock/Unlock/Create family — the census will separate them |

The rest are named by the census below rather than by reading 111 bodies.

## The call-census tracer (fable2recomp branch `native-gpu`)

`gen_trace_hooks.py <M3 json> <tables.txt> 441424C0 <fable2recomp>` writes

- `config/hooks/native_gpu_trace.toml` — one `[[midasm_hook]]` at the **first
  instruction** of each of the **223** functions (112 setters + 111 entry
  points), passing r3..r10. ReXGlue's mid-asm hooks are codegen-time, so the
  file is included from `fable2_manifest.toml` and the game is rebuilt.
- `src/native_gpu_trace.cpp` — the bodies: a per-function atomic counter, the
  first 4 argument samples, and every 10 s a `[ngpu]` census in the log
  (total calls, calls in the last 10 s, the samples once). Off unless the
  `ngpu_trace` cvar is set (`FABLE2_TUNE=ngpu_trace=1`), and then each call
  costs an atomic increment.

The same mid-asm mechanism with `return_on_true` is the HLE hook itself: the
hook takes over (returns true, r3 set) when the native backend handles the
call and falls through to the recompiled body when it does not — one file per
entry point, no table patching, and the emulated path stays available per
function during bring-up.

## Census 1 — the market, 150 s, hero 2 (`M4_fable2_census1.md`)

Tracer cost: none measurable (guest fps 55–60 in the market with 223 hooks
live, the v0.2.12 figure). Facts the census established:

- **The D3DRS enum reconstruction is verified in full.** Every state setter
  arrives with r8 = 0x40 + D3DRS value (the dispatch slot offset the generic
  `D3DDevice_SetRenderState(dev, state, value)` computes before `bctr`), and
  every r8 matches the name the generator assigned: ZENABLE 0x68 (40),
  COLORWRITEENABLE1 0x118 (216), VIEWPORTENABLE 0x170 (304),
  HIGHPRECISIONBLENDENABLE 0x174 (308), ALPHATOMASKENABLE 0x190 (336),
  HISTENCILENABLE 0x1A8 (360), HIZENABLE 0x1C0 (384), BUFFER2FRAMES 0x1D0
  (400). Samplers likewise: r8 = 0x1D4 + 4·D3DSAMP (ADDRESSU 0x1D4 ..
  TRILINEARTHRESHOLD 0x20C). So the engine reaches the setters through the
  **generic dispatch**, not direct inline calls — either hook point works.
- **Per-frame state traffic** (peak 10-s window / 60): HIGHPRECISIONBLENDENABLE
  2,500, HIZENABLE 1,700, ALPHATESTENABLE 275, ALPHAREF 215, STENCILREF 190,
  sampler ADDRESSU/V 150 each, MIN/MAG/MIPFILTER 100 each, SRC/DESTBLEND 90,
  ALPHABLENDENABLE 65, ZFUNC 55, CULLMODE 47, COLORWRITEENABLE 45, ZWRITE 45,
  ZENABLE 10; ~45 states are set exactly once per frame (the WRAP*, POINT*,
  STENCILFAIL... reset block); 24 setters ran once at boot only.
- **A state setter writes the shadow register and the dirty flags, never the
  ring**: `SetRenderState_ZENABLE` = `stw r4,0x2E64(r3)` (the D3D value),
  `rlwimi` into the packed register at device+0x2934 (DB_DEPTHCONTROL),
  `ld/std 16(r3)` with bits 11 and 17 set (the 64-bit dirty flags live at
  device+0x10, not +0x00 as in Sonic's XDK). The draw flushes dirty ranges.
- **Present** `sub_82BA34D8` once per frame (542–600 per 10 s = the fps), with
  `sub_82BA2F68` (WAIT_REG_MEM), `sub_82BA3148` (REG_RMW), `sub_82BA95E0`
  (VdQueryVideoMode), `sub_82BAED78` (EDRAM retrain), `sub_82BAAA28` (clock
  gating) and `sub_82BA1EE0` in lockstep — the swap path.
- `sub_82BA1FA8` (r3 = 0x701BF2B0, the ring status block, not the device) is
  the **GPU wait spin** — 1.3–2.2 M polls per 10 s: the game thread waiting for
  the command thread, i.e. the frame-rate bottleneck the emulation profile
  already named.
- `sub_82B9EEE0` + `sub_82B9F038` (the DRAW_INDX_2 kicks, 12/frame, args
  (dev, 0, surface-like pointer, 0, 0/1, 0x80, 0x20000, 0x5C8)) are the
  **Resolve** draws (their caller `sub_82196750` takes the device and a
  parameter block with rects and writes device+0x2898 first). The real
  per-object draws — thousands per frame by the setter counts — come from a
  function the address-cluster library definition missed: the fix is
  **census 2**, which also hooks the 78 direct callers of the ring make-space
  helper `sub_821E8EC0` (five clusters: 82193008–82242668 (37),
  822655F0–822C91F0 (13), 82A7FDE0–82A8ABB8 (3), 82B6F1D0, 82B9EEE0–82BAEA88
  (24)) — every PM4 writer, whatever its address.
- Other named: `sub_82BA4FB8` (SET_BIN_SELECT, r3 = device+0x2AC0, 9/frame) =
  the tiling-pass selector; `sub_82BAA848` (EVENT_WRITE_SHD, 2/frame) =
  frame fence write; `sub_82B98F00` (device, 15/frame, args 0,0,0x4040,0x4A,
  0x37E0) = a per-pass setup call, to be read; `sub_82B997A8` (2.7 M calls
  while loading, ptr/ptr/size) = the XDK's write-combined memcpy, not API.
- 69 of the 223 hooks never fired in this run (the CCW_STENCIL*, blend-alpha,
  HIGHPRECISIONBLENDENABLE1..3, clip-plane, and the display/scaler init
  entries — created once at boot before the tracer's first census, or unused).

## Census 2 — every PM4 writer hooked (`M4_fable2_census2.md`)

Hooking the 78 direct callers of the ring make-space helper found the real
per-frame API. Peak calls per frame in the market (10-s peak / 60), with the
name each function earned from its disassembly:

| function | per frame | what it is (evidence) |
| --- | --- | --- |
| `sub_822655F0` | 4,300 | **SetPredication**(dev, mask): stores the mask at device+0x31A4, emits `SET_BIN_MASK_LO` (0xC0006000); called from the engine's draw wrapper `sub_8217E0B8` around every draw (tiling) |
| `sub_82221B90` | 2,450 | **LoadShaderConstants**(dev, table, base, base2, n): walks a table of (count, start) pairs and emits `LOAD_ALU_CONSTANT` (0xC002xx2F00) packets whose address points into guest memory. Correction from reading `SetShader`: the table is the shader object's own literal-constant table (`obj+872`, base `[obj+32]`) and SetShader calls the loader itself, so these are shader literals; per-object constants still go through the device shadow (+0x780) — the dump's `vc` hash checks that |
| `sub_8221D1B0` | 2,100 | fetch-constant flush: iterates a 64-bit dirty mask, `sub_8228EF80`(dev, reg 0x4800 + 6·i, device+0x480 + 24·i, n, 6) → type-0 writes of the 6-dword fetch constants |
| `sub_8221DFC0` | 1,900 | **DrawIndexedVertices**(dev, primType, baseVertexIndex, startIndex, indexCount): flushes the five dirty masks (see the device map), reads the index buffer object at device+0x3094 (+0 address\|format, +24 size) and emits `DRAW_INDX` 0xC0032201 (predicated) |
| `sub_82221858` | 1,500 | **SetVertexShader / SetPixelShader**(dev, shader, type): emits `IM_LOAD` (0xC0012700) with the shader object's code address (+64 → header, +40..+52 sizes) \| type |
| `sub_8221B010` / `sub_8221AE18` | 1,250 each | the register/fetch flush pair for the dirty16 mask (bits 11..14 = fetch constants; `sub_8221B010` calls `sub_8221AE18` first) |
| `sub_82221740` | 1,100 | SetPredication variant that reads the device from the global `g_pDevice` (0x83360364): `SET_BIN_MASK_LO/HI` |
| `sub_822154B0` | 1,050 | render-target/predication flush (compares device+0x3098.. pending vs +0x31B8.. current EDRAM surfaces; `SET_BIN_MASK`, `SET_CONSTANT` 0xC0012D01) |
| `sub_82213AD8` | 970 | ring reserve(dev, dwords) → write pointer (the second space helper) |
| `sub_822192E8` / `sub_822194B8` | 580 | **SetRenderTarget / SetDepthStencilSurface**(dev, surface, index): type-0 writes of RB_SURFACE_INFO (0x2000), RB_COLOR_INFO, RB_DEPTH_INFO |
| `sub_8221C898` | 390 | register writer (`li 0x2200` = RB_DEPTHCONTROL block) |
| `sub_8227D150` | 360 | ? (dev, 1, 0, 4, obj) |
| `sub_8221C3E8` | 335 | **DrawVertices**(dev, primType, startVertex, count) — non-indexed (QUADLIST 0xD in the samples: the UI) |
| `sub_8220BD40` | 290 | (obj, dst, 16, ...) — a resource copy/lock helper |
| `sub_8222BFA0` | 240 | constant-table loader variant (also `LOAD_ALU_CONSTANT`, with an `sync`) |
| `sub_821F9918` | 200 | window scissor (type-0 PA_SC_WINDOW_SCISSOR_TL/BR, packs x/y/w/h from r4..r7) |
| `sub_82217DB8` | 115 | **DrawVerticesUP / DrawIndexedVerticesUP**(dev, prim, ..., stride, ...) (10 args, `mullw count·stride`) |
| `sub_82242668` | 100 | draw variant (dev, TRIANGLEFAN=5, 4, ...) |
| `sub_821EFAC0` | 32 | predicated `DRAW_INDX_2` (0x3601) — a rectangle draw (clears) |
| `sub_82206888` | 32 | 912 insns, sets every dirty bit: state invalidation after a command-buffer / tiling pass |
| `sub_82205F68` | 15 | DrawVertices RECTLIST (8) — resolves/clears |
| `sub_8219CD68` | 14 | `INDIRECT_BUFFER` — command-buffer replay (the second emitter `sub_822866E0` is called from inside the library) |
| `sub_82196628` / `sub_82196750` | 12 | **Resolve**(dev, flags, dest texture, ...) → the `DRAW_INDX_2` kicks `sub_82B9EEE0`/`sub_82B9F038` |
| ~20 functions | 1 | frame setup / Present internals (`sub_82193008`, `sub_821B9A08`, `sub_821C6478`, `sub_8227F3B0`, `sub_822A5EE8`, `sub_822A61C0`, ...) |

Totals: ~2,500 CPU-side draws per frame; the plugin counts 4,800–6,100 GPU
draws per frame in the same scene, so predicated tiling replays each draw
about twice (two tiles). The XDK's `D3DPRIMITIVETYPE` seen: 4 TRIANGLELIST,
5 TRIANGLEFAN, 6 TRIANGLESTRIP, 8 RECTLIST, 0xD QUADLIST.

## The device map (this XDK build) — what a draw hook can read

From the draw functions' flush prologue (`ld` of the five masks, then
`sub_8221DE68`(dev, mask, reg base, source) per block) and the setters:

| device offset | content |
| --- | --- |
| +0x00, +0x08 | dirty masks: vertex ALU constants (flushed to reg 0x4000 from +0x780), pixel ALU constants (reg 0x4400 from +0x1780) |
| +0x10 | dirty mask: registers and fetch constants (the setters OR bits here; bits 11..14 = fetch constants) |
| +0x18, +0x20, +0x28 | further masks (+0x28 is ANDed with the flushed mask) |
| +0x30 / +0x38 | ring write pointer / limit |
| +0x40 / +0x1D4 | SetRenderState / SetSamplerState dispatch tables |
| **+0x480** | **32 fetch constants × 24 bytes** (textures and vertex streams: word 0/1 carry the base address — the object identity key) |
| **+0x780** | vertex-shader float constants (0x400 dwords) |
| **+0x1780** | pixel-shader float constants (0x400 dwords) |
| +0x2880 | shadow registers 0x2000.. (RB_SURFACE_INFO, COLOR_INFO, DEPTH_INFO...) |
| +0x28CC | shadow registers 0x2100.. |
| +0x2920 | shadow registers 0x2180.. (SQ_PROGRAM_CNTL...) |
| +0x2934 | shadow registers 0x2200.. (RB_DEPTHCONTROL: `SetRenderState_ZENABLE` does `rlwimi` bit 1 here; its D3D value is kept at +0x2E64) |
| +0x2AC0 | tiling state object (r3 of the SET_BIN_SELECT function) |
| +0x2E2C | current shader/declaration pointer (5 setters write it with dirty bit 19) |
| +0x3094 | current index buffer object (DrawIndexedVertices reads +0 and +24 of it) |
| +0x3098..+0x30A8 vs +0x31B8..+0x31C8 | pending vs current render-target surfaces (compared by the scissor/flush/predication functions) |
| +0x31A4 | predication mask (`SetPredication` stores r4 here) |
| +0x33C8 / +0x33CC | the two words `DrawIndexedVertices` reads before its second packet (visibility/occlusion query?) |

This is the reference's `GuestDevice` layout shifted by the dirty-mask
prologue (Sonic's XDK: `samplerStates` at 0x480, constants at 0x780/0x1780
too), so **UnleashedRecomp's field offsets are directly reusable** for the
fetch constants and float constants.

## Census 3 + draw dump 1 — the shadow-only setters and the objects (`M4_fable2_census3.md`, `M4_fable2_dump1.md`)

The 29 functions `dirty_setters.py` found (OR into the dirty mask at +0x10
without writing PM4) were hooked too. Per frame in the market:

| function | per frame | what it is |
| --- | --- | --- |
| `sub_821B6C60` | 3,600 | **SetStreamSource**(dev, stream, vertexBuffer, offset, stride, dirtyBit): reads the VB object's physical address (+0x18) and size (+0x1C), writes the **vertex fetch constant pair for stream i at device+0x778 − 8·i** (= Xenos vertex fetch constant 95 − i, the top of the 192-word block), the VB object at device+0x30AC + 4·i, the stride byte at +0x30F0 + i, and ORs the caller's bit into the mask at +0x18 |
| `sub_82232510` | 1,060 | **SetVertexShader**(dev, shader): stores the object at **device+0x3198**, sets dirty bit 19, then walks the shader's literal-constant table (obj+872) and CLEARS those registers' bits in the vertex-constant dirty mask (+0x00) — the shader's LOAD packets own them |
| `sub_82208BB0` | 215 | **SetPixelShader**(dev, shader, ...): stores at **device+0x3194**, dirty bits 17, 20 |
| `sub_82221858` | 1,500 | the flush that emits `IM_LOAD` for the dirty shaders (its third argument is a pointer to the shader's variant-table entry, not a type) |
| `sub_821F9D00` | 136 | **SetViewport**(dev, viewport*): dirty bits 21..26, writes +0x3180 |
| `sub_82286CE8` | 63 | **SetRenderTarget**(dev, surface, surface): writes the RB_SURFACE_INFO shadow block (+0x2880) |
| `sub_82264590` | 42 | **SetDepthStencilSurface**-like (dev, surface, 0, 1280, 720, ...): writes +0x2880.., the depth-control shadow (+0x2934) |
| `sub_822869A0` | 210 | render-target-related setter, bit 20, writes +0x2E54.. (4 words) |
| `sub_8223B130` | 2,100 | not a device method (r3 = a 0x70xxxxxx object): a 3-word write helper |

**The first draw dump** (3 frames at frame 3000, 7,421 draws = 2,473 per
frame: 6,014 DrawIndexedVertices, 1,056 DrawVertices, 351 UP; primitives
TRIANGLESTRIP 5,375, TRIANGLELIST 1,899, POINTLIST 75, QUADLIST 72)
established the object layouts by reading them live (`peek.py`):

- **Fetch constants**: slots 0..19 are texture fetches (word 0 = `0x84000002`,
  type bits 2); slots 26..31 hold the **vertex fetch constants as 2-word
  pairs** (`address | 3`, `size/endian`), e.g. `1F3E2003/1000FA02` — so the
  vertex streams sit at the top of the 192-word block, as Xenos numbers
  vertex fetch constants (95 downwards). The dump now prints all six words of
  every slot and the report keys draws on every (address|3) pair.
- **Index buffer object** (device+0x3094 → e.g. 0x40C29508): +0 common word
  `0x20400002` (type 2 = index buffer), +4 refcount, +8 id, +0x18 **physical
  address** (`0xFD5BB180`), +0x1C **byte size** (0x36A = 437 indices × 2 —
  exactly the draw's count), +0x20 a code pointer, +0x24 end address.
- **Shader object** (e.g. 0x4D06EF50): +0 `0x00400006` (type 6 = vertex
  shader; 7 = pixel), +0x20 physical microcode base (`0xFD619280`), +0x30 a
  register-count word, +0x44.. (offset, size) pairs, +0x380 the 8-byte
  variant entries `SetShader` receives a pointer to; the variant record at
  obj+[entry] carries the microcode offset (+872), length (+876), register
  counts (+880..888) and the constant list (+892) — the "table" the loader
  walks is rec+872. Microcode lives in **physical** memory
  (`physical_membase + (addr & 0x1FFFFFFF)`), which the dump now reads.
- The XDK resource common word's low nibble is the resource type (index
  buffer 2, vertex shader 6, pixel shader 7) — the reference's
  `D3DCOMMON_TYPE` values.

## M4-a — the shadow window renders (2026-09-15 14:51)

`fable2recomp/src/native_gpu_present.cpp` (branch `native-gpu`): with
`ngpu_shadow=true` the Present hook creates a second window on the game's
render thread, a Plume D3D12 device + swap chain on it, and clears/presents
once per guest Present (message pump in the same hook). Result in the market
run: `[ngpu] shadow window up: Plume D3D12 device + swap chain (1262x673)`,
then `3000 native frames presented` in lockstep with the guest at **59 fps —
the same fps as without it** (the per-frame fence wait costs nothing
measurable at this frame rate). Plume (`NativeGPU/build/plume/plume.lib`,
MSVC /MD) links into the clang++-built executable without CRT conflicts;
CMake turns the backend on when the library exists (`FABLE2_NATIVE_GPU`).
The run then died in the draw dump (an unmapped guest read on the render
thread), fixed by page-checked reads.

## Next

1. Draw dump 2 with the corrected fields (six words per fetch slot, index
   buffer address/size, the device's own vs/ps at +0x3198/+0x3194, the shader
   containers from obj+872 + physical memory) → the object-key match rate
   and the shader set of one market frame.
2. M4-b: one DrawIndexedVertices drawn natively in the shadow window — the
   VB/IB bytes from guest memory (vertex fetch pair at device+0x778, the IB
   object's physical address), one shader pair translated with XenosRecomp
   from the dumped .xvu files, constants from device+0x780.
2. Name the remaining entry points from the census (Create*/Lock*/Unlock*),
   then the draw dump (VB, IB, shader pair, start/count per draw) at the draw
   entry points — also what the NG2 frame-interpolation work needs.
3. Present → Plume swap chain, Clear, one DrawIndexedPrimitive natively.
