"""Load a retail XEX2 as a flat, decrypted, decompressed memory image.

The recompiler does this internally, but the analysis tools here need the same
view to look at instructions by guest address. Results are cached next to the
project so repeated runs are cheap.

    from xex_image import XexImage
    img = XexImage.load("assets/default.xex")
    img.word(0x83846050)          # -> 0x81430000
"""

import os
import struct

# Public retail key, used by every Xbox 360 loader and emulator. Devkit XEXs
# use an all-zero key instead.
RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])

HDR_FILE_FORMAT_INFO = 0x000003FF
HDR_IMAGE_BASE_ADDRESS = 0x00010201
HDR_ENTRY_POINT = 0x00010100


class XexImage:
    def __init__(self, data, image_base, entry_point, sections):
        self.data = data
        self.image_base = image_base
        self.entry_point = entry_point
        self.sections = sections  # list of (name, va, vsize)

    # -- accessors -------------------------------------------------------

    def offset(self, va):
        return va - self.image_base

    def word(self, va):
        return struct.unpack_from(">I", self.data, self.offset(va))[0]

    def contains(self, va):
        return 0 <= va - self.image_base < len(self.data)

    def section_of(self, va):
        for name, base, size in self.sections:
            if base <= va < base + size:
                return name
        return None

    # -- loading ---------------------------------------------------------

    @classmethod
    def load(cls, xex_path, cache_path=None):
        if cache_path is None:
            cache_path = os.path.join(os.path.dirname(xex_path) or ".",
                                      "..", "out", "image.bin")
        cache_path = os.path.abspath(cache_path)
        meta_path = cache_path + ".meta"

        if (os.path.exists(cache_path) and os.path.exists(meta_path)
                and os.path.getmtime(cache_path) >= os.path.getmtime(xex_path)):
            with open(meta_path, encoding="utf-8") as f:
                base, entry = (int(x, 16) for x in f.readline().split())
                sections = []
                for line in f:
                    name, va, size = line.split()
                    sections.append((name, int(va, 16), int(size)))
            with open(cache_path, "rb") as f:
                return cls(f.read(), base, entry, sections)

        image, base, entry, sections = cls._decode(xex_path)
        os.makedirs(os.path.dirname(cache_path), exist_ok=True)
        with open(cache_path, "wb") as f:
            f.write(image)
        with open(meta_path, "w", encoding="utf-8") as f:
            f.write(f"{base:X} {entry:X}\n")
            for name, va, size in sections:
                f.write(f"{name} {va:X} {size}\n")
        return cls(image, base, entry, sections)

    @staticmethod
    def _decode(xex_path):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

        d = open(xex_path, "rb").read()
        if d[:4] != b"XEX2":
            raise SystemExit(f"{xex_path}: not an XEX2 file")

        _flags, pe_off, _res, sec_off, opt_count = struct.unpack_from(">IIIII", d, 4)
        hdrs = {}
        off = 0x18
        for _ in range(opt_count):
            k, v = struct.unpack_from(">II", d, off)
            off += 8
            hdrs[k] = v

        info_size, encryption, compression = struct.unpack_from(
            ">IHH", d, hdrs[HDR_FILE_FORMAT_INFO])
        if compression not in (0, 1):
            raise SystemExit(
                f"compression type {compression} (only none/basic handled here)")

        body = d[pe_off:]
        if encryption:
            # The per-title key is stored in the security header, itself
            # encrypted with the retail key (single ECB block). The image is
            # then CBC with a zero IV under that key.
            wrapped = d[sec_off + 0x150:sec_off + 0x160]
            ecb = Cipher(algorithms.AES(RETAIL_KEY), modes.ECB()).decryptor()
            session = ecb.update(wrapped) + ecb.finalize()
            cbc = Cipher(algorithms.AES(session), modes.CBC(b"\0" * 16)).decryptor()
            body = cbc.update(body) + cbc.finalize()

        if compression == 1:
            # "Basic" compression is run-length over zero pages: a list of
            # (data_size, zero_size) pairs describing the image layout.
            blocks = [struct.unpack_from(">II", d, hdrs[HDR_FILE_FORMAT_INFO] + 8 + i * 8)
                      for i in range((info_size - 8) // 8)]
            out = bytearray()
            p = 0
            for data_size, zero_size in blocks:
                out += body[p:p + data_size]
                p += data_size
                out += b"\0" * zero_size
            image = bytes(out)
        else:
            image = body

        base = hdrs.get(HDR_IMAGE_BASE_ADDRESS, 0x82000000)
        entry = hdrs.get(HDR_ENTRY_POINT, 0)

        sections = []
        if image[:2] == b"MZ":
            e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
            nsec = struct.unpack_from("<H", image, e_lfanew + 6)[0]
            optsz = struct.unpack_from("<H", image, e_lfanew + 20)[0]
            so = e_lfanew + 24 + optsz
            for i in range(nsec):
                o = so + i * 40
                name = image[o:o + 8].rstrip(b"\0").decode("latin1")
                vsize, va = struct.unpack_from("<II", image, o + 8)
                sections.append((name, base + va, vsize))

        return image, base, entry, sections


def pdata_functions(img):
    """Function start addresses from the .pdata unwind table."""
    for name, va, size in img.sections:
        if name == ".pdata":
            out = []
            for i in range(0, size & ~7, 8):
                a = struct.unpack_from(">I", img.data, img.offset(va) + i)[0]
                if a:
                    out.append(a)
            return sorted(out)
    return []


if __name__ == "__main__":
    import sys
    img = XexImage.load(sys.argv[1] if len(sys.argv) > 1 else "assets/default.xex")
    print(f"image base 0x{img.image_base:08X}  entry 0x{img.entry_point:08X}  "
          f"{len(img.data):,} bytes")
    for name, va, size in img.sections:
        print(f"  {name:<10} 0x{va:08X} {size:>12,}")
    print(f"{len(pdata_functions(img)):,} .pdata functions")
