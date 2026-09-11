# Ninja's Dawn

A static recompilation of **Ninja Gaiden II** (Xbox 360, title `544307D5`) to a
native Windows executable, built on the ReXGlue SDK.

It is not an emulator. The game's PowerPC code is translated ahead of time into
native x86-64, so there is no interpreter and no JIT warm-up: the game runs as a
Windows program, at 60 fps, with PC settings in front of it.

---

## What you must bring

**This project ships no game data of any kind.** No textures, no videos, no
saves, no DLC, no executable image from the disc. You provide all of it, from
your own copy.

You need **all** of the following:

| | What | Why |
|---|---|---|
| **1** | **Your own copy of Ninja Gaiden II for Xbox 360**, as a disc image (`.iso`) or an already-extracted folder containing `default.xex` | This is the game. Nothing here works without it. |
| **2** | **Windows 10 or 11, 64-bit** | The build is Windows-only. |
| **3** | **A GPU with Direct3D 12 and 2 GB of video memory** | 4 GB or more if you want the upscaled texture pack. |
| **4** | **About 8 GB of free disk space** | ~6.7 GB for the extracted disc, plus the port. |

Optional, and only if you want the extra features:

| | What | For |
|---|---|---|
| | **Your own Xbox 360 DLC packages** (the extensionless STFS files from `<content>\544307D5\00000002\`) | The DLC weapons and costumes. |
| | **Python 3** on PATH | The texture-pack tools. Without it the texture buttons say so and stay disabled. |
| | **A 43 MB Real-ESRGAN download** (one button in the settings) | The AI texture option. Deliberately not bundled — it is third-party software under its own licence, and whether to have it on your machine stays your decision. |

### Where to get the game

Nowhere in this project, and please do not ask. Dump your own disc, or extract
your own legally purchased copy. If you do not own Ninja Gaiden II, this
software has nothing to offer you.

---

## Setting it up

1. **Run `ng2.exe`.** The first launch opens a setup screen and runs a one-time
   hardware detection to pick sensible defaults for your card.

2. **Point it at your game**, either:
   - **Choose folder…** — a folder that already holds your extracted disc, with
     `default.xex` sitting in it; or
   - **Choose disc image…** — your own `.iso`, which it extracts for you
     (~6.7 GB, under a minute). The runtime mounts a folder rather than an
     image, which is why the copy is needed.

   It reads the title ID out of the disc, so it will tell you if you have
   pointed it at the wrong game.

3. **Optional — point it at your DLC folder.** The packages install into the
   game's content store on every launch; doing it twice is harmless.

4. **Optional — import your saves.** Xbox 360 save folders can be brought in on
   the same screen.

5. **Press Play.**

The setup screen comes back whenever you hold **Shift** while launching, or from
the "Show the setup screen at the next launch" tick in the settings.

---

## Building from source

This repository is the project's own code and documents; it holds no game
material and no built executable, because `ng2.exe` contains the game's own
translated code. You build it from your copy of the game.

1. **Tools.** Visual Studio 2022 Build Tools, LLVM/Clang in `C:\Program
   Files\LLVM`, Ninja and CMake, Python 3.12. The [ReXGlue](https://github.com/)
   SDK goes beside this folder as `..\RexBlue\win-amd64` (that is where
   `tools\build.cmd` looks); this port needs the runtime and the Xenos GPU
   plugin built from a tree that carries the fixes listed under
   *What this is built on*.

2. **Inputs.** Put the game's `default.xex` in `assets\` and the extracted disc
   in `game\`. Both folders are ignored by git and must stay that way.

3. **Generate and build.** `tools\build.cmd Release` runs the code generation
   (`rexglue codegen`) and then CMake and Ninja. Two fixes live outside the
   generator's reach and are applied to its output by scripts; run them after
   any regeneration and build again (only a handful of files recompile):

   ```
   python local\diag\patch_missed_regs.py
   python local\diag\patch_scanguard.py
   tools\build.cmd Release
   ```

   Every other fix is a hook declared in `config\hooks\patches.toml` or code
   under `src\`, and survives regeneration on its own. Do not run
   `local\diag\patch_fix_ge2.py`; its header explains why.

4. **Package.** `python tools\make_release.py` stages a runnable folder and a
   zip under `..\Releases`, with a provenance file listing the hash of every
   source that went in.

The full account of every problem met along the way, and its fix, is
[docs/ISSUES_AND_FIXES.md](docs/ISSUES_AND_FIXES.md).

---

## Keys

| Key | What it does |
|---|---|
| **F10** | The settings menu, over the running game |
| **F9** | Switch the upscaled texture pack on and off, live, so you can see the difference |
| **F8** | Show or hide the on-screen FPS / GPU / VRAM readouts |
| **F4** | Every runtime setting, unfiltered (the SDK's own screen) |
| **F3** | Frame-timing overlay |
| **`** | Console |
| **Esc** | Back out of the settings |

F9 and F8 do not write to your settings file — they are for looking, so a
comparison keypress cannot overwrite the preference you chose.

---

## Every setting

Settings that can change while the game runs do so immediately. Settings the
window or the guest's video mode were built from are saved now and applied on
the next launch, and are marked **restart required** in red rather than
accepted and quietly ignored.

### Display

| Setting | Default | Notes |
|---|---|---|
| Window size | 1280 × 720 | (restart) |
| Fullscreen | off | |
| Monitor | 0 | Which display to open on. (restart) |
| Frame rate | 60 | 30–144. |
| V-Sync | on | |
| Internal render size | 1120 × 584 (as shipped) | The game renders at 1120 × 584 and scales up. 1280 × 720 removes that upscale - the same change as the Xenia community patch for this title. (restart) |
| Internal supersampling | 1× | 1–8×. Renders above output size and downsamples. Also scales shadow maps, because the SDK's resolution scale applies to resolve targets. 4× and 8× are there for hardware that does not exist yet; 2× is the useful setting today. (restart) |
| Letterbox | on | Keeps the original aspect instead of stretching. |
| Dither the output | off | Hides colour banding on 8-bit displays. |

An **Autodetect** button re-runs the first-launch hardware detection and picks settings for your card again.

### Enhancements

| Setting | Default | Notes |
|---|---|---|
| Anisotropic filtering | auto | |
| Anti-aliasing | none | |
| Output filter | bilinear | Bilinear, or FidelityFX CAS. |
| CAS sharpness | 0.0 | Only meaningful with CAS selected. |
| Skip intro videos | off | Skips the openings and the attract demos. **Chapter-loading videos are deliberately left alone** — the game reads a failed open of one as a bad disc and stops. |
| Skip chapter cinematics | off | Presses A/START for you through the in-engine cinematic at a chapter start. Any real controller input disarms it instantly, so a cinematic you want to watch is one stick nudge away from being left alone. |
| Fuzzy alpha | off | Workaround. |
| Accurate depth | off | Workaround. |

### Textures

| Setting | Default | Notes |
|---|---|---|
| Texture folder | — | Where the dump and the finished pack live. |
| Dump textures while playing | off | Writes each texture the first time it is seen. Turn it off once you have a pack. |
| Use the upscaled pack | off | Needs a pack to have been made first. **F9** toggles it live. |
| Upscale factor | 2× | 2×, 4× or 8×. |
| Upscaler | Lanczos | **Lanczos** is a high-quality resample built in and needs nothing extra. **Real-ESRGAN AI** is a trained model that adds detail, and needs the 43 MB download plus a Vulkan-capable GPU. The AI entry only appears once the upscaler is installed. |
| Detail strength | 0.75 | How much of the model's **fine detail** is laid over the original. Not a cross-fade: the tone and colour stay the game's. Real-ESRGAN is photo-trained and denoises hard — on a smoke texture it cut mean brightness by 40%, because faint wisps against black are exactly what it treats as noise. Only the detail is taken. |
| Texture cache | plugin default | How much host memory the GPU plugin may hold textures in, up to 8 GB. |

Making a pack: set a folder, turn dumping on, play through the areas you care
about, turn dumping off, then press **Process textures**. The tools run in a
hidden child process and report progress in two steps (decode, then upscale);
the pack is loaded on the next launch or the next F9.

The counts above the button are textures that **can be enhanced**: how many
were dumped, how many are in the pack, how many are waiting. HUD, fonts, video
frames and other non-art are dumped too but never packed, by design, and are
not counted. A run processes only the waiting textures unless **Redo textures
already in the pack** is ticked; the line under the button says exactly what
it will do and at which scale, upscaler and strength. The pack records what it
was made with in `pack/pack.txt`; change the scale, the upscaler or the detail
strength and the next run redoes every texture, and the menu says so first.
"Enhanced textures: ON" below the counts, with the number of enhanced and
original textures loaded, is read from the renderer and answers whether the
pack is actually in use.

Dumping and using the pack are mutually exclusive. Together they put a file
write and a folder stat on the GPU thread for every texture the game creates,
which starves the command stream - so turning one on turns the other off.

**Per-stage warming.** A large pack does not fit in the texture cache: 6 GB of
replacements against a 4 GB ceiling means the cache evicts art it is about to
need again, which is felt as stutter while a scene streams in. One stage's worth
does fit. So the port records which textures each chapter actually uses - in
`<texture folder>/pack/stages/chNN.txt`, listing ids rather than copying files -
and reads that stage's files when the chapter loads, while a loading screen is
already on screen.

The lists are built by playing. A stage you have not visited yet has no list and
simply behaves as it did before, so this is never worse than not having it, and
the benefit arrives on your second visit to a stage rather than your first.

### On-screen readouts

| Setting | Default |
|---|---|
| Show readouts (**F8**) | off |
| FPS | on |
| GPU load | on |
| Video memory | on |
| Live cost bars in this menu | on |

The first four are the on-screen readout during play. The last is separate: the
CPU/GPU/VRAM bars drawn inside the Textures section, for watching the cost move
while you toggle the pack with F9.

### Other

| Setting | Default | Notes |
|---|---|---|
| Hide the cursor after | 5 s | |
| Keyboard control | off | |

### Report a problem

The settings menu has a **Copy diagnostics to a file** button. It gathers this
session's log, your settings and what the machine is into one text file, copies
its path to the clipboard and opens the folder. It contains no game data — but
it does name your folders, so have a look before you post it.

If the game never reaches the menu, set the environment variable
`NG2_DIAGNOSTICS=1` and launch; the same file is written during startup.

---

## Known issues

- **True ultrawide rendering is not there.** A 21:9 internal render size makes
  the 3D scene wider but the game's 2D layer does not follow: chapter cards
  stretch and the credits sit off centre. What works is an ultrawide window
  with "Keep aspect ratio" on, which pillarboxes.
- **The attract demo froze the game in earlier versions**, from a second
  ring-buffer failure unrelated to the one fixed in v1.0.0. It has not been seen
  since the ring fix was fully deployed, but it is not closed until a long
  unattended run says so.
- **Chapter 12 runs at around 28 fps.** Playable; not explained.
- **Chapter-loading videos cannot be skipped**, by design: the game treats one
  of them failing to open as a bad disc.
- **Achievements earned in a chapter are awarded at the next chapter load**,
  not at the chapter end. The chapter transition itself is fixed (v1.0.6).
- **The AI texture option** needs Python and a download this release does not
  bundle. The plain upscaler works without either, and the buttons say what is
  missing rather than sitting there greyed with no reason.

Fixed and no longer listed here: the red mist after a boss that never loaded
the next chapter (v1.0.6), the music that died mid-session (v1.0.2), the
chapter 13 boss crash (v1.0.4). Every issue ever met, with its cause and fix,
is in [docs/ISSUES_AND_FIXES.md](docs/ISSUES_AND_FIXES.md).

---

## Digging deeper

The interesting parts of this port are the investigations, not the code. They
are written up in `docs/`:

| Document | What it answers |
|---|---|
| [ISSUES_AND_FIXES.md](docs/ISSUES_AND_FIXES.md) | Every issue met while porting, its cause and its fix, by area, from v0.1.0 to v1.0.6. |
| [XENIA_ISSUES.md](docs/XENIA_ISSUES.md) | Every label on Xenia's compatibility issue for this title, and what this port does about each. Includes what the census does *not* cover. |
| [VECTOR_COVERAGE.txt](docs/VECTOR_COVERAGE.txt) | How the garbled videos were traced to VMX128 registers v64–v127 reading as zero — including the two dead ends that ruled out the video files and the SDK's decoder first. |
| [VIDEO_DECODE.md](docs/VIDEO_DECODE.md) | The game decodes its own WMVs in recompiled guest code; it imports no media APIs at all. |
| [TEXTURE_PACK.md](docs/TEXTURE_PACK.md) | The texture dump/upscale/pack pipeline and its file format. |
| [COMPATIBILITY.md](docs/COMPATIBILITY.md) | Whether the approach that produced re:Blue transfers to this title. Mostly the method does and none of the work does. |
| [DEVELOPMENT_JOURNAL.md](docs/DEVELOPMENT_JOURNAL.md) | The long form, in order, including the wrong turns. |

---

## What this is built on

[ReXGlue](https://github.com/) static recompilation SDK. The runtime and the
Xenos GPU plugin shipped here are built from source rather than taken from a
stock SDK drop, because this port depends on fixes made in that tree:

- **VMX128 registers v64–v127 were being localised into zero-initialised
  variables**, so the video codec's inverse-transform kernels read their permute
  control vectors as zero and garbled every pre-rendered video. Seven functions
  in the whole title did that, and all seven were the codec.
- **The GPU ring buffer's read pointer was reset without the write pointer.**
  Ninja Gaiden II re-initialises the ring at every mode change, so the parser
  would compare a fresh read pointer against a write pointer from the ring's
  previous life, read that as a wrap, and execute 32 KB of dead commands. That
  is what froze the game at the attract demo.

---

## Legal

This repository contains **no** Ninja Gaiden II code, assets, or data. Ninja
Gaiden II is © Tecmo Koei. You must supply your own legally obtained copy.

The MIT licence in this repository applies to the project's own source — the
build configuration, the tools, the launcher and its settings UI. It does not
and cannot apply to anything derived from the game.
