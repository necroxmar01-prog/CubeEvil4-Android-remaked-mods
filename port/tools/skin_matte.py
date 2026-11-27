#!/usr/bin/env python3
"""A player archive's shine held down (DevJunior, 26.11.2026: "quita el tono aceitoso que queda en los mods", an oily
look): each model part's specular colour (cPolyHeader 0x10..0x12, the light's highlight) at most --max, its strength
(0x15, percent) at most --decay. The new hair's part had 112 grey at 141 % (taken over with the Remake Lite's
template), the disc's own parts reach 102 at 50..80 %. The textures are not touched.

    skin_matte.py SKIN.drs OUT.drs [--max 40] [--decay 60] [--entries 0,2,4,5,6]

Writes OUT.drs; prints the parts changed per entry. Disc and mod files never go into the repository.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_graft as G  # noqa: E402
import uhd_skin_convert as U  # noqa: E402


def matte(entry, cap, decay):
    b = bytearray(entry)
    n = 0
    for p in G.parts(entry):
        before = bytes(b[p + 0x10:p + 0x13]) + bytes(b[p + 0x15:p + 0x16])
        for k in (0x10, 0x11, 0x12):
            b[p + k] = min(b[p + k], cap)
        b[p + 0x15] = min(b[p + 0x15], decay)
        if bytes(b[p + 0x10:p + 0x13]) + bytes(b[p + 0x15:p + 0x16]) != before:
            n += 1
    return bytes(b), n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--max", type=int, default=40, help="the specular colour's largest value (0..255)")
    ap.add_argument("--decay", type=int, default=60, help="the specular strength's largest value (percent)")
    ap.add_argument("--entries", default="0,2,4,5,6", help="the model entries (Leon: body, head, hair, face, extra)")
    a = ap.parse_args(argv)
    arc = U.Archive(open(a.skin, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    said = []
    for k in (int(x) for x in a.entries.split(",")):
        if k >= len(entries) or not entries[k]:
            continue
        try:
            entries[k], n = matte(entries[k], a.max, a.decay)
        except Exception:  # not a model
            continue
        said.append("entry %d: %d parts" % (k, n))
    out = U.build_archive(arc, U.build_body(entries, arc.types))
    Path(a.out).write_bytes(out)
    print("matte (specular at most %d, %d %%): %s; %s: %d bytes" % (a.max, a.decay, ", ".join(said), a.out, len(out)))


if __name__ == "__main__":
    main()
