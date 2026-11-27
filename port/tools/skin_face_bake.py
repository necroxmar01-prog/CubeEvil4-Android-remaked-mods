#!/usr/bin/env python3
"""The disc's head with a new head's face (wish of DevJunior, 27.11.2026: "ponle los ojos, párpados y boca del
clásico pero adaptando la textura a esos aditamentos para que salgan bien las cinemáticas"): the target skin keeps
its own head and face models (entries 2 and 4: the eyes, lids and mouth the game's events move), their colour texture
is baked anew from the source skin's head (another mesh and UV layout) and put in larger.

    skin_face_bake.py TARGET.drs SOURCE.drs OUT.drs [--size 512x1024] [--preview bake.png]

Each texel of the target's head texture (TPL entry 3, the texture its head parts use) has a place on the model
(skin_face.uv_positions, the bind pose); it takes the colour of the source head's surface nearest to that place (the
source's head parts and their textures, every texel's place likewise; the 4 nearest, weighted by distance). Where
the source has no surface within --reach (the inside of the mouth, the eye sockets' back) the target's own texel
stays. Texels no triangle covers take their neighbours' colour (no dark seams once filtered).

Both skins must stand in the same model space (the converted skins of this project do: the same skeleton). Disc and
mod files never go into the repository; this script only reads and writes the paths given."""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_face as F  # noqa: E402

HEAD_TPL = 3


def used_mats(blob, n):
    return [m for m in range(n) if len(U.gc_triangles(blob, m)[0])]


def texel_samples(blobs, imgs, mats):
    """The places (k x 3) and colours (k x 4) of every covered texel of the given colour textures."""
    P, C = [], []
    for m in mats:
        hdr, data = imgs[m]
        h, w, fmt = struct.unpack_from(">HHI", hdr)
        img = np.asarray(U.decode_gx(h, w, fmt, data).convert("RGBA"), dtype=np.float64)
        pos = F.uv_positions(blobs, m, w, h)
        ok = np.isfinite(pos[..., 0])
        P.append(pos[ok])
        C.append(img[ok])
    return np.concatenate(P), np.concatenate(C)


def fill_holes(img, ok, rounds=200):
    """Texels outside `ok` take the mean of covered neighbours, grown outwards."""
    from scipy.ndimage import uniform_filter
    img, ok = img.copy(), ok.copy()
    for _ in range(rounds):
        if ok.all():
            break
        w = uniform_filter(ok.astype(np.float64), 3)
        s = np.stack([uniform_filter(img[..., c] * ok, 3) for c in range(img.shape[2])], -1)
        new = (~ok) & (w > 0.05)  # (a float filter leaves tiny non-zero sums far off)
        img[new] = s[new] / w[new][:, None]
        ok |= new
    return img


def raster(tris_p, tris_uv, w, h, pos, lab, label):
    """Writes each triangle's place per texel into pos (h x w x 3) and `label` into lab where it covers."""
    for k in range(len(tris_p)):
        sx, sy = tris_uv[k][:, 0] * w, tris_uv[k][:, 1] * h
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
        P = tris_p[k]
        for c in range(3):
            v = w0 * P[0, c] + w1 * P[1, c] + w2 * P[2, c]
            pos[y0:y1 + 1, x0:x1 + 1, c][m] = v[m]
        lab[y0:y1 + 1, x0:x1 + 1][m] = label


# Leon's landmarks in the disc head (target) and the Remake face (source) of this project's skins, bind pose: the eye
# balls' middles, the nose tip, the upper lip, the chin, the brow; anchors that stay (the scalp's top, the back of the
# head, the ears, the neck). The target's places are moved by a thin-plate warp through them before the look-up.
LANDMARKS = [((27.8, 1724.5, 70.9), (30.7, 1731.5, 79.0)), ((-27.8, 1724.5, 71.4), (-33.5, 1730.1, 79.9)),
             ((0.0, 1689.4, 110.9), (-1.8, 1692.4, 115.4)), ((0.0, 1668.0, 96.0), (0.0, 1668.0, 95.0)),
             ((0.0, 1625.0, 86.0), (0.0, 1628.0, 80.0)), ((0.0, 1735.0, 94.0), (0.0, 1740.0, 103.5))]
ANCHORS = [(0.0, 1830.0, 0.0), (0.0, 1720.0, -85.0), (87.0, 1710.0, 0.0), (-87.0, 1710.0, 0.0), (0.0, 1600.0, -50.0)]


def warp(points, scale=1.0):
    from scipy.interpolate import RBFInterpolator
    src = np.array([t for t, _ in LANDMARKS] + ANCHORS, dtype=np.float64)
    dsp = np.array([np.subtract(s, t) for t, s in LANDMARKS] + [(0.0, 0.0, 0.0)] * len(ANCHORS)) * scale
    f = RBFInterpolator(src, dsp, kernel="thin_plate_spline", smoothing=1.0)
    return points + f(points)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target")
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--entries", default="2,4", help="the target's head models (default 2,4: head and face)")
    ap.add_argument("--source-entries", default="2", help="the source's head models (default 2)")
    ap.add_argument("--size", default="512x1024", help="the baked texture's size (WxH, at most 1024 each)")
    ap.add_argument("--reach", type=float, default=6.0, help="model units: farther, the target's own texel stays")
    ap.add_argument("--preview", help="also write the baked texture here (PNG)")
    ap.add_argument("--iris", type=float, default=11.0, help="the iris' width on the eye balls (model units)")
    ap.add_argument("--iris-share", type=float, default=0.227, help="the iris' radius in the eye texture (its share)")
    a = ap.parse_args(argv)
    from scipy.spatial import cKDTree
    tarc = U.Archive(open(a.target, "rb").read(), ">")
    sarc = U.Archive(open(a.source, "rb").read(), ">")
    tents = [tarc.entry(i) for i in range(len(tarc.offsets))]
    timgs = G.tpl_images(tents[HEAD_TPL])
    simgs = G.tpl_images(sarc.entry(HEAD_TPL))
    tblobs = [tents[int(e)] for e in a.entries.split(",")]
    sblobs = [sarc.entry(int(e)) for e in a.source_entries.split(",")]
    tmats = sorted(set(m for b in tblobs for m in used_mats(b, len(timgs))))
    smats = sorted(set(m for b in sblobs for m in used_mats(b, len(simgs))))
    # each texture of the target's head models on its own (a skin with the disc's head grafted in has one per model)
    reports = []
    for tm in tmats:
        W, H = (int(v) for v in a.size.lower().split("x"))
        hdr, data = timgs[tm]
        h0, w0, fmt = struct.unpack_from(">HHI", hdr)
        own = np.asarray(U.decode_gx(h0, w0, fmt, data).convert("RGBA").resize((W, H), Image.LANCZOS), dtype=np.float64)
        # the target's parts: skin (the head's largest part and every face part), the eye balls (small parts at the
        # eyes), the rest (the mouth's inside: teeth, tongue) keeps its own texels
        pos = np.full((H, W, 3), np.nan)
        lab = np.zeros((H, W), np.int8)  # 0 none, 1 skin, 2 eye, 3 own
        eyes = [np.array(t) for t, _ in LANDMARKS[:2]]
        for ei, blob in enumerate(tblobs):
            parts = U.gc_triangles(blob, tm, by_part=True)
            big = max(range(len(parts)), key=lambda i: len(parts[i][0])) if parts else -1
            for i, (P, N, T) in enumerate(parts):
                if not len(P):
                    continue
                c = P.reshape(-1, 3).mean(0)
                size = float((P.reshape(-1, 3).max(0) - P.reshape(-1, 3).min(0)).max())
                if i == big or ei > 0:
                    label = 1
                elif size < 45 and min(np.linalg.norm(c - e) for e in eyes) < 12:
                    label = 2
                else:
                    label = 3
                raster(P, T, W, H, pos, lab, label)
        out = own.copy()
        # skin: the source's skin texture (its largest texture), looked up at the warped place
        skin_m = max(smats, key=lambda m: sum(len(U.gc_triangles(b, m)[0]) for b in sblobs))
        sp, sc = texel_samples(sblobs, simgs, [skin_m])
        tree = cKDTree(sp)
        ok = lab == 1
        q = warp(pos[ok])
        d, idx = tree.query(q, k=4)
        wgt = 1.0 / np.maximum(d, 0.05)
        col = (sc[idx] * wgt[..., None]).sum(1) / wgt.sum(1)[:, None]
        t = np.clip((d[:, 0] - a.reach) / (0.5 * a.reach), 0.0, 1.0)[:, None]
        out[ok] = col * (1 - t) + out[ok] * t
        near = d[:, 0] <= a.reach
        # eye balls: the source's eye texture (the one nearest the eyes), by the direction from each ball's middle
        eye_m = min((m for m in smats if m != skin_m),
                    key=lambda m: min(np.linalg.norm(U.gc_triangles(sblobs[0], m)[0].reshape(-1, 3).mean(0)[1:] -
                                                     eyes[0][1:]), 1e9), default=None)
        if eye_m is not None and (lab == 2).any():
            # the eye texture has the iris in its middle (the RE Engine's eyes): each ball's texel looks it up by its
            # offset (x, y) from the ball's front (where the pupil sits), an iris of --iris units across
            ehdr, edata = simgs[eye_m]
            eh, ew, ef = struct.unpack_from(">HHI", ehdr)
            etex = np.asarray(U.decode_gx(eh, ew, ef, edata).convert("RGBA"), dtype=np.float64)
            k = a.iris_share / (0.5 * a.iris)
            for tc, _ in LANDMARKS[:2]:
                sel = (lab == 2) & (np.sign(pos[..., 0]) == np.sign(tc[0]))
                if not sel.any():
                    continue
                q = pos[sel]
                f = q[np.argmax(q[:, 2])]
                u = np.clip(0.5 + (q[:, 0] - f[0]) * k * np.sign(tc[0]), 0.0, 0.999)
                v = np.clip(0.5 - (q[:, 1] - f[1]) * k, 0.0, 0.999)
                out[sel] = etex[(v * eh).astype(int), (u * ew).astype(int)]
        ok = lab > 0
        out[..., 3] = 255.0
        out = fill_holes(out, ok)
        img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")
        if a.preview:
            img.save(a.preview)
        hdr2 = bytearray(hdr)
        struct.pack_into(">HH", hdr2, 0, H, W)
        timgs[tm] = G.encode_image(img, bytes(hdr2))
        reports.append("face bake: texture %d %dx%d -> %dx%d; skin from the source's texture %d (%.0f %% within %.1f "
                       "units, median %.2f), eye balls from %s (%d texels), %d texels kept (mouth)" %
                       (tm, w0, h0, W, H, skin_m, 100.0 * near.mean(), a.reach, float(np.median(d[:, 0])), eye_m,
                        int((lab == 2).sum()), int((lab == 3).sum())))
    tents[HEAD_TPL] = G.build_tpl(timgs)
    outb = U.build_archive(tarc, U.build_body(tents, tarc.types))
    Path(a.out).write_bytes(outb)
    for r in reports:
        print(r)
    print("face bake: %s: %d bytes, data part %d" % (a.out, len(outb), U.Archive(outb, ">").parts[0][1]))


if __name__ == "__main__":
    main()
