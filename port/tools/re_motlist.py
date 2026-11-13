#!/usr/bin/env python3
"""Reader for RE Engine motion lists (*.motlist.663: Resident Evil 4 (2023), mot versions 603 / 613).

For the RE9 melee finishers DevJunior brought (13.11.2026, "RE9 Melee - Hatchet - Finishers for Leon", a mod for
the 2023 game): its pair motions (Leon and the enemy) are read here and then retargeted to the GameCube skeletons
(re_motion_retarget.py). Only what that needs: the list's motions, each motion's bones (name, parent, rest
translation and rotation) and its per-bone tracks (translation, rotation, scale) decoded to keys.

Layout after alphaZomega's 010 Editor template (RE-Engine-010-Templates, RE_Engine_motlist.bt) and the codecs of
PredatorCZ's RevilLib (src/reng/codecs.cpp). Little-endian throughout.

    re_motlist.py FILE.motlist.663 [--dump]       # the motions, their bones and tracks (codec, keys)

Game files never go into the repository; this tool only reads the paths given.
"""
import argparse
import math
import struct
import sys

TRANSLATION, ROTATION, SCALE = 1, 2, 4


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def _f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def _wstr(b, o):
    end = o
    while end + 1 < len(b) and (b[end] or b[end + 1]):
        end += 2
    return b[o:end].decode("utf-16le", "replace")


def _qw(x, y, z):
    w = 1.0 - (x * x + y * y + z * z)
    return math.sqrt(w) if w > 0.0 else 0.0


class Track:
    """One track of a bone: keys (frame numbers) and values (3 floats, or quaternions x, y, z, w)."""

    def __init__(self, kind, flags, frames, values):
        self.kind, self.flags, self.frames, self.values = kind, flags, frames, values

    @property
    def codec(self):
        return (self.flags >> 12) & 0xFF

    def sample(self, frame):
        """The value at `frame` (linear between keys; quaternions normalised lerp, shortest way)."""
        f, v = self.frames, self.values
        if not f:
            return None
        if frame <= f[0]:
            return v[0]
        if frame >= f[-1]:
            return v[-1]
        lo, hi = 0, len(f) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if f[mid] <= frame:
                lo = mid
            else:
                hi = mid
        t = (frame - f[lo]) / float(f[hi] - f[lo])
        a, b = v[lo], v[hi]
        if len(a) == 4:
            if sum(p * q for p, q in zip(a, b)) < 0:
                b = tuple(-q for q in b)
            q = tuple(p + (r - p) * t for p, r in zip(a, b))
            n = math.sqrt(sum(c * c for c in q)) or 1.0
            return tuple(c / n for c in q)
        return tuple(p + (r - p) * t for p, r in zip(a, b))


def _decode(b, start, kind, flags, count, ind_offs, data_offs, unpack_offs):
    """Keys of one track. kind: TRANSLATION / ROTATION / SCALE."""
    ftype = (flags >> 20) & 0xF
    frames = []
    if ind_offs:
        o = start + ind_offs
        if ftype == 2:
            frames = list(b[o:o + count])
        elif ftype == 4:
            frames = list(struct.unpack_from("<%dH" % count, b, o))
        elif ftype == 5:
            frames = list(struct.unpack_from("<%dI" % count, b, o))
        else:
            raise ValueError("frame index type %d" % ftype)
    else:
        frames = list(range(count))
    un = struct.unpack_from("<8f", b, start + unpack_offs) if unpack_offs else (0.0,) * 8
    o = start + data_offs
    comp = flags & 0xFF000
    vals = []
    if kind == ROTATION:
        sc, base = un[0:4], un[4:8]
        for k in range(count):
            if comp == 0x00000:
                q = struct.unpack_from("<4f", b, o + 16 * k)
            elif comp in (0xB0000, 0xC0000):
                x, y, z = struct.unpack_from("<3f", b, o + 12 * k)
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x20000:
                d = _u16(b, o + 2 * k)
                x, y, z = (sc[i] * (((d >> (5 * i)) & 0x1F) / 31.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x30000:  # 8 bit
                x, y, z = (sc[i] * (b[o + 3 * k + i] / 255.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x40000:  # 10 bit
                d = _u32(b, o + 4 * k)
                x, y, z = (sc[i] * (((d >> (10 * i)) & 0x3FF) / 1023.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x50000:  # 13 bit, 5 bytes big-endian
                p = o + 5 * k
                d = int.from_bytes(b[p:p + 5], "big")
                x, y, z = (sc[i] * (((d >> (13 * i)) & 0x1FFF) / 8191.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x60000:  # 16 bit
                d = struct.unpack_from("<3H", b, o + 6 * k)
                x, y, z = (sc[i] * (d[i] / 65535.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x70000:  # 18 bit, 7 bytes big-endian
                p = o + 7 * k
                d = int.from_bytes(b[p:p + 7], "big")
                x, y, z = (sc[i] * (((d >> (18 * i)) & 0x3FFFF) / 262143.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp == 0x80000:  # 21 bit
                d = _u64(b, o + 8 * k)
                x, y, z = (sc[i] * (((d >> (21 * i)) & 0x1FFFFF) / 2097151.0) + base[i] for i in range(3))
                q = (x, y, z, _qw(x, y, z))
            elif comp in (0x21000, 0x22000, 0x23000):  # one axis, 16 bit
                d = _u16(b, o + 2 * k)
                a = un[1] + un[0] * (d / 65535.0)
                c = [0.0, 0.0, 0.0]
                c[(comp >> 12) - 0x21] = a
                q = (c[0], c[1], c[2], _qw(*c))
            elif comp in (0x31000, 0x32000, 0x33000, 0x41000, 0x42000, 0x43000):  # one axis, float
                a = _f32(b, o + 4 * k)
                c = [0.0, 0.0, 0.0]
                c[((comp >> 12) & 0xF) - 1] = a
                q = (c[0], c[1], c[2], _qw(*c))
            else:
                raise ValueError("rotation codec 0x%x" % comp)
            vals.append(q)
    else:
        for k in range(count):
            if comp == 0x00000:
                v = struct.unpack_from("<3f", b, o + 12 * k)
            elif comp == 0x20000:  # 5 bit
                d = _u16(b, o + 2 * k)
                v = tuple(un[i] * (((d >> (5 * i)) & 0x1F) / 31.0) + un[3 + i] for i in range(3))
            elif comp == 0x40000:  # 10 bit
                d = _u32(b, o + 4 * k)
                v = tuple(un[i] * (((d >> (10 * i)) & 0x3FF) / 1023.0) + un[3 + i] for i in range(3))
            elif comp == 0x80000:  # 21 bit
                d = _u64(b, o + 8 * k)
                v = tuple(un[i] * (((d >> (21 * i)) & 0x1FFFFF) / 2097151.0) + un[3 + i] for i in range(3))
            elif comp in (0x21000, 0x22000, 0x23000):  # one axis, 16 bit, the others fixed
                d = _u16(b, o + 2 * k)
                a = un[0] * (d / 65535.0) + un[1 + ((comp >> 12) - 0x21)]
                c = [un[1], un[2], un[3]]
                c[(comp >> 12) - 0x21] = a
                v = tuple(c)
            elif comp == 0x24000:  # all three axes the same, 16 bit
                d = _u16(b, o + 2 * k)
                a = un[0] * (d / 65535.0) + un[3]
                v = (a, a, a)
            elif comp in (0x31000, 0x32000, 0x33000, 0x41000, 0x42000, 0x43000):  # one axis, float
                a = _f32(b, o + 4 * k)
                c = [un[0], un[1], un[2]]
                c[((comp >> 12) & 0xF) - 1] = a
                v = tuple(c)
            elif comp in (0x34000, 0x44000):  # all three the same, float
                a = _f32(b, o + 4 * k)
                v = (a, a, a)
            else:
                raise ValueError("vector codec 0x%x" % comp)
            vals.append(v)
    return Track(kind, flags, frames, vals)


class Bone:
    def __init__(self, name, index, parent, trans, quat, hash_):
        self.name, self.index, self.parent, self.trans, self.quat, self.hash = \
            name, index, parent, trans, quat, hash_


class Motion:
    def __init__(self, name, frames, rate, bones, tracks):
        self.name, self.frames, self.rate, self.bones, self.tracks = name, frames, rate, bones, tracks


def read_motion(b, start, shared_bones=None):
    version = _u32(b, start)
    if b[start + 4:start + 8] != b"mot ":
        raise ValueError("no mot at 0x%x" % start)
    if version < 456:
        raise ValueError("mot version %d (only 456 and later)" % version)
    bone_hdr_offs = _u64(b, start + 16)
    clip_hdr_offs = _u64(b, start + 24)
    names_offs = _u64(b, start + 88)
    frames = _f32(b, start + 96)
    clip_count = _u16(b, start + 114)
    rate = _u16(b, start + 118) or _u16(b, start + 120)
    name = _wstr(b, start + names_offs) if names_offs else ""
    bones = shared_bones
    if bone_hdr_offs:
        hdr = start + bone_hdr_offs
        first, n = _u64(b, hdr), _u64(b, hdr + 8)
        if 0 < n <= 1000:
            bones, by_offs = [], {}
            for i in range(n):
                o = start + first + 80 * i
                by_offs[first + 80 * i] = i
                nm = _wstr(b, start + _u64(b, o))
                parent = _u64(b, o + 8)
                trans = struct.unpack_from("<3f", b, o + 32)
                quat = struct.unpack_from("<4f", b, o + 48)
                idx, h = struct.unpack_from("<II", b, o + 64)
                bones.append(Bone(nm, idx, parent, trans, quat, h))
            for bn in bones:
                bn.parent = by_offs.get(bn.parent, -1) if bn.parent else -1
    tracks = {}
    o = start + clip_hdr_offs
    for c in range(clip_count):
        bi, tflags, bhash, thdr = struct.unpack_from("<HHII", b, o + 12 * c)
        p = start + thdr
        per = {}
        for kind in (TRANSLATION, ROTATION, SCALE):
            if tflags & kind:
                fl, cnt, io, do, uo = struct.unpack_from("<5I", b, p)
                p += 20
                per[kind] = _decode(b, start, kind, fl, cnt, io, do, uo)
        tracks[(bi, bhash)] = per
    return Motion(name, frames, rate, bones, tracks)


def read_motlist(path):
    b = open(path, "rb").read()
    if b[4:8] != b"mlst":
        raise ValueError("%s: no motlist" % path)
    version = _u32(b, 0)
    pointers, count = _u64(b, 16), _u32(b, 48)
    out, bones = [], None
    for i in range(count):
        a = _u64(b, pointers + 8 * i)
        if not a or b[a + 4:a + 8] != b"mot ":
            continue
        m = read_motion(b, a, bones)
        bones = m.bones or bones
        if m.bones is None:
            m.bones = bones
        out.append(m)
    return version, out


def bone_name(m, key):
    bi, h = key
    if m.bones:
        for bn in m.bones:
            if bn.hash == h:
                return bn.name
        if bi < len(m.bones):
            return m.bones[bi].name
    return "#%d" % bi


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("motlist")
    ap.add_argument("--dump", action="store_true", help="every track: codec, keys, first and last value")
    a = ap.parse_args()
    version, mots = read_motlist(a.motlist)
    print("motlist version %d: %d motions" % (version, len(mots)))
    for m in mots:
        print("  %s: %.0f frames at %d/s, %d bones, %d tracks" % (m.name, m.frames, m.rate,
                                                                   len(m.bones or []), len(m.tracks)))
        if a.dump:
            for key, per in m.tracks.items():
                for kind, tr in per.items():
                    print("    %-12s %s codec 0x%02x keys %3d  %s .. %s" % (
                        bone_name(m, key), "TRS"[{1: 0, 2: 1, 4: 2}[kind]], tr.codec, len(tr.frames),
                        tuple(round(v, 3) for v in tr.values[0]), tuple(round(v, 3) for v in tr.values[-1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
