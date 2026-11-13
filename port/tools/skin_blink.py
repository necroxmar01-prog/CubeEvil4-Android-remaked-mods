#!/usr/bin/env python3
"""The in-play blink and the eyes of a skin's head (wish of DevJunior, 12.11.2026, device test of the RE9 head:
"the eyes still look bad and it does not blink correctly"):

    skin_blink.py SKIN.drs OUT.drs [--entry 2] [--palette 3] [--eye-mat 2] [--sclera 0.8] [--iris 1.15]
                  [--still-eyes] [--no-blink]

In play the game blinks by turning the eyelid part (0x1C) about x, at most 0.349 rad (pl_class.cpp
cPlayer::moveEyeNormal); the head's shapes play only in cutscenes (patch 0907). The RE9 head took its weights from
the old head's nearest positions: its upper lids on the head part, the skin under the eyes on the lid part, so it did
not blink (and the cheeks would have moved). Here, per eye:

- the eyeball: the --eye-mat part's positions on that side (centre, median radius); the lid rims as in
  skin_shapes.synth_blink (the skin's positions just outside the eyeball, in front; per 1.5 units across the upper
  rim's lowest and the lower rim's highest), each smoothed by a parabola;
- weights: the upper lid (from its rim up to 2.5 units above it fully, fading out smoothly at 6.5, the corners fading
  over 6 units: the skin up to the crease moves, the brow stays) on the lid part, the rest on the head part (nothing else of the head on the lid part any more);
- the sweep: the lid part turns about its joint (the skeleton's, behind the eyes); the angle that brings the upper
  rim's positions down onto the lower rim (80 % of them), plus 2 degrees, over the game's 0.349 is the factor patch
  0919 applies (the model's byte
  0x29, in tenths, 0.5 .. 4.0).

--still-eyes keeps the eyeballs whole (third device test of the RE9 head, 12.11.2026: in the opening cutscene the eyes
were grey noise without iris): their positions took their weights and the face shapes from the old head's nearest
positions, so up to three quarters of an eyeball followed the eye part (whose joint lies 20 units behind the RE9
eyeball's centre: the game turns it 18 degrees as the eyes wander) and the blink shapes (slots 10..12) dragged parts
of it by up to 4.5 units: the iris smeared over the eye. Now every eyeball position is on the head part alone and no
shape moves it (the eyes look ahead; the lids still blink). --no-blink leaves the lid weights as they are.

--sclera F darkens the eye texture outside the iris to F (the white glared in the game's light); --iris S scales the
iris about the texture's middle (its edge found as the strongest dark ring). Every other entry stays. Skins never go
into the repository; this tool only reads and writes the paths given.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pmd_weapon_convert as W  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import skin_slim as S  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

LID, HEAD = 0x1C, 4
EYES = (0x20, 0x21)  # the eye parts (cPlayer::moveEyeNormal turns them)
GAME_MAX = 0.3490658402442932
FULL, FADE = 2.5, 6.5  # the upper lid on the lid part fully up to FULL above its rim, nothing from FADE


def bone_world(blob, index):
    rec = {}
    for j in range(blob[0x19]):
        i, par = blob[0x60 + j * 16], blob[0x61 + j * 16]
        rec[i] = (par, np.array(struct.unpack_from(">3f", blob, 0x64 + j * 16)))
    p = np.zeros(3)
    i = index
    for _ in range(64):
        if i not in rec:
            break
        par, o = rec[i]
        p += o
        if par == 0xFF or par == i:
            break
        i = par
    return p


def rims(P, skin, c, R):
    """Upper and lower lid rim as parabolas y(x) over [x0, x1], from the skin's positions near the eyeball."""
    q = P[skin] - c
    r = np.linalg.norm(q, axis=1)
    near = (np.abs(q[:, 0]) < R + 8) & (np.abs(q[:, 1]) < R + 8) & (q[:, 2] > 2) & (r > R + 0.2) & (r < R + 3.5)
    xs, ys = q[near, 0], q[near, 1]
    if len(xs) < 12:
        return None
    bx, yu, yl = [], [], []
    for b in np.arange(math.floor(xs.min()), math.ceil(xs.max()) + 1, 1.5):
        sel = (xs >= b - 1.5) & (xs < b + 1.5)
        up, lo = ys[sel & (ys > 0)], ys[sel & (ys <= 0)]
        if len(up) and len(lo):
            bx.append(b)
            yu.append(up.min())
            yl.append(lo.max())
    if len(bx) < 4:
        return None
    bx = np.array(bx)
    fu = np.polyfit(bx, yu, 2)
    fl = np.polyfit(bx, yl, 2)
    return bx.min(), bx.max(), fu, fl


def blink(blob, eye_mat, notes):
    P = G.positions(blob)
    weights = G.model_weights(blob)
    vo = struct.unpack_from(">I", blob, 0x30)[0]
    per = [list(weights[struct.unpack_from(">H", blob, vo + i * 8 + 6)[0]]) for i in range(len(P))]
    lists = S.part_lists(blob)
    eye = set()
    for p, tris in lists:
        if blob[p + 0xC] == eye_mat:
            eye.update(c[0] for t in tris for c in t)
    used = set(c[0] for _p, tris in lists for t in tris for c in t)
    skin = np.array(sorted(used - eye))
    eye = np.array(sorted(eye))
    if not len(eye):
        raise SystemExit("no eyeball part drawing with texture %d" % eye_mat)
    pivot = bone_world(blob, LID)
    lidw = np.zeros(len(P))
    need = 0.0
    for side in (-1, 1):
        e = eye[np.sign(P[eye, 0]) == side]
        if len(e) < 20:
            continue
        c = P[e].mean(0)
        R = float(np.median(np.linalg.norm(P[e] - c, axis=1)))
        rim = rims(P, skin, c, R)
        if rim is None:
            notes.append("eye at x %.0f: no lid rims found" % c[0])
            continue
        x0, x1, fu, fl = rim
        q = P[skin] - c
        x = np.clip(q[:, 0], x0, x1)
        yu = np.polyval(fu, x)
        inside = (q[:, 0] > x0 - 3) & (q[:, 0] < x1 + 3) & (q[:, 2] > -2) & (q[:, 1] > yu - 0.6)
        above = q[:, 1] - yu
        w = np.clip(1.0 - (above - FULL) / (FADE - FULL), 0.0, 1.0)
        t = np.clip((np.minimum(q[:, 0] - x0, x1 - q[:, 0]) + 3.0) / 6.0, 0.0, 1.0)
        corner = t * t * (3.0 - 2.0 * t)  # smooth over the corners: no fold at the lid's ends
        w = np.where(inside, w * w * (3.0 - 2.0 * w) * corner, 0.0)
        lidw[skin] = np.maximum(lidw[skin], w)
        # the sweep about the joint: the angle that brings the upper rim's positions (on the lid part fully) down to
        # the lower rim, the 80th percentile of them (a few at the corners lag)
        rimv = skin[inside & (np.abs(above) < 0.8) & (np.abs(q[:, 0] - (x0 + x1) / 2) < (x1 - x0) / 2 - 2)]
        if len(rimv):
            ylow = np.polyval(fl, np.clip(P[rimv, 0] - c[0], x0, x1)) + c[1]
            lo, hi = 0.0, 1.2
            for _ in range(30):
                th = (lo + hi) / 2
                qq = P[rimv] - pivot
                y = qq[:, 1] * math.cos(th) - qq[:, 2] * math.sin(th) + pivot[1]
                if np.percentile(y - ylow, 80) > 0.0:
                    lo = th
                else:
                    hi = th
            need = max(need, hi)
        notes.append("eye at (%.0f, %.0f, %.0f) r %.1f: rims x %.0f..%.0f, opening %.1f" %
                     (c[0], c[1], c[2], R, c[0] + x0, c[0] + x1,
                      max(np.polyval(fu, t) - np.polyval(fl, t) for t in np.linspace(x0, x1, 9))))
    new = []
    moved = 0
    for i in range(len(P)):
        wl = lidw[i]
        rest = [(b, r) for b, r in per[i] if b != LID]
        total = sum(r for _, r in rest)
        if wl > 0:
            moved += 1
            entry = [(LID, 100.0 * wl)]
            if wl < 1.0:
                entry.append((HEAD, 100.0 * (1.0 - wl)))
            new.append(tuple(entry))
        elif len(rest) < len(per[i]):
            # off the lid part: its share to the head part
            lid_share = sum(r for b, r in per[i] if b == LID)
            d = {}
            for b, r in rest:
                d[b] = d.get(b, 0.0) + r
            d[HEAD] = d.get(HEAD, 0.0) + lid_share
            tot = sum(d.values()) or 1.0
            new.append(tuple(sorted(((b, 100.0 * r / tot) for b, r in d.items()), key=lambda x: -x[1])[:3]))
        else:
            new.append(tuple(per[i]) if total else ((HEAD, 100.0),))
    out = bytearray(blob)
    H.write_weights(out, new)
    scale = (need + math.radians(2.0)) / GAME_MAX if need > 0 else 1.0
    tenths = int(max(5, min(40, round(scale * 10))))
    out[0x29] = tenths
    notes.append("lid part: %d positions, the sweep %.0f degrees: factor %.1f (byte 0x29 = %d)" %
                 (moved, math.degrees(need), tenths / 10.0, tenths))
    return bytes(out)


def still_eyes(blob, eye_mat, notes):
    """Every position of the eyeball parts on the head part alone, their face shape offsets zero."""
    P = G.positions(blob)
    weights = G.model_weights(blob)
    vo = struct.unpack_from(">I", blob, 0x30)[0]
    per = [tuple(weights[struct.unpack_from(">H", blob, vo + i * 8 + 6)[0]]) for i in range(len(P))]
    eye = set()
    for p, tris in S.part_lists(blob):
        if blob[p + 0xC] == eye_mat:
            eye.update(c[0] for t in tris for c in t)
    if not eye:
        raise SystemExit("no eyeball part drawing with texture %d" % eye_mat)
    moved = sum(1 for i in eye if per[i] != ((HEAD, 100.0),))
    out = bytearray(blob)
    so = struct.unpack_from(">I", blob, 0x2C)[0]
    cleared = 0
    if so:
        for k in range(struct.unpack_from(">I", blob, so)[0]):
            ofs, cnt = struct.unpack_from(">Ii", blob, so + 4 + k * 8)
            for j in range(cnt):
                o = so + 4 + ofs + j * 8
                if struct.unpack_from(">H", blob, o)[0] in eye and any(out[o + 2:o + 8]):
                    out[o + 2:o + 8] = bytes(6)
                    cleared += 1
    new, skin = [], 0
    for i in range(len(P)):
        if i in eye:
            new.append(((HEAD, 100.0),))
        elif any(b in EYES for b, _r in per[i]):
            # the skin round the eyes took eye part shares too: on the head part (the eye parts turn about a joint
            # far behind the RE9 eyes)
            d = {}
            for b, r in per[i]:
                b = HEAD if b in EYES else b
                d[b] = d.get(b, 0.0) + r
            new.append(tuple(sorted(d.items(), key=lambda x: -x[1])))
            skin += 1
        else:
            new.append(per[i])
    H.write_weights(out, new)
    notes.append("eyeballs: %d positions, %d of them off other parts, %d shape offsets cleared; %d skin positions "
                 "off the eye parts" % (len(eye), moved, cleared, skin))
    return bytes(out)


def eye_texture(img, sclera, iris):
    a = np.asarray(img.convert("RGBA"), dtype=np.float64)
    h, w = a.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    rr = np.hypot((xx + 0.5) / w - 0.5, (yy + 0.5) / h - 0.5)
    lum = a[..., :3] @ np.array([0.3, 0.59, 0.11])
    # the iris edge: the radius where the mean brightness rises most going out
    bins = np.linspace(0.05, 0.48, 44)
    prof = np.array([lum[(rr >= b) & (rr < b + 0.01)].mean() for b in bins])
    edge = float(bins[int(np.argmax(np.diff(prof))) + 1])
    if iris != 1.0:
        src_r = np.where(rr < edge * iris, rr / iris, edge + (rr - edge * iris) * (0.5 - edge) / max(0.5 - edge * iris, 1e-3))
        f = np.where(rr > 1e-6, src_r / np.maximum(rr, 1e-6), 1.0)
        sx = np.clip(((xx + 0.5) / w - 0.5) * f + 0.5, 0, 0.9999) * w
        sy = np.clip(((yy + 0.5) / h - 0.5) * f + 0.5, 0, 0.9999) * h
        a = a[sy.astype(int), sx.astype(int)]
    outside = np.clip((rr - edge * iris) / 0.04, 0, 1)
    a[..., :3] *= (1.0 - outside * (1.0 - sclera))[..., None]
    a[..., 3] = 255
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGBA"), edge


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--entry", type=int, default=2)
    ap.add_argument("--palette", type=int, default=3)
    ap.add_argument("--eye-mat", type=int, default=2)
    ap.add_argument("--sclera", type=float, default=1.0)
    ap.add_argument("--iris", type=float, default=1.0)
    ap.add_argument("--still-eyes", action="store_true", help="the eyeballs on the head part, no shape moves them")
    ap.add_argument("--no-blink", action="store_true", help="the lid weights stay as they are")
    a = ap.parse_args(argv)
    gc = U.Archive(Path(a.inp).read_bytes(), ">")
    entries = [gc.entry(i) for i in range(len(gc.offsets))]
    notes = []
    if not a.no_blink:
        entries[a.entry] = blink(entries[a.entry], a.eye_mat, notes)
    if a.still_eyes:
        entries[a.entry] = still_eyes(entries[a.entry], a.eye_mat, notes)
    if a.sclera != 1.0 or a.iris != 1.0:
        images = W.tpl_images(entries[a.palette])
        disc = U.disc_textures(entries[a.palette])
        h, w, fmt, data = disc[a.eye_mat]
        img, edge = eye_texture(U.decode_gx(h, w, fmt, data), a.sclera, a.iris)
        levels = [img]
        while min(levels[-1].size) > 8:
            levels.append(img.resize((levels[-1].size[0] // 2, levels[-1].size[1] // 2), Image.LANCZOS))
        hdr, dat = H.encode_levels(levels, 0xE, images[a.eye_mat][0], H.enc_cmpr)
        images[a.eye_mat] = (hdr, dat)
        entries[a.palette] = W.build_tpl(images)
        notes.append("eye texture %d: iris edge at %.2f of the side, iris x%.2f, sclera x%.2f" %
                     (a.eye_mat, edge, a.iris, a.sclera))
    out = U.build_archive(gc, U.build_body(entries, gc.types))
    Path(a.out).write_bytes(out)
    print("%s: %d bytes, data part %d" % (a.out, len(out), U.Archive(out, ">").parts[0][1]))
    for n in notes:
        print("  " + n)


if __name__ == "__main__":
    main()
