# P2 attribution census - method and format (shared with the Fable II team)

Full-native plan, phase P2 ("The measure of 100%", FULL_NATIVE_PLAN.md 5627323 on Fable's native-gpu branch):
two numbers, driven to zero. NG2 defined the format (2026-09-27); Fable adopts it.

## What is measured

**Metric 1 - PM4 packets the game writes that no replaced function accounts for.** Every packet the plugin
executes is attributed to the guest Direct3D library entry point (or direct ring site) that wrote it. Attribution
is by ADDRESS, not by call count: engines that build packets with computed opcodes (NG2) cannot be attributed by
counting calls, and a call that writes nothing must not be credited.

**Metric 2 - draws the native front end did not issue itself**, per frame, compared against the bridge. Reported
as n/a until a front end exists (P3); then the front end's draw count per frame minus the bridge's, by frame.

## Source of truth

The XDK Direct3D device object's own ring write cursor: `device + 0x30`, the push pointer the library advances as
it writes packets (same layout on Fable, M4_fable2.md), read on the guest thread at the ENTRY and at every EXIT of
each hooked function. Also `device + 0x3484`, the copy some inline paths commit through (Fable's caveat); the join
picks whichever field attributes more. Not CP_RB_WPTR / the write-back pointer: those move only at kick-off and
would lump many producers into one range.

The device pointer is the first argument of the swap entry point (the frame marker hook); frames are counted by
that entry on the guest side and by the swap packet on the plugin side (the two differ by the swap packet itself,
which the join tolerates by also looking at the neighbouring frame).

## Recording

- Guest side (`src/ng2_p2_census.cpp`): the trace hook at each of the 108 entry points
  (`config/hooks/native_gpu_trace.toml`, `src/native_gpu_trace.cpp` kEntries order = the hook index) calls
  `Enter(hook, r3)`; `tools/native_gpu/p2_inject_exits.py` inserts `ng2_p2_exit(hook)` before every `return;` of
  the recompiled body (55 of the 108 leave by tail calls, so `blr` alone is not the exit). A per-thread stack pairs
  entries with exits; a nesting mismatch is counted. Output `p2_guest.bin`:
  header 8 x u32 {'NGP2', version 1, record size 28, first frame, frames, cursor offset, cursor2 offset, records};
  record {u32 frame, u32 thread id, u16 hook, u16 flags (bit 0 = exit matched below the stack top),
  u32 cursor in, u32 cursor out, u32 cursor2 in, u32 cursor2 out} - cursors as the library holds them (the CPU's
  0xA0000000/0xC0000000/0xE0000000 view of physical memory).
- Plugin side (fork `command_processor.cpp`, `ng2_p2`): at `ExecutePacket`, the packet's guest address = the
  buffer's physical base (ring, indirect buffer or immediate buffer, kept by a scope) + the reader offset; record
  {u32 frame, u32 address, u32 header word, u32 bin select} into `p2_packets.bin` (header 4 x u32
  {'NGP2', 1, 16, first frame}).
- Switch: `NG2_P2=<first frame>:<frames>` in the environment (both sides); `NG2_P2_DISCOVER=1` logs the device
  object's first 32 words at the first calls; `NG2_P2_CURSOR` / `NG2_P2_CURSOR2` override the offsets.

## Join and report (`tools/native_gpu/p2_census.py --guest p2_guest.bin --packets p2_packets.bin [--json out]`)

Addresses are compared as physical (`& 0x1FFFFFFF`). A packet is attributed to the call whose
[cursor in, cursor out) holds its address in the same frame, else the previous or the next frame; a range that
wraps is split. Lines:

    P2 CENSUS: <frames> frames (<first>..<last>), <call ranges> call ranges (attributing by <field> ...), <packets> packets, <draws> draws
    METRIC 1  unattributed packets: U of P (x.x%)   unattributed draws: Ud of D (x.x%)
    METRIC 2  draws not issued by a native front end: n/a | <per-frame difference once P3 exists>
    by producer (whole run): packets  draws  producer            <- sub_XXXXXXXX + the hook table's label
    unattributed packets by type/opcode:                          <- what still has no producer, DRAW opcodes flagged
    per frame: frame  packets  draws  unattributed(packets/draws)  type0/2/3  bins(draws by bin select)

The JSON carries the same per frame and in total (`by_producer`, `draws_by_bin_select`, `field`). The bin-select
column is the tiling census Fable asked for: draws repeated per tile appear as the same count under each select.

## Three producer classes the first NG2 censuses found (Chapter 1 gameplay, 1759-2219 draws per frame)

1. **Per-frame calls** - the call whose range holds the packet in the same frame (the plugin runs a frame or so
   behind the guest; the join looks at the neighbouring frame too). NG2: about 600 draws per frame, the draw
   entry point `sub_8373CB08`.
2. **Recorded once, replayed every frame** - a buffer written long before the window and executed through
   INDIRECT_BUFFER each frame. NG2: a 24-packet `DRAW_INDX_2` block at physical 1F037980 (the library's persistent
   template area next to the primary ring, emitted once at device creation by `sub_8373B060`, the one function
   with an immediate DRAW opcode) executed ~59 times per frame = 1416 of 2219 draws. Hence the recorder keeps the
   LAST WRITER of every 64-byte block from the first library call on (format version 2, section 'BLK1'), and the
   device is learned at that first library call, not at the first swap.
3. **Engine inline** - packets written by engine code right after a hooked library call returned. NG2: 32
   `DRAW_INDX` per frame exactly 56 bytes past the end of a `sub_8373BD50` range (the state flush before a draw:
   dirty masks at device+16/+24, constants at +1920/+6016); the join names these `ENGINE INLINE after sub_X`
   (gap up to 256 bytes past a range end). These are the direct writers the front end must replace by hooking the
   CALLERS, not the library.

## Reading it

- Metric 1 at 0 with the hooks covering the whole library surface means every packet has a producer the front end
  can replace. Non-zero: the `unattributed packets by opcode` list names the gap (library emitters not hooked,
  engine code writing the ring directly, or a cursor field the producer bypasses).
- A high `nesting mismatch` count in the exe's `[p2] wrote` line means exits were missed (a return path without
  an injected exit); ranges then run long and over-attribute - fix the injector before reading the numbers.
