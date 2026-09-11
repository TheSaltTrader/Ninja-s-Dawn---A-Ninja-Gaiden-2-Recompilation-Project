# ng2recomp

Static recompilation of **Ninja Gaiden II** (Xbox 360, Tecmo / Team NINJA, 2008)
to a native PC executable, built on the [ReXGlue SDK](../RexBlue/win-amd64) —
the same toolchain re:Blue uses for Blue Dragon.

This is **not** an emulator. `rexglue codegen` translates the game's PowerPC
machine code into C++, which is compiled into a normal x86-64 binary; the SDK
supplies the Xbox 360 kernel, filesystem, audio and GPU emulation around it.

> Requires your own copy of the game. No game code or assets are redistributable
> — `assets/`, `game/` and `out/` are all build/user data, not project content.

## Status

Stage 0 (does this recompile and build at all?) is **done**. The project
codegens, compiles, links, and starts executing guest code. It does not yet
reach a rendered frame.

| Step | State |
|---|---|
| Disc extraction | ✅ 65 files, 6.7 GB |
| XEX decrypt / analysis | ✅ 42,504 pdata functions, 0 analysis errors |
| `rexglue codegen` | ✅ 1,109 files, 563 MB of C++, ~43 s |
| Compile + link | ✅ 559 TUs, ~6 min, `ng2.exe` (164 MB) |
| Runtime init | ✅ 48,271 functions registered, XEX loaded, 238 imports resolved |
| GPU init | ✅ D3D12 device, Xenos plugin, GPU threads, shader storage |
| Codegen warnings | ✅ **zero** unresolved calls or branches |
| Guest execution | ✅ both init phases complete; reaches the main loop |
| Runs continuously | ✅ **5 min unattended, 0 errors, no behaviour hooks** |
| Rendering | ✅ **1,734 EDRAM resolves in 30 s at 1280x720**, double-buffered |
| Meaningful picture | ✅ **reaches the start menu, and the menu renders correctly** |
| Intro video | ⚠️ plays, but with colour artifacts (see below) |
| Settings menu | ✅ setup screen before boot + F10 overlay; ISO install, DLC, graphics |

The game's entry (`sub_822F34B0`) is:

```
bl 0x822F35A0     ; init phase 1  - completes
bl 0x822F3708     ; init phase 2  - completes
loop: bl 0x822F4010
      bl 0x822F2E38
      bl 0x822F0CA8
      bl 0x822F3A88   ; <- faults here, first pass through the loop
      bl 0x8233B6F8
      bl 0x8380E6D8
      b  loop
```

By this point the game has created five worker threads of its own, and the GPU
command processor is doing real work (`Make 1DC30000 -> 1F030000 (20971520b)
coherent`). It has not yet opened any file from the disc.

## What the game is

| | |
|---|---|
| Title ID / Media ID | `0x544307D5` / `0x69F555CB` |
| Original binary | `gaiden2_Release_LTCG.exe` (`c:\gaiden2\out\Release_LTCG\gaiden2.pdb`) |
| Image base / entry | `0x82000000` / `0x83818DA8` |
| `.text` | 24,245,316 bytes (~6.06M PowerPC instructions) |
| `.pdata` | 42,504 functions (median 584 B, p90 1 KB) |
| Imports | `xam.xex` (65) and `xboxkrnl.exe` (173) only — no guest DLLs |
| Build | LTCG, so heavily inlined and with no shipped symbols |

DLC (in `../DLC/544307D5/00000002/`, four `LIVE` STFS packages, ~175 MB):
Fiend Hayabusa, Shadow Walker Hayabusa, Biometal Hayabusa, and Mission Mode.
Not wired up yet — the SDK has STFS support (`rex/filesystem/devices/stfs_xbox.h`)
and a `--license_mask` cvar, so this is a post-boot task.

## The Xbox 360 XDK in `../Xbox 360 SDK`

Worth knowing what this copy is and is not. It is the **tools, art pipeline and
documentation** portion of the XDK — it contains **no headers and no `.lib`
files** (verified: 0 of each).

Useful:

- `doc/1033/xbox360sdk.chm` — 88 MB, 11,906 pages of official reference.
  Extract with 7-Zip. Covers EDRAM and predicated tiling, the graphics
  whitepapers, XACT3, XMA/xWMA — i.e. exactly the areas the GPU and audio work
  will land in.
- `bin/win32/` — `xma2encode.exe`, `xact3.exe`/`xactbld3.exe` (the game ships
  `stream_bgm.xwb`/`stream_system.xwb` + `.xsb`/`.xgs`), `imagexex.exe`,
  `XuiTool.exe`/`xui2bin.exe`, `pdbinfo.exe`/`xexpdb.exe`.

What would be a much bigger win, and is **not** here: the XDK's `lib\xbox\`
static libraries (`xapilib.lib`, `libcmt.lib`, `d3d9.lib`, `xactengine.lib`, …).
NG2 statically links all of them, so with those you could signature-match
library code and put real names on a large share of the 42,504 functions
instead of `sub_XXXXXXXX`. rexglue's codegen headers include a `sig_scanner.h`,
so the toolchain appears built for exactly that. Worth hunting down.

## Prerequisites

- Visual Studio 2022 Build Tools (MSVC 14.44) — supplies the Windows SDK
- LLVM/Clang 22 in `C:\Program Files\LLVM`
- Ninja (`pip install ninja`)
- Python 3.12 with `cryptography` and `capstone` (`pip install cryptography capstone`)

vcpkg is **not** needed: SDL3, fmt, spdlog, utf8cpp, the DXC headers and the
Xenos GPU plugin all ship inside the SDK.

## Layout

```
assets/default.xex     entrypoint, extracted from the disc (build input)
game/                  full disc extraction (runtime data)
config/functions.toml  manual function-boundary overrides
generated/             rexglue codegen output (563 MB, not source)
src/                   the host app - main.cpp and Ng2App
tools/                 analysis and build tooling (see below)
out/build/             build trees, per configuration
```

## Building and running

```cmd
tools\build.cmd Release            :: or RelWithDebInfo / Debug
tools\run.cmd
```

`build.cmd` runs `rexglue codegen` as its own step *before* CMake. That
ordering matters: codegen rewrites `generated/default/ng2_pch.h`, and inside a
single ninja run the precompiled header can be built from the old copy before
codegen replaces it, which fails every translation unit with *"file has been
modified since the precompiled header was built"*.

`Release` drops debug info for the recompiled TUs; any other configuration
keeps line tables, which is what makes crash triage work (below).

## Tools

| Tool | Purpose |
|---|---|
| `extract_disc.py` | Extract an Xbox 360 GDF disc image (XGD1/2/3) |
| `xex_image.py` | Decrypt/decompress a retail XEX to a flat memory image; caches to `out/image.bin` |
| `whereis.py` | What is at guest address X, and who calls it |
| `xex_imports.py` | Kernel imports and their call sites |
| `add_function.py` | Register a function the analyzer missed |
| `scan_missed.py` | Bulk search for missed function pointers (**diagnostic only** — see below) |
| `boot_loop.py` | Automated run → crash → register → rebuild loop |
| `crash_trace.cmd` | Run under `cdb`, resolve the fault to a guest instruction |
| `xrefs.py` | Who reads/writes a guest address range (resolves `lis`+`addi` bases) |
| `gstrings.py` | Strings in the decrypted image, with guest addresses and xrefs |
| `watch_cursor.cdb` | Scratch `cdb` script for hardware watchpoints - edit per question |
| `capture_intro.py` | Launch and photograph the window (Windows Graphics Capture, HWND from our PID) |
| `play_probe.py` | The same, plus scripted key presses |
| `ui_probe.py` | The same, plus scripted **mouse clicks** - how the settings menu is tested |
| `boot_check.py` | Boot N times and report how far each run got |

**`ui_probe.py` has to be DPI-aware, and it is the first line of the file.**
Windows Graphics Capture returns physical pixels - a 1280x720 window on a 125%
display photographs as 1600x900 - while a DPI-*unaware* process gets every
Win32 coordinate virtualised into logical units. Coordinates measured on a
screenshot then reach `SetCursorPos` in the wrong space, and every click lands
25% away from its target, hitting some other control with nothing to say it
went wrong. This looked exactly like an app bug and cost a rebuild of the game
to disprove: ImGui here runs in a 1280x720 logical space and its mouse mapping
is correct. Also note `PostMessage(WM_LBUTTONDOWN)` does nothing at all - SDL3
reads the pointer from raw input and ignores synthesised window messages - so
clicks must be real input, under a foreground check so they cannot be typed
into somebody else's window.

### Triaging a crash

Recompiled guest functions are real native functions named `sub_<guest address>`,
and codegen emits one source line per guest instruction. So with a
RelWithDebInfo build, `crash_trace.cmd` turns a bare fault address into the
exact guest instruction plus a full guest call stack:

```
ng2!__imp__sub_83845470+0x298
  generated/default/ng2_recomp.187.cpp(24241)
> 24241: ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 420);   // lwz r11,420(r31)
```

## Findings so far

**Missing function boundaries.** MSVC emits no `.pdata` for small helpers with
no prologue — `this`-adjusting thunks, interlocked helpers, one-line virtual
overrides. When one sits in the padding after a real function the analyzer
absorbs it, and a call reaching it dies with *"Call to invalid or unregistered
function"*. `config/functions.toml` carries the overrides; `add_function.py`
computes the extent.

**Bulk static discovery does not work here.** `scan_missed.py` finds ~1,340
candidates but most are false positives: `.text` contains pointer tables, and a
pointer beginning `0x822F` *decodes* as a plausible `lwz` instruction. Treat its
output as a lead, never as something to auto-register. The runtime is the
reliable oracle — every address it reports is genuine by construction, which is
what `boot_loop.py` exploits.

**`xrefs.py` had three bugs worth knowing about**, because each produced
*confident wrong answers* rather than obvious failures: it reported `va + i`
where `i` was already a file offset (shifting every address by the section
base, so `0x822F3C04` printed as `0x825E3C04`); it matched stores whose base
was `r1`, reporting stack writes as global ones; and it treated an `addi`'s own
destination as a clobber, discarding every match. Validate a scanner against a
reference you already know the answer to before trusting it.

**The GPU plugin must be selected in code.** `gpu_plugin` is a `RuntimeConfig`
field, not a command-line cvar. Without `Ng2App::OnPreSetup` setting it, the
runtime comes up in "native rendering mode" and silently ignores every `Vd*`
kernel call. `CMakeLists.txt` stages the plugin DLL via `GPU_PLUGINS xenos`.

**Codegen flags matter for correctness, not just speed.** With the defaults,
every function shares one `PPCContext`, so a callee losing a non-volatile
register corrupts its caller. The manifest now sets `non_volatile_as_local`
and friends (the configuration re:Blue ships), giving each function its own
`r14`–`r31` and letting the recompiler elide the `__savegprlr`/`__restgprlr`
helpers.

## Where it is now

The game **boots, runs its main loop indefinitely, and renders frames** - but
with a bring-up hook in place, and drawing almost nothing.

GPU telemetry over a 30 second run:

```
1,734 Resolve: 0,0 <= x,y < 1280,720, k_8_8_8_8 -> k_8_8_8_8
      alternating between 0x1F045000 and 0x1F3DD000   (double buffered)
    2 distinct shader pipelines
      1120x2352 1xMSAA colour + depth render targets at EDRAM base 0 / 518
       560x1176 4xMSAA colour + depth
       640x1024 4xMSAA depth
```

60.1 resolves/second with **100% strict alternation** between the two targets
is a clean vsync-locked 60 fps double-buffered frame loop, and a 5-minute
unattended run logged zero errors or warnings, and the game is compiling shaders into D3D12 pipelines and running 4xMSAA
render targets out of emulated EDRAM. But **two shader pipelines is almost
nothing** - a title screen would need dozens. The screen is near-certainly
blank or near-blank. Treat "renders frames" as "the pipeline is alive", not
"the game is playable".

There is **no visual confirmation**. `PrintWindow` cannot read a D3D12
swapchain (it returns a single flat colour), and capturing the screen region
under the window is not an acceptable substitute - it photographs whatever is
actually on top, which is not necessarily this game. `tools/screenshot.ps1`
deliberately refuses to do that. Look at the window directly.

## Fixed: low guest memory is now committed

The 160 slot records at `0x84C23C70` come out of BSS with `type == 0` (a valid
index) and a **null data buffer**, and 275+ sites index off them as
`buffer[idTable[type]]`. On hardware those reads land in low memory and return
zero, which every consumer treats as "nothing here". Here the pages were
uncommitted, so the same reads faulted at guest `0x59BF` / `0x4F60`.

`Ng2App::OnPostSetup` now commits guest `0x1000`-`0x3FFFF` as **read-only
zeroes** (page 0 left unmapped so a genuine null dereference still faults, and
read-only so a stray null *write* is still caught):

```cpp
heap->AllocFixed(kLowBase, kLowSize, 0, Reserve|Commit, Read|Write);
memory->Zero(kLowBase, kLowSize);
heap->Protect(kLowBase, kLowSize, Read, &old);
```

Commit read+write first - `Zero()` writes through the guest mapping, so a
read-only range faults on our own memset before the game runs. That mistake
showed up as `write of guest 0x00001040`, inside our own range.

This removed the last behaviour-changing hook. The game now runs with **no
guest patches at all**.

## Fixed: NG2's task system is fibers, and the scheduler uses a second entry

This was the reason the screen stayed black.

`sub_822F2CA0(id, entry)` registers a task by calling
`CreateFiber(0x8000, sub_822F2C70, slot)` - **tasks are fibers, not OS
threads**. Init phase 2 registers exactly one: task 0 = `sub_83649658`, the
front-end / boot sequencer. The main loop schedules fibers every frame through
`sub_822F2B48`.

A guest-implemented fiber switch cannot work under static recompilation. The
switch at `0x83819200` saves `r1`/LR/CR/`r14`-`r31`/VMX into one context block,
restores another and "returns" into it. On hardware that resumes the other
fiber; here the recompiled call state lives on the *host* stack and in C++
locals, so restoring guest registers transfers nothing - the switch returns to
its own caller and the target fiber never starts.

Measured before the fix: `SwitchToFiber` ran 60x/second with exactly **one**
distinct target, forever, and neither `sub_822F2C70` nor `sub_83649658` ever
executed.

The SDK already solves this - `rexcrt` provides host `CreateFiber`,
`SwitchToFiber`, `DeleteFiber`, `ConvertThreadToFiber` and
`ConvertFiberToThread`. `config/rexcrt.toml` maps NG2's:

| Guest | rexcrt | How it was identified |
|---|---|---|
| `0x8380E418` | `ConvertThreadToFiber` | errors `0x500` if already a fiber, else allocates the 0xA50 context |
| `0x8380E4A8` | `ConvertFiberToThread` | clears and frees `[r13+0x100]->0x164`, errors `0x501` if none |
| `0x8380E4F8` | `CreateFiber` | `(stackSize, entry, param)`; allocates context + stack, min `0x4000` |
| **`0x8380E638`** | `SwitchToFiber` | see below |

**The critical detail:** `SwitchToFiber` must be mapped to **`0x8380E638`, not
its real entry `0x8380E5D8`**. The scheduler calls `bl 0x8380e638` every frame
- a fast-path *second entry point* that skips the entry's "already current?"
guard. Mapping only the real entry left the hot path running guest code and
changed nothing. Both entries take `r3` = the fiber; `0x8380E5D8` is used only
on task teardown.

This is the same multi-entry-point pattern that has bitten repeatedly in this
project - `_setjmp` inside `longjmp`'s `.pdata`, the `__savegprlr` family, the
absorbed adjustor thunks. **When replacing a guest function, check whether
anything branches into the middle of it.**

Immediately after the fix the game opened a new file (`ng2_sound.xgs`), the log
grew from 68 to 190 lines, the runtime logged
`CreateFiber: fiber=0x302bf000 start=0x822f2c70 stack=0x8000`, APCs began
delivering, and new render targets appeared.

## Where it is now: the start menu

The game boots to its **start menu, and the menu renders correctly**. Two more
fixes got it there after the fiber work:

**Low guest memory must include page 0.** The commit in `Ng2App::OnPostSetup`
originally started at `0x1000` so a genuine null dereference would still fault.
The game reads guest `0x00000000` deterministically during boot, so the range
now starts at 0. It is still mapped **read-only**, so a null *write* is still
caught - only null reads are allowed, which is what the game expects. Opening
that page took the boot from 17 files to **38**, including the large content
archives (`cmn.ng2`, `char.ng2`, `other_rtm.ng2`, `rtm.ng2`), and from 2 shader
pipelines to 7.

**Use the ROV render-target path.** The plugin supports both ROV and RTV/DSV
(`--render_target_path_d3d12`). Measured over 30 s:

| path | resolves | pipelines |
|---|---|---|
| `rov` | 3128 | 7 |
| `rtv` | 22 | 0 |

RTV is effectively broken for this game; ROV emulates EDRAM accurately and is
what the default already selects. Do not switch to RTV.

## The current blocker: intro video artifacts

The menu is correct; the **intro video** shows colour artifacts. The cause is
specific, and it is not a general rendering problem.

NG2 does not decode WMV on the CPU. It links Microsoft's `XMEDIA` library,
whose **GPU-accelerated DXVA decoder** is compiled into the binary - the build
paths are still in `.rdata`:

```
D:\xenon\aug07\core\private\del\codecs\wmv\codecdsp\video\wmv\xplat\
    decoder_c9e\dxva_pk\xenon\x\obj\xbox\Shader_*.updb
```

The shader set: `Shader_DetileY` / `DetileUV` / `DetileYIntFrm` /
`DetileUVIntFrm` (plane detiling), `Shader_ResidualsY`/`UV` in `Prog` /
`IntFrm` / `Intra` variants (residual reconstruction), `Shader_RangeRedV9` /
`RangeRedWMVA` (WMV9 range reduction), `Shader_RepeatPadFirstPix` (padding).

During playback the GPU trace shows exactly the formats that pipeline needs:

```
474x  1120x584  k_8_8_8_8
474x  1120x584  k_16_16_16_16_     <- 64bpp signed residual buffers
  2x   640x360  k_8                <- single-channel Y / U / V planes
```

So the artifacts come from emulating a GPU video decoder built on unusual
formats (signed `k_16_16_16_16` residuals, `k_8` planes) and range-reduction
maths - not from the normal render path, which is demonstrably fine.

The videos it opens are `NinjaVI.wmv` (twice), `NinjaVI_Left.wmv` and
`NinjaVI_Right.wmv`.

### Diagnosis from an actual screenshot

The title screen renders **pixel-perfect** - logo, blood splatter, "PRESS START
BUTTON", alpha blending, grayscale grading, all correct. The intro video shows:

- single-pixel **horizontal striping across the whole frame**
- a central vertical band of alternating **magenta / green / black rows**
- real image structure still visible outside the band (a mountain silhouette),
  but drawn on alternate lines

That is field-separated output with chroma landing on the wrong rows - the
signature of the **interlaced** decode path (`Shader_DetileYIntFrm`,
`Shader_ResidualsUVIntFrm`; `IntFrm` = interlaced frame, versus the `Prog`
variants).

The likely mechanism is visible in the render-target list. These three describe
the **same EDRAM footprint** at different sample counts:

```
1120 x 2352 x 1 sample  = 2,634,240
1120 x 1176 x 2 samples = 2,634,240
 560 x 1176 x 4 samples = 2,634,240
```

The decoder aliases one EDRAM region as 1x, 2x and 4x MSAA to reinterpret its
data layout - a standard Xbox 360 trick, and a hard one to emulate. The plugin
also reports internally that **"2x MSAA is not supported, emulated via top-left
and bottom-right samples of 4x MSAA"**, which would scramble exactly this kind
of packing. ### The intro is THREE videos, not one

`NinjaVI.wmv`, `NinjaVI_Left.wmv` and `NinjaVI_Right.wmv` are opened together
and composited as a triptych - three 320x580 60fps panels side by side. Proven
from a screenshot: the coloured band measured 934 px with 707 px of grey left
and 567 px right, and 3 x 934 = 2802 exactly accounts for the 2208 px crop plus
the 227 + 367 px the overlay clipped off each edge.

The centre file is the odd one out - **WMV3 Main + WMA Pro 5.1**, where the
flanks are **VC-1 Advanced + WMAv2 stereo** - and it was the panel showing
magenta/green garbage while the flanks showed readable grey luma.

### Fixing the video by re-encoding it (the Namco22 route)

The `default.xex` does **not** validate a video's length, size or hash, so a
replacement is simply accepted and played (established on a previous project,
`Namco22\XBOX360_VIDEO_REPLACEMENT_GUIDE.md`). That makes the source video a
variable we control, which is the only lever this project has over a decoder
living inside the GPU plugin.

**ffmpeg cannot do this job** - it has no VC-1 encoder, and its ASF container is
rejected outright. Only Expression Encoder 4 / the Windows Media Format SDK
writes an ASF this path accepts. Driver: `tools/encode_vc1.ps1`, which **must**
run under 32-bit PowerShell (`C:\Windows\SysWOW64\WindowsPowerShell\v1.0`).

Three things had to be right, discovered one failed experiment at a time:

| variable | stock | fixed to | effect when wrong |
|---|---|---|---|
| codec | WMV3 centre, VC-1 flanks | uniform VC-1 Advanced | no change - **ruled the decoder path out** |
| height | 580 (pads to 16x37=592) | **576** = 16x36 | padding/EDRAM tiling mismatch; fixing it recovered visible outlines |
| GOP | **all-intra** | **all-intra** | a normal I/P/B GOP made it *worse* - see below |

**The stock videos are all-intra and that is load-bearing.** Every frame is a
keyframe. The decoder has separate shader variants for `Intra`, `Prog` and
`IntFrm`, and an all-intra stream never takes the predicted path, so it never
motion-compensates against reference surfaces in EDRAM - which is the machinery
that is broken. Re-encoding with a default GOP regressed the picture even while
the alignment fix improved it, which is exactly why the intermediate build read
as "slightly better, still garbled".

Result so far: magenta/green collapse and heavy striping gone; picture now
renders as clean grey. Chroma is still not being applied, and the source is
**not** the cause - the re-encode is `yuv420p` / `chroma_location=left`,
identical to stock. That points at the plugin's `Shader_DetileUV` /
`Shader_ResidualsUV` path.

Gotchas worth keeping:
- EE4's `AutoFit` silently preserves the source aspect ratio and turned a
  requested 320x576 into **316**x576. `MediaItem.VideoResizeMode` does *not*
  override it; `AdvancedVC1VideoProfile.AutoFit = $false` does.
- EE4 reads Xbox `.wmv` sources natively, so the `mc_*_ds.ax` filter
  registration in the Namco22 guide is only needed for MP4 input (and it wants
  elevation, so it hangs unattended).
- In a double-quoted **bash** string `\$f` collapses to a literal `$f`. Building
  Windows paths as `"$R\game_video_backup\$f.wmv"` silently produced a path with
  no filename and EE4 reported only "File not found".

Originals are preserved in `game_video_backup/`; restore with
`cp game_video_backup/*.wmv game/`.

## PC features

Everything the player touches is in the **settings menu** (below), saved to
`ng2_settings.cfg` beside the executable. `NG2_*` environment variables
override the file for scripted testing.

| feature | status | how |
|---|---|---|
| **Game data / disc install** | working | folder picker, or extract an ISO through the SDK's `DiscImageDevice` |
| **DLC** | working | a folder of STFS packages, installed via `ContentManager::InstallContent` |
| **Licence** | working | `license_mask` defaulted to 1 (full version) so DLC is visible |
| **Window resolution** | working | 640x480 - 7680x4320, exact client size verified |
| **Fullscreen** | working | live from the overlay, and honoured at launch |
| **Frame rate 30-144** | working | guest `video_mode_refresh_rate`; needs vsync on to pace |
| **V-Sync** | working | live |
| **Internal resolution scale 1-3** | working | true supersampling - see below |
| **Internal render size 1280x720** | working | the Xenia community patch, as midasm hooks |
| **Anisotropic filtering override** | working | `anisotropic_override`, 1x - 16x |
| **Letterbox / dither** | working | presenter cvars, live |
| **Controllers** | working | SDL driver; `gamecontrollerdb.txt` (2,285 mappings) shipped beside the exe |
| **FSR / CAS upscaling** | **not available** | this presenter implements `bilinear` and nothing else; the two sharpness cvars are not registered at all |

Verified numbers: `NG2_FPS` 30 -> 30.0 fps, 60 -> 60.0 fps, 144 -> 129.6 fps;
window sizes 1280x720 / 1920x1080 / 2560x1440 / 3840x2160 all produce exactly
that client size.

## The settings menu

Two surfaces over one settings model (`src/ng2_settings.h`), drawn by
`src/ng2_menu.cpp`:

**The setup screen** runs before the guest boots, from `ReXApp::OnFinalizePaths`
- the one hook where the window and the ImGui drawer are already live but the
runtime has not been constructed. That is what makes choosing the game data
possible at all: the path the screen returns is the path the runtime mounts. It
opens on the first run, whenever Shift is held at launch, and whenever the
configured game folder has gone missing. `--game_data_root` on the command line
suppresses it entirely, so scripted runs never stop at a dialog.

**The in-game overlay** is the same pages on **F10**. Settings the presenter
re-reads every frame are applied immediately; settings latched during startup
are shown greyed with `(restart)` on them rather than accepted and quietly
ignored. **F4** is the SDK's own cvar browser, unfiltered, and the menu links
to it rather than trying to replace it.

Three principles the menu is built on, each of which caught something real:

- **A control that does nothing must not be shown.** The output-filter list is
  read from `present_effect`'s own declared values, so it can never offer an
  effect this build lacks - which is how it correctly shows one option and says
  so. The two CAS/FSR sharpness settings are only written when their cvars
  exist; they do not.
- **Verify against the thing itself, not the value you wrote.** `resolution_scale = 2`
  was accepted and the plugin still reported `internal scale 1x1`: the
  convenience cvar is not what it reads. Sending `draw_resolution_scale_x/y`
  as well gets a genuine `2x2`.
- **Photograph the result.** `fullscreen=1` saved and reloaded correctly and
  the window still came up windowed, because the cvar is read when the window
  is created - during `SetupPresentation`, which runs *before* `OnPreSetup`.
  Only the capture being 1602x939 instead of the full screen gave it away.
  Window settings now go in from `OnConfigurePaths`, and the setup screen also
  pushes them straight at the live window when Play is pressed.

### Installing from an ISO

The runtime cannot mount a disc image as the game data root: `ConstructRuntime`
requires `is_directory(game_data_root)` and rejects a file outright (a `.iso`
produces "--game_data_root does not exist", which is a misleading message for a
path that plainly exists). So the setup screen extracts, exactly as re:Blue's
installer does, reading through the SDK's `DiscImageDevice` so the GDF parsing
is the SDK's problem.

Measured on this disc: 65 files, 6.7 GB, about 20 seconds, and the extracted
`default.xex` is byte-identical to the reference copy. A file already present
at the right size is skipped, so a cancelled install resumes instead of
starting over.

The disc's identity comes from the XEX itself - the optional header table's
`XEX_HEADER_EXECUTION_INFO` (key `0x00040006`), which on this disc reads media
`69F555CB`, title `544307D5`, disc 1 of 1. The screen names the title rather
than just confirming a file exists, so pointing it at the wrong game says so.

### Internal resolution scaling: what was wrong before

An earlier version of this section said internal scaling was impossible. It was
not; the value was being written at the wrong moment, twice over.

`draw_resolution_scale_x/y` lives in `rexgpu-xenos.dll`, which registers its
cvars after `OnPreSetup` and reads the scale once at GPU init. So a
`SetFlagByName` in `OnPreSetup` fails (unregistered) and one in `OnPostSetup`
succeeds too late - and, worse, wrote the settings value back over a scale that
had been set on the command line.

`cvar::LoadConfig` is the one path that survives the gap: it defers values for
cvars that do not exist yet and applies them at registration. The tuning is
written to a TOML and loaded from `OnPreSetup` for exactly this reason
(`src/ng2_tuning.h`). With that plus the correct cvar names, `OnPostSetup`
reads back `internal scale 2x2` and the title screen renders visibly sharper,
with the ring buffer clean.

### Capturing the screen without a human (tools/capture_intro.py)

The intro can only be judged visually, and both obvious approaches are wrong
here: **PrintWindow cannot read a D3D12 swapchain**, and grabbing the screen
*region* under the window photographs whatever is actually on top of it - which
has already gone wrong once on this machine. The runtime has no built-in
screenshot facility either (checked; the only capture-ish string in the plugin
is `CAPTURE_START_STATUS`).

**Windows Graphics Capture** is the correct API - it captures a specific
window's composited output including D3D12, and being bound to an HWND it
cannot pick up another window's pixels. `pip install windows-capture`, and the
HWND is resolved **from the PID we launched**, never by matching a window
title.

    python tools/capture_intro.py --seconds 34 --interval 1.5 --outdir out/shots
    python tools/score_shots.py "out/shots/*.png"
    tools/try_variant.sh <w> <h> <tag>     # encode + install + capture + score

`score_shots.py` exploits the fact that the corruption is purely vertical:
adjacent ROWS differ wildly while adjacent COLUMNS barely differ at all. The
headline number is that row/col gradient ratio - the clean menu measures ~0.7,
the corrupted intro 40-60.

**The score is noisy.** The same build measured 58.55 and 44.27 on two runs, so
only differences well beyond ~30% mean anything.

### Geometry sweep - and why geometry is not a free variable

| variant | ratio | row diff | sat | verdict |
|---|---|---|---|---|
| 320x576 all-intra | 44-59 | 8.3 | 2.6 | **best** |
| 640x576 | 53.4 | 10.8 | 2.1 | within noise of baseline |
| 320x1152 | 64.3 | 9.1 | 22.2 | worse - full colour garbage |
| 320x288 | 35.2 | 5.3 | 62.1 | worse - see below |

The half-height run scored *best* on striping and was plainly the worst
picture: the video filled only the top half of the frame with solid green
below. That is the useful finding - **the game blits the decoded video 1:1 with
no scaling**, so the dimensions must match what it expects. Geometry is
constrained, not free, and 320x576 is the end of that road. It also shows why
the score alone must never be trusted: a metric that rewards "fewer stripes"
rewards a frame that is half empty.

### Everything that was tested and ruled out

Measured with `tools/try_cvar.sh` / `tools/try_variant.sh`, all against a
baseline whose own run-to-run spread is **44-59**, so nothing inside that band
means anything.

| lever | result | ratio |
|---|---|---|
| `native_2x_msaa=true` | no effect (and the bare flag never applied at all) | - |
| `direct_host_resolve` true / false | no effect | 43.7 / 44.9 |
| `mrt_edram_used_range_clamp_to_min=false` | no effect | 40.0 |
| `pre_mask_resolve_l2_block=false` | no effect | 53.0 |
| `gpu_3d_to_2d_texture=false` | no effect | 44.8 |
| `readback_resolve_half_pixel_offset=true` | no effect | 44.3 |
| `draw_resolution_scale_x/y` 1 and 2 | no effect | 58.6 / 44.9 |
| `d3d12_readback_resolve=true` (~5fps) | no effect - **not a decode/resolve race** | 45.5 |
| width 256 (256-byte aligned pitch) | **pitch hypothesis disproved** - see below | 56.0* |
| width 640 | within noise | 53.4 |
| height 288 / 1152 | worse, and geometry is not free | 35.2 / 64.3 |

\* The 256-wide run *looked* like the best score at 32.9 until the coloured gap
either side was excluded; measured only inside the picture it is 56.0, i.e.
unchanged. A headline score that improves because part of the frame stopped
being a picture is not an improvement.

**Two measurement traps worth remembering**, both of which produced a wrong
answer before being caught:
1. `try_cvar.sh` does not reinstall videos, so a cvar run right after a geometry
   run silently measured the *previous* experiment's files. The driver now
   checksums the installed video against the reference and warns.
2. The score is computed over the whole content box, so solid fill (green
   uninitialised surface, colour garbage) drags it toward "clean". Always look
   at the frame; never promote a variant on the number alone.

### Xenia Canary plays this intro perfectly - proof the content is fine

`tools/capture_intro.py --exe <xenia_canary.exe> --no-default-args --game-args
<path>/game/default.xex` runs the very same extracted game files under Xenia
Canary (canary_experimental@49c700b9c, Jun 2026) and captures the result.

It renders the intro **flawlessly** - a sunrise over cloud-covered mountains,
full colour, no striping - from the **stock, untouched** .wmv files. Measured
row/col gradient ratio **0.8-2.1** against our **39-59**. Xenia's title bar
reports `Direct3D 12 - RTV/DSV`.

That settles several things at once: the disc files are fine, the videos need
no re-encoding, and the decode is entirely solvable on a modern GPU. The fault
is specific to this project's GPU backend.

(It also corrects an old note in this README. "RTV is broken here" was measured
back when fibers and low memory were still broken. On the RTV path the **menu
renders perfectly** - it is simply not the video fix.)

### Xenia's settings, applied here - all null

Xenia writes its full configuration on first run, which makes a direct
comparison easy. Everything relevant was tried against a 39-59 noise band:

| Xenia setting | tried here | ratio |
|---|---|---|
| `render_target_path` = RTV/DSV | `--render_target_path_d3d12=rtv` | 44.8 |
| `clear_memory_page_state` - *"Use for 'Team Ninja' Games"* | `=true` | 40.1 |
| both together | | 41.7 |
| `gpu_allow_invalid_fetch_constants = true` | `=true` | 63.4 |
| `draw_resolution_scale_x/y = 1` | `=1` | 58.6 |
| `mrt_edram_used_range_clamp_to_min` | `=false` | 40.0 |

`clear_memory_page_state` was the most promising lead in the whole hunt - its
own description says *"Refresh state of memory pages to enable gpu written
data. (Use for 'Team Ninja' Games to fix missing character models)"*, and NG2
is a Team Ninja game. It changes nothing here.

**Careful with this comparison.** Two of these runs initially looked like huge
wins (ratio 6.7 and 7.6) purely because the stock videos were still installed
from the Xenia test - the score had dropped because the picture was *different
garbage*, not better. `try_cvar.sh` now checksums the installed video against
the reference before every run.

### It is also not the recompiler

Worth ruling out, since we are a static recompilation rather than an emulator:
the video decoder is VMX-heavy code that builds GPU command buffers, so a
mistranslation there would look exactly like this while leaving simple
rendering intact. Rebuilt with `skip_lr`, `ctr/xer/cr/reserved_as_local` all
**off** - ratio 42.6, i.e. unchanged. The codegen optimisations are innocent
and have been restored.

### Where this leaves the video

Source-side levers are exhausted - codec, GOP structure and geometry have all
been tested and either fixed or ruled out. What remains is per-row,
macroblock-wide corruption plus absent chroma, in a decoder that lives inside
`rexgpu-xenos.dll`. The re-encode is `yuv420p` / `chroma_location=left`,
identical to stock, so nothing about the file is withholding colour. Fixing the
rest needs the plugin's source.

Worth keeping in proportion: Xenia's compatibility entry for this title is
"loads, plays some sounds for a moment and crashes". This build boots to a
pixel-perfect menu with no behaviour-changing guest hooks.

### Gotcha: a bare `--flag` does not set a boolean cvar

`--native_2x_msaa` on its own is **silently ignored** - no error, no warning,
no effect. It must be written `--native_2x_msaa=true`. Unrecognised and
valueless options both parse as success, so "the flag made no difference" is
never evidence on its own.

Always confirm a cvar actually applied by looking for its side effect in the
log, with a matched control run of the same duration:

```
--native_2x_msaa=false   234 lines   "2x MSAA is not supported" x1
--native_2x_msaa=true    232 lines   "2x MSAA is not supported" x0
```

Equal-length runs are the point - a shorter run can miss a late log line and
look like the flag worked. The first attempt here used the bare form, so the
setting was never actually under test.

Ruled out: no shader translation failures (all 8 pipelines translate cleanly),
and the normal render path is provably correct (see the title screen).

This looks like a **GPU-plugin-level** issue - EDRAM reinterpretation across
MSAA modes - which cannot be fixed from this project without the plugin source.

### What the research turned up

- **Xenia cannot run this game at all.** Its compatibility entry for title
  `544307D5` reads "Loads, plays some sounds for a moment and crashes", so there
  is no prior art for NG2 - and this recompilation is already further than the
  emulator.
- **Unleashed Recompiled has no video module.** It inherits playback from the
  recompiled game code, which says this class of problem is expected to be
  solved inside the GPU layer.
- **VC-1 residuals are signed** (9-bit signed residual + 8-bit unsigned
  prediction), matching the `k_16_16_16_16` buffers in the trace.

### Note on the frame dumper

`ng2DiagFrameTick` + `tools/untile.py` can copy resolve targets out of guest
memory, but **the output is not trustworthy** - untiled frames show uniform
striping identical across frames and a palette that does not match the screen,
which points at the untiler (try `--linear`), not the game. It also needs
`--d3d12_readback_resolve`, which drops the game to about **5 fps** - far too
slow to reach the menu. Debugging only; never use it for normal runs.

Three ways forward, in increasing order of effort and payoff:

1. **Skip the intro.** Cheapest, and a legitimate QoL option in its own right.
2. **Chase the decode path.** Check `k_16_16_16_16` signedness and the
   range-reduction shaders first - both are classic sources of this symptom.
3. **Replace playback host-side.** Hook the guest's video path and decode with
   the `libavcodec`/`libavutil` the SDK already bundles, presenting frames
   directly and bypassing the guest GPU decoder. This is the fix a mature
   recompilation would ship, and the SDK clearly anticipates it.

### Correction: the game *does* load files

An earlier reading of this said no disc file had ever been opened, inferred
from the absence of VFS log lines. That was wrong - the runtime does not log
successful opens. Breaking directly on
`rexruntimerd!rex::kernel::xboxkrnl::NtCreateFile_entry` shows **16 opens**
during init phase 2, via:

```
sub_822F34B0 -> sub_822F3708 -> sub_823D5930 -> sub_823D5A20 -> sub_8380EB88
```

which matches the ~16 sound and common-data files the binary names
(`ng2_sound.xgs`, `ng2_ssfile.sso`, `stream_bgm.xsb`, `ng2_hit_data.dat`, ...).
So the loader works. *Absence of logging is not absence of behaviour* - check
the runtime entry point directly.

### Thread health

All eight guest threads are healthy at the fault: four parked on a semaphore in
`sub_8380E028`, two on `KeWaitForSingleObject`, one waiting in `sub_83818F90`,
and the render thread (`sub_822F16D0`, created by init phase 2) running. The
D3D12 presenter is actively painting. Nothing is deadlocked.

## Fixed along the way

**setjmp/longjmp were the big one.** The resource parsers use
`setjmp`/`longjmp` for error recovery — `sub_83831098` does
`bl 0x83955c50; cmpwi r3,0; bne <handler>`, and the error callback longjmps
back. With `setjmp_address`/`longjmp_address` unset in the manifest, the
callback *returned* instead of unwinding, execution fell through into a copy
loop whose bounds check was meant to be fatal, and it overran a 256-byte stack
buffer — destroying an interface pointer two frames up and crashing later on a
garbage indirect call. Declaring both addresses fixed it outright and carried
the game from the middle of init phase 2 into the main loop.

The pair is easy to miss: `_setjmp` at `0x83955C50` has no `.pdata` entry of
its own, sitting inside `longjmp`'s. That is why re:Blue names both explicitly.

Worth remembering as a *diagnostic pattern*: a crash on a garbage pointer, some
distance from any obvious cause, is a plausible signature of missing
setjmp/longjmp — because an un-modelled longjmp turns "abort this operation"
into "carry on with invalid state".

## Known gaps

- **Missing functions with no `.pdata` entry are the dominant failure mode.**
  Five have been found so far, each by crashing into it: a DLC costume, New
  Game, Proceed on the chapter card, and the stage load. Expect one per new
  area. The fix is mechanical (read the address from the `[FATAL]` line,
  disassemble, register it in `config/functions.toml`, rebuild), and **two
  different bulk scanners have failed** to find them ahead of time - see the
  note on `scan_missed.py` above, and the v0.1.2 changelog entry for the
  second attempt. The runtime is the only reliable oracle.
- **The pre-rendered videos decode corrupted**, inside the SDK's GPU plugin -
  see `bugreport/REXGLUE-BUG-video-decode.md`. The attract-mode demo video is
  worse than cosmetic: it desynchronises the GPU command stream and freezes
  the picture while the game runs on. The settings menu can skip the videos,
  which avoids both; the decoder itself needs plugin source, or the guest
  video path hooked and decoded host-side with the libavcodec the SDK bundles.
- **Mission Mode needs the title update.** Three of the four DLC packages are
  costumes and they work. Mission Mode does not appear on the main menu, and
  it is not a licence or content-store problem - a full `license_mask` and
  installing the content under the signed-in profile's XUID both change
  nothing. The base game simply has no entry for it without the game update,
  which is what players are told to install when it is missing. A title update
  patches the executable, so for a static recompilation it means applying the
  `.xexp` to the XEX and running codegen again; `rexglue` has no `.xexp`
  support, so that patch would have to be applied by another tool first, and
  every entry in `config/functions.toml` re-verified against the new layout.
- **Bloom cannot be exposed as a setting yet.** `g_fBloomMod` and
  `g_fBloomSub` are pixel-shader constants c16 and c15 of a `ps_3_0` shader
  whose constant table is at `0x821B2990`, but nothing in the image points at
  that shader: no stored pointer into the blob, and no `lis`/`addi` pair in
  24 MB of `.text` that builds its address. The game reaches it through a
  table it indexes, so the code that uploads c15/c16 is not reachable from the
  constant names. Xenia has no bloom patch for this title either.
- **MSVC SEH funclets and `non_volatile_as_local`.** The SDK's `FunctionConfig`
  has a `shareRegisters` flag for exactly this: a funclet reads the registers
  its owner left live and would see zero from a localized copy. NG2's generated
  code currently contains no `__try`/`__except` at all, so nothing needs it yet
  - but if SEH shows up later, funclets must be marked `shareRegisters = true`
  in `config/functions.toml`.
- `XLiveBaseUnk58046` (xam message `0x58046`, called once from `0x8385F4FC`) is
  unimplemented in the runtime. Not yet proven to be load-bearing.
- `tools/scan_missed.py --write` exists but should not be used unattended - see
  the false-positive note above.
