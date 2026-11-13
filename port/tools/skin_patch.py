#!/usr/bin/env python3
"""Fills the holes of a skin's garment with the triangles of an older model of it (DevJunior, 12.11.2026, device test
of the RE9 shirt without the coat: "there are empty spaces and it does not look good"):

    skin_patch.py SKIN.drs OUT.drs --entry 12 --from OLD.drs[:12] --mats 16 [--cover-mats 20,16] [--reach 30]
                  [--back-only] [--as-tex 20]

The RE9 shirt (skin_body_fbx.py --merge --replace-mats 16 --replace-x 175) took the place of the old shirt's torso.
Its FBX was exported with the jacket on, so the shirt has no back where the jacket hid it: under the harness of pl08,
which has no coat, the body, the background or the shirt's inside showed through.

Every triangle of the old model's parts with the textures --mats is tested at its centre and halfway to each corner:
a ray along its normal (from 1 unit behind it, --reach long) and one against it (--behind, default 6) against the
current model's parts with the textures --cover-mats (default: all, the harness too). A triangle is added when at
least one of its points has no current surface in front of it and none of them has one just behind it (an old surface
lying over the new one or over the harness would stick out). It keeps the old model's positions, normals and weights.

Second device test (12.11.2026): "there are still holes, a piece on the back covers it very badly, and you put a piece
of clothing on the neck that spoils the V neck": the old shirt's texture is a dark, blotchy blue, its collar filled
the RE9 shirt's V neck, and old pieces lay over the harness. --back-only takes only triangles behind the body's middle
that face backwards; --as-tex T draws them with the current texture T, their texture coordinates an affine map of x
and y fitted to the triangles with texture T already on the back (the RE9 shirt's texture has its whole back panel:
the patch continues it).

The model is built anew (skin_head_fbx.build_model, skin_hair_fbx.write_weights: the normals split by weight entry).
Every other entry stays. Skins never go into the repository; this tool only reads and writes the paths given.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_body_fbx as BF  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import skin_head_fbx as HF  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

PLAYER_PLACE = 0x600000


def ray_hit_dist(origins, dirs, T, reach):
    """Per ray: whether it hits a triangle of T within reach."""
    e1 = T[:, 1] - T[:, 0]
    e2 = T[:, 2] - T[:, 0]
    out = np.zeros(len(origins), bool)
    for c in range(0, len(origins), 192):
        o, d = origins[c:c + 192, None, :], dirs[c:c + 192, None, :]
        p = np.cross(d, e2[None])
        det = np.einsum("rtk,tk->rt", p, e1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s = o - T[None, :, 0]
        u = np.einsum("rtk,rtk->rt", s, p) * inv
        q = np.cross(s, e1[None])
        v = np.einsum("rtk,rtk->rt", d, q) * inv
        t = np.einsum("tk,rtk->rt", e2, q) * inv
        out[c:c + 192] = (ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0) & (t < reach)).any(1)
    return out


def back_uv_fit(cc, cur, tex):
    """The affine map (x, y, 1) -> texture coordinates of the current parts with texture `tex` on the back (corners of
    triangles facing backwards behind the body's middle), least squares; None with too few."""
    X, Y = [], []
    for p, Pc, Nc, UVc, _w in cc:
        if cur[p + 0xC] != tex:
            continue
        n = Nc.mean(1)
        m = (Pc.mean(1)[:, 2] < 0) & (n[:, 2] < -0.5 * np.linalg.norm(n, axis=1))
        X.append(Pc[m].reshape(-1, 3)), Y.append(UVc[m].reshape(-1, 2))
    X, Y = np.concatenate(X), np.concatenate(Y)
    if len(X) < 12:
        return None
    A = np.c_[X[:, 0], X[:, 1], np.ones(len(X))]
    M, *_ = np.linalg.lstsq(A, Y, rcond=None)
    return M


def patch(cur, old, mats, cover_mats, reach=30.0, behind=6.0, back_only=False, as_tex=None):
    """Returns (new model, triangles added per texture)."""
    cc = BF.model_corners(cur)
    cover = np.concatenate([Pc for p, Pc, _n, _t, _w in cc if cur[p + 0xC] in cover_mats])
    fit = None
    if as_tex is not None:
        fit = back_uv_fit(cc, cur, as_tex)
        if fit is None:
            raise SystemExit("the current model has too little of texture %d on the back to fit its mapping" % as_tex)
    added = {}
    extra = {}
    for p, Pc, Nc, UVc, wc in BF.model_corners(old):
        tex = old[p + 0xC]
        if tex not in mats:
            continue
        n = Nc.mean(1)
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        cen = Pc.mean(1)
        pts = [cen] + [(cen + Pc[:, k]) / 2 for k in range(3)]
        open_ = np.zeros(len(cen), bool)
        over = np.zeros(len(cen), bool)
        for q in pts:
            # in front: from 1 unit behind (a surface where the old one lies counts), behind: the rest of --behind
            open_ |= ~ray_hit_dist(q - n, n, cover, reach + 1.0)
            over |= ray_hit_dist(q - n, -n, cover, behind - 1.0)
        take = open_ & ~over
        if back_only:
            take &= (cen[:, 2] < 0) & (n[:, 2] < -0.3)
        if not take.any():
            continue
        to = tex if as_tex is None else as_tex
        uv = UVc[take]
        if fit is not None:
            P = Pc[take]
            uv = (np.stack([P[..., 0], P[..., 1], np.ones(P.shape[:2])], -1) @ fit)
        extra.setdefault(to, []).append((Pc[take], Nc[take], uv, [w for k, w in zip(np.repeat(take, 3), wc) if k]))
        added[tex] = added.get(tex, 0) + int(take.sum())
    if not added:
        return cur, added
    Ps, Ns, Ts, Ws, groups = [], [], [], [], []

    def take(hdr, Pc, Nc, UVc, wc):
        base_p, base_t = sum(len(x) for x in Ps), sum(len(x) for x in Ts)
        k = len(Pc)
        Ps.append(Pc.reshape(-1, 3)), Ns.append(Nc.reshape(-1, 3)), Ts.append(UVc.reshape(-1, 2))
        Ws.extend(wc)
        idx = np.arange(k * 3).reshape(-1, 3)
        return np.stack([idx + base_p, idx + base_p, idx + base_t], 2)

    for p, Pc, Nc, UVc, wc in cc:
        tl = [take(p, Pc, Nc, UVc, wc)]
        for Pe, Ne, Te, we in extra.pop(cur[p + 0xC], []):
            tl.append(take(p, Pe, Ne, Te, we))
        groups.append((p, np.concatenate(tl)))
    if extra:
        raise SystemExit("no part of the current model draws with texture(s) %s" % sorted(extra))
    P, N, T = np.concatenate(Ps), np.concatenate(Ns), np.concatenate(Ts)
    model = HF.build_model(cur, P, N, T, groups)
    per = []
    for wd in Ws:
        top = sorted(wd.items(), key=lambda x: -x[1])[:3]
        tot = sum(v for _, v in top) or 1.0
        per.append(tuple((b, 100.0 * v / tot) for b, v in top))
    order = BF.model_positions_order(P, model)
    model = bytearray(model)
    H.write_weights(model, [per[i] for i in order])
    return bytes(model), added


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--entry", type=int, default=12)
    ap.add_argument("--from", dest="src", required=True, metavar="OLD.drs[:ENTRY]")
    ap.add_argument("--mats", required=True, help="the old model's textures to fill with")
    ap.add_argument("--cover-mats", default="", help="the current model's textures that count as cover (default: all)")
    ap.add_argument("--reach", type=float, default=30.0)
    ap.add_argument("--behind", type=float, default=6.0)
    ap.add_argument("--back-only", action="store_true",
                    help="only triangles on the back that face backwards (the V neck in front stays open)")
    ap.add_argument("--as-tex", type=int, help="draw the added triangles with this current texture, their texture "
                    "coordinates fitted (affine in x, y) to its triangles on the back")
    a = ap.parse_args(argv)
    gc = U.Archive(Path(a.inp).read_bytes(), ">")
    entries = [gc.entry(i) for i in range(len(gc.offsets))]
    src, _, se = a.src.partition(":")
    old = U.Archive(Path(src).read_bytes(), ">").entry(int(se) if se else a.entry)
    cur = entries[a.entry]
    mats = {int(x) for x in a.mats.split(",")}
    cover = ({int(x) for x in a.cover_mats.split(",")} if a.cover_mats else
             {cur[p + 0xC] for p, *_ in BF.model_corners(cur)})
    entries[a.entry], added = patch(cur, old, mats, cover, a.reach, a.behind, a.back_only, a.as_tex)
    body = U.build_body(entries, gc.types)
    out = U.build_archive(gc, body)
    Path(a.out).write_bytes(out)
    print("%s: %d bytes, data part %d (the player's place holds %d)" % (a.out, len(out), len(body), PLAYER_PLACE))
    for tex, n in sorted(added.items()):
        print("  entry %d: %d triangles of texture %d from %s%s" % (a.entry, n, tex, a.src,
              "" if a.as_tex is None else ", drawn with texture %d" % a.as_tex))
    if not added:
        print("  nothing to fill")
    if len(body) > PLAYER_PLACE:
        raise SystemExit("too large for the player's place")


if __name__ == "__main__":
    main()
