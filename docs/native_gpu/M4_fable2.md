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

## Next

1. Census 2 with the ring writers and the dirty-flag setters hooked (285+
   hooks): find the real draw entry point(s), SetTexture / SetStreamSource /
   SetIndices / SetVertexShader / SetPixelShader / shader-constant setters
   (shadow-only writers, found statically by `dirty_setters.py`).
2. Name the rest from the census (Create*/Lock*/Unlock*/Set*/Draw*),
   then the draw dump (VB, IB, shader pair, start/count per draw) at the draw
   entry points — also what the NG2 frame-interpolation work needs.
3. Present → Plume swap chain, Clear, one DrawIndexedPrimitive natively.
