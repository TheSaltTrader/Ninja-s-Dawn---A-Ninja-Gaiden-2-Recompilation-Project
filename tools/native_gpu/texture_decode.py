#!/usr/bin/env python3
"""Decode dumped Fable II textures (ngpu_dump_textures) to PNG, several ways.

    texture_decode.py <ngpu_textures dir> <out dir>

For every <base>_<w>x<h>_f<fmt>_e<endian>_<tiled|linear>_p<pitch>.src file
(the raw guest bytes) it writes PNGs of the DXT1/DXT5 (formats 18 / 20)
decode under different assumptions, so the right untiling / byte order can
be picked by eye:
  *_runtime.png   the runtime's own untiled rows (the .rows<pitch> file)
  *_linear.png    the raw bytes taken as linear rows, 8in16 swapped
  *_tiled.png     the raw bytes untiled with the Xenos formula (blocks), 8in16 swapped
  *_tiled_noswap.png  the same without the byte swap
Requires Pillow.
"""
import os
import re
import struct
import sys

from PIL import Image


def tiled_outer(y, width, log2_bpp):
    macro = ((y >> 5) * (width >> 5)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    return macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)


def tiled_inner(x, y, log2_bpp, base):
    macro = (x >> 5) << (log2_bpp + 7)
    micro = (x & 7) << log2_bpp
    off = base + (macro + ((micro & ~15) << 1) + (micro & 15))
    return ((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)


def swap16(b):
    out = bytearray(len(b))
    out[0::2] = b[1::2]
    out[1::2] = b[0::2]
    return bytes(out)


def c565(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31


def decode_bc1_block(blk, px, x0, y0, w, h):
    c0, c1, idx = struct.unpack_from("<HHI", blk, 0)
    a, b = c565(c0), c565(c1)
    if c0 > c1:
        pal = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)), tuple((a[i] + 2 * b[i]) // 3 for i in range(3))]
    else:
        pal = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)), (0, 0, 0)]
    for j in range(4):
        for i in range(4):
            x, y = x0 + i, y0 + j
            if x < w and y < h:
                px[x, y] = pal[(idx >> (2 * (j * 4 + i))) & 3]


def decode(data, w, h, fmt, bpb, out_path):
    img = Image.new("RGB", (w, h))
    px = img.load()
    bw = (w + 3) // 4
    for by in range((h + 3) // 4):
        for bx in range(bw):
            o = (by * bw + bx) * bpb
            blk = data[o:o + bpb]
            if len(blk) < bpb:
                continue
            if fmt == 18:
                decode_bc1_block(blk, px, bx * 4, by * 4, w, h)
            else:  # DXT5: colour part is the last 8 bytes
                decode_bc1_block(blk[8:], px, bx * 4, by * 4, w, h)
    img.save(out_path)


def untile(src, bw, bh, pitch_blocks, bpb, log2_bpp):
    out = bytearray(bw * bh * bpb)
    for by in range(bh):
        outer = tiled_outer(by, pitch_blocks, log2_bpp)
        for bx in range(bw):
            so = tiled_inner(bx, by, log2_bpp, outer)
            out[(by * bw + bx) * bpb:(by * bw + bx + 1) * bpb] = src[so:so + bpb]
    return bytes(out)


def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    for f in sorted(os.listdir(src_dir)):
        m = re.match(r"([0-9A-F]+)_(\d+)x(\d+)_f(\d+)_e(\d+)_(tiled|linear)_p(\d+)\.src$", f)
        if not m:
            continue
        base, w, h, fmt, endian, tiling, pitch = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6), int(m.group(7))
        if fmt not in (6, 18, 20):
            continue
        if fmt == 6:
            # 8_8_8_8: 1x1 blocks of 4 bytes; four byte-order guesses of the untiled data
            raw = open(os.path.join(src_dir, f), "rb").read()
            pitch_px = max(pitch, w)
            if tiling == "tiled":
                pitch_px = (pitch_px + 31) & ~31
            til = untile(raw, w, h, pitch_px, 4, 2) if tiling == "tiled" else b"".join(raw[y * pitch_px * 4:(y * pitch_px + w) * 4] for y in range(h))
            stem = os.path.join(out_dir, f"{base}_{w}x{h}_f6")
            for tag, order in (("rgba", (0, 1, 2, 3)), ("bgra", (2, 1, 0, 3)), ("argb", (1, 2, 3, 0)), ("abgr", (3, 2, 1, 0))):
                img = Image.new("RGB", (w, h))
                px = img.load()
                for y in range(h):
                    for x in range(w):
                        o = (y * w + x) * 4
                        px[x, y] = (til[o + order[0]], til[o + order[1]], til[o + order[2]])
                img.save(stem + f"_{tiling}_{tag}.png")
            print("decoded", f, "(8888)")
            continue
        bpb = 8 if fmt == 18 else 16
        log2 = 3 if fmt == 18 else 4
        raw = open(os.path.join(src_dir, f), "rb").read()
        bw, bh = (w + 3) // 4, (h + 3) // 4
        pitch_blocks = max(pitch // 4, bw)
        pitch_blocks = (pitch_blocks + 31) & ~31
        stem = os.path.join(out_dir, f"{base}_{w}x{h}_f{fmt}")
        rows = [r for r in os.listdir(src_dir) if r.startswith(f[:-4]) and ".rows" in r]
        if rows:
            rb = int(rows[0].split(".rows")[1])
            rt = open(os.path.join(src_dir, rows[0]), "rb").read()
            packed = b"".join(rt[y * rb:y * rb + bw * bpb] for y in range(bh))
            decode(packed, w, h, fmt, bpb, stem + "_runtime.png")
        lin = b"".join(raw[(y * pitch_blocks) * bpb:(y * pitch_blocks + bw) * bpb] for y in range(bh))
        decode(swap16(lin), w, h, fmt, bpb, stem + "_linear.png")
        til = untile(raw, bw, bh, pitch_blocks, bpb, log2)
        decode(swap16(til), w, h, fmt, bpb, stem + "_tiled.png")
        decode(til, w, h, fmt, bpb, stem + "_tiled_noswap.png")
        print("decoded", f)


if __name__ == "__main__":
    main()
