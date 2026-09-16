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

## Draw dump 2 — object identity in Fable II (`M4_fable2_dump2.md`)

Two consecutive market frames (3000, 3001; 2,411 + 2,410 draws):

- **The index buffer's physical address is a 100 % stable object key**: 566
  distinct (ib, vs, ps) keys per frame, every one of them present in the
  next frame. (The vs/ps fields of that dump were the device words
  +0x3198/+0x3194, which turned out not to hold the current shaders — mostly
  0 — so the match is the IB alone; the next dump carries the tracked
  SetShader pair.)
- **The vertex streams 0..2 are per-frame**: the pairs in fetch slot 31
  (device+0x768..0x77F) change address every frame (a ring the engine
  writes each frame), so a key that includes every vertex fetch pair
  matches 0 %. Slots 26..30 hold `00000001/00000000` filler. For the native
  port this means vertex data for those streams is re-uploaded per frame
  (as the emulation does today) unless the ring's content is hashed; the
  static geometry identity is the IB.
- What else changes per frame at the same draw: `rt` (the render target
  alternates: double-buffered), `vc` (the vertex constants c0..c15 hash —
  so per-frame view/projection constants DO go through the device shadow;
  identical between different objects in the same frame, so the per-object
  transform is elsewhere: another register range or the shader's own
  literals), texture slots 4/5 (per-frame textures: shadow/previous frame),
  and slot 16/17 for some draws.
- 29 distinct shader pairs in the frame (by the flawed field; the count of
  distinct SetShader objects in the S lines is 41 vertex shaders, whose
  containers were written as `.xvu`), 371 IBs drawn more than once (max
  161 times — instancing by repetition, e.g. foliage), 50 point-list, 48
  quad-list, 1,243 list and 3,480 strip draws.

## The shaders translate — XenosRecomp on the dumped containers (15:00)

XenosRecomp (built with clang-cl: its `pch.h` uses `__builtin_bswap*`;
`NativeGPU/build_xenosrecomp.cmd`) in single-file mode turns a dumped `.xvu`
into HLSL: `XenosRecomp <file>.xvu out.hlsl XenosRecomp/shader_common.h`.
The first Fable II vertex shader (`4CDF70E0_v.xvu`, 1,756 bytes) gave 12 KB of
HLSL and settled three things:

- **The containers carry reflection data**: the constant table names the
  registers — `float4 g_WorldViewProjection[4] : packoffset(c0)` — so the
  translator's constant-buffer generation works on Fable's shaders as is,
  and c0..c3 is the view-projection.
- **Per-object transforms travel in a vertex stream, not in constants**: the
  input declaration has `POSITION1`, `POSITION2`, `POSITION3` (three float4
  rows = a per-instance world matrix) besides `POSITION0`/`NORMAL0`/
  `TEXCOORD0..2`/`BLENDINDICES0`/`BLENDWEIGHT0`. That is why the vertex
  fetch pairs of streams 0..2 change every frame (the engine writes the
  instance rows into a per-frame ring) and why the c0..c15 hash is the same
  for different objects in one frame. For the NG2 frame-interpolation work:
  Fable's object transform is in the vertex data of the per-frame stream,
  not in the constant registers.
- All 38 containers run through the single-file mode: **30 translate, 8
  crash or hang** the recompiler (`4CE926A0 4CFEE5D0 4D051540 4D052F90
  4D0608E0 4D088BE0 4D097CB0 4D0A9E70`; a hang is an infinite loop in its
  control-flow handling — kill the process). The 30 HLSL files
  (`NativeGPU/build/fable2_shaders/hlsl/`) name Fable's vertex constant
  layout: `g_WorldViewProjection[4]` c0 (all 30), `g_WorldTransform[3]` c4
  (14 — so a constant world transform exists too, for non-instanced
  meshes), `g_WorldPositionAndReciprocalScale` c7 (15), `g_EyePosition` c9
  (10), `g_RepeatedMeshConstants` c13 (5), `g_TreeConstants[3]` c15 (5),
  `g_AmbientLightReferenceDirection` c19 (11), `g_PRTConstants[12]` c28
  (8). Vertex inputs: `POSITION0` 29, `POSITION1..3` 19–21 (the instance
  rows), `TEXCOORD0` 20, `NORMAL0` (uint4, packed) 18, `TEXCOORD1` 11,
  `TEXCOORD2` 9, `BLENDINDICES0` 7, `POSITION4` 5, `COLOR0/1` 5,
  `BLENDWEIGHT0` 3.
- Translator gaps seen so far: the instance inputs are emitted four times
  (a duplicate-declaration bug on this vertex declaration shape — Sonic never
  had it); `4CE926A0_v.xvu` crashes the recompiler; one shader makes it loop
  forever (killed after 290 s CPU). 11 of the first 13 translated. The
  directory mode (dxc → DXIL/SPIR-V cache) produced nothing: it needs the
  dxc DLLs beside the executable — to sort out when a shader pair is
  compiled for the first native draw.

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

## Draw dump 3 — vertex declarations, pixel shaders, the first native-draw run (15:38)

- **Vertex declaration object** (device+0x2E2C, common word type 5, e.g.
  0x42127EA0 / 0x422280E0 / 0x43107C50): element count at +0x18, elements
  from +0x34 as three words each — `stream << 16 | offset`, the Xenos format
  word (format in the low 6 bits: 0x39 = 32_32_32_FLOAT, 0x26 =
  32_32_32_32_FLOAT, 0x25 = 32_32_FLOAT, 0x21 = 32, 0x1A = 16_16_16_16),
  `usage << 16 | usageIndex << 8` (D3DDECLUSAGE: 0 POSITION, 3 NORMAL, 5
  TEXCOORD). Seen: `{POSITION0 float3 @0, TEXCOORD0 float2 @12}` (stride 20),
  twelve float4s at 0x10 steps (POSITION0, NORMAL0, TEXCOORD0..9), and the
  instanced shape `{POSITION0 float3 stream 0 @0, POSITION1 stream 1 @0
  (fmt 0x21), POSITION2/POSITION3 float4 stream 2 @0/@0x10}` — stream 2 is the
  32-byte instance-row stream whose stride byte (device+0x30F0+2 = 8, in
  dwords) is the only non-zero one: the mesh streams' strides come from the
  declaration, not the device. Many draws have device+0x2E2C = 0 (their
  declaration is bound elsewhere; to find).
- **Pixel shader objects** (common word type 7, e.g. 0x43178550) keep their
  container at **+0x28** (magic 0x102A1100) and the physical part at
  **+0x18**; vertex shader objects at +872 / +0x20. 281 SetPixelShader vs
  3,453 SetVertexShader calls per two frames; the tracked (vs, ps) pair per
  draw is now in the `D` lines.
- Native draws run 1: the shadow window rendered (topmost, captured), the
  pipelines and the 96 MB upload buffer came up (`native draws ready`), the
  game held 56–58 fps, but every draw was skipped: the stream chosen by the
  cvar (1) has stride byte 0. The index data read through the physical
  mapping is right (`2d8e 2d8f 2d90 ffff ...` — 16-bit indices with 0xFFFF
  strip restarts, which Plume's strip pipelines cut; the native path expands
  strips to lists anyway). Run 2 reads the declaration for POSITION0's stream,
  offset and stride.

## Native draws run 2 (15:48) — the first native triangle, and where the layout lives

With POSITION0's stream/offset/stride taken from the declaration object,
**one draw per frame rendered natively**: a dark wedge in the shadow window
(captured), drawn from the game's own index and vertex bytes through its
own c0..c3 — the first geometry of the native path. The other ~1,950 draws
per frame were skipped as "without a declaration": device+0x2E2C is 0 for
them. That is the XDK's normal case: the vertex declaration is **bound into
the vertex shader** (`SetVertexShader` alone selects the layout), and the
shader's container lists its vertex elements as `{vfetch instruction
address:12, usage:4, usageIndex:4}` (XenosRecomp's `VertexElement`); the
vertex fetch instruction at `code + address·12` (3 big-endian dwords) names
the fetch constant (`constIndex·3 + constIndexSelect` → stream = 95 − k),
the format, the offset and the stride (dwords). Run 3 resolves POSITION0
from the current vertex shader that way (`PositionFromShader`, cached per
shader object).

## Native draws run 3 (15:53) — the fetch instruction is found by shape

Resolving POSITION0 through the container (`code + element.address·12`)
found nothing: the runtime's physical block at [obj+0x20] does not keep the
container's offsets (its first words are already ALU instructions, no
0x40-byte header). Scanning the block for vertex-fetch-shaped rows (dword 0:
opcode 0 and the must-be-one bit 19; dword 1: format bits 16..21; dword 2:
stride and offset in dwords) found, for `4CDF70E0`, exactly the declaration
the `VD` lines showed for that shader's mesh class: `k = 95 → stream 0,
float3, stride 5 dwords (20 B), offset 0` (POSITION0) followed by `float2,
stride 5, offset 3 dwords` (TEXCOORD0) — at +0x224 of the block, not at
0x40 + 9·12. Run 4 takes the first float3/float4 fetch row in the block as
POSITION0 (cached per shader object).

## Native draws run 4 (16:00) — Fable's mesh vertices are half-float

The float3/float4 fetch scan found POSITION0 in 16 of 211 dumped vertex
shaders only. Scanning every fetch format shows the real mesh layouts:
the common Fable II vertex is **28 bytes: POSITION0 as 16_16_16_16_FLOAT
(format 32) at offset 0, two 8_8_8_8 (normal/tangent) at dwords 3 and 4, a
16_16_FLOAT texcoord at dword 5**, in stream 0; variants with 36-byte
(stride 9) and 24-byte (stride 6) vertices, and the instanced classes with
POSITION1 in stream 1 and POSITION2/3 as float4 in stream 2. Run 5 accepts
format 32 (big-endian halfs in memory order) for POSITION0. About half of
the shaders show no fetch row at all with this filter (tiny post-process
shaders that fetch nothing, or a fetch encoding the filter does not cover).

## Native draws run 5 (16:05) — the draws ran, the frame rate did not

With half-float positions accepted, 9 shader classes resolved and the native
draws started — and the game dropped to 1.7 fps: the draw path converted
vertices 0..max_index for every draw, and Fable's draws index into large
shared buffers, so each draw converted tens of thousands of vertices. Run 6
converts only min_index..max_index and rebases the indices.

## Native draws run 6 (16:31) — still 1.3 fps: the write-combined upload heap

Converting only the indexed range did not help (1.3 fps). The remaining
per-draw cost was reading the index buffer back out of the UPLOAD heap: the
rebase pass (`idx[i] -= min_index`) read write-combined memory, and every
uncached read is a bus transaction — 2,000 draws × ~1,000 indices per frame.
Run 7 assembles the indices in ordinary memory and copies them to the upload
heap once (vertices were already written sequentially).

## Native draws run 7 (16:36) — still 1.6 fps; instrumenting

Assembling the indices in ordinary memory changed nothing (1.6 fps), so the
cost is not the upload heap either. Run 8 carries per-frame timers (index
build, vertex conversion, the Plume calls, the shader/device lookups, the
execute + present + fence wait) logged every 60 frames as `[ngpu] frame
cost`, to name the ~600 ms before guessing again.

## Native draws run 8 (16:42) — the timers name it: reading the index buffer

`[ngpu] frame cost: 137 draws; index 255.4 ms, vertices 0.2 ms, plume calls
0.2 ms, other 0.0 ms, execute+present wait 0.1 ms`. Everything is free except
reading the game's index bytes: ~1.9 ms per draw for a few hundred indices,
through the physical arena (`physical_membase + (addr & 0x1FFFFFFF)`), while
the vertex bytes read the same way cost nothing — so the index buffer pages
are special (a watched/handled mapping, most likely the plugin's GPU-memory
tracking, faulting per access). Run 9 reads the indices through the guest
virtual address the game hands the GPU (0xFDxxxxxx) with one memcpy into
ordinary memory.

## Native draws run 9 (16:44) — **the market renders natively** (M4-b reached)

Reading the indices through the guest virtual mirror halved the cost
(`138 draws; index 133.9 ms`) and the game ran at 6.8 fps — enough to see
it: the shadow window shows the market street's walls, floor and the
buildings ahead as flat-shaded surfaces in correct perspective, drawn from
the game's own index and vertex buffers through its own view-projection.
143 of ~1,950 draws per frame render (the 9 shader classes whose POSITION0
fetch the scan resolves: 28-byte half-float vertices in streams 0/1/3,
36-byte and 20-byte variants); 1,469 are skipped because their vertex
shader's dumped physical block contains no fetch-shaped row at all (e.g.
`4D05F5D0`, `4D063B40` — the market's main mesh shaders: their microcode is
evidently not the block at obj+0x20 but a **variant** the `SetShader` entry
pointer selects, the next thing to resolve), and 344 tripped the stream
size check (relaxed in run 10).

The remaining per-draw cost is the read of guest memory the host maps
write-combined: run 10 copies index and vertex bytes with `MOVNTDQA`
streaming loads.

## Native draws run 10 (16:48) — streaming loads changed nothing

`175 draws; index 165 ms` with MOVNTDQA copies (5.4 fps): the guest memory
is not the slow part. What is left in that section is two `VirtualQuery`
calls on the 0xFD.. mirror per draw; the vertex path makes the same two
calls on its pages for 0.2 ms per frame, so the mirror's address range must
be a fragmented VAD (per-page protections from the plugin's memory watches)
that makes each query expensive. Run 11 drops the index-buffer page queries
(the object is a live guest buffer whose bounds the IB object states).
The captured frame (16:48) shows the street again — walls, floor, the
building face ahead — from 175 native draws.

## Native draws run 11 (16:54) — it is the pages, not the queries

Without the page queries: `173 draws; index 166 ms` (5.7 fps) — identical.
Every way of reading the index buffers costs ~1 ms per KB (physical arena
in run 8, virtual mirror in run 9, streaming loads in run 10, no queries in
run 11), while the vertex buffers read through the same mappings cost
0.2 ms per frame. So the index buffers' **pages are watched**: ReXGlue's
memory tracking (the GPU plugin's guest-page watches for uploads and
readbacks) protects them and every host access takes the fault handler —
tens of microseconds each, ~64 per KB. The vertex pages of these meshes are
not watched. The native path must read watched guest memory through the
runtime's own access route (the memory-watch API, or a copy the plugin
already holds) instead of raw host loads — the next engineering item.

## Native draws run 12 (17:55) — variants resolve; the mirror is the slow path

- `TriggerPhysicalMemoryCallbacks` on the index range found nothing to
  resolve (`other 0.1 ms` per frame) and the index cost stayed at ~150 ms.
  The difference to the vertex copies (0.5 ms per frame for more bytes) is
  the mapping: the index object's address is in the 0xE0..0xFF virtual
  mirror, which the runtime serves as an MMIO-style range, while the vertex
  fetch addresses are plain physical addresses read through the physical
  arena. Run 13 reads both through the physical arena.
- **Shader variants work**: with the flush's entry pointer, POSITION0 is
  found in the variant's microcode — 17 shader/variant classes resolve (was
  9), the same shader object giving different layouts per variant
  (`4D0570D0`: stride 20 in stream 0 with one entry, stride 28 in stream 1
  with another). 47 remain unresolved (the scan needs the block bounds
  logged, which run 13 adds).

## Native draws run 13 (18:15) — the physical arena is just as slow: re-armed watches

`207 draws; index 189 ms, vertices 45.7 ms` (3.9 fps): reading the index
buffers through the physical arena costs the same ~1 ms per KB as through
the mirror, and the vertex copies grew expensive too once every draw's
whole vertex span was copied (the stream size check was dropped). The
watch trigger reported nothing and cost nothing. Everything points at the
pages themselves: they are **data-provider watched** (GPU-written memory,
the plugin's "readback landed quietly" machinery), and such watches re-arm
after each access, so every 16-byte load takes the fault handler again.
Run 14 probes it (the trigger's return value, `QueryProtect` on the range
and the copy time alone, logged as `[ngpu] ib probe`), with a cvar to
trigger the watch as a write.

## The index pages: the plugin's on-demand readback provider (18:25)

`rexgpu-xenos` registers a data provider (`ResolveDataProvider`) and enables
provider watches on every range a resolve writes; a touch from any thread
but the GPU worker becomes `CallInThreadSafe` + a wait for the worker to
land the readback, and while the resolve's submission is still open the
page stays no-access — so a host copy of Fable's per-frame index data
(which shares those pages) pays a cross-thread round trip per 64-byte line.
The fix for the shadow path: release the provider watch on exactly the
index range before copying (`Memory::DisablePhysicalMemoryDataProviders`,
cvar `ngpu_ib_unwatch`). The installed SDK headers (RexBlue, Sep 3) predate
that method while the runtime DLL (Sep 14, built from the rexglue-src
working tree) exports it, so the call is bound by its decorated name at
runtime — a reminder that the SDK headers and the shipped runtime have
drifted apart since the plugin work began.

## Native draws run 15 (18:30) — the copy is not the cost either

With the provider watch released before each copy and the copy timed on
its own: `211 draws; index 192.7 ms (copy 0.1 ms, 0 draws had a watch),
vertices 45.8 ms`. The guest-memory copy of the indices costs 0.1 ms per
frame; the watch trigger found no watched page; and yet the index section
still takes ~190 ms. What remains in that section is the index decode /
rebase in ordinary memory and the memcpy of the result into the Plume
upload heap; the vertex section similarly ends in 4-byte float stores into
the upload heap. Run 16 times those pieces separately — the working
hypothesis is now the upload heap mapping itself (uncached or
write-combined writes at word granularity).

## Native draws run 16 (18:38) — found: VirtualQuery on the physical arena

Fine-grained timers: `index 177.5 ms (disable 0.0, copy 0.1, decode 0.2,
upload memcpy 0.0), vertices 44.4 ms (copy 4.6, convert+store 39.9)`. The
index bucket's parts add up to 0.4 ms; the ~177 ms was the part of that
bucket I had not timed — the two `VirtualQuery` page checks on the
**vertex** range (the bucket boundary sat after them). The runtime's
per-page protections cut the physical arena's address space into thousands
of regions, so each query costs ~0.4 ms there, while the same query on the
index buffer's mirror was cheap (which is why removing those in run 11
changed nothing). The provider watch, the memory type and the copies were
all innocent: the "1 ms per KB" was 2 queries per draw. Run 17 remembers
verified pages in a bitmap (one query per page, ever). The remaining real
cost is the half-float conversion + upload stores at ~40 ms per frame for
~400k vertices — to be replaced by uploading the raw 16-bit vertex data and
declaring R16G16B16A16_FLOAT input (only the byte order needs a pass).

## Native draws run 17 (18:42) — 38 fps with ~300 native draws per frame

With the page checks cached: `294 draws; index 6.9 ms (decode 0.4), vertices
9.0 ms (copy 6.3, convert+store 2.7), plume calls 0.1 ms, execute+present
wait 0.9 ms` and the game at **38.6 fps** (from 4.4) while the emulated path
still renders the real frame. ~300 draws per frame now render natively
(the variant resolution keeps adding shader classes). The native path costs
~17 ms per frame here, almost all of it copying whole vertex spans out of
guest memory and the residual page checks; run 18 uploads half-float
positions as 16-bit data (R16G16B16A16_FLOAT input, byte swap only).

## Native draws runs 18-19 (18:45-19:05) - the copies are bandwidth, not conversion

Run 18 (16-bit-float positions uploaded raw, R16G16B16A16_FLOAT input):
still 38-39 fps, `vertices 9.2 ms (copy 5.8, convert+store 3.4)` - the
per-draw path copies ~96 MB of vertex spans per frame (the "upload" skips
are the 96 MB frame heap running out), so the cost is memory bandwidth on
data that never changes. Run 19 added a persistent vertex/index cache
invalidated by the runtime's physical-memory write notifications
(`RegisterPhysicalMemoryInvalidationCallback` +
`EnablePhysicalMemoryAccessCallbacks`, exported by rexruntime.dll, missing
from the installed headers) but never engaged: every draw reported "no
vertex buffer object" because the object at device+0x30AC+4*stream keeps
its Xenos fetch constant, not a byte size - +0x18 = address | type,
+0x1C = size in dwords << 2 | endian (0x10004E42 = 5008 dwords, the
stream's fetch address 4 KB past the object's). Run 20 decodes it.

Run 18's dump settled the draw classes of a market frame (1,947 indexed
draws): vs 4CEE9520 with variant 4CEE9AE0 draws 585 of them, 4CE085B0
without a variant (a 108-byte block) 206, 4D058010/4D05A500 146. The
`.var.xvu` variant containers (153 written) translate 118/153 with the
recompiler (35 crash it) and compile 0: the variant block's layout is not
the object container's (literals + code at the header's offsets) - the
dumped blocks start with the previous allocation's tail, a heap header
(`ABCDABCD`, sizes, guest pointers) and the literals before the control
flow program (+256 in 4D067220/4D067FC0), so the patched header points
the recompiler at garbage. `tools/native_gpu/fetch_rows.py` lists the
vertex fetch rows of any container; run 20 dumps the raw variant entry and
header records (`VE`/`VH` lines) to settle the layout.

## Runs 20-21 (19:05-19:28): the pixel-shader "variant" and the host page offset

Two readings that every earlier run rested on were wrong.

**The shader-load flush is SetShaders(dev, VS, PS).** Run 20's dump printed
the raw "variant entry" records (`VE` lines): word 0 = `00400007` - the
common word of a **pixel shader object** (type nibble 7, container at +0x28
with flags 0x102A1100, physical block at +0x18). sub_82221858's second
path loads that pixel shader (IM_LOAD of `[ps+0x18] + Shader.physicalOffset`
with `Shader.size` bytes, the header at `container + shaderOffset`, type
bit 1) and then the vertex shader the same way from `[vs+0x20]` (type 0).
There are no vertex-shader variants: the per-(shader, entry) layouts, the
`.var.xvu` containers and the "entry shared by several shaders" puzzle were
all the pixel shader. The recompiled flush (generated/default/
fable2_recomp.254.cpp) is the reference.

**The 0xE0.. mirror sits one page higher on the host.** The runtime maps the
0xE0000000 range at file offset 0x100001000 but Windows' 64 KB mapping
granularity rounds the 0x1000 away, so every guest access adds it back
(`rex::memory::detail::PhysicalHostOffset`, used by the recompiled code and
by `GuestPtr`). The game does the same when it forms GPU addresses:
SetStreamSource, DrawIndexedVertices and the shader-load flush all compute
`(addr & 0x1FFFFFFF) + (addr >= 0xE0000000 ? 0x1000 : 0)`. So:

| address form | held where | host pointer |
| --- | --- | --- |
| CPU virtual (0xFD..) | IB +0x18, VB +0x18, VS +0x20, PS +0x18 | `virtual_membase + addr + 0x1000` (`Host()`) |
| GPU physical | fetch constants, ring packets, `Memory::GetPhysicalAddress(virt)` | `physical_membase + addr` (`Phys()`) |

Runs 9-20 read index buffers, shader blocks and the dumped `.xvu`
containers through `physical_membase + (addr & 0x1FFFFFFF)` - 4 KB too
low. Run 21's probe made it visible: the index buffer read that way holds
`3C000000 00000000`, read through the runtime's own physical number it
holds `00000001 0002FFFF` (a 0,1,2,restart strip); the vertex data read
through the fetch constant matches the runtime view. The clean quads of
runs 9-19 were meshes drawn with a neighbouring buffer's indices, the
"vertex fetch rows" found in pixel-shader blocks were the vertex shader
allocated one page below, and the 15 pixel shaders that compiled were
neighbouring objects glued to the wrong container.

Fixes (build 34): `Host()` for every object-held address (index copy, the
scanner's block, the dump's `.xvu` physical part), the scanner reads the
vertex shader's own block (`[vs+0x20] + physicalOffset`, `size` from the
container's Shader header, one layout per shader), the cache reads and
watches through `GetPhysicalAddress` (bound by decorated name) and matches
the stream's fetch address against the object's address + 0x1000. Runs
19-21 with the cache partly engaged stayed at 37-40 fps (their "range" and
"bypass" failures came from strides taken off the wrong block).

## Run 22 (19:28) - M4-c: every indexed draw native at the game's 60 fps

Build 34 (host page offset, the vertex shader's own block, the cache
through the runtime's physical numbers): the market walk runs at
**58-60 fps** (the cap; 38 fps in runs 17-21, 4 fps in run 15) with
**~1,815 native draws per frame - every DrawIndexedVertices, none
skipped** (`0 skipped ... 1815 without a declaration`), all 31 vertex
shaders resolved on their first sight (`(ok)` 31, `(not found)` 0). The
native path costs about 1.7 ms per frame: `index 1.1 ms (decode 0.6),
vertices 0.4 ms, plume calls 0.1 ms, execute+present wait 0.1 ms`;
the cache serves ~2,460 hits per frame from 26 MB (1,292 entries), ~650
draws per frame bypass it as dynamic (rewritten within 300 frames) and
take the per-draw path, 33 fall outside their buffer's size.

The shadow window shows one flat colour: with real indices the
post-process and UI passes' full-screen quads cover the scene (depth
LESS_EQUAL, no render-target separation). Run 23 skips draws under 12
indices (`ngpu_min_indices`) to look at the world.

## Resume checklist (state at 20:10, 2026-09-15)

- Branches (not pushed): `fable2recomp` `native-gpu` (code: src/native_gpu_present.cpp,
  native_gpu_dump.cpp/.h, native_gpu_trace.cpp, ngpu_shaders/, tools/ngpu_shaders.cmd, src/xxhash.h),
  `ng2recomp` `native-gpu` (tools/native_gpu/*, docs/native_gpu/*). XenosRecomp local branch
  `fable2` in NativeGPU/reference/XenosRecomp; Plume patched in NativeGPU/reference/plume (working
  tree, no git identity there); `NativeGPU/fable2_shader_common.h`.
- Build: scratch `build_renamed.cmd <tag>` (tools/build.cmd; log out/build_tu1_<tag>.log, look for
  `=== Done`); shaders: `tools
gpu_shaders.cmd` (absolute path) -> ngpu_vs/ngpu_ps/ngpu_ps_xs.dxil
  beside the exe. Recompiler: `NativeGPUuild_xenosrecomp.cmd`; Plume: `NativeGPUuild_plume.cmd`.
- Shader cache: run the game with `ngpu_dump_at_frame=3000` once (writes ngpu_shaders/<obj>_v|p.xvu
  on first sight), then `XENOS_COMMON=NativeGPU/fable2_shader_common.h translate_all.sh <ngpu_shaders>
  <out> "*.xvu"` and `pack_cache.py <out> <exe dir>/ngpu_cache`. The runtime hashes live containers
  (XXH3) and loads `<hash>_v.dxil` + `.layout`.
- Run: scratch `market_fps.sh <tag> "ngpu_trace=true;ngpu_shadow=true;ngpu_native_draws=true"`
  (hero 2 market walk, 150 s); watch `[ngpu] xs:`, `[ngpu] cache:`, `[ngpu] frame cost`,
  `[swap] guest fps`; the shadow window is at screen region 48,40 1264x712.
- Cvars: ngpu_shadow, ngpu_native_draws, ngpu_xs (translated shaders), ngpu_cache/_mb, ngpu_rt_last
  (scene pass by episode), ngpu_wvp_only, ngpu_min_indices, ngpu_depth_test, ngpu_reverse_z,
  ngpu_const_mode, ngpu_max_draws, ngpu_dump_at_frame/_frames.
- Next: M5-b = translated pixel shaders (PS container hash the same way: container +0x28, block
  +0x18; textures from the 32 fetch constants at device+0x480 - untile with the SDK's
  rex/graphics/pipeline/texture/util.h helpers, DXT pass-through; samplers; PS constants from
  device+0x1780; SharedConstants descriptor indices in the 8-float4-per-dimension layout), then
  render targets / resolve, blend + depth states from the D3DRS shadow, and the DrawVertices /
  DrawVerticesUP paths.

## Runs 34-43 (20:24-21:15) - M5-b textures: data right, one slot wrong

Builds 46-54: a texture cache fed from the 32 fetch constants at
device+0x480 (dword 1: format, endian, base page; dword 2: size; dword 5
bits 9-10: dimension - dword 4 has none), Xenos 2D untiling in blocks
(xenia's TiledOffset2DOuter/Inner, ported), DXT1/3/5 -> BC1/2/3 and
8_8_8_8, level 0 only, staged in the frame heap (256-byte pitches, 512-byte
offsets) and copied before the draw; descriptor set 0 holds 4,096 slots;
the shared-constants block carries each fetch slot's descriptor index and a
wrap/clamp sampler index. Findings, in order:

- `ngpu_dump_textures` + `tools/native_gpu/texture_decode.py`: the runtime's
  untiled rows decode to the real market atlas (barrels, windows, roof
  shingles) - untiling and the 8in16 swap are right.
- `ngpu_ps_debug` (the flat pixel shader painting interpolants or sampling a
  slot): TEXCOORD0.xy is smooth, slot 0 sampled with it shows correctly
  textured houses, so coordinates + textures + sampler + descriptor indices
  all work through my own pixel shader.
- The game's material shader (hash 52FE118F450D1448, ~750 of ~800 translated
  draws per frame) still renders noise. Its samplers: diffuse
  `g_BackgroundDiffuseTexture` at fetch slot 13 (packoffset c3.y), lightmap
  `g_TextureLightmapSampler` at slot 3, fog LUTs `g_InScattering` /
  `g_Extinction` at 4 / 5 (1D, served white). Its pixel constants are live
  (70 of 224 non-zero). With `ngpu_tex_slots=3` (slots 0-1 only) the noise
  is gone, so it comes from slot 3 or 13 (runs with masks 8192 and 8 split
  them).
- **Resolved (21:45):** the noise is fetch slot 13 (`g_BackgroundDiffuseTexture`,
  register 13 = Fable's main-texture register in most pixel shaders) whose
  texture for the walls is a **640x360 8_8_8_8 GPU-written texture** - the
  shared plugin's own diag calls it `640x360 fmt 6 GPU-written
  READBACK-PENDING`. Fable bakes material textures / its light buffer on
  the GPU at load and reads them back; guest memory holds the emulated
  GPU's readback, not a tiled texture, so my untiler makes bands of it and
  the walls sample noise. The DXT atlases the engine binds at slot 0 are
  not what the material shader samples. Confirmed by `ngpu_ps_debug=9`
  (slot 13 through my own pixel shader: same noise) and `ngpu_tex_slots`
  (slot 13 alone: noise; slots 0/1/3 alone: clean). Build 59 leaves
  screen-derived sizes (1280x720, 640x360, 320x180, 160x90) white; the real
  fix is M6: the native path owning the game's render targets and resolves.
- Interpolator packing: the vertex shader exports the lightmap coordinate
  as `oTexCoord5.xy` while the pixel shader reads `iTexCoord0.zw`; the
  interpolator tables carry a component mask per entry (bits 12-15 of the
  word: VS TEXCOORD0 = xy, PS TEXCOORD0 = xyzw), which the recompiler
  ignores. Whether hardware packs o5.xy into interpolator 0.zw is open.
- Signed 2_10_10_10 attributes (TEXCOORD1, `comp` = 1) now arrive as the raw
  dword in a float input and are unpacked in the shader
  (`unpack2_10_10_10_snorm`), since D3D12 has no R10G10B10A2_SNORM.

## Run 33 (20:18) - M5-b (placeholders): the game's own pixel shaders run too

Build 45 (`ngpu_xs`): translated pixel shaders (container at object +0x28,
block at +0x18, hashed like the vertex ones -> `ngpu_cache/<hash>_p.dxil`)
pair with the translated vertex shaders in per-pair pipelines; four
descriptor sets stand in for the shaders' register spaces 0..3 (Texture2D /
Texture3D / TextureCube / Sampler heaps) with a 1x1 white texture and a
default sampler at index 0 - the index every zeroed shared constant
selects - and the 224 pixel float constants come from device+0x1780 per
draw. Run 33: **944 of 945 translated draws use the game's pixel shader**
(7 pixel shaders loaded, 1 not cached, 43 vertex+pixel pipelines), 1.7 ms
per frame, 59 fps, no crash; with white textures the lit surfaces come out
white and the rest keeps the probe path's flat colours. Plume's
`createGraphicsPipeline` leaves `D3D12GraphicsPipeline::d3d` null when
D3D12 refuses a state object, so the runtime checks that member (via
`plume_d3d12.h`, which needs the D3D12MemAlloc include path) and falls
back to the flat pipeline. Next: real textures from the 32 fetch constants.

## Runs 28-30 (19:55-20:02) - M5-a: the game's own vertex shaders drive the native draws

Build 40-42 (`ngpu_xs`): XenosRecomp's translations are packed by container
hash (`tools/native_gpu/pack_cache.py` -> `ngpu_cache/<XXH3>_v.dxil` +
`.layout`), the runtime hashes each vertex shader's live container the same
way (virtual part + physical block through the host page offset), builds a
pipeline from the sidecar's fetch list (semantic, stream, offset, format ->
input elements; the recompiler now types normals by fetch format and
writes the declared input type per fetch, which the runtime checks against
the element format - run 29 crashed on a mismatch because Plume does not
check `CreateGraphicsPipelineState`), mirrors the whole vertex streams
into the persistent cache with the fetch constant's endian swap (8in32),
uploads the 256 vertex float constants per draw through a space-4 root
descriptor, and pairs the translated vertex shader with a flat
push-colour pixel shader (`ngpu_ps_xs.hlsl`).

Run 30: **788 of ~1,490 scene draws per frame go through the game's own
vertex shaders** (24 shaders loaded, 9 rejected: 4 sample textures in the
vertex stage, 4 fetch 8_8_8_8 integer texcoords into float inputs, 1 not
in the cache), 0.7 ms per frame, the game at 59-60 fps, and the window
shows the market drawn by the real shaders (arch, hero, houses, stalls,
cart). Fallbacks to the probe path: 362 draws whose streams have no
vertex buffer object (the per-frame ring streams: instance rows), 170 not
cached, 22 out of range. Runs 31-32 (builds 43-44) copy such streams per
draw into the frame's upload heap (base .. base + max index, swapped like
the cached ones): **947 draws per frame through the translated shaders**,
0 stream fallbacks, 179 draws of the 9 rejected shaders, 29 out of range,
still 60 fps. Plume gained `R10G10B10A2_UNORM` for the Xenos
2_10_10_10 attributes (`reference/plume`, rebuilt).

## Runs 23-26 (19:32-19:45) - M4-d: the market is recognisable in the shadow window

The flat colour of runs 22-23 was not a full-screen quad (`ngpu_min_indices`
changed nothing) nor a screen-space shader (every indexed draw goes through
a vertex shader with `g_WorldViewProjection` at c0: `ngpu_wvp_only` skipped
nothing) but the **shadow-map pass**: the frame's indexed draws fall into
two render-target episodes, 370 draws into surface 42620AB0 (the shadow
map, drawn first from the light's viewpoint - its casters cover the window)
and ~1,490 into a surface that alternates between frames (4319B710 /
4319A270, the scene). Matching the scene pass by *ordinal* (the episode
with the most draws in the previous frame; `ngpu_rt_last`) leaves the
1,488 scene draws, and run 26's window shows Bowerstone market: the stone
archway, the hero standing in the glow ring, the timber houses and stalls
on both sides, the cobbled street - each mesh in its flat per-draw colour,
at the game's 60 fps.

Learned on the way: the device's vertex constants are live per draw (709
distinct c0..c15 blocks over the 1,973 indexed draws of a frame), and the
natural `dot(v, cK)` order is right - the `.zxyw` swizzle the translated
shaders apply to `g_WorldViewProjection` pairs with the register
permutation they keep the position in (`r1 = (z, x, y, 1)`), so it cancels.
Run 27 (`ngpu_const_mode=1`, the swizzled variant) is the control.

## Shader translation, third pass (19:40) - 126 of 132 compile

The containers dumped by run 22 (read through the host page offset: 72
vertex, 60 pixel) translate 132/132 with no recompiler crash and compile
**126/132** with dxc (69/72 vertex, 57/60 pixel), from 15 this afternoon:

| fix | where | effect |
| --- | --- | --- |
| correct container bytes | `native_gpu_dump.cpp` (Host offset) | no crashes; 40 compile |
| `g_SpecConstants()` returns 0 (no permutations) | `NativeGPU/fable2_shader_common.h` | +68 (spec-constant externs need the lib profile) |
| 32 sampler slots per dimension + fallback `s<slot>` declarations; 1D fetches through the 2D heap at (x, 0.5) | XenosRecomp local branch `fable2` (0721ff9) | +8 |
| vertex stage samples level 0 (`XSAMPLE` macro: `SampleLevel` in vs, `Sample` in ps) | `fable2_shader_common.h` | +10 (vertex texture fetches: displacement, instancing) |

Left: integer / boolean constants the code uses without a definition
(`i0`, `i16`, `b129`, `b136` - loop counts and flow-control booleans the
game sets through SetVertexShaderConstantI/B; 4 shaders) and two shaders
using temporaries past the header's register count (`r32`, `r33`).
`translate_all.sh` takes the header from `XENOS_COMMON`; outputs in
`NativeGPU/build/fable2_run22d`.

The vertex shaders' constant tables (census of the 72): 57 have
`g_WorldViewProjection` at c0..c3, then `g_WorldTransform` c4..c6,
`g_WorldPositionAndReciprocalScale` c7, `g_EyePosition` c9,
`g_InstanceOffsetSize` c12, `g_RepeatedMeshConstants` c13, `g_TreeConstants`
c15..c17, `g_PRTConstants` c28..c39, `g_VertexAtmosphericParameters`
c108..c111, `g_KeyFrame*` c116..c175 (particles); the 15 without a c0 matrix
are the screen-space / sky / post shaders (`g_ScreenToViewportTransform`,
`g_ProjectToTexture`). Build 36's `ngpu_wvp_only` draws only through the 57.

## Shader translation, second pass (17:50) — the translator sees the wrong bytes for many shaders

XenosRecomp with duplicate vertex-input declarations removed (a local
change in `reference/XenosRecomp`): of 211 vertex + 54 pixel containers,
216 translate (49 crash the recompiler) and only **15 compile**. The
compile errors — registers r34..r52, constants c0/c12/c97 undeclared,
`iBinormal0` undeclared, "not all elements of SV_Position written" (75) —
are what decoding the wrong bytes produces. That matches the variant
finding: for most shaders the block at obj+0x20 is not the microcode the
game runs, so the `.xvu` files must be rebuilt from the variant record
(physical base [entry+24], code offset/length from entry+[entry+64]+40/44)
before the translator is judged.

## M4-b — native draws in the shadow window (built 15:18)

`native_gpu_present.cpp` with `ngpu_native_draws=true`: the DrawIndexedVertices
hook records every list/strip draw of the frame natively. Per draw it reads
the IB object (device+0x3094 → +0x18 physical address, 16-bit big-endian
indices from `start`), the vertex stream `ngpu_pos_stream` (default 1) from
its fetch-constant pair at device+0x778 − 8·stream (address | 3, size in
dwords | endian) with the stride byte at device+0x30F0 + stream, converts
the indices and the POSITION0 floats (offset `ngpu_pos_offset`) into one
96 MB per-frame upload buffer, and draws with hand-written test shaders
(`src/ngpu_shaders/ngpu_vs.hlsl` / `ngpu_ps.hlsl`, compiled to signed DXIL
by `tools/ngpu_shaders.cmd` with XenosRecomp's dxc): the vertex shader does
the four dp4s against c0..c3 read from device+0x780 (the game's own
g_WorldViewProjection, byte-swapped) as root constants, the pixel shader a
flat per-draw colour shaded by depth; D32 depth buffer, no culling. The
window is kept topmost (`ngpu_shadow_topmost`) so a monitor capture shows
it while the game runs behind. Expected: the non-instanced world geometry
(the 14 shader classes with `g_WorldTransform` in c4..c6 folded into c0..c3
by the engine, or not — to be seen) lands where the game draws it; the
instanced meshes (POSITION1..3 rows in the per-frame stream) draw at the
origin until the instance rows are applied.

## Runs 44-50 (21:45-22:30) - the noise is streamed texture memory, and the M6 function map corrected

**Noise diagnosis, continued.** The 640x360 GPU-written texture (run 46,
white) was only part of it: the walls, floor and pillars stayed noise with
it white, with the 1x1 textures white (`ngpu_tex_min=8`, run 48) and with
slot 13 fed from slot 0 (`ngpu_slot13=0`, mode 10). The dumped slot-13
textures decode correctly offline (512x512 DXT1 hair atlas, 128x128 glow),
so the untiler is right. **Mode 11** (build 64: the slot-13 texture laid
flat over the screen in screen space, `SampleLevel 0`, whatever the
texcoords) settled it: the wall textures' *content* is black with rows of
coloured garbage - the guest memory had not been written yet when the
texture was first seen. Fable II streams its world textures in after the
fetch constant exists (all census textures say `mips 0..3 packed`, base at
the fetch base, so the base level is expected there), and the texture
cache keyed by fetch constant kept the first, unloaded snapshot forever.
The Xenos plugin never shows this because its texture cache re-uploads on
guest writes.

**Fix (build 65, run 50):** every uploaded texture records its guest range
and the frame it was read in, arms the runtime's write watch on the range
(`EnablePhysicalMemoryAccessCallbacks`, the vertex cache's callback stamps
`g_page_tick`), and at the next lookup - once per frame - re-uploads when
a page is newer than the upload and the writes have settled for two frames
(or after 60 frames regardless). The old resource and view go to a retired
list freed at the next BeginFrame (after the frame fence); the descriptor
index is reused. The fetch constant's component swizzle (dword 3 bits
1..12, `0x688` identity for DXT, `0x60A` = BGRA for the 8_8_8_8 lookups)
now becomes a texture view mapping. `ngpu_tex_watch=false` disables it.

**Diagnostic branch bug:** modes 2-10 never bound the per-draw shared
constants (the static block stayed at b2), so mode 10 proved nothing;
fixed in build 64. Modes are now exact (`switch` on `int(color.a + 0.5)`):
11 = slot 13 flat, 12 = slot 0 flat, 13 = slot 13's descriptor index as a
colour.

**M6 function map, corrected by reading the recompiled bodies:**

| function | what it really is | evidence |
|---|---|---|
| `sub_822192E8` (580/frame, hooked as "SetRenderTarget") | **occlusion-query / sample-count issue**: writes RB_MODECONTROL, RB_SURFACE_INFO (0x20000), RB_COLOR_INFO 0, then `RB_SAMPLE_COUNT_ADDR` (0x2325) = `[obj+0x1C] + index*32` (mirror address, +0x1000 page trick); the "surface" objects are query objects (word 1 = 9, word 7 = a 32-byte record array at 0xFFA9Cxxx holding floats) | body in `fable2_recomp.193.cpp:630`; run 48 descriptor dumps |
| `sub_82206888` | **`D3DDevice_Resolve(dev, flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor, ClearZ=f1, ...)`** - the real XDK resolve: `flags & 7` = render-target index, `flags & 0x70` = source kind (0x10 depth), the current RT surfaces live at `dev+0x3098+4*i`, the depth surface at `dev+0x30A8`; the dest texture's fetch constant sits at `+0x1C` (dwords 0..5 at +28..+48: pitch/tiled, format+base, size, ..., mips) | `fable2_recomp.240.cpp:559` (writes RB_COPY_CONTROL 0x2318 / RB_COPY_DEST_INFO 0x231B) |
| `sub_82196750` (12/frame) | Fable's resolve wrapper: r5 = an engine object (`C0400002 00000001 ... FCC05040 00010000 820FB130 FCC15080`, two records) - not the XDK signature | run 47/48 dumps |
| `sub_821F0E00` | a **Clear**-family function (flags in r4: 0x10/0x20/0x40/0x80, RT index in r5, colour/z in f1; writes RB_STENCILREFMASK 0x210D) | `fable2_recomp.153.cpp:1269` |
| `sub_8221B010` | the pre-draw state commit (SQ_PROGRAM_CNTL 0x5C8 from the shader containers at +872), not a render-target setter | `fable2_recomp.243.cpp:5` |
| `sub_831F26D0` | the only other RB_COLOR_INFO (0x2001) writer - candidate for the engine's own render-target binding (Fable writes some PM4 itself: the `sub_82B9EEE0/F038` ring writers) | `fable2_recomp.154.cpp:34201`, to read |

The "render-target episodes" that ordered the passes (scene pass = the
episode with most draws) were therefore *occlusion-query boundaries*; they
still separate the shadow-map pass from the scene pass empirically, and
stay until the real RT binding is hooked.

Fetch-constant mip fields, verified against xenia's `xenos.h`: dword 4
bits 2..5 = mip_min_level, bits 6..9 = mip_max_level (the earlier "mips
3..0" print used the wrong bits); dword 5 bit 11 = packed mips, bits 12..
= mip address >> 12. The mip-chain layout rules (32-block-aligned power-of-
two mip pitches, 4 KB level alignment, the packed tail for levels <= 16
texels) are in xenia `texture_util.cc GetGuestTextureLayout` /
`GetPackedMipOffset`; mip uploads are still to do.

## Runs 51-52 (22:22-22:33) - the noise was the upload heap, not the textures

Run 51 dumped every large texture at first upload: the 1024x1024 DXT1 at
172B0000 decodes offline into a perfect market atlas (banners, wood,
awnings) from the very bytes the runtime staged, and the forced refresh
(`ngpu_tex_refresh=120`, 4,880 re-uploads) changed the noise pattern every
time without ever producing a texture. So the guest memory was right and
the GPU got something else. Cause: `SharedConstantsFor` took its 768-byte
block from the upload heap *before* resolving the draw's textures, and the
texture uploads it triggered staged their rows further up the heap - then
the function set `upload_used = block + 768`, rewinding the cursor below
the fresh rows; the next allocations (pixel constants, per-draw vertex
copies) overwrote them before the GPU executed the copy. Zero constants =
black rows, vertex data = coloured rows: the "black with noise rows" of
mode 11, and different garbage per refresh. Textures that happened to be
created early enough survived (the small character textures, the lightmap
atlases). Fix in build 67: gather the indices first, allocate the block
last. The write-watch re-upload (build 65) stays - it is the right thing
for streamed and dynamic textures - and the "streamed after first sight"
story of the previous section was wrong.

Run 52 (build 67, also the first with whole mip chains): no noise
anywhere. The scene is dark with blotchy blue/white patches on walls and
floor, which looks like the mip levels (level 0 near the hero is crisp,
distant surfaces black) - run 53 isolates it with `ngpu_tex_mips=false`.

## Runs 53-56 (22:33-22:50) - mips are innocent, render states and formats arrive

- Run 53 (`ngpu_tex_mips=false`) looked exactly like run 52, so the dark
  blotchy scene is shading, not the new mip chains (mip chains stay on).
- The draw census (`ngpu_dump_draws`, run 45) gives the material shader's
  slot layout: 0 = diffuse atlas (DXT1), 1 = DXT5/DXT1, 2 = **DXN** (49,
  normal map), 3 = 576x768 lightmap, 4 = 64x1 format 29 (1D), 5 = 256x1
  8888 (1D), 6/7 = 1024x1024 format 23 (shadow depth), 8 = 1280x720 format
  2 (k_8), 9/11/12/13/15 = 1x1 8888, 14 = 16x16. The translated material
  shader (`4CE80D10_p` = PS hash 52FE118F450D1448, 800+ draws per frame)
  reads `g_BackgroundDiffuseTexture` from shared-constant byte 52 = **slot
  13** and the lightmap from slot 3, in-scattering/extinction 1D LUTs from
  4/5; its output is `(diffuse^2 * (lightmap * ambient + light)) *
  brightness` plus fog. The dumped 1x1 at 1FC40000 is **all zeros** in guest
  memory - a GPU-written texture (resolve target; the plugin's readback is
  served on demand, never landed for a raw host read), so these draws come
  out black until M6 owns the resolves. The 64x1 format-29 LUT holds
  (0xFFFF,0xFFFF,0xFFFF,0) per texel and the 256x1 8888 LUT is all 0xFF -
  both equal the white placeholder, so the LUT formats changed nothing.
- Build 68: **render states from the device register shadow** (0x2200..
  at +0x2934: RB_DEPTHCONTROL +0x2934, RB_BLENDCONTROL0 +0x2938,
  RB_COLORCONTROL +0x293C, PA_SU_SC_MODE_CNTL +0x2948; 0x2100.. at +0x28CC:
  RB_COLOR_MASK +0x28DC, RB_ALPHA_REF +0x2904) - pipelines keyed by (pixel
  hash, state word); the alpha test goes to the translated shaders as
  c33.x = alpha ref, c33.y = SPEC_CONSTANT_ALPHA_TEST (the shader header now
  defines `g_SpecConstants()` as the per-draw `g_SpecFlags`; the cache is
  being re-translated). New formats: DXN -> BC5, DXT5A -> BC4, k_8, k_8_8,
  16-bit UNORM/SNORM/FLOAT, 32-bit float, 1D textures as Nx1.
- Run 54: the states took effect but everything front-facing vanished
  (floor gone, buildings as white back-face shells): the face bit was read
  the wrong way round. Build 69 flips it (face 0 = clockwise front here)
  and lets the depth clear follow the frame's depth functions (the state
  histogram says the market draws test **GREATER_EQUAL with z write** -
  Fable II uses reverse-Z - so the buffer is cleared to 0).
- Run 55: the histogram reads `depth 66 blend 00010001 cull 6 mask F` for
  809 of 850 draws (a few `blend 01060106`, `07010701`, `0D0C0D0C`), yet the
  scene is mostly white with the clear colour where the floor should be.
  Run 56 (`ngpu_state=false`) separates the states from the formats; build
  70 adds `ngpu_state_mask` (1 depth, 2 blend, 4 cull, 8 colour mask, 16
  alpha) to bisect, and a per-frame histogram of RB_SURFACE_INFO /
  RB_COLOR_INFO / RB_DEPTH_INFO (+0x2880/84/88) for the M6 render-target
  map.

## Runs 57-58 (22:50-22:57) - the render-target map: the scene is HDR

Build 70/71 histograms (per frame, from the register shadow at draw time
and the XDK Resolve hook):

| RB_SURFACE_INFO | RB_COLOR_INFO | RB_DEPTH_INFO | draws | meaning |
|---|---|---|---|---|
| 14010500 (pitch 1280, **2x MSAA**) | 00030000 (EDRAM 0, format 3 = **2_10_10_10_FLOAT**) | 00010400 (EDRAM 0x400, D24FS8) | ~800-970 | the scene pass: HDR colour |
| 14010500 | 000C0000 (format 12 = 2_10_10_10_FLOAT_AS_16_16_16_16) | 00010400 | 25-44 | same pass, another colour view |
| 04020118 (pitch 280, 4x MSAA) | 00000000 (8888) | 000100E0 | 13-16 | a small pass (impostors?) |
| 0A000280 (pitch 640) | 00000000 (8888) | 000100B8 | 2-3 | the 640-wide pass (light buffer) |

Resolves per frame (dest base, size, fetch format, flags): 256x256 f3 x4
(impostors), 320x180 f6 / f54 / f7 (bloom chain), 640x360 f54 (the HDR
scene downsampled: 2_10_10_10_AS_16_16_16_16) and 640x360 f6, 1280x720 f2
(k_8) with flags 0x10 (a depth resolve), 1024x1024 f23 flags 4 (the shadow
map). Every dest base is in the **0xE0 form** (F27xxxxx...), i.e. the
engine's texture headers hold raw virtual addresses for its render
targets while CPU-loaded textures carry converted physical ones.

Consequence for the shadow window: the scene is rendered into a 7e3-float
HDR target and tonemapped by the post-process chain, which the native
path does not run yet; my B8G8R8A8 window shows the raw HDR values and
saturates anything above 1 - the white walls since build 68 may be that
(the dark runs 52/53 had fixed states; with the game's states the same
draws saturate?) or the new formats. Run 58 (old formats only, states
off) was not captured; run 59 uses diagnostic mode 16 (a flat colour per
pixel shader) to tell which shader paints the walls.

M6 plan from these numbers: a native render target per (surface pitch /
MSAA, colour base + format, depth base + format) key - the scene target
R16G16B16A16_FLOAT (7e3 fits), the light buffer and impostor targets
8888, depth D32 - and on each Resolve a copy/blit of the source target
into a texture bound to the dest header's base, so the light buffer,
shadow maps, bloom chain and impostors come from our own passes; the swap
resolve (1280x720 8888) drives the window, i.e. the game's own tonemap.

## Runs 59-63 (22:58-23:25) - M6: native render targets

**Capture problems first.** From run 59 the game window covered the
shadow window (so `eye_look` returned the real game - a useful reference:
the market at night is dark timber and brick, i.e. the dark runs 52/53
were closer to right than the white ones). Build 73 adds in-process
screenshots (`ngpu_shot_every=N` writes the back buffer to
`ngpu_shot.bmp` next to the executable through a readback buffer); the
first attempt crashed inside Plume - `copyTextureRegion` calls
`setSamplePositions(dstLocation.texture)` and a buffer destination has no
texture - patched with a null check and Plume rebuilt (Release). Build 74
re-asserted the window's topmost flag every 2 s from the render thread
and **hung the game** (run 61: the window belongs to another thread, so
`SetWindowPos` blocks until that thread pumps) - `SWP_ASYNCWINDOWPOS`.

**M6 (build 75, run 62).** Native render targets keyed by the register
shadow at draw time: `GetRT(RB_SURFACE_INFO, RB_COLOR_INFO)` creates an
RGBA16F colour target + D32 depth at (pitch x height) - the height learnt
from the resolve destinations that read the target (16:9 guesses before
that) - and `BindRT` binds it, clearing colour to black and depth to 0 or
1 by the zfunc majority of the target's draws in the previous frame. The
XDK Resolve (`sub_82206888`) now calls `ResolveNative`: the bound target
(colour, or depth for flag 0x10 / format 23) is copied into a texture per
(dest base, size) - `g_resolved` - which `GetTexture` serves before the
guest-memory path, so the light buffer, bloom chain, impostors and shadow
maps come from our own passes. Every draw pipeline renders RGBA16F; at
EndFrame the widest / most-drawn target of the frame is blitted to the
window by a fullscreen triangle (`ngpu_blit_vs.dxil` + `ngpu_ps_xs` mode
17, `ngpu_exposure`). `ngpu_native_rt=false` restores the old path.

Run 62 (build 75): the machinery works - targets 1280x720 (colour formats
0, 3, 12), 1040-pitch and 560-pitch shadow maps (dests 1024x1024 / 512x512
format 23), 280x280 impostors (four 256x256 dests per frame), 640x360 and
320x180 bloom targets; resolves per frame as in the histogram; frame cost
~3 ms, the guest at 59 fps. The window stayed green because the blit
pipeline was built before its pixel shader existed (a null pixel shader
rasterises nothing) - build 76. The bloom chain reuses one dest base for
a 640x360 and a 320x180 resolve every frame, which recreated the resolved
texture twice per frame - build 77 keys `g_resolved` by (base, w, h), and
the fetch path looks up with the fetch constant's size.

## Runs 64-68 (23:20-23:42) - the blit works; the scene is HDR times 9.67

The blit path was proven step by step with in-process shots: mode 18
(solid magenta) fills the window, mode 19 shows the fullscreen triangle's
uv gradient, mode 20 (sample + gradient) shows the scene target - the
same white-geometry / black-floor image as the window path had, so M6's
plumbing is right and the *content* is the question. Run 68 dumped the
material shader's named constants for a sampled draw:
`g_GlobalAmbientAndBrightness = (0.0255, 0.0473, 0.121, 9.67)` - the
output is multiplied by **9.67** (the HDR target is tonemapped by the
game's post-process, which the native path does not run), the global
light params c80/c81/c87 are zero (night), `g_AtmosphericFactors = (1, 1,
0, 0)` (full fog influence), the atmospheric parameters c64..c68 small.
At `ngpu_exposure=0.02` the white areas become a flat uniform grey with
no texture detail anywhere, i.e. the lit term is ~0 on every wall and the
fog term (extinction / in-scattering, the 1D LUTs the game rewrites each
frame - `ngpu_dump_slot=-3` now dumps every upload) is what we see. The
sampled draw's slot 13 was a real 128x128 texture (descriptor 121), not
the 1x1, so the black lit term is not only the 1x1s. Diagnostic modes
21/22/23 (build 80: the material's diffuse via TEXCOORD0.xy, its lightmap
via TEXCOORD0.zw, slot 0) render through flat pipelines that now carry
the draw's own states (the reverse-Z clear made the fixed LESS_EQUAL
diagnostics draw nothing - mode 16 came out black for that reason).

## Runs 69-71 (23:44-23:55) - the market is textured; the pixel shader was the wrong one

Diagnostic mode 23 (build 80: slot 0 sampled with TEXCOORD0.xy through the
game's vertex shaders, flat pipelines carrying the draw's states) renders
the **whole market correctly textured** - timber houses, the stone arch,
the clock tower, roofs, signs (in-process shot `shot_m23.png`); mode 21
(slot 13 with the same coordinates) is black almost everywhere, mode 22
(slot 3 with TEXCOORD0.zw) the lightmap. So the real material shader of
these draws samples its diffuse at **slot 0**, and `4CE80D10_p`
(`g_BackgroundDiffuseTexture` at slot 13, 800 draws per frame) is not
their pixel shader: the SetPixelShader hook (`sub_82208BB0`, device+0x3194)
misses the path the engine sets pixel shaders through, so every draw was
paired with the last pixel shader that went through the hook - the
"background" shader - which is why the lit term was zero (1x1 black at
slot 13) and only fog showed. Build 81 takes the draw's vertex and pixel
shader objects from the device's own fields (+0x3198 / +0x3194, which the
XDK draw flush reads) with the hooks as fallback, and counts the draws
where they differ.

## Runs 69-74 (23:55-00:25) - reading the ring buffer

- Run 69 (build 81): the device's current-shader fields (+0x3198 / +0x3194)
  agree with the SetShader hooks on every draw, and run 71 (build 82, a
  backward scan of the ring for the last type-0 packet writing each fetch
  slot) agrees with the device fetch shadow - so the fetch constants are
  right and the pixel shader object really is `4CE80D10` for those draws.
  The backward scan (64 KB per draw) cost half the frame rate; it is off
  by default (`ngpu_ring_fetch`).
- Run 70 (`ngpu_slot13=0`, exposure 0.1): still the flat fog colour
  (`g_AtmosphericParameters(4)` = (0.27, 0.41, 0.47)) - the lit term is
  zero even with the atlas at slot 13. Run 72 (`ngpu_dump_slot=-3`) shows
  the fog LUTs are real dynamic ramps (the 256x1 extinction goes from white
  to (0x21,0x4d,0x59) across the row; the game rewrites them each frame -
  the write watch re-uploads them), and a family of odd-width 1D textures
  (1303x1 format 28, 1837x1 format 10, 2610x1 8888, 512x1 float) that are
  data buffers in texture clothing.
- Conclusion: the draws the engine kicks itself (`sub_82B9EEE0/F038` with
  `IM_LOAD_IMMEDIATE`) load their shaders through the ring, so the XDK
  objects describe only the draws that go through the XDK. Build 84 adds an
  **incremental ring parser** (`RingAdvance` before each draw): type-0
  register writes, SET_CONSTANT, LOAD_ALU_CONSTANT (ALU / fetch / bool /
  loop / register ranges), IM_LOAD and IM_LOAD_IMMEDIATE (the last loaded
  vertex / pixel microcode pointer and size). Draws take their fetch
  constants and states from `g_ring.regs` when a packet wrote them, and
  their shaders from the ring's microcode matched by XXH3 of the code
  block against the containers registered when they went through the
  hooks (`g_vs_by_code` / `g_ps_by_code`); the device fields remain the
  fallback. Ring geometry from run 73: the write pointer (+0x30) advances
  100-1000 bytes per draw inside a ~150 KB segment whose end is at +0x34
  (+0x38 = the kick margin, 0xA0 below), then jumps to another segment -
  the parser resyncs by parsing forward from 16 KB behind the new pointer
  until the packets parse cleanly.

## Runs 75-79 (00:25-01:05) - the parser works; the missing pixel shaders

- Run 75/76 (builds 85-86): the "bad" parse at every draw was the trailing
  packet: the XDK leaves the next packet's header (`C0006000`, count 1)
  right at the write pointer, so a packet that runs past the pointer now
  ends the stream successfully and the cursor stays on it. With that the
  parser is healthy - ~30-60 k packets and ~900-1900 shader loads per
  frame, one resync per segment switch, a dozen bad packets, the guest at
  59 fps. Resync tries are capped (512 starts from 16 KB back).
- The pixel codes the ring loads mostly matched no registered container:
  a diff against a same-size container showed a *different shader*, not a
  patched one. Cause: `SetShaders(dev, VS, PS)` (`sub_82221858`, the pair
  the engine's draws use) passed its pixel shader to the hook as the
  vertex shader's "entry", never as the current pixel shader, so every
  engine draw was paired with the last `SetPixelShader` (the background
  shader `4CE80D10`). Build 87 records it; those 32 pixel-shader objects
  were then rejected as "not in ngpu_cache" - never dumped, because
  `DumpMicrocode` wrote only the first argument. Build 88 dumps the pair's
  pixel shader too; run 78 (`ngpu_dump_at_frame=1`) collects them, the
  translation + pack follow, run 79 draws with them.

## Runs 80-82 (01:00-01:20) - the real material shaders render

- Run 79 (the 143 new containers translated and packed) still paired the
  draws with the background shader: `DevicePixelShader` preferred the
  device field (+0x3194, which only `SetPixelShader` writes) over the hook.
  Build 89 prefers the hooked SetShaders pair, applies REG_RMW packets in
  the parser; run 80 then rejected most of the real pixel shaders - 24 of
  them "sample a cube map" (a placeholder rejection). Build 90 adds **cube
  maps** (six tiled faces, each a level-0 image padded to 4 KB, a
  Texture2D array of 6 with a TextureCube view in their own 256-entry heap;
  the cube table at c16..23 of the shared constants).
- Run 81: **1059 of 1059 draws through the game's own pixel shaders**, 37
  pixel shaders loaded, 3 rejected (not in the cache). The picture shows
  the market's materials for the first time (the stone arch, a cart,
  torch sparks, the floor) but overexposed and with every draw at
  `RB_DEPTHCONTROL = 0` (no depth test): the parser's register file starts
  at zero, so the XDK's read-modify-write updates of the depth control
  produced 0. Build 91 seeds the register file from the device shadow at
  every resync (registers 0x2000.., 0x2100.., 0x2180.., 0x2200.. and the
  fetch constants) and the scene blit applies exposure then a Reinhard
  curve instead of a clamp.

## Run 83 (01:20) - the native draw moves to the draw function's exit

The parser's state was one draw late: the entry hook of `DrawIndexedVertices`
fires before the XDK flushes that draw's dirty state (fetch constants,
render states, shader loads) and writes its DRAW packet, so at draw N the
ring held draw N-1's state - run 82 saw `RB_COLOR_MASK = 0` and
`RB_DEPTHCONTROL = 0` for most draws (the depth-only pass's values,
one draw late). Build 92 keeps the entry hook for the parameters and
runs the native draw from a **completion hook at 0x8221E408** - the
`addi r1,r1,208` before the shared epilogue, reached after the packet loop
stores the write pointer. A hand edit of the generated `fable2_recomp.197.cpp`
was overwritten by the build's codegen (run 83 drew nothing), so the hook
is declared in `config/hooks/native_gpu_trace.toml` (build 93) and codegen
emits it; the entry, which the generator does not produce, is:

```
[[midasm_hook]]
address = 0x8221E408
name = "ngpu_8221E408"
registers = ["r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10"]
```

`OnDrawIndexed` now only records (dev, prim, base vertex, start, count) in a
thread-local pending slot; `OnDrawIndexedDone` performs `ShadowDrawIndexed`
with it.

## Runs 84-86 (01:22-01:40) - the night market

With the completion hook (build 93, declared in the hooks TOML) and type-1
packets parsed (build 94: two register writes per packet, which the parser
had treated as failures), run 85 renders the **night market as the game
means it**: dark timber buildings with lit details, the clock tower, the
moonlit stone building, people by the arch, light streaks on the stone
floor (tonemapped at exposure 0.15). The parser runs at ~40 k packets and
~1,300 shader loads per frame with a dozen bad packets and no resyncs
inside a segment; 348 of 1,228 pixel-shader loads match a translated
container by microcode hash. What is still wrong: `RB_DEPTHCONTROL` reads
0 for the scene draws (the depth-only pre-pass draws carry 0x66), so the
scene overdraws in submission order and some pillars look translucent;
build 95 logs the packet type behind every write to the depth, blend and
colour-mask registers to find where the 0 comes from.

## Runs 87-88 (01:35-01:45) - the market with occlusion; the state at the DRAW packet

Build 95's register-source log showed the zeros are genuine type-0 writes:
the game writes `RB_DEPTHCONTROL` 0x00708766 / 0x00008777 / 0 in pairs,
and the per-shader histogram (build 96) has one shader drawing under
eight different depth controls - i.e. the XDK writes more state *after*
the DRAW packet inside `DrawIndexedVertices` (a post-draw reset), which
the exit hook then reads as the draw's state. Run 87 with
`ngpu_force_depth=1` (every draw tests and writes GREATER_EQUAL) renders
the **market with correct occlusion**: the cobbled floor with light
pools and the compass rose, the hero at the centre, the buildings and
clock tower behind, the moonlit stone facade (`shot87.png`). Build 97
makes that the default the proper way: the parser snapshots the state
registers (0x2000.., 0x2100.., 0x2200.., the 32 fetch constants) and the
last-loaded shaders **at every DRAW packet** (opcodes 0x22/0x34/0x35/0x36),
and the draw uses that snapshot rather than the register file's final
state.

## Runs 89-90 (01:45-01:55) - what one draw looks like in the ring

Build 98 traced the packet sequence per draw. A typical engine draw at the
exit hook reads `T3[22 x4] T3[60 x1] NOP T0[4000 x32] T0[2203 x1] T0[484E
x18] T0[48BA x6] T0[5000 x3] T0[2102 x1] T3[22 x4]`: the previous draw's
DRAW_INDX (its header was the trailing packet last time), an XDK marker
(opcode 0x60, one dword), vertex ALU constants, a couple of state
registers, fetch constants 13..15 and 31 (the material textures and the
vertex stream), then this draw's DRAW_INDX - whose data is not complete
at the hook. Build 97 (snapshot at complete DRAW packets) therefore took
the previous draw's state (run 88: almost everything at colour mask 0,
i.e. the depth-only pass's state); build 99 snapshots at the trailing
DRAW header as well, which is this draw's, and the packet parses again
in the next range (harmless).

## Runs 91-94 (01:55-02:15) - which source is right for what

- Run 90 (snapshot at the trailing DRAW header, render target from the
  snapshot too): black - the pass-level surface registers are not
  rewritten per draw, so the snapshot sent the scene into the shadow-map
  target. Build 100: the render-target key stays with the device shadow.
- Run 91 (ring states + ring shaders): geometry warped by overdraw, 512
  draws at zfunc ALWAYS. Build 101 adds `ngpu_ring_states` /
  `ngpu_ring_shaders`. Bisect: **a** (states from the shadow, shaders from
  the ring) = the dark night market again; **b** (states from the ring,
  hooked shaders) = the overbright translucent pillars; **c** (both off) =
  the arch with the wrong (window) texture. So the device shadow is right
  for the render states and the ring's shader loads are right for the
  shaders; the ring's per-draw state snapshot is not (`ngpu_ring_states`
  now defaults off). The ring remains authoritative for the fetch
  constants and the shader pairs.
- Left vs the forced-depth picture of run 87: the floor's light pools.
  The additive light draws (blend 01010101) carry colour mask 0 in the
  shadow read at +0x28DC; run 94 draws with the colour mask ignored
  (`ngpu_state_mask=23`) and logs the words around +0x28DC against the
  ring's RB_COLOR_MASK to check that offset.

## Runs 95-96 (02:15-02:30) - non-indexed draws, first try

Build 103: `DrawVertices` (lists and strips) goes through the translated
path (no index buffer, `drawInstanced(count, 1, start)`, strip topology in
the pipeline key, fetch constants from the device shadow because it runs
at the entry hook), a cached stream shorter than a draw's range is copied
per draw instead of failing, and the probe pipelines gained GREATER_EQUAL
variants for reverse-Z frames. Run 95 with the non-indexed draws on:
huge dark quads over the whole scene - the full-screen and light-volume
draws landing in the scene target with the wrong shaders or constants
(entry-hook timing: their own packets are not in the ring yet, and the
hooked shader pair may be the previous draw's). Build 104 puts them
behind `ngpu_draw_vertices` (off); the exit-hook approach that fixed the
indexed draws is the way to do them properly (find `sub_8221C3E8`'s exit
the same way: the `addi r1` before its shared epilogue).

## Runs 97-98 (02:30-02:37) - the default look, and non-indexed draws at their exit

Run 96 showed the dark quads with the non-indexed draws off: they were
the fallback draws made visible by the reverse-Z probe pipelines (build
103) - the flat vertex shader places untranslated geometry (skinned
meshes, the 4 "format 6 TEXCOORD" vertex shaders) wrongly, so invisible
is better; build 105 keeps those pipelines behind `ngpu_probe_rz`. Run
97 is the current default look (`shot97.png`). Build 106 hooks
`DrawVertices`' exit (0x8221C7D0, the `addi r1,r1,160` before its
epilogue, declared in the hooks TOML) and runs the non-indexed draw
there: run 98 has no dark quads any more, but the arch pillars show the
wrong texture while they are on (a snapshot / fetch-constant interaction
still to understand), so `ngpu_draw_vertices` stays off.

## Runs 99-100 (02:40-02:50) - vertex shaders that sample textures

Build 107 let the four texture-sampling vertex shaders translate (the
header samples level 0 in the vertex stage). Run 99: 30 vertex shaders
loaded, but the arch pillars are painted with the window texture again -
the same look as run 98 with the non-indexed draws on: some extra draws
land on the pillars with the wrong material. Build 108 puts them behind
`ngpu_vs_textures` (off) until the pairing / target of those draws is
understood; run 100 is the default check.

## Runs 101-102 (02:50-03:00) - the ring's shader loads are engine copies

Run 101 (build 109) took the fetch constants from the parser's *final*
state (`ngpu_ring_final`, the XDK writes some fetch constants after the
DRAW packet) and got orange pillars; the snapshot state (runs 97-100) gave
window-textured or grey pillars, and the entry-hook timing of run 94 gave
stone ones. So the pillar material depends on *when* the draw's constants
are read, not on which register source - a sign that something the draw
depends on is written after the draw call returns (the engine fills the
LOAD_ALU_CONSTANT source memory, or the next material's constants land
before the GPU would have read this draw's).

Run 102 (build 110) matched the ring's shader loads by block address
first (`g_vs_by_addr`/`g_ps_by_addr` from `GpuAddr(block)`) and, with
`ngpu_ring_strict`, refused to draw a draw whose ring load matched nothing:

    ring parse: 1790 draws, 1513 shader loads; by code: vs 402 hit 1388 miss,
    ps 0 hit 303 miss
    ring strict: 1388 draws skipped for an unknown vertex shader,
    303 drawn flat for an unknown pixel shader
    135 draws through translated shaders

So most loads in the ring point at microcode the engine copied to its own
memory (no container we dumped lives there and the hashes miss too - the
copies are not byte-identical: Fable patches them). Strict matching is a
regression and is off again; the hooked SetShaders pair stays the shader
source, the ring supplies the ~400 loads it can identify.

Build 111 makes the draw timing switchable (`ngpu_draw_at_exit`, default
off = the entry-hook timing of run 94, the best pillars) so the exit-hook
path stays available for the ring work without being the default.

## Runs 103-104 (03:00-03:10) - the entry timing again, with the right sources

Run 103 (build 111, `ngpu_draw_at_exit` off) drew flat pale buildings: at
the entry hook the ring holds the *previous* draw's packets, so the
ring-sourced shaders and fetch constants were one draw behind. Build 112
gates every ring source on the exit timing; run 104 is back to the run-94
look (stone pillars, the clock tower, the timbered houses). What is still
pale and flat there is the fallback draws - ~210 "not cached" and ~210
"range" per frame (of ~1,550), which the flat pipeline paints:

- "not cached": ten hooked containers. Four fail to compile on the
  boolean / loop constants the game sets with SetShaderConstantB/I
  (`b129`, `b132`, `i0` - 316 of the 1,333 translated files use them),
  two are rejected by the runtime for "format 6 TEXCOORD" (an 8-bit
  *integer* attribute into a float input), four sample textures in the
  vertex stage (`ngpu_vs_textures`, kept off).
- "range": the per-draw stream copy refused draws whose vertex range
  exceeded the fetch constant's size - the instance / repeated-mesh rows,
  whose index the shader computes itself.

Build 113 + the re-translated cache (`fable2_run113`, all 2,472 dumped
containers through the new header) address the first two and the third:

- `fable2_shader_common.h`: `uint4 g_BoolConstants[2]` (c34..c35) and
  `uint4 g_LoopConstants[8]` (c36..c43) in the shared block; the new
  `tools/native_gpu/fix_hlsl.py` (run by `translate_all.sh` before dxc)
  rewrites `if (b132 != 0)` to `NGPU_BOOL(132)` and `i0.x` to
  `NGPU_LOOP(0).x` - plain `#define b1` would have broken the
  `register(b1, space4)` bindings - and inserts `iTexCoord0 *= 255.0` for
  integer 8/16-bit inputs, which the runtime now feeds as UNORM.
- the runtime fills c32.x (the named booleans: vertex bits 0..15, pixel
  16..31), c34..c43 from the device shadow at `ngpu_bool_off` (0x1780) /
  `ngpu_loop_off` (0x17A0) - the guess "right after the ALU constants";
  a 300-frame log compares them with the ring parser's 0x4900.. registers
  and scans the device object for their real home.
- `ngpu_range_clamp`: the copy takes the fetch constant's whole size when
  the draw's range overstates it (the shader indexes the rows).

## Build 114 (03:25) - DrawIndexedVerticesUP, and where the non-indexed draws really went

`sub_82217DB8` is DrawIndexedVerticesUP (dev, prim, minIndex, numVertices,
indexCount, ?, stride, vertexData, [indexData on the stack]) - the UI and
the post-process quads (prim 4, 4 vertices, 6 indices, stride 20 / 12). The
XDK copies the vertices and the indices into the ring and emits
SET_CONSTANT (stream 0) + DRAW_INDX, so the native draw runs at the
function's exit (`addi r1,r1,224` at **0x822182CC**, r3 = HRESULT; a
hand-declared hook like the two draw exits) from the ring parser's last
DRAW packet: the initiator (prim type bits 0..5, index source 6..7, 32-bit
bit 11, count 16..31), the DMA index base + size word (endianness in bits
30..31) or DRAW_INDX_2's inline indices. `ShadowDrawUP` rebuilds the index
list in the upload heap (strips expanded) and drives the translated draw
with it through a thread-local index override; the vertices come from the
shadow's stream-0 fetch pair as for any other draw. `ngpu_draw_up` (off by
default until run 106 shows the counters: `[ngpu] up draws: N calls, M
drawn, ...`).

Found on the way: the non-indexed support from build 105 (`ib_phys == 0`
= DrawVertices) had landed in `DrawCached` - the *flat probe* path - not
in `DrawTranslated`. So every DrawVertices went through the flat probe
(the "dark quads" of runs 95-96) and never through a translated shader;
`DrawTranslated` failed them at `CachedIB(0, ...)`. Build 114 moves the
logic where it belongs, so `ngpu_draw_vertices` gets its first real test
in run 107.

## Runs 105-107 (03:26-03:34) - the new cache, and two regressions caught

Run 105 (build 114, the run113 cache): 1,301 draws through translated
shaders (from 1,124), "range" fallbacks 0 (309 clamped), "not cached" 131.
But the picture grew a wide pale band across the middle: the clamped
instance draws feed zero rows to the input assembler, and the translated
instancing shaders do *not* compute their row index (XenosRecomp turns
every vfetch into an input-assembler attribute), so the rows must really
be per-instance data - `ngpu_range_clamp` is off again and instancing
stays open (it needs the shader's own index math or a per-instance input
slot: the layout sidecar does not say which fetch is the instance row).

Run 106 (`ngpu_draw_up`): 117 UP calls per frame, 0 drawn - 76 "without a
parsed packet" (the parser's DRAW sequence did not advance at the exit
hook), 36 "draw failed" (the indexed path refused them: probably the
stream-0 pair). Build 115 logs the first twelve exits (cursor, write
pointer, segment end, packets parsed, initiator).

Run 107 (`ngpu_draw_vertices`, its first real test - see build 114):
1,327 translated draws, but a flat pale plane with silhouettes appears at
mid-height: a screen-space quad (light / fog volume) drawn into the scene
target. Off.

The bool/loop constants: the ring parser sees the right bits
(`0x4904 = 0x110`: b132 and b136, the pixel shader's flow-control
booleans), but the device object holds no packed copy (the 300-frame scan
found none in its first 16 KB: the XDK packs them at the flush from an
unpacked shadow). Build 115 takes them from the ring's registers
(`ngpu_bools_ring`) - one draw late under the entry timing, right for
every draw that does not change them.

## Runs 108-110 (03:37-03:50) - UP draws reach the draw path; the instancing plan

Run 108 (build 115: range clamp off, booleans from the ring): the run-104
look again with the new cache - 1,055 translated draws, range 274, not
cached 125.

Runs 109-110 (`ngpu_draw_up`): the UP exit's ring parse sees the draw's
packet only sometimes - other XDK draws (resolve / clear rectangles,
`init 0x00030088` = RECTLIST, auto-index) follow it before the exit hook
fires - so build 116 matches the UP draw among the last 32 DRAW packets by
primitive type and index count (`init 0x00060004`: list, 6 indices, DMA
indices at 0x1F7A1A30, 6 bytes, 8-in-16) and takes stream 0's fetch pair
from the ring's SET_CONSTANT (the XDK writes it straight into the ring, not
the shadow). Run 110: 58 of 123 UP calls per frame now reach the draw path
and fail there - build 117 logs why (shader status, stream pair, stride).
The rest: 44 with no new DRAW packet parsed, 21 with no matching packet.
(A diagnostics line per UP call at frame multiples of 600 rotated the log
at 5 MB - `fable2.1.log` holds the first part of run 109; twelve lines
per run now.)

### Computed-index fetches (build 117 + the run117 cache)

The recompiler turned every vfetch into an input-assembler attribute and
never looked at the fetch's source register. Fable's instancing and
repeated-mesh vertex shaders compute their indices (`ps = r0.x * (1/N)`,
`r0.z = trunc(ps)` for the instance row, `r1.x = ...` for the vertex within
the instance) and fetch with them - so those draws got the wrong rows (the
"range" fallbacks) and the run-107 DrawVertices plane. The fix, on the
XenosRecomp `fable2` branch:

- vertex shaders get `iVertexId : SV_VertexID` and `r0.x = float(iVertexId)`
  (the Xenos convention; D3D12's vertex id includes the base vertex);
- a fetch whose source is not r0.x (register or swizzle) is *computed*:
  `ngpu_vload(NGPU_STREAM(s), uint(max(0, int(floor(r0.w)) * 7 + 2)), fmt, signed, integer)`
  loads the row from the stream as a `StructuredBuffer<uint>` (register
  space 4, the runtime's fifth descriptor set, 8,192 slots per frame,
  descriptor index per stream in c44..c47) and decodes formats 57 / 38 /
  37 / 36 / 32 / 31 / 6 / 26 / 25 / 7 / 16 / 17 in `fable2_shader_common.h`;
  the sidecar's 13th field marks it, the runtime makes no input element for
  it but binds the stream (its cached entry whatever the draw's range, or a
  per-draw copy of the fetch constant's whole size, 1 MB at most);
- the raw-index heuristic does not track r0.x being rewritten before a
  fetch (rare; would read the wrong row).

## Runs 111-113 (04:06-04:25) - computed fetches hit a pipeline wall; the real UP function

Run 111 (build 117 + the run117 cache: 333 compiled, 278 with computed
fetches): every vertex shader that loads a stream itself is rejected with
"pipeline" - `CreateGraphicsPipelineState` fails for them (Plume does not
report why; build 118 enables the D3D12 debug layer on demand,
`ngpu_d3d_debug`, and logs the info-queue messages when a pipeline fails).
The picture is the run-108 one (those draws fell back: "not cached" 350).

Run 112 (`ngpu_draw_up`): 30 UP calls per frame reached the draw path and
failed with "vs 4C277100 (not in ngpu_cache)" - the UI vertex shaders were
never dumped: the engine sets them through a setter the SetShaders hooks do
not cover (the device fields at +0x3198 / +0x3194 know them). And the ring
at the "UP exit" parsed as garbage (`T3[7F x16384]`): `sub_82217DB8` is
only the XDK's *begin* step. Its caller **sub_8222E120** is the real
DrawIndexedVerticesUP(dev, prim, minIndex, numVertices, indexCount,
pIndexData, indexFormat, pVertexData, stride at r1+0x54): it reserves ring
space (the begin), copies both arrays into it (`sub_82CA9480` = memcpy,
twice) and commits the write pointer from device+0x3484 - at the begin's
exit the reserved area is still unwritten. Build 118 hooks the wrapper's
entry (its arrays and the stack stride) and its exit at **0x8222E1B8**
(`addi r1,r1,176`) and draws from the arrays themselves: indices read from
guest memory (16-bit big-endian, minus minIndex), the vertices through a
stream-0 override (the call's bytes, 8-in-32 unless the ring's SET_CONSTANT
for the copy says 8-in-16), shaders from the device fields, and those
shader objects dumped on sight when dumping is on.

## Runs 113-114 (04:20-04:35) - the pipeline wall explained

Run 113 enabled the D3D12 debug layer from a cvar at device creation:
enabling it after the Xenos plugin's device exists removes that device
(`rex::FatalError` at the first paint, 3 s in). Build 119 enables it from a
static initializer at process start instead (`NGPU_D3D_DEBUG=1` in the
environment; `ngpu_d3d_debug` then logs the info queue on a pipeline
failure) - run 114.

The wall itself fell to `dxc -dumpbin` on the cache: a computed-fetch
vertex shader's **input signature still lists POSITION2/3/4/0 and NORMAL0**
(the recompiler declares an input per vertex element before it looks at
the fetches), and the runtime, making no input element for a computed
fetch, handed D3D12 an input layout missing signature elements - which is
a pipeline-creation error. Build 120 gives every computed fetch a dummy
element (one dword at offset 0 of its stream, R32_FLOAT or R32_UINT by the
declared type); the shader ignores the attribute and loads the row itself.

Run 114 (build 119, debug layer on from the start): no pipeline failed at
all - 1,234 translated draws, **"range" fallbacks 4** (from 283), the
market's ground band and a textured cart render where the pale plane was
(`shot114.png`): the instancing shaders work. Why the same cache failed in
runs 111-113 without the debug layer is unexplained (the signature /
layout mismatch is real either way; build 120's dummy elements make the
layout complete regardless). The UP draws did not run: the wrapper's exit
r3 is memcpy's return in the success path, not an HRESULT (build 121 skips
only E_OUTOFMEMORY). The dump-on-sight at the UP calls collected 187 new
containers - the UI / loading / post-process shaders - and 186 of them
compile (`fable2_run120`, packed on top of the run117 cache: 519 shaders).

## Runs 115-116 (04:25-04:31) - instancing without the debug layer

Run 115 (build 120, defaults): the dummy input elements hold - no pipeline
rejection, 1,179 draws through translated shaders, "range" 4, 6 vertex
shaders rejected (the four texture-sampling ones behind `ngpu_vs_textures`,
the 16_16-float BLENDINDICES one, one more). The market's ground band and
the instanced props render (`shot115.png`); the pale flat facade on the
right is still fallback draws. Run 116 (`ngpu_draw_up`, build 120) drew no
UP draws - the exit's r3 test (fixed in build 121).

## Run 117 (04:32) and the deferred UP draws (build 122)

Run 117 (build 121, the 519-shader cache): 1,250 translated draws, still no
UP draw - the wrapper `sub_8222E120` is never called by the engine. Fable
calls the XDK *begin* (`sub_82217DB8`) itself: it reserves the ring area
(the begin writes the fetch constant and the DRAW packet, returns the
vertex and index addresses through r10 and [r1+0x54]), fills the arrays
inline and commits the write pointer from device+0x3484 in its own code -
there is no function whose exit sees the data complete. Build 122 defers
the draw instead: at the begin's exit the device object (0x3600 bytes) and
the two returned ring addresses are copied; at the next draw hook (or the
present) the draw runs with `LoadV` reading from that snapshot (states,
fetch constants, shader fields, VS/PS constants all as the engine set them
for this draw; the ring parser is skipped while it is installed), the
indices read from the ring copy (16-bit big-endian minus minIndex), the
vertices through the stream-0 override with 8-in-32. `ngpu_draw_up` runs
120 and 121 (`ngpu_present_post`).

## Runs 118-119 (04:35-04:40) - the texture-sampling vertex shaders, and a log that rotates

Run 118 (`ngpu_vs_textures`): the four vertex shaders that sample textures
load (33 loaded, 2 rejected) and draw the same flat pale plane across the
market that DrawVertices drew in run 107 - a vertex-stage sample gone
wrong (level 0 through the 2D heap; the displacement / row-texture reads
collapse the geometry). Off until that fetch is understood.

Run 119 (`ngpu_present_post`, no UP draws yet): inconclusive - the log had
rotated. `[ngpu] ring pointers at draw` was gated on a counter that only
moves with `ngpu_ring_fetch`, so on every 300th frame it printed once per
draw (1,241 lines): the runtime rotates `fable2.log` at 5 MB and the early
lines (`blit:`, `not used:`, the first UP diagnostics) were lost. Gated on
the frame's draw count now (build 123).

## Runs 120-121 (04:41-04:47) - the deferred UP draws fire

Build 122: every UP call reaches the deferred draw (121 per frame, none
lost), and every one fails for two reasons fixed in build 123: the begin's
returned ring addresses are CPU-form like the ring pointers (`Host()`, the
0xE0 mirror + 0x1000) and were read through `Phys()` - a page off, all
indices zero; and the UI vertex shaders (`4C276D80`, `4C277C70`, ...) were
still "not in ngpu_cache" because the dump-on-sight sat in the unused
wrapper hook (moved to the begin hook). The chain after build 123: run 122
dumps them, they are translated and packed, runs 123 / 124 draw them
(124 presents the last 8888 target).

## Runs 122-124 (04:49-04:57) - the UI shaders' containers change every run

Build 123 (data through `Host()`, dump at the begin hook): every UP call
still fails, now with indices like 49024 (0xBF80 = the top of -1.0f): the
two out pointers were swapped - r10 receives the *index* area and the
stack one the *vertex* area, which comes first in the ring (4 x 20 bytes,
then the 12 index bytes). Swapped in build 124.

The UI vertex shader stays "not in ngpu_cache" although run 122 dumped it
(187 containers, 186 compile, packed): its object is a new one each run
(`4C276D80`, `4C276EF0`, `4C277530`, `4C276FB0`, `4C2799F0`) and its
*container* hash changes with it - the engine writes into the container -
while the code hash (`XXH3` of the microcode block, the key the ring
matching uses) does not. `pack_cache.py` now writes
`<codehash>_v.code` -> container hash aliases and the runtime falls back
to them when the container hash misses (build 124, the cache repacked from
run117 + run120 + run123). Runs 125 / 126 test it.

## Runs 125-126 (06:18-06:24) - the UI vertex shaders are built per run; runtime translation

With the out pointers swapped the deferred UP draws read real indices
(max 3 for a quad). The code-hash alias did not help: the runtime's own
"vs container" log shows the 96-byte UI vertex shader hashing
`D5E12A97B3A84881` in run 125 against `1B1DE457CA90EE5F` for run 122's
dump of the same shader - the engine patches the *microcode* per run (the
pixel shader objects `4216CED0` / `42151690` are static and cached; the
vertex shader objects live in a dynamic heap and change every run). No
offline cache can hold them.

Build 125 adds runtime translation (`ngpu_jit`, on by default): a cache
miss writes the container + block to `ngpu_jit/<hash>_v.xvu` and a worker
thread runs XenosRecomp, the fix_hlsl rewrite (ported to C++: unnamed
booleans / loops, integer inputs) and dxc, copies the DXIL + sidecar into
`ngpu_cache`, and bumps a generation the shader lookups check to retry a
"not in ngpu_cache" object. The tool paths are the dev machine's (a shipped
build would bundle XenosRecomp and dxc). Run 127 tests it with the UP
draws.

## Run 127 (06:28) - runtime translation works; the sky and the water composite appear

Six shaders were translated on first sight, 78-109 ms each (XenosRecomp +
the C++ fix + dxc), and two of them were picked up by the retry ("loaded"
lines two seconds later); 13 UP draws per frame render. The UI quad
shaders (`A108F279E72A396B`, `A00CE53EFBE08040`) were translated within
100 ms of their miss but the per-frame count still says 112 fail - the
reason log was spent in the first frame, so build 126 adds a per-frame
histogram of failure reasons.

The picture changed completely (`shot127.png`): three of the new shaders
are position-only full-screen passes the offline dumps never caught - two
sky-dome shaders (world-view-projection + eye position) and the **water
composite** (`g_WaterConstants`, `g_WaterHeight`, a screen-space quad with
the half-pixel offset). The sky gradient is right (a dawn sky over the
castle skyline); the water composite covers the whole market with its
blue plane, so its "is this pixel water" test passes everywhere - it reads
the scene depth resolve (`F2B1C000`, 1280x720, flags 0x10, native as
R32_FLOAT) and/or the scene colour, and one of those reads is wrong for
it. Next: log the fetch constants of the water pass's draws against the
resolved textures, and the depth decode the pixel shader applies.

## Run 128 (06:37) - the UP draws' stride

The failure histogram: 108 of 114 UP draws per frame fail with "draw path
(vs ok)" - the vertex shader is translated (48 loaded now, from 29) and
none of the translated draw's own early returns fired, so the draw entry
rejected them earlier: the UP path never writes the shadow's per-stream
stride table (the failure log showed "stride byte 0"), and the declaration
fallback yields nothing for these quads, so the entry's stride check drops
the draw. Build 128 supplies the call's own stride (20) through an
override and attributes the entry's skip counters in the histogram; runs
131 (UP draws, water composite skipped) and 132 (+ present the last 8888
target) follow runs 129-130.

## Next (state at 02:40, 2026-09-16)

**Where it stands.** The native path (fable2recomp `native-gpu`, build 102)
renders the market through the game's own vertex and pixel shaders,
textures (DXT1/3/5, DXN, 8888, 16-bit, float, 1D, cube maps, mip chains,
re-uploaded on guest writes), the game's render states from the device
shadow, native render targets with resolves feeding the light buffer /
bloom chain / impostors / shadow maps, a ring-buffer parser that supplies
the fetch constants and the shader pairs the engine loads, and a
tonemapped blit to the window; the guest runs at 59 fps with the native
path costing ~3-9 ms per frame. The best pictures: run 87 (`shot87.png`,
forced depth) and run 94 (`shot94.png`, defaults): the night market with
occlusion, the clock tower, the moonlit facade. Run the market with:

```
FABLE2_TUNE=ngpu_trace=true;ngpu_shadow=true;ngpu_native_draws=true;ngpu_shot_every=600;ngpu_exposure=0.15
```

(`ngpu_shot.bmp` beside the executable is the window's last shot.)

**What is still wrong or missing, in order:**

1. The floor and the light pools: ~400 draws per frame still fall back
   from the translated path ("not cached" index buffers, "range" = index
   range beyond the vertex buffer) to the flat pipelines, which test
   LESS_EQUAL against the reverse-Z clear and draw nothing. Make the
   translated path copy an uncached index buffer per draw, and revisit
   the range check (streams with a base vertex).
2. `DrawVertices` (non-indexed) draws run at their exit hook (0x8221C7D0)
   behind `ngpu_draw_vertices`: no more dark quads (run 98) but the arch
   pillars lose their stone texture while on - find why before enabling;
   `DrawVerticesUP` not rendered at all.
3. The ring parser's per-draw state snapshot disagrees with the device
   shadow (which is right); understand the XDK's post-draw writes before
   trusting the ring for states. `ngpu_ring_states` stays off.
4. Stencil is not implemented (the light pass may use it); `ngpu_force_depth`
   showed the depth pre-pass + GE semantics are otherwise right.
5. The game's own post-process (tonemap, bloom, the 1280x720 8888 swap
   resolve) is not run natively; the blit approximates it (Reinhard at
   `ngpu_exposure`). Wire the post-process draws (they draw into the
   1280-pitch 8888 target) and present the swap resolve instead.
6. Shader coverage (the texture-sampling vertex shaders translate but
   are off, `ngpu_vs_textures`: with them on the arch gets the wrong
   material - probably the same pairing / target question as the
   non-indexed draws): 315 of the 1445 dumped containers compile (the rest
   are the broken variant dumps); the ring matches ~350 of ~1,200 pixel
   loads per frame by code hash - the misses are containers never seen
   through SetShaders (dump more by playing further) and the `.cpu`
   variants. Cube-map shaders now compile; the 4 "format 6 TEXCOORD"
   vertex shaders and the int/bool-constant shaders still do not.
7. Texture write-watch churn: the fog LUTs and a few 16x16 textures are
   rewritten by the game each frame (~1-2 re-uploads per frame) - fine,
   but the 60-frame forced refresh also re-uploads textures that merely
   share a page with per-frame data.
8. The hand-declared exit hook (0x8221E408) must survive the next
   generator run of `native_gpu_trace.toml` (it is appended by hand).
9. Then: remove the Xenos plugin from the loop (the shadow window becomes
   the window), MSAA, the remaining passes (impostor/shadow correctness),
   and the frame-rate work.
