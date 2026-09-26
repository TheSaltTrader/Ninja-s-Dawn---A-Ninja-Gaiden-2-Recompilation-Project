# Carry-forward ledger — what the native integration must keep

Written 2026-09-26 on branch `native-gpu-ng2` (the alt branch), the day the
user said the Fable II Xenos layer had gone native and that its game-agnostic
DLL would be handed to NG2. The user's requirement, verbatim:

> "Just make sure we carry all the functionality we fought hard for like the
> texture upscale, ultra wide screen support, fade screen fixes, 60 fps and
> all the fixes we had to do on the sdk once we receive the handoff."

This file enumerates that functionality, says where each piece LIVES (exe,
runtime DLL, plugin DLL, codegen) and how its presence is PROVEN, so the
integration can be checked item by item instead of remembered. The check is
`tools/native_gpu/carry_census.py <folder>`; it reads the built files and
counts what it could not measure.

**The rule this ledger exists for:** presence is proven by CONTENT, never by
origin. v1.0.22 shipped without ultrawide because the packaging check compared
where each DLL came from rather than what was inside it (`HANDOFF.md` §8, §9).

**Testing embargo (user, 2026-09-26):** nothing here that needs the game to run
may be done until the Fable team has finished its optimisation testing. Every
"behavioural" proof below is deferred until then; the string proofs are not.

---

## 0. The baseline — what "everything" means, by identity

| what | identity |
|---|---|
| Release | v1.0.24 (2026-09-22), public, `Releases/v1.0.24`, portable copy `D:\Ninja Gaiden 2 Portable` |
| `rexgpu-xenos.dll` | md5 `399f9d0cf0157d569210aad7c2f63f4e`, 7,277,568 B, built 2026-09-22 13:45 |
| `rexruntime.dll` | md5 `86d407efda2d51ceecdc71eb2713ae1d` (the v1.0.20 runtime, 2026-09-14, "known good" in `make_release.py`) |
| SDK source of the shipped plugin | `rexglue-src` branch `hotfix-0.2.13-plugin`: upstream v0.10.0 (`c94f5eb`) -> `8cc841b` snapshot of the shared working tree (the v1.0.20 boot-stable base, 69 files / 6,635 insertions over upstream) -> UploadCopyPool fix `87e28df` + `f1a0dd3`. The later `c4ff4bc` (freed pages, 09-24) post-dates the shipped build. |
| Exe source | `ng2recomp` `main` at `ef5468d` (v1.0.24 + docs). Merged into `native-gpu-ng2` as `6c86551` on 2026-09-26, clean, no conflicts. |
| Same bytes in three places | `Releases/v1.0.24`, `RexBlue/win-amd64/bin`, the portable install: identical md5 for both DLLs (checked 2026-09-26). |

The snapshot `8cc841b` is the SHARED tree: it holds NG2's SDK work AND Fable
II's of that date. Not every one of its 69 files is NG2's, but every one is in
the bytes NG2 shipped, so the integrated build must be judged against the same
behaviour, not against a guess at attribution.

---

## 1. The user's named items

Home key: **EXE** = ng2.exe (`src/`), **RT** = rexruntime.dll, **PL** =
rexgpu-xenos.dll, **CG** = codegen (rexglue.exe + generated C++).

### 1.1 Texture pack and AI upscale

| piece | home | proof of presence |
|---|---|---|
| pack replacement by texture id (address + shape, v1.0.6 collision fix), live F9 toggle, dump | PL `src/graphics/d3d12/texture_cache.cpp` (+1,820 lines over upstream) | strings `[texpack]`, `texture_pack_path`; log `[texpack] '<dir>': N hashed files indexed` |
| per-stage warm with the on-screen bar (v1.0.0) | PL texture_cache.cpp + EXE overlay | string `warmed stage`; log `[texpack] warmed stage N: F files, M MB` |
| mip chains for replacements (compute pass) | PL `shaders/texpack_mip.cs.hlsl` | string `texpack_mip` |
| the pack tool, Lanczos + Real-ESRGAN at native 4x then down (v1.0.1), only-missing (v1.0.2), cancel via job object (v1.0.1), progress phases, bundled upscaler default (v1.0.14) | EXE `ng2_textool.cpp`, `ng2_menu.cpp`; `tools/upscale_textures.py`, `tools/ai_upscale.py` | exe strings `Real-ESRGAN`, `only-missing`; the tools ship in the zip (v1.0.0 "the release shipped no tools" fix) |
| settings: texture_pack, texture_dump, texture_scale, texture_ai, texture_ai_strength, texture_path, texture_cache_mb | EXE `ng2_settings.h` | `python tools/lodestone_census.py` (enumerates every field) |

**Re-home risk under a native layer: HIGH.** The replacement lookup sits on
the plugin's texture upload path. If the agnostic layer uploads textures
itself, the lookup (and the warm, and the mip pass) move with it, or the
feature is gone while every string above still reads present in the old DLL.
The census therefore checks the DLL the exe LOADS, not any DLL in the folder.

### 1.2 Ultrawide (3D)

| piece | home | proof |
|---|---|---|
| projection widen: column 0 of the WVP scaled by `ng2_fov_k`, projection block found per vertex shader, cached per shader (the cache is what keeps 60 fps; without it 60 -> ~40) | PL `command_processor.cpp` (UpdateBindings) | strings `ng2_fov_k`, `NG2_FOV_K` |
| scene detection from the game's own pause flags (`0x84C29930`, `0x84C39A4C`, both == 1) and `has_world = cnt3d >= 12`; writes cvar `ng2_uw_mode` 0/1/2 at swap | PL (IssueSwap) | strings `ng2_uw_mode`, `NG2_UW_NOPAUSE`, `NG2_DUMP_VSCONST`; log `[ng2uw] CHANGE -> mode=...` |
| presenter: reads `ng2_uw_mode` BY NAME across the DLL boundary; 1 = fill, 2 = pillarbox, 0 = user's Keep-aspect | RT `src/ui/presenter.cpp` | string `ng2_uw_mode` in the RUNTIME (this is the half v1.0.22 lost) |
| HUD stays a centred 16:9 band in gameplay (c21 ortho column 0 also scaled) | PL | same site as the widen |
| the guest is always told 16:9 (largest 16:9 box, v1.0.1) + `video_mode_explicit` so an explicit 1280x720 is honoured | EXE `ng2_app.h` ApplyDisplaySettings; RT `window.cpp`, `xboxkrnl_video.cpp`, `ui/flags.h` | runtime string `video_mode_explicit` |
| `ApplyFov()` sets `ng2_fov_k = (16/9)/monitorAspect`, live from the menu toggle | EXE | exe string `ng2_fov_k`; setting `ultrawide` in the census |
| KNOWN LIMIT carried as-is: full-screen 2D (cards, menus, credits) stays 16:9; render surface stays 16:9 (a wide surface breaks the resolves) | — | README "Known issues" |

**Re-home risk: HIGH.** The widen is applied where vertex constants are
uploaded per draw. A native layer that uploads constants itself must apply
`k` there with the same per-shader cache, and must keep writing `ng2_uw_mode`
or the presenter has nothing to read. Two DLLs, two halves, both required.

### 1.3 Scene fades (v1.0.20)

| piece | home | proof |
|---|---|---|
| a full-screen TEXTURELESS 2D draw (the fade) is left uncompressed so it covers the whole width; textured 2D (HUD) still compresses. `is_solid_2d = kind==2 && no texture bindings` | PL `command_processor.cpp` UpdateBindings, same block as the widen | string `solid2d`; log `[uw-2d] solid-fill 2D draws left full width` |
| menu transition layer kept full width (v1.0.19) | PL | log `[uw-menu] transition layer quad kept full width` |

**Re-home risk: HIGH** (same site as 1.2). Behavioural proof: fade-in and
fade-out full width on an ultrawide monitor, HUD and weapons menu unaffected.

### 1.4 60 fps at the correct game speed

| piece | home | proof |
|---|---|---|
| frame-rate options above 60 and the V-Sync row REMOVED (v1.0.21): both were game-SPEED controls; `Clamp()` and the tuning force vsync on; `fps` field clamped | EXE `ng2_settings.h`, `ng2_tuning.h`, `ng2_menu.cpp`; `tools/settings-ledger.json` rows `vsync`, `fov_scale` | lodestone census passes; README offers no rate above 60 |
| frame interpolation removed (v1.0.21) - do not revive | — | `ng2-frame-interpolation` memory: OPTION A CLOSED |
| the per-shader FOV cache (1.2) - the ultrawide feature costs nothing | PL | measured, not stringed |
| the UploadCopyPool race fix (v1.0.22): NG2 reaches the pool constantly (`clear_memory_page_state` on, ~17 MB/frame); the unfixed pool hangs the GPU worker at ~13 swaps | PL `shared_memory.cpp` (`87e28df`, `f1a0dd3`) | **NO STRING PROOF EXISTS** - the fix added no log text. Proof today is ancestry (the shipped DLL was built from `hotfix-0.2.13-plugin`) plus behaviour (survives 240 s where the unfixed plugin froze 2/2). ACTION when re-homed: add a marker string so the census can see it. |
| `clear_memory_page_state=true` for NG2 (tuning) - NG2 is the only title on this path; the readback fast-path fix `74e33d2` is gated and defaults to what shipped | EXE tuning; PL | plugin string `clear_memory_page_state` |
| correct game speed = the guest advances on vblank at 60 Hz | — | `[swap] guest fps` in the log reads 60 in gameplay, never the overlay (the overlay fix of v1.0.17 ticks on the guest frame hook at `0x822F3A8C`) |

**Behavioural proofs deferred** until the Fable team's testing is done. Report
p50/p99/worst/hitches beside the mean, per `RELEASE_GATE.md` clause A.

### 1.5 The SDK fixes ("all the fixes we had to do on the sdk")

The full list is the diff `c94f5eb..8cc841b` (69 files; saved beside this
file as `CARRY_FORWARD_sdk_diff.txt`) plus the pool fix. The NG2-attributable
ones, by area, with the proof each has:

| fix | home | proof |
|---|---|---|
| stuck-wait watchdog on ALL five kernel wait paths + critical sections (`WaitMark`), 30 s / 300 s reports | RT `xboxkrnl_threading.cpp` (+176), `threading.h`, `xboxkrnl_rtl.cpp` | runtime string `watchdog`; log `[watchdog] guest thread N has been waiting Ns in ...` |
| `db16cyc` translated to nothing -> `REX_SPIN_YIELD` / `rex_spin_yield()` | CG `src/codegen/builders/system.cpp`, `resources/templates/codegen/pch_h.inja` | the GENERATED source contains `rex_spin_yield` (1 file); it is inlined, so the exe carries no string - the codegen tool must be built from the snapshot or later and the 1,109 files regenerated with it |
| `video_mode_explicit` cvar (an explicit mode equal to the default was treated as "not configured") | RT | runtime string `video_mode_explicit` |
| presenter letterbox override via `ng2_uw_mode` | RT `presenter.cpp` | runtime string `ng2_uw_mode` |
| ring re-initialisation race (attract demo) + `RINGDUMP` ring-state dump on a bad packet | PL `command_processor.cpp` | plugin string `RINGDUMP` |
| UploadCopyPool race | PL `shared_memory.cpp` | none (see 1.4) |
| texture cache: pack, id = address + shape, dump refuses non-art, per-stage warm, mip pass, memory limits verifiably applied | PL `texture_cache.cpp` | strings in 1.1; cvars `texture_cache_memory_limit_soft/hard` |
| fuzzy alpha + accurate depth reach the plugin (the tuning brace bug, v1.0.0) | EXE `ng2_tuning.h` + PL cvars `use_fuzzy_alpha_epsilon`, `depth_float24_*` | lodestone DELIVERY sweep |
| input: d-pad keybinds, `hid_mappings_file`, `mnk_mode` | RT `input_system.cpp` (+26) | cvars emitted by the tuning (see §3) |
| audio: `audio_maxqframes` | RT `sdl_audio_driver.cpp` (+22) | cvar in §3 |
| kernel/xam: content, io, xmemory (+146), function_dispatcher (+56), `protect_zero` | RT | cvar `protect_zero`; the Chapter 5 thunk crash fix (v0.5.1) and the ~90 s crash (v0.5.3) live here or in codegen - re-run the game's own crash census when testing is allowed |
| chapter 13 boss crash (v1.0.4) and the ten unregistered virtual thunks (v0.5.1) | EXE hooks / `ng2_manifest.toml` function list | exe hook strings (§2); `Unresolved b target` lines in codegen output are the two known CRT ones only |

Items whose attribution is uncertain are still carried: the shipped bytes are
the baseline, and `carry_census.py --compare` diffs the string sets of two
folders so an unexplained loss is at least VISIBLE.

---

## 2. Guest hooks (exe side, `config/hooks/patches.toml`)

| hook | address | what it protects | string in ng2.exe (v1.0.24) |
|---|---|---|---|
| `ng2PatchRenderSize1` / `2` | `0x836261E8`, `0x837C62FC` | internal render size | not a string (name lives in TOML only) - proven by the census reading the TOML |
| `ng2PatchChapter12` | `0x82834C78` | the readability guard, `force = cvar` only (v1.0.3) | `ng2PatchChapter12` 1, `chapter-12 guard` 1 |
| `ng2PatchSkipVideos` | `0x8380EB9C` | skip intro videos | `ng2PatchSkipVideos` 1 |
| `ng2AudioBarrierFix` | `0x8374F164` | the music death: the game's own rendezvous race (v1.0.2) | `ng2AudioBarrierFix` 1, `audio barrier` 1 |
| `ng2ChapterAwardFix` | `0x8242D7B4` | chapter 12 -> 13: the achievement-write state the port never set (v1.0.6) | `ng2ChapterAwardFix` 1 |

All six are in the merged alt branch (`6c86551`). They are exe-side and do not
depend on the GPU layer; the risk is only that a re-generated exe loses a hook
- the census reads the TOML and the exe together.

---

## 3. SDK cvars the exe drives (`src/ng2_tuning.h`) - each needs a decision

Every one of these is a setting the player reaches. Under the agnostic layer
each is either honoured by the new layer, mapped to its equivalent, or
consciously retired with a settings-ledger reason. None may silently become a
no-op (that is the v0.5.4 "inert settings" defect in a new coat).

    GPU/plugin:  resolution_scale  draw_resolution_scale_x/y  render_target_path_d3d12  rov
                 readback_resolve  readback_memexport  clear_memory_page_state  d3d12_queue_priority
                 anisotropic_override  use_fuzzy_alpha_epsilon  depth_float24_convert_in_pixel_shader
                 depth_float24_round  texture_cache_memory_limit_soft/hard  texture_pack_path
                 texture_dump  texture_dump_path  draw_census  dump  pack  swap_post_effect
    presenter:   present_letterbox  present_dither  vsync  (ng2_uw_mode written by the plugin)
    runtime:     audio_maxqframes  hid_mappings_file  mnk_mode  keybind_dpad_up/down/left/right
                 protect_zero  video_mode_explicit

---

## 4. Where the re-homing will happen (measured overlap)

Files that BOTH NG2's snapshot changed (over upstream) AND Fable's `main`
changed after the snapshot - the places an integration will conflict or where
NG2 logic sits in code the native layer replaces:

    include/rex/graphics/command_processor.h        src/graphics/command_processor.cpp
    include/rex/graphics/d3d12/command_processor.h  src/graphics/d3d12/command_processor.cpp   <- ultrawide + fades + RINGDUMP
    include/rex/graphics/flags.h                    src/graphics/shared_memory.cpp
    include/rex/graphics/shared_memory.h            src/graphics/d3d12/shared_memory.cpp        <- UploadCopyPool
    include/rex/system/xmemory.h                    src/system/xmemory.cpp
    src/graphics/pipeline/render_target/cache.cpp

`texture_cache.cpp` is NOT in the overlap (Fable's main did not touch it after
the snapshot), so the texpack code merges cleanly as source - but see the
re-home risk in 1.1: merging cleanly is not the same as still being on the
path the new layer takes.

---

## 5. The verification procedure

1. **String census, now:** `python tools/native_gpu/carry_census.py <folder>`
   on the folder the exe runs from. It refuses to report on a DLL whose control
   string is absent, prints PRESENT / MISSING / UNMEASURED per item, and exits
   non-zero on any MISSING. `--compare <baseline> <candidate>` lists every
   ledger string the candidate lost.
2. **Settings census:** `python tools/lodestone_census.py` on the alt branch.
3. **Release gate:** `tools/make_release.py` `check_ng2_features()` already
   refuses a runtime without `ng2_uw_mode` and a plugin without `ng2_uw_mode`
   + `solid2d`. Extend its `SDK_FEATURES` with whatever the new layer's DLL
   must carry, and add the pool-fix marker once one exists.
4. **Behavioural, DEFERRED until the Fable team is done** (user's embargo):
   `[swap] guest fps` 60 in Chapter 1 gameplay with p50/p99/worst/hitches;
   `[ng2uw] CHANGE -> mode=1` and a full-width fade on an ultrawide monitor;
   `[texpack] warmed stage` on a chapter revisit with the pack on; no
   `AUDIO BARRIER STUCK` from `watch_ch13.py` over an idle title screen; a 240 s
   run with `shared_memory_upload_threads=3` that does not stop at ~13 swaps;
   the chapter 12 boss handing over to chapter 13.

## 6. Status (fill in as the integration proceeds)

| folder | census result | date |
|---|---|---|
| `Releases/v1.0.24` (baseline) | 24 present, 0 missing, 3 unmeasured (pool race fix, and the two 60 fps behavioural rows), 0 unreadable, of 27 rows | 2026-09-26 |
| alt worktree `out/build/win-amd64-Release` (09-17 DLLs) | 17 present, 7 MISSING: plugin lacks ng2_fov_k, ng2_uw_mode, NG2_UW_NOPAUSE, solid2d, texpack_mip, shared_memory_upload_threads; runtime lacks ng2_uw_mode. Those DLLs predate ultrawide and the pool fix and are NOT a deliverable; the census names exactly what the v1.0.22 gate missed | 2026-09-26 |
| integrated build | — | — |
