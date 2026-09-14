"""Decode dumped guest textures to PNG, upscale them, and write a texture pack.

The plugin dumps the RAW GUEST bytes plus a TextureKey, not finished pixels -
texture conversion in the plugin happens in a GPU compute shader, so host-format
pixels never exist on the CPU and reading them back would cost a fence and a
stall per texture. Untiling is a pure function of (x, y, pitch, bpp), so it is
reproduced here exactly instead.

    python tools/upscale_textures.py --dir C:/ng2tex            # decode only
    python tools/upscale_textures.py --dir C:/ng2tex --upscale  # decode + upscale

Progress is printed as machine-readable lines so the in-game options screen can
show a percentage and a time estimate:

    PROGRESS <done> <total> <name>

Xenos tiling is taken from the SDK's GetTiledOffset2D, and block-compressed
formats (DXT1/3/5) are decoded here rather than pulled from a library so the
tool has no dependencies beyond Pillow, which the upscaler needs anyway.

IDS CARRY A CONTENT HASH. The plugin's texture id is built from the texture's
memory address, format, size and pitch - nothing in it describes the pixels.
A game that streams its chapters reuses memory, so two different textures can
carry the same id, and a pack keyed on the id alone hands whichever was dumped
first to both (Ninja Gaiden II: a shop window rendered as a violet normal map).
So every file is named <id>-<hash>, hash = CRC-32 of the raw guest bytes,
computed by the plugin at dump and at lookup and reproduced here with
zlib.crc32. A pack made before hashes is migrated in place on the next run:
the raw dump gives each old file its hash, the file is renamed, nothing is
re-upscaled. Old files whose raw dump is gone cannot be hashed and are left
as they are; the game ignores them and says so in its log.
"""

import argparse
import os
import re
import struct
import sys
import time
import zlib

# TextureFormat values that matter for this title. Anything else is reported
# and skipped rather than guessed at - a wrong guess produces a plausible
# looking texture that is subtly wrong, which is worse than a gap.
FMT_8 = 2
FMT_1_5_5_5 = 3
FMT_5_6_5 = 4
FMT_8_8_8_8 = 6
FMT_8_8 = 10
FMT_8_8_8_8_A = 14
FMT_4_4_4_4 = 15
FMT_DXT1 = 18
FMT_DXT2_3 = 19
FMT_DXT4_5 = 20

# TextureKey.endianness
ENDIAN_NONE = 0
ENDIAN_8IN16 = 1
ENDIAN_8IN32 = 2
ENDIAN_16IN32 = 3

try:
    from PIL import Image
    Image_LANCZOS = Image.Resampling.LANCZOS
except ImportError:      # reported properly in main()
    Image = None
    Image_LANCZOS = None

# <id>-<hash>_<w>x<h>_<format>.png. Names without the hash are from before
# content hashes; migrate_pack renames them where the raw dump still exists.
DECODED_RE = re.compile(r"^([0-9A-Fa-f]{16}-[0-9A-Fa-f]{8})_(\d+)x(\d+)_(.+)\.png$")


def content_hash(data):
    """CRC-32 of the raw guest bytes, exactly as the plugin computes it."""
    return "%08X" % (zlib.crc32(data) & 0xFFFFFFFF)


def migrate_pack(dump, pack):
    """Bring a dump and pack made before content hashes up to date, in place.

    Runs at the start of every invocation and does nothing when there is
    nothing to do. Four things carry the old <id>-only names and each is
    renamed from the same source of truth, the raw guest bytes in the dump:

      dump/index.txt       nine-column lines get the hash as a tenth column
      pack/<id>.tex        renamed <id>-<hash>.tex (a rename, not a re-upscale)
      dump/<id>_WxH_F.png  renamed <id>-<hash>_WxH_F.png
      pack/stages/chNN.txt lines rewritten to <id>-<hash>

    Anything whose raw dump is missing keeps its old name: it cannot be
    hashed, the game ignores it, and its log line says why.
    """
    index = os.path.join(dump, "index.txt")
    if not os.path.isfile(index):
        return
    hashes = {}                       # id -> hash, from tex_<id>.bin
    unhashable = set()

    def hash_of(tid):
        if tid in hashes:
            return hashes[tid]
        if tid in unhashable:
            return None
        raw = os.path.join(dump, "tex_%s.bin" % tid)
        if not os.path.isfile(raw):
            unhashable.add(tid)
            return None
        with open(raw, "rb") as fh:
            hashes[tid] = content_hash(fh.read())
        return hashes[tid]

    # 1. The index. Old lines are rewritten once; the file is replaced
    #    atomically and the original kept beside it the first time.
    lines = open(index).read().splitlines()
    changed = 0
    out = []
    for line in lines:
        p = line.split()
        if len(p) == 9 and len(p[0]) == 16:
            h = hash_of(p[0])
            if h:
                line = "%s %s" % (line.rstrip(), h)
                changed += 1
        out.append(line)
    if changed:
        backup = os.path.join(dump, "index.pre-hash.txt")
        if not os.path.isfile(backup):
            os.replace(index, backup)
        else:
            os.remove(index)
        tmp = index + ".tmp"
        with open(tmp, "w") as fh:
            fh.write("\n".join(out) + "\n")
        os.replace(tmp, index)

    # 2. Pack files.
    renamed_tex = 0
    if os.path.isdir(pack):
        for fn in os.listdir(pack):
            if not fn.endswith(".tex") or len(fn) != 20:
                continue
            h = hash_of(fn[:16])
            if h:
                os.replace(os.path.join(pack, fn), os.path.join(pack, "%s-%s.tex" % (fn[:16], h)))
                renamed_tex += 1

    # 3. Decoded PNGs.
    renamed_png = 0
    for fn in os.listdir(dump):
        if not fn.endswith(".png") or len(fn) < 18 or fn[16] != "_":
            continue
        h = hash_of(fn[:16])
        if h:
            os.replace(os.path.join(dump, fn), os.path.join(dump, "%s-%s%s" % (fn[:16], h, fn[16:])))
            renamed_png += 1

    # 4. Stage lists (which pack files each chapter used, kept by the plugin).
    stages = os.path.join(pack, "stages")
    rewritten_stages = 0
    if os.path.isdir(stages):
        for fn in os.listdir(stages):
            path = os.path.join(stages, fn)
            if not fn.endswith(".txt"):
                continue
            rows = open(path).read().split()
            if not any(len(r) == 16 for r in rows):
                continue
            keep = []
            for r in rows:
                if len(r) == 16:
                    h = hash_of(r)
                    if h:
                        keep.append("%s-%s" % (r, h))
                else:
                    keep.append(r)
            with open(path, "w") as fh:
                fh.write("\n".join(sorted(set(keep))) + ("\n" if keep else ""))
            rewritten_stages += 1

    if changed or renamed_tex or renamed_png or rewritten_stages or unhashable:
        print("MIGRATED to content hashes: index lines %d, pack files %d, decoded PNGs %d, "
              "stage lists %d; %d id(s) have no raw dump and keep their old names"
              % (changed, renamed_tex, renamed_png, rewritten_stages, len(unhashable)),
              flush=True)

FMT_NAMES = {
    FMT_8: "k_8", FMT_1_5_5_5: "k_1_5_5_5", FMT_5_6_5: "k_5_6_5",
    FMT_8_8_8_8: "k_8_8_8_8", FMT_8_8: "k_8_8", FMT_8_8_8_8_A: "k_8_8_8_8_A",
    FMT_4_4_4_4: "k_4_4_4_4", FMT_DXT1: "k_DXT1", FMT_DXT2_3: "k_DXT2_3",
    FMT_DXT4_5: "k_DXT4_5",
}

# bytes per block, and block size in pixels
FMT_INFO = {
    FMT_8: (1, 1), FMT_1_5_5_5: (2, 1), FMT_5_6_5: (2, 1), FMT_4_4_4_4: (2, 1),
    FMT_8_8: (2, 1), FMT_8_8_8_8: (4, 1), FMT_8_8_8_8_A: (4, 1),
    FMT_DXT1: (8, 4), FMT_DXT2_3: (16, 4), FMT_DXT4_5: (16, 4),
}


def swap_endian(data, mode):
    """Undo the guest's endianness, from the TextureKey's own field.

    The 360 stores texture words byte-swapped and the GPU unswaps them on load;
    the plugin dumps the RAW bytes, so this has to do the same. Skipping it does
    not raise - it decodes to something that looks like coloured static, which
    is why it survived a "0 failed" run. Measured over a sample of the game's
    own DXT textures, the mean difference between neighbouring pixels is 11.6
    with this applied and 31.8 without: real art is spatially smooth, wrongly
    ordered data is close to white noise.

    Block formats are safe to swap before untiling: the swap granularity (2 or
    4 bytes) divides every block size, and blocks sit at aligned offsets.
    """
    if mode == ENDIAN_NONE or not data:
        return data
    b = bytearray(data)
    if mode == ENDIAN_8IN16:
        n = len(b) & ~1
        b[0:n:2], b[1:n:2] = bytes(b[1:n:2]), bytes(b[0:n:2])
    elif mode == ENDIAN_8IN32:
        for i in range(0, len(b) - 3, 4):
            b[i:i + 4] = b[i:i + 4][::-1]
    elif mode == ENDIAN_16IN32:
        for i in range(0, len(b) - 3, 4):
            b[i:i + 4] = b[i + 2:i + 4] + b[i:i + 2]
    return bytes(b)


def align(v, a):
    return (v + a - 1) // a * a


def tiled_offset_2d(x, y, pitch, bpb_log2):
    """Xenos 2D tiling, ported verbatim from the SDK's GetTiledOffset2D."""
    pitch = align(pitch, 32)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def untile(data, w, h, bpb, block, pitch_blocks):
    """Return linear block data for a tiled texture."""
    bw, bh = (w + block - 1) // block, (h + block - 1) // block
    bpb_log2 = bpb.bit_length() - 1
    out = bytearray(bw * bh * bpb)
    for by in range(bh):
        for bx in range(bw):
            off = tiled_offset_2d(bx, by, pitch_blocks, bpb_log2)
            if off + bpb <= len(data):
                dst = (by * bw + bx) * bpb
                out[dst:dst + bpb] = data[off:off + bpb]
    return bytes(out)


def _c565(v):
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def decode_dxt(data, w, h, fmt):
    """DXT1/2_3/4_5 to RGBA. Written out rather than imported: the tool would
    otherwise need a compression library purely for three well-documented
    block formats."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    px = bytearray(w * h * 4)
    stride = 8 if fmt == FMT_DXT1 else 16
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * stride
            if o + stride > len(data):
                continue
            alpha = None
            co = o
            if fmt == FMT_DXT2_3:
                a = data[o:o + 8]
                alpha = [((a[i >> 1] >> ((i & 1) * 4)) & 0xF) * 17 for i in range(16)]
                co = o + 8
            elif fmt == FMT_DXT4_5:
                a0, a1 = data[o], data[o + 1]
                bits = int.from_bytes(data[o + 2:o + 8], "little")
                tbl = [a0, a1]
                # The interpolation weights must SUM TO the divisor: 6-i and
                # 1+i sum to 7, 4-i and 1+i sum to 5. Written as 7-i and 5-i
                # they sum to 8 and 6, which overshoots by a seventh - enough
                # to push a0=255,a1=188 to 281 and raise "bytes must be in
                # range(0, 256)". That exception is the only reason this was
                # found: where the overshoot stayed under 255 it silently
                # decoded every DXT4/5 texture with slightly wrong alpha.
                if a0 > a1:
                    tbl += [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
                else:
                    tbl += [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
                alpha = [tbl[(bits >> (3 * i)) & 7] for i in range(16)]
                co = o + 8
            c0, c1 = struct.unpack_from("<HH", data, co)
            bits = struct.unpack_from("<I", data, co + 4)[0]
            r0, g0, b0 = _c565(c0)
            r1, g1, b1 = _c565(c1)
            if c0 > c1 or fmt != FMT_DXT1:
                pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
                       ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
                       ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)]
            else:
                pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
                       ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x >= w or y >= h:
                    continue
                c = pal[(bits >> (2 * i)) & 3]
                d = (y * w + x) * 4
                px[d:d + 4] = bytes((c[0], c[1], c[2],
                                     alpha[i] if alpha is not None else c[3]))
    return bytes(px)


def decode_plain(data, w, h, fmt):
    """Non-block formats to RGBA. The 360 stores multi-byte texels big-endian."""
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            d = i * 4
            if fmt == FMT_8:
                if i >= len(data):
                    break
                v = data[i]
                px[d:d + 4] = bytes((v, v, v, 255))
            elif fmt == FMT_8_8:
                o = i * 2
                if o + 2 > len(data):
                    break
                px[d:d + 4] = bytes((data[o], data[o + 1], 0, 255))
            elif fmt in (FMT_8_8_8_8, FMT_8_8_8_8_A):
                o = i * 4
                if o + 4 > len(data):
                    break
                a, r, g, b = data[o], data[o + 1], data[o + 2], data[o + 3]
                px[d:d + 4] = bytes((r, g, b, a))
            elif fmt == FMT_5_6_5:
                o = i * 2
                if o + 2 > len(data):
                    break
                v = struct.unpack_from(">H", data, o)[0]
                r, g, b = _c565(v)
                px[d:d + 4] = bytes((r, g, b, 255))
            elif fmt == FMT_1_5_5_5:
                o = i * 2
                if o + 2 > len(data):
                    break
                v = struct.unpack_from(">H", data, o)[0]
                px[d:d + 4] = bytes((((v >> 10) & 31) * 255 // 31,
                                     ((v >> 5) & 31) * 255 // 31,
                                     (v & 31) * 255 // 31,
                                     255 if v & 0x8000 else 0))
            elif fmt == FMT_4_4_4_4:
                o = i * 2
                if o + 2 > len(data):
                    break
                v = struct.unpack_from(">H", data, o)[0]
                px[d:d + 4] = bytes((((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17,
                                     (v & 15) * 17, ((v >> 12) & 15) * 17))
    return bytes(px)


DXT_FORMATS = (FMT_DXT1, FMT_DXT2_3, FMT_DXT4_5)


TEX_MAGIC = b"NG2T"
TEX_VERSION = 1


def write_tex(path, img):
    """Write a pack texture as a 16-byte header plus raw RGBA.

    NOT a PNG, and the reason is measured. Decoding PNG in the plugin cost
    ~16 ms per texture ON THE RENDER THREAD - a full frame's budget each, 12.2
    seconds across 800 textures, which showed up as an 8-second stall while a
    scene streamed in. The cost tracked pixel count, not file size, so it was
    the decode rather than the I/O.

    Raw pixels let the plugin read the file straight into the mapped upload
    buffer with no decode and no intermediate allocation. It costs disk - about
    2.3x a PNG - which is the trade being made deliberately.

    Header: magic "NG2T", u32 version, u32 width, u32 height (little-endian),
    then width*height*4 bytes of RGBA.
    """
    img = img.convert("RGBA")
    w, h = img.size
    with open(path, "wb") as f:
        f.write(TEX_MAGIC)
        f.write(struct.pack("<III", TEX_VERSION, w, h))
        f.write(img.tobytes())


def intact_pack_ids(pack):
    """The ids of the pack's .tex files that are whole: header present and the
    file exactly 16 + width*height*4 bytes. Returns (ids, number cut short).

    Reading 16 bytes of each of ~15,000 files takes a few seconds and is what
    makes "leave the textures already in the pack alone" safe after a run that
    died mid-write: the file the failure cut short is the one to redo, and the
    plugin would otherwise be handed a texture with no pixels behind its header.
    """
    ids = set()
    cut_short = 0
    for fn in os.listdir(pack):
        if not fn.endswith(".tex"):
            continue
        path = os.path.join(pack, fn)
        try:
            with open(path, "rb") as f:
                hdr = f.read(16)
            if len(hdr) == 16 and hdr[:4] == TEX_MAGIC:
                _, w, h = struct.unpack("<III", hdr[4:16])
                if os.path.getsize(path) == 16 + w * h * 4:
                    ids.add(fn[:-4])
                    continue
        except OSError:
            pass
        cut_short += 1
    return ids, cut_short


def pack_reason(w, h, fmt):
    """Why a texture is NOT a pack candidate, or None if it is.

    Two things have to be kept out, and they fail for different reasons.

    RENDER TARGETS AND VIDEO PLANES. The resident-memory load path carries the
    scene resolve, the HDR buffer and the video decoder's luma/chroma planes as
    well as art. Replacing any of those from a pack would be ruinous, and they
    are the easiest thing in the world to mistake for a texture: the first dump
    taken from this game was 71 files of which not one was art. The tell is
    that shipped 360 art is either block-compressed or power-of-two, and a
    framebuffer is neither - 1280x720, 1120x584, 320x580 and 890x224 are all
    render targets or video planes, and none is a power of two.

    FONTS AND HUD. An AI model invents detail in glyph edges and shifts them,
    which is the usual reason a texture pack looks broken. Small, single-channel
    and long-thin textures are the reliable signature.
    """
    if fmt not in FMT_INFO:
        return "unsupported format"
    if fmt in (FMT_8, FMT_8_8):          # masks, ramps, font coverage, video
        return "single-channel"
    if w <= 64 or h <= 64:
        return "too small"
    if max(w, h) / float(min(w, h)) >= 8.0:
        return "thin strip"
    if fmt not in DXT_FORMATS and not (_pot(w) and _pot(h)):
        # Uncompressed and not power-of-two: a framebuffer or a video plane.
        return "not art (npot uncompressed)"
    return None


def _pot(v):
    return v > 0 and (v & (v - 1)) == 0


def decode_dump(dump, tid, name, w, h, fmt, tiled, pitch, endian):
    """The dumped guest bytes of one texture as an RGBA image.

    Raises FileNotFoundError when the raw dump is gone, and whatever the
    decoder raises on bad data; the caller decides what each means.
    """
    raw = os.path.join(dump, "tex_%s.bin" % tid)
    if not os.path.isfile(raw):
        raw = os.path.join(dump, "tex_%s.bin" % tid[:16])   # dumped before hashes
    if not os.path.isfile(raw):
        raise FileNotFoundError(raw)
    data = open(raw, "rb").read()
    data = swap_endian(data, endian)

    bpb, block = FMT_INFO[fmt]
    if tiled:
        # The key's pitch is in units of 32 TEXELS, and tiled_offset_2d
        # wants a pitch in BLOCKS. Dividing by bytes-per-block instead of
        # by the block width put every macro-tile row at the wrong stride,
        # which decoded as recognisable art sliced into horizontal bands.
        pitch_texels = max(pitch * 32, w)
        pitch_blocks = max(1, pitch_texels // block)
        data = untile(data, w, h, bpb, block, pitch_blocks)
    if fmt in (FMT_DXT1, FMT_DXT2_3, FMT_DXT4_5):
        px = decode_dxt(data, w, h, fmt)
    else:
        px = decode_plain(data, w, h, fmt)
    return Image.frombytes("RGBA", (w, h), px)


def open_pending(entry, dump):
    """The decoded image behind a phase-1 entry: the PNG on disk, or a fresh
    decode when that file is unreadable (a run stopped by a full disk can
    leave one truncated)."""
    tid, path, key = entry
    try:
        return Image.open(path).convert("RGBA")
    except Exception as exc:                                    # noqa: BLE001
        if key is None:
            raise
        print("  %s unreadable (%s) - decoding it again" % (os.path.basename(path), exc),
              flush=True)
        img = decode_dump(dump, *key)
        img.save(path)
        return img


def make_upscaler(model_name, scale):
    """Return a function (PIL RGBA) -> upscaled PIL RGBA.

    Real-ESRGAN if it is installed, otherwise Lanczos. The fallback still
    enlarges: "--upscale did nothing and said so quietly" is a worse outcome
    than a plainly-labelled resample, and the pack is still usable.

    The model is NOT installed automatically. It needs a CUDA torch matched to
    the card, and picking that wrongly is an unpleasant thing to undo on
    someone else's machine.
    """
    try:
        import numpy as np
        from realesrgan import RealESRGANer
        from basicsr.archs.rrdbnet_arch import RRDBNet
    except ImportError:
        print("NOTE: Real-ESRGAN not installed - using Lanczos instead.")
        print("      For the AI model:  pip install realesrgan basicsr")
        print("      plus a CUDA torch matching your card.")

        def lanczos(img):
            return img.resize((img.width * scale, img.height * scale),
                              Image_LANCZOS)
        return lanczos

    net = RRDBNet(num_in_ch=3, num_out_ch=3, num_feat=64, num_block=23,
                  num_grow_ch=32, scale=4)
    model_path = os.path.join("models", model_name + ".pth")
    if not os.path.isfile(model_path):
        print("NOTE: %s not found - using Lanczos instead." % model_path)

        def lanczos2(img):
            return img.resize((img.width * scale, img.height * scale),
                              Image_LANCZOS)
        return lanczos2

    eng = RealESRGANer(scale=4, model_path=model_path, model=net,
                       tile=512, tile_pad=16, half=True)
    print("Real-ESRGAN ready: %s" % model_path)

    def esrgan(img):
        # The model is RGB; alpha is carried separately and resized with
        # Lanczos. Feeding a premultiplied or model-invented alpha back into a
        # game texture produces halos around cut-outs, which on a kunai or a
        # leaf is immediately visible.
        rgb = np.asarray(img.convert("RGB"))
        out, _ = eng.enhance(rgb, outscale=scale)
        res = Image.fromarray(out).convert("RGBA")
        alpha = img.getchannel("A").resize(res.size, Image_LANCZOS)
        res.putalpha(alpha)
        return res

    return esrgan


MANIFEST = "pack.txt"


def read_manifest(pack):
    """What the pack was made with, or {} if it never said."""
    out = {}
    try:
        with open(os.path.join(pack, MANIFEST)) as fh:
            for line in fh:
                if "=" in line:
                    k, v = line.strip().split("=", 1)
                    out[k] = v
    except OSError:
        pass
    return out


def write_manifest(pack, scale, upscaler, strength, complete):
    """The pack's own record of its settings, so the app can tell "the pack
    matches what is selected" from "the pack needs redoing" without guessing.
    Written once with complete=0 as the writing starts and again with
    complete=1 at the end, so a run stopped halfway leaves a pack that says so."""
    with open(os.path.join(pack, MANIFEST), "w") as fh:
        fh.write("scale=%d\nupscaler=%s\nstrength=%.2f\ncomplete=%d\nwritten=%s\n"
                 % (scale, upscaler, strength, 1 if complete else 0,
                    time.strftime("%Y-%m-%d %H:%M:%S")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="folder holding dump/ and pack/")
    ap.add_argument("--upscale", action="store_true", help="run the AI upscaler")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--model", default="RealESRGAN_x4plus")
    ap.add_argument("--ai", action="store_true",
                    help="use Real-ESRGAN (run tools/get_upscaler.py first)")
    ap.add_argument("--ai-strength", type=float, default=0.75,
                    help="how much model detail to lay over the original, 0..1")
    ap.add_argument("--gpu", type=int, default=0)
    ap.add_argument("--include-ui", action="store_true",
                    help="upscale UI/font textures too (usually a bad idea)")
    ap.add_argument("--only-missing", action="store_true",
                    help="leave textures already in the pack alone; decode and "
                         "upscale only the ones that are not there yet")
    ap.add_argument("--migrate-only", action="store_true",
                    help="give pre-hash dump and pack files their content-hash "
                         "names, then stop (this also happens at the start of "
                         "every run)")
    args = ap.parse_args()

    dump = os.path.join(args.dir, "dump")
    pack = os.path.join(args.dir, "pack")
    index = os.path.join(dump, "index.txt")
    if not os.path.isfile(index):
        print("no index.txt in %s - run the game with texture dumping on first" % dump)
        return 1
    os.makedirs(pack, exist_ok=True)

    # Migration first, so everything below only ever sees hashed names.
    migrate_pack(dump, pack)
    if args.migrate_only:
        print("DONE migrate-only", flush=True)
        return 0

    entries = []
    unhashed = 0
    for line in open(index):
        p = line.split()
        if len(p) >= 10:
            # <id>-<hash>: the id says where and what shape, the hash which pixels.
            entries.append(("%s-%s" % (p[0], p[9]), int(p[1]), int(p[2]), int(p[3]),
                            int(p[4]), int(p[5]), int(p[6]), int(p[7]), int(p[8])))
        elif len(p) == 9:
            unhashed += 1           # raw dump gone: cannot be hashed, so cannot be packed
    if unhashed:
        print("NOTE: %d index line(s) have no content hash and no raw dump - skipped"
              % unhashed, flush=True)
    # The game re-dumps a texture it has seen if the cache evicted it, so the
    # index can hold duplicates. Keep the first of each id+hash; two entries
    # that differ only in the hash are two different textures, and both stay.
    seen, uniq = set(), []
    for e in entries:
        if e[0] not in seen:
            seen.add(e[0])
            uniq.append(e)

    if Image is None:
        print("Pillow is required: pip install pillow")
        return 1

    # The Lanczos path only. With --ai the upscaling is done by the Real-ESRGAN
    # executable below and this object is never used - building it anyway
    # printed "Real-ESRGAN not installed - using Lanczos instead" (about the
    # Python package it looks for) at the top of every AI run, which read as
    # the AI pass being silently skipped. It was not.
    up = None
    if args.upscale and not args.ai:
        up = make_upscaler(args.model, args.scale)

    total = len(uniq)
    done = skipped = failed = ui = 0
    skips = {}
    pending = []   # (id, decoded image) awaiting upscale
    recovered = 0
    started = time.time()
    # A pack made with other settings cannot be topped up: the new textures
    # would not match the old ones. If the pack's own record disagrees with
    # what was asked for, every texture is redone, whatever the flag said.
    upscaler_name = "realesrgan" if args.ai else "lanczos"
    strength = args.ai_strength if args.ai else 0.0
    if args.only_missing:
        m = read_manifest(pack)
        if m:
            same = (m.get("scale") == str(args.scale) and m.get("upscaler") == upscaler_name
                    and abs(float(m.get("strength", "0")) - strength) < 0.005)
            if not same:
                print("NOTE: the pack was made at %sx with %s (strength %s); "
                      "the settings now are %dx with %s (strength %.2f) - redoing every texture"
                      % (m.get("scale"), m.get("upscaler"), m.get("strength"),
                         args.scale, upscaler_name, strength), flush=True)
                args.only_missing = False
            elif m.get("complete") != "1":
                # A run that stopped halfway (out of disk, cancelled, crashed)
                # leaves complete=0. Every texture it did write is whole and made
                # with these settings, so the run continues with what is missing.
                # It used to redo everything here, which after a failure 2,449
                # textures into a 9,418-texture run meant redoing all 22,026 at
                # 4x and rewriting the 104 GB already made.
                print("NOTE: the pack's last run was stopped halfway - continuing with the "
                      "textures still missing (a file cut short by the stop is redone)",
                      flush=True)
    # What is already in the pack, for --only-missing. A pack of ~6,000 takes
    # half an hour with the AI; the handful dumped since take minutes, and
    # redoing everything to get them was the only option before this.
    have_tex = set()
    if args.only_missing and os.path.isdir(pack):
        have_tex, cut_short = intact_pack_ids(pack)
        if cut_short:
            print("%d pack file(s) are shorter than their header says (a write cut short) "
                  "- they will be redone" % cut_short, flush=True)
    # A stopped run may have been REDOING an older pack: it rewrote the manifest
    # first and then overwrote the files one by one, so every file older than
    # that manifest was made by the run before it, at settings the manifest
    # no longer describes. Those are still to be done.
    if have_tex:
        m = read_manifest(pack)
        if m.get("complete") != "1" and m.get("written"):
            try:
                since = time.mktime(time.strptime(m["written"], "%Y-%m-%d %H:%M:%S"))
            except ValueError:
                since = None
            if since is not None:
                older = set()
                for tid in have_tex:
                    try:
                        if os.path.getmtime(os.path.join(pack, tid + ".tex")) < since - 1:
                            older.add(tid)
                    except OSError:
                        older.add(tid)
                if older:
                    print("NOTE: %d pack file(s) predate the run that was stopped halfway "
                          "- made with the settings before it, so they are redone"
                          % len(older), flush=True)
                    have_tex -= older
    reused = 0
    # Two steps, each reported as its own PROGRESS bar. Naming them lets the
    # app restart its bar and its clock at the second rather than showing 100%
    # and then a bar that starts again from nothing.
    if args.only_missing:
        print("PHASE 1/2 Checking %d dumped textures for ones not yet in the pack" % total,
              flush=True)
    else:
        print("PHASE 1/2 Decoding %d dumped textures" % total, flush=True)
    for tid, w, h, fmt, tiled, pitch, endian, dim, size in uniq:
        name = "%s_%dx%d_%s" % (tid, w, h, FMT_NAMES.get(fmt, "fmt%d" % fmt))
        done += 1
        print("PROGRESS %d %d %s" % (done, total, name), flush=True)

        if fmt not in FMT_INFO:
            skipped += 1
            continue
        if args.only_missing and tid in have_tex:
            reused += 1                 # in the pack already: leave it alone
            continue
        # Whether a texture is art is decided by shape and format alone, so
        # the render targets, fonts and HUD are turned away BEFORE decoding:
        # a full run used to decode every dump in pure Python to pack a
        # third of them, and the decode is nearly all of phase 1.
        reason = pack_reason(w, h, fmt)
        if reason and not args.include_ui:
            skips[reason] = skips.get(reason, 0) + 1
            ui += 1
            continue
        # The decoded PNG beside the raw dump is the SAME bytes a fresh decode
        # gives, so a run at new settings reuses it: the upscale is redone,
        # the decode is not. Phase 1 keeps the path, not the image - every
        # decoded texture held until phase 2 was gigabytes on a big dump.
        decoded = os.path.join(dump, name + ".png")
        key = (tid, name, w, h, fmt, tiled, pitch, endian)
        if os.path.isfile(decoded) and os.path.getsize(decoded) > 0:
            pending.append((tid, decoded, key))
            continue
        try:
            img = decode_dump(dump, *key)
        except FileNotFoundError:
            failed += 1
            continue
        except Exception as exc:                                # noqa: BLE001
            print("  decode failed for %s: %s" % (name, exc))
            failed += 1
            continue
        img.save(decoded)
        pending.append((tid, decoded, key))

    # Anything decoded on a PREVIOUS run whose raw .bin is gone.
    #
    # The dump folder holds both the guest bytes and the decoded PNGs, and the
    # two can get out of step - clearing the .bin files (or copying a dump
    # between machines) leaves the decoded art perfectly usable while the index
    # describes almost nothing. Recovering from the PNGs means a pack can be
    # rebuilt at a different scale or strength without replaying the game.
    # Everything the index listed was dealt with above, whatever the outcome
    # (packed, waiting, turned away, or already in the pack) - a PNG of one of
    # those is not a recovery, and counting a packed one again as 'already'
    # overstated that number by every PNG in the folder.
    handled = {e[0] for e in uniq} | {e[0] for e in pending}
    by_name = {v: k for k, v in FMT_NAMES.items()}
    for fn in sorted(os.listdir(dump)):
        m = DECODED_RE.match(fn)
        if not m:
            continue
        tid, w2, h2 = m.group(1), int(m.group(2)), int(m.group(3))
        fmt2 = by_name.get(m.group(4))
        if tid in handled or fmt2 is None:
            continue
        if args.only_missing and tid in have_tex:
            reused += 1
            continue
        if pack_reason(w2, h2, fmt2) and not args.include_ui:
            continue
        pending.append((tid, os.path.join(dump, fn), None))
        recovered += 1
    if recovered:
        print("recovered %d textures from previously decoded PNGs" % recovered)

    # Upscale and write. Batched for the AI path, which runs a GPU process.
    total_pack = len(pending)
    if reused:
        print("%d textures already in the pack, left as they are" % reused)
    if args.only_missing and total_pack == 0:
        print("nothing to do - every texture that can be enhanced is already in the pack")
    print("PHASE 2/2 Upscaling %d textures with %s at %dx"
          % (total_pack, "Real-ESRGAN" if args.ai else "Lanczos", args.scale),
          flush=True)
    if total_pack:
        write_manifest(pack, args.scale, upscaler_name, strength, complete=False)
    written = 0
    failed_at = failed
    if total_pack == 0:
        pass
    elif args.ai:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ai_upscale
        exe = ai_upscale.find_upscaler(args.dir)
        if not exe:
            print("FAILED the AI upscaler is not installed - use the Download "
                  "button, or run tools/get_upscaler.py")
            return 1
        print("AI upscaler: %s (detail strength %.2f)" % (exe, args.ai_strength))
        for base in range(0, total_pack, ai_upscale.CHUNK):
            part = []
            for entry in pending[base:base + ai_upscale.CHUNK]:
                try:
                    part.append((entry[0], open_pending(entry, dump)))
                except Exception as exc:                        # noqa: BLE001
                    print("  decode failed for %s: %s" % (entry[0], exc))
                    failed += 1
            got = ai_upscale.upscale_many(exe, part, args.scale, gpu=args.gpu)
            for tid, src in part:
                write_tex(os.path.join(pack, "%s.tex" % tid),
                          ai_upscale.blend(got[tid], src, args.ai_strength))
                written += 1
                print("PROGRESS %d %d %s" % (written + (failed - failed_at),
                                             total_pack, tid), flush=True)
    else:
        for i, entry in enumerate(pending, 1):
            tid = entry[0]
            print("PROGRESS %d %d %s" % (i, total_pack, tid), flush=True)
            try:
                img = open_pending(entry, dump)
            except Exception as exc:                            # noqa: BLE001
                print("  decode failed for %s: %s" % (tid, exc))
                failed += 1
                continue
            write_tex(os.path.join(pack, "%s.tex" % tid), up(img) if up else img)
            written += 1

    took = time.time() - started
    if total_pack or not read_manifest(pack):
        write_manifest(pack, args.scale, upscaler_name, strength, complete=True)
    for why, n in sorted(skips.items(), key=lambda kv: -kv[1]):
        print("  not packed - %-28s %d" % (why, n))
    # One line the app (and a person) can read the whole outcome from: how
    # many textures the tool considers art, how many it wrote this run, how
    # many it left alone, and how many it never packs by design.
    print("SUMMARY art=%d written=%d already=%d excluded=%d failed=%d"
          % (written + reused, written, reused, ui, failed))
    print("DONE decoded=%d skipped_format=%d skipped_ui=%d failed=%d in %.1fs"
          % (done - skipped - failed, skipped, ui, failed, took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
