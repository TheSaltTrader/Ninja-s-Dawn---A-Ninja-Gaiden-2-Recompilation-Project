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
| Fable's kit | `claudecode\NATIVE_GPU_MIGRATION_KIT` | received 16:36; the game-agnostic `ngpu_backend.dll` is STILL COMING |
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
  needed before the first run; a full SDK build is heavy, so it waits for a
  machine-free window.
- **ng2.exe BUILDS with the whole transplant** (2026-09-26 17:20: 71 steps,
  0 errors, warnings only; the exe carries the ngpu_backend cvar, the RexNgpu*
  symbol names and the [ngpu-window] log strings, and the carry census reads
  every exe-side row present). It has never run.
- **Texture pack under offload:** the vendored `texture_cache.cpp` carries
  `[texpack]`, so the replacement path exists in the backend; the exe's
  `texture_pack_path` reaches it through the plugin's cvar registry
  (accessors). UNVERIFIED until a run.
- **Ultrawide under offload:** the plugin still runs IssueSwap's scene
  detection and writes `ng2_uw_mode`? The vendored IssueSwap contains it
  too. Which one runs under offload decides where the value comes from;
  the window reads the registry either way. UNVERIFIED (asked the Fable
  session).
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
