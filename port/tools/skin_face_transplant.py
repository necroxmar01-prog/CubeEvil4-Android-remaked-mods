#!/usr/bin/env python3
"""A skin's head with the disc's eyes, lids and mouth (wish of DevJunior, 27.11.2026: "modelarás la cabeza nueva … y
le cambiarás los ojos por los del modelo clásico y la boca por la del modelo clásico … para que pueda concordar bien
con las cinemáticas"): the cutscenes move Leon's face by the shapes of the event's own head ev0001 (skin_shapes.py
carries them onto a skin's head by the nearest points; on a face of other proportions the lids and lips came out
wrong). This tool cuts the skin head's eyes and mouth out and puts the event head's in their place, so that those
parts are the event's own and take its shapes one to one:

    skin_face_transplant.py SKIN.drs OUT.drs --event r100s03.evd[.lfs] [--preview DIR]

- The event head (the PC release's or the disc's event file, converted to the GameCube's form) is put on the skin's
  face as skin_shapes.py puts it (heights piecewise by chin, mouth, nose, eyes, brow; depth by the front profile).
- Out of the skin's head (entry 2): its eyeballs and their wet layer, and the face's triangles around the eyes (an
  ellipse --eye-r around each eyeball's middle) and the mouth (--mouth-r around the lips' parting), in front.
- In: the event head's eyeballs, and its triangles within the same ellipses grown by --margin (lids, lashes' line,
  lips, teeth, tongue). Their outer edge is drawn onto the skin's surface (the nearest point of it, fading in over the
  margin), so the patch meets the face without a step.
- Their textures, new in the head's TPL: the skin's face colour at each texel's place (the nearest face texel), the
  inside of the mouth (more than --inside behind the face) a dark red, the eyeballs the skin's eye texture by each
  texel's offset from the ball's front.
- The skin's lashes (entry 4) are left out (they lay on the skin's own lids); the head is weighted anew from the old
  head (skin_hair_fbx's way). The head gets its shape table here (do not run skin_shapes.py on it): the patch's
  positions the event's own movements, the rest of the face skin_shapes.carry_shapes'.

Disc and mod files never go into the repository; this tool only reads and writes the paths given."""
import argparse
import os
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_face as F  # noqa: E402
import skin_shapes as S  # noqa: E402
import skin_head_fbx as HF  # noqa: E402
import skin_hair_fbx as H  # noqa: E402

HEAD, HEAD_TPL, FACE = 2, 3, 4


def event_model(path):
    """The event head ev0001 as a GameCube model, its positions and shapes (as skin_shapes.event_head)."""
    data = Path(path).read_bytes()
    if path.lower().endswith(".lfs"):
        import re4lfs
        data = re4lfs.unpack(data)
    at = 0
    while True:
        at = data.find(S.EVENT_HEAD, at)
        if at < 0:
            raise SystemExit("%s: no event head" % path)
        for e in "<>":
            off, size = struct.unpack_from(e + "II", data, at + 0x30)
            if 0 < off and off + size <= len(data) and size > 0x50 and \
                    struct.unpack_from(e + "I", data, off + 0x3C)[0] in S.VERSIONS:
                blob = data[off:off + size]
                if e == "<":
                    P, shapes = S.pc_head(blob)
                    gc = U.convert_model(blob, 4096, [])[0]
                else:
                    P, shapes = S.gc_head(blob)
                    gc = blob
                return gc, P, shapes
        at += 1


def aligner(Sk, E, shapes):
    """The event's points onto the skin's face (as skin_shapes.carry_shapes puts them)."""
    ls, (sys_, sz) = S.landmarks(Sk)
    le, (eys, ez) = S.landmarks(E, opening=shapes[1] if len(shapes) > 1 else None)
    keys = ("chin", "mouth", "nose", "eyes", "brow", "top")
    xe, xs = [le[q] for q in keys], [ls[q] for q in keys]

    def align(Q):
        Q = np.asarray(Q, float)
        y = Q[:, 1]
        ys = np.interp(y, xe, xs) + np.where(y < xe[0], y - xe[0], 0) + np.where(y > xe[-1], y - xe[-1], 0)
        dz = np.interp(ys, sys_, sz) - np.interp(y, eys, ez)
        dz = np.where(np.isfinite(dz), dz, 0.0)
        front = np.clip(Q[:, 2] / 30.0, 0, 1)
        return np.stack([Q[:, 0], ys, Q[:, 2] + dz * front], 1)
    return align, ls, le


def tri_indices(blob, mat):
    """The position index of each corner of the triangles uhd_skin_convert.gc_triangles gives for texture `mat`, in
    the same order (positions repeat in a model: the shapes name the index, not the place)."""
    u = lambda o: struct.unpack_from(">I", blob, o)[0]
    clr = bool(u(0x20) & 0x80000000)
    out = []
    p = u(0x1C)
    for _ in range(struct.unpack_from(">H", blob, 0x1A)[0]):
        size, m = u(p + 0x18), blob[p + 0xC]
        s, e = p + 0x20, p + 0x20 + size
        while s < e and blob[s] != 0:
            op, cnt = blob[s], struct.unpack_from(">H", blob, s + 1)[0]
            per = 4 if clr else 3
            idx = [struct.unpack_from(">%dH" % per, blob, s + 3 + k * per * 2)[0] for k in range(cnt)]
            s += 3 + cnt * per * 2
            if m != mat:
                continue
            if op == 0x98:
                out += [(idx[k], idx[k + 1], idx[k + 2]) if k % 2 == 0 else (idx[k + 1], idx[k], idx[k + 2])
                        for k in range(cnt - 2)]
            elif op == 0x90:
                out += [(idx[k], idx[k + 1], idx[k + 2]) for k in range(0, cnt, 3)]
            elif op == 0x80:
                for k in range(0, cnt, 4):
                    out += [(idx[k], idx[k + 1], idx[k + 2]), (idx[k], idx[k + 2], idx[k + 3])]
        p = e
    return np.array(out, int).reshape(-1, 3)


def inside(Q, regions):
    """How deep each point lies in the nearest region (0 at the edge, 1 at the middle; < 0 outside)."""
    best = np.full(len(Q), -1e9)
    for cx, cy, rx, ry, zmin in regions:
        r = np.sqrt(((Q[:, 0] - cx) / rx) ** 2 + ((Q[:, 1] - cy) / ry) ** 2)
        v = np.where(Q[:, 2] > zmin, 1.0 - r, -1e9)
        best = np.maximum(best, v)
    return best


def boundary(tris):
    """The positions on the open edges (an edge of one triangle only) of a triangle soup (n x 3 x 3)."""
    key = lambda q: tuple(np.round(q, 3))
    count = {}
    for t in tris:
        for i in range(3):
            e = tuple(sorted((key(t[i]), key(t[(i + 1) % 3]))))
            count[e] = count.get(e, 0) + 1
    pts = {q for e, n in count.items() if n == 1 for q in e}
    return np.array(sorted(pts), float).reshape(-1, 3)


def open_edges(tris):
    """The open edges (n x 2 x 3) of a triangle soup."""
    key = lambda q: tuple(np.round(q, 3))
    count = {}
    for t in tris:
        for i in range(3):
            e = tuple(sorted((key(t[i]), key(t[(i + 1) % 3]))))
            count[e] = count.get(e, 0) + 1
    return np.array([e for e, n in count.items() if n == 1], float).reshape(-1, 2, 3)


def onto_edges(Q, edges):
    """Each point's nearest point on these segments."""
    from scipy.spatial import cKDTree
    mids = edges.mean(1)
    _d, j = cKDTree(mids).query(Q, k=min(8, len(mids)))
    best = np.full((len(Q), 3), np.nan)
    bd = np.full(len(Q), np.inf)
    for c in range(j.shape[1]):
        e = edges[j[:, c]]
        ab = e[:, 1] - e[:, 0]
        t = np.clip(((Q - e[:, 0]) * ab).sum(1) / np.maximum((ab * ab).sum(1), 1e-9), 0, 1)
        p = e[:, 0] + ab * t[:, None]
        d = np.linalg.norm(p - Q, axis=1)
        better = d < bd
        best[better], bd[better] = p[better], d[better]
    return best


def bake(tris_pos, tris_uv, size, colour_of):
    """A texture of `size` for these triangles: colour_of(points) -> colours (n x 4) at each covered texel's place."""
    W, H_ = size
    pos = np.full((H_, W, 3), np.nan)
    lab = np.zeros((H_, W), np.int8)
    from skin_face_bake import raster, fill_holes
    raster(tris_pos, tris_uv, W, H_, pos, lab, 1)
    ok = lab > 0
    img = np.zeros((H_, W, 4))
    img[ok] = colour_of(pos[ok])
    img[..., 3] = 255
    return Image.fromarray(np.clip(fill_holes(img, ok), 0, 255).astype(np.uint8), "RGBA")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--event", required=True)
    ap.add_argument("--eye-r", default="21,12", help="the eyes' ellipses (x, y radii, model units)")
    ap.add_argument("--mouth-r", default="27,14", help="the mouth's ellipse (x, y radii)")
    ap.add_argument("--margin", type=float, default=0.3, help="the patch reaches this share beyond the hole")
    ap.add_argument("--parts", default="mouth", choices=("mouth", "eyes,mouth"),
                    help="what to take from the event head: the mouth (default; the eyes keep the skin's own lids "
                         "with a blink made for them, skin_shapes' way) or the eyes too (their blink stays weak: the "
                         "event closes them mostly with its lashes and its separate eye models)")
    ap.add_argument("--eye-forward", type=float, default=1.0, help="the event's lids this far ahead of the skin's")
    ap.add_argument("--inside", type=float, default=4.0, help="deeper than this behind the face: the mouth's inside")
    ap.add_argument("--tex", type=int, default=512, help="the patch's texture size")
    ap.add_argument("--preview", help="a folder for the patch's textures (PNG)")
    a = ap.parse_args(argv)
    from scipy.spatial import cKDTree
    arc = U.Archive(Path(a.skin).read_bytes(), ">")
    ents = [arc.entry(i) for i in range(len(arc.offsets))]
    head = ents[HEAD]
    imgs = G.tpl_images(ents[HEAD_TPL])
    gc, E, shapes = event_model(a.event)
    Sk = G.positions(head)
    _align, ls, _le = aligner(Sk, E, shapes)
    # the skin's parts: its face (the largest), its eyeballs (the part nearest the eyes), the rest
    parts = [(p, head[p + 0xC]) for p in G.parts(head) if not G.is_empty(head, p)]
    sizes = {m: len(U.gc_triangles(head, m)[0]) for _p, m in parts}
    face_mat = max(sizes, key=sizes.get)
    eye_mat = None
    for _p, m in parts:
        if m == face_mat:
            continue
        q = U.gc_triangles(head, m)[0].reshape(-1, 3)
        if abs(q[:, 1].mean() - ls["eyes"]) < 15 and q[:, 0].std() > 15:
            # the eyeballs lie behind their wet layer
            zm = lambda mm: U.gc_triangles(head, mm)[0].reshape(-1, 3)[:, 2].mean()
            eye_mat = m if eye_mat is None or zm(m) < zm(eye_mat) else eye_mat
    eyeq = U.gc_triangles(head, eye_mat)[0].reshape(-1, 3) if eye_mat is not None else None
    ex, ey = (28.0, ls["eyes"])
    if eyeq is not None:
        ex = float(np.abs(eyeq[:, 0]).mean())
        ey = float(eyeq[:, 1].mean())
    erx, ery = (float(v) for v in a.eye_r.split(","))
    mrx, mry = (float(v) for v in a.mouth_r.split(","))
    holes = [(ex, ey, erx, ery, 30.0), (-ex, ey, erx, ery, 30.0), (0.0, ls["mouth"], mrx, mry, 30.0)]
    with_eyes = a.parts != "mouth"
    if not with_eyes:
        holes = [holes[2], holes[2], holes[2]]
    grown = [(cx, cy, rx * (1 + a.margin), ry * (1 + a.margin), z) for cx, cy, rx, ry, z in holes]
    # the skin's face: its triangles outside the holes; the texels of its face texture as a colour source
    fp, fn, ft = U.gc_triangles(head, face_mat)
    holes_grown = [(cx, cy, rx * (1 + a.margin), ry * (1 + a.margin), z) for cx, cy, rx, ry, z in holes]
    # a face triangle stays unless all its corners lie in a grown hole: its edge covers the patch's (no gap)
    keep = (inside(fp.reshape(-1, 3), holes_grown) < 0).reshape(-1, 3).any(1)
    fh, fw, ff = struct.unpack_from(">HHI", imgs[face_mat][0])
    face_img = np.asarray(U.decode_gx(fh, fw, ff, imgs[face_mat][1]).convert("RGBA"), dtype=np.float64)
    fpos = F.uv_positions([head], face_mat, fw, fh)
    fok = np.isfinite(fpos[..., 0])
    ftree = cKDTree(fpos[fok])
    fcol = face_img[fok]
    # the event head (ev0001): texture 2 its head in one piece (the eyes' surfaces in it, the lids around them; the
    # blink, slot 9, moves only the lids) and three loose pieces in the mouth (the teeth above and below, the
    # tongue); texture 0 the lashes' cards (left out: their colour is in the alpha of a texture this tool has not)
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components

    def mid(q):
        return (q.min(0) + q.max(0)) / 2

    def lip_front(Q, y):
        sel = (np.abs(Q[:, 0]) < 4) & (np.abs(Q[:, 1] - y) < 4) & (Q[:, 2] > 40)
        return float(Q[sel, 2].max()) if sel.any() else 0.0
    etree = cKDTree(E)
    big = max((gc[p + 0xC] for p in G.parts(gc) if not G.is_empty(gc, p)),
              key=lambda m: len(U.gc_triangles(gc, m)[0]))
    P, N, T = U.gc_triangles(gc, big)
    vi = tri_indices(gc, big)
    r, c = np.r_[vi[:, 0], vi[:, 1], vi[:, 2]], np.r_[vi[:, 1], vi[:, 2], vi[:, 0]]
    _n, lab = connected_components(coo_matrix((np.ones(len(r)), (r, c)), shape=(len(E), len(E))), directed=False)
    tl = lab[vi[:, 0]]
    head_comp = np.bincount(tl).argmax()
    blink = shapes[9] if len(shapes) > 9 else {}
    lids = np.array(sorted(blink), int)
    lidq = E[lids] if len(lids) else np.zeros((0, 3))
    ecs = {}
    for side in (1, -1):
        q = lidq[lidq[:, 0] * side > 0]
        ecs[side] = (q.min(0) + q.max(0)) / 2 if len(q) else np.array([side * 30.0, _le["eyes"], 75.0])
    moves = {}
    for side in (1, -1):
        sk = mid(eyeq[eyeq[:, 0] * side > 0]) if eyeq is not None else np.array([side * ex, ey, ecs[side][2]])
        moves[side] = np.array([sk[0] - ecs[side][0], sk[1] - ecs[side][1], 0.0])
        # the depth: the event's eye opening as far forward as the skin's eyeballs' front
        if eyeq is not None:
            q = eyeq[eyeq[:, 0] * side > 0]
            moves[side][2] = float(q[:, 2].max()) - float(E[lids][E[lids][:, 0] * side > 0][:, 2].max()) + 2.0
    mouth_move = np.array([0.0, ls["mouth"] - _le["mouth"], lip_front(Sk, ls["mouth"]) - lip_front(E, _le["mouth"])])
    ecx = float(abs(ecs[1][0]))
    ecy = float((ecs[1][1] + ecs[-1][1]) / 2)
    ev_holes = [(ecs[1][0], ecs[1][1], erx, ery, 30.0), (ecs[-1][0], ecs[-1][1], erx, ery, 30.0),
                (0.0, _le["mouth"], mrx, mry, 30.0)]
    if not with_eyes:
        ev_holes = [ev_holes[2]] * 3
    ev_grown = [(cx, cy, rx * (1 + a.margin), ry * (1 + a.margin), z) for cx, cy, rx, ry, z in ev_holes]
    # each eye's patch fitted to the skin's eye opening: the event head's eye hole (its open edges near the eye) on
    # the skin face's (around its eyeballs) by their boxes (x, y scaled, the middles on each other) and their mean
    # depth; the event's shapes there scaled alike
    # (the event head's eye sockets are closed pockets: its openings measured by the lids that the blink moves more
    # than 3 units, about 1.15 times the opening's width; the skin's by its face's open edge around each eyeball)
    sk_open = boundary(fp)
    fits = {}
    for side in (1, -1):
        mv = np.array([v for v, dv in blink.items() if np.linalg.norm(dv) > 3 and E[v, 0] * side > 0], int)
        sc = mid(eyeq[eyeq[:, 0] * side > 0]) if eyeq is not None else np.array([side * ex, ey, 70.0])
        qs = sk_open[(np.abs(sk_open[:, 0] - sc[0]) < 22) & (np.abs(sk_open[:, 1] - sc[1]) < 14) &
                     (sk_open[:, 2] > 40)]
        if len(mv) < 6 or len(qs) < 6:
            fits[side] = (np.ones(3), ecs[side], ecs[side] + moves[side])
            continue
        qe = E[mv]
        ce = np.array([mid(qe)[0], ecs[side][1], qe[:, 2].mean()])
        cs = np.array([mid(qs)[0], mid(qs)[1], qs[:, 2].mean() + a.eye_forward])
        k = np.ptp(qs[:, 0]) / max(np.ptp(qe[:, 0]) / 1.15, 1e-3)
        fits[side] = (np.array([k, k, 1.0]), ce, cs)
    print("eyes: the event's openings scaled by %s / %s onto the skin's; mouth moved by %s" %
          (np.round(fits[1][0][:2], 2).tolist(), np.round(fits[-1][0][:2], 2).tolist(),
           np.round(mouth_move, 1).tolist()))

    def region_of(Q):
        """1 / -1 the eyes by side, 0 the mouth: the event hole a point lies deepest in (the upper lip reaches higher
        than half way to the nose)."""
        dm = inside(Q, [ev_grown[2]])
        de = inside(Q, ev_grown[:2])
        mouth = dm >= de
        return np.where(mouth, 0, np.where(Q[:, 0] >= 0, 1, -1))

    def place(Q):
        out = Q.copy()
        r = region_of(Q)
        for side in (1, -1):
            sel = r == side
            scale, ce, cs = fits[side]
            out[sel] = cs + (Q[sel] - ce) * scale
        out[r == 0] += mouth_move
        return out
    # the triangles taken: the head's within the grown ellipses, the mouth's loose pieces whole; their kinds: the
    # skin, the teeth, the mouth's inside
    cen = P.mean(1)
    take = (tl != head_comp) | (inside(cen, ev_grown) >= 0)
    kind = np.zeros(len(P), int)  # 0 skin, 2 teeth, 3 inside
    loose = sorted((int((tl == k).sum()), k) for k in np.unique(tl) if k != head_comp)
    for n_, k in loose:
        kind[tl == k] = 3 if k == loose[0][1] else 2  # the tongue the smallest, the teeth's rows the others
    P0, N0, T0, k0, v0 = P[take], N[take], T[take], kind[take], vi[take]
    A = place(P0.reshape(-1, 3))
    # the head's triangles in the mouth more than `inside` behind the skin's face (the lips' inner sides, the
    # mouth's walls): the mouth's inside
    dsk, _ = ftree.query(A.reshape(P0.shape).mean(1))
    cm = A.reshape(P0.shape).mean(1)
    k0[(k0 == 0) & (dsk > a.inside) & (np.abs(cm[:, 1] - ls["mouth"]) < mry * 0.5) & (np.abs(cm[:, 0]) < mrx * 0.6) &
       (N0.mean(1)[:, 2] < 0.0)] = 3  # (facing back: the walls; the chin's crease in front stays skin)
    reg = region_of(P0.reshape(-1, 3))
    shape_scale = np.ones((len(A), 3))
    for side in (1, -1):
        shape_scale[reg == side] = fits[side][0]
    # the patch's open edge onto the skin hole's edge (the skin cut by the grown ellipses, so nothing lies twice), the
    # move fading out over 6 units into the patch
    # (onto the face that stays, 0.4 units behind it: the face's border triangles cover the seam)
    stay = fpos[fok][inside(fpos[fok], holes_grown) < 0]
    pe = boundary(A.reshape(P0.shape)[tl[take] == head_comp])
    _d, jb = cKDTree(stay).query(pe)
    shift = stay[jb] - pe - np.array([0.0, 0.0, 0.4])
    bt = cKDTree(pe)
    dd, jj = bt.query(A, k=4)
    ww = 1.0 / (dd + 0.5) ** 2
    disp = (shift[jj] * ww[..., None]).sum(1) / ww.sum(1)[:, None]
    w = np.clip(1.0 - dd[:, 0] / 4.0, 0, 1) * np.repeat(tl[take] == head_comp, 3)
    A = A + disp * w[:, None]
    # the shapes: the event's own but within 1.5 units of the seam (there the face's carried ones, so the seam holds)
    ws = np.clip(1.0 - dd[:, 0] / 1.5, 0, 1) * np.repeat(tl[take] == head_comp, 3)
    patch_src = [(A, v0.reshape(-1), ws, shape_scale)]
    A = A.reshape(P0.shape)
    print("event patch: %d triangles: %d skin, %d eyes, %d teeth, %d the mouth's inside" %
          tuple([len(A)] + [int((k0 == q).sum()) for q in range(4)]))
    # their texture coordinates are their own: the skin a front projection of the patch's box into one new texture
    # (beside it a strip for the teeth and one for the mouth's inside), the eyes straight onto the skin's eye texture
    # by each point's offset from its eye's middle
    new_imgs = list(imgs)
    groups = []
    S_ = a.tex
    Tn = np.zeros(A.shape[:2] + (2,))
    sk = k0 == 0
    flat = A[sk].reshape(-1, 3)
    lo, hi = flat.min(0), flat.max(0)
    Tn[sk, :, 0] = (A[sk][..., 0] - lo[0]) / (hi[0] - lo[0]) * 0.8
    Tn[sk, :, 1] = 1.0 - (A[sk][..., 1] - lo[1]) / (hi[1] - lo[1])
    Tn[k0 == 2] = (0.85, 0.5)
    Tn[k0 == 3] = (0.95, 0.5)

    # the colour: the skin face's front layer seen from the front (its texels within 2 units of the frontmost at
    # their place on a 0.5 unit grid), looked up by x and y: a point of the patch a little behind the old surface
    # still takes the old surface's colour, not the mouth's inside behind it
    fp3 = fpos[fok]
    gx = np.round(fp3[:, 0] * 2).astype(int)
    gy = np.round(fp3[:, 1] * 2).astype(int)
    keyg = gx * 100000 + gy
    order = np.argsort(keyg)
    ks, zs = keyg[order], fp3[order, 2]
    starts = np.r_[0, np.where(np.diff(ks))[0] + 1]
    zmax = np.maximum.reduceat(zs, starts)
    front = np.zeros(len(fp3), bool)
    grp = np.repeat(np.arange(len(starts)), np.diff(np.r_[starts, len(ks)]))
    front[order] = zs >= zmax[grp] - 2.0
    fronttree = cKDTree(fp3[front][:, :2])
    frontcol = fcol[front]

    def colour_of(Q):
        dd, jj = fronttree.query(Q[:, :2], k=4)
        ww = 1.0 / np.maximum(dd, 0.05)
        return (frontcol[jj] * ww[..., None]).sum(1) / ww.sum(1)[:, None]
    img = bake(A[sk], Tn[sk], (S_, S_), colour_of)
    arr = np.asarray(img).copy()
    arr[:, int(0.8 * S_):int(0.9 * S_)] = (196, 186, 166, 255)  # the teeth
    arr[:, int(0.9 * S_):] = (104, 44, 44, 255)  # the tongue, the mouth's inside
    img = Image.fromarray(arr, "RGBA")
    if a.preview:
        Path(a.preview).mkdir(parents=True, exist_ok=True)
        img.save(Path(a.preview) / "patch.png")
    hdr = bytearray(imgs[face_mat][0])
    struct.pack_into(">HH", hdr, 0, S_, S_)
    new_imgs.append(G.encode_image(img, bytes(hdr)))
    notes = k0 != 1
    groups.append((len(new_imgs) - 1, A[notes], N0[notes], Tn[notes]))
    if eye_mat is not None and (k0 == 1).any():
        sel_e = k0 == 1
        Ae = A[sel_e]
        Te = np.zeros(Ae.shape[:2] + (2,))
        kk = 0.227 / 5.5
        for side in (1, -1):
            sel = Ae.mean(1)[:, 0] * side > 0
            q = Ae[sel].reshape(-1, 3)
            if not len(q):
                continue
            f = (q.min(0) + q.max(0)) / 2
            Te[sel, :, 0] = np.clip(0.5 + (Ae[sel][..., 0] - f[0]) * kk * side, 0.0, 1.0)
            Te[sel, :, 1] = np.clip(0.5 - (Ae[sel][..., 1] - f[1]) * kk, 0.0, 1.0)
        groups.append((eye_mat, Ae, N0[sel_e], Te))
    if len(new_imgs) > 255:
        raise SystemExit("too many textures")
    # the new head: the skin's face (without the holes), its other parts but the eyeballs and their wet layer, the
    # event's patch; each part header the face's, its texture the patch's own
    face_hdr = next(p for p, m in parts if m == face_mat)
    Ps, Ns, Ts, gl = [], [], [], []
    po = no = to = 0

    def add(hdr, Pc, Nc, Tc):
        nonlocal po, no, to
        Tc = Tc.copy()
        Tc[..., 1] = 1.0 - Tc[..., 1]
        P2, t2, N2, T2 = HF.corner_mesh(Pc, Nc, Tc)
        gl.append((hdr, t2 + np.array([po, no, to])))
        Ps.append(P2), Ns.append(N2), Ts.append(T2)
        po, no, to = po + len(P2), no + len(N2), to + len(T2)

    if os.environ.get("TRANSPLANT_NO_FACE"):  # (debugging: the patch alone)
        keep = keep & (fp.mean(1)[:, 2] < 0)
    add(face_hdr, fp[keep], fn[keep], ft[keep])
    wet = [m for _p, m in parts if m not in (face_mat, eye_mat) and
           abs(U.gc_triangles(head, m)[0].reshape(-1, 3)[:, 1].mean() - ey) < 15]
    # the skin's eyeballs and their wet layer stay (the event head has none: its eye sockets are closed pockets, the
    # event draws its eyes apart), set back so that the event's lids, closed (the blink, slot 9), lie 0.8 units in
    # front of each ball's front
    back = {1: 0.0, -1: 0.0}
    eye_pts = []
    if with_eyes and eyeq is not None and len(blink):
        closed = E.copy()
        for v, dv in blink.items():
            closed[v] = closed[v] + np.array(dv)
        closed = place(closed)
        for side in (1, -1):
            q = eyeq[eyeq[:, 0] * side > 0]
            c = mid(q)
            lidv = np.zeros(len(E), bool)
            lidv[np.unique(vi[tl == head_comp])] = True  # the head's own (not the lashes' nor loose ones)
            near = closed[lidv & (np.abs(closed[:, 0] - c[0]) < 4) & (np.abs(closed[:, 1] - c[1]) < 4) &
                          (closed[:, 2] > 40)]
            if len(near):
                back[side] = max(0.0, float(q[:, 2].max()) - (float(near[:, 2].max()) - 0.8))
        print("eyeballs set back by %.1f / %.1f units" % (back[1], back[-1]))
    for p, m in parts:
        if m == face_mat:
            continue
        Pc, Nc, Tc = U.gc_triangles(head, m)
        if m == eye_mat or m in wet:
            Pc = Pc.copy()
            side = np.where(Pc.mean(1)[:, 0] >= 0, 1, -1)
            Pc[..., 2] -= np.where(side > 0, back[1], back[-1])[:, None]
            eye_pts.append(Pc.reshape(-1, 3))
        add(p, Pc, Nc, Tc)
    for tex, A, N, T in groups:
        hdr = bytearray(head[face_hdr:face_hdr + 0x18])
        hdr[0xC] = tex
        add(bytes(hdr), A, N, T)
    P, N, T = np.concatenate(Ps), np.concatenate(Ns), np.concatenate(Ts)
    model = HF.build_model(head, P, N, T, gl)
    ne, step, bones = H.skin_weights(model, head, physics=0.0)
    # the cutscenes' shapes: the event's own on the patch (each patch position takes its event vertex's movement,
    # faded out over the margin), skin_shapes' carry for the rest of the face
    Pm = G.positions(bytes(model))
    carried, _ls2, _le2 = S.carry_shapes(Pm, E, shapes)
    pa = np.concatenate([q for q, _v, _w, _s in patch_src])
    pv = np.concatenate([v for _q, v, _w, _s in patch_src])
    pw = np.concatenate([w for _q, _v, w, _s in patch_src])
    ps = np.concatenate([sc for _q, _v, _w, sc in patch_src])
    d, j = cKDTree(pa).query(Pm)
    on_patch = np.where(d < 0.2)[0]
    for slot in range(len(carried)):
        if slot >= len(shapes):
            break
        sh = dict(carried[slot])
        ev = shapes[slot]
        for i in on_patch:
            k = j[i]
            own = np.array(ev.get(int(pv[k]), (0.0, 0.0, 0.0))) * ps[k]
            mix = own * (1 - pw[k]) + np.array(sh.get(int(i), (0.0, 0.0, 0.0))) * pw[k]
            if np.abs(mix).max() > 0.05:
                sh[int(i)] = tuple(mix)
            else:
                sh.pop(int(i), None)
        carried[slot] = sh
    # the skin's eyeballs and their wet layer never move with the face (the carry would pull them with the lids)
    balls = eye_pts
    if balls:
        db, _ = cKDTree(np.concatenate(balls)).query(Pm)
        still = set(np.where(db < 0.1)[0].tolist())
        carried = [{v: d for v, d in sh.items() if v not in still} for sh in carried]
    if not with_eyes and len(carried) > 9:
        own_blink, line = S.synth_blink(bytes(model))
        print(line)
        if own_blink:
            carried[9] = {v: dv for v, dv in own_blink.items() if max(abs(c) for c in dv) > 0.05}
    table, counts = S.shape_table(carried, model[0x28])
    print("shapes: %d positions on the event's patch take its own; moved per slot %s" %
          (len(on_patch), "/".join(str(c) for c in counts)))
    ents[HEAD] = S.with_shapes(bytes(model), table)
    ents[HEAD_TPL] = G.build_tpl(new_imgs)
    out = U.build_archive(arc, U.build_body(ents, arc.types))
    Path(a.out).write_bytes(out)
    print("transplant: face %d -> %d triangles (holes: eyes at x +-%.1f y %.1f, mouth at y %.1f); the event's patch %s; "
          "eyeballs (texture %s) and wet layers %s out; weights %d entries, bones %s; %s: %d bytes, data part %d" %
          (len(fp), int(keep.sum()), ex, ey, ls["mouth"],
           ["%d triangles (texture %d)" % (len(A_), t) for t, A_, _n, _t in groups], eye_mat, wet, ne, bones, a.out,
           len(out), U.Archive(out, ">").parts[0][1]))


if __name__ == "__main__":
    main()
