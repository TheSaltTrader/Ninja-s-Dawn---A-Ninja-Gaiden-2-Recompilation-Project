#!/usr/bin/env python3
"""Build the runtime shader manifest: full-program hash -> translated artefact.

WHY BY CONTENT AND NOT BY ADDRESS. The containers were extracted from a guest
memory dump and are named for their offset in it (ng2_8200AC88), while the
runtime IM_LOAD addresses are physical (1C464000). Those are different address
spaces and there is no reliable mapping between them from outside. The microcode
itself is the same bytes in both, so content is the key that survives.

WHY THE WHOLE PROGRAM AND NOT A PREFIX. Measured here: the first code dword of
the first shader the game loads, 0x30052003, is shared by FIFTEEN of the 625
containers. Xenos microcode prologues are highly repetitive, and this project
has already had one false finding from a 48-byte needle colliding. The rule
recorded from that: match on at least 256 bytes at 98% or better. Hashing the
full program is strictly stronger and costs nothing here, because the draw
record carries the program's dword count.

The manifest is a flat text file so the runtime loader needs no parser:
    <hash16>  <dwords>  <v|p>  <artefact name>

Usage: make_shader_manifest.py <container dir> <dxil dir> <out manifest>
                               [<container dir>=<dxil dir> ...]

The extra pairs exist because the containers come from TWO places and must land
in ONE manifest: the 625 extracted from the memory dump, and the ones
SYNTHESISED from microcode dumped at runtime for shaders that have no container
at all (only 54 of the 87 a frame loads do). Both are keyed by the same content
hash, so they merge without a second lookup path - which is the whole point.
"""
import os
import struct
import sys


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def code_region(path):
    """Return (bytes_of_code, is_pixel) or None."""
    d = open(path, "rb").read()
    if len(d) < 0x24:
        return None
    flags, vsize, psize = struct.unpack_from(">3I", d, 0)
    if (flags & 0xFFFFFF00) != 0x102A1100:
        return None
    shoff = struct.unpack_from(">I", d, 0x18)[0]
    if shoff + 8 > len(d):
        return None
    po, size = struct.unpack_from(">2I", d, shoff)
    at = vsize + po
    if at + size > len(d) or size == 0:
        return None
    # isPixelShader = (flags & 1) == 0
    return d[at:at + size], (flags & 1) == 0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    containers, dxil_dir, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    pairs = [(containers, dxil_dir)]
    for extra in sys.argv[4:]:
        if "=" not in extra:
            print("extra sources are <container dir>=<dxil dir>, got: %s" % extra)
            return 2
        c, _, d = extra.partition("=")
        pairs.append((c, d))

    rows, skipped, no_artefact = [], 0, 0
    superseded = []
    by_hash = {}
    per_source = []
    for containers, dxil_dir in pairs:
        have_dxil = set()
        if os.path.isdir(dxil_dir):
            have_dxil = {f[:-5] for f in os.listdir(dxil_dir) if f.endswith(".dxil")}
        if not os.path.isdir(containers):
            print("no container dir: %s" % containers)
            return 2
        before = len(rows)
        for name in sorted(os.listdir(containers)):
            if not name.endswith(".xvu"):
                continue
            r = code_region(os.path.join(containers, name))
            if r is None:
                skipped += 1
                continue
            code, is_pixel = r
            stem = name[:-4]
            if stem not in have_dxil:
                no_artefact += 1
                continue
            h = fnv1a64(code)
            # A collision here would mean two DIFFERENT programs hashing the
            # same, which would silently serve the wrong shader - exactly the
            # failure the whole-program hash exists to prevent. Say so rather
            # than overwrite.
            if h in by_hash and by_hash[h][0] != code:
                print("COLLISION: %016X shared by %s and %s" % (h, by_hash[h][1], stem))
            # THE FIRST SOURCE WINS - AND FOR FETCHLESS CONTAINERS THE REBUILT
            # ONE MUST COME FIRST. This preference was originally the other way
            # round, and the audit refuted it: run against a patched XenosRecomp
            # that REFUSES a fetch-element miss instead of dereferencing end(),
            #
            #     originals : 625 containers, REFUSED 279
            #     rebuilt   : 330 containers, REFUSED   0
            #
            # and every one of the 279 is a container synth_fetchless rebuilds.
            # A refusal means the unpatched binary emitted a shader assembled
            # from whatever byte followed the end iterator, so those 279 entries
            # in the manifest are WRONG TRANSLATIONS THAT LOOK FINE. Preferring
            # the original for them is preferring the corrupt artefact.
            #
            # Pass synth_fetchless BEFORE the xvu directory for that reason.
            # Everything outside those 279 is unaffected and the real container
            # still wins, because the rebuild only covers fetchless-with-vfetch.
            #
            # Three other programs synthesised from runtime dumps turned out to
            # HAVE containers all along - they had merely failed to translate
            # under the wrong shader_common header.
            #
            # That an EMPTY table is not a truer table was already noted here
            # and I still drew the wrong conclusion from it: all three declare
            # vertexElementCount == 0 while their microcode contains 3 to 6
            # vfetch instructions. I recorded that the real container's table is
            # empty and then preferred it anyway, on the grounds that it is "the
            # game's own data". An empty table is precisely what makes the
            # lookup miss, so that was preferring the input that causes the bug.
            #
            # The runtime loader assigns g_manifest[hash] per row, so a
            # duplicate row would silently overwrite whichever came first;
            # skipping it here is what makes the preference real rather than an
            # accident of file order.
            if h in by_hash:
                superseded.append((stem, by_hash[h][1]))
                continue
            by_hash.setdefault(h, (code, stem))
            # THE PATH, NOT THE STEM. Merging two container sources into one
            # manifest made a bare stem ambiguous about WHICH dxil directory
            # holds it, and the runtime would have needed a search rule - one
            # more place for the two sources to be treated differently. The
            # path is unambiguous and the runtime needs no rule at all.
            dxil = os.path.join(dxil_dir, stem + ".dxil").replace("\\", "/")
            rows.append((h, len(code) // 4, "p" if is_pixel else "v", dxil))
        per_source.append((containers, len(rows) - before))

    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        f.write("# full-program FNV-1a-64 of Xenos microcode -> translated artefact\n")
        f.write("# hash              dwords stage name\n")
        for h, dwords, stage, stem in rows:
            f.write("%016X %6d %s %s\n" % (h, dwords, stage, stem))

    for src, n in per_source:
        print("  from %-44s %4d" % (src, n))
    print("containers scanned : %d" % (len(rows) + skipped + no_artefact))
    print("  unparseable      : %d" % skipped)
    print("  no .dxil artefact: %d" % no_artefact)
    print("  IN MANIFEST      : %d  (%d vertex, %d pixel)"
          % (len(rows), sum(1 for r in rows if r[2] == "v"), sum(1 for r in rows if r[2] == "p")))
    print("  distinct hashes  : %d" % len(by_hash))
    if superseded:
        print("  SUPERSEDED       : %d (same program from a later source, not emitted)" % len(superseded))
        for later, first in superseded[:8]:
            print("     %-28s already covered by %s" % (later, first))
    print("written: %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
