#!/usr/bin/env python3
"""One shirt from two (DevJunior, 12.11.2026, device test of v12: "the shirt is still incomplete, it does not fill
the harness, its texture is not the same all over, it still has holes"; he wants the RE9 shirt's V neck):

    skin_shirt.py SKIN.drs OUT.drs --entry 12 --base LITE.drs:12 --base-tex 16 --skin-tex 20
                  [--neck Y:Z:X] [--sleeves X] [--reach 30] [--smooth-sleeves] [--own-uv] [--inside-from DY]
                  [--vee-max X] [--wings X]

The base model's garment (the Lite shirt, texture --base-tex: whole, its back and shoulders included, the harness
made for it) is the shirt's shape. Its torso (|x| below --sleeves) takes the RE9 shirt's look: every triangle draws
with --skin-tex. Front and sides: texture coordinates projected from the nearest triangle of the RE9 shirt's front
piece (its largest texture island; the corners put on that triangle's plane, their barycentric coordinates there
giving the texture coordinates), facing the same way within --reach, front-facing ones failing that within three
times --reach. The back (the RE9 FBX has only its lower part): an affine map of x and y fitted to the RE9 back
piece, heights beyond it mirrored back into it, sides beyond it its edge. So the whole shirt shows the RE9 cloth and
never the atlas's other pieces or gaps.
In front of the neck (above Y, in front of Z, within |x| X: --neck) the base's high round collar gives way to the
RE9 shirt's own triangles (the V neck, the zipper): where they cover a base triangle whole (facing its way, from 25
behind to 20 in front, at its middle and near its corners; upward faces stay) or where the V opens. The RE9 neck's
triangles wound against their normals are turned (the game culls them otherwise), and its collar (above Y + 50) is
also drawn from inside, seen through the V. Best with the RE9 shirt widened to the base's girth first
(skin_slim.py --girth). The base's other parts (the harness) come as they are; the sleeves keep --base-tex, and
with --smooth-sleeves that texture is smoothed and coloured like the RE9 cloth (the Lite's cloth is a blotchy
camouflage).
With --own-uv (v14, DevJunior on v13: "still holes, the texture is not even, the neck is not in proportion") the
torso keeps the base's own texture and coordinates (one continuous layout, no seams) and the RE9 neck draws with it
too; the base's texture becomes the RE9 cloth's colour, even (the RE9 atlas is compressed in 4 x 4 blocks: its fine
detail is block noise, and its collar region holds the label: the light patches). The base stays under the RE9 neck
and only gives way in the V (--vee-max caps its width), the RE9 neck takes the nearest base corner's weights (both
bend alike in every pose), and the base's collar is also drawn from inside (--inside-from). --wings limits the RE9
neck to |x| X. Positions, normals and weights are the base's (the RE9 neck's its own); the model is built anew
(skin_head_fbx build_model, skin_hair_fbx.write_weights). Every other entry stays. Skins never go into the
repository; this tool only reads and writes the paths given.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_body_fbx as BF  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import skin_head_fbx as HF  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

PLAYER_MAX = 0x800000


def unit(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-9)


def islands(UV):
    """Per triangle (UV n x 3 x 2) the id of its texture island (triangles sharing a texture corner)."""
    key = np.round(UV.reshape(-1, 2) * 4096).astype(np.int64)
    _, inv = np.unique(key[:, 0] * 100000 + key[:, 1], return_inverse=True)
    inv = inv.reshape(-1, 3)
    par = np.arange(inv.max() + 1)

    def root(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    for a, b, c in inv:
        ra = root(a)
        par[root(b)] = ra
        par[root(c)] = ra
    return np.array([root(a) for a in inv[:, 0]])


class BackMap:
    """The RE9 shirt's back piece (the largest island facing backwards; the FBX has only its lower part) as an affine
    map of x and y; heights beyond the piece are mirrored back into it (continuous at every fold, so no seams) and
    sides beyond it take its edge, so the
    whole back draws with the back's own cloth and never with the atlas's other pieces or gaps."""

    def __init__(self, P, N, UV, isl):
        best = None
        for i in np.unique(isl):
            m = isl == i
            if N[m, 2].mean() < -0.5 and (best is None or m.sum() > best.sum()):
                best = m
        if best is None or best.sum() < 12:
            raise SystemExit("the RE9 shirt has no back piece")
        X, Y = P[best].reshape(-1, 3), UV[best].reshape(-1, 2)
        self.y0, self.y1 = np.percentile(X[:, 1], 10), np.percentile(X[:, 1], 90)  # off the atlas's ragged edges
        self.x0, self.x1 = np.percentile(X[:, 0], 5), np.percentile(X[:, 0], 95)  # wider sides: the edge stretched
        A = np.c_[X[:, 0], X[:, 1], np.ones(len(X))]
        self.M, *_ = np.linalg.lstsq(A, Y, rcond=None)

    def __call__(self, Pc):
        L = self.y1 - self.y0
        k = np.floor((Pc.mean(1)[:, 1] - self.y0) / L)[:, None]  # one fold per triangle
        y = Pc[..., 1] - self.y0 - k * L
        y = np.where(k % 2 == 0, y, L - y) + self.y0
        x = np.clip(Pc[..., 0], self.x0, self.x1)
        return np.stack([x, y, np.ones(Pc.shape[:2])], -1) @ self.M


def covered(Q, n, R, RN, before=25.0, reach=20.0):
    """Per point Q (n x 3): whether a triangle of R facing its way (normal n) lies on the line through it along n,
    from `before` behind it to `reach` in front."""
    e1, e2 = R[:, 1] - R[:, 0], R[:, 2] - R[:, 0]
    out = np.zeros(len(Q), bool)
    for c in range(0, len(Q), 128):
        o, d = Q[c:c + 128, None, :] - n[c:c + 128, None, :] * before, n[c:c + 128, None, :]
        p = np.cross(d, e2[None])
        det = np.einsum("rtk,tk->rt", p, e1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s = o - R[None, :, 0]
        u = np.einsum("rtk,rtk->rt", s, p) * inv
        q = np.cross(s, e1[None])
        v = np.einsum("rtk,rtk->rt", d, q) * inv
        t = np.einsum("tk,rtk->rt", e2, q) * inv
        face = np.einsum("rk,tk->rt", n[c:c + 128], RN) > 0.3
        out[c:c + 128] = (ok & face & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0) & (t < before + reach)).any(1)
    return out


def project_uv(Pc, Nc, R, RN, RUV, reach, fallback):
    """Texture coordinates for triangles Pc (n x 3 x 3) from the triangles R (m x 3 x 3, normals RN m x 3,
    uv m x 3 x 2): per triangle the nearest R triangle facing the same way within reach; the rest by `fallback`
    (corners -> uv)."""
    from scipy.spatial import cKDTree
    rc = R.mean(1)
    tree = cKDTree(rc)
    c = Pc.mean(1)
    n = unit(Nc.mean(1))
    d, near = tree.query(c, k=8)
    uv = np.zeros(Pc.shape[:2] + (2,))
    projected = np.zeros(len(Pc), bool)
    for i in range(len(Pc)):
        for dd, j in zip(d[i], near[i]):
            if not np.isfinite(dd) or dd > reach or RN[j] @ n[i] < 0.3:
                continue
            a, b, cc = R[j]
            e1, e2 = b - a, cc - a
            nn = np.cross(e1, e2)
            if np.linalg.norm(nn) < 1e-9:
                continue
            nn = nn / np.linalg.norm(nn)
            q = Pc[i] - np.outer((Pc[i] - a) @ nn, nn)  # onto its plane
            # barycentric coordinates (may lie outside: an affine extension of that triangle's mapping)
            M = np.array([[e1 @ e1, e1 @ e2], [e1 @ e2, e2 @ e2]])
            rhs = np.stack([(q - a) @ e1, (q - a) @ e2], 1)
            try:
                st = np.linalg.solve(M, rhs.T).T
            except np.linalg.LinAlgError:
                continue
            w = np.c_[1 - st.sum(1), st]
            if np.abs(w).max() > 2.5:  # too far out of it: its mapping would stretch
                continue
            uv[i] = w @ RUV[j]
            projected[i] = True
            break
    rest = ~projected
    if rest.any():
        uv[rest] = fallback(Pc[rest])
    return uv, int(projected.sum())


def cloth_samples(img, UV, n=24, seed=1):
    """RGB samples (k x 3) of `img` at n random points in each triangle of UV (m x 3 x 2; v up, as model_corners
    gives them), as the game samples it."""
    a = np.asarray(img.convert("RGB"), dtype=np.float64)
    h, w = a.shape[:2]
    r = np.random.default_rng(seed).dirichlet((1, 1, 1), size=(len(UV), n))
    q = np.einsum("mnk,mkj->mnj", r, UV).reshape(-1, 2)
    return a[np.clip(((1.0 - q[:, 1]) % 1) * h, 0, h - 1).astype(int), np.clip((q[:, 0] % 1) * w, 0, w - 1).astype(int)]


def uniform_cloth(ref, size, vary=0.0, seed=7):
    """An even cloth texture of `size` (w, h): the RE9 cloth's colour (median of the RGB samples `ref`: its texture is
    compressed in 4 x 4 blocks of two colours, so its fine detail is block noise, not weave), its brightness varying
    by `vary` in broad soft patches (default none: the shirt's dark blue in 5-6-5 bit steps showed every variation as
    a blotch). The base's coordinates draw the whole shirt with it; the
    folds' shading is the game's light on the shape."""
    ref = ref[(ref @ [0.3, 0.59, 0.11]) > 4]
    colour = np.median(ref, 0)
    W, H = size
    rng = np.random.default_rng(seed)
    noise = Image.fromarray((rng.random((H // 16 + 1, W // 16 + 1)) * 255).astype(np.uint8)).resize(
        (W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(12))
    n = np.asarray(noise, dtype=np.float64)
    n = (n - n.mean()) / max(n.std(), 1e-6)
    out = colour[None, None, :] * (1.0 + vary * np.clip(n, -2, 2))[..., None]
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGB").convert("RGBA"), colour


def smooth_cloth(img, ref):
    """The base's cloth texture smoothed (the blotches gone, the folds' shading kept) and coloured like `ref` (RGB
    samples of the RE9 cloth: their mean and spread of brightness)."""
    a = np.asarray(img.convert("RGBA"), dtype=np.float64)
    rgb = a[..., :3]
    lum = rgb @ [0.3, 0.59, 0.11]
    big = np.asarray(Image.fromarray(np.clip(lum, 0, 255).astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(max(img.size) / 40.0)), dtype=np.float64)
    ref = ref[(ref @ [0.3, 0.59, 0.11]) > 4]  # the cloth, not a stray black texel
    mean_c = ref.mean(0)
    mean_l, std_l = (ref @ [0.3, 0.59, 0.11]).mean(), (ref @ [0.3, 0.59, 0.11]).std()
    shade = (big - big.mean()) / max(big.std(), 1e-6) * std_l * 0.8 + mean_l
    out = mean_c[None, None, :] * (shade / max(mean_l, 1e-6))[..., None]
    a[..., :3] = np.clip(out, 0, 255)
    return Image.fromarray(a.astype(np.uint8), "RGBA")


def build(cur, base, base_tex, skin_tex, neck, sleeves, reach, own_uv=False, inside_from=50.0, vee_max=1e9,
          wings=1e9):
    cc = BF.model_corners(cur)
    bc = BF.model_corners(base)
    parts = {cur[p + 0xC]: p for p, *_ in cc}
    for t in {base[p + 0xC] for p, *_ in bc} | {skin_tex}:
        if t not in parts:
            raise SystemExit("the skin's model has no part drawing with texture %d" % t)
    A = np.concatenate([Pc for p, Pc, *_ in cc if cur[p + 0xC] == skin_tex])
    AN = unit(np.concatenate([Nc.mean(1) for p, _P, Nc, *_ in cc if cur[p + 0xC] == skin_tex]))
    AUV = np.concatenate([UVc for p, _P, _N, UVc, _w in cc if cur[p + 0xC] == skin_tex])
    isl = islands(AUV)
    # the texture comes from the RE9 shirt's front piece only (its largest island: front and sides in one); its
    # other pieces (collar, linings, seams, hems) would put stray bits of the atlas on the chest
    ids, cnt = np.unique(isl, return_counts=True)
    front = isl == ids[np.argmax(cnt)]
    R, RN, RUV = A[front], AN[front], AUV[front]
    fallback = BackMap(A, AN, AUV, isl)
    ny, nz, nx = neck
    ac = A.mean(1)
    # the RE9 shirt's triangles that come in front of the neck (a little beyond the box: they overlap the base's edge)
    vneck = (ac[:, 1] > ny - 10.0) & (ac[:, 2] > nz - 5.0) & (np.abs(ac[:, 0]) < nx + 10.0)
    groups = {t: [] for t in parts}
    stats = {"ref_uv": RUV}
    # the base garment's corners and weights: the RE9 neck takes the weights of the nearest (so both bend alike in
    # every pose; with their own weights gaps opened between them)
    from scipy.spatial import cKDTree
    bP = np.concatenate([Pc.reshape(-1, 3) for p, Pc, *_ in bc if base[p + 0xC] == base_tex])
    bW = [w for p, _P, _N, _U, wc in bc if base[p + 0xC] == base_tex for w in wc]
    bUV = np.concatenate([UVc.reshape(-1, 2) for p, _P, _N, UVc, _w in bc if base[p + 0xC] == base_tex])
    # with own_uv the RE9 neck draws with the base's (recoloured) texture too, at the nearest base corner's
    # coordinates: the RE9 atlas around its collar holds the label and other pieces (light patches in the poses) and
    # its compressed blocks never matched the even cloth
    neck_tex = base_tex if own_uv else skin_tex
    btree = cKDTree(bP)
    for p, Pc, Nc, UVc, wc in bc:
        t = base[p + 0xC]
        if t != base_tex:
            groups[t].append((Pc, Nc, UVc, wc))
            continue
        c = Pc.mean(1)
        in_neck = (c[:, 1] > ny) & (c[:, 2] > nz) & (np.abs(c[:, 0]) < nx)
        # there the base gives way only where the RE9 neck's triangles cover it whole (facing its way, from 25 behind
        # to 20 in front, at its middle and near its corners) or where the RE9 shirt's V opens (|x| < 0.6 (y - Y - 10):
        # the neck shows); elsewhere (under the harness straps the RE9 shirt has holes) the base stays
        nrm = unit(Nc.mean(1))
        cov = np.ones(len(c), bool)
        for q in [c] + [(c + Pc[:, k]) / 2 for k in range(3)] + [0.1 * c + 0.9 * Pc[:, k] for k in range(3)]:
            cov &= covered(q, nrm, A[vneck], AN[vneck])
        vee = np.abs(c[:, 0]) < np.minimum(0.6 * (c[:, 1] - ny - 10.0), vee_max)
        # surfaces facing upwards (the shoulders' tops) stay: seen from the front at a grazing angle they show more
        # than the RE9 shirt below them covers
        # with own_uv both draw the same even cloth: the covered base stays under the RE9 neck (in the poses the two
        # bend a little apart; without it the gaps showed through) and only gives way in the V
        in_neck &= vee if own_uv else ((cov & (nrm[:, 1] < 0.5)) | vee)
        torso = (np.abs(c[:, 0]) < sleeves) & ~in_neck
        sleeve = (np.abs(c[:, 0]) >= sleeves) & ~in_neck
        sel = lambda m: (Pc[m], Nc[m], UVc[m], [w for k, w in zip(np.repeat(m, 3), wc) if k])
        groups[t].append(sel(sleeve))
        # front-facing ones the near pass misses (where the base stands out from the RE9 shirt, the chest's top) try
        # again farther before the back's map would put the back's cloth on the front
        far = lambda X: project_uv(X, np.repeat(np.array([[[0.0, 0.0, 1.0]]]), len(X), 0).repeat(3, 1), R, RN, RUV,
                                   reach * 3, fallback)[0]
        if own_uv:  # the base's own texture and coordinates (no seams at all), the texture recoloured in main
            groups[t].append(sel(torso))
            nproj = 0
            # the base's collar around the neck (above Y + inside_from) also from inside: when the head turns one
            # looks past the neck at the inside of the back collar (else culled: the sky through the shirt)
            m3 = torso & (c[:, 1] > ny + inside_from) & (np.abs(c[:, 0]) < nx)
            rev = [0, 2, 1]
            groups[t].append((Pc[m3][:, rev], -Nc[m3][:, rev], UVc[m3][:, rev],
                              [wc[3 * i + k] for i in np.flatnonzero(m3) for k in rev]))
            stats["base_inside"] = int(m3.sum())
        else:
            uv, nproj = project_uv(Pc[torso], Nc[torso], R, RN, RUV, reach,
                                   lambda X: np.where((X.mean(1)[:, 2] > 0)[:, None, None], far(X), fallback(X)))
            P_, N_, _uv, w_ = sel(torso)
            groups[skin_tex].append((P_, N_, uv, w_))
        stats.update(torso=int(torso.sum()), projected=nproj, sleeves=int(sleeve.sum()), neck_out=int(in_neck.sum()))
    # the RE9 neck's triangles themselves
    vn = flipped = inside = 0
    for p, Pc, Nc, UVc, wc in cc:
        if cur[p + 0xC] != skin_tex:
            continue
        c = Pc.mean(1)
        m = (c[:, 1] > ny - 10.0) & (c[:, 2] > nz - 5.0) & (np.abs(c[:, 0]) < min(nx, wings) + 10.0)
        # turned the way their normals face (the FBX has some wound backwards: the game would cull them, holes)
        g = np.einsum("ij,ij->i", np.cross(Pc[:, 1] - Pc[:, 0], Pc[:, 2] - Pc[:, 0]), Nc.mean(1))
        order = np.where((g < 0)[:, None], [0, 2, 1], [0, 1, 2])
        ii = np.arange(len(Pc))[:, None]
        Pc, Nc, UVc = Pc[ii, order], Nc[ii, order], UVc[ii, order]
        near = btree.query(Pc.reshape(-1, 3))[1]
        wc = [bW[j] for j in near]
        if own_uv:
            UVc = bUV[near].reshape(-1, 3, 2)
        flipped += int((m & (g < 0)).sum())
        groups[neck_tex].append((Pc[m], Nc[m], UVc[m], [w for k, w in zip(np.repeat(m, 3), wc) if k]))
        # the collar (above Y + 50) also from inside: through the V one looks at the inside of its back, which the
        # game culls when drawn one-sided (holes beside the neck)
        m2 = m & (c[:, 1] > ny + inside_from)
        rev = [0, 2, 1]
        groups[neck_tex].append((Pc[m2][:, rev], -Nc[m2][:, rev], UVc[m2][:, rev],
                                 [wc[3 * i + k] for i in np.flatnonzero(m2) for k in rev]))
        inside += int(m2.sum())
        vn += int(m.sum())
    stats["neck_in"], stats["flipped"], stats["inside"] = vn, flipped, inside
    Ps, Ns, Ts, Ws, glist = [], [], [], [], []
    for t, p in parts.items():
        tl = []
        for Pc, Nc, UVc, wc in groups[t]:
            if not len(Pc):
                continue
            base_p, base_t = sum(len(x) for x in Ps), sum(len(x) for x in Ts)
            k = len(Pc)
            Ps.append(Pc.reshape(-1, 3)), Ns.append(Nc.reshape(-1, 3)), Ts.append(UVc.reshape(-1, 2))
            Ws.extend(wc)
            idx = np.arange(k * 3).reshape(-1, 3)
            tl.append(np.stack([idx + base_p, idx + base_p, idx + base_t], 2))
        if tl:
            glist.append((p, np.concatenate(tl)))
    P, N, T = np.concatenate(Ps), np.concatenate(Ns), np.concatenate(Ts)
    model = HF.build_model(cur, P, N, T, glist)
    per = []
    for wd in Ws:
        top = sorted(wd.items(), key=lambda x: -x[1])[:3]
        tot = sum(v for _, v in top) or 1.0
        per.append(tuple((b, 100.0 * v / tot) for b, v in top))
    order = BF.model_positions_order(P, model)
    model = bytearray(model)
    H.write_weights(model, [per[i] for i in order])
    return bytes(model), stats


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--entry", type=int, default=12)
    ap.add_argument("--palette", type=int, default=1, help="the entry's palette (default 1)")
    ap.add_argument("--base", required=True, metavar="LITE.drs:ENTRY")
    ap.add_argument("--base-tex", type=int, default=16)
    ap.add_argument("--skin-tex", type=int, default=20)
    ap.add_argument("--neck", default="1470:20:115", metavar="Y:Z:X")
    ap.add_argument("--sleeves", type=float, default=165.0)
    ap.add_argument("--reach", type=float, default=30.0)
    ap.add_argument("--smooth-sleeves", action="store_true")
    ap.add_argument("--inside-from", type=float, default=50.0, metavar="DY",
                    help="the RE9 collar above Y + DY is also drawn from inside (default 50)")
    ap.add_argument("--vee-max", type=float, default=1e9, metavar="X",
                    help="the V opens at most |x| X (narrower than the neck: the base's collar keeps hugging its "
                         "sides in every pose)")
    ap.add_argument("--wings", type=float, default=1e9, metavar="X",
                    help="only the RE9 neck's triangles within |x| X come (its collar wings beyond lift off the "
                         "neck in the poses; the base's collar stays there)")
    ap.add_argument("--own-uv", action="store_true",
                    help="the torso keeps the base's texture and coordinates (recoloured with the sleeves)")
    a = ap.parse_args(argv)
    gc = U.Archive(Path(a.inp).read_bytes(), ">")
    entries = [gc.entry(i) for i in range(len(gc.offsets))]
    src, _, be = a.base.rpartition(":")
    base = U.Archive(Path(src).read_bytes(), ">").entry(int(be))
    neck = tuple(float(x) for x in a.neck.split(":"))
    entries[a.entry], st = build(entries[a.entry], base, a.base_tex, a.skin_tex, neck, a.sleeves, a.reach,
                                 a.own_uv, a.inside_from, a.vee_max, a.wings)
    print("torso: %d triangles of the base drawn with texture %d (%d projected from the RE9 shirt near them, the "
          "rest from farther or mapped like its back); sleeves %d; neck: %d of the base's out, %d of the RE9 shirt's "
          "in (%d of them turned, %d of the collar also from inside)" %
          (st["torso"], a.skin_tex, st["projected"], st["sleeves"], st["neck_out"], st["neck_in"],
           st["flipped"], st["inside"]))
    if a.smooth_sleeves or a.own_uv:
        imgs = G.tpl_images(entries[a.palette])
        h16, h20 = imgs[a.base_tex][0], imgs[a.skin_tex][0]
        import struct
        sl = U.decode_gx(*struct.unpack_from(">HHI", h16), imgs[a.base_tex][1])
        like = U.decode_gx(*struct.unpack_from(">HHI", h20), imgs[a.skin_tex][1])
        if a.own_uv:
            cloth, colour = uniform_cloth(cloth_samples(like, st["ref_uv"]), sl.size)
            imgs[a.base_tex] = G.encode_image(cloth, h16)
            print("shirt: texture %d replaced by the RE9 cloth's colour (%d,%d,%d, from texture %d's front piece)" %
                  (a.base_tex, *[int(round(c)) for c in colour], a.skin_tex))
        else:
            imgs[a.base_tex] = G.encode_image(smooth_cloth(sl, cloth_samples(like, st["ref_uv"])), h16)
            print("sleeves: texture %d smoothed and coloured like the RE9 shirt's front piece (texture %d)" %
                  (a.base_tex, a.skin_tex))
        entries[a.palette] = G.build_tpl(imgs)
    body = U.build_body(entries, gc.types)
    out = U.build_archive(gc, body)
    Path(a.out).write_bytes(out)
    print("%s: %d bytes, data part %d (the player's places hold %d)" % (a.out, len(out), len(body), PLAYER_MAX))
    if len(body) > PLAYER_MAX:
        raise SystemExit("too large for the player's place")


if __name__ == "__main__":
    main()
