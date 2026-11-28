#!/usr/bin/env python3
"""The face curves of a disc event (wish of DevJunior, 28.11.2026: compare a skin's cutscene faces with the original's):
the event's pl0000/face/ev0001_*.fcv files (ShapeData, include/model.h; read as game/shape.cpp CalculateShape_new
and game/motion.cpp HermiteInterpolation do): per channel its shape slot, active flag, key format and the y axis'
keys (the weight in percent). weights_at() gives the weights of every active slot at a frame (keys interpolated
linearly; the game uses Hermite tangents, close enough to pick poses).

    event_face_curves.py r100s03.evd            lists the curves
Disc files never go into the repository."""
import re
import struct
import sys

import numpy as np

KEY_SIZE = {0: 12, 1: 8, 2: 6, 4: 10, 5: 6, 6: 4, 8: 9, 9: 5, 10: 3, 15: 4}


def _value(fmt, b, i):
    o = i * KEY_SIZE[fmt]
    if fmt in (0, 1, 2, 15):
        return struct.unpack_from(">f", b, o)[0]
    if fmt in (4, 5, 6):
        return struct.unpack_from(">h", b, o)[0] * 0.0001
    return struct.unpack_from(">b", b, o)[0] * 0.0001


def read(path, actor=b"pl0000/face/ev0001"):
    """{file name: (frames, [(slot, active, fmt, (frames, values)), ...])} of the event's face curves."""
    d = open(path, "rb").read()
    out = {}
    for m in re.finditer(rb"event/[a-z0-9/]+/" + actor + rb"_[a-z0-9_]+\.fcv\x00", d):
        off, size = struct.unpack_from(">II", d, m.start() + 0x30)
        if not (0 < off < len(d) and 0 < size < 0x100000):
            continue
        b = d[off:off + size]
        nf, num = struct.unpack_from(">H", b, 0)[0] & 0x3FFF, b[2]
        flags = [struct.unpack_from(">H", b, 3 + 2 * i)[0] for i in range(num)]
        idx = list(b[3 + 2 * num:3 + 3 * num])
        p = ((3 + 3 * num + 3) & ~3) + 4
        chans = []
        for i in range(num):
            fmt, q = flags[i] >> 12, struct.unpack_from(">i", b, p + 4 * i)[0]
            axes = []
            for _ax in range(3):
                n = struct.unpack_from(">H", b, q)[0]
                fr = [struct.unpack_from(">H", b, q + 2 + 2 * k)[0] for k in range(n)]
                data = b[q + 2 + 2 * n:]
                axes.append((fr, [_value(fmt, data, k) for k in range(n)]))
                q += 2 + 2 * n + n * KEY_SIZE[fmt]
            chans.append((idx[i], bool(flags[i] & 4), fmt, axes[1]))
        out[m.group()[:-1].decode()] = (nf, chans)
    return out


def weights_at(curves, name, frame):
    """{slot: weight (1 = 100 %)} of the active channels of `name` at `frame`."""
    _nf, chans = curves[name]
    w = {}
    for slot, act, _fmt, (fr, vals) in chans:
        if act and slot:
            w[slot] = float(np.interp(frame, np.array(fr, float), np.array(vals))) / 100.0
    return w


if __name__ == "__main__":
    for name, (nf, chans) in sorted(read(sys.argv[1]).items()):
        print("%s: %d frames" % (name, nf))
        for slot, act, fmt, (_fr, vals) in chans:
            if act:
                print("  slot %2d: %3d keys, %7.1f .. %7.1f %%" % (slot, len(vals), min(vals), max(vals)))
