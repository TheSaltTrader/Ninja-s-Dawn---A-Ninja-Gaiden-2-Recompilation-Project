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

## Next

1. Run the census through boot → menu → the hero-1 save → 60 s in Bowerstone
   (pad script), read the `[ngpu]` lines: which entry points run per frame,
   their argument shapes (r3 = 0x44142480 for device methods, resource
   pointers otherwise), and the per-frame counts that size the HLE.
2. Name the rest of the 111 from the census (Create*/Lock*/Unlock*/Set*/Draw*),
   then the draw dump (VB, IB, shader pair, start/count per draw) at the draw
   entry points — also what the NG2 frame-interpolation work needs.
3. Present → Plume swap chain, Clear, one DrawIndexedPrimitive natively.
