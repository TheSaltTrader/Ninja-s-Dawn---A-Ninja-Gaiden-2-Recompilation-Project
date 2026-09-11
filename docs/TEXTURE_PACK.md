# Texture pack: extraction, upscaling, replacement

**Status: extraction and decoding WORK and are verified. Replacement is not
built yet.** `docs/texpack/decoded_sample.png` is real output - a flower and a
kunai decoded from raw guest bytes, correct alpha, no artefacts.

## How it works, and why it is built this way

The obvious design is to dump finished textures. That is not possible cheaply
here: **the plugin converts textures on the GPU.** Guest bytes go into shared
memory and a compute "load shader" untiles and converts them straight into a GPU
copy buffer, so host-format pixels never exist CPU-side. Reading them back would
cost a readback buffer, a fence and a stall *per texture*.

So the plugin dumps the **raw guest bytes plus the TextureKey**, which costs one
`memcpy` the first time a texture is seen, and the untiling is reproduced
offline. It is a pure function of `(x, y, pitch, bytes-per-block)`, ported
verbatim from the SDK's `GetTiledOffset2D`, so nothing is approximated.

```
game (plugin, cvar-gated)          tools/upscale_textures.py
  dump/tex_<id>.bin   raw guest bytes  ->  untile -> decode -> dump/<name>.png
  dump/index.txt      key per texture  ->  upscale (Real-ESRGAN) -> pack/<id>.png
```

## Ids carry a content hash (2026-09-11)

**The bug.** The plugin files a texture under an id built from its memory
page, format, size, pitch and tiling - `TexturePackId` in
`rexglue-src/src/graphics/d3d12/texture_cache.cpp`. Nothing in that id
describes the pixels. Ninja Gaiden II streams its chapters through the same
memory, so the shop-window glass at the start of chapter 1 landed at the same
address, with the same format and size, as a normal map from a later chapter
that had been dumped first. Both had one id; the pack held the normal map; the
window rendered violet with bumps (a normal map drawn as colour). Pressing F9
made it cream again, and moving the 1084 violet normal-map-looking files out of
the pack fixed the window with the pack on, which is how it was pinned.

**The fix, in the plugin.** Every file is now `<id>-<hash>.tex`, where the
hash is a CRC-32 (the zlib polynomial) of the raw guest bytes - the same bytes
the dump writes. The lookup hashes what is in guest memory at texture creation
(once per texture; the load and view lookups reuse it), builds an index of the
pack folder once per pack path, and only opens a file whose id AND hash match.
A collision therefore misses, the game's own texture is used, and one log line
per id says so:

    [texpack] 0179...: 1 pack file(s) carry this id but none matches the
    texture in memory (hash 575934BE) - another texture shares its address;
    the game's own texture is used

The dump names its files `tex_<id>-<hash>.bin` and writes the hash as a tenth
column of `index.txt`, so two textures that share an address are dumped as two
files instead of the second being lost behind the first. Stage lists carry the
same `<id>-<hash>` keys. Old `<id>.tex` files are ignored, counted and reported
at index time rather than silently served.

**Migration is automatic.** `tools/upscale_textures.py` starts every run with
`migrate_pack`: nine-column index lines get their hash from `tex_<id>.bin`,
`<id>.tex` and the decoded PNGs are renamed, stage lists are rewritten. It is a
rename, not a re-upscale - the 46 GB pack here migrated in 14 seconds. A file
whose raw dump is gone cannot be hashed and keeps its old name (198 of 11793
here); re-dumping the scene that uses it brings it back. `--migrate-only` does
just the migration. The CRC is reproduced with `zlib.crc32`, chosen over the
SDK's xxHash precisely so the tool needs no extra package; parity is verified
against the plugin's implementation on sizes 0..1 MB and a real dump file.

**Why not a bigger id.** No field the plugin has at lookup distinguishes the
two textures except their bytes. A content hash is the only key that follows
the art rather than the address.

## Using it

Settings are wired through `ng2_settings.cfg` and the generated tuning file:

```
texture_dump=1
texture_path=C:/ng2tex
```

Then `python tools/upscale_textures.py --dir C:/ng2tex [--upscale]`.

**GPU plugin cvars cannot be passed on the command line.** They only reach the
plugin through `cvar::LoadConfig` deferral, i.e. the generated
`cache/ng2_tuning.toml`, which `ng2_tuning.h` writes each launch. `--texture_dump`
on the command line silently does nothing.

## What is verified

* the dump hook is on the only texture load path - `LoadTextureData` has one
  implementation, `LoadTextureDataFromResidentMemoryImpl`, called from one place
* 71 textures dumped, 70 decoded, 0 failures; decoded output visually correct
  after the stride fix below
* the upscaler enlarges: 1280x720 -> 5120x2880 at `--scale 4`

**Not yet verified: that any of it sees real art.** Both capture runs so far
reached the title screen and idled into the attract demo, and 82 seconds of that
produces no art at all. See below.

## What a menu capture actually contains, and why it matters

Every texture in the sample dump was one of three things, and none was art:

| what | how it shows up |
|---|---|
| video decoder planes | `k_8` at 1280x720, 640x360, and half-size chroma pairs like 320x580 / 160x290 |
| render targets | `k_8_8_8_8` at 1280x720 and 1120x584 (the game's internal size) |
| the HDR scene buffer | `k_16_16_16_16_FLOAT` at 1120x584, 5.4 MB |

The tell is that **the only power-of-two entries in 71 textures were `256x1` and
`4x4`**. Shipped 360 art is block-compressed or power-of-two; a framebuffer is
neither.

This was nearly a silent disaster. Before the filter existed, the pack came out
holding five files, two of them the scene resolve upscaled to 5120x2880 - a
"texture pack" whose largest members were photographs of the screen. Replacing
those at runtime would corrupt every frame.

So there are now two filters, at different places and for different reasons:

* **at the dump site** (`TexturePackIsArtFormat` in `texture_cache.cpp`) - only
  formats a shipped asset is stored in. This throws away the video planes and the
  HDR buffer at source, so the dump folder does not fill with multi-megabyte
  framebuffers rewritten on every key change.
* **in the tool** (`pack_reason` in `upscale_textures.py`) - reversible, and it
  reports a count per reason. It rejects uncompressed non-power-of-two textures,
  which is what catches the render targets that pass the format filter, plus the
  font/HUD rules below.

Run against the menu dump, the tool now packs **nothing** and says why:

```
  not packed - single-channel               50
  not packed - too small                    15
  not packed - not art (npot uncompressed)   5
```

An empty pack is the correct answer for that input.

## The UI exclusion list

Fonts, HUD atlases and gradient ramps look **worse** upscaled: a model invents
detail in glyph edges and shifts them, which is the usual reason a texture pack
looks broken. `pack_reason()` skips anything <= 64px on a side, single-channel
formats (`k_8`, `k_8_8` - masks and ramps), and strips with an aspect ratio >= 8.
`--include-ui` overrides all of it.

## Two traps that cost time

**The plugin and the runtime must be built from the SAME source tree.** A
source-built `rexgpu-xenos.dll` against the stock `rexruntime.dll` makes the game
exit during startup with no error - the log simply stops after the tuning lines.
This is why the shipped releases use both stock, and why enabling texture
dumping currently requires deploying both source-built DLLs together.

**Cvar macros must be at global scope.** Defined inside
`namespace rex::graphics::d3d12`, they compile and link but the plugin fails to
initialise and the game exits silently at startup.

## The stride bug worth remembering

First decode produced recognisable art sliced into horizontal bands. The
`TextureKey` pitch is in units of **32 texels**, and `tiled_offset_2d` wants a
pitch in **blocks**; dividing by bytes-per-block instead of by block width put
every macro-tile row at the wrong stride. Correct form:

```python
pitch_texels = max(pitch * 32, w)
pitch_blocks = max(1, pitch_texels // block)   # block = 1, or 4 for DXT
```

## What remains

0. **A gameplay capture.** Everything below is blocked on it, and it costs two
   minutes: turn dumping on, load a level, fight for a bit. Until then there is
   no art to replace and no way to tell working code from broken code - which is
   exactly why the replacement path has NOT been written blind.
1. **Replacement** - the plugin must upload a pack texture instead of the guest
   one. Harder than the dump: `CreateTexture` sizes the resource from the guest
   key and the load shader writes the copy buffer, so an upscaled replacement
   needs its own resource at the new size, an `stb_image` decode (already in
   `thirdparty/stb`), a direct upload bypassing that shader, and an SRV whose
   format and swizzle match the replacement rather than the guest format. The
   cvar (`texture_pack_path`) and the settings/tuning plumbing already exist.
2. **Real-ESRGAN** - `--upscale` currently falls back to Lanczos and says so if
   `realesrgan` is not installed (`pip install realesrgan basicsr` plus a CUDA
   torch). The model call is deliberately isolated so it can be swapped.
3. **Options-screen section** - built (`SettingsOverlay::DrawTextures`): folder
   field, Dump and Use checkboxes, a Process button and a progress bar with
   percentage and ETA. Compiled but never exercised on screen.
