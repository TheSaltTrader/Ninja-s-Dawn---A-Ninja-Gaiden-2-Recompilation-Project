# Native-GPU draw dump — format (Fable II TU1, branch `native-gpu`)

Written by `fable2recomp/src/native_gpu_dump.cpp`, fed by the generated hooks
in `src/native_gpu_trace.cpp` (`gen_trace_hooks.py --dump`). Records what the
XDK draw entry points read from the device struct, per draw, for a window of
frames; `tools/native_gpu/dump_report.py` summarises a dump (draws per frame,
distinct object keys, frame-to-frame key match rate).

Enable at launch (FABLE2_TUNE, both cvars are int32):

```
ngpu_dump_at_frame=<first frame>   0 = off (frames count Present calls from boot)
ngpu_dump_frames=<n>               default 2
```

Output: `ngpu_dump_<serial>.txt` beside the executable (one file per window).
All numbers are guest values, hex is big-endian as the game sees it.

```
D f<frame> <kind> prim=<p> base=<b> start=<s> count=<c> ib=<obj>:<w0>/<w6> vs=<obj> ps=<obj> ct=<table>+<base>:<n> rt=<surface>:<i> pred=<mask> vc=<hash> fc<i>=<w0>/<w1>/<w2> ...
S f<frame> shader=<obj> type=<t> code=<addr>
M shader=<obj> type=<t> code=<addr> len=<bytes> hdr40=.. hdr48=.. hdr52=.. hdr56=.. hdr64=..
C f<frame> table=<t> base=<b> base2=<b2> n=<n> list=<addr> len=<bytes>
R f<frame> surface=<s> index=<i> w0=<word>
F <frame>
```

- **D** — one draw. `kind`: `DI` = DrawIndexedVertices (`sub_8221DFC0`:
  prim, baseVertexIndex, startIndex, indexCount), `DV` = DrawVertices
  (`sub_8221C3E8`: prim, start, count; base printed as 0), `UP` =
  DrawVerticesUP (`sub_82217DB8`; a second line `up r8= r9= r10=` carries the
  remaining registers). `prim` is the XDK `D3DPRIMITIVETYPE`: 1 POINTLIST,
  2 LINELIST, 3 LINESTRIP, 4 TRIANGLELIST, 5 TRIANGLEFAN, 6 TRIANGLESTRIP,
  8 RECTLIST, 0xD QUADLIST.
- `ib` — the current index buffer object (device+0x3094), then its word 0
  (guest address | format bits) and word 6 (size). `DV`/`UP` draws still print
  whatever is bound.
- `vs` / `ps` — the last shader object passed to `sub_82221858` with type
  bit 0 clear / set (the `S` lines show every call; `code` is the object's
  +64 word, the microcode header).
- `ct` — the last `LoadShaderConstants` (`sub_82221B90`) table pointer, its
  base and count. Reading `SetShader` further showed the table is the
  **shader object's own literal-constant table** (`obj+872`, base `[obj+32]`):
  SetShader calls the loader itself, so the `LOAD_ALU_CONSTANT` packets carry
  shader literals, and the per-object transforms go through the device
  shadow (`vc` below) — the reference's model after all.
- `vc` — FNV-1a hash of vertex float constants c0..c15 (device+0x780, 256
  bytes) at the draw: changes per object when the transform is written
  through the device shadow.
- **M** — first sight of a shader's microcode: the blob (`code`, `len` bytes:
  `[obj+24] + [hdr+40]`, `[hdr+44]`, hdr = `obj + [obj+64]`) is written to
  `ngpu_shaders/<code>_<v|p>.bin` beside the executable, once per address —
  the input for the offline shader translation.
- `rt` — the last `SetRenderTarget` surface and index.
- `pred` — the tiling predication mask at device+0x31A4.
- `fc<i>` — every fetch constant slot (32 × 24 bytes at device+0x480) whose
  word 0 is non-zero: words 0, 1, 2. Texture fetch constants carry the base
  address in word 1 (word 0 = type/pitch), vertex fetch constants carry
  `address | size` in word 0 with word 1 = size/endian bits (the report keys
  vertex slots on word 0; which slots are vertex vs texture in this XDK is
  what the first dump settles).
- **F** — Present (`sub_82BA34D8`) closed frame `<frame>`.

Object identity key (what the NG2 frame-interpolation session matches on and
measured at 94–96 % frame-to-frame in NG2): `(vertex fetch slots' word 0, ib
word 0, vs, ps)`; `dump_report.py` prints the same match rate for Fable II
from a dump, with and without the texture slots in the key.
