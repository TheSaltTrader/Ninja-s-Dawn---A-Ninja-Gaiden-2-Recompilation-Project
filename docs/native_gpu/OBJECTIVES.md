# Native renderer: the objectives, and how two sessions share one machine

The goal is a full replacement for the Xenia-derived Xenos GPU plugin: a native
Plume/D3D12 renderer driven by per-game Direct3D-9-device HLE. Fable II first,
because Ninja Gaiden 2 already holds 60 fps on the plugin and has less to gain
from the switch. When Fable II renders its own frames end to end and the plugin
is out of its loop, the same path is brought to NG2.

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
7. **Bring the path to NG2.** After 6. OWNER: NG2 session.

Alongside, and not blocked by any of the above: NG2's 120 fps frame
interpolation, and the heavy-scene stall that still freezes NG2 in ordinary
play. Those stay with the NG2 session because they are that game's, but the
stall is in the shared plugin and whatever is learned there applies to Fable.

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
