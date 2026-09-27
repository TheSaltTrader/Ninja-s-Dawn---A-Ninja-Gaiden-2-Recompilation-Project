# v1.0.25 night work: the ultrawide fade and the streaming stutter (2026-09-26)

The user's verdict on the v1.0.25 test build (22:20): "lost the full screen fade in ultra wide when pressing
select or start and after a video intermission the fade is not ultrawidescreen", and "a lot of stutter, can we
optimize the game like what fable is doing and keep the fps at 60 with no drops and optimize the game for lower end
pcs". Later: "The start fade should do a full screen ultrawide fade when returning to the game, right now only the
menu fades back and the sides appear abruptly." Everything below was measured on this machine (RTX 5090, the user's
settings: 3840x1600 fullscreen, Ultrawide (3D), pack at 2x AI) unless stated; the plugin lineage is the shipped one
(rexglue-src `hotfix-0.2.13-plugin` f1a0dd3, worktree `D:\ng2_frameinterp\rexglue-v1025`, branch `ng2-v1.0.25`).

## 1. The fade: a measured mechanism, not a lineage regression

- v1.0.24's plugin and the v1.0.25 rebuild are string-identical in the ultrawide/fade code, and `git diff f1a0dd3
  8545b81 -- src/graphics/d3d12/command_processor.cpp` has no hunk in the scene detection: every generation since
  v1.0.19 carries the same detector.
- The mechanism (ng2_005.log, the v1.0.20 plugin, NG2_DUMP_VSCONST timeline, 22:44:08-21): with the weapons menu
  OPEN the two guest "paused" flags toggle at about 1 Hz, so `ng2_uw_mode` flipped 1/2/1/2 every 0.5-1.8 s and the
  presenter swung between full width and pillarbox; on return the fade played inside the 16:9 band and the sides
  popped in. The v1.0.20 pause picture (22:44:15) is the weapons menu pillarboxed.
- Fix (rexglue-v1025 64422ce): the pause flags no longer pillarbox by default (`NG2_UW_PAUSE_16_9=1` restores the
  v1.0.19-24 behaviour); a frame that draws the solid fade counts as ultrawide even with no world behind it, so
  the fades that bracket a video or a scene change cover the width. In-engine cinematics were already ultrawide:
  ng2_007's timeline shows mode 1 unbroken across the cinematic-to-gameplay handover (3d=412..175).
- Verified so far: no mode change at a Start press in gameplay (ng2_007, +120 s). Pictures of the pause and the
  return: see section 4 (the Chapter 1 opening cinematic runs past +175 s on the scripted route, so the presses
  must come after it).

## 2. The stutter: what a slow frame does (instrumentation)

`[hitch]` lines now carry `creates N (ms)`, `tex-load cpu ms`, `pack N (read ms)`; the 5-second `[swap]` line
carries the window totals (rexglue-v1025 64422ce). `tools/native_gpu/hitch_compare.py` aligns legs by route time.

The user's own 8-minute session (ng2_125.log, chapters 10 and 13): 28 hitches, ten 5-s windows with a worst frame
over 100 ms, three over 500 ms; frame time correlates 0.74 with texture KB loaded that frame; pipeline waits 0,
sync readbacks 0. Every slow frame is a texture-load burst.

Chapter 1 arrival, scripted route (ng2_007, sync path): one 1230 ms frame = 572 textures (48.5 MB), 542 D3D12
creations (153 ms), 1037 ms of CPU inside the texture loads, of which 413 pack files read on the render thread
(391 ms) plus two CreateCommittedResource and a mip pass per replacement.

## 3. The fix: pack replacements built on worker threads (`texture_pack_async`)

rexglue-v1025 3bccf47 (+ f5df288, a 24-per-frame cap on applied results). The guest texture is always decoded and
visible; the worker reads the file into a mapped upload buffer and creates both resources; the render thread swaps
the view at BeginSubmission. A live-texture registry and a per-texture pending hash drop stale results.

| leg (same route, 175 s, chapter 1 new game) | worst frame | windows > 100 ms | hitches | load CPU over the route | pack read |
|---|---|---|---|---|---|
| sync1 (ng2_007, v1025b) | 1230 ms | 7 | 16 (26 windows) | 4303 ms | 1816 ms |
| sync2 (ng2_010, v1025b) | 1221 ms | 9 | 28 | 3764 ms | 1261 ms |
| async1 (ng2_009, v1025c) | 456 ms | 3 | 35 | 668 ms | 1530 ms (on the worker) |
| async2 (ng2_011, v1025c) | 472 ms | 3 | 30 | 442 ms | 1315 ms (on the worker) |

The later streaming frames on the route: 873 / 631 / 474 / 290 ms (sync) against 82 / 63 / 85 / 37 ms (async).
Replacements applied: 787 in every leg; none dropped.

What remains of the arrival frame is the game's own 542 texture creations: 153 ms alone, 286-302 ms beside the
worker (its creations contend for the kernel resource lock - the Fable II session's finding, and its placed-heap
attempt was a measured no-win: b9a4903). The hitch COUNT rose with the async path because the drain applied every
finished result in one frame (a window of 14 hitches): hence the per-frame cap (f5df288), measured in section 4.

Next targets, in order: a pre-created spare pool for the pack's few shapes (removes the worker's creations from the
burst), then the game's own creations (Fable: 84% repeated shapes in town), then the XXH3 unchanged-page upload
skip after a churn census (NG2 re-uploads ~17 MB/frame under clear_memory_page_state).

## 4. Results appended as they land

### 4.1 Cap, spare pool, creation hold (23:30-23:52)

| leg (chapter 1 new game, same route) | plugin | worst frame | windows > 100 ms | hitches | load CPU (route) | arrival frame: creates / load |
|---|---|---|---|---|---|---|
| cap1 / cap2 (ng2_012 / 013) | v1025d: async + apply cap 24 | 499 / 501 ms | 2 / 2 | 19 / 25 | 464 / 459 ms | 542 creates 315 ms, load 145 ms |
| spare (ng2_014) | v1025e: + spare pool (671 of 1000 replacements took a spare) | 501 ms | 4 | 29 | 817 ms (240 s) | 542 creates 319 ms, load 136 ms |
| final (ng2_015) | v1025f: + creation hold | **286 ms** | **2** | **11** | **434 ms** | 542 creates 132 ms, load 118 ms |

- The cap removed the 200 ms drain frame and cut the hitch count (35/30 -> 19/25); the arrival frame was unchanged.
- The spare pool alone changed nothing on the arrival frame: the render thread's own 542 creations stayed at ~319 ms
  beside the worker. So the contention is not creation-vs-creation only; any concurrent worker D3D activity
  (creations, Map/Unmap) doubles the render thread's creation cost (153 ms alone in the sync legs).
- The hold (a frame past 16 creations parks the worker's D3D calls until the next BeginSubmission; file reads go
  on) brought the creations back to 132 ms: arrival 286 ms, second burst 170 ms, the worker held 557 ms in total
  across both bursts. Every later streaming frame on the route is under 80 ms (sync: 290-873 ms).
- rexglue-v1025 commits: 64422ce (fade + instrumentation), 3bccf47 (async), f5df288 (cap), 9156afd (spares + hold).
  Plugin sha256 B02AC650 (pair v1025f) is the release plugin.

### 4.2 The pause pictures

Not obtained tonight: on the scripted new-game route the Chapter 1 opening cinematic runs past +175 s and a Start
press during it skips a segment rather than pausing; the copied Chapter 4 save did not route to Continue on the
second attempt. The evidence for the fix is the mechanism: with the pause flags ignored, the world drawn behind the
weapons menu (3d=515-522 in the v1.0.20 timeline while paused) keeps the frame ultrawide, so the presenter never
switches and the fade in and out of the menu covers the width; the v1.0.19-v1.0.24 behaviour is one environment
variable away (NG2_UW_PAUSE_16_9=1) if the menu itself should look different than expected. The user's morning
session is the picture test.

### 4.3 The stage pre-cache goes GPU-ready (user direction relayed at 23:58; ng2_016, 00:02)

Today's StageWarm only read a stage's pack files into the OS page cache. Now, at a stage change, the stage list
(stages/chNN.txt) becomes prebuild jobs behind the textures' own jobs: the worker reads each header, keeps within
`texture_pack_prebuild_mb` (1536), builds both resources and reads the file; the drain records the copy and the mip
pass under the apply cap and keeps the finished resource keyed by file id + content hash; a streamed texture whose
content matches swaps to it with no read, no creation, no upload, no mip pass (rexglue-v1025 texpack_prebuild).

ng2_016 (v1025g): stage 1 lists 3,106 files; 1,836 built GPU-ready in 4.4 s during the front end (1,524 MB), 1,270
skipped by the budget (the list is a set in id order, so the cut is arbitrary - a usage-ordered list would cover
the burst first). Arrival burst: 251 of the 413 replacements took a prebuilt resource, 162 missed; the arrival
frame 286 -> 212 ms (load CPU inside it 118 -> 65 ms, creates 132 -> 115 ms), the second burst 170 -> 132 ms, the
route's pack reads on the worker 1428 -> 559 ms. Hitch count rose (11 -> 38) because the prebuild's own drain
frames land in the front end; they are before play. On this GPU a budget of 4096 MB would cover the whole stage.

### 4.4 "Do supersampling / antialiasing apply on the fly?" (user, 00:1x)

- Antialiasing (`swap_post_effect`): the F10 row said "not restart-bound, the plugin applies it per swap" and the
  exe's ApplyLiveSettings pushed the cvar, but the plugin read it ONCE at graphics-system init and declared it
  kRequiresRestart (graphics_system.cpp) - a live change never reached the command processor. Fixed in the plugin:
  hot-reload lifecycle + a change callback into SetDesiredSwapPostEffect (rexglue-v1025 fxaa_live).
- Supersampling (`draw_resolution_scale_x/y`): restart-bound by design - the scale is baked into the texture and
  render-target caches at creation. The menu's "restart required" tag was gated on `!live`, which was always false,
  so it was never drawn on any of the 8 restart-bound rows. Fixed in the exe (ng2_menu.cpp): the tag is drawn.
- The user's saved values (resolution_scale=2, antialias=fxaa_extreme, anisotropic=4) were active in their 22:14
  session from its start (ng2_125.log tuning lines: draw_resolution_scale_x/y = 2, swap_post_effect = fxaa_extreme),
  so what they saw at 22:14 WAS 2x + FXAA. Whether 2x is visibly different from 1x on this title is checked by the
  scale1/scale2 legs (title screen, eye_zoom at native pixels), see 4.5.

### 4.5 F10 census: does every row reach the GPU? (user, 00:1x: "verify all the F10 enhancements are wired in")

`tools/f10_census.py` (full output in `docs/F10_CENSUS_2026-09-27.txt`): each of the 25 RowStart rows -> the
Ng2Settings fields it edits -> the cvars the exe derives (startup tuning in ng2_tuning.h, live push in
ApplyLiveSettings) -> the SDK/exe definition, lifecycle, read sites with their enclosing function (an init-time read
= restart needed) -> the user's session log's `Tuning:` lines as startup evidence. The registry accepts live writes
to kRequiresRestart flags (only kInitOnly is refused after finalisation; callbacks still fire), so every verdict
depends on the CONSUMER re-reading.

| verdict | rows |
|---|---|
| live, as labelled | Keep aspect ratio (present_letterbox, per paint), Ultrawide (ng2_fov_k per swap), Keyboard and mouse (mnk_mode), Folder / Use the upscaled textures (texture_pack_path, hot), Skip intro videos (ng2_video_mode, exe), Fullscreen / pointer (window) |
| restart by design, now tagged | Supersampling (draw_resolution_scale at texture-cache creation), Accurate depth (render-target cache Initialize), Anisotropic (new samplers only), Texture cache limits, Internal render size, Monitor / Resolution / Frame rate, Dump while playing, Fuzzy alpha test (new shaders only; tag added) |
| WAS BROKEN, fixed tonight | Antialiasing (swap_post_effect read once at SetupGuestGpu + kRequiresRestart -> hot-reload + change callback in the plugin); Dither and Extra sharpness (the presenter builds its paint config once in InitializeCommonSurfaceIndependent; the exe has no public route to its live setter - Window::presenter() is protected - so both rows now carry the restart tag, and the CAS sharpness, which was never in the startup tuning list, now is) |
| cannot do anything in this build | Sharpening (present_effect): the shipped runtime is built with FidelityFX off, so the effect enum holds bilinear only (the settings comment already says the row "offers bilinear and nothing else") |
| census artefact | On-screen readouts / hud_menu_bars: read inside ng2_menu.cpp itself (the menu's own bars), which the census excludes from "outside the menu" |

Both games share graphics_system.cpp and presenter.cpp, so the Antialiasing, Dither and Extra sharpness holes
apply to Fable II's F10 too; sent to the Fable session at the user's direction.

### 4.6 The game's own textures pre-created at a chapter load (user, 00:0x; ng2_017 / ng2_018, 00:37-00:43)

Pack OFF, same route, plugin v1025h (4B3D8F32). The first visit records the chapter's resource descriptions
(cache/texture_shapes/ch01.txt: 144 shapes, 2,618 bytes); the second visit pre-creates them on the worker within
texture_precreate_mb (512): 1,178 resources planned, 1,030 textures took a ready-made resource by +70 s.

| leg | arrival frame (572 textures) | its 542 creations | route worst | windows > 100 ms | hitches |
|---|---|---|---|---|---|
| packoff1 (records) | 145 ms | 102 ms | 145 ms | 1 | 6 |
| packoff2 (pre-created) | **38 ms** | **1.6 ms** | 72 ms | 0 | 3 |

The 72 ms frame left on the route is a 28-texture frame at the chapter-card-to-scene cut with 0 creations and 0
load time in every leg tonight (sync and async alike); it is not a texture-creation cost and is the next thing to
name.
