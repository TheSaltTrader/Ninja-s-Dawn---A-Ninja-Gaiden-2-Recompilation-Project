# NG2 native-GPU integration — handover, 2026-09-26

The day the user said Fable II's Xenos layer had gone native and told NG2 to
take the same road on the alt branch, carrying everything shipped through
v1.0.24. This file is the pick-up point; the ledger of what must be carried is
`CARRY_FORWARD_LEDGER.md`; the method is the Fable kit's `MIGRATION_GUIDE.md`
(`C:\users\renoi\claudecode\NATIVE_GPU_MIGRATION_KIT`).

**Standing constraints (all the user's):** nothing pushed or released from the
native work until it passes `claudecode/RELEASE_GATE.md`; NO NG2 run until
the Fable session says its optimisation testing is finished (it promised an
explicit message); builds only while `~/.game-test-lock` reads machine-free,
at low priority; one game at a time; carry the texture pack, ultrawide, the
fades, 60 fps at the correct game speed, and every SDK fix.

---

## 1. Where the pieces are

| piece | where | state |
|---|---|---|
| App, alt branch | `ng2recomp` branch `native-gpu-ng2`, worktree `D:\ng2_frameinterp\ng2recomp-worktree` | v1.0.24 merged (`6c86551`), ledger + census (`e1edb41`, `f26126e`), transplant wired (this commit) |
| Engine fork | `D:\ng2_frameinterp\ng2-rexglue` branch `native-integration` | `d87ebec2` Fable's rexglue-src main **8b4a025** imported wholesale outside thirdparty/; `80ac2721` NG2's ring re-init fix + RINGDUMP restored. The parser TU compiles (warnings only) |
| Fable's kit | `claudecode\NATIVE_GPU_MIGRATION_KIT` | received 16:36; `bin/ngpu_backend_dll/` arrived 17:22 (ABI 1, sha256 d522daf7..., from Fable commit 012fcf3 on engine 8b4a025). **THE DLL IS NG2's DEFAULT ROUTE** (cvar `ngpu_backend_dll`, manual mode: NG2's window + presenter half, callbacks forwarded, `NgpuBackendSetSetting("ng2_fov_k")` in, `NgpuBackendPresentMode` out); the in-exe copy is the fallback when the DLL is absent or answers another ABI. The header is `src/ngpu_backend_api.h` verbatim; the DLL is staged beside the exe (not tracked). Fable's caveat: the DLL was only run under offload there, so DLL-in-lockstep is first tested here |
| Fork-built SDK pair | `D:\ng2_frameinterp\ng2-rexglue\out\win-amd64\Release\` | built 17:23 by the waiter: plugin md5 `353c79e21479` (7,405,056 B), runtime `e0c0217b745b` (11,036,160 B). Verified by content: all five RexNgpu exports, gpu_offload_to_native, ng2_fov_k / ng2_uw_mode / NG2_UW_NOPAUSE / solid2d / [texpack] / texpack_mip, `pointers reset`, RINGDUMP, wait_reg_mem_yield_ms; runtime ng2_uw_mode / video_mode_explicit / watchdog |
| EMBARGO | lifted 17:31 by the Fable session: "optimisation testing is complete, the machine is yours"; lock free, no game running | runs allowed from here, one game at a time |
| Vendored backend | `src/native_gpu_xlat/` (171 files, 8.6 MB) + `src/ng2_native_backend.{h,cpp}` | Fable's tree verbatim, namespace `fable2::ngpu` -> `ng2::ngpu` (50 files, 267 replacements); `ORIGIN.txt` says why not re-vendored |
| NG2's glue | `src/ng2_ngpu_bridge.{h,cpp}` | plugin ABI bound by GetProcAddress, lockstep draw/swap, dirty-bitmap register sync with self-check and coverage, reveal hold on NG2's mode word |
| NG2's window | `src/ng2_ngpu_window.{h,cpp}`, `src/ngpu_shaders/` | raw D3D12 (no Plume), own thread, presents at the swap, THE ULTRAWIDE PRESENTER HALF (fill / pillarbox / keep-aspect from `ng2_uw_mode`) |
| Levers | `src/ng2_tuning.h` | `NG2_NATIVE_GPU=1` (ngpu_backend), `NG2_NATIVE_OFFLOAD=1` (gpu_offload_to_native), `NG2_TUNE=a=b;c=d` |
| Retired from the build | `src/ng2_native_gpu.cpp`, `src/ng2_plume_renderer.cpp` | the M6 shadow renderer against NG2's own hand-off ABI; superseded by the RexNgpu* ABI; files kept |

## 2. Decisions taken today, and why

1. **Engine: import Fable's main wholesale rather than patch the fork.**
   `git am -3` could not build a fake ancestor against the fork's rewritten
   history, and patch 0001 collides with the fork's own hand-off ABI, which
   Fable's ABI supersedes. 8b4a025 and the fork's base 8cc841b are SIBLINGS
   off upstream v0.10.0 (not ancestor and descendant - claudecode-76's
   correction); the NG2 SDK features are the same code on both lines,
   checked by content. Only 15 files differ between the two lines; the whole
   diff was read for deletions, which is how the ring fix surfaced.
2. **The ring re-initialisation fix was NOT in Fable's line** (their title
   never re-inits the ring mid-run; NG2 does at every mode change). Ported
   back with the trigger condition in the commit message; the census proves
   it by `pointers reset`.
3. **Vendored tree taken from the kit verbatim, not re-vendored.** The fork
   is Fable's tree plus the ring fix, and the ring fix lives in the plugin's
   PM4 parser, which the transplant never runs (the Driver replaces the ring
   worker). Re-vendoring would reproduce the same bytes. The scripts are
   kept for a future re-vendor.
4. **Native window in raw D3D12, not Plume.** The backend needs an
   ID3D12Device and a direct queue, nothing more; Plume was Fable's renderer
   heritage. NG2's window is ~400 lines with one blit (fxc-built DXBC).
5. **Present at the swap, on the window's own thread**, exactly as the guide
   says (the guest's present hook showed an old frame; a window proc on the
   render thread went Not Responding).
6. **Reveal hold trigger for NG2 = the mode word at 0x84C25070 entering 3
   (gameplay) from 0 or 2** (front-end / chapter card). Mapped for the
   ultrawide work in v1.0.19; read from the exe through the kernel's memory.

## 3. What is NOT done

- **Nothing has run.** Compile-only, by the user's embargo. The first run
  will be lockstep beside the plugin (both render), then offload.
- **Neither DLL is built from the fork's `native-integration` branch yet.**
  A plugin/runtime pair carrying the RexNgpu* exports and the ring fix is
  needed before the first run. The Fable session runs back-to-back legs, so
  `D:/ng2_frameinterp/work/wait_then_build_pair.ps1` (detached, PID 119920 at
  17:14) starts `build_fork_pair.cmd` after the lock has read machine-free for
  three consecutive minutes; its log is beside it. Not on the critical path
  until the embargo lifts. The first-run launcher is
  `tools/native_gpu/run_native.ps1` (stages the pair with a backup and a
  STAGED marker, claims the lock, backs up the saves, kills only its own PID).
- **ng2.exe BUILDS with the whole transplant** (2026-09-26 17:20: 71 steps,
  0 errors, warnings only; the exe carries the ngpu_backend cvar, the RexNgpu*
  symbol names and the [ngpu-window] log strings, and the carry census reads
  every exe-side row present). It has never run.
- **Texture pack under offload:** the vendored `texture_cache.cpp` carries
  `[texpack]`, so the replacement path exists in the backend; the exe's
  `texture_pack_path` reaches it through the plugin's cvar registry
  (accessors). UNVERIFIED until a run.
- **Ultrawide under offload - SETTLED by reading the code (17:30).** The plugin's
  IssueSwap returns at its FIRST line under offload (d3d12/command_processor.cpp:2489),
  before its scene detection, so the plugin registry's `ng2_uw_mode` goes stale.
  The vendored IssueSwap runs the detection but `REXCVAR_SET` inside the vendored
  code writes an APP-LOCAL static (rtc_d3d12/flags.cpp accessor-only definitions),
  never the registry; and the vendored `ng2_fov_k` is read from the registry once.
  So the two values have TWO HOMES. Fixed: the native window reads the backend's
  copy (`ng2::ngpu::UltrawideMode()`), and `ApplyFov` pushes k into the backend's
  copy (`ng2::ngpu::SetFovK`) beside its registry write. The same shape will apply
  to every accessor-only cvar in the coming DLL - asked the Fable session for a
  get/set-copy entry in its C API. UNVERIFIED in a run.
- **The game's own window goes black under offload.** Fable accepted a
  second window; for a shippable NG2 the native output must eventually
  land in the game's window (a shared-handle copy into the runtime
  presenter, or hiding the main window) - the productisation phase, not
  parity.
- **Settings rows** for the native path: deliberately none yet (env levers);
  the lodestone census would demand documentation for a settings field, and
  the feature is not shippable until the gate passes.
- **The carry census on the integrated build** (folder census + `--compare`
  against `Releases/v1.0.24`) - after the SDK pair is built.

## 4. The verification order (when runs are allowed)

1. Stock plugin, `NG2_NATIVE_GPU=1`: the log must say the exports are
   missing and the game must run exactly as before (the no-op path).
2. Fork plugin pair, `NG2_NATIVE_GPU=1`, lockstep: `[ngpu] LOCKSTEP` lines,
   `diff self-check ... 0 mismatches` at `ngpu_backend_selfcheck_every=1`,
   `REGISTER COVERAGE` naming what NG2 changes outside the forwarded ranges
   (a title may differ from Fable), the native window showing the same
   picture as the game's. Picture: windiff native vs plugin on a PAUSED scene,
   with the cross-run floor from two same-config pairs measured FIRST.
3. `NG2_NATIVE_OFFLOAD=1`: the game's window black, the native one alive;
   `[swap] guest fps` at 60 with p50/p99/worst; texture pack, ultrawide,
   fades, audio, chapter transitions - the ledger's behavioural rows.
4. The carry census on the run folder and `--compare` against v1.0.24.

## 5. Peers

- Fable session "Fable 2 agnostic xenos to native PC" [302eea6b]: sent the
  kit; building the DLL; will message when the embargo lifts.
- claudecode-76 (manager): verified the user's instruction, un-parked NG2 in
  `blocked_teams.txt`, corrected the sibling premise, verified the ring
  finding on Fable's logs, flagged that 8b4a025 is unpushed (pinned here).

## 6. First legs (embargo lifted 17:31)

All through `tools/native_gpu/run_native.ps1` (stages the fork pair, claims the lock, backs up the
saves, `NG2_NATIVE_GPU=1`, kills only its own PID, restores). Logs beside the exe under `logs/`,
launcher logs in `D:/ng2_frameinterp/work/leg_*.log`.

| leg | what | result |
|---|---|---|
| lockstep1 17:31 | fork pair, native on, selfcheck 1 | NATIVE NEVER STARTED, and the run had NO tuning at all: the launcher's `$tune`/`-Tune` were one variable (PowerShell names are case-insensitive) and passed the self-check cvar twice; the TOML parser refused the duplicate and `rex::cvar::LoadConfig` applied nothing (one error line). The game still booted and rendered on the fork pair at the cutscene's 30 fps then 60 at the title. Fixed: launcher variable renamed; `Ng2Tuning::Apply` now folds duplicates (last wins) so one repeated key can never drop the config again |
| lockstep2 17:36 | same, tuning fixed | native window up, DLL loaded (ABI 1), then 0xC0000005 in ngpu_backend.dll+0x7645 = `Driver::Init` at `KernelState::memory()`: `kernel_state()` was NULL because NG2 started the native path in `OnCreateDialogs` (UI creation), before the kernel exists. Fixed: `ng2::ngpu::Start` moved to `OnPostSetup`, after `ApplyFov` (where Fable starts it). Symbolised with llvm-symbolizer on the DLL's pdb from the Windows event log's fault offset |
| lockstep3 17:39 | same, start in OnPostSetup, 200 s, offload OFF, selfcheck EVERY draw | **FIRST NATIVE NG2 FRAMES.** DLL started in manual mode on NG2's device; `[ngpu] LOCKSTEP (dll)` at 17:42:57: 791,138 draws seen, 791,138 fed, 0 failed, 8,701 swaps, self-check 799,838 runs / 0 mismatches, window presented 8,700 of 8,700 requests, 0 waits timed out. Both windows showed the same title frame 11 ms apart (screens 17:40:21). Rates as pairs, plugin line: intro/attract segment 30.0 fps in both leg 1 (no native) and leg 3 (lockstep), title 60.0 in both - lockstep did not slow the game where the two legs overlap; lockstep legs are not timing-valid anyway (both renderers use the GPU). DLL diagnostics per frame at the logos: GPU TIME 0.12 ms, CPU COST plugin GPU thread 2.6 ms + submit thread 0.4 ms, 4 upload batches hoisted |

Observed, not yet acted on: the native window opens on monitor index 0 in EnumDisplayMonitors order,
which is the leftmost monitor here, while the game's window uses `ng2_platform`'s ordering (primary);
align the window module with `ng2::MonitorFullSize`'s index semantics. Each `[swap]` line appears
twice in the log under the DLL: the vendored command processor reports swaps too (same numbers).

Next, in the Fable order: the static-scene picture check with the floor first
(`windiff2.py --pairs 2` on the main menu opened by the launch-time pad script, `-PadScript
"45:start,48:start" -DiffAt 70`), then offload ON with a plugin-alone baseline interleaved on the
same scene (title + attract demo, no input) for the timing pair.

| menu1 17:44 | lockstep, pad script START at 22 s and 34 s (main menu), windiff2 at +60 s, 2 pairs | native vs game mean abs diff 0.0209 / 0.0208 (pixels over 0.1: 4.6% / 4.5%); FLOOR from the same window 2.3 s apart: native vs native 0.0163, game vs game 0.0163 (over 0.1: 0.6%). The menu's fog moves, so the floor is high; the excess over it is 0.0045 and 4 points of over-0.1 pixels - close, NOT parity-proven. Client sizes differed (native 1276x728 on the 1.0-scale monitor, game 1273x720 on the 1.25-scale primary), a resampling term to remove by placing the native window on the game's monitor. A truly static subject (a paused gameplay scene from a save, or the options screen) decides it. Captures in D:/ng2_frameinterp/captures/menu1_* |

### The first timing pair (17:47-17:58): baseline / offload / baseline / offload

Same fork pair, same exe, one variable (`NG2_NATIVE_GPU` + `gpu_offload_to_native`), 150 s each from
boot with no input (logos, intro cutscene, title, attract), interleaved. `tools/native_gpu/abfps2.py`
over the [swap] windows from +40 s (21 windows each):

| leg | fps mean | median | p50 ms | p99 ms | worst ms | hitches | native GPU ms/frame | native CPU on the plugin GPU thread |
|---|---|---|---|---|---|---|---|---|
| base1 (no native) | 42.00 | 30.0 | 33.20 | 35.40 | 36.6 | 1 | - | - |
| off1 (offload) | 42.02 | 30.0 | 33.20 | 35.30 | 36.5 | 3 | 0.07 median, 0.35 max | 2.29 median (submit thread 0.21) |
| base2 (no native) | 42.01 | 30.0 | 33.30 | 35.50 | 37.1 | 0 | - | - |
| off2 (offload) | 42.00 | 30.0 | 33.20 | 35.10 | 36.6 | 0 | 0.07 median, 0.35 max | 2.55 median (submit thread 0.31) |

Both offload legs: ~607,000 draws fed, 0 failed, 0 self-check mismatches (every 1024th draw), every
present served; the game's own window black, the native window carrying the game (screens 17:52:01).

READ IT AS: the sequence is CAPPED by the game (cutscene segments at 30, title at 60) and light, so the
rates cannot show headroom - Fable's rule about the cap. What it does show: under offload the native
backend is the only GPU at a per-frame cost of 0.07-0.35 ms GPU and ~2.4 ms CPU, and the pair is
indistinguishable from the plugin-alone baseline at every percentile. It is NOT the "speed bump"
measurement: that needs a heavy gameplay scene (a save, a fixed route) where the plugin path shows its
cost, measured as GPU TIME / CPU COST / p99 / worst beside a plugin-alone baseline.

Also learned: the eye tool's "stable for 4 s" fired on the intro's slow fog shot while the swap counter
kept advancing - on this title a stability capture is not a freeze without the counter beside it.

### The picture, decided (18:03-18:05, leg pause2, lockstep, DLL cc620728)

Route by NG2's own pad script (no desktop input): START 22 s, START 34 s (menu), A 40/46/52/58 (new
game, difficulty), START 64/70 (cutscene skip); the chapter card auto-proceeds; Chapter 1 gameplay from
about +75 s; START through the live pad file pauses (the weapons screen). Native window mirrored the
game's client (1273x720 both) on the same monitor. `windiff2.py`, captures 0-1 ms apart:

| subject | native vs game mean abs diff | pixels over 0.1 | same-window floor across ~2.3 s |
|---|---|---|---|
| Chapter 1 gameplay, in combat (moving) | 0.0053, 0.0110 | 1.8%, 3.6% | 0.0805 / 0.0830 (32-33%) |
| pause screen (weapons; animated background) | 0.0002, 0.0010 | 0.0%, 0.1% | 0.0258 / 0.0251 (26%) |

Read: on the pause screen the two windows show THE SAME FRAME to within 0.0002-0.0010 - below Fable
II's 0.0030 (theirs carried a client-size resampling term; these clients are identical). In moving
gameplay the simultaneous pairs sit far below the motion floor, so the at-most-one-frame present offset
is all that separates them. Captures in `D:/ng2_frameinterp/captures/play1_*`, `wmenu1_*`.

Chapter 1 itself: 11.3 M draws fed in the first ~2 min of gameplay (about 2,700 per frame), 0 failed,
11,048 self-checks / 0 mismatches, 8,382 of 8,382 presents, 18 frames held by the DLL's reveal hold at
the card -> gameplay transition (the card's capture read black on the native side for that reason: the
hold kept the fade's last frame; not a capture fault - the gameplay captures read the native window
fine). FIRST NATIVE NG2 GAMEPLAY: HUD, blood, blossoms, the Tokyo rooftops, enemies - all through the
transplanted backend, beside the plugin.

Route trap: the chapter card does NOT wait for A here (it auto-proceeds), so a static-card diff has to
be taken during "NOW LOADING" (the flower animates in one corner) or not at all.

### Corrections to the two sections above (claudecode-76's review, 18:12)

**The boot-sequence pair, read properly.** "fps mean 42.00" is the MEAN OF 5-SECOND WINDOW RATES over a
bimodal sequence - 8 windows at 60 (title), 12 at 30 (cutscene) and one 42.4 transition window, in every leg:
(480 + 360 + 42.4) / 21 = 42.02 - and the "p50 33.2 ms"
is the median of the windows' own p50s. Two statistics over windows, not one frame distribution.
Phase-separated (`abfps2.py` now prints this):

| phase | leg | p50 ms | p99 ms (median / max) | worst ms | hitches |
|---|---|---|---|---|---|
| 60 fps (n=8) | base1 / off1 / base2 / off2 | 16.75 / 16.70 / 16.80 / 16.70 | 19.00/19.2 / 18.90/19.1 / 18.95/19.1 / 19.05/19.3 | 19.8 / 19.5 / 19.7 / 19.7 | 0 / 0 / 0 / 0 |
| 30 fps (n=13) | base1 / off1 / base2 / off2 | 33.30 / 33.30 / 33.40 / 33.30 | 35.70/36.5 / 35.60/36.2 / 35.60/36.6 / 35.60/36.1 | 36.6 / 36.5 / 37.1 / 36.6 | 1 / 3 / 0 / 0 |

So: the sequence is pinned at the game's own 60 and 30, and the offload legs sit on the same frame-time
distribution as the plugin-alone legs at every percentile - no headroom visible, no cost visible. A mean
over a bimodal run is not a frame rate; it is not quoted again.

**The picture numbers, labelled properly.** The "floor" of 0.025 / 0.08 is CROSS-INSTANT (the same
window 2.3 s apart): it is the animation term, not noise. The signal is SAME-INSTANT lockstep. The capture
path's own same-image floor is 0.0000 (a static frame re-captured), so 0.0002-0.0010 on the pause screen is
a real, tiny difference (first suspect: the at-most-one-frame present offset between the plugin's presenter
and the native window). The claim that holds: the native-vs-plugin difference is 25-125x smaller than one
frame of the plugin's own animation, so no observer could attribute it to the renderer rather than to the
animation. A same-instant plugin-vs-plugin pair cannot exist (one plugin window); Fable's cross-RUN floor at
a paused scene is the honest substitute and needs a scene NG2 can hold still - open.

**Page-state coherency, a number to test against (from Fable II via claudecode-76, 18:15, corrected 18:17).**
Fable's `nocleanpg` A/B, same plugin, same offload, one cvar, screened for third-party GPU load, TWO rounds:
plugin GPU thread CPU OFF 13.23-14.06 vs ON 15.99-16.22 ms/frame (round 1), OFF 14.04-14.97 vs ON 15.73-16.27
(round 2) - separated in both, about 2 ms, with the mechanism beside it: uploads 1.3 vs ~7 GB per 5 s in
both rounds. A stutter/worst-frame figure first sent with it was WITHDRAWN by its sender (third-party GPU
interference in the contaminated windows, not the cvar) and is not recorded here. NOT transferable as a
default: on NG2 `clear_memory_page_state` is a coherency REQUIREMENT (`ng2_tuning.h`: without it Team Ninja
titles lose character models; the pool race only became reachable through it). It says roughly what NG2 pays
for that coherency. An NG2 A/B of it must be judged on the PICTURE (character models present, `windiff2`
pairs) before any millisecond is read, and only once the native backend's own coherency under offload is
understood. Pin stays true.

**Per-draw cost, an observation (claudecode-76, 18:17), not a defect claim:** the same vendored layer runs
Fable II's town at ~14.0 ms CPU for ~3,400 draws (4.12 us/draw; GPU 11.8 ms) and NG2's Chapter 1 at ~4.8 ms
for ~2,700 draws (1.78 us/draw; GPU ~1.05 ms). Draws are not fungible across titles (scene, state changes,
resolution, texture loads), but NG2 should not expect to inherit Fable's CPU cost from the layer itself.

### The gameplay timing pair (18:07-18:20): Chapter 1 combat, baseline / offload / baseline / offload

Same fork pair, same exe, same pad script into Chapter 1 (gameplay from ~+75 s), 180 s each, interleaved,
one variable. `abfps2.py --from 80` (19 windows each, a single 60-fps phase in every leg):

| leg | fps median (min-max) | p50 ms | p99 ms median / max | worst ms | hitches | native GPU ms/frame | native CPU on the plugin GPU thread (submit) |
|---|---|---|---|---|---|---|---|
| gbase1 (plugin alone) | 60.0 (59.0-60.0) | 16.60 | 19.00 / 30.5 | 31.0 | 0 | - | - |
| goff1 (offload) | 60.0 (60.0-60.0) | 16.60 | 19.00 / 19.5 | 19.8 | 0 | 1.09 | 5.47 (0.83) |
| gbase2 (plugin alone) | 60.0 | 16.70 | 18.90 / 19.6 | 20.1 | 0 | - | - |
| goff2 (offload) | 60.0 | 16.60 | 19.00 / 29.8 | 30.5 | 0 | 0.96 | 4.43 (0.62) |

Offload legs: 17.1 M and 10.6 M draws fed over 10,501 swaps each (the combat evolved differently - draws
per frame are not fixed by the route), 0 failed, 0 self-check mismatches, 18 frames held at the reveal.

READ: at every percentile the offload legs sit on the plugin-alone distribution; the worst-frame band has
one ~30 ms frame in one leg of EACH arm, so it has no direction at n=2. NG2's plugin path already holds 60
here, so **there is no speed bump to measure at the cap**; the native path's cost is ~1 ms GPU and 4.4-5.5
ms CPU per frame, and what it can buy on this title is HEADROOM - which shows only when the GPU is made the
limiter. Next pair: the same route at 3x internal resolution (draw_resolution_scale 3), both arms.

### The headroom pair at 3x internal resolution (18:20-18:34): x3base1 / x3off1 / x3base2 / x3off2

Same route, `NG2_TUNE=draw_resolution_scale_x=3;draw_resolution_scale_y=3` (the app's own line confirms
"internal scale 3x3" in every leg), 180 s each, from +80 s, 19 windows, one 60-fps phase in all four:

| leg | fps median | p50 ms | p99 ms median / max | worst ms | hitches | native GPU ms/frame | native CPU (submit) |
|---|---|---|---|---|---|---|---|
| x3base1 | 60.0 | 16.80 | 19.00 / 30.1 | 30.2 | 0 | - | - |
| x3off1 | 60.0 | 16.60 | 19.00 / 30.2 | 30.6 | 0 | 3.96 (max 4.00) | 4.58 (0.52) |
| x3base2 | 60.0 | 16.90 | 18.90 / 30.3 | 30.6 | 0 | - | - |
| x3off2 | 60.0 | 16.60 | 19.00 / 30.6 | 39.9 | 1 | 4.02 (max 4.14) | 4.63 (0.76) |

Offload legs 11.8 M and 11.9 M draws fed, 0 failed, 0 mismatches. Even at 3840x2160 internal neither arm
is GPU-limited on this RTX 5090: the native backend's GPU work rises from ~1 ms to ~4 ms per frame and the
game stays pinned at 60 with the same percentiles in both arms; one ~30 ms frame appears in every leg.

**THE SPEED CONCLUSION, stated plainly for the user:** Fable II's "huge speed bump" (31 -> 60 in its town)
came from Fable being CPU-bound on the plugin path. NG2's plugin path already holds 60 in every scene
reached today (boot, title, attract, Chapter 1 combat at 1x and at 3x internal), so on this title and this
hardware there is NO frame-rate gain to take from the native path at the cap; its cost is small (~1 ms GPU
at 1x, ~4 ms at 3x, 4.4-5.5 ms CPU on the plugin GPU thread) and its picture matches to 0.0002-0.0010. What
the native path gives NG2 is the same features on a native, optimisable layer with headroom to spare -
the user's stated destination - not a higher number on the counter. A scene where the plugin path drops
below 60 (later chapters via a save; the withdrawn "chapter 12 ~28 fps") is the only place a speed
difference could still appear, and it has not been measured.

### The last carry leg, pre-registered (18:40; runs after the Fable session's 25-minute slot)

The user's own configuration, from the portable install's settings: fullscreen 3840x1600 on monitor 0 (2.4:1),
`ultrawide=1`, texture pack on at 2x (AI, `D:/Ninja Gaiden 2 Portable/textures`, 24,868 .tex under pack/, no
stages/ lists so no warm bar), under OFFLOAD, into Chapter 1 by the pad script. These settings have no NG2_*
environment override (only WIDTH/HEIGHT/FPS/SCALE/FULLSCREEN/GAME/DLC/NO_SETUP/IMPORT_SAVE do), so the launcher's
new `-Settings` patches `ng2_settings.cfg` for the run and restores it.

Pass criteria, stated before the run:
- ultrawide presenter half (ledger 1.2): `[ngpu] LOCKSTEP (dll)` reports `uw mode 1 fov_k 0.74` in gameplay
  (k = 1.7778/2.4), `[ngpu-window] presenting ... (uw mode 1, viewport 3840x1600 at 0,0)`, and the native
  window's picture at 3840x1600 shows correct proportions with the HUD as a centred 16:9 band; the game's own
  window black. Menus / the pause screen: mode 2, a pillarboxed 16:9 viewport.
- texture pack under offload (ledger 1.1): `[texpack] '<dir>': N hashed files indexed` from the DLL's texture
  cache and non-zero replacement counts in gameplay; no `[texpack]` error lines.
- everything else unchanged: 0 failed draws, 0 self-check mismatches, no stall.
