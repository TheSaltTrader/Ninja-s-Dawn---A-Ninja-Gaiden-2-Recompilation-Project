# Architecture

How the pieces of this port fit together: what is in each binary, how a setting
reaches the engine, and which changes do not live in any source file. Written
because more than one defect here has come from the shape of the system rather
than from any single file — a feature split across two DLLs that lost half of
itself in a release (before the graphics moved into the executable), and two crash fixes that vanished when the translated code
was regenerated.

For what the port *does*, see the [README](../README.md). For what went wrong
and why, see [ISSUES_AND_FIXES.md](ISSUES_AND_FIXES.md).

---

## The binaries

Since v1.1.1 a release is one executable and one SDK DLL, plus the Visual C++
runtime files beside them. **They are a matched set.** A mismatched pair does
not fail politely: the game exits during startup with no error message and an
empty log.

| Binary | Built from | Holds |
|---|---|---|
| `ng2.exe` | `ng2recomp` (this repo) + the translated game code | The game's own PowerPC translated to x86-64, **its graphics system** (the console's command stream reader, the Direct3D 12 renderer, the texture pack, the ultrawide widen, the present thread), the settings screens, the setup screen, the updater, the texture tools' front end |
| `rexruntime.dll` | the ReXGlue SDK tree | Kernel, filesystem, audio, input, and **the presenter** |

Up to v1.0.25 there was a third, `rexgpu-xenos.dll`, the SDK's Xenos GPU
plugin: the ring parser, shader translation and the Direct3D 12 backend. v1.1.0
compiled that backend into the executable beside the plugin, and v1.1.1 removed
the plugin (ISSUES_AND_FIXES N1). An install updated from an older release
still has the file; nothing loads it.

`rexruntime.dll` is built from source rather than taken from a stock SDK drop,
because this port depends on fixes in that tree (see the README's "What this is
built on").

**`RexBlue/win-amd64/bin/` is not authoritative.** It is a staging directory and
has held binaries older than the last four releases. Check a staged DLL against
a known-good release, never against that folder.

---

## The graphics path

    game thread     the translated game writes the console's command stream
                    (PM4 packets) as it did on the Xbox 360; ng2.exe reads it
                    and decodes each packet, on the game's own thread
        |           ordered stream of decoded draws, 128 chunks deep, with
        v           every fence, interrupt and kick side effect queued behind
    draw thread     the draws before it (`ng2_opt_split`, default on)
                    records the Direct3D 12 commands: the translated shaders,
                    the EDRAM render-target model (ROV path), the texture
                    cache and the texture pack
        |
        v
    present thread  hands each finished frame to the game's window, so the
                    game thread never waits on the copy to the screen

The renderer is the SDK's Direct3D 12 backend, vendored into
`src/native_gpu_xlat/` by way of the Fable II port's migration kit
(`src/native_gpu_xlat/ORIGIN.txt` records every source commit and every patch),
and driven by the executable instead of the plugin. The thread split and the
present thread came from the Fable II port as well. A machine without
Direct3D 12 gets a message box at start-up.

---

## Why a feature can span two binaries

Ultrawide is the worked example, and the one that has already cost a release.

    ng2.exe            computes k = render_aspect / display_aspect, scales
                       column 0 of the per-object World-View-Projection by it
                       -> the 3D field of view widens (a true Hor+ widen, not
                       a stretch). Classifies each frame as gameplay or
                       menu/video, compresses the HUD into the 16:9 band, and
                       writes the answer to the ng2_uw_mode cvar
        |
        v
    rexruntime.dll     the presenter reads ng2_uw_mode BY NAME and overrides
                       letterboxing: mode 1 fills the screen, mode 2
                       pillarboxes at 16:9, mode 0 defers to the player's
                       "Keep aspect ratio" setting

The presenter is compiled into `rexruntime.dll` and cannot link against code
in the executable, so **the shared cvar registry, addressed by name, is the
channel.** Until v1.1.1 the widen lived in the GPU plugin, which could not call
the runtime either, and the same channel joined three binaries.

The consequence is the part worth remembering: a build with the widen and a
runtime without the presenter's half widens the field of view into a frame that
is then letterboxed anyway. Nothing errors. Ultrawide simply reads as absent,
and so do the full-screen scene fades, which depend on the same fill. That
shipped as v1.0.22.

`make_release.py` now refuses to package unless the staged runtime carries
`ng2_uw_mode` and the staged executable carries `ng2_uw_mode`, the fade fix's
own marker and the string that says its own graphics system was created. Each
check runs a control string first, so a reader that can see nothing refuses
rather than reporting every feature missing.

---

## How a setting reaches the engine

    ng2_settings.cfg  ->  Ng2Settings (src/ng2_settings.h)  ->  ng2_tuning.h
                                                             ->  cvars

Three things decide whether a setting can be changed while the game runs:

* **Live settings** are pushed through `ApplyLiveSettings`, which writes the
  cvars directly. The Ultrawide toggle is one of these.
* **Restart-bound settings** are saved immediately and applied at the next
  launch. The row stays editable and is marked, rather than being greyed out:
  the value is real, just deferred.
* **Renderer settings read once at start-up** (the internal resolution, the
  anisotropic level, the output filter and its sharpness, dither, the texture
  cache size and others) are restart-bound whatever the row looks like. The
  F10 census (`tools/f10_census.py`) established which; each such row carries a
  red "takes effect after a restart" note (`RestartTag` in `ng2_menu.cpp`), and
  changing one raises a RESTART REQUIRED banner. Up to v1.1.0 these were the
  GPU plugin's cvars, which could be set neither from the command line nor in
  `OnPostSetup` - the plugin registered them after `OnPreSetup` - and went
  through a TOML loaded via `cvar::LoadConfig`.

**A setting that is removed keeps its field.** When a setting is shown to harm
the game it is taken out of every screen, the tuning forces the safe value, and
the field stays only so that older `ng2_settings.cfg` files still parse — with
the reason recorded in `tools/settings-ledger.json`. The frame-rate range above
60 and V-Sync are both retired this way: on this title each is a game-speed
control rather than a display control.

`tools/lodestone_census.py` enumerates every field in `ng2_settings.h` and
checks that each is reachable in a UI or declared in the ledger, that each
reachable one appears in the README, that each round-trips through save and
load, and that a guarded cvar is guarded by its own setting. Run it after
touching settings or the settings UI.

It checks **coverage, not accuracy**: it verifies that a phrase appears in the
README, not that what the README says is true. It passed for some time while the
README still offered a 30–144 frame-rate range and a V-Sync row for settings
that had been removed as unsafe.

---

## Changes that live outside any source file

Two classes, and both are invisible to a reader of the repository.

**Post-generation patches.** Some fixes are applied to the *translated* C++
after codegen, because the defect is in generated code. They are not in any
source file, so regenerating the game code drops them — which is exactly what
happened between v1.0.16 and v1.0.17, and three previously-fixed crashes came
back. The build now re-applies them and prints either the count it changed or
`SKIP (already patched)`, so a regeneration that loses them shows up in the
build log rather than at the next boss.

* `patch_missed_regs` — restores a register hand-off across a branch whose saved
  registers were re-initialised to zero, which made the fragment write through a
  null pointer (Ninpo, and a late boss).
* `scanguard` — null-guards the end-of-chapter effect-list scanners, so a freed
  list does not walk into unmapped memory (chapter 12 → 13).

**Midasm hooks into guest code.** The internal render size patch and the
frame-hook that drives the on-screen FPS counter are hooks placed at guest
addresses, declared in `config/`. A guest address in generated code is written
as a **signed decimal** `lis` immediate — `0x84C40000` appears as
`-2067529728` — so grepping for it as hex finds nothing and proves nothing.

---

## Packaging and release

`tools/make_release.py` stages a release into `../Releases/vX.Y.Z` and zips it.
It refuses to cut a version with no changelog entry, one missing a required
tool, a build older than its sources, or anything that looks like game data.

Two checks specifically guard the binaries described above:

* `check_sdk_pair()` — up to v1.1.0 refused one stock DLL beside one
  source-built one; with one SDK DLL left it now reports which runtime was
  staged. **This checks origin, not content**, and it passed on the build that
  shipped without ultrawide.
* `check_ng2_features()` — reads the *staged* files and refuses to package if an
  NG2 feature has gone missing from them.

`make_release.py --update <folder>` applies a build over an existing install,
keeping `game/`, `dlc/`, `user/`, `logs/`, `cache/` and the settings files. It
does **not** refresh `README.txt`, `RELEASE_NOTES.md`, `provenance.txt` or
`SHA256SUMS`; copy those from the staged release and regenerate the checksums
against the install's own files, or it will report failures for the settings
file the player has legitimately changed.

The in-game updater is all-or-nothing: it refuses to begin if the old process is
still running, stages every file beside its destination, and commits only once
all of them have landed. Its reason for declining is written to
`update\last_error.txt`.
