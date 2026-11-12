#!/usr/bin/env python3
"""Clothes from an FBX for a skin (wish of DevJunior, 12.11.2026: "the full outfit of Leon from Resident Evil Requiem";
an FBX exported with RE-Mesh-Editor, skinned to the RE Engine's skeleton, and the outfit's textures as TGA):

    skin_body_fbx.py SKIN.drs OUT.drs --fbx Leon.fbx --tex jacket=ALBD.tga[,ACOT.tga[,NRMR.tga]][@SIZE] ...
                     --mesh Set00_Jacket_Leather_Mat=jacket:4500 ... [--into 6] [--drop 0:3,6] [--preview DIR]

Every --mesh (the FBX's mesh by its material's name, "Group_0_Sub_1__Set00_Jacket_Leather_Mat") goes into the skin's
model --into (Leon: 6, pl000e, drawn in every costume and in the cutscenes instead of the event's jacket), drawn with
its --tex, its triangles reduced to the number given:

- Bones: each RE Engine bone counts as the first of itself and its ancestors that has a counterpart in Leon's skeleton
  (BASE: hip 17, spine 1 and 2, neck 3, head 4, clavicles 5/11, upper arms 7/13, forearms 8/14 and their outer half
  9/15, hands 10/16, legs 18..25); cloth bones (jacket tails, collar, sleeves) count as the body part they hang from.
- Pose: the FBX stands with its arms down, Leon's models are made in his T-pose. Each upper arm turns about its
  shoulder joint towards Leon's elbow (436 to the side), shortened along itself to that length; each forearm goes
  from Leon's elbow towards his wrist (704), its length scaled to his. A position follows its bones by their weights
  (linear blend), the rest of the body stays where skin_hair_fbx.place puts the FBX on the skin's head (the head
  from skin_head_fbx.py and the hair from skin_hair_fbx.py sit the same way).
- Fewer triangles: MeshLab's quadric edge collapse keeping the texture coordinates (pymeshlab, which needs
  libOpenGL.so.0), after welding the positions the RE Engine splits at every texture seam (else each texture island
  collapses into a fan), with the planar quadric and the texture coordinates weighted 4 (else triangles reach
  across a seam and draw the texture's dark gutter); the normals from the reduced mesh, the weights of each position
  from the nearest original one (its three strongest bones).
- Textures: the albedo (ALBD) shaded by the ACOT texture's occlusion (blue) and cavity (green) and lit from above by
  the normal map (NRMR), CMPR at SIZE (default 512), into the skin's palette of that model (Leon: entry 1): replacing
  the images only the parts --drop empties and the old model --into used (when no other model uses them), else added.
- --drop ENTRY:TEX,...: the parts of another model drawn with those textures are emptied (the old coat under the new
  jacket) and the model compacted.

Skins, FBX files and textures never go into the repository; this tool only reads and writes the paths given.
"""
import argparse
import collections
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_graft as G  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import skin_head_fbx as HF  # noqa: E402
import skin_slim as S  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

HEAD = 2
SEAMS = 4.0  # MeshLab: weight of the texture coordinates (keeps triangles from reaching across a texture seam)
QUALITY = 0.6  # MeshLab: penalty for thin triangles; with the planar quadric flat parts do not end up as fans
BASE = {"root": 0, "Hip": 17, "Spine_0": 1, "Spine_1": 2, "Spine_2": 2, "Neck_0": 3, "Neck_1": 3, "Head": 4}
for side, (cl, up, lo, lo2, hand, leg) in (("R", (5, 7, 8, 9, 10, 18)), ("L", (11, 13, 14, 15, 16, 22))):
    BASE.update({side + "_Arm_Clavicle": cl, side + "_Arm_Upper": up, side + "_Arm_Lower": lo, side + "_Arm_Hand": hand,
                 side + "_Arm_Lower_Twist_2": lo2, side + "_Arm_Lower_Twist_3": lo2,
                 side + "_Arm_Lower_Twist_Ext_2": lo2, side + "_Arm_Lower_Twist_Ext_3": lo2,
                 side + "_Leg_Upper": leg, side + "_Leg_Lower": leg + 1, side + "_Leg_Ankle": leg + 2,
                 side + "_Leg_Toe": leg + 3})
# Leon's T-pose (the disc's skeleton, skin_graft notes): elbows and wrists at the shoulders' height
LEON_Y, LEON_Z = 1507.8, -34.4
LEON_ELBOW, LEON_WRIST = 436.0, 704.0


# ---- the FBX's skeleton and skinning ---------------------------------------------------------------------------------


def rig(root):
    """(per mesh name: [(bone, control point indices, weights)], bone -> parent, bone -> bind position in game units
    before placing: the clusters' TransformLink, centimetres, y up, z forward)."""
    objs, con = root.first("Objects"), root.first("Connections")
    byid = {o.props[0]: o for o in objs.children if o.props}
    kids = collections.defaultdict(list)
    for c in con.children:
        kids[c.props[2]].append(c.props[1])
    name = lambda n: n.props[1].split("\x00")[0]
    parent = {}
    for c in con.children:
        a, b = byid.get(c.props[1]), byid.get(c.props[2])
        if a is not None and b is not None and a.name == "Model" and b.name == "Model":
            parent[name(a)] = name(b)
    skins, bind = {}, {}
    for g in objs.find("Geometry"):
        if len(g.props) < 3 or g.props[2] != "Mesh":
            continue
        out = []
        for sk in (byid[k] for k in kids[g.props[0]] if k in byid and byid[k].name == "Deformer"):
            for cl in (byid[k] for k in kids[sk.props[0]] if k in byid):
                bones = [name(byid[k]) for k in kids[cl.props[0]] if k in byid and byid[k].name == "Model"]
                idx, w, link = cl.first("Indexes"), cl.first("Weights"), cl.first("TransformLink")
                if not bones:
                    continue
                if link is not None and bones[0] not in bind:
                    bind[bones[0]] = np.array(link.props[0], dtype=np.float64).reshape(4, 4)[3, :3] * 10.0
                if idx is not None and w is not None:
                    out.append((bones[0], np.asarray(idx.props[0]), np.asarray(w.props[0], dtype=np.float64)))
        skins[name(g).split("__")[-1]] = out
    return skins, parent, bind


def leon_bone(bone, parent):
    """The bone of Leon's skeleton an RE Engine bone counts as (itself or its first ancestor in BASE; else the chest)."""
    b = bone
    while b is not None:
        if b in BASE:
            return BASE[b]
        b = parent.get(b)
    return 2


def leon_weights(n, clusters, parent):
    """Per control point (n) its weights over Leon's bones: dict bone -> share (summing to 1)."""
    acc = [collections.defaultdict(float) for _ in range(n)]
    for bone, idx, w in clusters:
        lb = leon_bone(bone, parent)
        for i, x in zip(idx, w):
            if x > 0:
                acc[i][lb] += float(x)
    out = []
    for a in acc:
        t = sum(a.values())
        out.append({b: v / t for b, v in a.items()} if t > 0 else {2: 1.0})
    return out


# ---- the pose --------------------------------------------------------------------------------------------------------


def turn(u, v):
    """The rotation taking unit vector u onto unit vector v (the shortest)."""
    u, v = u / np.linalg.norm(u), v / np.linalg.norm(v)
    c = float(u @ v)
    k = np.cross(u, v)
    s = np.linalg.norm(k)
    if s < 1e-9:
        return np.eye(3)
    k /= s
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + s * K + (1 - c) * (K @ K)


def segment(a, b, a2, b2):
    """(R, f(points)) taking the segment a..b onto a2..b2: turned about a, scaled along itself, moved to a2."""
    u = (b - a) / np.linalg.norm(b - a)
    k = np.linalg.norm(b2 - a2) / np.linalg.norm(b - a)
    R = turn(b - a, b2 - a2)

    def f(X):
        d = X - a
        along = d @ u
        return a2 + (d + np.outer(along * (k - 1.0), u)) @ R.T
    return R, f


def arm_moves(bind, s, t):
    """Leon's bone -> (rotation, function on points) for the arms (the rest stays), from the FBX's joints placed."""
    J = lambda n: bind[n] * s + t
    moves = {}
    for side, sign, (up, lo, lo2, hand) in (("R", -1.0, (7, 8, 9, 10)), ("L", 1.0, (13, 14, 15, 16))):
        sh, el, wr = J(side + "_Arm_Upper"), J(side + "_Arm_Lower"), J(side + "_Arm_Hand")
        el2 = np.array([sign * LEON_ELBOW, LEON_Y, LEON_Z])
        wr2 = np.array([sign * LEON_WRIST, LEON_Y, LEON_Z])
        moves[up] = segment(sh, el, sh, el2)
        low = segment(el, wr, el2, wr2)
        for b in (lo, lo2, hand):
            moves[b] = low
        print("pose: %s arm: shoulder (%.0f, %.0f, %.0f), elbow %.0f -> %.0f, wrist %.0f -> %.0f" % (
            side, *sh, el[0], el2[0], wr[0], wr2[0]))
    return moves


def repose(P, N, weights, moves):
    """Positions and normals moved by their bones' moves (linear blend; bones without a move stay)."""
    P2, N2 = np.zeros_like(P), np.zeros_like(N)
    for b, (R, f) in list(moves.items()) + [(None, (np.eye(3), lambda X: X))]:
        w = np.array([sum(v for k, v in wd.items() if (k == b if b is not None else k not in moves))
                      for wd in weights])
        if not w.any():
            continue
        P2 += w[:, None] * f(P)
        N2 += w[:, None] * (N @ R.T) if N is not None else 0
    return P2, N2


# ---- fewer triangles -------------------------------------------------------------------------------------------------


def reduce(P, tris, T, target):
    """(positions, normals, wedge uvs per corner, faces) of the mesh reduced to `target` triangles (MeshLab's quadric
    edge collapse with texture coordinates; boundaries kept)."""
    import pymeshlab
    # the RE Engine splits its meshes at every texture seam: welded by position first, else each texture island is
    # a piece of its own, its rim kept, and the inside collapses into a fan
    _, weld = np.unique(np.round(P / 0.02), axis=0, return_inverse=True)
    weld = weld.reshape(-1)
    first = np.full(weld.max() + 1, -1)
    for i in range(len(P) - 1, -1, -1):
        first[weld[i]] = i
    tris = tris.copy()
    tris[:, :, 0] = first[weld[tris[:, :, 0]]]
    ok = (tris[:, 0, 0] != tris[:, 1, 0]) & (tris[:, 1, 0] != tris[:, 2, 0]) & (tris[:, 0, 0] != tris[:, 2, 0])
    tris = tris[ok]
    used = np.unique(tris[:, :, 0])
    remap = np.full(len(P), -1)
    remap[used] = np.arange(len(used))
    F = remap[tris[:, :, 0]].astype(np.int32)
    W = T[tris[:, :, 2]].reshape(-1, 2).astype(np.float64)
    import tempfile
    ms = pymeshlab.MeshSet()
    with tempfile.TemporaryDirectory() as tmp:  # through an OBJ with a material: MeshLab's faces then have a texture
        tmp = Path(tmp)
        Image.new("RGB", (4, 4)).save(tmp / "t.png")
        (tmp / "m.mtl").write_text("newmtl m\nmap_Kd t.png\n")
        lines = ["mtllib m.mtl", "usemtl m"]
        lines += ["v %.6f %.6f %.6f" % tuple(v) for v in P[used]]
        lines += ["vt %.7f %.7f" % tuple(w) for w in W]
        lines += ["f %d/%d %d/%d %d/%d" % (f[0] + 1, 3 * i + 1, f[1] + 1, 3 * i + 2, f[2] + 1, 3 * i + 3)
                  for i, f in enumerate(F)]
        (tmp / "m.obj").write_text("\n".join(lines) + "\n")
        ms.load_new_mesh(str(tmp / "m.obj"))
    if target < len(F):
        ms.meshing_decimation_quadric_edge_collapse_with_texture(targetfacenum=int(target), preserveboundary=True,
                                                                 boundaryweight=2.0, optimalplacement=True,
                                                                 preservenormal=True, qualitythr=QUALITY, extratcoordw=SEAMS,
                                                                 planarquadric=True)
    ms.compute_normal_per_vertex()
    m = ms.current_mesh()
    V, Fo = m.vertex_matrix(), m.face_matrix()
    return V, m.vertex_normal_matrix(), m.wedge_tex_coord_matrix().reshape(-1, 3, 2), Fo, P[used], used


# ---- textures --------------------------------------------------------------------------------------------------------


def outfit_texture(spec, relief=0.3, occlusion=0.5, cavity=0.25):
    """ALBD[,ACOT[,NRMR]][@SIZE] -> RGBA image of SIZE (default 512): the albedo shaded (see the module text)."""
    paths, _, size = spec.partition("@")
    size = int(size or 512)
    paths = paths.split(",")
    load = lambda p: np.asarray(Image.open(p).convert("RGBA").resize((size, size), Image.LANCZOS), dtype=np.float64)
    img = load(paths[0])
    if len(paths) > 1 and paths[1]:
        ac = load(paths[1]) / 255.0
        img[..., :3] *= ((1 - occlusion + occlusion * ac[..., 2]) * (1 - cavity + cavity * ac[..., 1]))[..., None]
    if len(paths) > 2 and paths[2]:
        nm = load(paths[2]) / 127.5 - 1.0
        img[..., :3] *= np.clip(1.0 + relief * nm[..., 1] * 0.6, 0.6, 1.4)[..., None]
    img[..., 3] = 255
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")


def darken(img, gain, keep=70.0):
    """The texture times `gain` (DevJunior, 12.11.2026: "the jacket is rather black, not grey"; the RE Engine's
    albedo comes out lighter in the game's light), its bright details (metal, buttons: brightness above `keep`,
    faded in over 40) kept."""
    x = np.asarray(img, dtype=np.float64)
    lum = x[..., :3] @ np.array([0.3, 0.59, 0.11])
    w = np.clip((lum - keep) / 40.0, 0, 1)
    x[..., :3] *= (gain * (1 - w) + w)[..., None]
    return Image.fromarray(np.clip(x, 0, 255).astype(np.uint8), "RGBA")


def users(entries, tpl_users):
    """Palette image -> the (entry, part) pairs drawing with it, over the models of that palette."""
    out = collections.defaultdict(list)
    for e in tpl_users:
        b = entries[e]
        if not b:
            continue
        for p in G.parts(b):
            if not G.is_empty(b, p):
                out[b[p + 0xC]].append(e)
    return out


def model_corners(blob):
    """Per non-empty part of a GameCube model: (header offset, positions, normals, texture coordinates (v up, as the
    FBX's), weights) per corner (n x 3 x ...), the weights as dicts bone -> share."""
    u = lambda o: struct.unpack_from(">I", blob, o)[0]
    flags = u(0x20)
    clr, s8 = bool(flags & 0x80000000), bool(flags & 0x20000000)
    nv, nn = struct.unpack_from(">HH", blob, 0x38)
    P = G.positions(blob)
    if s8:
        Nm = np.frombuffer(blob, ">i1", nn * 4, u(0x34)).reshape(-1, 4)[:, :3] / 64.0
    else:
        Nm = np.frombuffer(blob, ">i2", nn * 4, u(0x34)).reshape(-1, 4)[:, :3] / 16384.0
    sc = 256.0 if clr else 32768.0
    fmt = ">hh" if clr else ">HH"
    W = G.model_weights(blob)
    vo = u(0x30)
    pw = [W[struct.unpack_from(">H", blob, vo + i * 8 + 6)[0]] for i in range(nv)]
    out = []
    for p, tris in S.part_lists(blob):
        if not tris:
            continue
        r = np.array(tris)  # n x 3 x records
        uv = np.array([[struct.unpack_from(fmt, blob, u(0x10) + c[-1] * 4) for c in t] for t in tris]) / sc
        uv[..., 1] = 1.0 - uv[..., 1]
        w = [{b: x / 100.0 for b, x in pw[c[0]]} or {2: 1.0} for t in tris for c in t]
        out.append((p, P[r[:, :, 0]], Nm[r[:, :, 1]], uv, w))
    return out


def extend_neck(head, reach, band=12.0, sectors=24, axis=(0.0, -30.0)):
    """The head's neck longer by `reach` at its lower edge: per direction around the neck (`sectors`), the positions
    within `band` of the lowest go down, the lowest by `reach`, fading to 0 at `band` above it (a shirt with a lower
    collar, the V of Resident Evil Requiem's, would show the gap under the neck). Returns (model, how many)."""
    P = G.positions(head)
    ang = ((np.arctan2(P[:, 0] - axis[0], P[:, 2] - axis[1]) + np.pi) / (2 * np.pi) * sectors).astype(int) % sectors
    near = np.hypot(P[:, 0] - axis[0], P[:, 2] - axis[1]) < 120  # the neck, not the hair hanging over the shoulders
    low = np.full(sectors, np.inf)
    np.minimum.at(low, ang[near], P[near, 1])
    out = bytearray(head)
    n = 0
    for i in np.nonzero(near)[0]:
        f = 1.0 - (P[i, 1] - low[ang[i]]) / band
        if f > 0:
            G.set_position(out, i, P[i] - np.array([0.0, reach * f, 0.0]))
            n += 1
    return bytes(out), n


def sample(img, uv):
    """The image's colours (n x 3) at texture coordinates (v up)."""
    x = np.asarray(img.convert("RGB"), dtype=np.float64)
    h, w = x.shape[:2]
    i = np.clip(((1.0 - uv[:, 1]) % 1.0 * h).astype(int), 0, h - 1)
    j = np.clip((uv[:, 0] % 1.0 * w).astype(int), 0, w - 1)
    return x[i, j]


def match_colour(img, have, want):
    """img (RGBA) moved per channel from the colours `have` (mean and spread) to `want`'s (spread ratio 0.5..2)."""
    x = np.asarray(img.convert("RGBA"), dtype=np.float64).copy()
    for c in range(3):
        k = np.clip(want[:, c].std() / max(have[:, c].std(), 1e-3), 0.5, 2.0)
        x[..., c] = (x[..., c] - have[:, c].mean()) * k + want[:, c].mean()
    return Image.fromarray(np.clip(x, 0, 255).astype(np.uint8), "RGBA")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--fbx", required=True)
    ap.add_argument("--tex", action="append", default=[], metavar="KEY=ALBD[,ACOT[,NRMR]][@SIZE]")
    ap.add_argument("--mesh", action="append", default=[], metavar="MATERIAL=KEY:TRIANGLES")
    ap.add_argument("--into", type=int, default=6, help="the model the clothes become (Leon: 6, pl000e)")
    ap.add_argument("--tpl", type=int, default=1, help="its palette (Leon: 1, for models 0, 6, 9, 12)")
    ap.add_argument("--tpl-users", default="0,6,9,12", help="the models drawing with that palette")
    ap.add_argument("--drop", action="append", default=[], metavar="ENTRY:TEX,...")
    ap.add_argument("--gain", action="append", default=[], metavar="KEY=FACTOR[,KEEP]",
                    help="a texture darker (bright details above brightness KEEP, default 70, kept)")
    ap.add_argument("--merge", action="store_true", help="keep the old model --into's triangles (see --cover)")
    ap.add_argument("--cover", type=float, default=0.0,
                    help="with --merge: an old triangle whose middle lies within this of a new position goes")
    ap.add_argument("--replace-mats", default="", help="with --merge: of the old parts with these textures ...")
    ap.add_argument("--replace-x", type=float, default=0.0,
                    help="... the triangles nearer the middle than this (|x|) go: the sleeves stay")
    ap.add_argument("--match-colour", action="append", default=[], metavar="IMAGE=KEY",
                    help="with --merge: the palette image IMAGE (an old one) to the colours of texture KEY")
    ap.add_argument("--neck", type=float, default=0.0, help="the head's neck longer by this at its lower edge")
    ap.add_argument("--inside", action="append", default=[], metavar="ENTRY:COVER",
                    help="positions of ENTRY poking out through COVER pushed back under it (skin_slim.inside, from y 950)")
    ap.add_argument("--relief", type=float, default=0.3)
    ap.add_argument("--occlusion", type=float, default=0.5)
    ap.add_argument("--cavity", type=float, default=0.25)
    a = ap.parse_args(argv)

    arc = U.Archive(open(a.skin, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    root = H.load_fbx(a.fbx)
    fbx = H.meshes(root)
    skins, parent, bind = rig(root)
    s, t = H.place(fbx, entries[HEAD])
    print("FBX on the skin's head: scale %.3f, shift (%.1f, %.1f, %.1f)" % (s, *t))
    moves = arm_moves(bind, s, t)
    tpl_users = [int(x) for x in a.tpl_users.split(",")]
    imgs = G.tpl_images(entries[a.tpl])
    # the parts emptied, the palette images nobody else draws with any more
    old_users = users(entries, tpl_users)
    freed = set()
    for spec in a.drop:
        e, _, mats = spec.partition(":")
        e, mats = int(e), {int(x) for x in mats.split(",")}
        blob = bytearray(entries[e])
        n = G.empty_parts(blob, lambda m: m in mats)
        kept = [tris for _, tris in S.part_lists(bytes(blob))]
        entries[e] = S.rebuild(bytes(blob), kept)
        print("drop: entry %d, %d parts with textures %s emptied, %d positions left" % (
            e, n, sorted(mats), struct.unpack_from(">H", entries[e], 0x38)[0]))
        freed |= {m for m in mats if set(old_users[m]) <= {e}}
    template = entries[a.into]
    for m, us in old_users.items():
        if set(us) <= {a.into} and not a.merge:
            freed.add(m)
    head_part = next(p for p in G.parts(template) if not G.is_empty(template, p))
    # the textures
    tex_index, tex_imgs, WT_by_key = {}, {}, collections.defaultdict(list)
    gains = {}
    for spec in a.gain:
        key, _, rest = spec.partition("=")
        v = [float(x) for x in rest.split(",")]
        gains[key] = (v[0], v[1] if len(v) > 1 else 70.0)
    free = sorted(freed)
    for spec in a.tex:
        key, _, rest = spec.partition("=")
        img = outfit_texture(rest, a.relief, a.occlusion, a.cavity)
        if key in gains:
            img = darken(img, *gains[key])
            print("texture %s: times %.2f (above brightness %.0f kept)" % (key, *gains[key]))
        hdr = bytearray(imgs[free[0] if free else 0][0])  # the replaced image's header (size and format set anew)
        if free:
            k = free.pop(0)
            imgs[k] = G.encode_image(img, bytes(hdr))
        else:
            k = len(imgs)
            imgs.append(G.encode_image(img, bytes(hdr)))
        tex_index[key] = k
        tex_imgs[key] = img
        print("texture %s: palette %d image %d, %dx%d" % (key, a.tpl, k, img.size[0], img.size[1]))
    for k in free:  # images nobody draws with any more: an 8x8 stand-in (one CMPR tile)
        hdr = bytearray(imgs[k][0])
        imgs[k] = G.encode_image(Image.new("RGBA", (8, 8), (0, 0, 0, 255)), bytes(hdr))
    entries[a.tpl] = G.build_tpl(imgs)
    # the meshes
    groups = collections.OrderedDict()
    Ps, Ns, Ts, Ws = [], [], [], []
    po = 0
    for spec in a.mesh:
        mat, _, rest = spec.partition("=")
        key, _, target = rest.partition(":")
        P, tris, N, T = fbx[mat]
        weights = leon_weights(len(P), skins[mat], parent)
        Pp, _ = repose(P * s + t, np.zeros_like(P), weights, moves)
        V, VN, WT, F, Porig, used = reduce(Pp, tris, T, int(target or 10 ** 9))
        from scipy.spatial import cKDTree
        _, near = cKDTree(Porig).query(V)
        wv = [weights[used[j]] for j in near]
        n = len(V)
        corner = np.arange(len(F) * 3).reshape(-1, 3)
        trip = np.stack([F + po, F + po, corner + sum(len(x) for x in Ts)], 2)
        groups.setdefault(key, []).append(trip)
        WT_by_key[key].append(WT.reshape(-1, 2))
        Ps.append(V), Ns.append(VN), Ts.append(WT.reshape(-1, 2)), Ws.extend(wv)
        po += n
        print("mesh %s: %d -> %d triangles, %d positions, texture %s" % (mat, len(tris), len(F), n, key))
    extra = []
    if a.merge:  # the old model's triangles stay, but those the new meshes cover (--cover) go
        from scipy.spatial import cKDTree
        newP = np.concatenate(Ps)
        tree = cKDTree(newP)
        kept_old = 0
        replace_mats = {int(x) for x in a.replace_mats.split(",") if x}
        for p, Pc, Nc, UVc, wc in model_corners(template):
            cen = Pc.mean(1)
            d, _ = tree.query(cen)
            keep = d > a.cover if a.cover > 0 else np.ones(len(cen), bool)
            if template[p + 0xC] in replace_mats:  # the torso of the old shirt: the new one replaces it
                keep &= np.abs(cen[:, 0]) >= a.replace_x
            if not keep.any():
                continue
            n = int(keep.sum())
            base_p, base_t = sum(len(x) for x in Ps), sum(len(x) for x in Ts)
            Ps.append(Pc[keep].reshape(-1, 3)), Ns.append(Nc[keep].reshape(-1, 3)), Ts.append(UVc[keep].reshape(-1, 2))
            Ws.extend(w for k, w in zip(np.repeat(keep, 3), wc) if k)
            idx = np.arange(n * 3).reshape(-1, 3)
            extra.append((template[p:p + 0x18], np.stack([idx + base_p, idx + base_p, idx + base_t], 2), p))
            kept_old += n
            print("merge: old part with texture %d keeps %d of %d triangles" % (template[p + 0xC], n, len(cen)))
        # the old images matched to the new textures' colours (--match-colour IMAGE=KEY)
        for spec in a.match_colour:
            im, _, key = spec.partition("=")
            im = int(im)
            want = np.concatenate([sample(tex_imgs[key], WT_by_key[key][k]) for k in range(len(WT_by_key[key]))])
            olduv = np.concatenate([UVc.reshape(-1, 2) for p, _, _, UVc, _ in model_corners(template)
                                    if template[p + 0xC] == im])
            cur = U.decode_gx(*struct.unpack_from(">HHI", imgs[im][0]), imgs[im][1]).convert("RGBA")
            have = sample(cur, olduv)
            imgs[im] = G.encode_image(match_colour(cur, have, want), imgs[im][0])
            print("colour: image %d (mean %s) to %s's (mean %s)" % (im, have.mean(0).round().tolist(), key,
                                                                    want.mean(0).round().tolist()))
        entries[a.tpl] = G.build_tpl(imgs)
    P, N, T = np.concatenate(Ps), np.concatenate(Ns), np.concatenate(Ts)
    parts = []
    for key, tl in groups.items():
        hdr = bytearray(template[head_part:head_part + 0x18])
        hdr[0xC] = tex_index[key]
        hdr[0xB] &= ~4 & 0xFF  # no alpha texture
        parts.append((bytes(hdr), np.concatenate(tl)))
    parts += [(hdr, tl) for hdr, tl, _ in extra]
    model = HF.build_model(template, P, N, T, parts)
    per = []
    for wd in Ws:
        top = sorted(wd.items(), key=lambda x: -x[1])[:3]
        tot = sum(v for _, v in top)
        per.append(tuple((b, 100.0 * v / tot) for b, v in top))
    # build_model reorders positions (unique, in the order the parts use them): weights by position
    order = model_positions_order(P, model)
    ne, step, bones = H.write_weights(model, [per[i] for i in order])
    print("model %d: %d positions, %d normals, %d triangles; %d weight entries (%d%% steps), bones %s" % (
        a.into, struct.unpack_from(">H", model, 0x38)[0], struct.unpack_from(">H", model, 0x3A)[0],
        sum(len(tl) for _, tl in parts), ne, step, bones))
    entries[a.into] = bytes(model)
    if a.neck:
        entries[HEAD], n = extend_neck(entries[HEAD], a.neck)
        print("neck: %d positions of the head's lower edge down by up to %.0f" % (n, a.neck))
    for spec in a.inside:
        e, c = (int(x) for x in spec.split(":"))
        entries[e], n = S.inside(entries[e], entries[c], low=950.0)
        print("inside: %d positions of entry %d pushed back under entry %d" % (n, e, c))
    body = U.build_body(entries, arc.types)
    data = U.build_archive(arc, body)
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's place holds %d)" % (a.out, len(data), len(body), 0x600000))
    if len(body) > 0x600000:
        raise SystemExit("too large: fewer triangles or smaller textures")


def model_positions_order(P, model):
    """Per position of the built model the index into P it came from (build_model quantises and merges equal ones)."""
    from scipy.spatial import cKDTree
    Q = G.positions(model)
    _, j = cKDTree(P).query(Q)
    return j


if __name__ == "__main__":
    main()
