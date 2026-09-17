# M4 NG2 — the shape of one frame

Frame 7201, Chapter 7 gameplay, 2026-09-16. **5,618 calls into the Direct3D
surface, recorded in order**, plus per-frame counts. Raw:
`M4_ng2_frame_transcript.txt`.

This is the artefact the Fable session said was worth more than any histogram:
the pass structure of a frame is what explains where every render target comes
from, and a cumulative census cannot show it. Captured with
`--frame-marker 837452C0` (NG2's VdSwap caller, identified from the census by
firing 581 times per 10 s = once per frame at 60 fps) and dumped once, after a
tunable frame number so it lands on a real 3D frame rather than the title.

## Per-frame counts

46 distinct entry points in the frame. Top by count:

| entry point | calls/frame | call sites |
|---|---:|---:|
| sub_8373B1F8 | 868 | 26 |
| sub_837369A8 | 854 | 4 |
| sub_83742C10 | 352 | 0 (EVENT_WRITE) |
| sub_83734B10 | 348 | 1 |
| sub_83734A78 | 345 | 1 |
| sub_8373B4D8 | 308 | 34 |
| sub_837370F0 | 297 | 10 |
| sub_8373CB08 | 296 | 6 |
| sub_8373EA90 | 266 | 8 |
| sub_83736568 / sub_837363D8 | 230 each | 55 each |
| sub_8373B880 | 228 | 32 |

## Three repeating cycles, in order

The transcript is not a flat list — it is three motifs.

**1. The frame preamble**, appearing at the start and again between phases:

    8373A3D0  837397E0x2  83739CF0  8373A088  83739CF0  [8374C4D0]  8373A4D0

`83739CF0` is the INDIRECT_BUFFER emitter and `8373A088` an EVENT_WRITE_SHD, so
this is command-buffer chaining plus a fence — a pass boundary. `8374C4D0` is
the VdRetrainEDRAM caller and appears only in the first occurrence.

**2. A five-call state block**, 76 occurrences, always in this exact order:

    83736568  837363D8  83736908  837369A8  83736A28

These are the shadow-state setters identified in the census: each takes a slot
index in r4, a dirty bit `0x80000000 >> slot` in r6, and a constant device
shadow offset in r8 (0x1E4, 0x1E8, 0x1F0, 0x1F4, 0x208). Five different shadow
offsets written together as a unit is a **state block commit**, not five
unrelated setters.

**3. A draw-shaped cycle**, 19 occurrences of its tail:

    8373B1F8x2  8373B880  8373B4D8  837369A8x3  83734B10  83734A78
    8373B880  8373B4D8  837370F0  8373CB08  83742C10

`8373B1F8` is the frame's hottest call and passes floats plus a guest pointer
(constant upload). `8373CB08` carries values shaped like Xenos fetch constants
(`161A3003`, `16199003`, `176CF003`) — resource binding. `83742C10` is an
EVENT_WRITE emitter and closes the cycle. So the order is: upload constants,
set state, bind resources, emit. (INFERENCE from argument shapes; not yet
confirmed against a draw packet in the ring.)

**The frame closes** with a different motif built from `8373EA90x6`,
`8373BD50`, `837397E0` and the 55-site pair — repeated many times. `8373EA90`
is the third-hottest call at 266/frame across 8 sites.

## Why this matters for the native path

- The five-call state block is the unit to intercept. Hooking one setter gives a
  fragment; hooking the block gives a coherent state commit.
- The preamble marks pass boundaries, which is where render targets change. That
  is the hook point for reproducing NG2's pass structure rather than guessing it.
- `8373CB08` at 296/frame is the resource-binding entry point to decode next: its
  arguments should map onto Xenos fetch constants, which is what the native path
  needs to resolve textures and vertex streams.

## What is still missing, and why

- **Caller return address.** The Fable session's second logging ask. Not done:
  mid-asm hooks can only pass GPRs, and `lr` is a raw `uint64_t` in PPCContext
  rather than a `PPCRegister`, so requesting it does not compile. The transcript
  recovers most of what the call graph would have given for pass structure, but
  not which engine function drives each cycle.
- **First ring dwords per PM4 emitter.** Their fourth ask — "writes the ring" is
  50 functions, "writes a packet whose opcode is a draw" is three. Not yet done.
- The cycle interpretations above are inference from argument shapes. The next
  step that would confirm them is correlating one cycle against the PM4 the
  plugin executes for the same frame.
