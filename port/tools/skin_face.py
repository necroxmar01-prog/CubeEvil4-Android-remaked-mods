#!/usr/bin/env python3
"""Leon's head in a GameCube player archive (pl00/pl08 .drs, the output of uhd_skin_convert.py / skin_graft.py):
hair colour and stubble, after reference pictures (wish of DevJunior, 09.11.2026: Leon of the Infinite Darkness look:
ash brown hair with lighter strands, stubble on the jaw, the chin and the upper lip).

  skin_face.py IN.drs OUT.drs [--hair R,G,B:R,G,B:R,G,B] [--stubble STRENGTH] [--face-size 512]

--hair: a colour ramp (shadow : middle : highlight) over the hair texture's brightness (its own strands' light and
dark give the highlights), for the hair model (entry 5) and the hair painted on the face's texture (the hairline,
the temples, the nape: dark texels above the brows or behind the cheeks). --stubble: short beard over the jaw, the
chin, the cheeks up to a line from the nostrils to the sideburns, the upper lip and under the chin, the lips left
free; the face's texture first enlarged to --face-size (finer stubble). Where a texel lies on the head is taken from
the model (each texel's position on its triangle); Leon's landmarks (bind pose, model space): nose tip 0/1689/112,
lips 1638..1662, chin 1600..1630, brows about 1740, the head's model ends at 1596 (front) .. 1633 (nape).

Disc and mod files never go into the repository; this script only reads and writes the paths given."""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402
import skin_graft as G  # noqa: E402

HEAD, HEAD_TPL, HAIR = 2, 3, 5


def uv_positions(blob, mat, w, h):
    """Per texel (h x w) of texture mat the model position on its triangle (nan where none)."""
    P, _, UV = U.gc_triangles(blob, mat)
    pos = np.full((h, w, 3), np.nan)
    for k in range(len(P)):
        sx, sy = UV[k][:, 0] * w, UV[k][:, 1] * h
        x0, x1 = int(max(0, np.floor(sx.min()))), int(min(w - 1, np.ceil(sx.max())))
        y0, y1 = int(max(0, np.floor(sy.min()))), int(min(h - 1, np.ceil(sy.max())))
        if x0 > x1 or y0 > y1:
            continue
        ax, ay, bx, by, qx, qy = sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]
        den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
        if abs(den) < 1e-12:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((by - qy) * (gx - qx) + (qx - bx) * (gy - qy)) / den
        w1 = ((qy - ay) * (gx - qx) + (ax - qx) * (gy - qy)) / den
        w2 = 1 - w0 - w1
        m = (w0 >= -0.02) & (w1 >= -0.02) & (w2 >= -0.02)
        for c in range(3):
            v = w0 * P[k, 0, c] + w1 * P[k, 1, c] + w2 * P[k, 2, c]
            pos[y0:y1 + 1, x0:x1 + 1, c][m] = v[m]
    return pos


def ramp(lum, stops):
    """lum 0..1 to the colour ramp shadow (0) : middle (0.5) : highlight (1)."""
    s = np.array(stops, dtype=np.float32)
    t = np.clip(lum, 0, 1)[..., None]
    lo = s[0] + (s[1] - s[0]) * np.clip(t * 2, 0, 1)
    return np.where(t < 0.5, lo, s[1] + (s[2] - s[1]) * np.clip(t * 2 - 1, 0, 1))


def hair_colour(rgba, stops, mask=None):
    """The hair's texels (brightness > 2, or mask) coloured by the ramp over their brightness (2nd..98th percentile,
    a gamma that widens the highlights)."""
    a = rgba.astype(np.float32)
    lum = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    used = lum > 2 if mask is None else mask
    if not used.any():
        return rgba
    lo, hi = np.percentile(lum[used], 2), np.percentile(lum[used], 98)
    t = 0.15 + 0.75 * np.clip((lum - lo) / max(hi - lo, 1e-3), 0, 1)  # not the full ramp: no hard blocks
    out = a.copy()
    out[..., :3] = np.where(used[..., None], ramp(t, stops), a[..., :3])
    return np.clip(out, 0, 255).astype(np.uint8)


def blur1d(m, sigma, axis):
    r = max(1, int(sigma * 3 + 0.5))
    g = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    g /= g.sum()
    pad = [(0, 0)] * m.ndim
    pad[axis] = (r, r)
    return np.apply_along_axis(lambda v: np.convolve(v, g, "valid"), axis, np.pad(m, pad, mode="edge"))


def strands(rgba, size, seed=7):
    """The hair texture enlarged to size and its brightness redrawn as strands: blurred along them (the atlas runs
    them up and down) so the old texture's 4x4 blocks no longer show once it is lighter, then fine lighter and darker
    strands (noise per column, long along them) for the highlights."""
    img = np.asarray(Image.fromarray(rgba, "RGBA").resize((size, size), Image.LANCZOS)).astype(np.float32)
    lum = img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11
    used = lum > 2
    soft = blur1d(blur1d(lum, size / 64.0, 0), 0.6, 1)
    rng = np.random.default_rng(seed)
    streak = blur1d(blur1d(rng.standard_normal(lum.shape), size / 24.0, 0), 0.5, 1)
    streak /= streak.std() + 1e-6
    new = np.clip(soft * (1 + 0.28 * streak), 0, 255)
    ratio = np.where(lum > 1e-3, new / np.maximum(lum, 1e-3), 1.0)
    out = img.copy()
    for c in range(3):
        out[..., c] = np.where(used, np.clip(img[..., c] * ratio, 0, 255), img[..., c])
    return out.astype(np.uint8)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def stubble_weight(pos):
    """0..1 per texel: where the beard grows (from the model position)."""
    x, y, z = pos[..., 0], pos[..., 1], pos[..., 2]
    ax = np.abs(x)
    top = 1667 + 0.5 * ax  # under the nostrils, rising over the cheeks to the sideburns
    w = 1 - smoothstep(top - 6, top + 4, y)
    w *= smoothstep(-10, 15, z + 0.35 * (1700 - y) * (ax > 40))  # not behind the jaw's angle (the neck's back)
    w *= smoothstep(1594, 1608, y) * 0.55 + 0.45 * (y > 1605)  # thinner under the chin, down to the neck
    lips = (1 - smoothstep(20, 27, ax)) * smoothstep(1636, 1641, y) * (1 - smoothstep(1657, 1662, y)) * \
        smoothstep(78, 84, z)
    w *= 1 - lips
    w *= 1 - 0.35 * smoothstep(1650, 1700, y) * smoothstep(30, 60, ax)  # sparser up on the cheeks
    return np.nan_to_num(w)


def add_stubble(rgba, pos, strength, seed=4):
    a = rgba.astype(np.float32)
    w = stubble_weight(pos) * strength
    rng = np.random.default_rng(seed)
    noise = rng.random(w.shape)
    # dots a texel wide, a little blurred into the skin
    dots = np.asarray(Image.fromarray((noise * 255).astype(np.uint8)).filter(
        __import__("PIL.ImageFilter", fromlist=["GaussianBlur"]).GaussianBlur(0.6)), dtype=np.float32) / 255.0
    dots = np.clip((dots - 0.35) * 2.2, 0, 1)
    hair = np.array([52, 43, 35], dtype=np.float32)  # the stubble's colour (ash brown, a little grey)
    k = (w * (0.25 + 0.75 * dots))[..., None]
    shade = 1 - 0.22 * w[..., None]  # the skin under it a little darker
    out = a.copy()
    out[..., :3] = (a[..., :3] * shade) * (1 - k * 0.8) + hair * (k * 0.8)
    return np.clip(out, 0, 255).astype(np.uint8)


def bump(q, c, r):
    """Smooth weight 1 at c falling to 0 at distance r."""
    d = np.linalg.norm(q - np.asarray(c, dtype=np.float64), axis=-1) / r
    return np.clip(1 - d * d, 0, 1) ** 2


def sculpt_face(P, amount=1.0):
    """Leon's face towards the references' older, harder look (bind pose, model space; all in model units, about
    millimetres): a longer lower face and a squarer chin, a wider jaw, higher cheekbones over hollower cheeks, a heavier
    brow, thinner lips. The neck's lowest ring (below 1608) stays where it is."""
    q = P.astype(np.float64).copy()
    x, y, z = q[:, 0], q[:, 1], q[:, 2]
    sx = np.where(x >= 0, 1.0, -1.0)
    d = np.zeros_like(q)
    keep = smoothstep(1600, 1612, y)
    # the lower face a little longer (nothing above the cheekbones)
    d[:, 1] -= 4.0 * smoothstep(1700, 1630, y) * keep * smoothstep(0, 40, z)
    # a squarer, firmer chin
    w = bump(q, (0, 1612, 80), 26) * keep
    d[:, 1] -= 2.5 * w
    d[:, 2] += 3.0 * w
    for s in (-1.0, 1.0):
        # the jaw's angles wider
        w = bump(q, (s * 50, 1632, 35), 28) * keep
        d[:, 0] += s * 4.0 * w
        # cheekbones higher and further out, the cheeks under them hollower
        w = bump(q, (s * 54, 1702, 70), 18)
        d[:, 0] += s * 2.5 * w
        d[:, 2] += 1.5 * w
        w = bump(q, (s * 46, 1666, 74), 15)
        d[:, 0] -= s * 1.8 * w
        d[:, 2] -= 2.2 * w
        # a heavier brow (the eyes look deeper)
        w = bump(q, (s * 26, 1744, 92), 15)
        d[:, 2] += 2.5 * w
        d[:, 1] -= 1.0 * w
    # thinner lips
    lips = (1 - smoothstep(22, 30, np.abs(x))) * smoothstep(1634, 1640, y) * (1 - smoothstep(1660, 1666, y)) * \
        smoothstep(76, 84, z)
    d[:, 1] += (1650 - y) * 0.15 * lips
    return P + d * amount


def sculpt_hair(P, volume=5.0, length=18.0):
    """Leon's hair fuller and longer (the references' shaggy cut over the ears and the nape): away from the head's
    middle (0, 1735, -5) by `volume` on the scalp, the hanging ends lower by up to `length` (from 1710 down), a little
    outwards with them so they stay clear of the face and the neck."""
    q = P.astype(np.float64).copy()
    x, y, z = q[:, 0], q[:, 1], q[:, 2]
    c = np.array([0.0, 1735.0, -5.0])
    v = q - c
    n = v / (np.linalg.norm(v, axis=1, keepdims=True) + 1e-6)
    d = n * (volume * smoothstep(1680, 1760, y))[:, None]
    hang = smoothstep(1710, 1630, y)
    d[:, 1] -= length * hang
    out = np.stack([x, np.zeros_like(x), z + 5.0], axis=1)
    out /= np.linalg.norm(out, axis=1, keepdims=True) + 1e-6
    d += out * (0.25 * length * hang)[:, None]
    return P + d


def set_positions(blob, P):
    blob = bytearray(blob)
    for i, q in enumerate(P):
        G.set_position(blob, i, q)
    return bytes(blob)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--hair", help="shadow:middle:highlight colours, e.g. 40,32,24:96,80,60:170,150,118")
    ap.add_argument("--stubble", type=float, default=0.0, help="0 none .. 1 full")
    ap.add_argument("--sculpt-face", type=float, default=0.0, metavar="AMOUNT",
                    help="the face's shape towards the references (1: as measured; 0 none)")
    ap.add_argument("--hair-volume", type=float, default=0.0, help="the hair fuller on the scalp (model units)")
    ap.add_argument("--hair-length", type=float, default=0.0, help="the hanging hair longer (model units)")
    ap.add_argument("--hair-size", type=int, default=512, help="the hair texture's size (power of two, <= 1024)")
    ap.add_argument("--face-size", type=int, default=512, help="the face texture's size (power of two, <= 1024)")
    a = ap.parse_args(argv)
    arc = U.Archive(Path(a.inp).read_bytes(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    head = entries[HEAD]
    imgs = G.tpl_images(entries[HEAD_TPL])
    decode = lambda k: np.asarray(U.decode_gx(*struct.unpack_from(">HHI", imgs[k][0]), imgs[k][1]).convert("RGBA"))
    # the head's colour textures: the face (the one with most texels, not the eyes'), the hair's
    mats = sorted({head[p + 0xC] for p in G.parts(head) if not G.is_empty(head, p)})
    face = max(mats, key=lambda m: len(U.gc_triangles(head, m)[0]))
    hair_mats = sorted({entries[HAIR][p + 0xC] for p in G.parts(entries[HAIR]) if not G.is_empty(entries[HAIR], p)})
    stops = [[float(v) for v in c.split(",")] for c in a.hair.split(":")] if a.hair else None
    if stops:
        for m in hair_mats:
            src = strands(decode(m), a.hair_size)
            imgs[m] = G.encode_image(Image.fromarray(hair_colour(src, stops), "RGBA"), imgs[m][0])
            print("hair texture %d recoloured (%dx%d, strands)" % (m, src.shape[1], src.shape[0]))
    if a.sculpt_face:
        P = G.positions(head)
        P2 = sculpt_face(P, a.sculpt_face)
        head = entries[HEAD] = set_positions(head, P2)
        print("face sculpted: %d positions, at most %.1f" % (int((np.abs(P2 - P).max(1) > 0.05).sum()),
                                                            float(np.abs(P2 - P).max())))
    if a.hair_volume or a.hair_length:
        P = G.positions(entries[HAIR])
        P2 = sculpt_hair(P, a.hair_volume, a.hair_length)
        entries[HAIR] = set_positions(entries[HAIR], P2)
        print("hair shaped: %d positions, at most %.1f" % (int((np.abs(P2 - P).max(1) > 0.05).sum()),
                                                          float(np.abs(P2 - P).max())))
    img = decode(face)
    if a.face_size and a.face_size != img.shape[1]:
        img = np.asarray(Image.fromarray(img, "RGBA").resize((a.face_size, a.face_size), Image.LANCZOS))
    pos = uv_positions(head, face, img.shape[1], img.shape[0])
    if stops:
        lum = img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11
        y, z = pos[..., 1], pos[..., 2]
        painted = np.nan_to_num((y > 1752) | ((y > 1690) & (z < 40)), nan=False).astype(bool) & (lum < 60)
        img = hair_colour(img, stops, painted)
        print("face texture %d: %d hair texels recoloured" % (face, int(painted.sum())))
    if a.stubble > 0:
        img = add_stubble(img, pos, a.stubble)
        print("face texture %d: stubble %.2f" % (face, a.stubble))
    imgs[face] = G.encode_image(Image.fromarray(img, "RGBA"), imgs[face][0])
    entries[HEAD_TPL] = G.build_tpl(imgs)
    data = U.build_archive(arc, U.build_body(entries, arc.types))
    Path(a.out).write_bytes(data)
    print("%s: %d bytes" % (a.out, len(data)))


if __name__ == "__main__":
    main()
