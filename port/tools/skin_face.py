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


def uv_positions(blobs, mat, w, h):
    """Per texel (h x w) of texture mat the model position on its triangle (nan where none), over the given models
    (Leon's head and his face with its shapes, entry 4, when that has one)."""
    tris = [U.gc_triangles(b, mat) for b in blobs]
    P = np.concatenate([t[0] for t in tris if len(t[0])]) if any(len(t[0]) for t in tris) else np.zeros((0, 3, 3))
    UV = np.concatenate([t[2] for t in tris if len(t[0])]) if len(P) else np.zeros((0, 3, 2))
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


def strands(rgba, size, seed=7, equalize=False):
    """The hair texture enlarged to size and its brightness redrawn as strands: blurred along them (the atlas runs
    them up and down) so the old texture's 4x4 blocks no longer show once it is lighter, then fine lighter and darker
    strands (noise per column, long along them) for the highlights."""
    img = np.asarray(Image.fromarray(rgba, "RGBA").resize((size, size), Image.LANCZOS)).astype(np.float32)
    lum = img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11
    used = lum > 2
    if equalize and used.any():
        # each patch to the atlas's mean brightness (its own large-scale brightness divided out, only over hair)
        um = used.astype(np.float32)
        r = size / 24.0
        big = blur1d(blur1d(lum * um, r, 0), r, 1) / np.maximum(blur1d(blur1d(um, r, 0), r, 1), 1e-3)
        lum_eq = np.where(used, lum / np.maximum(big, 1.0) * float(lum[used].mean()), lum)
        ratio = np.where(lum > 1e-3, lum_eq / np.maximum(lum, 1e-3), 1.0)
        for c in range(3):
            img[..., c] = np.where(used, np.clip(img[..., c] * ratio, 0, 255), img[..., c])
        lum = lum_eq
    if equalize and used.any():
        # the patches' own tone kept only weakly: an even base with the strands drawn on it
        lum = np.where(used, 0.25 * lum + 0.75 * float(lum[used].mean()), lum)
    soft = blur1d(blur1d(lum, size / 64.0, 0), 0.6, 1)
    rng = np.random.default_rng(seed)
    streak = blur1d(blur1d(rng.standard_normal(lum.shape), size / 24.0, 0), 0.5, 1)
    streak /= streak.std() + 1e-6
    new = np.clip(soft * (1 + (0.35 if equalize else 0.28) * streak), 0, 255)
    ratio = np.where(lum > 1e-3, new / np.maximum(lum, 1e-3), 1.0)
    out = img.copy()
    for c in range(3):
        out[..., c] = np.where(used, np.clip(img[..., c] * ratio, 0, 255), img[..., c])
    return out.astype(np.uint8)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# Where the face's features lie (bind pose, model space), per head: re4 the original game's Leon, remake the head of
# the Resident Evil 4 Remake mod (DevJunior, 09.11.2026: its eyes 1722, its mouth 1652, its nose's base 1678, its chin
# 1615, its lips 6..10 further forward)
PROFILES = {
    "re4": dict(top=1667, slope=0.5, chin=(1594, 1608), lips_x=(20, 27), lips_y=(1636, 1641, 1657, 1662),
                lips_z=(78, 84), cheek=(1650, 1700), edge=0.0, side=0.0, fade=6, neck=None),
    "remake": dict(top=1676, slope=0.18, side=1.3, chin=(1604, 1617), lips_x=(21, 27), lips_y=(1639, 1643, 1659, 1663),
                   lips_z=(83, 88), cheek=(1662, 1690), edge=2.0, soft=True, fade=14,
                   neck=(1590, 1603)),
}


def stubble_weight(pos, prof=None, cheeks=1.0, under_lip=0.0, lip_margin=0.0):
    """0..1 per texel: where the beard grows (from the model position); cheeks: how much of it on the cheeks above
    the jaw line (1 all; less: a light beard on the chin, the jaw and the upper lip)."""
    q = PROFILES["re4"] if prof is None else prof
    x, y, z = pos[..., 0], pos[..., 1], pos[..., 2]
    ax = np.abs(x)
    top = q["top"] + q["slope"] * ax  # under the nostrils, rising over the cheeks to the sideburns
    top = top + q["side"] * np.maximum(ax - 55, 0)  # (remake: low over the cheeks, up to the sideburns at the ears)
    if q["edge"]:  # a ragged edge, not a drawn line
        rng = np.random.default_rng(9)
        n = blur1d(blur1d(rng.standard_normal(x.shape), 2.0, 0), 2.0, 1)
        top = top + q["edge"] * n / (n.std() + 1e-6)
    w = 1 - smoothstep(top - q["fade"], top + 4, y)
    w *= smoothstep(-10, 15, z + 0.35 * (1700 - y) * (ax > 40))  # not behind the jaw's angle (the neck's back)
    c0, c1 = q["chin"]
    w *= smoothstep(c0, c1, y) * 0.55 + 0.45 * (y > c1 - 3)  # thinner under the chin, down to the neck
    if q["neck"]:  # (remake: none where the head's model meets the body's neck, no edge there)
        w *= smoothstep(q["neck"][0], q["neck"][1], y)
    lx, (l0, l1, l2, l3), (z0, z1) = q["lips_x"], q["lips_y"], q["lips_z"]
    m = lip_margin  # a clean edge around the lips and the mouth's corners (no grime at the mouth)
    lips = (1 - smoothstep(lx[0] + m, lx[1] + m, ax)) * smoothstep(l0 - m, l1 - m, y) * \
        (1 - smoothstep(l2 + m, l3 + m, y)) * smoothstep(z0 - 2 * m, z1 - 2 * m, z)
    w *= 1 - lips
    w *= 1 - 0.35 * smoothstep(q["cheek"][0], q["cheek"][1], y) * smoothstep(30, 60, ax)  # sparser up on the cheeks
    if under_lip > 0:  # the hollow under the lower lip clean, down to the chin's tip (a tired, grown-out look)
        l0 = q["lips_y"][0]
        gap = (1 - smoothstep(13, 22, ax)) * smoothstep(l0 - 12, l0 - 7, y) * (1 - smoothstep(l0 - 1, l0 + 3, y))
        w *= 1 - under_lip * gap
    if cheeks < 1:  # the jaw line: from the chin (its lowest point) up to the jaw's angle below the ear
        jaw = q["chin"][1] + 1 + 0.7 * np.maximum(ax - 15, 0)
        w *= 1 - (1 - cheeks) * smoothstep(jaw + 4, jaw + 14, y) * smoothstep(24, 34, ax)
    return np.nan_to_num(w)


def smooth_noise(pos, scale=1.0):
    """A smooth noise over the model (sines of the position: no seams between the texture's islands), about -1..1."""
    x, y, z = (np.nan_to_num(pos[..., c]) * scale for c in range(3))
    n = (np.sin(x * 0.31 + 1.3) * np.sin(y * 0.27 + 0.7) + np.sin(x * 0.17 - y * 0.21 + 2.0) * np.sin(z * 0.23 + 0.4) +
         np.sin(x * 0.53 + z * 0.19 + 0.9) * np.sin(y * 0.47 - 1.1) * 0.6)
    return n / 1.6


def add_stubble(rgba, pos, strength, seed=4, colour=(52, 43, 35), prof=None, grey=0.0, cheeks=1.0, patchy=0.0,
                under_lip=0.0, lip_margin=0.0):
    """Short beard; grey: the share of grey hairs among the dark (0 none .. 1 all; salt and pepper); patchy: thinner
    in patches (0 even .. 1 patches almost bare)."""
    a = rgba.astype(np.float32)
    w = stubble_weight(pos, prof, cheeks, under_lip, lip_margin) * strength
    if patchy > 0:
        w = w * (1 - patchy * smoothstep(-0.3, 0.6, smooth_noise(pos)))
    rng = np.random.default_rng(seed)
    noise = rng.random(w.shape)
    # dots a texel wide, a little blurred into the skin
    dots = np.asarray(Image.fromarray((noise * 255).astype(np.uint8)).filter(
        __import__("PIL.ImageFilter", fromlist=["GaussianBlur"]).GaussianBlur(0.6)), dtype=np.float32) / 255.0
    dots = np.clip((dots - 0.35) * 2.2, 0, 1)
    hair = np.broadcast_to(np.array(colour, dtype=np.float32), a[..., :3].shape).copy()  # ash brown; greyer with age
    if grey > 0:
        salt = (rng.random(w.shape) < grey)[..., None]
        hair = np.where(salt, np.array((112, 106, 100), dtype=np.float32), hair)
    soft = prof is not None and prof.get("soft")  # (remake: an even shadow of beard, the single hairs fainter)
    k = (w * ((0.5 + 0.5 * dots) if soft else (0.25 + 0.75 * dots)))[..., None]
    shade = 1 - (0.1 if soft else 0.22) * w[..., None]  # the skin under it a little darker (remake: less, no grime)
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


def head_shell(head_P, centre=(0.0, 1730.0, 5.0), bins=36):
    """The head's outer radius around centre per direction (bins x bins over the angles; the farthest vertex)."""
    c = np.asarray(centre)
    v = head_P - c
    r = np.linalg.norm(v, axis=1)
    near = r < 200  # the head only (an empty face entry's stand-in points lie far below)
    v, r = v[near], r[near]
    th = np.arctan2(v[:, 0], v[:, 2])
    ph = np.arcsin(np.clip(v[:, 1] / np.maximum(r, 1e-6), -1, 1))
    i = ((th + np.pi) / (2 * np.pi) * bins).astype(int) % bins
    j = np.clip(((ph + np.pi / 2) / np.pi * bins).astype(int), 0, bins - 1)
    shell = np.zeros((bins, bins))
    np.maximum.at(shell, (i, j), r)
    # the empty bins from their neighbours
    for _ in range(4):
        z = shell == 0
        if not z.any():
            break
        nb = np.maximum.reduce([np.roll(shell, 1, 0), np.roll(shell, -1, 0), np.roll(shell, 1, 1),
                                np.roll(shell, -1, 1)])
        shell[z] = nb[z]
    return c, shell, bins


def outside_head(P, shell, gap=2.0):
    """Positions that a restyle pushed into the head back out along their direction from its centre."""
    c, sh, bins = shell
    v = P - c
    r = np.linalg.norm(v, axis=1)
    th = np.arctan2(v[:, 0], v[:, 2])
    ph = np.arcsin(np.clip(v[:, 1] / np.maximum(r, 1e-6), -1, 1))
    i = ((th + np.pi) / (2 * np.pi) * bins).astype(int) % bins
    j = np.clip(((ph + np.pi / 2) / np.pi * bins).astype(int), 0, bins - 1)
    need = sh[i, j] + gap
    k = np.clip(np.where(r < need, need / np.maximum(r, 1e-6), 1.0), 1.0, 1.25)
    return c + v * k[:, None]


def restyle_re9(P, head_P):
    """Leon's hair towards Resident Evil Requiem's cut (references of DevJunior, 09.11.2026): parted on his left
    (+x), the front on the left swept up and back (the left of the forehead free), the fringe right of the part
    lifted at the roots into a swoop and falling diagonally to his right over the right eye down to the cheekbone
    (not to the mouth), the right side to the jaw, the crown fuller. Bind pose, model space; then nothing inside the
    head (head_shell)."""
    q = P.astype(np.float64).copy()
    x, y, z = q[:, 0], q[:, 1], q[:, 2]
    d = np.zeros_like(q)
    part = 22.0
    front = smoothstep(25, 55, z)
    drop = np.clip(1795 - y, 0, None)  # how far below the hairline's top
    # left of the part: up and back, a little out
    wl = smoothstep(part, part + 18, x) * front
    lift = np.minimum(0.35 * drop, 22.0)  # a sweep back, no higher than the roots
    d[:, 1] += lift * wl
    d[:, 2] -= np.minimum(0.35 * drop, 35.0) * wl
    d[:, 0] += np.minimum(0.12 * drop, 12.0) * wl
    # right of the part: the swoop (roots up and forward), the fringe diagonally to the right
    wr = (1 - smoothstep(part - 6, part + 6, x)) * front
    roots = smoothstep(1760, 1805, y)
    d[:, 1] += 9.0 * roots * wr
    d[:, 2] += 6.0 * roots * wr
    d[:, 0] -= 0.22 * drop * wr * smoothstep(-70, 10, x)
    # the fringe's tips up to the cheekbone (squeezed below 1720)
    tips = (1 - smoothstep(1650, 1722, y)) * wr * smoothstep(-90, -20, x)
    d[:, 1] += (1722 - y) * 0.45 * tips
    # the right side down to the jaw, the crown fuller
    ws = (1 - smoothstep(-80, -55, x)) * (1 - smoothstep(20, 50, z))
    d[:, 1] -= 12.0 * smoothstep(1720, 1640, y) * ws
    c = np.array([0.0, 1735.0, -5.0])
    v = q - c
    n = v / (np.linalg.norm(v, axis=1, keepdims=True) + 1e-6)
    d += n * (6.0 * smoothstep(1740, 1800, y))[:, None]
    out = outside_head(q + d, head_shell(head_P))
    out[:, 1] = np.minimum(out[:, 1], q[:, 1].max() + 3.0)  # no tuft standing above the crown
    return out


def set_positions(blob, P):
    blob = bytearray(blob)
    for i, q in enumerate(P):
        G.set_position(blob, i, q)
    return bytes(blob)


def pos_normal_pairs(blob):
    """Each (position index, normal index) the model draws with."""
    u = lambda o: struct.unpack_from(">I", blob, o)[0]
    per = 4 if u(0x20) & 0x80000000 else 3
    pairs = set()
    p = u(0x1C)
    for _ in range(struct.unpack_from(">H", blob, 0x1A)[0]):
        size = u(p + 0x18)
        s, e = p + 0x20, p + 0x20 + size
        while s < e and blob[s] != 0:
            cnt = struct.unpack_from(">H", blob, s + 1)[0]
            for k in range(cnt):
                v = struct.unpack_from(">2H", blob, s + 3 + k * per * 2)
                pairs.add(v)
            s += 3 + cnt * per * 2
        p = e
    return pairs


def fold_band(x, y, profile, inner=3.0, outer=7.0):
    """0..1: how close (front view) to a fold from the nose to the mouth."""
    band = np.zeros(np.shape(x))
    for line in AGE_LINES[profile]:
        if len(line) > 3 and line[3] == "nose":
            pts = [(px, py + 2) for px, py in line[0]] + [(line[0][-1][0], line[0][-1][1] - 6)]
            for sgn in (1, -1):
                d = seg_dist(x, y, [(sgn * px, py) for px, py in pts])
                band = np.maximum(band, 1 - smoothstep(inner, outer, d))
    return band


def fill_folds_model(blob, amount=1.0, profile="re4", radius=7.0, do_pos=True, do_nrm=True):
    """The folds from the nose to the mouth out of the head's shape and its lighting: in a band along each fold the
    depth (z) of the positions and the normals drawn with them move towards their neighbourhood's (a Gaussian over
    the front view, radius in model units); the nostrils and the lips (z far off the neighbourhood) stay."""
    u = lambda o: struct.unpack_from(">I", blob, o)[0]
    s8 = bool(u(0x20) & 0x20000000)
    P = G.positions(blob)
    front = P[:, 2] > 60
    band = fold_band(P[:, 0], P[:, 1], profile) * front * amount
    sel = np.where(band > 0.01)[0]
    F = np.where(front)[0]
    P2 = P.copy()
    for i in (sel if do_pos else ()):
        d2 = (P[F, 0] - P[i, 0]) ** 2 + (P[F, 1] - P[i, 1]) ** 2
        w = np.exp(-d2 / (2 * (radius / 2) ** 2)) * (d2 < radius * radius)
        z = (w * P[F, 2]).sum() / max(w.sum(), 1e-9)
        if abs(z - P[i, 2]) < 4:  # (not the nostrils' rims nor the lips)
            P2[i, 2] = P[i, 2] + (z - P[i, 2]) * band[i]
    out = bytearray(set_positions(blob, P2))
    nn = struct.unpack_from(">H", blob, 0x3A)[0]
    no, sc = u(0x34), (64.0 if s8 else 16384.0)
    N = (np.frombuffer(blob, ">i1" if s8 else ">i2", nn * 4, no).reshape(-1, 4)[:, :3] / sc).astype(float)
    by_n = {}
    for vp, vn in pos_normal_pairs(blob):
        if vp < len(P) and vn < nn:
            by_n.setdefault(vn, []).append(vp)
    # the neighbourhood's normal at each front position
    # (a normal the converter shares between distant positions of equal value stays: only those drawn at one spot)
    pos_of = {vn: np.mean(P[vps], axis=0) for vn, vps in by_n.items() if np.ptp(P[vps], axis=0).max() < 3}
    keys = np.array(sorted(pos_of))
    Q = np.array([pos_of[k] for k in keys])
    moved = 0
    for vn, q in (zip(keys, Q) if do_nrm else ()):
        b = float(fold_band(q[0], q[1], profile)) * (q[2] > 60) * amount
        if b <= 0.01 or N[vn][2] < 0.4 * np.linalg.norm(N[vn]):  # (only skin facing forwards: not under the
            continue                                                 # nostrils nor the lips' undersides)
        d2 = (Q[:, 0] - q[0]) ** 2 + (Q[:, 1] - q[1]) ** 2
        fwd = N[keys][:, 2] > 0.4 * np.linalg.norm(N[keys], axis=1)
        m = (d2 < radius * radius) & (np.abs(Q[:, 2] - q[2]) < 4) & fwd
        w = np.exp(-d2[m] / (2 * (radius / 2) ** 2))
        avg = (w[:, None] * N[keys[m]]).sum(0)
        n = N[vn] + (avg / (np.linalg.norm(avg) + 1e-9) - N[vn] / (np.linalg.norm(N[vn]) + 1e-9)) * b
        n = n / (np.linalg.norm(n) + 1e-9)
        if s8:
            struct.pack_into(">3b", out, no + vn * 4, *[max(-64, min(63, int(round(c * 64)))) for c in n])
        else:
            struct.pack_into(">3h", out, no + vn * 8, *[max(-16384, min(16383, int(round(c * 16384)))) for c in n])
        moved += 1
    return bytes(out), len(sel), moved


def seg_dist(px, py, pts):
    """Distance in the front view (x, y) from each texel to a polyline."""
    best = np.full(px.shape, np.inf)
    for (x0, y0), (x1, y1) in zip(pts[:-1], pts[1:]):
        dx, dy = x1 - x0, y1 - y0
        t = np.clip(((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy + 1e-9), 0, 1)
        best = np.minimum(best, np.hypot(px - (x0 + t * dx), py - (y0 + t * dy)))
    return best


# Leon's creases of age (the references: Resident Evil Requiem, Infinite Darkness), in the front view (x, y) of the
# bind pose; each line mirrored to both sides: (points, depth[, width])
AGE_LINES = {
    "re4": [
        ([(-36, 1761), (-12, 1763), (12, 1763), (36, 1761)], 0.16),  # the forehead
        ([(-30, 1771), (0, 1773), (30, 1771)], 0.13),
        ([(-24, 1781), (0, 1782), (24, 1781)], 0.10),
        ([(5, 1736), (6, 1749)], 0.10),  # between the brows (faint: deeper reads as a scar)
        ([(44, 1725), (57, 1733)], 0.18),  # crow's feet
        ([(45, 1722), (60, 1722)], 0.18),
        ([(44, 1718), (57, 1710)], 0.16),
        ([(16, 1709), (28, 1703), (41, 1709)], 0.22),  # under the eyes
        ([(15, 1681), (24, 1668), (29, 1656), (31, 1645)], 0.26, 1.3, "nose"),  # nose to mouth
        ([(28, 1640), (30, 1628)], 0.12),  # down from the mouth's corners
    ],
    # the Remake's head: eyes x 12..44 / y 1712..1732 (pupil 28/1722), brows 1740, nose's wings 16/1682,
    # mouth's corners 24/1652; softer and wider lines (its texture is finer)
    "remake": [
        ([(-36, 1756), (-12, 1758), (12, 1758), (36, 1756)], 0.13, 1.7),  # the forehead
        ([(-31, 1765), (0, 1767), (31, 1765)], 0.11, 1.6),
        ([(-25, 1774), (0, 1775), (25, 1774)], 0.08, 1.5),
        ([(5, 1735), (6, 1746)], 0.09, 1.0),  # between the brows
        ([(46, 1725), (55, 1730)], 0.13, 0.9),  # crow's feet
        ([(47, 1721), (57, 1721)], 0.13, 0.9),
        ([(46, 1717), (54, 1712)], 0.12, 0.9),
        ([(14, 1711), (21, 1706), (29, 1704), (37, 1706), (44, 1711)], 0.17, 1.4),  # the bags' lower edge
        ([(18, 1700), (29, 1696), (40, 1700)], 0.07, 2.0),  # below them, faint
        ([(17, 1683), (22, 1673), (27, 1662), (29, 1652), (30, 1642)], 0.22, 2.0, "nose"),  # nose to mouth
        ([(27, 1644), (29, 1632)], 0.09, 1.4),  # down from the mouth's corners
    ],
}
# the shadows of the bags under the eyes and the hollows of the cheeks: (centre, radius, depth)
AGE_SHADOWS = {
    "re4": [((28, 1703), 9, 0.10), ((44, 1668), 14, 0.07)],
    "remake": [((29, 1708), 8, 0.12), ((42, 1672), 13, 0.07)],
}


# the eyes' sockets (deep-set eyes, Infinite Darkness): under the brows, the inner corners, the outer corners
SOCKETS = {
    "re4": [((29, 1726), 11, 0.14), ((14, 1716), 6, 0.12), ((44, 1718), 7, 0.08)],
    "remake": [((29, 1732), 11, 0.16), ((13, 1722), 6, 0.14), ((44, 1724), 7, 0.09)],
}


def add_skin(rgba, pos, pale=0.0, sockets=0.0, pores=0.0, profile="re4"):
    """The skin of the references (Infinite Darkness): paler and cooler (less rosy, the lips a little), deeper eye
    sockets, a fine grain of pores over the face."""
    a = rgba.astype(np.float32)
    x, y = np.nan_to_num(pos[..., 0]), np.nan_to_num(pos[..., 1])
    ok = np.isfinite(pos[..., 0])
    out = a.copy()
    if pale > 0:
        g = out[..., :3].mean(-1, keepdims=True)
        out[..., :3] = out[..., :3] + (g - out[..., :3]) * (0.35 * pale * ok)[..., None]
        out[..., :3] *= 1 + pale * ok[..., None] * np.array((-0.03, 0.0, 0.035))
    if sockets > 0:
        dark = np.zeros(x.shape)
        q = np.stack([x, y, np.zeros_like(x)], -1)
        for (cx, cy), r, depth in SOCKETS[profile]:
            for sgn in (1, -1):
                dark = np.maximum(dark, depth * bump(q, (sgn * cx, cy, 0), r))
        out[..., :3] *= (1 - sockets * dark * ok)[..., None]
    if pores > 0:
        rng = np.random.default_rng(23)
        n = blur1d(blur1d(rng.standard_normal(x.shape), 0.7, 0), 0.7, 1)
        n /= n.std() + 1e-6
        out[..., :3] *= (1 + 0.05 * pores * n * ok)[..., None]
    return np.clip(out, 0, 255).astype(np.uint8)


def fill_folds(rgba, pos, amount=1.0, profile="re4"):
    """The folds from the nose to the mouth painted into the texture itself (the Remake's face has them) filled: in a
    band along each fold the darker texels are lifted to the skin's local brightness (a wide blur), their colour and
    grain kept."""
    a = rgba.astype(np.float32)
    x, y = np.nan_to_num(pos[..., 0]), np.nan_to_num(pos[..., 1])
    ok = np.isfinite(pos[..., 0]) & (np.nan_to_num(pos[..., 2]) > 60)
    band = fold_band(x, y, profile)
    band *= ok
    lum = a[..., :3].mean(-1)
    k = 10.0 * a.shape[1] / 512  # texels
    num = blur1d(blur1d(lum * ok, k, 0), k, 1)
    den = blur1d(blur1d(ok.astype(np.float64), k, 0), k, 1)
    local = num / np.maximum(den, 1e-3)
    lift = np.clip(local / np.maximum(lum, 1.0), 1.0, 1.6)  # only lighter, never darker
    f = 1 + (lift - 1) * band * amount
    out = a.copy()
    out[..., :3] = a[..., :3] * f[..., None]
    return np.clip(out, 0, 255).astype(np.uint8)


def add_age(rgba, pos, strength=1.0, profile="re4", mouth=1.0, nose=1.0, cheeks=1.0):
    """Creases of age, shadows under the eyes and a weathered skin on the face's texture (front texels only)."""
    a = rgba.astype(np.float32)
    x, y, z = pos[..., 0], pos[..., 1], pos[..., 2]
    ok = np.isfinite(x)
    x, y, z = np.nan_to_num(x), np.nan_to_num(y), np.nan_to_num(z)
    front = smoothstep(40, 62, z) * ok
    dark = np.zeros(x.shape)
    for line in AGE_LINES[profile]:
        pts, depth = line[0], line[1]
        width = line[2] if len(line) > 2 else 1.3
        if max(py for _, py in pts) < 1650:  # down from the mouth's corners
            depth *= mouth
        if len(line) > 3 and line[3] == "nose":  # the folds from the nose to the mouth
            depth *= nose
        if depth <= 0:
            continue
        for s in (1, -1):
            if s == -1 and all(px == 0 for px, _ in pts):
                continue
            d = seg_dist(x, y, [(s * px, py) for px, py in pts])
            dark = np.maximum(dark, depth * np.exp(-(d / width) ** 2))
    # the shadow under the eyes and in the hollows of the cheeks
    for (cx, cy), r, depth in AGE_SHADOWS[profile]:
        if cy < 1690:  # the hollows of the cheeks (DevJunior: they stay without the folds)
            depth *= cheeks
        for s in (1, -1):
            dark = np.maximum(dark, depth * bump(np.stack([x, y, np.zeros_like(x)], -1), (s * cx, cy, 0), r))
    rng = np.random.default_rng(11)
    blotch = blur1d(blur1d(rng.standard_normal(x.shape), 3.0, 0), 3.0, 1)
    blotch /= blotch.std() + 1e-6
    k = 1 - strength * (dark * front)
    k = k * (1 + 0.035 * strength * blotch * front)
    out = a.copy()
    out[..., :3] = a[..., :3] * k[..., None]
    # a little less rosy: towards its own grey by 12 %
    g = out[..., :3].mean(-1, keepdims=True)
    out[..., :3] = out[..., :3] + (g - out[..., :3]) * (0.12 * strength * front)[..., None]
    return np.clip(out, 0, 255).astype(np.uint8)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--hair", help="shadow:middle:highlight colours, e.g. 40,32,24:96,80,60:170,150,118")
    ap.add_argument("--stubble", type=float, default=0.0, help="0 none .. 1 full")
    ap.add_argument("--head-from", metavar="SKIN.drs",
                    help="Leon's whole head (head, palette, face with its shapes, hair: entries 2..5) from another "
                         "converted skin first (DevJunior, 09.11.2026: the hair of Resident Evil Requiem, from the "
                         "Leon Requiem mod)")
    ap.add_argument("--equalize", action="store_true",
                    help="the hair texture's patches of different brightness (tiles of the mod's atlas) evened out "
                         "before the strands are drawn")
    ap.add_argument("--sculpt-face", type=float, default=0.0, metavar="AMOUNT",
                    help="the face's shape towards the references (1: as measured; 0 none)")
    ap.add_argument("--restyle", choices=("re9",), help="the hair's cut: re9 Resident Evil Requiem's")
    ap.add_argument("--hair-volume", type=float, default=0.0, help="the hair fuller on the scalp (model units)")
    ap.add_argument("--hair-length", type=float, default=0.0, help="the hanging hair longer (model units)")
    ap.add_argument("--hair-size", type=int, default=512, help="the hair texture's size (power of two, <= 1024), its strands redrawn; 0: as it is (a texture with "
                         "good strands of its own)")
    ap.add_argument("--stubble-colour", default="52,43,35", help="R,G,B of the stubble (greyer: older)")
    ap.add_argument("--age", type=float, default=0.0, help="creases of age on the face (0 none .. 1 full)")
    ap.add_argument("--face-size", type=int, default=512, help="the face texture's size (power of two, <= 1024)")
    ap.add_argument("--face-model", choices=sorted(PROFILES), default="re4",
                    help="whose head the archive carries (where the stubble and the creases go): re4 the game's "
                         "Leon, remake the head of the Resident Evil 4 Remake mod")
    ap.add_argument("--stubble-cheeks", type=float, default=1.0,
                    help="how much stubble on the cheeks above the jaw line (1 all .. 0 none: a light beard on the "
                         "chin, the jaw and the upper lip)")
    ap.add_argument("--stubble-patchy", type=float, default=0.0,
                    help="the stubble thinner in patches (0 even .. 1 patches almost bare)")
    ap.add_argument("--stubble-under-lip", type=float, default=0.0,
                    help="the hollow under the lower lip clean (0 bearded .. 1 clean)")
    ap.add_argument("--stubble-lip-margin", type=float, default=0.0,
                    help="a clean edge around the lips and the mouth's corners (model units, e.g. 3)")
    ap.add_argument("--mouth-lines", type=float, default=1.0,
                    help="the creases at the mouth's corners (1 as the age gives them .. 0 none)")
    ap.add_argument("--nose-lines", type=float, default=1.0,
                    help="the folds from the nose to the mouth (1 as the age gives them .. 0 none)")
    ap.add_argument("--cheek-hollows", type=float, default=1.0,
                    help="the shadows in the hollows of the cheeks (1 as the age gives them .. 0 none)")
    ap.add_argument("--pale", type=float, default=0.0, help="the skin paler and cooler (0 as it is .. 1)")
    ap.add_argument("--sockets", type=float, default=0.0, help="deeper eye sockets (0 none .. 1)")
    ap.add_argument("--pores", type=float, default=0.0, help="a fine grain of pores over the face (0 none .. 1)")
    ap.add_argument("--stubble-grey", type=float, default=0.0,
                    help="the share of grey hairs in the stubble (0 none .. 1 all; salt and pepper)")
    a = ap.parse_args(argv)
    arc = U.Archive(Path(a.inp).read_bytes(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    if a.head_from:
        src = U.Archive(Path(a.head_from).read_bytes(), ">")
        for e in (HEAD, HEAD_TPL, 4, HAIR):  # the head, its palette, the face with its shapes, the hair
            entries[e] = src.entry(e)
        print("head, face and hair (entries 2..5) from %s" % a.head_from)
    head = entries[HEAD]
    imgs = G.tpl_images(entries[HEAD_TPL])
    decode = lambda k: np.asarray(U.decode_gx(*struct.unpack_from(">HHI", imgs[k][0]), imgs[k][1]).convert("RGBA"))
    # the head's colour textures: the face (the one with most texels, not the eyes'), the hair's
    mats = {head[p + 0xC] for p in G.parts(head) if not G.is_empty(head, p)}
    mats |= {entries[4][p + 0xC] for p in G.parts(entries[4]) if not G.is_empty(entries[4], p)}
    face = max(mats, key=lambda m: len(U.gc_triangles(head, m)[0]) + len(U.gc_triangles(entries[4], m)[0]))
    hair_mats = sorted({entries[HAIR][p + 0xC] for p in G.parts(entries[HAIR]) if not G.is_empty(entries[HAIR], p)})
    stops = [[float(v) for v in c.split(",")] for c in a.hair.split(":")] if a.hair else None
    if stops:
        for m in hair_mats:
            src = strands(decode(m), a.hair_size, equalize=a.equalize) if a.hair_size else decode(m)
            imgs[m] = G.encode_image(Image.fromarray(hair_colour(src, stops), "RGBA"), imgs[m][0])
            print("hair texture %d recoloured (%dx%d%s)" % (m, src.shape[1], src.shape[0], ", strands" if a.hair_size else ""))
    if a.sculpt_face:
        for e in (HEAD, 4):  # the head and the face with its shapes (their deltas stay as they are)
            P = G.positions(entries[e])
            P2 = sculpt_face(P, a.sculpt_face)
            entries[e] = set_positions(entries[e], P2)
            print("entry %d sculpted: %d positions, at most %.1f" % (e, int((np.abs(P2 - P).max(1) > 0.05).sum()),
                                                                    float(np.abs(P2 - P).max())))
        head = entries[HEAD]
    if a.restyle == "re9":
        P = G.positions(entries[HAIR])
        P2 = restyle_re9(P, np.concatenate([G.positions(entries[HEAD]), G.positions(entries[4])]))
        entries[HAIR] = set_positions(entries[HAIR], P2)
        print("hair restyled (Resident Evil Requiem): %d positions, at most %.1f" % (
            int((np.abs(P2 - P).max(1) > 0.05).sum()), float(np.abs(P2 - P).max())))
    if a.hair_volume or a.hair_length:
        P = G.positions(entries[HAIR])
        P2 = sculpt_hair(P, a.hair_volume, a.hair_length)
        entries[HAIR] = set_positions(entries[HAIR], P2)
        print("hair shaped: %d positions, at most %.1f" % (int((np.abs(P2 - P).max(1) > 0.05).sum()),
                                                          float(np.abs(P2 - P).max())))
    img = decode(face)
    if a.face_size and a.face_size != img.shape[1]:
        img = np.asarray(Image.fromarray(img, "RGBA").resize((a.face_size, a.face_size), Image.LANCZOS))
    pos = uv_positions([head, entries[4]], face, img.shape[1], img.shape[0])
    if stops:
        lum = img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11
        y, z = pos[..., 1], pos[..., 2]
        painted = np.nan_to_num((y > 1752) | ((y > 1690) & (z < 40)), nan=False).astype(bool) & (lum < 60)
        img = hair_colour(img, stops, painted)
        print("face texture %d: %d hair texels recoloured" % (face, int(painted.sum())))
    if a.pale > 0 or a.sockets > 0 or a.pores > 0:
        img = add_skin(img, pos, a.pale, a.sockets, a.pores, a.face_model)
        print("face texture %d: pale %.2f, sockets %.2f, pores %.2f" % (face, a.pale, a.sockets, a.pores))
    if a.nose_lines < 1:  # the head's shape and normals, and the texture's own folds
        entries[HEAD], npos, nnrm = fill_folds_model(entries[HEAD], 1 - a.nose_lines, a.face_model)
        head = entries[HEAD]
        print("head: folds from the nose to the mouth filled (%d positions, %d normals)" % (npos, nnrm))
        img = fill_folds(img, pos, 1 - a.nose_lines, a.face_model)
        print("face texture %d: folds from the nose to the mouth filled %.2f" % (face, 1 - a.nose_lines))
    if a.age > 0:
        img = add_age(img, pos, a.age, a.face_model, a.mouth_lines, a.nose_lines, a.cheek_hollows)
        print("face texture %d: creases of age %.2f" % (face, a.age))
    if a.stubble > 0:
        img = add_stubble(img, pos, a.stubble, colour=[float(v) for v in a.stubble_colour.split(",")],
                          prof=PROFILES[a.face_model], grey=a.stubble_grey,
                          cheeks=a.stubble_cheeks, patchy=a.stubble_patchy,
                          under_lip=a.stubble_under_lip, lip_margin=a.stubble_lip_margin)
        print("face texture %d: stubble %.2f" % (face, a.stubble))
    imgs[face] = G.encode_image(Image.fromarray(img, "RGBA"), imgs[face][0])
    entries[HEAD_TPL] = G.build_tpl(imgs)
    data = U.build_archive(arc, U.build_body(entries, arc.types))
    Path(a.out).write_bytes(data)
    print("%s: %d bytes" % (a.out, len(data)))


if __name__ == "__main__":
    main()
