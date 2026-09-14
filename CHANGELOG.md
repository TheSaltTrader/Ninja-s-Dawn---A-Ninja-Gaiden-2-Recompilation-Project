# Changelog

Versions are cut with `tools/make_release.py`, which refuses to package a
version that has no section here.

## v1.0.18 - 2026-09-14

### Fixed - two crashes that slipped back into v1.0.17

Two recompilation fixes that live as post-generation patches were silently lost
when the translated game code was last regenerated, so v1.0.17 shipped without
them. Both are restored:

- **A crash when using Ninpo / during a late boss.** A code fragment reached by
  a branch inside one function had its saved registers re-initialised to zero
  instead of carried across, so it wrote through a null pointer. Restored the
  register hand-off (`patch_missed_regs`).
- **A crash at the Chapter 12 -> 13 transition.** The end-of-chapter effect-list
  scanners were not null-guarded, so a freed (null) list walked off into
  unmapped memory. Restored the guard (`scanguard`).

Ultrawide (3D) and everything else in v1.0.17 are unchanged.

## v1.0.17 - 2026-09-14

### Added - Ultrawide (3D) field of view

A new **Ultrawide (3D)** switch on the Display screen widens Ninja Gaiden II's
horizontal field of view to fill a wider-than-16:9 monitor with correct
proportions: the world, characters, enemies and effects all show more across
the width, with no stretching, and the vertical view and depth are unchanged.
It is scene-aware - actual gameplay fills the screen while full-screen menus,
videos and the in-game HUD stay 16:9 (pillarboxed with black bars on an
ultrawide), so 2D art is never stretched. It takes effect immediately, with no
restart, and is off by default.

Under the hood the GPU plugin scales the horizontal column of each 3D draw's
projection (a Hor+ widen that is mathematically exact), detects gameplay from
menus by how much 3D versus 2D each frame draws, and caches the per-shader
projection location so the widen costs no measurable frame rate.

### Fixed - the on-screen FPS readout now shows the game's frame rate

The F8 FPS number was counted once per host present, so on a high-refresh
monitor with V-Sync it showed the display's rate (e.g. 165) rather than the
game's. It is now counted from the game's own main-loop frame, so it reflects
how fast the game is actually running.

### Changed

The fine field-of-view slider from an earlier internal build was removed; the
Ultrawide toggle alone picks the correct field of view for the monitor.

## v1.0.16 - 2026-09-13

### Added - the game checks for updates on launch and can install them itself

On start-up the game now asks GitHub whether a newer release exists and, if
one does, shows a prompt: **Update now**, **What's new**, or **Later**.
Choosing to update downloads the release zip and, after a confirmation,
closes the game, swaps in the new program files and reopens - the player
never visits the website or unzips anything by hand. The installer only ever
replaces the program (the executable, the runtime and GPU plugin, the Visual
C++ runtime, the controller database and the texture tools); `game\`, `dlc\`,
`user\` and the settings files are never touched, the same contract as a
manual "unzip over it" update.

The check is a background network call that never delays the boot, and the
prompt appears only when there is genuinely a newer version. A new setting,
**"Check for updates when the game starts"** (on by default), turns the whole
thing off for anyone who would rather keep launch offline; the F10 settings
menu also has a **Check now** button and shows the download progress and the
same install button. The check talks only to the GitHub releases API and
downloads only the signed release asset over HTTPS (WinHTTP); nothing else
leaves the machine.

Implemented app-side in `src/ng2_update.*` with no plugin change; the runtime
and GPU plugin are the stock pair, unchanged since v1.0.13.

## v1.0.15 - 2026-09-13

### Fixed - the bundled AI upscaler was never looked at

v1.0.14 put the Real-ESRGAN engine in the zip at `tools/upscaler/` and made
AI the default, but the two places that decide whether the AI is available
- the Textures page and the pack tool - still looked only in the texture
folder's own `upscaler/`, where the Download button puts a copy. On a fresh
install the page therefore said "not installed yet", reset the method to
Lanczos, and every pack was made with Lanczos: the very thing v1.0.14
claimed to end. Both now check the texture folder first and the port's
`tools/upscaler/` second, the way the Fable 2 port does, and the log line
"Texture tools: ..." written when the Textures page is first opened says
which copy was found.

### Changed - the pack tool decides before it decodes, and holds paths, not pixels

Phase 1 of a full run decoded every dumped texture in pure Python and only
then asked whether it was art; on the Fable 2 port's 196,000-dump folder
that was two and a half hours to pack 70,000, with every decoded image kept
in memory until phase 2 (heading for some 45 GB). The tool now turns render
targets, fonts and HUD away by shape and format before decoding, reuses the
decoded PNG beside each raw dump when it exists (same decoder, same bytes),
and keeps only the path until the chunk that needs the pixels. The output
is byte-identical to the old tool's (117 of 117 on a 400-texture sample);
phase 1 of that Fable 2 run went from hours to thirty seconds.

### Fixed - continuing a stopped run no longer keeps the previous pack's files

A run at new settings rewrites the manifest first and then overwrites the
pack file by file. Stopped halfway, the next run saw a manifest matching
its settings and marked incomplete, and continued it - counting every
file already in the folder as done, including the ones the OLD settings
had made. Files older than the manifest of a stopped run are now redone.

## v1.0.14 - 2026-09-13

### Added - the AI upscaler ships in the zip, and is the default

The Textures page has offered Real-ESRGAN since the pack tool existed,
but a fresh install had to fetch the engine with the Download button
first, and until then every upscale fell back to Lanczos. The release now
carries the engine at `tools/upscaler/` (Real-ESRGAN ncnn-vulkan with the
x4plus model and its siblings - a local model that runs on your GPU, no
account, no network; BSD-3, third party), and AI at detail strength 0.75
is the default for new settings. Existing settings files keep whatever
they say. The choice came out of the ACME Texture Upscaler comparisons
and a trial on real game textures in the Fable II port the same day: the
AI wins clearly on hard surfaces (wood grain, rivets, edges) and the
strength blend keeps it from turning soft organic textures to speckle.
The Download button stays for installs that lack the folder. The runtime
and GPU plugin are the stock pair, unchanged from v1.0.13.

## v1.0.13 - 2026-09-12

### Fixed - streamed textures (like chapter 10) can now be upscaled

Some stages stream many textures through the same memory addresses. The pack
matched a texture the moment it was created, on whatever bytes were there then,
which for a streamed texture is a leftover from the previous one. So the game
asked for an image the pack did not have and fell back to the original, and no
amount of dumping helped, because the match happened at the wrong moment.

The match now happens when the texture is actually loaded, on the bytes really
in memory - the same bytes the dump records. Each texture keeps its normal
version, always drawn, and gains a separate upscaled copy only when its real
content is found in the pack, rechecked every load so a reused address stays
correct. Nothing can go missing: a texture with no pack entry simply shows at
its original resolution. Measured in chapter 10: about 1500 textures upscaled
where roughly 400 did before, at full frame rate, with your existing pack.

To fill a stage out, dump it and process it as before; the difference is that
the dumped textures now actually take effect. This applies to every chapter.

### Changed - texture dumping now takes effect on the next launch

Turning dumping on mid-stage only captured what loaded afterwards, so the start
of the stage and everything already in memory were silently missed, which read
as "dumping does nothing". Dumping is now a restart-required setting, marked as
such in the menu: it begins at the next launch and captures the whole stage
from its start.

## v1.0.12 - 2026-09-12

### Fixed - switching the texture pack with F9 a few times in a row crashed the game

Every crash of this kind had the same stack: the GPU plugin reading a 4x pack
file into a texture that had been created at 1x. A texture created while the
pack was off has the game's own size; when the pack came back on before the
end-of-frame cache clear had recreated it, the upload looked the pack file up
again, found it, and read four times the buffer. The 1.5-second guard on the
key (v1.0.10) only narrowed the window.

The plugin now records the replacement it chose when the texture was created
and uploads that one, and refuses any file whose size differs from the texture
it is going into. This port takes the runtime and GPU plugin pair that carries
it (the Fable II port, which shares the plugin, made the change), so F9 can be
pressed as often as wanted, mid-load included.

## v1.0.11 - 2026-09-11

### Fixed - collapsing the settings window crashed the game

The arrow at the top left of the in-game settings window collapses it, as
any such window does. Collapsed, the window is not drawn, but the texture
section went on submitting its rows into a table that was never begun, and
the first row read through a null table pointer inside the runtime. The crash
dump named the line. The window no longer offers the arrow (F10 closes it), a
window that is not drawn ends early, and the texture table is guarded like
every other table on the page.

Found through Windows' own crash dumps in `%LOCALAPPDATA%\CrashDumps`, which
the port had never looked at: every crash the game has had is recorded there
with a stack, even when the log stops without a word. Four other crashes from
the same evening are named in `docs/ISSUES_AND_FIXES.md` (S22, still open).

## v1.0.10 - 2026-09-11

### Fixed - pressing F9 twice quickly could crash the game

F9 switches the texture pack on and off during play. A path change makes the
GPU plugin clear its pack tables at once and drop the texture cache at the end
of the frame, after draining the GPU; a second press landing inside that
window flipped the path back while the first clear was still pending. The
Fable II port, which shares the plugin, crashed on exactly that: F9 twice
within a second, and the process died with nothing further in the log. This
port has the same handler and the same plugin and had simply not been hit.

A press within 1.5 seconds of the previous one is now dropped, and the log
says "F9 ignored: the pack is still switching". Nobody compares textures at
that rate, so nothing is lost.

## v1.0.9 - 2026-09-11

### Fixed - the settings menu still forced the full redo v1.0.8 had fixed in the tool

v1.0.8 taught the texture tool to continue an interrupted run. The menu had
the same rule of its own: a pack whose record said "incomplete" greyed the
"Redo textures already in the pack" box ticked, and the button became
"Process all 22,026 textures", run without the only-missing flag. So the
first run after v1.0.8 was still a full redo; it was caught two minutes in
and stopped. The menu now forces a redo only when the pack was made with
other settings. An interrupted pack shows "the next run continues with the
textures still missing" and the button counts only those.

### Added - a settings path may be relative to the executable's folder

`texture_path`, `game_path` and `dlc_path` in `ng2_settings.cfg` are now taken
relative to the folder ng2.exe is in when they are not absolute. That is what
makes a portable install: one folder holding the game, the DLC, the profile
and saves, and the texture dump and pack, that keeps working when it is moved
to another drive or PC without a visit to the setup screen. A relative path
used to be resolved against the process's working directory, which is that
folder only when the game is launched from it.

## v1.0.8 - 2026-09-11

### Fixed - a texture run that stopped halfway redid every texture

A 4x AI run died 2,449 textures into the 9,418 it had left: the disk was full
("No space left on device", in the log). The pack's record then said the run
was incomplete, and the tool treated that exactly like a pack made with other
settings: the next run would redo every texture, all 22,026, and rewrite the
104 GB already made. Hours of work, thrown away for one interrupted write.

The completion flag no longer decides that. When the pack's scale, upscaler
and strength match the settings, an interrupted run continues with the
textures still missing. What counts as "already in the pack" is now a file
whose size matches its own header; the one file the failure cut short (a
header with no pixels behind it) is redone rather than handed to the game.
Checking 15,255 files takes two seconds. The settings menu says "the next run
continues with the textures still missing" instead of promising to redo
everything.

Worth knowing: a 4x pack is raw RGBA, about 7 MB per 1024x1024 texture, so
the full game at 4x runs to well over 100 GB. The menu's size estimate is
being revisited.

## v1.0.7 - 2026-09-11

### Fixed - Escape took three seconds to quit, with a black window

Escape released everything the port owns, asked the runtime to quit
gracefully, and then waited: that graceful path never returns on this title
(the guest's threads are fibers and it waits on something that never
finishes), so the process sat with a black window until the three-second
watchdog killed it. The window's close button never had the problem, because
the SDK's close path terminates the title and hard-exits at once.

Escape now takes that same path: it saves the settings, joins the port's own
threads, and then asks the window to close. The watchdog stays behind it as a
backstop and no longer fires. Measured from the "Escape: quitting" line to the
process being gone: 0.7 seconds, where it was 3.3. The Fable II port made this
change first and measured the same.

The exit is measurable rather than believed: `NG2_QUIT_AFTER=<seconds>` in the
environment fires Escape's own code from a timer, and `scratchpad/quit_test.ps1`
launches the build with it and times the process to its end.

## v1.0.6 - 2026-09-11

### Fixed - the red mist after a boss: the next chapter now loads on its own

Finishing a chapter ended in a red mist that never went away. The results,
rating and save screens all worked, and the game asked whether to continue
without saving, but Proceed did nothing.

The post-boss flow is a small state machine in the game's front end. After
Proceed it checks whether anything unlocked during the chapter is still
missing from the save, and if so it waits for the profile's achievement write
to report completion before it starts the next chapter. On the console that
write completes at once. In this port it was never issued at the chapter end,
so the game waited for ever. The state it waits on is now supplied at that
exact point, and the game runs its own transition unchanged: the loading
screen with its background, the "press to proceed" prompt, and the mist
clearing as the next chapter starts. The pending achievements are still
awarded by the game itself the next time the chapter loads, as before.

Every earlier workaround for this hang (forcing the loader from outside)
is superseded: those loaded the chapter but skipped the loading screen and
left a faint mist over the next chapter. The fix is `src/ng2_chapter_fix.cpp`
and can be switched off with `--ng2_chapter_award_fix=false`.

The instrumentation used to find this (function tracers, state dumps, the
`--ng2_chfix3` test switch and the synthetic auto-click) is gone from the
build: the recompiled sources were regenerated from the game and only the
fixes remain, so the game runs without the per-function trace check.

### Fixed - the release needed the Visual C++ runtime to be installed already

A standalone check of what the game loads found one thing outside Windows and
the release folder: the Visual C++ 2015-2022 runtime (msvcp140, vcruntime140,
vcruntime140_1 and msvcp140_atomic_wait). Windows does not ship it, so on a
machine without it the game would not start, with a "DLL not found" message
and nothing in the log. The four DLLs now ship in the release folder, copied
from the compiler's own redistributable set and listed in the provenance file
like everything else. Everything else the game loads is Windows itself, the
two runtime DLLs beside it, and the files you bring: the game and the DLC.


### Fixed - the pack could serve the wrong texture (shop windows rendered violet)

The GPU plugin filed each texture under an id built from its memory address,
format, size and pitch - nothing about its pixels. The game streams its
chapters through the same memory, so the glass of the shop windows at the
start of chapter 1 shared an id with a normal map from a later chapter that
had been dumped first, and the pack handed the normal map to the window: a
violet tint with bumps, which is a normal map drawn as colour. Any two
textures that ever land at the same address with the same shape could swap
this way, anywhere in the game.

Pack files now carry a content hash of the texture's own bytes in their name
(`<id>-<hash>.tex`), the plugin hashes what is actually in memory before it
opens anything, and a texture whose bytes do not match falls back to the
game's own art with one log line naming the id. The dump keeps both textures
of a shared address as two files. An existing pack is migrated in place the
next time the texture tool runs: every file is renamed from the raw dump, in
seconds, with nothing re-upscaled. Files whose raw dump was cleared cannot be
verified and are left out until their scene is dumped again; the game reports
how many at start-up.

The same plugin change is deployed to the Fable II project, which streams its
world the same way. Details: `docs/TEXTURE_PACK.md`, "Ids carry a content
hash".

### Fixed - "Dump while playing" did nothing until the next launch

The dump switch travelled to the GPU plugin only through the tuning file at
start-up, so ticking it mid-game, walking a chapter and coming back to the
settings found nothing written and "0 waiting". The plugin's dump settings
are now hot-reloadable and the settings screen applies them live, in the same
breath as switching the pack off, which drops every texture - so the scene in
front of the player is written out immediately.

### Fixed - the census could not see a second texture at the same address

"Process N waiting textures" counted by id, so a texture that shares an id
with one already in the pack read as packed and the button stayed at zero -
exactly the collision case the new dump exists to capture. The census now
counts id+hash, the same key the game loads by, and files the game would
ignore (no hash) count for nothing.

## v1.0.5 - 2026-09-09

### Fixed - the settings pointer never hid

"Hide the pointer after" set an idle delay but the cursor stayed on screen
forever. The window hides the pointer only while its visibility is in the
auto-hide mode; the code set the delay and left the mode at "always visible",
so the delay was dead configuration. Both places that apply the setting now
switch the window into auto-hide when a delay is chosen - and back to
always-visible when set to "never" - so the pointer fades after the chosen
idle time and returns on the next mouse movement.

### Fixed - the texture upscale died when the settings menu was closed

Starting a texture pack build and then closing the settings menu - which
tabbing out of it does - cancelled the run, leaving the only option to redo
every texture from scratch. The run was owned by the settings overlay, and its
destructor cancelled it. It now belongs to the application for the life of the
process: closing the menu leaves the run going, reopening the menu shows its
progress again, and only the Cancel button or quitting the game stops it.

### Changed - the enhanced-texture status no longer shows a false count

Turning enhanced textures on reported "N enhanced and M original textures
loaded right now" - a number that counted only the textures resident for the
current screen, so it ticked constantly and read as low as 57, as if only 57 of
the pack's thousands had ever been made. That per-frame count is gone. The
status now states plainly whether the pack is on and whether it is actually
replacing textures on screen; the true, stable totals ("N textures can be
enhanced, M in the pack") stay on the line above.

## v1.0.4 - 2026-09-06

### Fixed - crash at the chapter 13 boss (and one latent twin)

Reaching the chapter 13 boss crashed the game with a null-pointer write. To
make stray branches resolve, the recompiler had registered two functions'
epilogue fragments as standalone "_missed" functions (config/functions.toml).
That severs the parent's live non-volatile registers - r26/r27 for
sub_8372398C, r22/r31 for sub_83A566F8 - so they read as zero and the code
stored to guest address 0 (value 24, which byte-swaps to the 0x18000000 seen
in the crash dump). Both fragments now carry those registers across the split
through the shared CPU context. All 23 "_missed" fragments were audited; only
these two read a non-volatile register before writing it. A general fix, not
tied to any save.

### Still open - chapter 12 never hands over to chapter 13

The v1.0.3 note below claimed this fixed. It is not - that guard change was
necessary but not sufficient, and the mist still hangs after the chapter 12
boss. The advance chain is now fully mapped: a boss clear runs sub_82441A80,
whose flow writes sequencer request value 2, which loads the next chapter. It
fires correctly on other chapters and never starts for chapter 12. Xenia runs
the same game code and transitions fine, so this is a recompilation bug in the
chapter-12 clear path, still being tracked.

## v1.0.3 - 2026-09-06

### Fixed - chapter 12 never handed over to chapter 13

Kill the chapter 12 boss and the game stayed in the mist for good. It still
answered the pad, the frame rate held, nothing was deadlocked; the state that
starts chapter 13 simply never ran.

It was the port's own doing. The community "Chapter 12 crash workaround"
forces an early return at one site in the game, and its author warns that it
causes problems elsewhere. This port had replaced that blanket switch with a
guard that tests the pointer the site is about to use and only takes the early
return when the pointer cannot be read - except that inside chapter 12 it
still forced the return unconditionally, "scoped to where it belongs". At the
boss's death the site is called with a new pointer, a real object in mapped
memory, and the forced return threw away the work that starts the next
chapter. Seen live: the guard's own log line at 12:50:19 naming the pointer,
the pointer readable in the running process, and the game in the mist from
that second on.

The guard now tests the pointer in chapter 12 exactly as it does everywhere
else. A pointer that can be read is a pointer the game may use; one that
cannot still takes the early return, which is the case the workaround was
written for. The always-on cvar remains for anyone who wants the original
behaviour.

## v1.0.2 - 2026-09-06

### Fixed - the music died mid-session and never came back

Twenty to forty minutes into a session, often at the title screen as the
attract demo handed back to the menu, all sound stopped while the game played
on. It has been open since the first playable build.

Read live from a dead-silent game, with nothing attached to it: the game's two
audio worker threads were both spinning inside their own rendezvous barrier,
and the audio callback was waiting for a completion they would never signal.
The barrier is the game's: each worker marks its arrival in one byte of a
word and spins until the word matches the expected set, and whoever sees the
match clears the word so the others can leave. Two words alternate per round
so that a slow reader is not confused by the next round's arrivals - but a job
with an even number of rounds ends on the same word the next job starts on.
One worker completed the round, cleared the word, was handed the next job and
arrived on that same word again before the other worker had re-read it. The
other worker then saw a word that was neither the expected set nor zero, and
spun forever.

On the console the spinner re-reads within nanoseconds and always wins that
race. On a PC a spinning thread can be off the processor for milliseconds at
exactly the wrong moment, and it lost. The port now lets a worker that finds
its own arrival byte gone from the word take the exit it missed, which is
precisely what it would have done had it read the zero. A log line records
each rescue, so the race can still be seen happening; the music no longer
stops when it does.

### Changed - the texture counts say what can be enhanced, and what is waiting

"7,778 dumped, 5,907 in the pack" read as 1,871 textures missing. Nothing was
missing: the tool never packs the HUD, fonts, video frames and other non-art
it dumps, by design, and the menu was counting files rather than textures.

The Textures section now classifies the dump the same way the tool does and
reports the numbers that matter: how many textures can be enhanced, how many
of those are in the pack, and how many are waiting to be processed. The ones
that are never packed are mentioned in a note and left out of the count. When
every texture that can be enhanced is in the pack, it says so.

Below the counts it now also says whether the enhanced textures are actually
in use right now - "Enhanced textures: ON" with how many enhanced and how many
original textures are loaded, read from the renderer itself - so "is the pack
on?" is answered on the same screen that builds it.

### Added - processing only what is missing

"Process textures" redid the whole pack every time, half an hour with the AI,
to pick up the few textures dumped since. It now processes only the textures
that are not in the pack yet, and the button says how many: "Process 128
waiting textures". A "Redo textures already in the pack" box brings back the
full rebuild. The line under the button states exactly what a run will do and
with which scale, upscaler and detail strength.

The pack now records what it was made with, in `pack/pack.txt`, and the menu
compares that with the settings selected. When they differ - a different
scale, a different upscaler, a different detail strength - or when the last run
was stopped halfway, the next run redoes every texture and the menu says so
before you press the button. A pack made before this version has no record; it
is redone once and recorded.

## v1.0.1 - 2026-09-06

### Fixed - the whole picture stretched on any window that was not 16:9

Pick your monitor's native size on an ultrawide, or any resolution wider than
16:9, and the game filled it edge to edge - HUD, menus and all - with "Keep
aspect ratio" switched on and doing nothing. It had always done this; it went
unseen because every window until now was 1280x720.

The port told the game that the display was the window's size and shape. The
game renders 16:9 regardless, and the presenter only pillarboxes when the
game's idea of the display differs from the window's - so at 3840x1600 the
game was told the display was 2.4:1, the 16:9 frame "matched" it, and the
setting that would have added the bars never got a say.

The game is now told a 16:9 display whatever the window is: the largest 16:9
box that fits inside it. A 16:9 window is told exactly what it was before. An
ultrawide is told a display of its own height, and the picture is pillarboxed
with the setting on and stretched with it off, as the setting always promised.
The Resolution row's description said the game rendered at that size; it does
not, and the text now says what the row actually does.

One part is in the runtime: it treats a video mode equal to its default of
1280x720 as "not configured" and substitutes the window size, so a 1280x720
window on a 125% display was quietly being told 1024x576. A new runtime option,
`video_mode_explicit`, makes it take the value as set, and the port turns it on.

### Fixed - Cancel did not cancel the texture processing

Pressing Cancel set a flag nothing read, and the run carried on to the end.
Closing the game stopped it only by accident, some seconds later, when the
pipe it was writing to went away. The tool is a tree of processes - the Python
launcher starts the interpreter, which starts the upscaler - and stopping just
the first would have left the other two running.

The tree now lives in a Windows job object. Cancel terminates the job within a
moment, and so does closing the game while a run is going; the button reads
"Stopping..." until it has. Every texture is written whole, so whatever reached
the pack before the stop is usable, and the next run overwrites it.

### Fixed - the progress bar reached 100% and then started again from nothing

Processing has two steps - decode every dumped texture, then upscale the ones
that are art - and the script reported both on the same bar, so the first
step's 100% was followed by a bar at 0% with a time estimate of 228 minutes.
The estimate was wrong because it divided by the time since the button was
pressed, which included the whole of the first step.

The bar now says "Step 1 of 2" and "Step 2 of 2", names what each step is
doing, and times each step by itself. The second step of the run that showed
228 minutes was in fact going to take about 26.

The line "Real-ESRGAN not installed - using Lanczos instead" at the top of
every AI run is gone. It referred to a Python package the AI path does not use,
and read as the AI pass being skipped when it was about to run.

### Fixed - the AI upscaler produced garbled textures at 2x

Real-ESRGAN x4plus is a 4x network and nothing else. The pack tool asked the
executable for 2x directly, and it obliged by running the 4x network and
assembling the tiles into a 2x canvas: the result was the texture shifted,
repeated and mostly black. Measured on a starburst effect texture, the 2x
output's brightest pixel was 26 out of 255. The blend step then laid that
picture's edges over a plain resize of the original, so every AI-processed
texture carried ghost edges from a copy of itself in the wrong place.

The model now runs at its own 4x and the result is resized down to the scale
asked for, which is what Real-ESRGAN's reference script does. Verified on four
dump textures: every output is exactly 2x, none blank, and fine detail is up on
three of the four against a plain resize (the fourth is a noisy cloud the model
smoothed).

Any pack built with the AI option before this version has the fault in every
AI-processed texture. Run "Process textures" again to rebuild it.

## v1.0.0 - 2026-09-05

First public release.

### Added - a Browse button for the texture folder

The texture folder was the one folder on the setup screen you had to type by
hand; every other one had a picker. It now has a "Browse..." button beside the
field, using the same folder picker as the rest. Typing a path still works.

### Changed - every setting is editable in-game, with a red "restart required" note

Several display settings - resolution, monitor, frame rate, internal render
size, supersampling, accurate depth, fuzzy alpha, anisotropic filtering, the
texture cache - were greyed out once a game was running, because the window and
the guest video mode are built at startup and cannot be rebuilt underneath a
running game. Greyed out with no way to act on them, they read as broken.

They are now editable everywhere. Changing one while a game is running saves it
and applies it on the next launch, and the row says so: a red "restart required"
sits next to it, in place of the old amber "(restart)". Nothing is applied live
that cannot safely be - the live-apply path only ever touched a curated safe
subset, and none of these are in it, so the change is stored, not mis-applied.

### Fixed - the stuck-wait watchdog watched one wait path in five

The watchdog exists to name what a hung guest thread is waiting on. It
instrumented `NtWaitForSingleObjectEx` and nothing else, so
`KeWaitForSingleObject`, `KeWaitForMultipleObjects`,
`NtWaitForMultipleObjectsEx` and `NtSignalAndWaitForSingleObjectEx` were all
invisible - and so was `RtlEnterCriticalSection`, whose contended path waits on
a null timeout without going through any of them.

That mattered more than a missing feature, because its silence had already been
used as evidence. "The watchdog reports no stuck kernel waits" appears in the
notes for the Chapter 12 to 13 investigation as a ruled-out cause. It never
meant that. It meant no stuck waits on the one path that was watched.

A live capture settled it. When sound stopped mid-session, the game's audio
callback was sitting in `KeWaitForMultipleObjects` and had been for over half an
hour, burning no CPU and printing nothing.

All five entry points now claim their slot through a single scope guard, and the
guard is applied to `xeKeWaitForSingleObject` itself as well, so everything that
funnels through the primitive - the critical section, the reader/writer locks -
is covered by construction rather than by anyone remembering to add it. Nesting
is a no-op, so the outermost caller keeps the slot and the most specific reason
wins: a blocked critical section says `RtlEnterCriticalSection`, not
`KeWaitForSingleObject`.

Thresholds moved from 10 seconds to 30, with a second and final report at 300.
At 10 seconds a perfectly healthy title screen produced thirteen warnings a
minute - four worker threads parked on a job semaphore, mostly - and a warning
that is usually nothing is a warning people learn to skip. A thread that parks
on the same object twice is silent the second time: it was released and came
back, so it is working, and a deadlocked thread never gets a second wait.

The 300-second line reports a fact, not a verdict. It was going to say "this one
is stuck" until the running build showed two threads passing 300 seconds on a
single unbroken wait while the game held a clean 60 fps - some threads here just
park until the player acts. So it says what it observed and leaves the
conclusion to whoever reads it next to a frame counter.

### Fixed - `db16cyc` was translated to nothing

`db16cyc` is Xenon's spin-wait hint: it delays about 16 cycles and hands them to
the other SMT thread on the core. The recompiler emitted nothing for it, in
twelve spin loops in this title. The right x86 translation is `PAUSE`, which
hints the spin, drops its power and pipeline cost, and avoids the
memory-order-violation penalty when the loop exits, so that is what it emits
now - plus a real `yield()` every 1024 iterations, for a spin that has run long
enough to be a genuine wait.

This required rebuilding `rexglue.exe` itself. It is the codegen tool rather
than a runtime DLL, so an instruction-builder change only reaches the game after
the tool is rebuilt, deployed, the stale PCH cleared and all 1,109 generated
files regenerated. Deploying the runtime does not carry it.

**It is a correctness fix and not a fix for anything visible.** It was pursued as
a candidate cause of the Chapter 12 to 13 hang and does not explain it - the
starvation the theory needs cannot occur, because guest threads are not pinned
(`ignore_thread_affinities` defaults true) and float across all 32 logical
processors. `docs/XENIA_ISSUES.md` records the full reasoning, including what
ruled it out.

### Fixed - two graphics settings could never reach the plugin

"Fuzzy alpha test" and "Accurate depth" have been in the settings since v0.5.4
and, unless you had also overridden anisotropic filtering, neither of them did
anything at all.

A guard in `ng2_tuning.h` reading `if (s.anisotropic >= 0)` was written to gate
one line - the anisotropic override, which is only sent when the player has
actually chosen a level. Its closing brace ended up three settings too late, so
`use_fuzzy_alpha_epsilon` and both `depth_float24_*` cvars were emitted only
when that unrelated condition happened to be true. Anisotropic defaults to -1,
so on a default install the guard was false and those three settings were never
written to the TOML the plugin reads.

The tick-box moved, the file said the setting was on, and nothing changed. That
is worse than not offering the setting.

Fixed by giving the guard back the single line it was written for. A new
DELIVERY sweep in the census now walks `ng2_tuning.h`, tracks brace depth, and
fails if any cvar is emitted only under a condition that does not mention the
setting driving it. The sweep was verified by reintroducing this exact bug and
confirming it fails - a check that has only ever passed has not been tested.

### Changed - V-Sync states its consequence on the page

Turning V-Sync off here does not just tear. The runtime raises the guest's
vblank from 60 Hz to 1000 Hz when it is off, and this title advances its game
logic on vblank, so the game runs faster than it should. That was explained in
the setting's tooltip, while the "Frame rate" row directly above it puts the
same class of warning in plain sight whenever its value goes above 60.

Now V-Sync does too, when it is switched off. Same hazard, same treatment.

### Added - a census of Xenia's known issues for this title

`docs/XENIA_ISSUES.md` takes the eight labels on Xenia's compatibility issue for
`544307D5` and gives each one a verdict. Seven are defects; five are fixed or
handled outright, one is handled by default with the consequence surfaced, and
one - the NVIDIA alpha flicker workaround - is available, off by default, and
has not been observed on the hardware tested.

The subjects are the labels themselves rather than a list typed into the
document, so it cannot quietly stop covering something. It also records what the
census does *not* cover: Xenia's report stops at "loads, plays some sounds for a
moment and crashes," so every problem this port meets after that point is
absent from it by construction. A second table lists those, including the one
still open - the Chapter 12 to 13 hand-off, which has not been observed
completing.

### Fixed - the publication allowlist dropped one document and let another through

Two faults in `tools/stage_repo.py`, found by counting what it staged rather
than trusting it:

`docs/VECTOR_COVERAGE.txt` was never published. The docs rule allowed `.md` and
nothing else, so the write-up of the VMX128 v64-v127 investigation - the root
cause of the garbled video, and the most useful document in the folder - was
dropped by file extension rather than by any decision.

Allowing `.txt` then immediately staged `docs/texpack/sample_index.txt`, an
index of decoded game textures. The script's own docstring had always claimed
`docs/texpack` was excluded; nothing excluded it. Its two files simply happened
to be a `.png` (denied by extension) and a `.txt` (not allowlisted), so a
documented rule was really a coincidence of file naming, and any `.md` dropped
in that folder would have been published.

`texpack` is now denied by name, which is how the one other directory inside an
allowed tree (`tools/upscaler`) was already handled. Staging is 83 files and the
sweep is clean.

### Fixed - the attract demo no longer freezes the game

Leaving the title screen alone started the attract demo and the picture stopped
updating: the guest froze permanently while the presenter carried on, so what
you saw was a black screen at a steady frame rate.

The cause was in the SDK's command processor, and it is worth stating precisely
because the visible symptom named the wrong thing. `InitializeRingBuffer` reset
the GPU ring's READ pointer and left the WRITE pointer alone. Ninja Gaiden II
re-initialises the ring at every mode change - twice within a millisecond when
the attract demo starts - so the parser then compared a fresh read pointer
against a write pointer from the ring's previous life. Read was greater than
write, `RingBuffer::read_count()` reads that as a wrap, and the parser walked an
entire 32 KB ring of dead commands. The "impossible packet" in the log was the
victim, never the cause.

Both pointers are now reset, which is what CP_RB_RPTR and CP_RB_WPTR actually do
when CP_RB_BASE is programmed, and a ring epoch counter stops a re-initialisation
that lands mid-pass from writing the old read pointer back over the reset.

Measured across full attract cycles: 6 and 2 ring failures on the last two
pre-fix builds, 0 across 107 ring re-initialisations and 52 attract cycles after
it. The demo plays for its 88 seconds and the game returns to the title screen
at 60 fps.

**That is not the whole story, and the "0" above should not be read as "never
again".** Later testing on 2026-09-05 reproduced ring failures at the attract
demo on three separate runs, twice ending in a hard freeze - and with no
`InitializeRingBuffer` in the log at all, so whatever causes those is NOT the
re-initialisation race this section fixed. It is a second, still-unidentified
way for the parser to read something that was never a packet. What happens next
is understood, though: `ExecutePrimaryBuffer` "recovers" by moving the read
pointer up to the write pointer, which discards every packet it skipped -
including any interrupt the guest was waiting on. The guest then waits forever
for a signal that was thrown away while the GPU worker waits for commands that
never come, and the picture stops. See `docs/XENIA_ISSUES.md`.

### Added - one button for a bug report

"Copy diagnostics to a file", on both settings screens, gathers this session's
log, the settings and what the machine is into one text file, copies its path to
the clipboard and opens the folder. It contains no game data.

A report needs three files from two folders, and asking for that is asking for a
report with none of them. `NG2_DIAGNOSTICS=1` writes the same file during
startup, for the reports worth having most: the ones from a game that never
reaches a menu.

### Added - a Lodestone census that enumerates its own subjects

`tools/lodestone_census.py` takes its subjects from the artefact rather than
from a list: every field in `ng2_settings.h`, every cvar that mirrors one, every
script the C++ looks up by name. Each must be reachable, documented, or declared
in `tools/settings-ledger.json` with a reason. A setting added tomorrow cannot
quietly stop being covered.

It found three real defects on its first run, all of which are fixed below.

### Added - the texture pack is warmed per stage, not all at once

A 6 GB pack cannot live in a 4 GB texture cache, and loading it all up front
makes that worse rather than better: the cache evicts art it is about to need
again, which is why 1,290 textures once produced 2,900 uploads. One stage's
worth does fit.

So which textures a chapter uses is now recorded as you play - by observation,
against the chapter the game says is loading, keyed by the id the pack files are
already named by. Nothing is renamed and nothing is duplicated: a texture used
in six stages is listed six times in `pack/stages/chNN.txt`, not copied six
times. An existing pack needs no re-dump and no re-pack.

When a chapter loads, that stage's files are read off the render thread so the
first draw needing one is a warm read rather than a cold one. It reads and
discards - the target is the OS page cache, not our memory, since a second copy
would double the footprint of the thing being relieved.

A stage with no list behaves exactly as before, so a half-observed pack is never
worse than none, and the benefit arrives on the second visit to a stage.

The list is written every 64 new textures rather than only when the chapter
changes. It was the latter first, which meant a session ending in the stage it
was recording - quitting, or stopping for the night - silently threw away
everything it had observed.

### Fixed - audio could stop for the rest of the run, silently

Sound died mid-session and never came back: no error, the audio session still
ACTIVE, its peak flat at zero, and immune to alt-tabbing. Worst in Chapter 12,
which is the clue - at 28 fps it underruns far more often than anywhere else.
(The 28 fps was the test machine's graphics card being saturated by other
work at the time, not the port; the underruns were real all the same.)

The SDL driver's semaphore is a credit meaning "the guest may submit one more
frame". It was returned ONLY when a real frame had been consumed, so the first
time the guest was late the queue emptied, the callback played silence and
returned nothing, and the count reached zero with nothing queued. The audio
worker waits on exactly that semaphore to dispatch the guest's callback, so the
guest was never asked for audio again and could never refill the queue. Neither
side could leave that state.

The credit is now returned on an underrun too, which is precisely when another
frame is most wanted. Measured after the fix: 1,681 audible samples across a
full Chapter 12 run, in the exact conditions that killed it before.

### Fixed - opening the settings could kill the process

Pressing F10 while a video played crashed with no log entry. Windows recorded
what the log could not: an access violation in rexruntime at
`ImGui::TableNextRow`, dereferencing a null current table.

`RowStart()` calls `TableNextRow()`, which does not check. A control added to
the Textures page sat 113 lines after the `EndTable()` it needed, so drawing
that section read through a null pointer. The video was incidental - it made the
section slower to reach, not the crash more likely.

The whole file is now swept for the same shape: zero remaining.

### Fixed - the settings menu scanned two folders every frame

The dumped and packed counts walked their directories on the UI thread on every
frame the Textures section was open - about 14,800 entries once a pack had grown
to 4,125 files beside 10,669 dumps. They now run on a detached thread, refreshed
at most every five seconds. The counts are advisory; a value a few seconds stale
is worth far more than a stalled frame.

### Changed - dumping and the pack can no longer both be on

Together they put a file write and a stat on the GPU thread for every texture
the decoder creates, which starves the command stream. That was documented as a
warning nobody could see; it is now a state that cannot be selected, enforced in
the menu and again when a settings file is loaded.

### Changed - the upscaler is a choice, not a tick-box

"Enhance with AI" implied its alternative was no enhancement. It never was:
unticking it selected Lanczos, which is a perfectly good result and what the
tool falls back to. It is now a list - Lanczos, or Real-ESRGAN once downloaded -
and a setting carried from a machine that had the model cannot claim it on one
that does not.

### Fixed - the Chapter 12 workaround now applies only to Chapter 12

The community patch is documented as causing problems elsewhere, which is why it
could not simply be left on. The file the game opens for a chapter -
`s_chap_NN.ng2` - says which chapter is loading, so the workaround is scoped to
12 and released everywhere else. The first file of a burst is the answer, not
the last: a chapter load is followed immediately by an enumeration of every
other chapter file, and taking the last would have reported chapter 14 every
time, confidently.

### Added - three more guest functions the analyzer could not see

MSVC emits no `.pdata` for small helpers with no prologue, so one sitting in the
padding after a real function is absorbed into it and never becomes callable.
They are reachable only through function-pointer tables, so each is found only
by running the code that calls it. Deleting a corrupt system save sent the game
down a path that had never executed in this port's history, and it found two
immediately.

**Known issue, and it is not ours to fix cheaply:** the Chapter 12 to 13
transition does not progress. The game busy-waits on a flag at `0x84C39440`
which its own consumer never clears, and issues no I/O for the next chapter.
This is Xenia's long-standing `kernel-save-file-errors`, open since 2018.

### Fixed - the release shipped no tools, so the texture feature was dead

`TOOLS` in the packaging script was empty, so a release carried none of
`upscale_textures.py`, `ai_upscale.py` or `get_upscaler.py`. The app finds them
by name beside the executable, so in a shipped build every texture control
failed with "missing from this install" - a fault invisible in the source tree
and certain in the package. The ASSEMBLY sweep now checks the package for every
script the code asks for.

### Fixed - "Skip intro videos" could not be turned back off

Unticking it wrote video mode 1, the retired overlay mode, rather than 0. The
compiled-in default of the matching cvar was 1 as well, disagreeing with the
setting's own default of 0. The DEFAULTS sweep exists because of this.

### Fixed - "Skip chapter cinematics" could never fire

The synthetic pad, the arming, the disarm-on-real-input and the setting were all
wired correctly to a file that does not exist: it armed on "stryd" / "ng2stry",
and this title's per-chapter story data is `s_chap_NN.ng2`. Every part worked and
the feature did nothing. Checked against the disc this time, and against the boot
sequence, which opens none of them.

### Removed - the video conversion machinery

The port used to re-encode all 27 videos at install time and draw its own copies
over the game, because the guest's decoder produced garbage. That decoder was
fixed at the source in v0.5.0 - the recompiler was localising VMX128 registers
v64-v127 into zero-initialised locals, so the codec's inverse-transform kernels
read their permute control vectors as zero.

Everything built on the workaround is now gone: the overlay player, the
conversion driver, the `video/` folder, the ffmpeg search, and an install
message that told every player their videos would render "with washed-out
colour" - which stopped being true two versions ago.

## v0.5.4 - 2026-09-05

### Added - live CPU/GPU/VRAM readouts, on F8

Three independent switches (frame rate, GPU, video memory) shown top right, plus
the same figures as bars in the Textures section - next to the switches that
cause the cost, so toggling the pack with F9 shows the difference immediately
instead of being discovered as a stutter later.

Everything is measured for THIS PROCESS. On a machine that also runs other GPU
work - which this one does, to the tune of 24 GB - a system-wide number says
nothing about the game and would make the pack look expensive or cheap depending
on what else happened to be running. GPU load comes from Windows' PDH GPU Engine
counters, summed across this process's engines, so it is vendor-neutral: this
machine has both an NVIDIA card and an AMD integrated GPU, and the upscaler
happily ran on the wrong one. It shows "n/a" rather than a misleading 0%.

F8 shows and hides them; which of the three are shown is a separate setting, so
hiding the readouts does not lose the selection.

### Changed - supersampling goes to 8x, and it really does raise shadow detail

The cap was 6 (and 3 before that), both arbitrary; the plugin allows 8.

That this improves SHADOWS was measured, not assumed. A 360 game renders its
shadow map to EDRAM and RESOLVES it to a texture, and only resolved textures are
enlarged by `draw_resolution_scale`. Whether THIS title works that way is a
question about the game, so a probe logged what comes through: it resolves a
**512x512 k_24_8_FLOAT depth buffer** as a scaled resolve, which at 3x is
allocated at 1536x1536 - nine times the shadow pixels.

Also measured: on a card already holding 24 GB of other work, 8x is not
reachable. 4x exhausted video memory (185 failed upload buffers, 23 fps); 3x runs
clean at 60. The readouts above are how that is now visible rather than guessed.

### Added - accurate depth and fuzzy alpha

Two quality flags the plugin has and the menu never offered, found by
enumerating every GPU cvar with its description rather than grepping for the
words a menu would use.

* **Accurate depth** emulates the console's 24-bit float depth exactly instead of
  approximating it. Costs pixel-shader work, buys depth precision - which is
  where shadow acne and z-fighting come from.
* **Fuzzy alpha test** is described by the plugin itself as preventing
  "flickering on NVIDIA graphics cards". Off by default because it changes what
  the alpha test accepts, but worth trying when foliage or grates shimmer.

There is still NOTHING in the SDK for lighting or colour saturation; both are
guest shader work, and the presenter's output shaders ship as prebuilt bytecode
with no HLSL source to extend.

### Fixed - F9 no longer overwrites the saved texture-pack setting

F9 called Save(), so ending a session mid-comparison quietly wrote that state
back: the next launch started with the pack off while the menu still said on.
Startup itself was never broken - with the setting on, the tuning file carries
`texture_pack_path` and the plugin uploads replacements before anything is
pressed - but a comparison key should not be a decision. It toggles live and
changes nothing on disk; the checkbox is what persists, and both say so.

The Advanced button is now "Advanced (all cvars)" and warns that what it writes
to `ng2.toml` is applied BEFORE these settings, so a value set there silently
wins. That precedence is the obvious cause of a future "this setting does
nothing" and was undocumented.

### Added - AI upscaling, as an optional download

Real-ESRGAN, off by default and not shipped with the port. The settings screen
offers a "Download AI upscaler (43 MB)" button; the AI option stays disabled
until it succeeds, so the control is never present-but-dead.

**The standalone Vulkan build, not the Python package.** The usual route needs
torch matched to the card's CUDA version - gigabytes, and version-sensitive -
and this machine's Python already pins onnxruntime-gpu for another project, so
installing into it risked breaking that. The official `realesrgan-ncnn-vulkan`
release is one executable plus its models, runs on Vulkan with no CUDA matching
at all, and touches nothing outside its own folder. "Latest" means the newest
release that actually carries the asset: the current newest tag has no files
attached, so `/releases/latest` alone reports failure while the download plainly
exists.

**Detail is transferred, not cross-faded.** The brief was more detail, not
different art, and a straight blend cannot do that: Real-ESRGAN x4plus is
photo-trained and denoises hard - on a smoke texture it cut the mean brightness
from 11.3 to 6.7, about 40% darker, because faint wisps against black are
exactly what it treats as noise. So the model's output is high-pass filtered and
laid over a plain resize of the original. Tone, colour and brightness stay the
game's; only the fine detail is borrowed. Measured after the change: 11.4 at
every strength. A slider controls how much, defaulting to 0.75.

**Every output is checked.** Running 30 textures produced `vkQueueSubmit failed
-4` (device lost) partway through, after which the tool carried on, wrote a file
for every remaining texture and exited successfully - **16 of the 30 were
blank**. The same batch on an idle GPU still produced one blank. So work is done
in chunks with retries, each result is verified for size and for being not-blank,
and anything that still fails falls back to a plain resize. A texture that is
merely not enhanced is a far better outcome than a hole in the wall.

### Added - the memory settings are chosen from the hardware on first run

First launch reads the GPU and sets the texture cache size and upscale factor
from it; a "Detect from hardware" button re-runs it. It runs ONCE - the result
is recorded, so a value changed by hand afterwards is never overwritten.

Sized against the card's CAPACITY, not against free memory. DXGI's budget looked
like the better signal because it is supposed to reflect system pressure, but
measured on a 32 GB card with 24 GB held by other work it reported "budget
30.7 GB, in use 0.0 GB" while nvidia-smi showed 8.2 GB free: the budget is an
allowance, not a measurement. Availability is the wrong basis regardless - it
changes minute to minute, so a setting derived from it freezes whatever happened
to be running at first launch.

### Added - F9 switches the pack, with an on-screen indicator

F9 toggles the upscaled textures during play without opening a menu, and a green
panel appears top-left naming which set is on screen and how many of each are
loaded ("1290 enhanced loaded, 485 original"). It fades after a few seconds
rather than sitting there.

Both halves earn their place. Comparing a texture pack through a settings screen
barely works - by the time the overlay closes the eye has lost the detail it was
comparing - and once the difference is subtle, "which one am I looking at?" is
exactly the question that makes the comparison worthless. The counts come from
the GPU plugin through `REXCVAR_QUERY`, since the app is a separate DLL that the
plugin is not on the link line of.

The shortcut is named in the settings panel itself, not only in a tooltip.

Dumping and using the pack are NOT mutually exclusive, and the panel now says
so: the dump reads the GAME's textures, which a replacement never touches, so it
keeps collecting new ones while you play with the pack on.

### Changed - the pack format is raw, and the default upscale is 2x

Both from measurement, and both because the first version stuttered badly.

**PNG decoding cost ~16 ms per texture on the RENDER THREAD** - a full frame's
budget each, 12.2 seconds across 800 textures, surfacing as an 8-second frame
while a scene streamed in. It scaled with pixel count rather than file size,
which is what identified the decode rather than the I/O. The pack is now raw
RGBA behind a 16-byte header, read straight into the mapped upload buffer with
no decode and no intermediate allocation: **16.5 ms -> 1.0 ms**.

**4x was over budget.** Replacements are uncompressed, so a 4x texture costs
about 128x a DXT1 original; 1290 textures came to 5.9 GB against the plugin's
MAXIMUM soft cache limit of 4096 MB. The result was 2900 uploads for a
1290-texture pack and hitches of 1.5-2 seconds. Raising the cache could not fix
it, because the ceiling that evicts is the soft limit and it cannot go higher.

At 2x the same pack is 1.5 GB: p99 frame time 19 ms against 126-269 ms, and most
five-second windows now report zero hitches at a steady 60 fps. 4x and 8x are
still offered, with their cost stated in the menu rather than discovered later -
the ceiling here is memory, and memory grows.

The cache dropdown gains an 8 GB entry (the plugin's own maximum: soft 4096,
hard 8192) for anyone choosing 4x.

### Added - the texture pack is actually used, and can be toggled live

The last link. `texture_pack_path` was defined, written and exposed as a
checkbox, and nothing read it - so the control did nothing at all. Now:

* `CreateTexture` sizes the resource from the replacement PNG rather than the
  guest key. A 4x replacement is a different SIZE, not a different filling for
  the same container; the view's format follows, or D3D12 has a view that
  disagrees with its resource and only the debug layer would ever say so.
* the load path decodes the PNG and copies it straight in, skipping the compute
  shader that converts guest formats - a pack texture is already host-format.
* the SWIZZLE is deliberately left alone. It describes how the guest's channels
  map to what the shader expects, which is independent of how the texture is
  stored, and the offline tool decodes into that same order.
* upload buffers are retired by submission index. Freeing one while its copy is
  still queued corrupts the texture in a way that reads exactly like a decoder
  bug, and this project has already paid for that lesson twice.

**Toggling it takes effect immediately.** Textures are created once and kept, so
changing the path alone would only affect textures loaded afterwards - the scene
in front of you would not change, which makes comparing the pack against the
original nearly impossible. The plugin now requests a full texture-cache clear
when the path changes, performed at end of frame after the GPU is drained, so
the picture switches over within a frame or two.

One risk was measured rather than assumed: the pack key contains `base_page`, a
guest ADDRESS, so if textures loaded at different addresses between runs the
pack would silently match nothing. Two cold runs to the same screen shared **785
of 792 ids (99.1%)**, so the key holds.

Verified: 500+ replacements uploaded at 1024x512, 1024x1024 and 512x512, 60fps,
0 hitches, no ring-buffer errors. The pack covers 71-75% of the boot and title
screens.

### Added - frame-time statistics, so stutter is measurable

The diagnostic line reported an average and nothing else, over `GetTickCount64`,
whose ~15.6ms resolution cannot measure a 16.7ms frame even in principle. An
average is the wrong statistic for the thing people actually notice: a second
holding 59 good frames and one 200ms frame still reports "60 fps", and that one
frame IS the hitch.

It now reports p50, p99, the worst frame and a count of frames over twice the
median, from `steady_clock`:

```
59.4 fps (298 frames in 5.0s)  frame ms: p50 16.6  p99 19.5  worst 95.6  hitches 1
```

That line is from the first run after the change - a 95.6ms frame the old
counter would have reported as a healthy 59.4 fps.

### Changed - the texture cache can be raised, and it verifiably applies

The menu control existed; what was missing was any evidence it did anything.
Confirmed from the plugin's own cvar table (`NG2_DUMP_CVARS`) rather than from
the fact that the tuning file was written: at `texture_cache_mb = 2048` the
plugin holds `texture_cache_memory_limit_soft = 1024` (default 384) and
`_hard = 2048` (default 768). The startup log's "(now )" is not evidence either
way - GPU-plugin cvars are simply not registered yet when the tuning lines are
logged, which is also true of `texture_dump`, and that demonstrably works.

Whether it reduces stutter for this title is NOT established: the title screen
streams almost nothing, so the comparison has to be made during play, which is
what the hitch counter above is for. For scale: one level produced 1775 unique
textures and 199 MB of raw guest texture data, against a 384 MB default soft
limit.

`store_shaders` (true) and `d3d12_pipeline_creation_threads` (-1, auto) are
already at sensible defaults, and the shader cache is working - the log shows
177 shaders and 256 pipelines restored from storage in ~100ms.

## v0.5.3 - 2026-09-05

### Added - an application icon

A katana over the kanji for ninja in red. `tools/make_icon.py` draws it and
writes `resources/ng2.ico`; the `.rc` is generated from the VERSION file so the
Properties version block cannot drift from it.

Windows takes the icon from three places and none implies the others - Explorer
reads the exe's resource, the taskbar reads the WINDOW's icon, and grouping
follows the AppUserModelID. Only one turned out to be needed: SDL's window class
falls back to `EnumResourceNames(RT_GROUP_ICON)` over `GetModuleHandle(NULL)`,
which is `ng2.exe` and not the runtime DLL SDL is linked into, so the single
resource covers both. Verified on each path separately rather than assumed -
`ExtractAssociatedIcon` for the shell, `GetClassLongPtr(GCLP_HICON)` on the live
window for the taskbar.

The small sizes are drawn simplified rather than downsampled: at 16px a detailed
blade averages into a grey smudge. Two drawing errors were caught only by
rendering and looking - the handle was nearly as long as the blade (which reads
as a rifle), and the "detailed" blade was thinner than the simplified one, so the
256px icon had the faintest sword of the set.

### Fixed - "Quit Game" left a black screen; it now returns to the setup screen

The guest's own Quit calls `XamLoaderTerminateTitle`, which reaches
`KernelState::TerminateTitle`. That marks every guest thread, wakes the ones
blocked in a kernel wait, gives them 200ms, and then **deliberately leaves the
stragglers running** - force-killing a guest thread orphans whatever host lock
it holds.

NG2's tasks are fibers, so its main guest thread is essentially always inside
`SwitchToFiber` rather than a kernel wait. It never reaches
`XThread::CheckTitleTermination`, so it is always a straggler. The SDK quits the
app from a thread that `Wait()`s on exactly that thread, so the wait never
returned, the app never quit, and the window sat there with nothing left to draw
into it.

That also explains why the earlier `OnGuestThreadExit` attempt never fired: that
hook is called from that same wait, for the main thread only, and the main thread
is the one that does not exit. (`TerminateTitle` also clears
`terminating_title_` *before* the calling thread exits, so the flag the old code
tested was false by the time anything could read it - two independent reasons the
hook could not work.)

What is reliable is that `TerminateTitle` **erases every guest thread from the
kernel's map whether or not it actually stopped**. So the fix watches for the
main thread's id going missing: a stable, permanent signal rather than a race
against the 200ms drain. It requires having seen the thread present first, so
"not registered yet" can never be mistaken for "the title ended" - that mistake
would relaunch the app in a loop only a task manager could stop.

Quitting then returns the player to this port's setup screen, by relaunching with
`--ng2_setup` rather than tearing the runtime down: the guest's stragglers are
still running inside a runtime whose memory would be going away underneath them,
which is the fault the window-close path avoids by hard-exiting.

**No SDK change** - this is entirely in the port, so it works with the stock
DLLs the releases ship.

### Fixed - the video mode setting could never select 0

`ApplyLiveSettings` pushed `ng2_video_mode` as `s.video_mode == 2 ? "2" : "1"`,
which cannot produce 0 - so the one mode that matters was unreachable and the
setting did not mean what it said. It passes the value through now.

**A correction to an earlier version of this entry.** Making that honest exposed
a run that black-screened at the intro, and the default was changed to 1 with a
long note claiming mode 0 desynchronises the GPU command stream. That was wrong,
and wrong in the way that matters: the v0.5.0 codegen fix repaired the guest's
decoder - which is precisely why v0.5.2 deleted the converted videos and the
install step that produced them - so mode 0 is native playback and is the point
of that work. The same black screen appears under mode 1, and mode 0 reaches the
title screen normally once the real trigger is removed.

That trigger is **dumping and the texture pack running together**: each puts
file I/O on the GPU thread for every texture the video decoder creates, and the
ring buffer overflows while it waits. Either alone is fine. The default is 0
again, and the 0-to-1 migration written on the back of the bad diagnosis is
gone.

### Fixed - the game crashed ~90 seconds after every launch

Not the attract demo, and not the game: **a leftover diagnostic of ours**.

`OnPostSetup` started `StartOscillationLoop()` unconditionally, and its scan did
this every 12 seconds:

```cpp
const uint32_t lo = 0x00020000u, hi = 0x1F000000u;   // ~502 MB
auto* base = memory->TranslatePhysical<uint8_t*>(lo);
std::vector<uint8_t> snap(span);
std::memcpy(snap.data(), base, span);                // blind
```

`TranslatePhysical` is `base + offset` and validates nothing. The guest's
physical range is reserved, not committed, so the copy faulted on the first page
the host had not backed - killing the process about 90 seconds in, on **every**
launch, in v0.5.0, v0.5.1 and v0.5.2. The `[gprof]` lines flooding every log were
the other half of the same oversight.

It read as a game bug for a long time because the attract demo was on screen when
the timer elapsed, so the crash correlated perfectly with the video. Three
measurements said otherwise and were all consistent with a fixed-address host
read: it crashed identically with `texture_dump=0`, identically with
`video_mode=0`, and **every dump had byte-identical fault registers** -
`rsi=0x2_00020000`, which is exactly `TranslatePhysical(0x20000)`, the hardcoded
`lo` above. Constants, not corruption.

Two changes:

* the profiler and the scan loop are now behind `NG2_DIAG`, so they are opt-in
  rather than shipped hot. They are kept, not deleted - the oscillation scan is
  what caught the ledge bug - and F7 still runs a single scan.
* the scan copies only committed pages (`CopyCommitted`, via `VirtualQuery`) and
  never reads an uncommitted one, so using it cannot fault. This is the same
  lesson as reading a guest pointer without checking it is mapped, which crashed
  a live session earlier in this project.

Verified: 7 minutes past the attract demo, against a crash that reproduced 6 out
of 6 times at 96 seconds.

### Changed - texture dumping now refuses to dump things that are not art

The resident-memory load path also carries the video decoder's luma/chroma
planes, the scene resolve and the HDR buffer. Before this, a capture taken at the
menu produced a "pack" of five files, two of which were the framebuffer upscaled
to 5120x2880. There is now a format filter at the dump site and a
power-of-two/compression check in `tools/upscale_textures.py`, which reports a
count per rejection reason. See `docs/TEXTURE_PACK.md`.

## v0.5.2 - 2026-09-05

### Fixed - character stuck in a looping animation at a ledge (Chapter 5)

**This was a regression introduced by v0.5.0's video fix.** Leaving a platform
edge locked the character into an endless jump/bob animation - camera, menus and
frame rate all fine, only the player state machine stuck.

The video fix de-localised VMX128 registers **v64-v127** so the codec's
transform kernels could receive their `vperm` control vectors. That was too
broad, and it broke an invariant nobody had written down:

```
// bl 0x83954260      <- the __savevmx prologue helper
// bl 0x83957114      <- COMMENT ONLY. NO CODE IS EMITTED.
```

The recompiler recognises the `__savevmx`/`__restvmx` helpers and emits nothing
for them. That is safe **only while those registers are per-function locals**,
because then isolation is automatic and never restoring them cannot matter.
Sharing all 64 through the context made the skipped saves load-bearing:
`sub_836ECF20` uses `v116`-`v127` as scratch, never restores them, and is called
**fifteen times in a loop immediately before the animation update**.

Now only the registers that genuinely carry a value between functions are
shared. Found by scanning all 42,504 functions for one that *uses* a register
before writing it, with saves excluded (a save reads a value only to put it
back). Exactly five qualify:

| register | why |
|---|---|
| `v69`, `v72` | the video codec's inverse-transform kernels - the `vperm` controls |
| `v64`, `v65` | `sub_83A2B890`, same codec module |
| `v96` | `sub_8376EE38`, `sub_8376FA20`, `sub_8376FFB0` |

The other 59 are local again, so the skipped save helpers are harmless as
before. This is now a codegen option, `shared_vector_registers`, set in
`ng2_manifest.toml` - other titles need their own list.

### Added - external live analysis, no restarts

`tools/watch_guest.py` attaches to a **running** ng2.exe as a debugger, sets
hardware watchpoints on a guest address and reports the writing function with a
stack-scanned caller chain. `tools/dump_table_live.py` makes the running process
write out its 48k-entry guest->host function table via `CreateRemoteThread`.

Both exist because every in-process probe change relinks `ng2.exe`, and the
linker cannot replace a running executable - so each iteration cost the player
their session. This found the ledge bug without closing the game once.

Notes for reuse: guest memory is mapped at several host addresses, and a
hardware watchpoint is a HOST address, so watching the wrong alias catches
nothing. Never dereference a `TranslateVirtual` pointer without `VirtualQuery`
first - probing an uncommitted alias killed the game outright. In ctypes,
declare `argtypes` or 64-bit addresses are silently truncated to 32 bits.

## v0.5.1 - 2026-09-04

### Fixed - a crash in Chapter 5 on ten unregistered virtual thunks

Two play sessions died at the same place:

    [FATAL] Call to invalid or unregistered function at guest address 0x8375B108

`0x8375B108` is a two-instruction **C++ adjustor thunk** - `addi r3,r3,-16 ;
b <method>` - one of the `this`-pointer fixups a virtual call jumps through.
MSVC emits no `.pdata` for them, so the analyser absorbs them into whichever
real function precedes them and never registers them; a call that reaches one
then dies. It sits in a table of 17 such thunks, of which **15 were registered
and 2 were not**.

Rather than patch the two and wait for the next crash, the whole `.text`
section was swept for the pattern - `addi r3,r3,<negative>` followed by a `b`
whose target is an already-known function. That found **82 adjustor thunks, 10
of them unregistered**. All ten are now in `config/functions.toml`.

Worth recording: `add_function.py`'s own docstring says these are "only
discoverable by running". They are not - all ten were found statically, so
nine more crashes were not needed to find the rest.

One of the ten needed correcting by hand. `add_function.py` sizes a function by
scanning forward to the next `blr`, and for `0x837FAEDC` that walked through the
zero padding and across two unrelated functions, proposing **0x3DD4 - 15,828
bytes for two instructions**. Registering that span would have swallowed
everything inside it. It is now `0x8`, with a note. The other nine happened to
sit directly before a `blr` and came out correct, which is the only reason this
one was visible at all.

### Fixed - "Quit Game" left a black screen

The game's own quit calls `XamLoaderTerminateTitle`, which stops every guest
thread but does not close our window - so the app sat there with a live window
and nothing left to draw. Neither Escape's path nor the close button's runs,
because the request came from inside the guest, and the SDK has no
"title terminated" callback.

The port now watches guest threads exiting and checks the kernel's
`is_terminating_title()` flag, so a quit from inside the game shuts down the way
Escape does - settings saved, cheat worker joined, process gone.

(If what you saw was a *brief* black screen that then closed by itself, that is
a different thing and still present: the exit watchdog gives the runtime three
seconds to finish before forcing the process out, and the window is dead for
those three seconds.)

### Not fixed - the character can still get stuck jumping at a ledge

Reported in Chapter 5: stepping off a platform edge starts an endless jump loop.
**Still open.** Recorded here because two wrong answers were ruled out properly
and that is worth not repeating:

* **It is not the controller.** A stuck-input detector - which reports any
  device holding an identical non-neutral state for over two seconds - fired
  **zero** times across a full reproduction, and unplugging the second pad
  changed nothing.
* **It is not a known emulation quirk.** Xenia's compatibility entry for this
  title (544307D5) records only an early crash, nothing resembling this.

A merge change was written against the controller theory and has been
**reverted**: with the theory disproved it fixed nothing observed, and it would
have let a jittering device lock out a working pad. v0.5.1 therefore ships the
**stock SDK runtime** - the video fix lives in the codegen tool, not the runtime.

What is known: a guest profiler shows `0x8374F078` taking 28-39% of samples
during ordinary gameplay, so the loop lives inside normal game code rather than
spinning somewhere exotic.

## v0.5.0 - 2026-09-04

### Fixed - every video in the game, at the actual cause

Every pre-rendered video has been garbled since the port first ran. It is fixed,
and the fault was in the **recompiler**, not in the game, the video files, the
GPU plugin or the codec.

`BuilderContext::v()` turned VMX128 registers **v64-v127** into zero-initialised
local variables whenever `non_volatile_as_local` was set, with no check for
whether a function reads such a register before writing it. A function handed a
value in one of them read **zero**.

Exactly **seven** functions in the whole 42,504-function title do that, and all
seven are the video codec. Two are its inverse-transform kernels, which take
their `vperm` control vectors in `v69` and `v72`. Both arrived as zero, so both
permutes did the same thing, the 8x8 transpose duplicated one row over another,
every block's DC was split evenly between vertical coefficients 0 and 1, and a
flat block came out as a gradient. That is the whole reason videos were the only
thing in the game that looked wrong.

Localisation is now limited to **v14-v31**, which is what stops a callee
corrupting its caller and is still needed. v32-v63 are volatile and unaffected.

Verified against ground truth rather than by eye. A synthetic test video - a
static greyscale ramp whose every row is one flat value - makes the correct
output computable, so the decoder can be marked right or wrong instead of
argued about:

| | before | after |
|---|---|---|
| DC-only block decodes to | `281 271 211 161 80 30 -30 -40` | **flat, all 8 rows equal** |
| correlation with the source video | r = 0.18 - 0.25 | **r = 1.000** (mean 0.938) |
| pixels clipped to 0 | 3 - 10% | **0.0%** |
| pixels at 255 | 4 - 5% | **0.0%** |

Ground truth has exactly 0.0% of each, and now so do we.

### Removed - the entire video re-encoding pipeline

It existed only to work around the decoder, so it is gone:

* installing a game **no longer re-encodes** its 27 videos, and no longer
  writes ~340 MB of derived files
* releases **no longer ship `ffmpeg.exe`** (97 MB) or `prepare_videos.py`
* video replacement is off - the guest plays its own videos, and
  `video_mode` now defaults to 0

This also fixes the **Techniques menu** clips, which the old workaround could
never reach: they are a small inset inside a scrolling page, and the overlay
paints the whole viewport, so replacing one covered the scroll, the text and
the button prompt.

Measured bonus: over the same 165-second run, letting the guest play its own
videos produced **1** GPU ring-buffer hiccup where the overlay produced **2**.
The remaining one is the known pre-existing attract-demo (`demo1.wmv`)
transition; it is non-fatal and the game recovers.

## v0.4.5 - 2026-09-04

### Added - the port's own version, on screen

The title bar read `ng2 [rexglue-v0.10.0-Release]`, which is the **SDK's**
version and never changes - so there was nothing anywhere to tell v0.3.4 from
v0.4.4. That is not cosmetic: three bug reports in one afternoon turned out to be
an old executable still being run, and there was no way to see that from inside
the game.

It is now in three places, all of them reachable without opening a log:

* the **window title** - `Ninja Gaiden II  -  v0.4.5`
* the **setup screen**, under the heading
* the **F10 overlay's title bar**, because that is the one surface always
  reachable while playing

The number comes from the `VERSION` file at configure time rather than being
typed into the source, so it cannot drift from what the release script packages.
`VERSION` is registered as a CMake configure dependency, so bumping it re-runs
configuration instead of quietly baking the previous number into the next build -
which is exactly the sort of silent staleness this is meant to expose.

## v0.4.4 - 2026-09-04

### Fixed - pressing anything during a video also worked the menu behind it

In "replace" mode the guest is never given the video file, so its own playback
fails instantly and it moves straight on. That is the whole trick - but it means
that while the overlay is still showing the intro, the game is **already sitting
in the main menu behind the picture, taking input**. Press anything to skip the
video and the same press picks whatever the menu cursor happened to be on,
without you ever seeing the menu. Blind input into a screen that is not on
screen.

Guest input is now held back for exactly as long as the overlay is drawing, so
the press that stops a video is consumed by stopping it - which is what a skip
should do, and what it looked like it was doing all along.

The overlay's own skip is untouched: it reads ImGui's keyboard, which comes from
the window rather than through the guest's input system, so the two never fight.

The gate keeps a foreground check as well, because that is the other thing the
runtime's input-active callback is plainly for, and replacing it outright would
let a background window's keystrokes drive the game. It asks whether the
foreground window belongs to this process rather than comparing handles - the
SDK does not hand out the HWND, and the process answers the same question.

Verified: one press during the intro, and the game afterwards sits at the main
menu with NEW GAME merely highlighted rather than entered.

## v0.4.3 - 2026-09-04

### Changed - the monitor list shows each screen's resolution, and picking one uses it

The list named displays by size of their *work area*, which is the screen minus
the taskbar - so a 3840x2160 monitor was listed as "3840 x 2088" and did not look
like the 4K screen the person was trying to select. It shows the real resolution
now: `1: 3840 x 1600 (main)`, `2: 3840 x 2160`, `3: 3440 x 1440`.

Picking one also **sets the resolution to that screen**, because "play on
monitor 2" and "at monitor 2's size" are the same wish, and leaving a size chosen
for a different display behind is precisely how a 4K window ended up on a 1600p
screen. It is still free to be changed afterwards.

The "bigger than this screen" warning now fires only when the size exceeds the
monitor itself, not its work area. Otherwise it would fire every time a screen
was picked, on the one path that is working correctly - a window is always a
taskbar shorter than the display.

### Fixed - the installer forgot which disc image it used

`iso_path` was session-only, so the field came up blank next launch even though
the file was still sitting there. An empty box beside a 7 GB image that has not
moved reads as "the ISO is gone".

It is remembered now, and checked before being shown - a path to an image that
really has been moved is dropped rather than displayed as if it were still
there. Nothing is installed without pressing the button; this is only the field.

For the record: nothing in this port has ever deleted a disc image. The
installer opens one read-only and copies out of it.

## v0.4.2 - 2026-09-04

### Fixed - 4K could not be set, on any monitor

Nothing to do with the monitor setting. The runtime's `window_width` and
`window_height` are **logical** sizes - Windows multiplies them by the display's
scaling - and this machine runs three displays at three different scalings:

| display | size | scaling | logical work area |
|---|---|---|---|
| left | 3440x1440 | 100% | 3440x1392 |
| primary | 3840x1600 | 125% | 3072x1232 |
| 4K | 3840x2160 | 150% | 2560x1392 |

So asking for "3840" requested 3840 x 1.25 = 4800 physical pixels, which fits on
nothing here. The window overflowed and got centred, which looks exactly like a
window refusing to move. Measured before the fix: 3840x2160 requested came back
as a **4818x1972** window.

v0.4.0's clamp made it worse rather than better - it compared a logical request
against a physical work area, so a window that "fitted" was then scaled past the
edge of the screen anyway.

The menu still means physical pixels, because that is what "4K" means to a
person. The conversion happens once, on the way into the runtime.

**The scale to convert by is the PRIMARY display's, not the target's.** Measured:
converting with the 4K monitor's own 150% produced a 3218-pixel window on a
3840-pixel screen, because that scaling is never applied - this process is
System-DPI-aware, so Windows uses the primary's scaling wherever the window goes.

### Fixed - the monitor list was in the wrong order

v0.4.0 listed displays left to right. Wrong, and it took testing every index to
find out: Windows numbers these three 4K=1, 3440=2, primary=3, while the
runtime's own indices are 1=primary, 2=4K, 3=3440. Neither Windows' numbering
nor screen position matches - the runtime puts the **primary first and the rest
in enumeration order**, and its numbering is one-based with 0 meaning "wherever
Windows likes".

Each entry now lands where it says:

| entry | window | display |
|---|---|---|
| 1 | 3858x1587 at x=-9 | primary, fills 3840x1540 |
| 2 | 3858x2135 at x=3831 | **4K, fills 3840x2088** |
| 3 | 3458x1440 at x=-3449 | 3440x1440, fills 3440x1392 |

The list shows each display's size and scaling, so the right one is recognisable
without counting.

## v0.4.1 - 2026-09-04

### Added - two graphics levers the plugin already had

Both were read off the GPU plugin's own cvar dump rather than guessed at, which
also settles what is **not** there: `present_effect` allows only `bilinear`, so
there is no upscaling filter to offer, and `swap_post_effect` allows only
`none / fxaa / fxaa_extreme`, so there is no SMAA, TAA or true MSAA either. The
menu was not hiding those - the plugin does not have them.

**Supersampling now goes to 6x.** The plugin accepts up to 7 and says so when
asked for more; the menu stopped at 3 for no reason anyone had measured. It is
the sharpest setting available by a distance, because it renders the game's own
framebuffer larger and filters down rather than post-processing a finished
frame.

Measured at a 1920x1080 window: 1x, 2x, 3x, 4x and 6x each held a locked 60 fps.
That measurement is from the main menu, which is a light scene, so the tooltip
says as much - the high settings are offered as worth trying, not as free. The
cost goes with the square of the number: 4x is sixteen times the pixels.

**Texture cache size**, up to 4 GB. The plugin's own limits are 384 MB soft and
768 MB hard - console-era numbers on a card with 32 GB. Raising them means fewer
evictions and re-uploads, which shows up as fewer hitches while an area streams
in rather than as a sharper picture, and the tooltip says that rather than
implying otherwise. Left alone, nothing is written at all, so the plugin keeps
its own defaults instead of having them restated as if they were a choice.

Verified by readback rather than by assuming the deferral worked, which is the
step this project has been caught by before:

```
draw_resolution_scale_x           = 4
texture_cache_memory_limit_hard   = 2048
texture_cache_memory_limit_soft   = 1024
```

## v0.4.0 - 2026-09-04

### Fixed - the settings screen "went full screen" and lost its buttons

Nothing was missing. The window was 3840x2160 - chosen while the game was on a
4K monitor - and it reopened on the default screen, which is a 3840x1600
ultrawide. A window 560 pixels taller than the display puts its bottom edge
below the desk, and the bottom edge is where the footer and Play live.

Two things were wrong and neither works without the other:

* The runtime has always had a `monitor` cvar and **nothing was driving it**, so
  the window went wherever Windows put it - not necessarily the screen the size
  was chosen for. It is a setting now, shown only when there is more than one
  display, listing each with its size so the right one is obvious.
* The resolution list offers 4K and four ultrawide sizes and had no idea what
  was plugged in. The window is now clamped to the work area of the monitor it
  will actually open on, and the menu says so in red *before* you press Play
  rather than after.

Verified: asking for 3840x2160 on this 3840x1600 desktop now opens 3840x1540 -
the height minus the taskbar - instead of running off the bottom of the screen.

Clamping the window costs no image quality. The internal render scale is a
separate setting and is what supersampling actually uses, so 3x on a 1600p
screen still renders at 3x.

### Added - update an install in place

`python tools/make_release.py --update <folder>`

Every release so far has been a fresh folder with an empty `game\`, so the only
folder with 7 GB of disc rip and 340 MB of prepared videos in it stayed the one
installed first - and that is the one that kept being played. Three separate bug
reports in one afternoon ("invincibility came back", "save import disappeared",
"videos still stretched on ultrawide") were all a single cause: an old
executable, still being run because moving to a new release meant reinstalling
the disc.

`--update` writes this build over an existing install - executable, runtime, GPU
plugin, controller database, tools - and touches nothing the player put there:
`game\`, `dlc\`, `video\`, `user\` and `ng2_settings.cfg` are left alone. It
refuses a folder that does not already look like an install, so a mistyped path
cannot scatter an executable into somebody's documents. The release README now
says all of this too, which is where it should have said it from the start.

## v0.3.9 - 2026-09-04

### Fixed - v0.3.4 to v0.3.8 were built with no optimization at all

`ng2.exe` was 143.7 MB through v0.3.3 and 580.5 MB from v0.3.4 onwards. A 4x
jump, and nothing in those releases explains it: the executable is 576 MB of
`.text` and about 4 MB of everything else, so it is all code.

The cause was a poisoned CMake cache, and I poisoned it. The first build of the
day ran `cmake --build`, which triggered a reconfigure at a moment when clang
was not on PATH. The compiler check failed - and a failed compiler check leaves
CMake caching **empty** language flags. `CMAKE_CXX_FLAGS_RELEASE` became `""`
instead of `-O3 -DNDEBUG`, every configure afterwards reused the cached empty
value, and 42,504 recompiled functions were compiled at `-O0`. No error was ever
printed after that first one; the build was green every time.

Fixed by deleting the cache and reconfiguring in an environment that has BOTH
toolchains, which is the real lesson: the build needs vcvars **and** LLVM, and
without vcvars the failure is not "cannot link" but "cannot identify the
compiler", which fails quietly into a cache nobody looks at again. The build
script now sets up both every time and passes `rexglue_DIR`, so a wiped cache
rebuilds correctly from it.

Measured after the repair:

| | before | after |
|---|---|---|
| ng2.exe | 580.5 MB | **143.8 MB** |
| release zip | 177.7 MB | **~65 MB** |
| working set | 605 MB | **576 MB** |

Zero fatals, locked 60 fps. The working-set saving is modest because the guest's
own 512 MB dominates it; the real win is 436 MB less executable to page in, and
recompiled code that is actually optimised - which is what v0.3.3 and everything
before it shipped, so this is a return to the known-good configuration rather
than a new one.

## v0.3.8 - 2026-09-04

### Changed - import a folder of saves, not one file at a time

A save pack is a folder - nineteen files in the set this was built against - and
importing them one by one through a file dialog is nineteen trips through the
same dialog for no reason.

The picker asks for a folder now and takes every Ninja Gaiden II save in it,
searching subfolders too, so a pack that arrives with its files nested works
without anyone flattening it first. Anything that is not a save is skipped
silently: a save pack is a folder of mixed files, and the useful answer is how
many saves are in it, not a complaint per file. Pointing it at a single save
still works, because someone who does that has said what they meant clearly
enough.

The section shows the count and reads back the first few names, because "18
saves found" and "18 files that happen to be in that folder" look identical
until one of them is read out. One bad file does not stop the rest - a folder is
exactly where a stray or corrupt one turns up - and the log ends with a tally.

There is a Rescan beside it, for the same reason the game folder has one: a pack
unzipped after the folder was chosen is otherwise invisible until it is chosen
again, which is the trap the game folder's Rescan fell into in v0.3.7.

Verified end to end against all nineteen: `Save: 19 of 19 imported`, in about a
tenth of a second, every display name read correctly including the Japanese
ones.

## v0.3.7 - 2026-09-04

### Fixed - the install finished at 106%

The encoder is driven as a child process and its output is parsed to count
videos: a line printed flush left means a new video started, an indented line is
its result. But three of the lines the script prints flush left are not videos -
`ffmpeg: ...`, `27 video(s) -> ...` and the closing `total ...`. So a 27-video
job counted 30 done, and with the bar weighted 45% copying / 55% encoding that
is `0.45 + 30/27 x 0.55 = 1.061`. Exactly the number reported.

A line now counts only when its first word actually names a video, which every
per-video line does whether it goes on to say "already prepared", "(triptych:
...)" or nothing at all.

The displayed fraction is also clamped, and reads 100% the moment encoding
completes. That is a guarantee rather than an assumption about the arithmetic
above it: a progress bar that can show 106% is one nobody can trust again.

### Fixed - Rescan did not see freshly installed files

Adopting the install destination as the game folder sat *inside* the branch that
chains into video preparation. So an install with no preparation to do - which is
every install that v0.3.4 produced, since it shipped no encoder - finished with
the game folder still pointing wherever it pointed before. Rescan then dutifully
re-inspected the old path and found nothing, and the files only appeared after
choosing the folder by hand.

Adopting the destination is about the install, not about the videos. It now
happens whenever an install completes, once, and writes the settings.

### Fixed - videos were stretched on an ultrawide display

The overlay scaled every video to FILL the window, which is right on 16:9: the
footage is 948x580 (1.63) against 1.78, so filling crops about 8% off the top and
bottom - what a video player does - and pillarboxing a full-screen video for two
thin bars looks like a fault.

On an ultrawide it is badly wrong. Filling a 21:9 display means cropping 43% of
the picture's height, and 32:9 more than half: what reaches the screen is a slot
through the middle of the frame with the tops of heads and the floor gone.

Videos are now filled only while the shapes are close - within 15%, chosen to sit
above the 7.4% mismatch of a 16:9 screen and well below the 43% of the narrowest
ultrawide - and fitted with bars beyond that, which is the original framing and
the only honest way to show 16:9 footage on a 32:9 display.

### Known - true ultrawide rendering is not there

Measured, not assumed. Forcing the internal render size to 2560x1080 does make
the game render into a 21:9 buffer rather than being pillarboxed - but its 2D
layer does not follow: the chapter card is visibly stretched horizontally and the
opening credits sit left of centre instead of centred, because the game composes
its overlays in a fixed coordinate space and has no idea the frame got wider.

Whether the 3D field of view genuinely widens is still open - the runs above
reached the chapter card and the opening cinematic but not a playable 3D frame,
so there is nothing to report yet either way. Until there is, the internal size
stays where it is and is not offered in the menu.

What does work is an ultrawide window with **Keep aspect ratio** on: correct
geometry, full height, pillarboxed. That is the game's own framing shown
honestly rather than distorted to fill the panel.

## v0.3.6 - 2026-09-04

### Changed - one cheat, and it is the one that works

Invincibility and infinite Ninpo are gone. Both were live combat values, neither
is in the game's progress block - it does not hold a single float - so each
needed a memory search the player had to drive, and a settings menu is the wrong
place to ask someone to go hunting for an address. Listing them greyed out with
"not found yet" was honest but useless; removing them is better than either.

Infinite karma stays and is unaffected. It needs no address from anyone: it
finds the progress block by that block's own header signature and writes karma
at a known offset inside it, so it works on any save as soon as a chapter is in
play.

The whole search went with them - the scanner, the snapshot mode, the candidate
list, the frozen-value table, the adopted-address plumbing in the settings file.
`ng2_cheats` is a third of the size it was, and `ng2_settings.cfg` no longer
carries two addresses that nothing reads.

### Fixed - the setup-screen tick pushed Save off the window

Added in v0.3.5 to the footer, which is the fixed area below the scrolling body:
one more row there and the Advanced settings and Save settings buttons were
below the bottom edge of a 620-pixel window. It sits in the Content section now,
inside the body that scrolls, where it cannot crowd anything out.

Caught by photographing the window rather than by reading the code.

## v0.3.5 - 2026-09-04

The v0.3.4 install produced a game with broken videos, and the cause was
entirely in the packaging.

### Fixed - releases shipped without ffmpeg, so no install ever prepared videos

The build defaults to REPLACING the pre-rendered videos, the installer prepares
them by driving ffmpeg, and the release shipped `prepare_videos.py` **without
ffmpeg**. ffmpeg is not on PATH on a normal Windows machine, so preparation
found no encoder, did nothing, and the install finished with no `video/` folder
at all - invisibly, because the game simply falls back to its own broken
playback.

This is the same failure the release script's own docstring already warned about
for `prepare_videos.py`: "the first cut of v0.2.0 enabled that while shipping
neither the tool that makes them nor a folder to put them in." The lesson had
been learned and written down; it just was not enforced.

So it is enforced now. Tools are declared with a **required** flag and a missing
one stops the release instead of producing a quiet dud. ffmpeg is searched for
in `tools/` and in the build directory, because at 100 MB it lives with the
build rather than in the source tree.

### Fixed - three settings were read but never written

`video_mode`, `keyboard_control` and `cursor_hide_seconds` were parsed on load
and left out of `Save()`, so any change to one of them lasted until the next
save and then vanished. `video_mode` is what the new "Skip intro videos"
checkbox sets, and a setting that forgets itself is worse than no setting.

### Fixed - Escape did not close the game

It ran the shutdown and then sat there not responding. Measured: our own
teardown completes and logs it, and the runtime's graceful path never returns -
the guest's threads are fibers and something in that path waits forever. The
close button never hit it, because the SDK hard-exits on that path itself.

Escape now releases what this port owns - the cheat worker is **joined**, not
just asked to stop, because it writes guest memory and guest memory goes away
with the runtime - writes the settings, then gives the runtime three seconds and
exits anyway if it has not finished. Quitting cleanly means the settings are
written and our threads are joined, not that the process is entitled to hang.

Verified: `exited after 3.3s (rc=0)`.

### Changed - Cheats are ticks and nothing else

The value search is gone from the player's world: no F5-F9, no "Find a value
yourself", no candidate list. It was a tool in a settings menu - it asked
someone who wanted invincibility to first learn how to find it.

Infinite karma is unaffected and still works: it finds the game's progress block
by that block's own header signature, so it needs no address from anyone.

The search code itself is kept but is driven by an environment variable and
mentioned nowhere in the UI, because it is still how the remaining two cheats
get their addresses.

### Added - ultrawide resolutions

1440p, 21:9 (2560x1080, 3440x1440), 24:10 (3840x1600) and 32:9 (5120x1440).

The window really can be that shape, but Ninja Gaiden II is a 2008 console title
with a 16:9 HUD and no wider field of view to give: at 21:9 the picture is
either pillarboxed or stretched, and which one is the "Keep aspect ratio"
setting. Pillarboxed is the honest choice and is why that defaults to on.

### Added - Save settings, and a way back to the setup screen

A **Save settings** button on both the setup screen and the F10 overlay. Play
already saved, so this is for reassurance - but "did that take?" is a fair
question to have about a settings menu, and answering it costs one button.

And a **Show the setup screen at the next launch** tick in the overlay. Getting
back to that screen previously meant knowing to hold Shift while launching,
which is not something anyone discovers.

## v0.3.4 - 2026-09-04

Save import, a Cheats section, and the reading of an STFS package's own name -
which had to be got right before either of the other two could work.

### Added - import a saved game, from the setup screen

"Import a saved game" sits under Content. Pick an Xbox 360 save package and it
is copied into this profile, and the game lists it under LOAD GAME.

The import cannot happen on that screen: it runs before the runtime is built,
so there is no ContentManager to extract with and no signed-in profile to
import into. So the file is only *read* there - enough to name it and to reject
the wrong game early - and the import itself is queued and carried out in
`OnPostSetup`, which is after both exist and still before the guest looks for
saves.

Extraction reuses `ContentManager::InstallContent`, the same call that installs
the DLC. It always lands in the downloadable-content slot (XUID 0, content type
00000002) because that is what it is for; the move to
`<profile XUID>/544307D5/00000001/` is what turns it into a save, and the
content header written afterwards is what makes XAM enumerate it. Without that
header the save is invisible no matter where its bytes are.

The destination is *asked for* rather than assembled - `CreateContent` makes the
directory and `GetOpenPackagePath` names it - because `ContentManager` has no
accessor for its own root. That path is also how the root is discovered.

### Fixed - reading an STFS package's display name

The documented `XContentMetadata` layout puts the display name at 0x3ED. That
is wrong for these files: all nineteen carry it at **0x411**, big-endian, in one
of nine 128-character locale slots, and which slot is used varies - a Japanese
save fills slot 1 and leaves slot 0 empty, an English one fills slot 0. So the
first non-empty slot wins rather than a fixed one.

The offset is odd, which is the trap. Reading it as aligned 16-bit words gives
back text that reads convincingly as little-endian, and following that reading
produces mojibake for every save - which is exactly the wrong turn I took first.

### Fixed - the controller did nothing, and the game kept asking you to sign in

Two reports, one cause.

The runtime's default input policy is `SlotAssignment`: **device ordinal N feeds
guest user N**. Ninja Gaiden II is single-player - it polls user 0 and nothing
else. So a pad that does not happen to enumerate first lands on guest user 1,
where nobody is listening: the character does not move, and the profile the game
asks about belongs to a user index that does not exist. Hence a sign-in prompt
that cannot be satisfied, and a "continue without signing in" with no save
device behind it.

The corroboration is that I never saw any of it. Every test run here drove the
game from the keyboard, and with `mnk_mode` on the keyboard is a *synthetic*
device - and synthetic devices always feed user 0. Same build, different input
path, different symptom.

`SharedAssignment` is the SDK's own answer for this case: every device feeds
user 0, users 1 and up report not connected - "for single-player titles that
only ever poll user 0", in as many words. It is applied in `OnPostSetup`, which
is before the guest starts polling.

It asks for the concrete input system rather than assuming it, so a different
backend keeps the default instead of crashing. Verified in the log:
`Input: every controller drives guest user 0`.

### Added - Skip intro videos

A checkbox under Display. On, the opening sequence and the attract demos are
skipped entirely; off, they play through the port's own decode.

Chapter-loading videos are deliberately never skipped either way - the game
treats one of those failing to open as a bad disc and stops, which is the whole
reason replacement is an allow-list.

Videos were already skippable while playing, with Start, A, Space, Enter or a
mouse click; that is unchanged.

### Changed - the profile, saves and DLC live with the game

`user\` beside ng2.exe instead of `Documents
g2`. A copy of the folder is now
a copy of the install, saves included.

Whatever was at the old location is moved in on the first run rather than
orphaned - which mattered, since that is where the imported saves were.
Verified: both saves, the system save, their headers and all four DLC packages,
167 MB. The old path is not hard-coded; it is read from what the runtime had
already filled in, so this survives the SDK changing its default.

One thing that cannot be done: the gamertag. `UserProfile` has no setter for its
name or XUID and hardcodes `signin_state()` to 1, so the profile is whatever the
SDK makes it - permanently signed in, and not renameable from here.

### Added - a Cheats section, on F10

Three named cheats with a tick beside each:

| Cheat | State |
|---|---|
| Infinite karma | **Working.** Holds karma at 99,999,999 |
| Invincibility | Listed, greyed - address not found yet |
| Infinite Ninpo | Listed, greyed - address not found yet |

The first cut of this put the *search* in front of the player - a type, a
number and five buttons - which is a tool, not a setting: it asks someone who
wants invincibility to first learn how to find it. The search is still there,
folded into a "Find a value yourself" header, because it is how the next cheat
gets found and how one that breaks gets found again without a rebuild. It is
just not what the section opens with.

A cheat with no address behind it says so and cannot be ticked, rather than
offering a checkbox that quietly does nothing.

### How Infinite karma works without an address

Nothing publishes Ninja Gaiden II's memory layout, and a static recompilation
carries no symbols - so a hard-coded address was never available. Instead the
cheat finds what it needs by its own contents.

The game keeps progress in a block it writes to a save verbatim, and that block
opens with a header distinctive enough to search for:

```
00 00 78 80   00 00 00 06   00 00 00 00   01 23 45 67
```

a size, a version, and 0x01234567 as a magic. Karma sits **0x230 bytes in** -
found by intersecting eighteen saves whose point totals are written in their own
display names, and exact in sixteen of them. The three that disagree are the
saves where karma had been spent at a shop, which is the tell that 0x230 is the
*balance* and the display name is the *score*.

The cached address is re-checked against that signature every tick, so a block
that moves - a new chapter, a different save - is found again rather than
written over blindly. Verified in play: `Cheats: progress block at 0x85417A10`,
and the tick reads **Active**.

Health and Ninpo are not in that block - it does not contain a single float, so
they are live combat values with no anchor yet. That is why they are listed and
greyed instead of shipped broken.

### Fixed - the search crashed the game on its first run

```
Unhandled guest access violation: read of guest 0x40430000
```

`QueryProtect` was the wrong question. It answers "what protection would these
pages have", so a **reserved** page - address space claimed with nothing behind
it - answers "readable" while touching it faults. The right question is the
region's allocation *state*, which `QueryRegionInfo` carries.

Two changes, because one is not enough:

- **Walk regions, not pages.** `QueryRegionInfo` returns the size of the run
  with identical attributes, so a reserved hole of a quarter of a gigabyte costs
  one query instead of 65,536. That is also what makes a whole-address-space
  sweep quick enough to run from the UI thread.
- **Read through `ReadProcessMemory`, never a dereference.** Even a correctly
  committed page can be decommitted by the guest between the query and the read
  - the game is running while the search runs - and RPM returns false where a
  dereference would raise. Writes go through `WriteProcessMemory` for the same
  reason: the worker writes sixty times a second.

Re-verified: 6,831 blocks swept, no fault.

The trap worth naming separately: the guest is a PowerPC, so everything in its
memory is big-endian. Searching for 7,371,107 without swapping finds nothing at
all and looks exactly like searching for a value that is not there.

### Found - karma lives at 0x230 of the save payload

See above. It does not cheat anything by itself; what it gives is an exact
number to search for, which turns finding the live currency address from a hunt
into one search - and, in the end, into no search at all.

### Fixed - the system save's content header

`ng2sysd.dat.header` in the live profile held the ASCII hexdump of a
neighbouring file - a stray shell redirect of mine, not the game's doing. The
save data itself was intact, so only the enumeration metadata had to be put
back. The corrupt file is kept beside it as `.corrupt`.

## v0.3.3 - 2026-09-04

Three bugs, all found by trying to load a real save rather than by reading code.

### Fixed - loading a save raised "Disc Read Error"

Mine, and the log gave the whole chain inside 34 milliseconds:

```
[NtCreateFile] FAILED: path='game:__video_skipped__.wmv' -> 0xc000000f
XamShowDirtyDiscErrorUI called! user_index=0
```

Taking a video away from the game means failing its open, and I had assumed
that was universally safe because the intro and the attract demo shrug it off.
A chapter-loading video does not: `aurora*` failing to open is treated as a bad
disc and the game stops.

Replacement is now an **allow-list** - the intro and the demos, both played
through many times - and everything else keeps the game's own playback, which
is corrupted but works, until it has actually been tested. The alternative,
letting the guest decode while we draw over it, is still not available: that
desynchronises the GPU command stream.

Verified: the same run that produced the error now reaches the chapter load
with 0 disc errors, 0 faults and 0 ring buffer errors, and logs
`leaving aurora12.wmv to the game`.

### Fixed - videos were looked for in the wrong place

The video folder was resolved from the *settings* path, but `--game_data_root`
changes where the game actually loads from. A launch with that flag therefore
found no prepared videos, silently fell back to the guest's broken playback,
and produced ring buffer errors. It now follows the path the runtime really
mounted.

### Fixed - the d-pad could not be used from the keyboard

The runtime binds it to Shift+Arrow, and modifier combinations do not reach the
game at all: a bare Shift+Down does not move the main menu cursor, while the
unmodified left-stick keys do. Several of this title's menus - the save-slot
carousel among them - navigate with the d-pad, so on the default binding they
were unreachable without a controller. It is now on the plain arrow keys, which
were free (the left stick is on WASD).

### Save import

An Xbox 360 `CON` save package can be imported. The SDK's STFS extractor reads
it; what matters is where the result goes:

```
<user>/ng2/<profile xuid>/544307D5/00000001/<name>.dat/<name>.dat
<user>/ng2/<profile xuid>/544307D5/Headers/00000001/<name>.dat.header
```

A save is a *directory* named `<name>.dat` holding the inner file, plus a
328-byte header. Put there, LOAD GAME goes from greyed out to selectable and
the slot shows its real thumbnail.

## v0.3.2 - 2026-09-04

### The Chapter 12 guard is now properly verified

v0.3.1 reported that the guard "fires 0 times in normal play". That claim was
weaker than it sounded: a hook that is never reached also fires zero times, and
nothing had established that the patched site runs at all.

It does. Instrumenting it shows the site is called **over 500 times before
Chapter 1 is even playable**, carrying the same pointer - `0xF0AAD450` - every
time, which the guard correctly judged valid and left alone. So the original
measurement was real, and now it is falsifiable.

The instrumentation stays, in a quiet form: one line per *distinct* value seen
at `[0x84C23C48]`. In a full run to Chapter 1 combat that is exactly one line.
If Chapter 12 puts a different value there, the log names it - which is the
evidence for whether that crash is what this guard assumes it is.

## v0.3.1 - 2026-09-04

### The Chapter 12 workaround is automatic, and needs no setting

The community patch writes `li r30, 0` at `0x82834C78` unconditionally, which
is exactly why its author warns it breaks every chapter but the one it fixes:
it makes the function give up even when nothing was wrong.

Reading further into that function shows why it does not have to. `r30` is a
**pointer**, and a few instructions later the function dereferences it:

```
0x82834CD8  lbz  r10, 8(r30)
0x82834CE4  lwz  r10, 0x20(r30)
0x82834CEC  lwzx r11, r11, r10
```

So the crash is a bad-pointer dereference, and the fix does not need to know
which chapter is loading - which is fortunate, because the game gives no signal
that it is. The two fields the function reads are checked against mapped,
readable guest memory (`LookupHeap` + `QueryProtect`; `TranslateVirtual` cannot
be used for this, it is pure arithmetic and validates nothing). If they are
readable, nothing happens and the game behaves exactly as it always has. If
they are not, the read two instructions away would fault, so `r30` is zeroed
and the game takes its **own** early return - the same path it already takes
for a null pointer.

Because it only acts where the alternative is a crash, it is on by default and
there is nothing to ask the player. The Chapter 12 row is gone from the
settings for good rather than for want of a signal.

**Verified:** the guard fires **0 times** across a full run from boot into
Chapter 1 combat, with no faults and no ring buffer errors - so it is a no-op
in normal play, which is the half that could have broken something.

**Not verified:** that it fixes the Chapter 12 crash itself. Reaching Chapter 12
takes hours of play. If it fires there, it logs the pointer that caused it,
which is the evidence either way.

`ng2_chapter12_workaround` survives as a cvar meaning "force the early return
regardless", in case the guard ever proves too permissive.

## v0.3.0 - 2026-09-04

**Installing a disc image now leaves a game that is ready to play.** Choosing an
ISO and pressing Install extracts it and converts its videos in one operation,
behind one progress bar, with no console windows and nothing left to do
afterwards.

### Fixed - the intro had stopped playing

A regression, and mine. With no prepared videos the port took the game's own
video away and put nothing in its place, so a fresh install simply had no
intro and said nothing about why. The changelog claimed it fell back to the
game's own playback; the code never did. It does now: the guest only loses a
video when there is a replacement ready for it.

That is also why it looked fine here and broke for you - this machine had
prepared videos sitting beside the executable, and a fresh release does not.

### Added

- **Videos are prepared as part of installing.** All 27 of them, from your own
  copy, in about 80 seconds after the copy finishes.
- **They live with the game data**, in `<game>/video`, so an install can be put
  anywhere and its videos are found again without being asked where they went.
- **Escape quits.** It is no longer one of the keys that skips a video, so it
  cannot mean two things at once.

### Changed

- **One progress bar for the whole install**, under the destination, weighted
  across copying and encoding - the second bar in the footer is gone. It
  reaching the end means the game is ready, which is the only thing the number
  needed to say.
- **No console windows.** The conversion ran through `_popen`, which gives the
  child a console; so did each of the three `where` probes for Python and
  ffmpeg, before the setup screen had even drawn. All of it now goes through
  `CreateProcess` with `CREATE_NO_WINDOW`.
- **ffmpeg is looked for beside the game**, not just on PATH - it is not on
  PATH on a normal Windows machine, including the one this was built on - and
  when it is missing the screen says where to put it instead of quietly
  skipping the step.
- **The Videos setting is gone.** Decoding them ourselves is simply what the
  port does; the alternatives were a corrupted picture and no picture. It
  remains in `ng2_settings.cfg` for anyone who wants one back.
- **The Chapter 12 workaround is gone from the settings too** - see below.

### Chapter 12 cannot be automated, and here is why

The workaround can only be on for the one chapter it fixes, because its author
warns it breaks the others. Turning it on automatically needs the game to say
which chapter is loading, and it does not: all fourteen `s_chap_NN.ng2` files
are opened together in a single burst, the same burst every run, and the only
other thing a chapter load touches is `aurora12.wmv` - the same file for
chapter 1. Five approaches were tried, including two scans for the code that
indexes the chapter filename table and a search for a vertical alignment
signal; none found anything that distinguishes chapter 12.

It is therefore no longer a question the menu asks. It stays as the
`ng2_chapter12_workaround` cvar, on the F4 screen and in `ng2_settings.cfg`.
The realistic route to automating it is to reach chapter 12 once and read the
crash - which is exactly how the five missing functions before it were fixed.

## v0.2.2 - 2026-09-04

### The intro reads as one picture

The middle of the intro measured about 8% brighter than the panels either side
of it, which is why it still read as three sections after the joins themselves
had been matched.

It is **not** a per-panel encoding difference, and that matters for how it had
to be fixed. Measured across the width, the brightness rises and falls smoothly
and peaks at the middle of the middle panel, and the step across each join is
under 1.3 out of 255 - the signature of a vignette in the source, not a
mismatch between three files. Correcting the middle panel on its own would
therefore have put a step back at both joins, which is the very thing that
makes the sections visible.

So the whole horizontal profile is levelled instead, from one correction curve
measured over the entire video (measuring per frame would make the correction
follow the action and flicker):

| | before | after |
|---|---|---|
| panel means, left / centre / right | 86.6 / 95.4 / 88.9 | 90.7 / 89.3 / 89.6 |
| centre against the two sides | +7.7 | -0.8 |
| step at the two joins | -0.04 / -1.21 | -1.07 / +0.29 |

On by default, since it alters nothing else; `--no-flatten` keeps the picture
exactly as it was shot.

### Also ruled out

A vertical misalignment at the first join, which would have explained why it
correlates 0.881 against 0.997 for the second. A search over vertical shifts
found no optimum - the three best offsets score within 0.002 of each other - so
there is no alignment to correct, and the difference is in the source.

## v0.2.1 - 2026-09-04

**Every video in the game is handled, not just the intro.** All 29 source
videos convert (the three intro panels merge into one, so 27 files), in about
34 seconds, for 340 MB.

| | |
|---|---|
| intro | 1 triptych, 960x580 |
| chapter loading | `aurora07`-`aurora12`, 320x240, 30-47 s each |
| attract demo | `demo1`, 960x540, 88 s |
| ending | `ending`, 960x540, 233 s - 201 MB on its own |
| ghost / tutorial | 18 clips, 320x240, 3-12 s each |

### The attract demo now plays instead of being skipped

It was excepted from replacement because it reliably desynchronised the GPU
command stream. That reasoning belonged to the first design, where the guest
kept playing its own copy underneath; since v0.2.0 the guest is denied the file
entirely, so a demo never reaches the broken decoder and there is nothing left
to except. Measured over a 155-second run that reaches it: the full 88 seconds
plays, with **0 ring buffer failures and 0 out-of-bounds register writes**,
against 3 and 253 before.

### Fixed

- **Videos are kept at their own size.** The height defaulted to 580, which
  upscaled the 320x240 clips - most of them - to 773x580: larger files with no
  more detail in them.
- **A literal NUL byte in `src/patch_hooks.cpp`**, where `'\0'` was meant. An
  earlier edit ate the backslash. It compiled to the same value, which is why
  it went unnoticed, but the file read as binary to every tool that touched it.
- The panel edge trim is off by default. It was added on the theory that the
  outermost column was an encoder artifact; it is not, and cropping removed
  real content at the joins - column correlation across the first seam was
  0.830 with a 2 px trim against 0.997 across the second without one.

### On the intro seams

The three panels are one continuous image, proved by column correlation:
adjacent columns inside a panel score 1.000 and across the second join 0.997,
so the stacking order is right. Per-frame level matching took the
column-to-column step at the joins from 3.30 to 0.55. What remains is not
correctable: the panels differ by about 28/255 in mean brightness, and that
barely moves under whole-panel correction because it is genuine content - one
wide shot is brighter in the middle than at its edges.

### Note

`ending.ng2v` is 201 MB by itself. `prepare_videos.py --quality` (2 best, 31
worst) and `--fps` trade size against fidelity if that matters.

## v0.2.0 - 2026-09-04

**The intro video plays correctly.** The game's own GPU decode path is broken
in the SDK's Xenos plugin and cannot be fixed from here, so the port decodes
the video itself and draws it instead.

### How it works

`tools/prepare_videos.py` transcodes the game's `.wmv` files once, at install
time, into `.ng2v` - JPEG frames with an index. The intro is three separate
videos (`NinjaVI_Left`, `NinjaVI`, `NinjaVI_Right`) which the game composites
side by side; they are stacked into one 948x580 image. 601 frames, ~32 MB.
At runtime the guest's open is failed and the overlay plays the prepared file.

### Four things this cost, each found by measuring

- **The SDK's `DecodeImageRGBA` cannot decode JPEG.** It is built
  `STBI_ONLY_PNG` - re:Blue's copy of the same header shows the flag - and
  handed a valid 64 KB JPEG it returns 0x0 and an empty buffer, silently. The
  overlay ran perfectly and drew nothing for three builds. stb_image is now
  vendored in `src/third_party/` with JPEG enabled.
- **Drawing over the game's own playback destroys its rendering.** The first
  design let the guest keep playing (for its audio and timing) and drew on
  top. Creating a texture per frame while the guest decodes video
  desynchronises the GPU command stream: 2 ring buffer failures and a wrecked
  picture, against 0 for the identical run with texture creation disabled. The
  guest is now denied the file entirely.
- **A full-screen ImGui window is not composited during guest rendering.**
  `Begin()` returned true and `AddImage` ran every frame, and nothing
  appeared - not even an opaque black background. Drawn through
  `GetForegroundDrawList()` instead, which needs no window.
- **The panel seams are per-frame, not a fixed level difference.** The mean
  brightness step at each join was +1.1 and -2.0 out of 255, but the standard
  deviation was 4.9 and 6.5 - four times the mean - so a constant correction
  would have been fitting noise. Each frame is now measured at the seam and
  corrected, ramped to nothing over 48 pixels. Column-to-column step at the
  two joins: **3.08 and 2.09 before, 0.80 and 0.18 after**, against a median
  of 0.216 for the picture as a whole.

### Also

- The video **fills the screen** rather than fitting inside it: 948x580 in a
  16:9 window pillarboxed, and black bars on a full-screen video look like a
  fault. About 8% is cropped top and bottom instead.
- It **fades to black** over the last 0.9 s, so the cut to the menu is a
  transition rather than a jump.
- The Videos setting is now three-way: Original, Replace, Skip.

### Known

- The game's own **audio and exact timing are lost** for a replaced video: a
  failed open makes the game move on immediately, which is the price of not
  letting it near the broken decoder. Audio for the overlay is not implemented.
- Only the intro is prepared by default. `prepare_videos.py` will do every
  `.wmv` in the game folder.

## v0.1.5 - 2026-09-04

Quality-of-life borrowed from re:Blue's settings menu, which is further along
than this one and worth reading rather than reinventing.

### Added

- **Quality preset** - Performance / Balanced / Quality / Maximum, moving
  supersampling, antialiasing and texture filtering together. "Custom" is a
  state the row can display but not select, which is how re:Blue does it and
  the only way a preset row cannot lie about what the rows under it say.
- **Keyboard and mouse control.** The runtime ships the driver and leaves it
  off, so without this only a real controller works - a surprising default for
  a PC port. Verified with the setting alone and no command-line flag: Enter
  opens the main menu.
- **Hide the pointer after N seconds** (0 keeps it), the same comfort setting
  re:Blue has.

### Changed

- **Workarounds are their own group**, apart from the quality settings, and
  say so: "Each of these works around a specific fault... they are not
  improvements." Skip videos and the Chapter 12 crash patch live there now.
- Settings rows are drawn tighter than the rest of the UI. At the global
  spacing the list ran two rows past the bottom of a 720p window, and the
  choice was between dropping settings and reclaiming ten pixels a row.

### Not brought over from re:Blue

Its renderer is its own (`config.graphics = nullptr`, a full `src/gpu/`), so
its MSAA/SSAA/backend rows have no counterpart here - ours are cvars belonging
to `rexgpu-xenos.dll`, which re:Blue never loads. Its controller icon sets,
per-save setting scoping and update channel are equally specific to that
project.

## v0.1.4 - 2026-09-04

### Added

- **Skip videos**, in the settings. The pre-rendered videos go through a decode
  path in the GPU plugin that renders them corrupted, and the attract-mode demo
  is worse than cosmetic: it desynchronises the GPU command stream, and the
  picture freezes at half rate while the game carries on running underneath.
  With videos skipped the game's file wrapper is pointed at a path that cannot
  resolve, the game takes its own missing-file path, and a two-minute sit on
  the title screen produces **0 ring buffer errors and 0 out-of-bounds register
  writes**, against 3 and 253 before.

  This is a workaround, not a fix. The decoder fault is inside
  rexgpu-xenos.dll and needs plugin source, or the guest video path hooked and
  decoded host-side.

### Findings

- **Mission Mode needs the game's title update**, which is why it has no menu
  entry. Ruled out first: a full `license_mask` (0xFFFFFFFF) changes nothing,
  and neither does installing the DLC under the signed-in profile's XUID
  instead of 0 - both were tested. The three costume packages work; the base
  game has no Mission Mode entry without the update. For a static
  recompilation a title update is not a mount, it is a different executable:
  the `.xexp` has to be applied and codegen re-run, and `rexglue` cannot apply
  one.
- **Bloom is located but not reachable.** `g_fBloomMod` and `g_fBloomSub` are
  pixel-shader constants c16 and c15, decoded from the `ps_3_0` constant table
  at `0x821B2990`. Nothing in the image references that shader - no stored
  pointer, and no `lis`/`addi` pair in 24 MB of `.text` - so the code that
  uploads those constants cannot be found from the names. Xenia has no bloom
  patch for this title either; its patch file has exactly two entries.

### Documentation

- The README's "Known gaps" section was three-quarters wrong: it still claimed
  setjmp/longjmp were unset (that was the breakthrough fix), that the
  `0x83819200` branch was unresolved, and that DLC was not wired up. Rewritten
  against what the project actually does now.

## v0.1.3 - 2026-09-04

**Gameplay is verified.** New Game runs through the Chapter 1 cinematic into
playable combat on the Sky City Tokyo rooftop - health bar, tutorial prompts,
the Tokyo skyline - at a locked 60 fps with no ring buffer errors and no faults.

### Added

- **Chapter 12 crash workaround**, in the settings. It is the community patch
  by Gliniak from Xenia's patch file for this title, which forces an early
  return at `0x82834C78` (`lwz r30, 0x30(r11)` feeds a `cmplwi`/`bne` that
  returns when zero, so zeroing r30 takes that path). Off by default, and the
  menu repeats its author's warning: it causes problems in every other chapter,
  so turn it on only if Chapter 12 stops you.

  That file also confirms the 1280x720 patch addresses this project already
  implements, arrived at independently.

### Bloom

Asked for, and not added, because it cannot yet be made to work honestly:

- There is **no community bloom patch** - Xenia's patch file for this title
  contains exactly two entries, the resolution patch and the Chapter 12
  workaround.
- The game's own bloom parameters were located: `g_fBloomMod` and
  `g_fBloomSub` are pixel-shader float constants **c16 and c15** of a `ps_3_0`
  shader whose constant table sits in `.rdata` at `0x821B2990`, decoded from
  the CTAB header there.
- But nothing in the image points at that shader: no stored pointer into the
  blob, and no `lis`/`addi` pair in 24 MB of `.text` that builds its address.
  The game reaches it through a shader table it indexes, so the code that
  uploads c15/c16 is not findable from the constant names.

Finding the upload site is a real reverse-engineering job rather than a patch
with a known address. A bloom slider is possible; it is not close.

## v0.1.2 - 2026-09-03

**The game now starts.** New Game reaches Chapter 1, Sky City Tokyo, and plays
its opening cinematic in-engine with a clean command stream - no ring buffer
errors, no out-of-bounds registers, no faults.

Getting there was four more absorbed functions, each found by crashing into it
and each the same shape: a small helper with no prologue, sitting after another
function's final instruction, that `.pdata` had swallowed into its neighbour so
nothing could call it.

| address | what it is | where it crashed |
|---|---|---|
| `0x836D7C08` | delegate trampoline | selecting a DLC costume |
| `0x836D7BF8` | adjustor thunk beside it | (registered pre-emptively) |
| `0x824356D0` | one-line setter | starting a new game |
| `0x83682F30` | one-instruction jump thunk (`b`) | pressing Proceed |
| `0x82348688` | small setter | loading the stage |

### Two things worth knowing for the next one

- **`tools/add_function.py` sizes a lone `b` thunk wrong.** For `0x83682F30`,
  a single `b 0x83683358`, it reported `0x804`: its size heuristic does not
  stop at an unconditional branch, so it ran through the padding and the whole
  of the next function, which it would have swallowed. The correct size is 4.
  Always check the size it prints against the disassembly.
- **A bulk scanner for these still does not work.** A second approach was
  tried - find blocks that follow a non-falling-through instruction inside a
  `.pdata` range and are never branched to from within it - and it failed its
  own known-answer test: 11,822 candidates, and it did not find the address
  that had just crashed, because that one is a single instruction and the scan
  required at least two. Computed jumps (jump tables) make the "not branched
  to from within" test unsound. The runtime remains the only reliable oracle.

## v0.1.1 - 2026-09-03

### Fixed

- **Selecting a DLC costume crashed the game.** It died with
  `[FATAL] Call to invalid or unregistered function at guest address
  0x836D7C08` once the costume sprites and message files had loaded. The DLC
  itself was never the problem - it installs and its files open fine. The
  address is a delegate trampoline (swap in a new `this` from `[this+0x10]`,
  tail-call `[this+0xC]`) sitting in the padding after another function's
  `blr`, so `.pdata` had swallowed it and no call could reach it. Registered
  it, and its identical neighbour at `0x836D7BF8`, in `config/functions.toml`.

### Settings

- **The settings menu is now one screen.** Four tabs became a single page:
  display and enhancements on the left, game data and DLC on the right, with
  every row visible at once on a 720p window. Explanations moved into hover
  markers so the settings themselves are not pushed below the fold; the two
  that describe a genuine surprise - what V-Sync and a refresh rate above 60
  do to a game that paces itself off the display - stay on the page as text.
- **Antialiasing** is new, and real: `swap_post_effect` declares
  none / fxaa / fxaa_extreme, and FXAA Extreme measurably reduces edge energy
  by 12% on the title screen. It applies to the next frame, so it is one of
  the few enhancement rows that is live in the in-game overlay.
- **Resolution** is now the three sizes worth having - Original 1280x720,
  Full HD, 4K - rather than a list of six. A config with some other size is
  shown as "Custom" instead of being snapped silently to 720p.
- **Frame rate** is a choice of 30 / 60 / 120 / 144.
- The disc-image destination row only appears once an image is chosen.

### Not added, and why

- **Upscaling filter.** The presenter declares exactly one value for
  `present_effect`: bilinear. There is no FSR or CAS in this runtime build, so
  there is no choice to offer. Supersampling is the quality lever that does
  exist.
- **Texture replacement.** The GPU plugin has no texture-mod facility at all -
  no dump, no load, no replacement path. A control for it would do nothing.

## v0.1.0 - 2026-09-03

First packaged build. Ninja Gaiden II boots from your own disc and reaches the
title screen, rendering correctly at a vsync-locked 60 fps, with a settings
menu in front of it.

### The recompilation

- Static recompilation of the retail XEX through the ReXGlue SDK 0.10.0:
  24 MB of `.text`, 42,504 `.pdata` functions, 48,271 registered, **zero**
  unresolved calls or branches out of codegen.
- Reaches the start menu and renders it correctly. Runs indefinitely with no
  behaviour-changing guest hooks.
- **NG2's tasks are fibers.** The guest's own context switch cannot work under
  static recompilation - recompiled call state lives on the host stack, so
  restoring guest registers transfers nothing. The family is mapped to the
  SDK's host implementations, and `SwitchToFiber` has to be mapped at
  `0x8380E638`, the fast-path second entry the scheduler actually calls, not
  at the function's real entry.
- **setjmp/longjmp declared in the manifest.** The resource parsers use them
  for error recovery; unset, an error callback returned instead of unwinding
  and corrupted memory far from the eventual crash.
- Low guest memory (including page 0) committed as read-only zeroes, matching
  what the hardware returns for the game's uninitialised slot table.
- ROV render-target path, measured: 3,128 EDRAM resolves and 7 pipelines in
  30 s against 22 and 0 for RTV.

### Settings

- **Setup screen before the guest boots**, from `OnFinalizePaths`: choose the
  game folder, install from your own ISO, choose a DLC folder. It reads the
  title out of the disc's XEX rather than just checking a file exists, so it
  can tell you when you have picked the wrong game. Opens on first run, on
  Shift-at-launch, or when the configured folder has gone missing.
- **Installing from an ISO**: 65 files, 6.7 GB, about 20 seconds, with the
  extracted `default.xex` byte-identical to a reference copy. Files already
  present at the right size are skipped, so a cancelled install resumes.
- **In-game settings on F10**, sharing the same pages. Live settings apply on
  the next frame; startup-latched ones are greyed and marked `(restart)`
  instead of being accepted and ignored.
- Window size, fullscreen, frame rate 30-144, V-Sync, internal supersampling
  1-3x, the 1280x720 internal render patch, anisotropic override, output
  filter, letterbox, dither. DLC is installed automatically and the licence
  mask defaults to the full version so it is visible.
- The output-filter list is read from the cvar's own declared values, so the
  menu cannot offer an effect this build does not implement - and this build
  implements bilinear only.

### Fixed

- **Internal resolution scaling now works.** It was written off as impossible;
  it was being written at the wrong moment. `resolution_scale` is also not the
  cvar the plugin reads - it accepted `2` while the GPU reported `internal
  scale 1x1`. Sending `draw_resolution_scale_x/y` through the deferred config
  gets a real `2x2`.
- **Fullscreen is honoured at launch.** The cvar is read when the window is
  created, during `SetupPresentation`, which runs *before* `OnPreSetup` - so a
  saved setting came back windowed every time. Window settings now go in from
  `OnConfigurePaths`, and the setup screen also pushes them at the live window.
- The pre-boot UI is repainted by an explicit pump. Nothing else drives the
  presenter before the guest exists, so the setup screen previously drew once
  and froze: a perfect screenshot, completely dead to the mouse.

### Known issues

- The in-engine intro video renders corrupted. The fault is in the SDK's Xenos
  GPU plugin - see `bugreport/REXGLUE-BUG-video-decode.md`.
- Leaving the title screen alone starts the attract-mode demo video, which
  breaks the GPU command stream: the picture stops updating and the frame rate
  halves while the game carries on running.
- FSR/CAS output filtering is not available in this SDK build.
- Gameplay past the title screen has not been verified end to end.
