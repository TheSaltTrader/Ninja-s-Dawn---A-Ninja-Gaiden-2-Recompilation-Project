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

## Next

1. **Read watched guest pages without faulting.** ReXGlue's memory has the
   watch API the plugin uses (`include/rex/system/xmemory.h`:
   `TriggerPhysicalMemoryCallbacks(..., is_write, unwatch_exact_range,
   unprotect)`, `IsHostPageWriteWatched`, `QueryProtect`): resolve the watch
   on an index buffer's range once per frame before copying it (under the
   global critical region as the header requires), and the ~1 ms per draw
   goes away — the timers put everything else at 0.5 ms per frame for ~175
   draws, so the native path should then cost nothing at the game's 55–60 fps.
2. **Shader variants.** The market's main mesh shaders (`4D05F5D0`,
   `4D063B40`, ...) keep no fetch instruction in the block at obj+0x20;
   `SetShader` receives a pointer to a variant entry (obj+0x380.. or an
   external table such as 0x43004990), so the microcode in use is the
   variant's. Resolve the entry → microcode address in `sub_82221858`'s
   second path and the dump's `M` line, then the remaining ~1,450 draws per
   frame render too.
3. More position formats (2_10_10_10, 16_16_16_16 int) and the instance
   rows (POSITION1..3 from streams 1/2) for the instanced classes.
4. Then M5: XenosRecomp fixes for Fable's containers (duplicate inputs,
   SV_Position export, the 8 crashers) so the draws use the game's shaders.
