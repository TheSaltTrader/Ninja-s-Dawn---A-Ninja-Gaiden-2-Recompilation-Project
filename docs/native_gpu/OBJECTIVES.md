# Native renderer: the objectives, and how two sessions share one machine

The goal is a full replacement for the Xenia-derived Xenos GPU plugin: a native
Plume/D3D12 renderer, no emulation in the rendering path.

**Changed 2026-09-16, by the user: the two games are now two migrations, run in
parallel by the two sessions, not one path brought to the other afterwards.**
NG2 is its own native migration on its own branch; the Fable II session is there
for guidance and for anything shared, not as a dependency. NG2's earlier
objective 7 ("after 6") no longer applies.

Also retired by the user the same day: NG2's 120 fps frame interpolation, and
every frame-rate option above 60. Shipped in v1.0.21. Do not revive either.

Two Claude sessions work on this. This file is the shared list so neither of us
guesses what the other is doing.

## Where it stands

Fable II's native path draws about 2,150 of the frame's 2,300 draws at roughly
52 fps in Bowerstone market, through the game's own translated vertex and pixel
shaders, its own index and vertex buffers, and its own textures. Architecture,
props, foliage, bunting, the clock tower and the characters all render.

## Objectives, in order

1. **The ground.** Nothing writes the plaza's pixels in the presented pass, and
   every cheap explanation has been eliminated by experiment (see M4_fable2.md,
   runs 229-262). The instrument now in place is a draw census on both sides:
   Fable's own copy of the plugin reports every draw it executes, keyed by the
   vertex shader's microcode fingerprint, and the native path reports the same
   for the draws it issues. The shader in one list and not the other names the
   missing geometry. OWNER: Fable session.
2. **Impostor render targets.** Fable bakes each distant character's billboard
   by rendering it orthographically. That pass is currently dropped, which is
   what stopped it painting over the scene, but dropping it also means the
   billboards have no texture. It needs its own offscreen target and a resolve
   into the texture the billboards sample. OWNER: Fable session.
3. **The atmospheric pass.** A full-screen quad lays a flat pale wash over the
   sky and the distance. It is drawn but wrong, most likely because it samples
   a depth target the native path does not provide. OWNER: open.
4. **Stencil.** Unimplemented. OWNER: open.
5. **Native presentation.** `ngpu_present_post` still shows the wrong texture.
   OWNER: open.
6. **Take the plugin out of Fable's loop.** The milestone that makes the
   replacement real. Needs 1-5. OWNER: joint.
7. ~~Bring the path to NG2. After 6.~~ **Superseded** - NG2 runs in parallel,
   see below.

## NG2's own track (OWNER: NG2 session, runs in parallel)

Recon is complete and the design is decided. See `M4_ng2_pm4_census.md` for the
measurements and `M5_ng2_architecture.md` for the decision.

- **N1. Interception point: the PM4 packet, not the guest D3D call.** DECIDED.
  NG2's Direct3D entry points cannot be identified with this SDK (return
  addresses are unobtainable - tested, both routes) and the census showed the
  one static anchor was wrong. Everything a native draw needs is in the stream.
  The guest-call route stays the destination, reachable later by replacing the
  front end.
- **N2. Auto-index point sprites.** FIRST, because they are 1,701 of 2,338
  draws. Fable's IB/VB renderer has no auto-index path; copying it would miss
  every particle in the frame.
- **N3. Indexed geometry.** One shape only: kDMA + 16-bit + TRI_STRIP.
- **N4. Shaders keyed on the IM_LOAD address.** 63-67 per frame, no guest hook
  needed - this is cheaper than the route the Fable side had to take.
- **N5. Constants by DMA.** LOAD_ALU_CONSTANT and type-0 ranges. NG2 never uses
  SET_CONSTANT; a device implementing only SET_CONSTANT renders nothing.
- **N6. Depth with its own identity and ownership, keyed on the depth base.**
  From the Fable session's finding - taken up front rather than retrofitted.
- **N7. First provable milestone:** one frame in a Plume window from NG2's own
  geometry, recognisable silhouette, shaders translated, no textures.

Also with the NG2 session, unblocked: the heavy-scene stall that freezes NG2 in
ordinary play. It is in the shared plugin, so whatever is learned applies to
Fable.

## Sharing the machine

One game at a time. `~/.game-test-lock` is the token:

    session=<who> game=<Fable2|ng2|none> until=<HH:MM> note=<what>

- Never start a run while the lock names a game. Poll, do not assume.
- Claim for no more than 30 minutes at a time, and write an honest `until`.
- Release the moment you stop, with `game=none`. Between runs still counts as
  held if you are about to start another; say so in the note.
- A lock reading `game=none` does not mean the other session is finished. If
  you want a long block, ask by message rather than taking it.
- Build while the other session runs. Builds do not need the machine's
  attention; only the game does.

## Sharing what we find

Anything learned about the shared plugin, the ring, EDRAM, resolves or texture
formats goes in this directory, not only into the finder's own notes, because
both games run the same plugin. Corrections matter more than findings: several
confident conclusions on both sides have been wrong this week, and the ones
that cost the least were the ones written down and revisited.


## Gotchas that have cost a run each

- **The chapter card needs an A press AFTER the load.** Presses queued during
  loading are swallowed. A chapter card sitting on "Proceed" renders perfectly
  and looks exactly like a hang.
- **Verify a kill from the process list, never assume it.** A cleanup script
  whose earlier step is blocked aborts before its `Stop-Process`, leaving the
  game holding the machine while the lock reads free.
- **The in-app update dialog blocks a fresh install's first run.** Set
  `check_for_updates=0` in the build's own `ng2_settings.cfg`. The in-process
  pad channel still drives the game through it, so it is survivable, but the
  mouse cannot dismiss it if another process owns an invisible window over it.
- **Environment variables need `Start-Process`, not WMI.** `Win32_Process.Create`
  drops the parent's environment, so every env-gated probe (NGPU_PM4,
  NGPU_BASES, FABLE2_TUNE) silently does nothing when launched that way.
- **A stale PCH fails the plugin build with a bogus toolchain error.** Build
  from a shell that has run `vcvars64.bat`; without it clang falls back to
  MSVC 19.33 and refuses a PCH compiled by 19.44.
