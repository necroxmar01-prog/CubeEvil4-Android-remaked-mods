#!/usr/bin/env python3
"""Converts motions (FCV) of the PC release (Resident Evil 4 UHD mods, little-endian) to the GameCube's form and
packs them as the melee archive of a mod (wish of DevJunior, 09.11.2026: custom melee animations).

    uhd_motion_convert.py OUT/melee/melee.dat --punch "Super Punch.fcv" --kick "Super Kick.fcv" [--punch ... --kick ...]
    uhd_motion_convert.py --check GC.drs PC.udas          (every motion both archives hold: converted == disc)

MotionData (port/whole/runtime/formats/formats_model.cpp convertMotionAt): u16 maxFrame, u8 joints, u16 kind[n]
(unaligned, at 3), u8 parts[n], pad to 4, u32 size (the motion's length; big-endian on the PC as well), u32 offsets[n] of the joints'
curves; a joint has three axes of u16 count, u16 frames[count] and count keys of the kind's size (kind >> 12: 12,
8, 6, -, 10, 6, 4, -, 9, 5, 3 bytes: a value and two slopes, KEY_WORDS). The PC stores every word little-endian.

The melee archive (runtime/melee.cpp reads mods/<pack>/melee/melee.dat): a DAT of FCV entries, the even ones moves on
a standing enemy (the kick's place, enemy archives' entry 0x29D), the odd ones on a kneeling one (Leon's archive,
entry 0x25), up to 8 of each (--punch / --kick repeated); an empty entry is none. Entry 16 (--box) is the box strike's
motion (patch 0916), played from START to END, the box breaking on HIT; entry 17 ("BXF") holds the three as bytes. The game draws each move among
them and its own (the app's random mix). Mod and disc files never go into the repository."""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402

KEY_SIZE = [12, 8, 6, 0, 10, 6, 4, 0, 9, 5, 3, 0, 0, 0, 0, 4]
# element widths of one key by its kind (kind >> 12): a value and two slopes, each a float, an s16 or a byte; how the
# PC's little-endian words are swapped. Derived from the 62 motions Leon's archive shares with the PC's (every key
# of every kind agrees: --check); kind 15 does not occur there.
KEY_WORDS = {0: (4, 4, 4), 1: (4, 2, 2), 2: (4, 1, 1), 4: (2, 4, 4), 5: (2, 2, 2), 6: (2, 1, 1), 8: (1, 4, 4),
             9: (1, 2, 2), 10: (1, 1, 1), 15: (4,)}


def convert(pc, key_words=KEY_WORDS, fix_size=True):
    """A PC motion as the GameCube's (big-endian) bytes."""
    out = bytearray(pc)
    n = pc[2]
    table = (3 + n * 3 + 3) & ~3

    def swap(at, width, count=1):
        for k in range(count):
            o = at + k * width
            out[o:o + width] = pc[o:o + width][::-1]

    swap(0, 2)
    swap(3, 2, n)
    swap(table + 4, 4, n)  # the size word before the offsets is big-endian on the PC too
    # the size word: the motion's length on the disc (Leon's 62 all); tools for the PC leave another motion's there
    # (Jake Muller's custom moves: 6272 for 37008 bytes), and the app's reader takes it for the bounds
    if fix_size and n:
        struct.pack_into(">I", out, table, len(pc))
    kinds = struct.unpack_from("<%dH" % n, pc, 3)
    offs = struct.unpack_from("<%dI" % n, pc, table + 4)
    seen = set()
    for kind, off in zip(kinds, offs):
        key = KEY_SIZE[kind >> 12]
        if off in seen or key == 0 or off >= len(pc):
            continue
        seen.add(off)
        p = off
        for _ in range(3):
            if p + 2 > len(pc):
                break
            count = struct.unpack_from("<H", pc, p)[0]
            if p + 2 + count * 2 + count * key > len(pc):
                break
            swap(p, 2, 1 + count)
            q = p + 2 + count * 2
            for _k in range(count):
                for w in key_words[kind >> 12]:
                    if w > 1:
                        swap(q, w)
                    q += w
            p = q
    return bytes(out)


def check(gc_path, pc_path):
    gc = U.Archive(Path(gc_path).read_bytes(), ">")
    pc = U.Archive(Path(pc_path).read_bytes(), "<")
    same = diff = 0
    for i in range(min(len(gc.offsets), len(pc.offsets))):
        if gc.types[i] != b"FCV\x00" and gc.types[i][:3] != b"FCV":
            continue
        a, b = gc.entry(i), pc.entry(i)
        if not a or len(a) != len(b):
            continue
        c = convert(b)
        if c == a:
            same += 1
        else:
            diff += 1
            k = next(j for j in range(len(a)) if a[j] != c[j])
            if diff <= 5:
                print("entry 0x%x differs at 0x%x of 0x%x: disc %s, converted %s (kinds %s)" % (
                    i + 4, k, len(a), a[k - 4:k + 8].hex(), c[k - 4:k + 8].hex(),
                    sorted({x >> 12 for x in struct.unpack_from("<%dH" % b[2], b, 3)})))
    print("%d motions equal to the disc's, %d different" % (same, diff))
    return diff == 0


def trim_tail(gc, end):
    """A GameCube motion (convert's result) ending on frame `end`: its last frame set to it and, per axis, the keys
    after the first one at or past it left out (the curves up to `end` stay as they were; the box strike stops there
    and the archive keeps its place: RE4_MEM1_MELEE_MAX)."""
    n = gc[2]
    kinds = struct.unpack_from(">%dH" % n, gc, 3)
    o = (3 + 3 * n + 3) & ~3
    offs = struct.unpack_from(">%dI" % n, gc, o + 4)
    head = bytearray(gc[:o + 4 + 4 * n])
    struct.pack_into(">H", head, 0, end)
    curves = bytearray()
    new_offs = []
    for j in range(n):
        fmt = kinds[j] >> 12
        size = KEY_SIZE[fmt]
        p = offs[j]
        new_offs.append(len(head) + len(curves))
        for _ in range(3):
            c = struct.unpack_from(">H", gc, p)[0]
            frames = struct.unpack_from(">%dH" % c, gc, p + 2)
            keys = gc[p + 2 + 2 * c:p + 2 + 2 * c + size * c]
            keep = next((i + 1 for i, f in enumerate(frames) if f >= end), c)
            curves += struct.pack(">H", keep) + struct.pack(">%dH" % keep, *frames[:keep]) + keys[:size * keep]
            p += 2 + 2 * c + size * c
        curves += bytes((-len(curves)) % 4)
    out = head + curves
    struct.pack_into(">I", out, o, len(out))
    struct.pack_into(">%dI" % n, out, o + 4, *new_offs)
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--punch", action="append", default=[], help="PC motion for a standing enemy (the kick's place)")
    ap.add_argument("--kick", action="append", default=[], help="PC motion for a kneeling enemy")
    ap.add_argument("--box", help="PC motion for the box strike (patch 0916; wish of DevJunior, 12.11.2026: Jake's "
                    "low kick to break barrels, without the frames where he raises his hand)")
    ap.add_argument("--box-frames", default="4:20:27", metavar="START:HIT:END",
                    help="the box strike plays the --box motion from START, breaks the box on HIT, ends on END")
    ap.add_argument("--check", nargs=2, metavar=("GC.drs", "PC.udas"))
    a = ap.parse_args(argv)
    if a.check:
        sys.exit(0 if check(*a.check) else 1)
    if len(a.punch) > 8 or len(a.kick) > 8 or not (a.punch or a.kick or a.box):
        sys.exit("one to eight motions of each kind")
    entries = []
    for k in range(max(len(a.punch), len(a.kick))):
        for moves in (a.punch, a.kick):
            entries.append(convert(Path(moves[k]).read_bytes()) if k < len(moves) else b"")
    frames = None
    if a.box:  # entry 16, after the eight places of each kind; its frames in the header's bytes 4..8 ("BX" s h e)
        box = convert(Path(a.box).read_bytes())
        frames = [int(x) for x in a.box_frames.split(":")]
        last = struct.unpack_from(">H", box, 0)[0]
        if not (0 <= frames[0] < frames[1] < frames[2] <= last <= 255):
            sys.exit("--box-frames: START < HIT < END <= %d (the motion's last frame)" % last)
        box = trim_tail(box, frames[2])
        entries += [b""] * (16 - len(entries)) + [box, bytes(frames) + bytes(13)]
    tags = [b"FCV\x00"] * len(entries)
    if frames:
        tags[17] = b"BXF\x00"  # the box strike's frames: no converter, read as bytes
    body = bytearray(U.build_body(entries, tags))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(body)
    print("%s: %d bytes, %d moves on a standing enemy, %d on a kneeling one%s" % (
        a.out, len(body), len(a.punch), len(a.kick),
        ", the box strike (frames %d..%d, the box breaks on %d)" % (frames[0], frames[2], frames[1]) if frames else ""))

if __name__ == "__main__":
    main()
