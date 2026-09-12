# Issues and fixes

Every problem met while bringing Ninja Gaiden II to the PC with the ReXGlue
static recompiler, and what fixed each one, from the first packaged build
(v0.1.0, 2026-09-03) to v1.0.6 (2026-09-11). The changelog is the record of
what shipped when; this document is the record of what went wrong and why,
grouped by area so that a symptom can be looked up without knowing which
version fixed it.

How to read an entry. Each has a title stating the symptom as a player or a
builder saw it, then four fields: **Symptom** (what was observed), **Cause**
(what was actually wrong), **Fix** (what changed, naming the file, hook,
setting or script), and **Version** (the changelog version that shipped it, or
the record it comes from when it never had a changelog entry of its own). An
entry that describes something later withdrawn or replaced says so in the
entry itself, so that a superseded fix is never read as the current one.

Entries carry an id (B1, R2, ...) so that they can cross-refer. The appendix at
the end maps every one of the changelog's 136 headings to the entry that
covers it, so what this document does not cover can be counted rather than
guessed.

| Area | Entries |
|---|---|
| Boot and runtime | 8 |
| Recompiler and generated-code defects | 8 |
| Video and cinematics | 11 |
| Display, resolution and graphics | 12 |
| Audio | 2 |
| Chapter transitions and crashes | 4 |
| Texture pack and AI upscaling | 17 |
| Settings, installer and launcher | 20 |
| Input | 5 |
| Tools and diagnostics | 15 |
| Still open | 14 |

Guest addresses and function names below are those of the game's retail
executable (`sub_<address>` is a recompiled guest function). Nothing here
reproduces any game data.

---

## Boot and runtime

**B1. The game crashed on a garbage pointer, far from any obvious cause**

**Symptom.** Init phase 2 died on an indirect call to a garbage target, some
distance from anything that looked wrong. The first diagnostic hooks in
`config/hooks/diagnostics.toml` were written for exactly this crash.

**Cause.** The game's resource parsers use `setjmp` and `longjmp` for error
recovery. With neither declared in the manifest, an error callback returned
instead of unwinding, execution fell through into a copy loop whose bounds
check was meant to be fatal, and a 256-byte stack buffer was overrun, wrecking
an interface pointer two frames up. The crash came much later. `_setjmp` is
easy to miss because it has no `.pdata` entry of its own and sits inside
`longjmp`'s.

**Fix.** `setjmp_address` and `longjmp_address` declared in
`ng2_manifest.toml` (0x83955C50 and 0x83955930). This carried the game from the
middle of init phase 2 into its main loop. A crash on a garbage pointer far
from its cause is now a known signature of a missing `setjmp` pair.

**Version.** v0.1.0.

**B2. The screen stayed black: the game's tasks are fibers**

**Symptom.** Both init phases completed, the main loop ran, the GPU resolved a
frame sixty times a second, and the picture stayed blank. The scheduler's
context switch ran sixty times a second with exactly one target, forever, and
the front-end sequencer task never executed.

**Cause.** The game registers each task as a fiber (`sub_822F2CA0` calls its
own `CreateFiber`). A guest-implemented fiber switch cannot work under static
recompilation: the switch at 0x83819200 saves and restores guest registers, but
the recompiled call state lives on the host stack and in C++ locals, so
restoring guest registers transfers nothing and the target fiber never starts.

**Fix.** The fiber family is mapped to the SDK's host implementations in
`config/rexcrt.toml`. The critical detail is that `SwitchToFiber` must be
mapped at 0x8380E638, the fast-path second entry the scheduler actually calls
every frame, not at the function's real entry at 0x8380E5D8. Mapping only the
real entry left the hot path in guest code and changed nothing. When replacing
a guest function, check whether anything branches into the middle of it.

**Version.** v0.1.0.

**B3. Reads of low guest memory faulted, including page zero**

**Symptom.** Faults reading guest addresses such as 0x59BF and 0x4F60 during
boot; later, a deterministic read of guest address 0.

**Cause.** The game's 160 slot records come out of BSS with a valid-looking
type of 0 and a null data buffer, and hundreds of sites index off them. On the
console those reads land in low memory and return zero, which every consumer
treats as "nothing here". Here the pages were not committed, so the same reads
faulted. The game also reads address 0 itself during boot.

**Fix.** `Ng2App::OnPostSetup` commits guest 0 to 0x3FFFF as read-only zeroes:
reads return zero as on hardware, a null write still faults. The range must be
committed read-write first and protected afterwards, because zeroing it writes
through the guest mapping. Opening page zero took the boot from 17 files opened
to 38 and let the large content archives load. The same behaviour is what
Xenia's `protect_zero=false` label asks for, and `ng2_tuning.h` sends that as
well.

**Version.** v0.1.0. The Xenia label is recorded in `docs/XENIA_ISSUES.md`.

**B4. Nothing rendered: the GPU plugin has to be selected in code**

**Symptom.** The runtime came up in "native rendering mode" and silently
ignored every `Vd*` kernel call.

**Cause.** `gpu_plugin` is a `RuntimeConfig` field, not a command-line cvar,
so no launch flag could select it. Separately, the RTV render-target path drew
almost nothing for this title when measured: 22 resolves and 0 pipelines in
30 seconds against 3,128 and 7 on the ROV path.

**Fix.** `Ng2App::OnPreSetup` sets the plugin, and `CMakeLists.txt` stages the
DLL. The ROV path, which emulates EDRAM accurately, is the one used. Pages the
GPU writes also need their state refreshed or the CPU reads stale contents;
`clear_memory_page_state=true` is sent from `ng2_tuning.h`, which is Xenia's
second requirement for this title.

**Version.** v0.1.0.

**B5. Escape did not close the game**

**Symptom.** Escape ran the shutdown and then the process sat there not
responding.

**Cause.** The port's own teardown completed and logged it; the runtime's
graceful path never returned, because the guest's threads are fibers and
something in that path waits forever. The window's close button never hit it
because the SDK hard-exits on that path.

**Fix.** Escape releases what the port owns first: the cheat worker is joined
rather than just asked to stop, since it writes guest memory that goes away
with the runtime; the settings are written; then the runtime is given three
seconds and the process exits anyway. Measured: exited after 3.3 seconds. The
window was dead for those seconds, which was the brief black screen on this
path until v1.0.7.

v1.0.7 removed the wait. Escape now saves the settings, joins the port's own
threads and asks the window to close, which is the close button's path: the
SDK terminates the title and hard-exits at once. The watchdog stays behind it
as a backstop and no longer fires. Measured from the "Escape: quitting" line
to the process being gone: 0.7 seconds. The Fable II port made the change
first and measured the same. `NG2_QUIT_AFTER=<seconds>` fires Escape's code
from a timer so a script (`scratchpad/quit_test.ps1`) can time the exit.

**Version.** v0.3.5, v1.0.7.

**B6. "Quit Game" left a black screen**

**Symptom.** The game's own Quit stopped every guest thread and left the
window open with nothing to draw into it.

**Cause.** The guest's Quit calls `XamLoaderTerminateTitle`. The kernel marks
every guest thread, wakes those in a kernel wait, gives them 200 ms, and
deliberately leaves stragglers running. The main guest thread is a fiber
scheduler, essentially always inside `SwitchToFiber` rather than a kernel wait,
so it never checks for termination and is always a straggler; the SDK's quit
waits on exactly that thread and never returns. The first fix (v0.5.1) watched
guest threads exiting and checked `is_terminating_title()`; it could not work,
because that hook is called from the same wait for the main thread only, and
the flag is cleared before anything could read it.

**Fix.** `TerminateTitle` erases every guest thread from the kernel's map
whether or not it stopped, so the port watches for the main thread's id going
missing, having first seen it present. Quitting then relaunches the port with
`--ng2_setup` to return to the setup screen rather than tearing the runtime
down under threads still running inside it. No SDK change.

**Version.** v0.5.1 (first attempt, did not work), v0.5.3 (the fix).

**B7. The game crashed about 90 seconds after every launch**

**Symptom.** Every launch of v0.5.0 to v0.5.2 died about 96 seconds in, with
the attract demo on screen, so it read as a video bug.

**Cause.** A leftover diagnostic of the port's own: `OnPostSetup` started an
oscillation scan unconditionally, and every 12 seconds it copied about 500 MB
of guest physical memory blind. The guest's physical range is reserved, not
committed, so the copy faulted on the first unbacked page. Every crash dump had
byte-identical fault registers pointing at the scan's hard-coded start
address, and it reproduced identically with texture dumping off and with the
video mode changed.

**Fix.** The profiler and the scan are behind `NG2_DIAG`, opt-in rather than
shipped hot, and the scan copies only committed pages, found with
`VirtualQuery`. The scan is kept because it is what found the ledge loop (R4).

**Version.** v0.5.3.

**B8. The release needed the Visual C++ runtime installed already**

**Symptom.** On a Windows install that never had the Visual C++ 2015-2022
redistributable, the game would not start: a "DLL not found" box and nothing
in the log.

**Cause.** The executable and both SDK DLLs import `msvcp140`, `vcruntime140`,
`vcruntime140_1` and `msvcp140_atomic_wait`, which Windows does not ship.
Found by listing every module the running game loads: those four were the
only ones outside Windows and the release folder (the rest being Windows
itself, the two runtime DLLs, and the game data the player brings).

**Fix.** `tools/make_release.py` copies the four DLLs from the compiler's
redistributable set into the release folder (app-local deployment, which
Microsoft permits). Verified by running a copy of the release folder and
confirming the runtime resolved from beside the executable.

**Version.** v1.0.6.

---

## Recompiler and generated-code defects

**R1. Calls died on functions the analyzer never saw**

**Symptom.** `[FATAL] Call to invalid or unregistered function at guest address
...`, each time in a new place: selecting a DLC costume (0x836D7C08), starting a
new game (0x824356D0), pressing Proceed on the chapter card (0x83682F30),
loading the stage (0x82348688), a crash in chapter 5 (0x8375B108), and two more
when a corrupt system save was deleted.

**Cause.** MSVC emits no `.pdata` for small helpers with no prologue:
`this`-adjusting thunks, delegate trampolines, one-line setters, interlocked
helpers. One sitting in the padding after a real function is absorbed into it
by the analyzer and never becomes callable, and since they are reached only
through function-pointer tables, each is found only by running the code that
calls it.

**Fix.** Each is registered in `config/functions.toml`, with
`tools/add_function.py` computing the extent. The chapter 5 crash led to a
static sweep of `.text` for the adjustor-thunk pattern, `addi r3,r3,<negative>`
followed by a `b` to a known function, which found 82 thunks, 10 of them
unregistered; all ten were registered at once rather than one crash at a time.
Two sizing traps in `add_function.py` are recorded under X10. Bulk static
discovery of the general case does not work: `.text` contains pointer tables
whose entries decode as plausible instructions, so `tools/scan_missed.py` finds
about 1,340 candidates that are mostly false, and a second scanner failed its
own known-answer test. The running game remains the only reliable oracle, and
one new function per new area of the game should still be expected (O10).

**Version.** v0.1.1, v0.1.2, v0.5.1, v1.0.0.

**R2. A callee corrupted its caller's registers**

**Symptom.** A crash where `r31` went null across a call.

**Cause.** With the codegen defaults every function shares one `PPCContext`,
so a callee that loses a non-volatile register corrupts its caller.

**Fix.** The manifest sets `non_volatile_as_local` and the `skip_lr`, `ctr`,
`xer`, `cr` and `reserved_as_local` flags, the configuration re:Blue ships,
giving each function its own r14 to r31 and letting the recompiler elide the
`__savegprlr` and `__restgprlr` helpers. `skip_lr` was checked to be safe
here: there are no `bl $+4` PC-capture idioms in the image.

**Version.** v0.1.0, recorded in `docs/DEVELOPMENT_JOURNAL.md`.

**R3. VMX128 registers v64 to v127 were read as zero**

**Symptom.** Every pre-rendered video in the game was garbled, from the first
run. The full history of that symptom is V1; this entry is the recompiler
defect behind it.

**Cause.** `BuilderContext::v()` in the SDK's codegen turned v64 to v127 into
zero-initialised local variables whenever `non_volatile_as_local` was set, with
no check for whether a function reads such a register before writing it.
Exactly seven functions in the 42,504-function title do, and all seven are the
video codec: two are its inverse-transform kernels, which take their `vperm`
control vectors in v69 and v72 and received zero.

**Fix.** Localisation is limited to v14 to v31, which is what stops a callee
corrupting its caller. This is a change to the codegen tool `rexglue.exe`, so
it takes effect only after the tool is rebuilt, deployed, the stale precompiled
header cleared and every generated file regenerated. The write-up is
`docs/VIDEO_DECODE.md`.

**Version.** v0.5.0.

**R4. The character locked into a looping animation at a ledge**

**Symptom.** Stepping off a platform edge in chapter 5 started an endless
jump-and-bob animation. Camera, menus and frame rate stayed fine; only the
player state machine was stuck. v0.5.1 recorded it as not fixed and ruled out
the controller (a stuck-input detector fired zero times through a full
reproduction) and any known emulation quirk.

**Cause.** A regression from the video fix (R3). The recompiler emits no code
for the `__savevmx` and `__restvmx` prologue helpers, which is safe only while
those registers are per-function locals. Sharing all of v64 to v127 through the
context made the skipped saves load-bearing: `sub_836ECF20` uses v116 to v127
as scratch, never restores them, and is called fifteen times in a loop right
before the animation update. Found with a hardware watchpoint on the animation
progress value in the running game (X9).

**Fix.** Only the registers that genuinely carry a value between functions are
shared, found by scanning all 42,504 functions for one that uses a register
before writing it, with saves excluded. Exactly five qualify: v69 and v72 (the
codec's transform kernels), v64 and v65 (`sub_83A2B890`), v96 (three functions
in `sub_8376xxxx`). This is the codegen option `shared_vector_registers` in
`ng2_manifest.toml`; other titles need their own list.

**Version.** v0.5.1 (not fixed), v0.5.2 (fixed).

**R5. `db16cyc` was translated to nothing**

**Symptom.** None visible. This was pursued as a candidate cause of the
chapter 12 to 13 hang and does not explain it.

**Cause.** `db16cyc` is Xenon's spin-wait hint: it delays about 16 cycles and
hands them to the sibling SMT thread. The recompiler emitted nothing for it, in
twelve spin loops in this title.

**Fix.** It now emits `PAUSE`, plus a real yield every 1024 iterations. This
required rebuilding `rexglue.exe`. The hang theory was wrong because the
starvation it needs cannot occur: `ignore_thread_affinities` defaults true, so
guest threads float across every logical processor and a spinner denies nothing
a core. The claim that `db16cyc` fixes the hang is formally withdrawn.

**Version.** v1.0.0.

**R6. Crash at the chapter 13 boss, with one latent twin**

**Symptom.** A null-pointer write at the chapter 13 boss; the value stored was
24, which byte-swaps to the 0x18000000 seen in the crash dump.

**Cause.** To make stray branches resolve, two functions' epilogue fragments
were registered as standalone `_missed` functions in `config/functions.toml`.
That severs the parent's live non-volatile registers, which are C++ locals per
function: r26 and r27 for `sub_8372398C`, r22 and r31 for `sub_83A566F8`. They
read as zero, and the code stored to guest address 0. All 23 `_missed`
fragments were audited; only these two read a non-volatile before writing it.

**Fix.** At each branch site the live locals are spilled into the shared
context and restored at the fragment's entry. This is a hand edit to the
generated sources, applied by `local/diag/patch_missed_regs.py` (14 sites in
four files), and it is lost whenever the code is regenerated; see "How the
fixes are applied".

**Version.** v1.0.4.

**R7. A "codegen mistranslated `bne`" note that was wrong**

**Symptom.** Four generated sites (in `sub_8245B850`, `sub_82444A90` and
`sub_8246A500`) carried a note claiming the recompiler had emitted a
less-than test for a `bne`, clamping the chapter-end sequence state at 3.

**Cause.** The less-than tests had been introduced by a script of the port's
own, `local/diag/patch_fix_ge2.py`, written on the theory that a long post-boss
sequence let a timer run past the value the checks wanted. A later session
found the `cr6.lt` and blamed the code generator. A fresh regeneration emits
`!cr6.eq` at all four sites, which is correct; the tree-wide check for any
`bne` emitted as less-than finds zero.

**Fix.** The script is marked superseded and must not be run. The real cause
of the chapter hang was elsewhere (C2).

**Version.** Withdrawn in the v1.0.6 clean-up; never a changelog entry.

**R8. Two codegen warnings in the CRT region**

**Symptom.** Codegen prints `Unresolved b target 0x83A4A9E0 from 0x83A56758`
and the same for 0x83A4A9B4 on every run.

**Cause.** Pre-existing and unchanged across every regeneration; the targets
are in the C runtime region. Not chased.

**Fix.** None; listed under Still open (O12).

**Version.** Recorded in `HANDOFF.md`.

---

## Video and cinematics

**V1. Every pre-rendered video was garbled**

**Symptom.** Single-pixel horizontal striping across the frame, a central band
of magenta, green and black rows, and image structure drawn on alternate lines.
The title screen beside it rendered pixel-perfect. Listed as a known issue in
v0.1.0.

**Cause.** The recompiler defect R3: the codec's inverse-transform kernels read
their permute controls as zero, so the 8x8 transpose duplicated one row over
another and every block's DC was split between two vertical coefficients. It
took four wrong hypotheses to get there, each expensive and now closed: the
video files (re-encoded to a byte-matching profile with Microsoft's own encoder,
still wrong), an SDK or kernel decoder (the game imports no media APIs at all),
a missing opcode (zero unimplemented sites), and the GPU's DXVA path (during
playback the GPU binds only the video planes; the incriminating 1120x584
16-bit resolve was the scene's HDR buffer). Layout, pitch and tiling were
eliminated in one measurement with a synthetic ramp video whose every row is
one flat value: rows came back flat with wrong values, which only the vertical
transform can produce.

**Fix.** R3, verified against ground truth rather than by eye: a DC-only block
decodes flat, correlation with the source video went from 0.18 to 1.000, and
clipped pixels from 3 to 10 percent to none. This also fixed the Techniques
menu clips, which the earlier overlay never could reach. Along the way the port
had shipped two workarounds, both since removed: skipping the videos (V8) and
an overlay player that decoded them itself (V2).

**Version.** v0.1.0 (known), v0.5.0 (fixed). `docs/VIDEO_DECODE.md`.

**V2. The overlay video player and its own faults**

**Symptom.** Between v0.2.0 and v0.4.x the port re-encoded the game's 27
videos at install time into JPEG frames and drew them itself, because the guest
decoder was believed unfixable from here. The player had its own defects, and
the whole thing is now gone.

**Cause.** Four things cost time and were each found by measuring: the SDK's
image decoder was built PNG-only and returned an empty buffer for a valid JPEG
silently, so the overlay drew nothing for three builds; drawing over the guest's
own playback created a texture per frame while the guest decoded, which
desynchronised the GPU command stream, so the guest had to be denied the file
entirely; a full-screen ImGui window was not composited during guest rendering,
so the frames went through the foreground draw list; and the intro's three
panels had per-frame seams that a constant correction would have fitted to
noise. Later refinements: videos were being upscaled to a default height
(v0.2.1), an edge trim removed real content (v0.2.1), the intro's middle panel
read brighter because of a vignette in the source (v0.2.2, levelled with one
curve measured over the whole video), and a vertical misalignment at the first
join was searched for and not found. Known and accepted at the time: the
game's own audio and exact timing were lost for a replaced video, and only the
intro was prepared by default until v0.2.1 handled every video.

**Fix.** Superseded. The decoder was fixed at the source (V1), and the overlay
player, the conversion driver, the `video/` folder, the ffmpeg search and the
install message about washed-out colour were all removed (V11).

**Version.** v0.2.0, v0.2.1, v0.2.2 (built and refined); v0.5.0 and v1.0.0
(removed).

**V3. The attract demo, in the video era**

**Symptom.** Leaving the title screen alone started the attract demo and the
picture stopped updating while the game ran on; v0.1.0 listed it as breaking
the GPU command stream.

**Cause.** Two things were tangled here. The garbled decode (V1) made the
demo's playback itself suspect, and separately the GPU ring buffer was being
re-initialised wrongly at that transition (D12). In the video era the demo was
simply skipped with the videos (v0.1.4), then played through the overlay
(v0.2.1) with zero ring failures measured over a full 88-second cycle.

**Fix.** The decode is fixed (V1) and the ring re-initialisation is fixed
(D12); the demo plays natively. A second, rarer ring failure at the demo is
recorded under O2.

**Version.** v0.1.4, v0.2.1; resolved by v0.5.0 and v1.0.0.

**V4. The intro had stopped playing on a fresh install**

**Symptom.** A fresh install had no intro and said nothing about why.

**Cause.** With no prepared videos present, the port took the game's own video
away and put nothing in its place. The changelog claimed it fell back to the
game's playback; the code never did. It looked fine on the development machine
only because prepared videos were sitting beside the executable.

**Fix.** The guest only lost a video when a replacement was ready for it. Moot
since the overlay was removed.

**Version.** v0.3.0.

**V5. Loading a save raised "Disc Read Error"**

**Symptom.** Loading a real save produced `XamShowDirtyDiscErrorUI` 34 ms after
a failed open of a chapter-loading video.

**Cause.** Taking a video away from the game means failing its open, which had
been assumed safe because the intro and the attract demo shrug it off. A
chapter-loading video (`aurora*`) failing to open is treated as a bad disc and
the game stops.

**Fix.** Video replacement became an allow-list of the intro and the demos;
everything else kept the game's own playback. The rule survives the overlay's
removal: chapter-loading videos are never skipped by any setting, by design
(O7).

**Version.** v0.3.3.

**V6. Videos were looked for in the wrong place**

**Symptom.** A launch with `--game_data_root` found no prepared videos, fell
back to the guest's broken playback silently, and produced ring buffer errors.

**Cause.** The video folder was resolved from the settings path rather than the
path the runtime actually mounted.

**Fix.** It followed the mounted path. Moot since the overlay was removed.

**Version.** v0.3.3.

**V7. Videos were stretched on an ultrawide display**

**Symptom.** On a 21:9 or 32:9 display the overlay cropped 43 percent or more
of the picture's height, leaving a slot through the middle of the frame.

**Cause.** The overlay scaled every video to fill the window, which is right on
16:9 (an 8 percent crop, what a video player does) and wrong on anything
wider.

**Fix.** Filled only while the shapes were within 15 percent, fitted with bars
beyond that. Moot since the overlay was removed; the guest's own playback is
framed by the game.

**Version.** v0.3.7.

**V8. "Skip intro videos", and the switch that could not be turned back off**

**Symptom.** Skipping the videos began as the only defence against the garbled
decode (v0.1.4). Once the decode was fixed the setting stayed as a preference,
and then unticking it did not restore the videos (v1.0.0).

**Cause.** The skip works by pointing the game's file wrapper at a path that
cannot resolve, from the hook `ng2PatchSkipVideos` at 0x8380EB9C in
`config/hooks/patches.toml`; the game takes its own missing-file path.
Unticking it wrote video mode 1, the retired overlay mode, rather than 0, and
the compiled-in default of the matching cvar was 1 as well, disagreeing with
the setting's own default of 0.

**Fix.** The setting writes 0 when off. The DEFAULTS sweep of the census (X5)
exists because of this. The intermediate three-way Videos setting of v0.2.0
(Original, Replace, Skip) was removed in v0.3.0 when decoding became simply
what the port did, and reintroduced as a plain "Skip intro videos" in v0.3.4.

**Version.** v0.1.4, v0.3.0, v0.3.4, v1.0.0.

**V9. "Skip chapter cinematics" could never fire**

**Symptom.** The setting existed, every part of it was wired, and it did
nothing.

**Cause.** The synthetic pad armed on files named "stryd" or "ng2stry", names
that appear nowhere in the game data. This title's per-chapter story data is
`s_chap_NN.ng2`.

**Fix.** It arms on `s_chap_`, checked against the disc and against the boot
sequence, which opens none of them. The file-open hook `ng2DiagFileOpen` is
the source of that event (`src/diag_hooks.cpp`).

**Version.** v1.0.0.

**V10. The video mode setting could never select 0**

**Symptom.** The one video mode that matters, native playback, was
unreachable from the settings.

**Cause.** `ApplyLiveSettings` pushed the mode as "2 if 2 else 1", which cannot
produce 0. Making it honest exposed a run that black-screened at the intro, and
an earlier version of the changelog entry changed the default to 1 with a note
claiming mode 0 desynchronised the GPU command stream. That was wrong: the same
black screen appeared under mode 1, and its real trigger was texture dumping
and the texture pack running together (T8).

**Fix.** The value passes through, the default is 0 again, and the migration
written on the back of the bad diagnosis is gone.

**Version.** v0.5.3.

**V11. The video conversion machinery, removed**

**Symptom.** Installing a game re-encoded 27 videos and wrote about 340 MB of
derived files; releases shipped a 97 MB encoder; the install told every player
their videos would render with washed-out colour.

**Cause.** All of it existed to work around the decoder (V1), and the message
had stopped being true two versions before it was removed. Along the way the
install had once shipped without the encoder at all (X2) and once counted its
own summary lines as videos (S5).

**Fix.** The re-encoding step, the encoder, `prepare_videos.py`, the overlay
player, the conversion driver, the `video/` folder and the ffmpeg search are
gone. Releases went from 420 MB to 157 MB.

**Version.** v0.5.0 (pipeline), v1.0.0 (the rest of the machinery).

---

## Display, resolution and graphics

**D1. Internal resolution scaling was written off as impossible**

**Symptom.** `resolution_scale = 2` was accepted and the GPU still reported an
internal scale of 1x1.

**Cause.** Two things. That cvar is not the one the plugin reads; the plugin
reads `draw_resolution_scale_x` and `_y`. And those live in
`rexgpu-xenos.dll`, which registers its cvars after `OnPreSetup` and reads the
scale once at GPU init, so a value set in `OnPreSetup` failed as unregistered
and one set in `OnPostSetup` arrived too late.

**Fix.** The tuning is written to a TOML and loaded from `OnPreSetup` through
`cvar::LoadConfig`, the one path that defers values for cvars that do not exist
yet (`src/ng2_tuning.h`). The plugin reads back a real 2x2. This is also why
GPU-plugin cvars cannot be passed on the command line at all.

**Version.** v0.1.0.

**D2. Fullscreen was not honoured at launch**

**Symptom.** A saved fullscreen setting came back windowed every time.

**Cause.** The cvar is read when the window is created, during
`SetupPresentation`, which runs before `OnPreSetup`. Only a screen capture
measuring 1602x939 instead of the full screen gave it away.

**Fix.** Window settings go in from `OnConfigurePaths`, and the setup screen
also pushes them at the live window when Play is pressed.

**Version.** v0.1.0.

**D3. The settings window ran off the bottom of the screen**

**Symptom.** The settings screen "went full screen" and lost its buttons.

**Cause.** The window was 3840x2160, chosen while the game was on a 4K monitor,
and it reopened on the default 3840x1600 display: 560 pixels of it, including
the footer and Play, were below the desk. Nothing was driving the runtime's
`monitor` cvar, so the window went wherever Windows put it, and the resolution
list had no idea what was plugged in.

**Fix.** A monitor setting, shown only with more than one display, and the
window clamped to the work area of the monitor it will open on, with a red
warning before Play. Clamping costs no image quality; the internal render scale
is separate.

**Version.** v0.4.0.

**D4. 4K could not be set, on any monitor**

**Symptom.** Asking for 3840x2160 produced a 4818x1972 window that overflowed
and got centred, which looks exactly like a window refusing to move.

**Cause.** The runtime's window sizes are logical: Windows multiplies them by
the display's scaling, and the primary display here runs at 125 percent. The
v0.4.0 clamp made it worse by comparing a logical request against a physical
work area.

**Fix.** The menu means physical pixels and the conversion happens once, on
the way into the runtime, by the primary display's scaling, not the target's:
this process is system-DPI-aware, so Windows uses the primary's scaling wherever
the window goes. Measured with the target's own 150 percent it produced a
3218-pixel window on a 3840-pixel screen.

**Version.** v0.4.2.

**D5. The monitor list was in the wrong order, and named the wrong sizes**

**Symptom.** Picking monitor 2 opened the window on a different display; a
3840x2160 monitor was listed as 3840x2088.

**Cause.** The runtime puts the primary first and the rest in enumeration
order, one-based, with 0 meaning "wherever Windows likes"; neither Windows'
numbering nor screen position matches. The sizes shown were work areas, the
screen minus the taskbar.

**Fix.** Entries land where they say, each labelled with its real resolution
and scaling. Picking a monitor also sets the resolution to that screen, since
"play on monitor 2" and "at monitor 2's size" are the same wish, and the
"bigger than this screen" warning compares against the monitor rather than its
work area.

**Version.** v0.4.2, v0.4.3.

**D6. Ultrawide sizes, and what an ultrawide can and cannot show**

**Symptom.** No window size wider than 16:9 was offered.

**Cause.** The game is a 2008 console title with a 16:9 HUD and no wider field
of view to give.

**Fix.** 1440p, 21:9, 24:10 and 32:9 sizes were added, with "Keep aspect ratio"
defaulting on so the picture is pillarboxed rather than stretched. True
ultrawide rendering was measured and is not there (O1).

**Version.** v0.3.5.

**D7. The whole picture stretched on any window that was not 16:9**

**Symptom.** On an ultrawide, or any resolution wider than 16:9, the game
filled the window edge to edge, HUD and all, with "Keep aspect ratio" on and
doing nothing. It had always done this; every window until v1.0.0 had been
1280x720, and the first 3840x1600 window was a settings change made in-game,
not a code regression.

**Cause.** The port told the game the display was the window's size and shape.
The game renders 16:9 regardless, and the presenter only pillarboxes when the
game's idea of the display differs from the window's, so at 3840x1600 the game
was told a 2.4:1 display, the frame "matched", and the setting never got a say.
A second part is in the runtime: it treats a video mode equal to its default of
1280x720 as not configured and substitutes the window size, so a 1280x720
window on a 125 percent display was quietly told 1024x576.

**Fix.** `ApplyDisplaySettings` in `src/ng2_app.h` tells the game the largest
16:9 box that fits the window (3840x1600 becomes 2844x1600), and a new runtime
option `video_mode_explicit` makes the SDK take the value as set. Verified by
measurement: lit columns 498 to 3341 of 3840, bars at zero brightness. The
same fix was carried to the Fable II project, which shares the runtime.

**Version.** v1.0.1.

**D8. The graphics levers the plugin already had, and their limits**

**Symptom.** Supersampling stopped at 3x for no measured reason, the texture
cache was at console-era limits, and two quality flags the plugin has were
never offered.

**Cause.** The menu had been written from what a menu would usually offer
rather than from the plugin's own cvar table.

**Fix.** Read off the plugin's cvar dump: supersampling to 6x (v0.4.1) and then
8x (v0.5.4), where it was measured to raise shadow detail because the game
resolves its 512x512 shadow map through the scaled path; the texture cache up
to 4 GB and then 8 GB, verified by reading the plugin's limits back rather than
by the tuning file having been written; "Accurate depth" and "Fuzzy alpha
test" (v0.5.4), the latter being the runtime's answer to Xenia's NVIDIA
alpha-flicker label, off by default. Whether a larger cache reduces stutter for
this title is not established; the hitch counter (X8) exists to measure it. The
same dump settles what is not there: no upscaling filter beyond bilinear, no
SMAA, TAA or true MSAA (O6).

**Version.** v0.4.1, v0.5.4.

**D9. Two graphics settings could never reach the plugin**

**Symptom.** "Fuzzy alpha test" and "Accurate depth" had been in the settings
since v0.5.4 and, unless anisotropic filtering was also overridden, did nothing
at all. The tick-box moved, the file said on, nothing changed.

**Cause.** A guard in `ng2_tuning.h` reading `if (s.anisotropic >= 0)` was
written for one line and its closing brace ended up three settings too late.
Anisotropic defaults to -1, so on a default install the three cvars were never
written to the TOML the plugin reads.

**Fix.** The guard was given back its single line, and a DELIVERY sweep in the
census (X5) walks the file, tracks brace depth, and fails if any cvar is emitted
only under a condition that does not mention the setting driving it. The sweep
was verified by reintroducing this exact bug.

**Version.** v1.0.0.

**D10. V-Sync off makes the game run fast**

**Symptom.** Turning V-Sync off does not just tear.

**Cause.** The runtime raises the guest's vblank from 60 Hz to 1000 Hz when it
is off, and this title advances its game logic on vblank. This is Xenia's
`vsync-off-speedup` label for the title.

**Fix.** V-Sync defaults on, and the row states the consequence inline when
switched off, the same treatment the Frame rate row already gave values above
60.

**Version.** v1.0.0.

**D11. Antialiasing, and the output filter that could only be bilinear**

**Symptom.** No antialiasing setting; an output-filter list that might have
offered effects this build lacks.

**Cause.** The presenter declares exactly one `present_effect`, bilinear, and
the two CAS/FSR sharpness cvars are not registered at all in this runtime
build.

**Fix.** Antialiasing was added from the plugin's `swap_post_effect` values
(FXAA Extreme measurably cuts edge energy by 12 percent on the title screen).
The output-filter list is read from the cvar's own declared values, so it
cannot offer an effect the build does not implement, and it says so when there
is one option. FSR and CAS remain unavailable (O6).

**Version.** v0.1.0, v0.1.1.

**D12. The attract demo froze the game: the GPU ring re-initialisation race**

**Symptom.** Idle at the title screen, the demo started, the log showed
`ExecutePacketType0 overflow` on the primary ring, and the guest froze while
the presenter carried on at a steady frame rate: a black screen at 30 fps.

**Cause.** In the SDK's command processor, `InitializeRingBuffer` reset the
ring's read pointer and left the write pointer alone. This title
re-initialises the ring at every mode change, twice within a millisecond when
the demo starts, so the parser compared a fresh read pointer against a write
pointer from the ring's previous life, read that as a wrap, and walked an
entire 32 KB ring of dead commands. The "impossible packet" in the log was the
victim, never the cause. A 40-deep packet trace proving the parser
byte-aligned is what moved the question from "how is it misreading" to "how
much was it told to read".

**Fix.** Both pointers are reset, matching what the hardware does when the
ring base is programmed, and a ring epoch counter stops a re-initialisation
that lands mid-pass from writing the old read pointer back. Measured: 0
failures across 107 re-initialisations and 52 attract cycles. One caveat the
changelog records at length: the fix was not in the running game for four
sessions because the app build re-copied a stale plugin over the fresh one
(X13), and once it was really deployed a long run did 26 clean cycles. A second
way for the parser to read a non-packet was seen three times before that
deploy and has not been observed since; it is kept open as O2.

**Version.** v1.0.0.

---

## Audio

**A1. Audio could stop for the rest of the run, silently**

**Symptom.** Sound died mid-session and never came back: no error, the audio
session still active, its peak flat at zero, immune to alt-tabbing. Worst in
chapter 12, which on the test machine at the time ran at 28 fps (the graphics
card was saturated by other work, O3) and so underran far more often than
anywhere else.

**Cause.** The SDL audio driver's semaphore is a credit meaning "the guest may
submit one more frame", and it was returned only when a real frame had been
consumed. The first time the guest was late the queue emptied, the callback
played silence and returned nothing, and the count reached zero with nothing
queued. The audio worker waits on exactly that semaphore to dispatch the
guest's callback, so the guest was never asked for audio again.

**Fix.** The credit is returned on an underrun too, which is precisely when
another frame is most wanted. Measured after the fix: 1,681 audible samples
across a full chapter 12 run in the conditions that killed it before. This is
a runtime change and is one of the two audio deaths; the other is A2.

**Version.** v1.0.0.

**A2. The music died mid-session and never came back**

**Symptom.** Twenty to forty minutes into a session, often at the title screen
as the attract demo handed back to the menu, all sound stopped while the game
played on. Open since the first playable build. The first live capture showed
the guest's audio callback blocked in `KeWaitForMultipleObjects` for over half
an hour with the watchdog saying nothing (X4); one stuck callback ends all
audio because the runtime's audio loop calls it synchronously.

**Cause.** The game's own rendezvous barrier, `sub_8374F078`. Each audio
worker marks its arrival in one byte of an 8-byte word and spins until the word
matches the expected set; whoever sees the match clears the word so the others
can leave. Two words alternate per round so that a slow reader is not confused
by the next round, but a job with an even number of rounds ends on the same
word the next job starts on. One worker completed the round, cleared the word,
was handed the next job and arrived on the same word again before the other
worker had re-read it; that worker then saw a word that was neither the
expected set nor zero and spun forever. On the console the re-read happens
within nanoseconds and always wins. On a PC the spinning thread can be off the
processor for milliseconds at exactly the wrong moment. Read live from a
silent game with nothing attached: word {4}, expected {4, 5}, both workers
spinning with the correct hardware-thread numbers.

**Fix.** The hook `ng2AudioBarrierFix` at 0x8374F164 in
`config/hooks/patches.toml`, body in `src/ng2_audio_fix.cpp`: a worker that
finds its own arrival byte gone from the word takes the exit its loop missed,
which is exactly what it would have done had it read the zero. Each rescue is
logged, so the race can still be seen happening. Cvar `ng2_audio_barrier_fix`,
default on. Found by decoding the spin loop statically to get the word and mask
addresses, then polling them every two seconds from outside the process; it
reproduced in three minutes at the title screen.

**Version.** v1.0.2.

---

## Chapter transitions and crashes

**C1. The community "Chapter 12 crash workaround", from a switch to a guard**

**Symptom.** The community patch for this title forces an early return at
0x82834C78 and its author warns it causes problems in every other chapter. It
began here as an off-by-default setting (v0.1.3), moved into a "Workarounds"
group (v0.1.5), and could not be automated because the game gives no signal
which chapter is loading (v0.3.0, five approaches tried).

**Cause.** Reading further into that function showed why it does not need to
know the chapter: `r30` is a pointer the function dereferences a few
instructions later, so the crash is a bad-pointer dereference. The community
patch makes the function give up even when nothing was wrong.

**Fix.** The hook `ng2PatchChapter12` (patches.toml, after the load at
0x82834C78) tests the two fields the function reads against mapped, readable
guest memory and takes the game's own early return only when they cannot be
read (v0.3.1). v0.3.2 established that the site runs at all: over 500 calls
before chapter 1 is playable, all with the same pointer, so "fires 0 times in
normal play" was a real measurement. v1.0.0 scoped the forced return to chapter
12 using the file the game opens for a chapter, `s_chap_NN.ng2`, taking the
first file of a burst rather than the last. That scoping was the mistake C2
records: inside chapter 12 it forced the return unconditionally, and at the
boss's death the site is called with a real object. v1.0.3 made the guard test
the pointer in chapter 12 exactly as everywhere else. The always-on cvar
`ng2_chapter12_workaround` remains for anyone who wants the original behaviour.

**Version.** v0.1.3, v0.1.5, v0.3.0, v0.3.1, v0.3.2, v1.0.0, v1.0.3.

**C2. The red mist after a boss: the next chapter never loaded**

**Symptom.** Finishing a chapter ended in a red mist that never went away. The
results, rating and save screens worked, the game asked whether to continue
without saving, and Proceed did nothing. First met at chapter 12, then shown
to be every chapter (a chapter 1 run reproduced it in 13 minutes); the player
had only met it at 12 because they had been loading chapter 12 saves rather
than playing the earlier transitions. A working manual detour existed all
along: save at the prompt, quit, load the save from the menu, and the next
chapter loads clean.

**Cause.** The post-boss flow is a state machine in the game's front end
(`sub_8242BCF8`). After Proceed it checks whether anything unlocked during the
chapter is still missing from the save data, and if so it waits for the
profile's achievement write to report completion, a field at offset 88 of the
profile block at 0x8555B930 reaching 2. On the console that write completes at
once. In this port it was never issued at the chapter end (the runtime issues
it at the next chapter load), so the field stayed 0 and the machine waited for
ever. Proved with the game parked at the mist: writing 2 into that field by
hand made the game run its own transition, with the loading screen, its
background and the mist clearing. Xenia, running the same game code, was used
as ground truth for the transition's write sequence.

Everything below was believed before that and is withdrawn or superseded:

- v1.0.0's "known issue, not ours to fix cheaply": a busy-wait on a flag at
  0x84C39440 that its consumer never clears, attributed to Xenia's
  `kernel-save-file-errors`. The flag is a two-flag handshake between the
  main side and the level worker and stays set for as long as a mode runs; it
  is not the hang, and it fired a false stall alarm at a healthy title screen.
- "The watchdog reports no stuck kernel waits" as evidence: it watched one
  wait path in five (X4).
- `db16cyc` (R5): the starvation it needs cannot occur.
- v1.0.3's "fixed": the guard scoping (C1) was necessary and not sufficient,
  as v1.0.4 said.
- The end-of-chapter request-byte machine (`sub_8246A500`, sub-states 1 to 5):
  forcing sub-state 5 fires `sub_82441A80`, which for this state is a game-over
  path, at any timing. Poking the sequencer request word returns to the title.
  Both were dead ends.
- The mode word 0x84C25070 "stuck at 3": true, but a consequence, not a cause.
- The four `bne` sites (R7).
- External remedies that wrote the loader's target field and the unfreeze word
  from outside, with or without a press-driven detector: they loaded a
  playable chapter but skipped the loading screen and left a faint mist over
  the next chapter, because they bypassed the wait rather than satisfying it.
  A press-driven version also fired from the in-game pause menu, which wears
  the same mist. All of that code is gone from the build.
- The "codegen mistranslated `bne`" note (R7).

**Fix.** The hook `ng2ChapterAwardFix` at 0x8242D7B4 in
`config/hooks/patches.toml`, body in `src/ng2_chapter_fix.cpp`: right after the
field is loaded, when it is still below 2, report 2. The game then runs its own
transition unchanged. The pending achievements are still awarded by the game at
the next chapter load, as before (O9). Cvar `ng2_chapter_award_fix`, default
on. Verified on the no-save path and on the Save path, on the clean v1.0.6
binary, on more than one stage.

**Version.** v1.0.0 (known), v1.0.3 (partial), v1.0.4 (still open), v1.0.6
(fixed).

**C3. Crash at the chapter 13 boss**

**Symptom.** A null-pointer write reaching the chapter 13 boss.

**Cause.** A recompiler defect in how split fragments carry registers, R6.

**Fix.** R6, `local/diag/patch_missed_regs.py`.

**Version.** v1.0.4.

**C4. The transition command-list scanners ran off a null pointer**

**Symptom.** Forcing the chapter 12 handover from the mist produced "game over"
and a crash to desktop, reading guest address 0x51000 on the chapter-load
fiber. The fault was first taken for a decompressor reading a corrupt save.

**Cause.** Named through the live function table: `sub_82442AD8` walks a
32-slot transition-effect table and hands each slot's command-list pointers to
scanner functions that read the list's first word with no null check and walk
upward looking for a terminator. By the time the handover was forced, the
effect lists had been freed and the pointers were null, so the scanners ran off
the committed low region into unmapped memory. On hardware an empty list read
from low memory is a no-op (B3).

**Fix.** `local/diag/patch_scanguard.py` guards the four scanners reachable
from that walk (`sub_82467E70`, `sub_82467FF8`, `sub_82467D68`,
`sub_82484658`) at their entry: a list pointer below 0x00100000 is an empty
list. With the guards the forced handover no longer crashed. It was found while
forcing a path the game does not take on its own, so whether normal play ever
reaches it is not established; it is kept as a genuine null-dereference guard.
Like R6 it is a hand edit to generated code and must be re-applied after
regeneration.

**Version.** In the build since 2026-09-07 (shipped from v1.0.5); no changelog
entry of its own.

---

## Texture pack and AI upscaling

**T1. There was no texture replacement, and the first pack held photographs of the screen**

**Symptom.** The GPU plugin had no texture-mod facility at all (v0.1.1, "not
added, and why"). Once dumping existed, a capture taken at the menu produced a
"pack" of five files, two of them the framebuffer upscaled to 5120x2880.

**Cause.** The plugin converts textures on the GPU, so host-format pixels never
exist CPU-side and reading them back would cost a fence and a stall per
texture. And the resident-memory load path also carries the video decoder's
planes, the scene resolve and the HDR buffer, none of which is art.

**Fix.** The plugin dumps the raw guest bytes plus the texture key, one memcpy
the first time a texture is seen, and the untiling is reproduced offline in
`tools/upscale_textures.py`, ported verbatim from the SDK. A format filter at
the dump site and a power-of-two or compression check in the tool reject
non-art, with a count per reason; fonts, HUD atlases and ramps are excluded
because a model invents detail in glyph edges. One stride bug is recorded for
the next person: the key's pitch is in units of 32 texels and the tiler wants
blocks. The replacement path followed (v0.5.4): the resource is sized from the
replacement rather than the guest key, the load skips the conversion shader,
the swizzle is left alone, and upload buffers are retired by submission index
because freeing one while its copy is queued corrupts the texture in a way that
reads exactly like a decoder bug. The pack format is raw RGBA behind a 16-byte
header, because PNG decoding cost 16 ms per texture on the render thread, and
the default scale is 2x, because a 4x pack of 1,290 textures came to 5.9 GB
against a 4 GB cache ceiling and produced 2,900 uploads and two-second hitches.

**Version.** v0.1.1 (not available), v0.5.3, v0.5.4. `docs/TEXTURE_PACK.md`.

**T2. F9 switches the pack live, and once overwrote the saved setting**

**Symptom.** Comparing a pack through a settings screen barely works, since the
eye loses the detail while the overlay closes. And F9, once added, quietly
wrote its state to disk, so ending a session mid-comparison left the next
launch with the pack off while the menu still said on.

**Cause.** F9 called `Save()`.

**Fix.** F9 toggles live and changes nothing on disk; the checkbox is what
persists, and a green panel names which set is on screen. The plugin requests a
full texture-cache clear when the path changes, performed at end of frame after
the GPU drains, so the picture switches within a frame or two; since v1.0.6
the same key reloads the pack from disk. The "Advanced (all cvars)" button
warns that what it writes is applied before these settings, so a value there
silently wins.

**Version.** v0.5.4.

**T3. AI upscaling as an optional download, and what it took to make its output trustworthy**

**Symptom.** Real-ESRGAN was wanted; the usual Python route needs a torch build
matched to the card's CUDA version and would have broken another project's
pinned packages on this machine.

**Cause.** Three separate findings. The photo-trained model denoises hard: on a
smoke texture it cut the mean brightness by 40 percent, because faint wisps
against black are what it treats as noise. Running 30 textures produced a
Vulkan device-lost error partway through, after which the tool carried on,
wrote a file for every remaining texture and exited successfully with 16 of the
30 blank. And the newest release tag of the tool carries no files, so
`/releases/latest` reports failure while the download plainly exists.

**Fix.** The standalone Vulkan build is downloaded on request, and the AI
option stays disabled until it succeeds. Detail is transferred, not
cross-faded: the model's output is high-pass filtered and laid over a plain
resize, so tone and colour stay the game's, with a strength slider. Work is
done in chunks with retries, every result is verified for size and for being
not blank, and anything that still fails falls back to a plain resize.

**Version.** v0.5.4.

**T4. The AI upscaler produced garbled textures at 2x**

**Symptom.** Every AI-processed texture was shifted, repeated and mostly black;
a starburst effect's brightest pixel measured 26 of 255.

**Cause.** Real-ESRGAN x4plus is a 4x network and nothing else. Asked for 2x
directly, the executable ran the 4x network and assembled the tiles into a 2x
canvas. The blend step then laid that picture's edges over a plain resize, so
every texture carried ghost edges of itself in the wrong place. A sharpness
metric alone would not have caught it; a side-by-side did.

**Fix.** The model runs at its own 4x and the result is resized down
(`NATIVE_SCALE` in `tools/ai_upscale.py`). Any pack built with the AI option
before v1.0.1 has the fault in every AI-processed texture and must be rebuilt.

**Version.** v1.0.1.

**T5. Cancel did not cancel the texture processing**

**Symptom.** Pressing Cancel set a flag nothing read, and the run carried on to
the end; closing the game stopped it only some seconds later when the pipe it
wrote to went away.

**Cause.** The tool is a tree of processes, the Python launcher, the
interpreter and the upscaler, and stopping the first would have left the other
two running.

**Fix.** The tree lives in a Windows job object (`RunHidden` in
`src/ng2_textool.cpp`); Cancel terminates the job within a moment and so does
closing the game. Every texture is written whole, so whatever reached the pack
before the stop is usable.

**Version.** v1.0.1.

**T6. The progress bar reached 100 percent and started again from nothing**

**Symptom.** A bar at 100 percent followed by a bar at 0 percent estimating 228
minutes, and a line at the top of every AI run saying Real-ESRGAN was not
installed.

**Cause.** Processing has two steps, decode then upscale, reported on one bar,
and the estimate divided by the time since the button was pressed, which
included the whole first step. The message referred to a Python package the AI
path does not use.

**Fix.** "Step 1 of 2" and "Step 2 of 2", each timed by itself; the second
step of the run that showed 228 minutes took about 26. The message is gone.

**Version.** v1.0.1.

**T7. The counts said 1,871 textures were missing, and every run redid the whole pack**

**Symptom.** "7,778 dumped, 5,907 in the pack" read as 1,871 textures missing
when nothing was; and "Process textures" redid every texture, half an hour with
the AI, to pick up the few dumped since.

**Cause.** The tool never packs the HUD, fonts and other non-art it dumps, by
design, and the menu was counting files rather than textures. The run had no
notion of what was already done.

**Fix.** The Textures section classifies the dump the way the tool does and
reports what can be enhanced, what is in the pack, and what is waiting. The
button processes only what is missing and says how many; a box brings back the
full rebuild. The pack records what it was made with in `pack/pack.txt`, and a
different scale, upscaler or strength, or a run stopped halfway, redoes every
texture and says so before the button is pressed.

**Version.** v1.0.2.

**T8. Dumping and the pack together starved the GPU thread**

**Symptom.** A black screen at the intro that was first blamed on the native
video mode (V10).

**Cause.** Together they put a file write and a stat on the GPU thread for
every texture the decoder creates, and the ring buffer overflows while it
waits. Either alone is fine.

**Fix.** The two cannot both be selected, enforced in the menu and again when a
settings file is loaded. The dump procedure is therefore: pack off, dump on,
play, dump off, pack on, process.

**Version.** v0.5.3 (diagnosed), v1.0.0 (enforced).

**T9. "Enhance with AI" implied its alternative was no enhancement**

**Symptom.** A tick-box whose unticked state was quietly a different, perfectly
good upscaler.

**Cause.** The choice was presented as a boolean.

**Fix.** A list: Lanczos, or Real-ESRGAN once downloaded. A setting carried from
a machine that had the model cannot claim it on one that does not.

**Version.** v1.0.0.

**T10. The settings menu scanned two folders every frame**

**Symptom.** With the Textures section open the frame stalled.

**Cause.** The dumped and packed counts walked their directories on the UI
thread on every frame, about 14,800 entries once the pack had grown.

**Fix.** A detached thread, refreshed at most every five seconds. The counts are
advisory.

**Version.** v1.0.0.

**T11. A pack larger than the texture cache evicted what it was about to need**

**Symptom.** A 6 GB pack in a 4 GB cache: 1,290 textures produced 2,900
uploads, and loading it all up front made it worse.

**Cause.** One stage's worth fits; the whole pack does not.

**Fix.** Which textures a chapter uses is recorded as you play, against the
chapter the game says is loading, in `pack/stages/chNN.txt`, keyed by the pack
file names. When a chapter loads, that stage's files are read and discarded off
the render thread so the OS page cache is warm. The list is written every 64
new textures rather than only at the chapter change, because a session ending
in the stage being recorded used to throw everything away. A stage with no list
behaves exactly as before. The feature was inert for its first day because the
plugin carrying it was never deployed (X13).

**Version.** v1.0.0.

**T12. The texture upscale died when the settings menu was closed**

**Symptom.** Starting a pack build and then closing the settings menu, which
tabbing out of it does, cancelled the run.

**Cause.** The run was owned by the settings overlay, and its destructor
cancelled it.

**Fix.** It belongs to the application for the life of the process; reopening
the menu shows its progress again, and only Cancel or quitting stops it.

**Version.** v1.0.5.

**T13. The enhanced-texture status showed a false count**

**Symptom.** "N enhanced and M original textures loaded right now" ticked
constantly and read as low as 57, as if only 57 of the pack's thousands had
ever been made.

**Cause.** It counted only the textures resident for the current screen.

**Fix.** The per-frame count is gone; the status states whether the pack is on
and whether it is replacing textures on screen, and the stable totals stay on
the line above.

**Version.** v1.0.5.

**T14. The pack served the wrong texture: shop windows rendered violet**

**Symptom.** The glass of the shop windows at the start of chapter 1 rendered
with a violet tint and bumps: a normal map drawn as colour. F9 made it cream
again.

**Cause.** The plugin filed each texture under an id built from its memory
address, format, size and pitch, nothing about its pixels. The game streams its
chapters through the same memory, so the window shared an id with a normal map
from a later chapter that had been dumped first. Any two textures that ever
land at the same address with the same shape could swap this way.

**Fix.** Pack files carry a content hash of the texture's own bytes in their
name, the plugin hashes what is in memory before it opens anything, and a
mismatch falls back to the game's own art with one log line naming the id. The
dump keeps both textures of a shared address as two files. An existing pack
migrates in place the next time the tool runs, a rename rather than a
re-upscale (46 GB in 14 seconds); files whose raw dump was cleared cannot be
verified and are left out until their scene is dumped again, and the game
reports how many at start-up. The plugin change is shared with the Fable II
project.

**Version.** v1.0.6. `docs/TEXTURE_PACK.md`, "Ids carry a content hash".

**T15. "Dump while playing" did nothing until the next launch**

**Symptom.** Ticking the dump switch mid-game, walking a chapter and coming
back found nothing written and "0 waiting".

**Cause.** The switch travelled to the plugin only through the tuning file at
start-up.

**Fix.** The plugin's dump settings are hot-reloadable and the settings screen
applies them live, in the same breath as switching the pack off, so the scene
in front of the player is written out immediately.

**Version.** v1.0.6.

**T16. The census could not see a second texture at the same address**

**Symptom.** "Process N waiting textures" stayed at zero for exactly the
collision case the new dump exists to capture.

**Cause.** It counted by id, so a texture sharing an id with one already in the
pack read as packed.

**Fix.** The census counts id plus hash, the key the game loads by, and files
the game would ignore count for nothing.

**Version.** v1.0.6.

**T17. A texture run that stopped halfway redid every texture**

**Symptom.** A 4x AI run died 2,449 textures into the 9,418 it had left
(the disk was full: "No space left on device" in the log). The settings
menu then said the next run would redo every texture: all 22,026, rewriting
the 104 GB already made.

**Cause.** The pack's record (`pack.txt`) is written with `complete=0` when a
run starts and `complete=1` when it ends. The tool's only-missing check
required `complete=1` as well as matching settings, so an interrupted run
was treated like a pack made with other settings, and the flag that leaves
existing textures alone was switched off. The textures it had written were
all whole and made with the same settings; only the one file the failure
cut short (a header with no pixels behind it) was bad.

**Fix.** `tools/upscale_textures.py`: the completion flag no longer decides.
When scale, upscaler and strength match, the run continues with what is
missing and says so. `intact_pack_ids()` defines "already in the pack" as a
file whose size is exactly 16 + width*height*4 from its own header; a short
file is redone. Two seconds for 15,255 files. The menu text in
`src/ng2_menu.cpp` now says the next run continues with the missing textures.
Verified on a throwaway pack folder: same settings with `complete=0`
continue; a changed scale still redoes everything.

v1.0.8 was not enough: the settings menu had the same rule of its own
(`must_redo` in `src/ng2_menu.cpp` included "pack incomplete"), so it
greyed the redo box ticked, labelled the button "Process all 22,026
textures" and launched the tool without the only-missing flag. The first run
after v1.0.8 was still a full redo; it was caught two minutes in and
stopped, and the pack's record was set complete by hand, since every file in
it was whole. v1.0.9 makes the menu force a redo only when the pack was made
with other settings.

**Version.** v1.0.8, v1.0.9.

---

## Settings, installer and launcher

**S1. The setup screen and the disc install, and the setup screen that drew once and froze**

**Symptom.** The runtime cannot mount a disc image as the game data root and
rejects a file with "does not exist", a misleading message for a path that
plainly exists. And the first setup screen was a perfect screenshot, completely
dead to the mouse.

**Cause.** `ConstructRuntime` requires a directory. Nothing drives the
presenter before the guest exists, so the pre-boot UI painted once.

**Fix.** A setup screen before the guest boots, from `OnFinalizePaths`, the one
hook where the window and the drawer are live but the runtime is not built. It
extracts an ISO through the SDK's disc device (65 files, 6.7 GB, about 20
seconds; files already present at the right size are skipped so a cancelled
install resumes), reads the title out of the disc's own XEX so it can say when
the wrong game was picked, and is repainted by an explicit pump. It opens on
first run, on Shift at launch, or when the configured folder is missing;
`--game_data_root` suppresses it for scripted runs.

**Version.** v0.1.0.

**S2. The settings menu on one screen, and the display choices as first offered**

**Symptom.** Four tabs, with rows pushed below the fold on a 720p window.

**Cause.** Explanations lived inline.

**Fix.** One page, explanations in hover markers except the two genuine
surprises (V-Sync and refresh rate) which stay on the page; Antialiasing (D11);
Resolution as the three sizes worth having, with any other size shown as
"Custom" rather than snapped to 720p; Frame rate 30 to 144; the disc-image
destination row only once an image is chosen. Settings the presenter re-reads
apply immediately; startup-latched ones were shown greyed and marked
"(restart)" until v1.0.0 changed that (S18).

**Version.** v0.1.1.

**S3. Quality presets, pointer hiding, a Workarounds group and tighter rows**

**Symptom.** Comfort settings re:Blue already had were missing, and the list
ran two rows past the bottom of a 720p window.

**Cause.** The menu was younger than re:Blue's.

**Fix.** A quality preset row moving supersampling, antialiasing and filtering
together, with "Custom" as a state the row can display but not select;
keyboard and mouse control (I1); hide the pointer after N seconds (which did
not actually work until v1.0.5, S20). Workarounds are their own group apart
from the quality settings and say so. Rows are drawn tighter than the rest of
the UI. What was not brought over is recorded too: re:Blue's renderer rows,
controller icon sets, per-save scoping and update channel are specific to that
project.

**Version.** v0.1.5.

**S4. Installing a disc image leaves a game ready to play**

**Symptom.** Installing left console windows flashing, a second progress bar,
and, in the video era, a conversion step to run by hand.

**Cause.** The conversion ran through `_popen`, as did three `where` probes
before the setup screen had drawn; the encoder was not on PATH on a normal
Windows machine.

**Fix.** One progress bar for the whole install, `CreateProcess` with
`CREATE_NO_WINDOW` everywhere, the encoder looked for beside the game with a
message saying where to put it when missing, and the videos prepared as part
of the install and kept with the game data. The Videos setting and the chapter
12 row left the menu in the same version. The video parts of this are all gone
since v0.5.0 (V11).

**Version.** v0.3.0.

**S5. The install finished at 106 percent**

**Symptom.** Exactly 106 percent.

**Cause.** The encoder's output was parsed to count videos, and three lines it
prints flush left are not videos, so a 27-video job counted 30 done; with the
bar weighted 45 percent copying and 55 percent encoding that is 1.061.

**Fix.** A line counts only when its first word names a video, and the
displayed fraction is clamped and reads 100 percent the moment encoding
completes. Moot since the encoding step was removed, but a progress bar that
can show 106 percent is one nobody trusts again, so the clamp stays.

**Version.** v0.3.7.

**S6. Rescan did not see freshly installed files**

**Symptom.** After an install the game folder still pointed wherever it had
pointed before, and Rescan re-inspected the old path.

**Cause.** Adopting the install destination as the game folder sat inside the
branch that chained into video preparation, so an install with nothing to
prepare never adopted it.

**Fix.** Adopting the destination happens whenever an install completes.

**Version.** v0.3.7.

**S7. The installer forgot which disc image it used**

**Symptom.** An empty box beside a 7 GB image that had not moved.

**Cause.** The path was session-only.

**Fix.** Remembered, and checked before being shown. Nothing in this port has
ever deleted a disc image; the installer opens one read-only.

**Version.** v0.4.3.

**S8. Every release was a fresh folder, so the old one kept being played**

**Symptom.** Three bug reports in one afternoon were a single cause: an old
executable, still being run because moving to a new release meant reinstalling
the disc.

**Cause.** The release layout put the install and the executable in the same
folder with no way to update one without the other.

**Fix.** `python tools/make_release.py --update <folder>` writes a build over
an existing install and touches nothing the player put there; it refuses a
folder that does not already look like an install. The port's own version is
shown in three places so an old executable can be recognised (S15).

**Version.** v0.4.0.

**S9. Three settings were read but never written**

**Symptom.** `video_mode`, `keyboard_control` and `cursor_hide_seconds`
changed, then vanished at the next save.

**Cause.** Parsed on load, left out of `Save()`.

**Fix.** Written. A setting that forgets itself is worse than no setting.

**Version.** v0.3.5.

**S10. No way to confirm a save, and no way back to the setup screen**

**Symptom.** Getting back to the setup screen meant knowing to hold Shift at
launch.

**Cause.** Nothing offered it.

**Fix.** A Save settings button on both screens and a "Show the setup screen at
the next launch" tick in the overlay.

**Version.** v0.3.5.

**S11. A footer tick pushed Save off the window**

**Symptom.** One more row in the fixed footer put the Advanced and Save buttons
below the bottom edge of a 620-pixel window.

**Cause.** The row was added to the footer rather than the scrolling body.

**Fix.** Moved into the body. Caught by photographing the window rather than by
reading the code.

**Version.** v0.3.6.

**S12. Cheats: one that works, a search that crashed, and two that were removed**

**Symptom.** Nothing publishes the game's memory layout and a static
recompilation carries no symbols, so no cheat had an address. The first cut put
a memory search in front of the player, and the search crashed the game on its
first run reading guest 0x40430000.

**Cause.** The search asked `QueryProtect`, which answers what protection pages
would have, so a reserved page answered "readable" while touching it faulted.
Health and Ninpo turned out to be live combat values with no anchor: the
game's progress block does not hold a single float.

**Fix.** Infinite karma finds the progress block by its own header signature
(a size, a version and a magic) and writes the balance at 0x230, found by
intersecting eighteen saves whose scores are written in their names. The cached
address is re-checked against the signature every tick. The search walks
regions rather than pages with `QueryRegionInfo`, reads through
`ReadProcessMemory` and never dereferences, and remembers that the guest is
big-endian. The search was folded away from the player (v0.3.5) and then
invincibility and infinite Ninpo were removed rather than listed greyed
(v0.3.6); the settings file no longer carries addresses nothing reads.

**Version.** v0.3.4, v0.3.5, v0.3.6.

**S13. Importing a console save**

**Symptom.** A real Xbox 360 save package could not be used; once it could, its
name came out as mojibake; and a nineteen-file save pack meant nineteen trips
through a file dialog.

**Cause.** The import cannot happen on the setup screen, which runs before the
runtime exists. The documented metadata layout puts the display name at 0x3ED;
these files carry it at 0x411, big-endian, in one of nine locale slots, and the
odd offset read as aligned words produces convincing little-endian text. A
save is a directory holding the inner file plus a 328-byte header, and without
the header it is invisible no matter where its bytes are. One system save
header on the development machine held a stray shell redirect of the port
author's own and had to be put back.

**Fix.** The file is only read on the setup screen and the import is queued
into `OnPostSetup` (`ng2_saveimport.*`): extracted with the SDK's content
manager, which always lands in the DLC slot, then moved under the profile's
XUID with the content header that makes XAM enumerate it. The first non-empty
locale slot wins. The picker takes a folder and every save in it, subfolders
included, skipping non-saves silently, with a Rescan beside it. Verified: 19 of
19 imported in a tenth of a second, Japanese names included. This is what
answers Xenia's `kernel-save-file-errors` label for the title.

**Version.** v0.3.3, v0.3.4, v0.3.8.

**S14. The profile, saves and DLC live with the game**

**Symptom.** They lived under Documents, so a copy of the install was not a
copy of the saves.

**Cause.** The runtime's default.

**Fix.** `user\` beside the executable, with whatever was at the old location
moved in on the first run rather than orphaned. The old path is read from what
the runtime had filled in, not hard-coded. One thing that cannot be done: the
gamertag, since the SDK's profile has no setter for its name (O13).

**Version.** v0.3.4.

**S15. There was no way to tell one build from another**

**Symptom.** The title bar showed the SDK's version, which never changes.

**Cause.** The port had no version of its own on screen.

**Fix.** The window title, the setup screen and the overlay's title bar all show
the port's version, read from the `VERSION` file at configure time and
registered as a configure dependency so bumping it cannot bake the previous
number into the next build.

**Version.** v0.4.5.

**S16. An application icon**

**Symptom.** None.

**Cause.** Windows takes an icon from three places and none implies the others.

**Fix.** `tools/make_icon.py` draws a katana over the kanji for ninja and
writes `resources/ng2.ico`; the `.rc` is generated from `VERSION`. Only the
executable's resource turned out to be needed, because SDL's window class falls
back to the process's own resources. The small sizes are drawn simplified
rather than downsampled.

**Version.** v0.5.3.

**S17. The memory settings are chosen from the hardware on first run**

**Symptom.** A texture cache size and upscale factor that suited no card in
particular.

**Cause.** DXGI's memory budget is an allowance, not a measurement: on a 32 GB
card with 24 GB held by other work it reported the full budget with nothing in
use.

**Fix.** First launch sizes them from the card's capacity, once, with a "Detect
from hardware" button to re-run; a value changed by hand afterwards is never
overwritten.

**Version.** v0.5.4.

**S18. Greyed-out settings read as broken, and the texture folder had to be typed**

**Symptom.** The display settings were greyed out once a game was running, and
the texture folder was the one folder without a picker.

**Cause.** The window and the guest video mode are built at start-up; greying
was the honest state but gave no way to act.

**Fix.** Every setting is editable everywhere; a change to a startup-latched
one saves and applies at the next launch, with a red "restart required" beside
it. Verified before editing: the live-apply path only ever touched a curated
safe subset, and none of these are in it, so nothing can be mis-applied. A
"Browse..." button beside the texture folder.

**Version.** v1.0.0.

**S19. Opening the settings could kill the process**

**Symptom.** F10 while a video played crashed with no log entry; the Windows
event log named an access violation in `ImGui::TableNextRow`.

**Cause.** `RowStart()` calls `TableNextRow()`, which does not check for a
current table, and a control on the Textures page sat 113 lines after the
`EndTable()` it needed. The video only made the section slower to reach.

**Fix.** The control moved inside its table and the whole file was swept for
the same shape. Reading the event log first, rather than assuming a GPU
timeout, is what found it.

**Version.** v1.0.0.

**S20. The settings pointer never hid**

**Symptom.** "Hide the pointer after" set an idle delay and the cursor stayed on
screen forever.

**Cause.** The window hides the pointer only in its auto-hide mode; the code
set the delay and left the mode at "always visible".

**Fix.** Both places that apply the setting switch the window into auto-hide
when a delay is chosen and back when set to never.

**Version.** v1.0.5.

---

## Input

**I1. Only a real controller worked**

**Symptom.** A PC port that could not be played from the keyboard.

**Cause.** The runtime ships the keyboard-and-mouse driver and leaves it off.

**Fix.** A "Keyboard and mouse control" setting, verified with the setting
alone: Enter opens the main menu.

**Version.** v0.1.5.

**I2. The d-pad could not be used from the keyboard**

**Symptom.** The save-slot carousel and other d-pad menus were unreachable
without a controller.

**Cause.** The runtime binds the d-pad to Shift plus an arrow, and modifier
combinations do not reach the game at all.

**Fix.** The plain arrow keys, which were free.

**Version.** v0.3.3.

**I3. The controller did nothing, and the game kept asking you to sign in**

**Symptom.** Two reports, one cause: the character did not move, and a sign-in
prompt that could not be satisfied.

**Cause.** The runtime's default input policy feeds device N to guest user N,
and this single-player title polls user 0 only. A pad that does not enumerate
first landed on a user nobody was listening to. It was never seen on the
development machine because the keyboard is a synthetic device and synthetic
devices always feed user 0.

**Fix.** The SDK's own `SharedAssignment` policy, every device feeding user 0,
applied in `OnPostSetup` before the guest starts polling. The log line
`Input: every controller drives guest user 0` confirms it.

**Version.** v0.3.4.

**I4. Escape meant two things**

**Symptom.** Escape both skipped a video and quit.

**Cause.** It was one of the skip keys.

**Fix.** Escape quits and nothing else; Start, A, Space, Enter or a mouse click
skip a video.

**Version.** v0.3.0.

**I5. Pressing anything during a video also worked the menu behind it**

**Symptom.** In the overlay era the press that skipped the intro also picked
whatever the main menu's cursor was on, without the menu ever being seen.

**Cause.** The guest was never given the video file, so its own playback failed
instantly and it was already sitting in the main menu taking input behind the
picture.

**Fix.** Guest input was held back for exactly as long as the overlay drew,
through the runtime's input-active callback, which also kept its foreground
check. Moot since the overlay was removed.

**Version.** v0.4.4.

---

## Tools and diagnostics

This section also covers the build and release tooling, since a defect there
reaches the player exactly as a defect in the game does.

**X1. v0.3.4 to v0.3.8 were built with no optimisation at all**

**Symptom.** `ng2.exe` grew from 143.7 MB to 580.5 MB and nothing in those
releases explained it.

**Cause.** A poisoned CMake cache. A build triggered a reconfigure at a moment
when clang was not on PATH; the compiler check failed and left the release
flags cached empty, and every configure afterwards reused the empty value. No
error was ever printed again; the build was green every time.

**Fix.** The cache deleted and reconfigured in an environment with both
toolchains; the build script sets up vcvars and LLVM every time and passes
`rexglue_DIR`. Back to 143.8 MB, a 65 MB release zip, and recompiled code that
is actually optimised.

**Version.** v0.3.9.

**X2. Releases shipped without a required tool, twice**

**Symptom.** v0.3.4 shipped `prepare_videos.py` without the encoder, so no
install ever prepared videos and the game silently fell back to its broken
playback. v1.0.0's first cut shipped none of the three texture scripts, so every
texture control failed with "missing from this install", a fault invisible in
the source tree and certain in the package.

**Cause.** The packaging script's tool list was advisory the first time and
empty the second. The first failure had already been written down in that
script's own docstring; it just was not enforced.

**Fix.** Tools are declared with a required flag and a missing one stops the
release. The ASSEMBLY sweep of the census checks the package for every script
the code asks for. `tools/make_release.py` also refuses a version with no
changelog section, refuses to package anything that looks like game data, and
records the hash of every source input in a provenance file.

**Version.** v0.3.5, v1.0.0.

**X3. The publication allowlist dropped one document and let another through**

**Symptom.** The write-up of the VMX128 investigation was never published, and
allowing `.txt` then immediately staged an index of decoded game textures.

**Cause.** The docs rule allowed `.md` and nothing else, dropping a file by
extension rather than by decision. `docs/texpack` was excluded only by the
coincidence of its two files' extensions, while the script's docstring claimed
it was excluded by rule.

**Fix.** `texpack` denied by name in `tools/stage_repo.py`, found by counting
what it staged rather than trusting it.

**Version.** v1.0.0.

**X4. The stuck-wait watchdog watched one wait path in five**

**Symptom.** The watchdog exists to name what a hung guest thread is waiting
on. Its silence had been used as evidence: "the watchdog reports no stuck
kernel waits" appears in the chapter 12 notes as a ruled-out cause. A live
capture then found the audio callback sitting in `KeWaitForMultipleObjects` for
over half an hour with the watchdog saying nothing.

**Cause.** It instrumented `NtWaitForSingleObjectEx` and nothing else; four
other wait entries and `RtlEnterCriticalSection` were invisible.

**Fix.** One scope guard at all five entry points and at the shared primitive
itself, so everything funnelling through it is covered by construction.
Thresholds moved from 10 seconds to 30 with a final report at 300, because at
10 a healthy title screen produced thirteen warnings a minute. The 300-second
line reports a fact, not a verdict: two threads crossed it on a single unbroken
wait while the game held 60 fps, so it is read next to a frame counter. This
is a runtime change.

**Version.** v1.0.0.

**X5. A census that enumerates its own subjects**

**Symptom.** Three real defects sat in the settings for a version or more
without being noticed (V8, D9, X2).

**Cause.** No check took its subjects from the artefact itself.

**Fix.** `tools/lodestone_census.py` takes every field in `ng2_settings.h`,
every cvar that mirrors one, and every script the C++ looks up by name, and
requires each to be reachable, documented, or declared in
`tools/settings-ledger.json` with a reason. Its DELIVERY sweep (D9), DEFAULTS
sweep (V8) and ASSEMBLY sweep (X2) each exist because of a specific bug, and
each was verified by reintroducing that bug.

**Version.** v1.0.0.

**X6. A census of Xenia's known issues for this title**

**Symptom.** Xenia's compatibility report for the title stops at "loads, plays
some sounds for a moment and crashes", so everything this port meets after that
point is absent from it by construction.

**Cause.** No document related the two.

**Fix.** `docs/XENIA_ISSUES.md` takes the eight labels on Xenia's issue as its
subjects and gives each a verdict, and a second table lists what Xenia never
recorded. Two of its rows have moved on since it was written: the music death
(A2) and the chapter transition (C2) are fixed.

**Version.** v1.0.0.

**X7. One button for a bug report**

**Symptom.** A report needs three files from two folders, and asking for that
is asking for a report with none of them.

**Cause.** No collector.

**Fix.** "Copy diagnostics to a file" on both settings screens gathers the
session's log, the settings and what the machine is, with no game data;
`NG2_DIAGNOSTICS=1` writes the same file during start-up for a game that never
reaches a menu.

**Version.** v1.0.0.

**X8. Stutter was not measurable, and the load of the pack was not visible**

**Symptom.** The diagnostic line reported an average over a clock whose
resolution cannot measure one frame, so a second holding one 200 ms frame
still reported 60 fps.

**Cause.** The wrong statistic and the wrong clock.

**Fix.** p50, p99, the worst frame and a count of frames over twice the median,
from a steady clock; the first run after the change showed a 95.6 ms frame the
old counter would have called a healthy 59.4 fps. On F8, live frame rate, GPU
load and video memory for this process only, because on a machine that also
runs 24 GB of other GPU work a system-wide number says nothing about the game;
GPU load comes from the PDH engine counters so it is vendor-neutral.

**Version.** v0.5.4.

**X9. Every in-process probe cost the player their session**

**Symptom.** Each probe change relinked `ng2.exe`, and the linker cannot
replace a running executable, so the player's game was closed six times before
better tooling existed.

**Cause.** Analysis lived inside the process.

**Fix.** `tools/watch_guest.py` attaches to the running game as a debugger,
sets hardware watchpoints on a guest address and reports the writing function
with a caller chain; `tools/dump_table_live.py` makes the running process write
out its guest-to-host function table. This found the ledge loop (R4) without
closing the game. Notes for reuse: guest memory is mapped at several host
addresses and a watchpoint is a host address, so watch every alias; never
dereference a translated pointer without `VirtualQuery` first; declare ctypes
argument types or 64-bit addresses are truncated silently. A later addition:
reading and writing guest words from outside through `ReadProcessMemory` and
`WriteProcessMemory`, big-endian, is what found A2 and proved C2, and works on
Xenia too. A debugger session that disassembles with symbols suspends every
thread for seconds and has killed the game silently; short captures have not.

**Version.** v0.5.2.

**X10. `add_function.py` sizes some functions wrong**

**Symptom.** For a lone `b` thunk it proposed 0x804 bytes, and for an adjustor
thunk 0x3DD4 bytes for two instructions, in each case walking through padding
and unrelated functions that registering the span would have swallowed.

**Cause.** Its size heuristic scans forward to the next `blr` and does not stop
at an unconditional branch.

**Fix.** Both corrected by hand (4 and 8 bytes); the rule is to check the size
it prints against the disassembly every time. The tool's own docstring claim
that these functions are "only discoverable by running" was also wrong for the
adjustor thunks, which a static pattern sweep found (R1).

**Version.** v0.1.2, v0.5.1.

**X11. A literal NUL byte in `src/patch_hooks.cpp`**

**Symptom.** The file read as binary to every tool that touched it.

**Cause.** An earlier edit ate the backslash of `'\0'`; it compiled to the same
value, which is why it went unnoticed.

**Fix.** Restored, and later edits to that file are made at the byte level for
the same reason.

**Version.** v0.2.1.

**X12. The README's known gaps were three-quarters wrong**

**Symptom.** It still claimed `setjmp` and `longjmp` were unset, a branch was
unresolved, and DLC was not wired up.

**Cause.** Written before the fixes and never revisited.

**Fix.** Rewritten against what the project actually did. The habit that
followed is the changelog's: a claim later found wrong is corrected in place
and says so.

**Version.** v0.1.4.

**X13. The GPU plugin that was never deployed**

**Symptom.** For four sessions the ring fix (D12), the ring-state dump and the
per-stage warming (T11) printed nothing in the running game although every
build succeeded.

**Cause.** The app build copies the runtime and GPU plugin from the SDK install
tree into the run directory, and that tree held stale DLLs, so each app build
copied the stale plugin over the fresh one. The sizes were the tell: 6.4 MB
fresh against 2.7 MB stale. A related trap: a source-built plugin beside a stock
runtime makes the game exit during start-up with no error, the log simply
stopping after the tuning lines.

**Fix.** Fresh DLLs are deployed to both the SDK tree and the run directory,
and a marker string is checked in the run-directory DLL after an app build.
`tools/make_release.py` refuses to package one stock DLL beside one source-built
one.

**Version.** Recorded in `HANDOFF.md` and `docs/TEXTURE_PACK.md`; enforced in
the release script from v1.0.1.

**X14. The game launched from the tool shell could not start Python**

**Symptom.** "Process textures" failed with "Access is denied" starting
`python.exe`, thirteen times in five seconds, on a machine where the same
command had worked the day before.

**Cause.** Environmental to the launching shell: a game started from the
assistant's tool shell could start `py.exe` but `py.exe` could not start
`python.exe`. The identical game started through WMI ran the whole upscale.

**Fix.** The game and any long-lived listener are launched through WMI, which
also keeps them alive when the session that started them ends. The
thirteen-times relaunch was the launcher failing fast; if it recurs with a
working spawn, the button is re-arming (O14).

**Version.** Recorded in `HANDOFF.md`, 2026-09-06.

**X15. Three bugs in `xrefs.py`, each a confident wrong answer**

**Symptom.** Every address shifted by the section base, stack writes reported
as global ones, and every match discarded.

**Cause.** A file offset added to a virtual address, stores with base `r1` not
excluded, and an `addi`'s own destination treated as a clobber.

**Fix.** All three corrected. The rule that came out of it, and out of the
hex-grep that missed the chapter handshake because codegen writes `lis`
immediates as signed decimal, is to validate a scanner against a reference
whose answer is already known before trusting it.

**Version.** Recorded in `docs/DEVELOPMENT_JOURNAL.md` and `HANDOFF.md`.

---

## Still open

Everything not fixed at v1.0.6. Each was checked against every later version
before being listed here.

**O1. True ultrawide rendering is not there.** Forcing the internal render size
to 21:9 makes the game render into a wider buffer, but its 2D layer does not
follow: the chapter card stretches and the credits sit off centre, because the
game composes overlays in a fixed coordinate space. Whether the 3D field of
view widens was never measured on a playable frame. What works is an ultrawide
window with "Keep aspect ratio" on, which since v1.0.1 really pillarboxes (D7).
Recorded v0.3.7.

**O2. A second ring-buffer failure at the attract demo.** Unrelated to the
re-initialisation race (D12): reproduced three times on 2026-09-05 with no
re-initialisation in the log, twice ending in a hard freeze, with a
deterministic garbage packet count near the ring size, which reads as a
write-pointer value taken for a packet header. The recovery is what makes it
fatal: skipping to the write pointer discards any interrupt the guest was
waiting on. It has not been observed since the ring fix was really deployed
(26 clean cycles in a 67-minute run and more since), and the ring-state dump
that would root-cause it is in the deployed plugin, but it is not closed until
a long unattended run says so.

**O3. Withdrawn: "chapter 12 runs at about 28 fps".** The low frame rate was
measured while this machine's graphics card was saturated by other work; it
was never the port's. Not an open issue.

**O4. Mission Mode needs the game's title update.** Not a licence or content
problem: a full licence mask and installing the content under the profile's
XUID both change nothing. A title update patches the executable, so for a
static recompilation it means applying the `.xexp` to the XEX and running
codegen again; `rexglue` cannot apply one, and every function override would
need re-verifying against the new layout. Recorded v0.1.4.

**O5. Bloom cannot be exposed as a setting.** The game's bloom parameters are
pixel-shader constants c15 and c16 of a shader whose constant table is known,
but nothing in the image points at that shader, so the code that uploads them
is not findable from the names. Xenia has no bloom patch for this title
either. Recorded v0.1.3, v0.1.4.

**O6. No FSR or CAS output filter, no SMAA, TAA or true MSAA.** The presenter
implements bilinear only and the plugin declares no other post effect than
FXAA; these are limits of the SDK build, not of the menu (D8, D11). Recorded
v0.1.0, v0.1.1, v0.4.1.

**O7. Chapter-loading videos cannot be skipped.** By design: the game treats
one of them failing to open as a bad disc (V5).

**O8. The AI texture option needs Python and a download the release does not
bundle.** By design; the plain upscaler works without either, and the buttons
say what is missing.

**O9. Pending achievements are awarded at the next chapter load, not at the
chapter end.** The transition fix (C2) supplies the completion state the game
waits for; the achievement write itself is still issued by the runtime when the
chapter next loads, as it was before. Awarding them at the chapter end is a
possible follow-up.

**O10. More functions the analyzer never saw will be found.** About 1,300
candidates from the bulk scanner need per-candidate extent validation before
they could be registered, and a previous attempt at bulk registration produced
overlapping extents. Expect one genuine one per new area of the game, found by
crashing into it (R1).

**O11. BC7 and BC3 texture compression for the pack were never attempted.**
The pack is uncompressed RGBA, which is why 4x costs what it does (T1).

**O12. Two codegen warnings in the CRT region** (R8), and one unimplemented
xam message (`XLiveBaseUnk58046`, called once), neither proven to matter.

**O13. The gamertag cannot be set.** The SDK's profile has no setter for its
name or XUID and is permanently signed in (S14).

**O14. Minor and unconfirmed.** The "Process textures" button
possibly re-arming after a fast failure (X14); on machines whose raw dump was
cleared before the content-hash migration, a number of pack files are ignored
until their scene is dumped again (T14).

---

## How the fixes are applied

The fixes live in three places, and knowing which is which is what makes a
rebuild safe.

**Hooks declared in TOML, regenerated with the code.** `config/hooks/patches.toml`
declares the midasm hooks that change the game's behaviour, and the codegen
emits the call for each at the named address. Their bodies are plain C++
functions in `src/` with the signature `void Name(PPCRegister& rXX, ...)`
matching the TOML register list:

| Hook | Address | Body | Purpose |
|---|---|---|---|
| `ng2PatchRenderSize1`, `ng2PatchRenderSize2` | 0x836261E8, 0x837C62FC | `src/patch_hooks.cpp` | the 1280x720 internal render size, as the community patch does it |
| `ng2PatchChapter12` | 0x82834C78 | `src/patch_hooks.cpp` | the pointer guard that replaced the community workaround (C1) |
| `ng2PatchSkipVideos` | 0x8380EB9C | `src/patch_hooks.cpp` | "Skip intro videos" (V8) |
| `ng2AudioBarrierFix` | 0x8374F164 | `src/ng2_audio_fix.cpp` | the music that died mid-session (A2) |
| `ng2ChapterAwardFix` | 0x8242D7B4 | `src/ng2_chapter_fix.cpp` | the red mist after a boss (C2) |

`config/hooks/diagnostics.toml` declares six hooks that only observe: the file
open (which also drives the cinematic auto-skip and the chapter tracking the
guard uses), the context switch counter, the once-per-frame tick behind the
opt-in capture tools, and three from the first start-up investigation. Their
bodies are in `src/diag_hooks.cpp`.

Xenia ships its patches as guest memory patches. That cannot work for a static
recompilation, because the values being patched are immediates inside
instructions that have already been translated to C++ constants; rewriting the
register right after the instruction runs has the same effect.

**Hand edits to the generated code, re-applied by script.** Two fixes cannot be
expressed as a hook and are applied to `generated/default` after codegen:

- `local/diag/patch_missed_regs.py`: R6, the split-fragment register carry,
  14 sites in four files.
- `local/diag/patch_scanguard.py`: C4, the null guard at the entry of four
  command-list scanners.

Both are idempotent and mark their edits `NG2FIX`. Codegen rewrites any
generated file whose content differs from what it would emit, so both edits are
lost whenever the code is regenerated and must be re-applied.

**Manifest and runtime configuration.** `ng2_manifest.toml` carries the
function overrides (`config/functions.toml`, R1), the `setjmp` pair (B1), the
register-localisation flags (R2) and the five shared vector registers (R4);
`config/rexcrt.toml` maps the fiber family (B2); `src/ng2_tuning.h` writes the
runtime and GPU-plugin settings the game needs (B3, B4, D1). Three fixes are in
the SDK itself rather than in this project: the VMX128 localisation (R3) and
`db16cyc` (R5) in the codegen tool, and the ring re-initialisation (D12), the
audio credit (A1), the watchdog (X4) and `video_mode_explicit` (D7) in the
runtime. The runtime and the GPU plugin must always be deployed as a pair from
the same build (X13).

**Regenerating the code.** After any change to the TOMLs:

1. `scratchpad/codegen_ng2.cmd` (runs `ninja ng2_codegen`; only files whose
   content changed are rewritten).
2. `python local/diag/patch_missed_regs.py`
3. `python local/diag/patch_scanguard.py`
4. Check that no `bne` was emitted as a less-than test: over
   `generated/default/*.cpp`, the line after each `// bne cr6` comment must
   not read `cr6.lt) goto`. A fresh codegen gives zero.
5. Build with `scratchpad/build_ng2.cmd`.

**Do not run `local/diag/patch_fix_ge2.py`.** It is the script that turned four
equality checks into greater-or-equal checks and caused the state clamp that
was later misattributed to the code generator (R7). It is kept for the record
with a superseded notice at its top.

**What v1.0.6 removed.** The instrumentation used to find C2 (a per-function
ring tracer in the precompiled header, state dumps, a synthetic auto-click, the
`--ng2_chfix3` test switch and the external remedies) is gone: the generated
tree was regenerated from the game with only the fixes above re-applied, and
`src/ng2_app.h` and `src/ng2_autoskip.cpp` are byte-identical to the v1.0.5
release, verified against the hashes in its provenance file. The release
package's provenance file lists the hash of every source and configuration
input, so whether a file is at its shipped state is a hash comparison.

---

## Appendix: changelog coverage

Every `###` heading in `CHANGELOG.md`, numbered in file order, and the entry
that covers it. Headings that are not issues (a "How it works", a note, a
findings list) are mapped to the entry that carries their substance.

| # | Version | Heading | Entry |
|---|---|---|---|
| 1 | v1.0.9 | Fixed - the settings menu still forced the full redo | T17 |
| 2 | v1.0.8 | Fixed - a texture run that stopped halfway redid every texture | T17 |
| 3 | v1.0.7 | Fixed - Escape took three seconds to quit | B5 |
| 4 | v1.0.6 | Fixed - the red mist after a boss | C2 |
| 5 | v1.0.6 | Fixed - the release needed the Visual C++ runtime | B8 |
| 6 | v1.0.6 | Fixed - the pack could serve the wrong texture | T14 |
| 7 | v1.0.6 | Fixed - "Dump while playing" did nothing | T15 |
| 8 | v1.0.6 | Fixed - the census could not see a second texture | T16 |
| 9 | v1.0.5 | Fixed - the settings pointer never hid | S20 |
| 10 | v1.0.5 | Fixed - the texture upscale died when the menu closed | T12 |
| 11 | v1.0.5 | Changed - the enhanced-texture status false count | T13 |
| 12 | v1.0.4 | Fixed - crash at the chapter 13 boss | R6, C3 |
| 13 | v1.0.4 | Still open - chapter 12 never hands over to 13 | C2 |
| 14 | v1.0.3 | Fixed - chapter 12 never handed over to 13 | C1, C2 |
| 15 | v1.0.2 | Fixed - the music died mid-session | A2 |
| 16 | v1.0.2 | Changed - the texture counts | T7 |
| 17 | v1.0.2 | Added - processing only what is missing | T7 |
| 18 | v1.0.1 | Fixed - the whole picture stretched | D7 |
| 19 | v1.0.1 | Fixed - Cancel did not cancel | T5 |
| 20 | v1.0.1 | Fixed - the progress bar reached 100% | T6 |
| 21 | v1.0.1 | Fixed - the AI upscaler garbled at 2x | T4 |
| 22 | v1.0.0 | Added - a Browse button | S18 |
| 23 | v1.0.0 | Changed - every setting editable in-game | S18 |
| 24 | v1.0.0 | Fixed - the stuck-wait watchdog | X4 |
| 25 | v1.0.0 | Fixed - `db16cyc` translated to nothing | R5 |
| 26 | v1.0.0 | Fixed - two graphics settings never reached the plugin | D9 |
| 27 | v1.0.0 | Changed - V-Sync states its consequence | D10 |
| 28 | v1.0.0 | Added - a census of Xenia's known issues | X6 |
| 29 | v1.0.0 | Fixed - the publication allowlist | X3 |
| 30 | v1.0.0 | Fixed - the attract demo no longer freezes | D12, O2 |
| 31 | v1.0.0 | Added - one button for a bug report | X7 |
| 32 | v1.0.0 | Added - a Lodestone census | X5 |
| 33 | v1.0.0 | Added - the texture pack is warmed per stage | T11 |
| 34 | v1.0.0 | Fixed - audio could stop silently | A1 |
| 35 | v1.0.0 | Fixed - opening the settings could kill the process | S19 |
| 36 | v1.0.0 | Fixed - the settings menu scanned two folders | T10 |
| 37 | v1.0.0 | Changed - dumping and the pack cannot both be on | T8 |
| 38 | v1.0.0 | Changed - the upscaler is a choice | T9 |
| 39 | v1.0.0 | Fixed - the Chapter 12 workaround applies only to 12 | C1 |
| 40 | v1.0.0 | Added - three more guest functions | R1 |
| 41 | v1.0.0 | Fixed - the release shipped no tools | X2 |
| 42 | v1.0.0 | Fixed - "Skip intro videos" could not be turned off | V8 |
| 43 | v1.0.0 | Fixed - "Skip chapter cinematics" could never fire | V9 |
| 44 | v1.0.0 | Removed - the video conversion machinery | V11 |
| 45 | v0.5.4 | Added - live CPU/GPU/VRAM readouts | X8 |
| 46 | v0.5.4 | Changed - supersampling goes to 8x | D8 |
| 47 | v0.5.4 | Added - accurate depth and fuzzy alpha | D8 |
| 48 | v0.5.4 | Fixed - F9 no longer overwrites the setting | T2 |
| 49 | v0.5.4 | Added - AI upscaling, as an optional download | T3 |
| 50 | v0.5.4 | Added - the memory settings from the hardware | S17 |
| 51 | v0.5.4 | Added - F9 switches the pack | T2 |
| 52 | v0.5.4 | Changed - the pack format is raw, default 2x | T1 |
| 53 | v0.5.4 | Added - the texture pack is actually used | T1 |
| 54 | v0.5.4 | Added - frame-time statistics | X8 |
| 55 | v0.5.4 | Changed - the texture cache can be raised | D8 |
| 56 | v0.5.3 | Added - an application icon | S16 |
| 57 | v0.5.3 | Fixed - "Quit Game" returns to the setup screen | B6 |
| 58 | v0.5.3 | Fixed - the video mode setting could never select 0 | V10 |
| 59 | v0.5.3 | Fixed - the game crashed ~90 seconds after launch | B7 |
| 60 | v0.5.3 | Changed - texture dumping refuses non-art | T1 |
| 61 | v0.5.2 | Fixed - character stuck at a ledge | R4 |
| 62 | v0.5.2 | Added - external live analysis | X9 |
| 63 | v0.5.1 | Fixed - a crash in Chapter 5 on ten thunks | R1 |
| 64 | v0.5.1 | Fixed - "Quit Game" left a black screen | B6 |
| 65 | v0.5.1 | Not fixed - the ledge jump loop | R4 |
| 66 | v0.5.0 | Fixed - every video in the game | V1, R3 |
| 67 | v0.5.0 | Removed - the video re-encoding pipeline | V11 |
| 68 | v0.4.5 | Added - the port's own version, on screen | S15 |
| 69 | v0.4.4 | Fixed - pressing anything during a video worked the menu | I5 |
| 70 | v0.4.3 | Changed - the monitor list shows resolutions | D5 |
| 71 | v0.4.3 | Fixed - the installer forgot which disc image | S7 |
| 72 | v0.4.2 | Fixed - 4K could not be set | D4 |
| 73 | v0.4.2 | Fixed - the monitor list was in the wrong order | D5 |
| 74 | v0.4.1 | Added - two graphics levers | D8 |
| 75 | v0.4.0 | Fixed - the settings screen lost its buttons | D3 |
| 76 | v0.4.0 | Added - update an install in place | S8 |
| 77 | v0.3.9 | Fixed - built with no optimization | X1 |
| 78 | v0.3.8 | Changed - import a folder of saves | S13 |
| 79 | v0.3.7 | Fixed - the install finished at 106% | S5 |
| 80 | v0.3.7 | Fixed - Rescan did not see freshly installed files | S6 |
| 81 | v0.3.7 | Fixed - videos stretched on an ultrawide | V7 |
| 82 | v0.3.7 | Known - true ultrawide rendering is not there | O1 |
| 83 | v0.3.6 | Changed - one cheat | S12 |
| 84 | v0.3.6 | Fixed - the setup-screen tick pushed Save off | S11 |
| 85 | v0.3.5 | Fixed - releases shipped without ffmpeg | X2 |
| 86 | v0.3.5 | Fixed - three settings read but never written | S9 |
| 87 | v0.3.5 | Fixed - Escape did not close the game | B5 |
| 88 | v0.3.5 | Changed - Cheats are ticks | S12 |
| 89 | v0.3.5 | Added - ultrawide resolutions | D6 |
| 90 | v0.3.5 | Added - Save settings, and a way back | S10 |
| 91 | v0.3.4 | Added - import a saved game | S13 |
| 92 | v0.3.4 | Fixed - reading an STFS display name | S13 |
| 93 | v0.3.4 | Fixed - the controller did nothing, sign-in prompt | I3 |
| 94 | v0.3.4 | Added - Skip intro videos | V8 |
| 95 | v0.3.4 | Changed - the profile, saves and DLC live with the game | S14 |
| 96 | v0.3.4 | Added - a Cheats section | S12 |
| 97 | v0.3.4 | How Infinite karma works without an address | S12 |
| 98 | v0.3.4 | Fixed - the search crashed the game | S12 |
| 99 | v0.3.4 | Found - karma lives at 0x230 | S12 |
| 100 | v0.3.4 | Fixed - the system save's content header | S13 |
| 101 | v0.3.3 | Fixed - loading a save raised "Disc Read Error" | V5 |
| 102 | v0.3.3 | Fixed - videos looked for in the wrong place | V6 |
| 103 | v0.3.3 | Fixed - the d-pad from the keyboard | I2 |
| 104 | v0.3.3 | Save import | S13 |
| 105 | v0.3.2 | The Chapter 12 guard is now properly verified | C1 |
| 106 | v0.3.1 | The Chapter 12 workaround is automatic | C1 |
| 107 | v0.3.0 | Fixed - the intro had stopped playing | V4 |
| 108 | v0.3.0 | Added (videos prepared at install, live with the game, Escape quits) | S4, I4 |
| 109 | v0.3.0 | Changed (one bar, no consoles, ffmpeg beside the game, Videos row gone, Chapter 12 row gone) | S4, C1 |
| 110 | v0.3.0 | Chapter 12 cannot be automated | C1 |
| 111 | v0.2.2 | The intro reads as one picture | V2 |
| 112 | v0.2.2 | Also ruled out | V2 |
| 113 | v0.2.1 | The attract demo now plays instead of being skipped | V3 |
| 114 | v0.2.1 | Fixed (own size, NUL byte, edge trim) | V2, X11 |
| 115 | v0.2.1 | On the intro seams | V2 |
| 116 | v0.2.1 | Note | V2 |
| 117 | v0.2.0 | How it works | V2 |
| 118 | v0.2.0 | Four things this cost | V2 |
| 119 | v0.2.0 | Also | V2 |
| 120 | v0.2.0 | Known | V2 |
| 121 | v0.1.5 | Added (preset, keyboard control, hide pointer) | S3, I1 |
| 122 | v0.1.5 | Changed (Workarounds group, tighter rows) | S3 |
| 123 | v0.1.5 | Not brought over from re:Blue | S3 |
| 124 | v0.1.4 | Added (Skip videos) | V8 |
| 125 | v0.1.4 | Findings (Mission Mode, bloom) | O4, O5 |
| 126 | v0.1.4 | Documentation | X12 |
| 127 | v0.1.3 | Added (Chapter 12 crash workaround) | C1 |
| 128 | v0.1.3 | Bloom | O5 |
| 129 | v0.1.2 | Two things worth knowing (and the four absorbed functions above it) | R1, X10 |
| 130 | v0.1.1 | Fixed (DLC costume crash) | R1 |
| 131 | v0.1.1 | Settings | S2, D11 |
| 132 | v0.1.1 | Not added, and why | D11, T1, O6 |
| 133 | v0.1.0 | The recompilation | B1, B2, B3, B4 |
| 134 | v0.1.0 | Settings | S1 |
| 135 | v0.1.0 | Fixed | D1, D2, S1 |
| 136 | v0.1.0 | Known issues | V1, V3, O6 |
