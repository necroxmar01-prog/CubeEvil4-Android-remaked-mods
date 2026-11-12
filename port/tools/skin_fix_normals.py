#!/usr/bin/env python3
"""Splits the shared normals of every model of a skin archive (skin_hair_fbx.split_shared_normals) without building
it anew (DevJunior, 12.11.2026: dark dots on the RE9 head, its eyes a mosaic: normals carried another bone's weight).

    skin_fix_normals.py IN.drs OUT.drs

Every other byte of the archive stays. Skins never go into the repository; this tool only reads and writes the paths
given.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_hair_fbx as H  # noqa: E402
import uhd_skin_convert as U  # noqa: E402


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        sys.exit(__doc__)
    gc = U.Archive(Path(argv[0]).read_bytes(), ">")
    entries = []
    for i in range(len(gc.offsets)):
        b = gc.entry(i)
        if gc.kind(i) == "BIN" and b:
            blob = bytearray(b)
            n = H.split_shared_normals(blob)
            if n:
                print("  entry %d: %d normals split by weight" % (i, n))
            b = bytes(blob)
        entries.append(b)
    out = U.build_archive(gc, U.build_body(entries, gc.types))
    Path(argv[1]).write_bytes(out)
    print("%s: %d bytes" % (argv[1], len(out)))


if __name__ == "__main__":
    main()
