# Architecture

How the pieces of this port fit together: what is in each binary, how a setting
reaches the engine, and which changes do not live in any source file. Written
because more than one defect here has come from the shape of the system rather
than from any single file — a feature split across two DLLs that lost half of
itself in a release, and two crash fixes that vanished when the translated code
was regenerated.

For what the port *does*, see the [README](../README.md). For what went wrong
and why, see [ISSUES_AND_FIXES.md](ISSUES_AND_FIXES.md).

---

## The three binaries

A release is one executable and two DLLs, and **they are one matched set**. A
mismatched trio does not fail politely: the game exits during startup with no
error message and an empty log.

| Binary | Built from | Holds |
|---|---|---|
| `ng2.exe` | `ng2recomp` (this repo) + the translated game code | The game's own PowerPC translated to x86-64, the settings screens, the setup screen, the updater, the texture tools' front end |
| `rexruntime.dll` | the ReXGlue SDK tree | Kernel, filesystem, audio, input, and **the presenter** |
| `rexgpu-xenos.dll` | the ReXGlue SDK tree | The Xenos GPU plugin: the PM4 ring parser, shader translation, the upload copy pool |

Both DLLs are built from source rather than taken from a stock SDK drop,
because this port depends on fixes in that tree (see the README's "What this is
built on").

**`RexBlue/win-amd64/bin/` is not authoritative.** It is a staging directory and
has held binaries older than the last four releases. Check a staged DLL against
a known-good release, never against that folder.

---

## Why a feature can span two DLLs

Ultrawide is the worked example, and the one that has already cost a release.

    ng2.exe            computes k = render_aspect / display_aspect
                       and writes it to the ng2_fov_k cvar
        |
        v
    rexgpu-xenos.dll   reads ng2_fov_k per draw, scales column 0 of the
                       per-object World-View-Projection -> the 3D field of
                       view widens (a true Hor+ widen, not a stretch).
                       Classifies each frame as gameplay or menu/video and
                       writes the answer to the ng2_uw_mode cvar
        |
        v
    rexruntime.dll     the presenter reads ng2_uw_mode BY NAME and overrides
                       letterboxing: mode 1 fills the screen, mode 2
                       pillarboxes at 16:9, mode 0 defers to the player's
                       "Keep aspect ratio" setting

The two DLLs cannot call each other. `presenter.cpp` compiles into
`rexruntime.dll` while `command_processor.cpp` compiles into
`rexgpu-xenos.dll`, and the GPU plugin does not relink the core objects, so a
plain extern or a compile-time cvar reference across that boundary will not
link. **The shared cvar registry, addressed by name, is the channel.**

The consequence is the part worth remembering: a build with the right plugin and
the wrong runtime widens the field of view into a frame that is then letterboxed
anyway. Nothing errors. Ultrawide simply reads as absent, and so do the
full-screen scene fades, which depend on the same fill. That shipped as v1.0.22.

`make_release.py` now refuses to package unless the staged runtime carries
`ng2_uw_mode` and the plugin carries `ng2_uw_mode` and the fade fix's own
counter. Each check runs a control string first, so a reader that can see
nothing refuses rather than reporting every feature missing.

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
* **GPU-plugin cvars cannot be set from the command line at all**, and cannot be
  set in `OnPostSetup` either — the plugin registers its cvars after
  `OnPreSetup` and reads some of them once at GPU init. They go through a TOML
  loaded in `OnPreSetup` via `cvar::LoadConfig`, the one path that defers values
  for cvars that do not exist yet.

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

Two checks specifically guard the trio described above:

* `check_sdk_pair()` — refuses one stock DLL beside one source-built one.
  **This checks origin, not content**, and it passed on the build that shipped
  without ultrawide.
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
