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


def inside(Q, regions):
    """How deep each point lies in the nearest region (0 at the edge, 1 at the middle; < 0 outside)."""
    best = np.full(len(Q), -1e9)
    for cx, cy, rx, ry, zmin in regions:
        r = np.sqrt(((Q[:, 0] - cx) / rx) ** 2 + ((Q[:, 1] - cy) / ry) ** 2)
        v = np.where(Q[:, 2] > zmin, 1.0 - r, -1e9)
        best = np.maximum(best, v)
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
    grown = [(cx, cy, rx * (1 + a.margin), ry * (1 + a.margin), z) for cx, cy, rx, ry, z in holes]
    # the skin's face: its triangles outside the holes; the texels of its face texture as a colour source
    fp, fn, ft = U.gc_triangles(head, face_mat)
    keep = inside(fp.mean(1), holes) < 0
    fh, fw, ff = struct.unpack_from(">HHI", imgs[face_mat][0])
    face_img = np.asarray(U.decode_gx(fh, fw, ff, imgs[face_mat][1]).convert("RGBA"), dtype=np.float64)
    fpos = F.uv_positions([head], face_mat, fw, fh)
    fok = np.isfinite(fpos[..., 0])
    ftree = cKDTree(fpos[fok])
    fcol = face_img[fok]
    # the event's parts, each region moved as a whole onto the skin's (its shapes stay the event's own): each eye by
    # its eyeball's middle, the mouth by the lips' parting and the lips' front
    egc = {gc[p + 0xC]: U.gc_triangles(gc, gc[p + 0xC]) for p in G.parts(gc) if not G.is_empty(gc, p)}
    ball_m = min(egc, key=lambda m: len(egc[m][0]))
    eb = egc[ball_m][0].reshape(-1, 3)

    def mid(q):
        return (q.min(0) + q.max(0)) / 2

    def lip_front(Q, y):
        sel = (np.abs(Q[:, 0]) < 4) & (np.abs(Q[:, 1] - y) < 4) & (Q[:, 2] > 40)
        return float(Q[sel, 2].max()) if sel.any() else 0.0
    moves = {}
    for side in (1, -1):
        se = mid(eb[eb[:, 0] * side > 0])
        sk = mid(eyeq[eyeq[:, 0] * side > 0]) if eyeq is not None else np.array([side * ex, ey, se[2]])
        moves[side] = sk - se
    mouth_move = np.array([0.0, ls["mouth"] - _le["mouth"], lip_front(Sk, ls["mouth"]) - lip_front(E, _le["mouth"])])
    ecx = float(np.abs(mid(eb[eb[:, 0] > 0])[0]))
    ecy = float(mid(eb)[1])
    ev_holes = [(ecx, ecy, erx, ery, 30.0), (-ecx, ecy, erx, ery, 30.0), (0.0, _le["mouth"], mrx, mry, 30.0)]
    ev_grown = [(cx, cy, rx * (1 + a.margin), ry * (1 + a.margin), z) for cx, cy, rx, ry, z in ev_holes]
    print("moved: eyes by %s / %s, mouth by %s" % (np.round(moves[1], 1).tolist(), np.round(moves[-1], 1).tolist(),
                                                   np.round(mouth_move, 1).tolist()))

    def place(Q):
        """The event's points (n x 3) moved by their region's move (the eyes by side, the mouth below the nose)."""
        out = Q.copy()
        mouth = Q[:, 1] < (_le["mouth"] + _le["nose"]) / 2
        for side in (1, -1):
            sel = ~mouth & (Q[:, 0] * side >= 0)
            out[sel] += moves[side]
        out[mouth] += mouth_move
        return out
    groups_src = []
    patch_src = []  # (placed point, event vertex index, snap weight)
    etree = cKDTree(E)
    for m, (P, N, T) in egc.items():
        is_ball = m == ball_m
        depth = inside(P.mean(1), ev_grown)
        take = np.ones(len(P), bool) if is_ball else depth >= 0
        if not take.any():
            continue
        P0 = P[take]
        A = place(P0.reshape(-1, 3))
        w = np.zeros(len(A))
        if not is_ball:
            # the patch's edge onto the skin's surface, fading in over the margin
            d = inside(P0.reshape(-1, 3), ev_holes)  # < 0 in the margin
            w = np.clip(-d / max(a.margin, 1e-3), 0, 1)
            _dist, j = ftree.query(A)
            A = A + (fpos[fok][j] - A) * w[:, None]
        _d, vi = etree.query(P0.reshape(-1, 3))
        patch_src.append((A, vi, w))
        groups_src.append((m, is_ball, A.reshape(P0.shape), N[take], T[take]))
    # the patch's texture coordinates are its own (the event model's are not usable as converted): the skin patch
    # a front projection of its box into one new texture (the mouth's inside, more than `inside` behind the face, in
    # a strip of its own: dark, the teeth light), the eyeballs straight onto the skin's eye texture by each point's
    # offset from its ball's front
    new_imgs = list(imgs)
    groups = []
    skin = [(A, N) for m, is_ball, A, N, T in groups_src if not is_ball]
    balls = [(A, N) for m, is_ball, A, N, T in groups_src if is_ball]
    if skin:
        A = np.concatenate([q for q, _n in skin])
        N = np.concatenate([n for _q, n in skin])
        flat = A.reshape(-1, 3)
        ds, _ = ftree.query(A.mean(1))
        deep = (ds > a.inside) & (np.abs(A.mean(1)[:, 1] - ls["mouth"]) < mry * 1.3) & (np.abs(A.mean(1)[:, 0]) < mrx)
        lo, hi = flat.min(0), flat.max(0)
        T = np.zeros(A.shape[:2] + (2,))
        T[..., 0] = (A[..., 0] - lo[0]) / (hi[0] - lo[0]) * 0.8
        T[..., 1] = 1.0 - (A[..., 1] - lo[1]) / (hi[1] - lo[1])
        teeth = deep & (A.mean(1)[:, 1] > ls["mouth"] - 1.0) & (ds < a.inside + 5.0)
        T[deep, :, 0] = np.where(teeth[deep, None], 0.85, 0.95)
        T[deep, :, 1] = 0.5
        S_ = a.tex

        def colour_of(Q):
            d, j = ftree.query(Q, k=4)
            w = 1.0 / np.maximum(d, 0.05)
            return (fcol[j] * w[..., None]).sum(1) / w.sum(1)[:, None]
        img = bake(A[~deep], T[~deep], (S_, S_), colour_of)
        arr = np.asarray(img).copy()
        arr[:, int(0.8 * S_):int(0.9 * S_)] = (205, 198, 182, 255)  # the teeth
        arr[:, int(0.9 * S_):] = (62, 26, 26, 255)  # the mouth's inside
        img = Image.fromarray(arr, "RGBA")
        if a.preview:
            Path(a.preview).mkdir(parents=True, exist_ok=True)
            img.save(Path(a.preview) / "patch.png")
        hdr = bytearray(imgs[face_mat][0])
        struct.pack_into(">HH", hdr, 0, S_, S_)
        new_imgs.append(G.encode_image(img, bytes(hdr)))
        groups.append((len(new_imgs) - 1, A, N, T))
        print("patch: %d triangles, %d of them the mouth's inside (%d teeth)" % (len(A), int(deep.sum()),
                                                                                int(teeth.sum())))
    if balls and eye_mat is not None:
        A = np.concatenate([q for q, _n in balls])
        N = np.concatenate([n for _q, n in balls])
        T = np.zeros(A.shape[:2] + (2,))
        k = 0.227 / 5.5
        for side in (1, -1):
            sel = A.mean(1)[:, 0] * side > 0
            q = A[sel].reshape(-1, 3)
            f = q[np.argmax(q[:, 2])]
            T[sel, :, 0] = np.clip(0.5 + (A[sel][..., 0] - f[0]) * k * side, 0.0, 1.0)
            T[sel, :, 1] = np.clip(0.5 - (A[sel][..., 1] - f[1]) * k, 0.0, 1.0)
        groups.append((eye_mat, A, N, T))
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

    add(face_hdr, fp[keep], fn[keep], ft[keep])
    wet = [m for _p, m in parts if m not in (face_mat, eye_mat) and
           abs(U.gc_triangles(head, m)[0].reshape(-1, 3)[:, 1].mean() - ey) < 15]
    for p, m in parts:
        if m in (face_mat, eye_mat) or m in wet:
            continue
        Pc, Nc, Tc = U.gc_triangles(head, m)
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
    pa = np.concatenate([q for q, _v, _w in patch_src])
    pv = np.concatenate([v for _q, v, _w in patch_src])
    pw = np.concatenate([w for _q, _v, w in patch_src])
    d, j = cKDTree(pa).query(Pm)
    on_patch = np.where(d < 0.2)[0]
    for slot in range(len(carried)):
        if slot >= len(shapes):
            break
        sh = dict(carried[slot])
        ev = shapes[slot]
        for i in on_patch:
            k = j[i]
            own = np.array(ev.get(int(pv[k]), (0.0, 0.0, 0.0)))
            mix = own * (1 - pw[k]) + np.array(sh.get(int(i), (0.0, 0.0, 0.0))) * pw[k]
            if np.abs(mix).max() > 0.05:
                sh[int(i)] = tuple(mix)
            else:
                sh.pop(int(i), None)
        carried[slot] = sh
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
