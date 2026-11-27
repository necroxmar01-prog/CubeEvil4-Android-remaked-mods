#!/usr/bin/env python3
"""Muscle definition painted on a tight shirt (wish of DevJunior, 26.11.2026: "our model should show defined muscles
too", a picture of a Leon in a tight black shirt with pecs and abs): the chest and stomach of a player archive's shirt
texture get the light and shade of muscles under the cloth, made here from a height field of the muscles in model
space (pecs, a six-pack, the line down the middle, the obliques), lit from above and in front; nothing is copied from
another model or picture.

    skin_muscles.py SKIN.drs OUT.drs [--mat 5] [--strength 1.0] [--scale 4] [--max 1024]
                    [--chest-y 1440] [--navel-y 1190] [--front-z 0]

--mat       the shirt's texture (the model part's TEV texture number; Leon's disc pl08: 5, the torso)
--scale     the texture this many times larger first (at most --max a side), with a full mipmap chain
--chest-y / --navel-y   the heights (mm) of the pecs' middle and the navel, the muscles laid out between them
--front-z   texels whose model z (front +z) lies within 45 mm of the front surface at their (x, y) are the front
Writes OUT.drs; prints the texels painted. Shows nothing on the back or the sleeves.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_face as F  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import uhd_skin_convert as U  # noqa: E402


def blob(x, y, cx, cy, rx, ry, power=2.0):
    d = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2
    return np.exp(-d ** (power / 2.0))


def height(x, y, chest, navel):
    """The muscles' height (0..1) over the torso at model (x, y) mm."""
    h = np.zeros_like(x)
    span = chest - navel  # ~250 mm
    # pecs: broad, flat at the top, a firm lower edge
    for s in (-1, 1):
        h += 1.4 * blob(x, y, s * 78.0, chest, 80.0, 0.24 * span, 4.0)
    # six-pack: three rows of pairs and a softer lower pair, narrower as they go down
    rows = [navel + 0.62 * span, navel + 0.40 * span, navel + 0.18 * span, navel - 0.05 * span]
    for i, ry in enumerate(rows):
        for s in (-1, 1):
            h += (0.75 if i < 3 else 0.5) * blob(x, y, s * (36.0 - 2.0 * i), ry, 30.0, 0.095 * span, 3.0)
    # obliques: long soft ridges on the sides of the stomach
    for s in (-1, 1):
        h += 0.35 * blob(x, y, s * 105.0, navel + 0.25 * span, 28.0, 0.45 * span, 2.0)
    # the line down the middle (linea alba) and under the pecs: grooves
    h -= 0.35 * np.exp(-(x / 6.0) ** 2) * ((y < chest + 0.15 * span) & (y > navel - 0.2 * span))
    return h


def shade(x, y, chest, navel, strength):
    """Multiplier per texel: light from above and in front on the height field, the grooves darker."""
    eps = 2.0
    h = height(x, y, chest, navel)
    hx = (height(x + eps, y, chest, navel) - height(x - eps, y, chest, navel)) / (2 * eps)
    hy = (height(x, y + eps, chest, navel) - height(x, y - eps, chest, navel)) / (2 * eps)
    relief = 22.0  # mm of the muscles' height at 1.0
    nx, ny, nz = -hx * relief, -hy * relief, np.ones_like(x)
    n = np.sqrt(nx * nx + ny * ny + nz * nz)
    light = np.array([0.0, 0.55, 0.83])
    lam = (nx * light[0] + ny * light[1] + nz * light[2]) / n
    flat = light[2]
    s = 1.0 + (lam / flat - 1.0) * 1.6 * strength
    s += 0.18 * strength * (np.clip(h, 0, 1.2) - 0.4)  # the muscles a little lighter, the grooves darker
    return np.clip(s, 0.45, 1.8)


def arm_shade(u, th, strength):
    """Multiplier on the arms (T-pose along x): u the distance from the body's middle (mm), th the angle about the
    arm (0 up, +90 the front, -90 the back, radians). Biceps and triceps on the upper arm, the deltoid's edge, the
    forearm's ridge (brachioradialis) and its underside; light from above."""
    def bump(uc, tc, ru, rt, k=1.0):
        dt = np.angle(np.exp(1j * (th - tc)))
        return k * np.exp(-(((u - uc) / ru) ** 2 + (dt / rt) ** 2) ** 1.5)
    def hf(u_, th_):
        global_th = th_
        def b(uc, tc, ru, rt, k=1.0):
            dt = np.angle(np.exp(1j * (global_th - tc)))
            return k * np.exp(-(((u_ - uc) / ru) ** 2 + (dt / rt) ** 2) ** 1.5)
        return (b(345.0, 1.45, 70.0, 0.75, 1.2) + b(330.0, -1.45, 75.0, 0.8, 0.9) + b(255.0, 0.2, 45.0, 1.1, 0.7) +
                b(470.0, 0.55, 75.0, 0.55, 0.7))
    e = 0.04
    h = hf(u, th)
    dth = (hf(u, th + e) - hf(u, th - e)) / (2 * e)
    du = (hf(u + 3.0, th) - hf(u - 3.0, th)) / 6.0
    # (DevJunior, 26.11.2026, device picture: "borra esa mancha negra de los antebrazos") no groove term, the shadows
    # held lighter: the forearm's underside went near black
    s = 1.0 + strength * (0.45 * dth * np.sign(th) + 0.15 * (h - 0.35))
    return np.clip(s, 0.8, 1.45)


def paint_arms(entries, tpl, mat, scale, mx, strength, imgs):
    hdr, data = imgs[mat]
    h0, w0, fmt = struct.unpack_from(">HHI", hdr)
    s = max(1, min(scale, mx // max(w0, h0)))
    w, h = w0 * s, h0 * s
    tex = U.decode_gx(h0, w0, fmt, data).convert("RGBA").resize((w, h), Image.LANCZOS)
    pos = F.uv_positions([entries[0]], mat, w, h)
    ok = np.isfinite(pos[..., 0])
    x, y, z = (np.nan_to_num(pos[..., i]) for i in range(3))
    arm = ok & (np.abs(x) > 200.0)
    # the arm's axis per 10 mm along it: the middle of its section
    u = np.abs(x)
    bins = np.round(u / 10.0).astype(int)
    yc, zc = np.zeros_like(x), np.zeros_like(x)
    for side in (-1, 1):
        sel = arm & (np.sign(x) == side)
        for b in np.unique(bins[sel]):
            m = sel & (bins == b)
            yc[m] = (y[m].max() + y[m].min()) / 2.0
            zc[m] = (z[m].max() + z[m].min()) / 2.0
    th = np.arctan2(z - zc, y - yc)
    mul = np.ones((h, w))
    mul[arm] = arm_shade(u[arm], th[arm], strength)
    from scipy.ndimage import gaussian_filter
    mul = 1.0 + gaussian_filter(mul - 1.0, 0.6 * s)
    px = np.asarray(tex, dtype=np.float64)
    px[..., :3] = np.clip(px[..., :3] * mul[..., None], 0, 255)
    img = Image.fromarray(px.astype(np.uint8), "RGBA")
    imgs[mat] = H.encode_levels([img] + [img.resize(sz, Image.BOX) for sz in H.mip_sizes(w, h)[1:]], 0xE, hdr,
                                H.enc_cmpr)
    print("arms: %d texels of texture %d (%dx%d -> %dx%d)" % (int(arm.sum()), mat, w0, h0, w, h))
    return img


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--mat", type=int, default=5)
    ap.add_argument("--tpl", type=int, default=1, help="the body's palette (archive entry)")
    ap.add_argument("--strength", type=float, default=1.0)
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--max", type=int, default=1024)
    ap.add_argument("--chest-y", type=float, default=1440.0)
    ap.add_argument("--navel-y", type=float, default=1190.0)
    ap.add_argument("--arms", type=int, help="also the arms' skin texture (Leon's disc pl08: 0)")
    ap.add_argument("--arm-strength", type=float, default=1.0)
    ap.add_argument("--preview", help="also write the painted texture here (PNG)")
    a = ap.parse_args(argv)
    arc = U.Archive(open(a.skin, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    imgs = G.tpl_images(entries[a.tpl])
    hdr, data = imgs[a.mat]
    h0, w0, fmt = struct.unpack_from(">HHI", hdr)
    s = max(1, min(a.scale, a.max // max(w0, h0)))
    w, h = w0 * s, h0 * s
    tex = U.decode_gx(h0, w0, fmt, data).convert("RGBA").resize((w, h), Image.LANCZOS)
    pos = F.uv_positions([entries[0]], a.mat, w, h)
    ok = np.isfinite(pos[..., 0])
    x, y, z = pos[..., 0], pos[..., 1], pos[..., 2]
    # the front: within 45 mm of the front-most texel at the same (x, y), 10 mm bins
    front = np.zeros_like(ok)
    bx = np.where(ok, np.round(np.nan_to_num(x) / 10.0), 0).astype(int)
    by = np.where(ok, np.round(np.nan_to_num(y) / 10.0), 0).astype(int)
    best = {}
    for i, j, zz in zip(bx[ok], by[ok], z[ok]):
        if zz > best.get((i, j), -1e9):
            best[(i, j)] = zz
    zf = np.full(z.shape, -1e9)
    zf[ok] = [best[(i, j)] for i, j in zip(bx[ok], by[ok])]
    front = ok & (z > zf - 45.0) & (np.abs(np.nan_to_num(x)) < 175.0) & (z > -20.0)
    torso = front & (np.nan_to_num(y) > a.navel_y - 120.0) & (np.nan_to_num(y) < a.chest_y + 110.0)
    m = np.ones((h, w))
    m[torso] = shade(x[torso], y[torso], a.chest_y, a.navel_y, a.strength)
    # soft edges of the painted area (no seam where the front mask ends)
    from scipy.ndimage import gaussian_filter
    wgt = gaussian_filter(torso.astype(float), 2.0 * s)
    m = 1.0 + (m - 1.0) * np.clip(wgt * 1.6, 0, 1)
    px = np.asarray(tex, dtype=np.float64)
    px[..., :3] = np.clip(px[..., :3] * m[..., None], 0, 255)
    img = Image.fromarray(px.astype(np.uint8), "RGBA")
    if a.preview:
        img.save(a.preview)
    imgs[a.mat] = H.encode_levels([img] + [img.resize(sz, Image.BOX) for sz in H.mip_sizes(w, h)[1:]], 0xE, hdr,
                                  H.enc_cmpr)
    if a.arms is not None:
        arm_img = paint_arms(entries, a.tpl, a.arms, a.scale, a.max, a.arm_strength, imgs)
        if a.preview:
            arm_img.save(a.preview.replace(".png", "_arms.png"))
    entries[a.tpl] = G.build_tpl(imgs)
    body = U.build_body(entries, arc.types)
    out = U.build_archive(arc, body)
    Path(a.out).write_bytes(out)
    print("muscles: %d texels of texture %d (%dx%d -> %dx%d); %s: %d bytes, data part %d" % (
        int(torso.sum()), a.mat, w0, h0, w, h, a.out, len(out), len(body)))


if __name__ == "__main__":
    main()
