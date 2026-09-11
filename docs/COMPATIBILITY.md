# Blue Dragon vs. Ninja Gaiden II: does the same approach work?

Written to answer a specific question: re:Blue proved this toolchain works for
*Blue Dragon*, but does Ninja Gaiden II do anything different enough to break
the method? Short answer: **the method transfers, essentially none of the work
does**, and the two features that dominated re:Blue's effort are the two that
are hardest for NG2.

Everything below is either measured from `assets/default.xex` in this project or
read out of re:Blue's own configuration, except the two rows marked *(published)*.

## What makes them compatible

Both are Xbox 360 titles: PowerPC Xenon CPU, Xenos GPU, XMA audio, the same
kernel (`xboxkrnl`/`xam`). That alone would not be enough — what matters more is
that **both were built with the same Microsoft Xbox 360 MSVC toolchain**.

NG2's binary is `gaiden2_Release_LTCG.exe` (PDB path `c:\gaiden2\out\Release_LTCG\gaiden2.pdb`),
and its recompiled output references all six MSVC PowerPC CRT helper families:

| Helper family | Distinct entry points in NG2 |
|---|---|
| `__savegprlr` / `__restgprlr` | 18 / 18 |
| `__savefpr` / `__restfpr` | 18 / 18 |
| `__savevmx` / `__restvmx` | 82 / 82 |

These are the multi-entry prologue/epilogue helpers a recompiler *must*
special-case. rexglue recognises all of them in NG2 by name, unchanged. The same
is true of the other deep toolchain concerns — jump tables (codegen resolved
NG2's into C++ `switch` statements), C++ exception state, and vtable layout.

That is the compatibility that actually matters, and it is confirmed working:
NG2 analyses with **0 errors across 42,504 functions**, codegens, compiles,
links, and executes guest code under the SDK's stock Xenos GPU plugin.

## What is different, and why it matters

| | Blue Dragon | Ninja Gaiden II |
|---|---|---|
| Genre / loop | Turn-based JRPG | 60 fps-target character action |
| Discs | 3 *(published)* | **1** (XEX says disc 1/1) |
| Native resolution | 720p *(published)* | **585p** *(published)* |
| Frame rate | Locked 30 *(published)* | **Variable 30–60 with tearing** *(published)* |
| Guest DLL modules | — | **None** (imports are `xam.xex` + `xboxkrnl.exe` only) |
| `.text` | not measurable here | 24.2 MB / 6.06M instructions |
| Float + vector share of `.text` | — | **30.9%** (19.2% scalar FP, 11.7% VMX128) |

Four consequences:

**1. NG2 is far more SIMD-dense.** Nearly a third of NG2's instruction stream is
floating-point or VMX128 — 707,691 vector instructions. This is what a 60 fps
action game's animation, physics and particle math looks like. It raises the
stakes on VMX128 translation correctness (a rarely-hit vector edge case is a
subtle visual or physics bug, not a crash) and makes recompiled vector
throughput a real performance concern rather than a footnote.

**2. Single disc removes a whole category.** re:Blue needed multi-DVD handling
(`disc_io.toml` skips the disc-number store so every disc-change check sees "no
disc"). NG2 ships on one disc, so that work simply does not exist here.

**3. Simpler module graph.** NG2 imports only `xam.xex` and `xboxkrnl.exe` —
everything else is statically linked. There are no guest DLL modules to
recompile alongside the main executable.

**4. The two biggest QoL features are harder, not easier.** This is the
important one. Of re:Blue's 127 midasm hooks:

| Hook file | Hooks | Purpose |
|---|---|---|
| `frame_interp.toml` | 27 | Unlock frame rate |
| `output_resolution.toml` | 27 | Raise resolution |
| `render_tweaks.toml` | 16 | Renderer adjustments |
| `pso_predictor.toml` | 15 | Pre-compile PSOs to avoid stutter |
| `hud_anchor.toml` | 9 | Ultrawide HUD |
| `hud_fade.toml` | 6 | HUD fade |
| everything else | 27 | title menu, save, tutorials, cutscene, battle, … |

**42% of all hooks exist to unlock frame rate and raise resolution.** Both are
harder for NG2:

- *Frame rate.* Blue Dragon is locked 30 with fixed-step accumulators, so
  re:Blue's approach is "skip one accumulator store on interpolated frames",
  applied 27 times. NG2 already runs at a *variable* 30–60, and its combat is
  built on frame-counted invincibility windows and cancel timings. There is no
  single accumulator to gate, and getting it wrong changes how the game plays
  rather than just how it looks.
- *Resolution.* 585p is not an arbitrary choice: 1280×585 is about what fits in
  the Xbox 360's 10 MB EDRAM with MSAA without resorting to predicated tiling.
  (The constant 585 appears 8 times as an immediate in `.text`.) Raising it
  means understanding render-target sizing that was deliberately tuned to that
  constraint.

The upside: native execution alone eliminates NG2's notorious frame drops, since
the 30–60 variance was a Xbox 360 performance limit, not a design choice.

## What does not transfer at all

Every one of re:Blue's 127 hook addresses is a Blue Dragon address. So is its
`[rexcrt]` block, which maps the game's statically-linked CRT file functions
(`CreateFileA`, `ReadFile`, `SetFilePointer`, …) to guest addresses so the SDK
can substitute host implementations — 13+ functions that must be located by hand
in each binary. So are its 1,615 named functions.

re:Blue's ~78,000 lines of host code split roughly into:

- **Portable in principle** — installer wizard, profiles, config, VFS, input,
  mod manager (~11k lines). Adaptable with work.
- **Game-specific** — `src/engine` (24k lines) and `src/gpu` (35k lines) are
  written against Blue Dragon's own structures. Not reusable.

NG2 also brings work Blue Dragon did not: XACT3 streaming audio (two wavebanks,
209 MB and 172 MB), WMV/VC-1 cutscenes (`libavcodec` ships in the SDK), and four
`LIVE` STFS DLC packages.

## Verdict

Nothing about Ninja Gaiden II is *incompatible* with the re:Blue approach — the
hardware, toolchain, compiler idioms, and SDK all line up, and the first four
pipeline stages already work end to end. What differs is the amount and shape of
the game-specific work: NG2 removes the multi-disc problem, adds a much heavier
vector workload, and makes the two features that consumed the largest share of
re:Blue's hook budget substantially harder.

Treat re:Blue as proof the road exists, not as a shortcut along it.
