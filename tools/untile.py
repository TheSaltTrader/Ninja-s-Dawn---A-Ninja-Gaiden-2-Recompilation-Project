"""Turn a raw Xbox 360 tiled 32bpp surface dump into a PNG.

`ng2DiagFrameTick` copies the GPU resolve targets straight out of guest memory,
so the bytes are still in the console's tiled layout and big-endian. This undoes
both, which is the only way to actually look at a frame: PrintWindow cannot read
a D3D12 swapchain, and screen-grabbing the window region photographs whatever
happens to be on top of it.

    python tools/untile.py out/frame_A.bin out/frame_A.png
    python tools/untile.py out/frame_A.bin out/frame_A.png --order BGRA
"""

import argparse
import struct
import sys
import zlib


def tiled_texel_offset(x, y, width, texel_pitch):
    """XGAddress2DTiledOffset - texel index of (x, y) in a tiled surface."""
    aligned_width = (width + 31) & ~31
    log_bpp = (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2))
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    offset = (macro + ((micro & ~15) << 1) + (micro & 15) +
              ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4))
    return ((((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) +
             ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))
            >> log_bpp)


def write_png(path, width, height, rgb_rows):
    """Minimal PNG writer - no Pillow dependency."""
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c +
                struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--order", default="ARGB",
                    help="channel order of each stored 32-bit texel (default ARGB)")
    ap.add_argument("--linear", action="store_true",
                    help="treat the dump as already untiled")
    args = ap.parse_args()

    data = open(args.src, "rb").read()
    need = args.width * args.height * 4
    if len(data) < need:
        sys.exit(f"{args.src}: {len(data)} bytes, need {need}")

    ri = args.order.index("R")
    gi = args.order.index("G")
    bi = args.order.index("B")

    rows = []
    for y in range(args.height):
        row = bytearray(args.width * 3)
        for x in range(args.width):
            if args.linear:
                o = (y * args.width + x) * 4
            else:
                o = tiled_texel_offset(x, y, args.width, 4) * 4
            if o + 4 > len(data):
                continue
            texel = data[o:o + 4]
            p = x * 3
            row[p] = texel[ri]
            row[p + 1] = texel[gi]
            row[p + 2] = texel[bi]
        rows.append(bytes(row))

    write_png(args.dst, args.width, args.height, rows)
    nonzero = sum(1 for r in rows if any(r))
    print(f"wrote {args.dst}  ({nonzero}/{args.height} rows non-black)")


if __name__ == "__main__":
    sys.exit(main())
