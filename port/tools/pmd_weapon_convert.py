#!/usr/bin/env python3
"""A weapon of the PC release (RE4 2007 / UHD .pmd with its .tga textures) into the disc's weapon archive (wish of
DevJunior, 12.11.2026: "replace the weapons the pack is made for"):

    pmd_weapon_convert.py DISC_WEPxx.drs OUT.drs --pmd ENTRY=FILE.pmd [--pmd ...] --tex DIR
                          [--tris N] [--size PX] [--budget BYTES] [--preview DIR]

The PC models are the disc's in another form: the same skeleton (bones "bornNN__KK": the disc model's part KK, at the
same place), the units a hundredth (x 100 gives the disc's), the same axes. So each mapped entry (a BIN of the
archive: the weapon's model, a variant per ENTRY, e.g. the pistols' 2 plain and 3 with the silencer) is built anew
(skin_head_fbx.build_model with the disc model as template: its header, parts records, colour table and x-flip
table):

- Geometry: every triangle of the PMD, its positions x 100; each position rigidly on the part its vertex names
  (the PC weapons have one bone per vertex; "Scene_Root" is part 0). Where the archive would not fit the weapon's
  place (ReadWepData, read.cpp: WEAPON_DATA_MAX 0x70000 bytes for the whole data part), each piece (part x texture)
  is reduced (skin_body_fbx.reduce, MeshLab's quadric collapse with texture coordinates) in proportion; the normals
  stay the PMD's (each new corner takes the normal of the nearest original corner facing the same way: hard edges
  stay hard). The winding follows the disc model's (the side its normals face).
- Drawing: one part per texture, with the header of the disc model's largest textured part (its specular and
  environment settings) and that texture's index.
- Textures: the disc's palette stays as it is (the weapon's other models, the muzzle flash, the second texture an
  environment-mapped part names); the PMD's textures are added behind it, CMPR with mipmaps down to 8 pixels, at most
  --size a side (default 256), opaque. The model's texture count (header 0x24, a u32) is set to the palette's.

The sizes go down until the data part fits --budget (default 0x70000 - 0x1000): first the triangles (--tris, default
4000 per model, then 20 % less per round), the textures halved from the largest once a model is under 1500.

Everything else in the archive (the motions, the effects, the module, the sound) stays. Weapon packs and disc files
never go into the repository; this tool only reads and writes the paths given. --preview DIR draws each new model
(textured, from the built archive) next to the disc's. Check the result with the core's own converters:
re4_format_compare --data DIR (DIR/disc0/files/em/wepNN.drs): every archive must be "OK" (the first device test,
12.11.2026, drew no weapon: the texture count was written into the u32's high byte, and the x-flip table's offset
pointed into the new normals; both found by that check).
"""
import argparse
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pmd_preview as PMD  # noqa: E402
import skin_body_fbx as B  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import skin_head_fbx as HF  # noqa: E402
import skin_slim as S  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

LIMIT = 0x70000
SCALE = 100.0


# ---- the PMD ---------------------------------------------------------------------------------------------------------

def pmd_pieces(path):
    """The PMD's triangles grouped by (disc part, texture name): {key: (P n x 3 x 3, N n x 3 x 3, T n x 3 x 2)} in the
    disc's units, T with v up."""
    m = PMD.read(path)
    part_of = {}
    for k, name in enumerate(m['bones']):
        mm = re.search(r'__(\d+)$', name or '')
        part_of[k] = int(mm.group(1)) if mm else 0
    acc = {}
    for node in m['nodes']:
        for tex, idx, vs in node['meshes']:
            name = m['mats'][tex][0] if 0 <= tex < len(m['mats']) else ''
            V = np.array(vs, dtype=np.float64)
            t = np.array(idx[:len(idx) // 3 * 3]).reshape(-1, 3)
            if not len(t):
                continue
            k = np.clip(np.round(V[:, 5] / 3).astype(int), 0, max(0, len(node['bones']) - 1))
            bone = np.array([part_of.get(node['bones'][j][0], 0) if node['bones'] else 0 for j in k])
            tb = bone[t[:, 0]]
            for b in np.unique(tb):
                tt = t[tb == b]
                P = V[tt][:, :, 0:3] * SCALE
                N = V[tt][:, :, 7:10]
                T = np.stack([V[tt][:, :, 10], 1.0 - V[tt][:, :, 11]], axis=-1)
                acc.setdefault((int(b), name), []).append((P, N, T))
    return {k: clean(*(np.concatenate([x[i] for x in v]) for i in range(3))) for k, v in acc.items()}


def clean(P, N, T):
    """Without degenerate triangles and without the second of a pair on the same three positions (the PC's Uzi draws
    every triangle twice, once each way round: as it is, MeshLab cannot collapse a single edge); of such a pair the
    one facing as its normals stays."""
    q = np.round(P.reshape(-1, 3) / 0.02).astype(np.int64)
    _, weld = np.unique(q, axis=0, return_inverse=True)
    F = weld.reshape(-1, 3)
    ok = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    facing = np.einsum('ij,ij->i', face_normals(P), N.mean(1)) >= 0
    seen, keep = {}, []
    for i in np.argsort(~facing, kind='stable'):  # the ones facing as their normals first
        if not ok[i]:
            continue
        key = tuple(sorted(F[i]))
        if key in seen:
            continue
        seen[key] = i
        keep.append(i)
    keep = np.sort(np.array(keep, dtype=np.int64))
    return P[keep], N[keep], T[keep]


def face_normals(P):
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    return n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-12)


def winding_sign(P, N):
    """+1 when the triangles' (a, b, c) cross product faces as their vertex normals, -1 otherwise (the majority)."""
    d = np.einsum('ij,ij->i', face_normals(P), N.mean(1))
    return 1 if (d > 0).sum() >= (d < 0).sum() else -1


def decimate(P, T, target, keep_rim=True):
    """A triangle soup (3 positions, 3 uvs per triangle in order) welded by position and reduced to about `target`
    triangles with MeshLab's quadric collapse (texture coordinates kept; skin_body_fbx.reduce's settings). keep_rim:
    the open edges stay (a light reduction); a heavy one lets them go too (a model of many small open pieces, the
    PC's Uzi, cannot shrink otherwise), and the new positions stay on the old ones (MeshLab's optimal placement drew
    spikes out of the thin pieces then). Returns (positions, wedge uvs n x 3 x 2, faces)."""
    import tempfile
    import pymeshlab
    _, weld = np.unique(np.round(P / 0.02), axis=0, return_inverse=True)
    weld = weld.reshape(-1)
    first = np.full(weld.max() + 1, -1)
    for i in range(len(P) - 1, -1, -1):
        first[weld[i]] = i
    F = first[weld].reshape(-1, 3)
    ok = (F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])
    corners = np.arange(len(P)).reshape(-1, 3)[ok]
    F = F[ok]
    used = np.unique(F)
    remap = np.full(len(P), -1)
    remap[used] = np.arange(len(used))
    F = remap[F]
    ms = pymeshlab.MeshSet()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        Image.new("RGB", (4, 4)).save(tmp / "t.png")
        (tmp / "m.mtl").write_text("newmtl m\nmap_Kd t.png\n")
        lines = ["mtllib m.mtl", "usemtl m"]
        lines += ["v %.6f %.6f %.6f" % tuple(v) for v in P[used]]
        lines += ["vt %.7f %.7f" % tuple(w) for w in T[corners.reshape(-1)]]
        lines += ["f %d/%d %d/%d %d/%d" % (f[0] + 1, 3 * i + 1, f[1] + 1, 3 * i + 2, f[2] + 1, 3 * i + 3)
                  for i, f in enumerate(F)]
        (tmp / "m.obj").write_text("\n".join(lines) + "\n")
        ms.load_new_mesh(str(tmp / "m.obj"))
    if target < len(F):
        ms.meshing_decimation_quadric_edge_collapse_with_texture(
            targetfacenum=int(target), preserveboundary=keep_rim, boundaryweight=2.0, optimalplacement=keep_rim,
            preservenormal=True, qualitythr=B.QUALITY, extratcoordw=B.SEAMS, planarquadric=True)
    m = ms.current_mesh()
    return m.vertex_matrix(), m.wedge_tex_coord_matrix().reshape(-1, 3, 2), m.face_matrix()


_REDUCED = {}


def reduce_piece(P, N, T, target):
    """A piece (triangle soup) reduced to about `target` triangles; normals from the nearest original corner facing
    the same way."""
    from scipy.spatial import cKDTree
    n = len(P)
    if target >= n:
        return P, N, T
    flatP = P.reshape(-1, 3)
    V, W, F = decimate(flatP, T.reshape(-1, 2), max(4, int(target)), keep_rim=target >= n / 2)
    Pn = V[F]
    fn = face_normals(Pn)
    tree = cKDTree(flatP)
    flatN = N.reshape(-1, 3)
    Nn = np.zeros_like(Pn)
    k = min(12, len(flatP))
    for c in range(3):
        d, j = tree.query(Pn[:, c], k=k)
        j = j.reshape(len(Pn), -1)
        cand = flatN[j]
        score = np.einsum('ijk,ik->ij', cand, fn) - 0.001 * d.reshape(len(Pn), -1)
        Nn[:, c] = cand[np.arange(len(Pn)), score.argmax(1)]
    return Pn, Nn, W


# ---- textures --------------------------------------------------------------------------------------------------------

def tpl_images(tpl):
    """The palette's images: (header 0x24 bytes, data) each, in order."""
    n, table = struct.unpack_from(">II", tpl, 4)
    hdrs = [struct.unpack_from(">II", tpl, table + i * 8) for i in range(n)]
    datas = sorted(struct.unpack_from(">I", tpl, h + 8)[0] for h, _ in hdrs)
    out = []
    for h, pal in hdrs:
        if pal:
            raise ValueError("a palette with a colour table (CI formats) is not supported")
        d = struct.unpack_from(">I", tpl, h + 8)[0]
        nxt = [x for x in datas if x > d]
        end = nxt[0] if nxt else len(tpl)
        out.append((bytes(tpl[h:h + 0x24]), bytes(tpl[d:end])))
    return out


def build_tpl(images):
    n = len(images)
    head = 12 + n * 8
    out = bytearray(U.align(head + n * 0x24))
    struct.pack_into(">III", out, 0, 0x0020AF30, n, 12)
    for i, (hdr, data) in enumerate(images):
        out += bytes(U.align(len(out)) - len(out))
        hdr = bytearray(hdr)
        struct.pack_into(">I", hdr, 8, len(out))
        out[head + i * 0x24:head + i * 0x24 + 0x24] = hdr
        struct.pack_into(">II", out, 12 + i * 8, head + i * 0x24, 0)
        out += data
    out += bytes(U.align(len(out)) - len(out))
    return bytes(out)


def find_texture(folder, name):
    want = name.lower()
    for p in Path(folder).rglob('*'):
        if p.is_file() and p.name.lower() == want:
            return p
    stem = Path(want).stem
    for p in Path(folder).rglob('*'):
        if p.is_file() and p.stem.lower() == stem and p.suffix.lower() in ('.tga', '.png', '.dds', '.bmp'):
            return p
    return None


_IMG = {}
_ENC = {}


def texture_image(folder, name, notes, disc_tpl=None):
    if name not in _IMG:
        p = find_texture(folder, name)
        colour = [t for t in U.disc_textures(disc_tpl) if t[2] == 0xE] if disc_tpl else []
        if p is None and colour:
            # one of the PC game's own textures (wep09_00.tga ...): the PC's models of the original weapons keep the
            # disc's layout, so the disc palette's largest colour image
            big = max(colour, key=lambda t: t[0] * t[1])
            notes.append("texture %s not in the pack: the disc's %dx%d" % (name, big[1], big[0]))
            _IMG[name] = U.decode_gx(*big).convert("RGB")
        elif p is None:
            notes.append("texture %s missing: grey" % name)
            _IMG[name] = Image.new("RGB", (8, 8), (110, 110, 110))
        else:
            _IMG[name] = Image.open(p).convert("RGB")
    return _IMG[name]


def encode_texture(img, side, hdr_template):
    w, h = img.size
    s = min(1.0, side / float(max(w, h)))
    tw = max(8, 1 << int(round(np.log2(max(1, w * s)))))
    th = max(8, 1 << int(round(np.log2(max(1, h * s)))))
    base = img.resize((tw, th), Image.LANCZOS).convert("RGBA")
    levels = [base]
    while min(levels[-1].size) > 8:
        levels.append(base.resize((levels[-1].size[0] // 2, levels[-1].size[1] // 2), Image.LANCZOS))
    hdr, data = H.encode_levels(levels, 0xE, hdr_template, H.enc_cmpr)
    return hdr, data, (tw, th)


# ---- the model -------------------------------------------------------------------------------------------------------

def model_triangles(blob):
    """Any model's triangles per part: [(texture index, P n x 3 x 3, N n x 3 x 3, T n x 3 x 2 with v down)]."""
    b = bytes(blob)
    flags, shift = struct.unpack_from(">I", b, 0x20)[0], b[0x28]
    clr, s8 = bool(flags & 0x80000000), bool(flags & 0x20000000)
    nv, nn = struct.unpack_from(">HH", b, 0x38)
    vo, no, to = (struct.unpack_from(">I", b, o)[0] for o in (0x30, 0x34, 0x10))
    Pa = np.frombuffer(b, ">i2", nv * 4, vo).reshape(-1, 4)[:, :3] / float(1 << shift)
    Na = (np.frombuffer(b, ">i1", nn * 4, no).reshape(-1, 4)[:, :3] / 64.0 if s8 else
          np.frombuffer(b, ">i2", nn * 4, no).reshape(-1, 4)[:, :3] / 16384.0)
    lists = S.part_lists(b)
    nt = 1 + max((c[-1] for _p, tris in lists for t in tris for c in t), default=0)
    Ta = np.frombuffer(b, ">i2" if clr else ">u2", nt * 2, to).reshape(-1, 2) / (256.0 if clr else 32768.0)
    out = []
    for p, tris in lists:
        if not tris:
            continue
        t = np.array(tris)
        out.append((b[p + 0xC], Pa[t[:, :, 0]], Na[t[:, :, 1]], Ta[t[:, :, -1]]))
    return out


def template_header(template):
    """The 0x18-byte header of the template's largest part drawing with a colour texture."""
    best, size = None, -1
    for p in G.parts(template):
        n = struct.unpack_from(">I", template, p + 0x1C)[0]
        if template[p + 0xC] != 0xFF and n > size:
            best, size = p, n
    if best is None:
        best = G.parts(template)[0]
    return bytes(template[best:best + 0x18])


def build_weapon_model(template, pieces, tex_index, tex_count):
    """pieces: {(part, texture name): (P, N, T)}; tex_index: texture name -> palette index."""
    # the disc model's winding (all its parts, whatever texture)
    mt = model_triangles(template)
    sign = winding_sign(np.concatenate([x[1] for x in mt]), np.concatenate([x[2] for x in mt])) if mt else 1
    Ps, Ns, Ts, groups, bones = [], [], [], {}, []
    base = 0
    for (part, name), (P, N, T) in pieces.items():
        if not len(P):
            continue
        if winding_sign(P, N) != sign:
            P, N, T = P[:, ::-1], N[:, ::-1], T[:, ::-1]
        n = len(P)
        Ps.append(P.reshape(-1, 3))
        Ns.append(N.reshape(-1, 3))
        Ts.append(T.reshape(-1, 2))
        bones += [part] * (3 * n)
        ix = np.arange(base, base + 3 * n).reshape(n, 3)
        tri = np.stack([ix, ix, ix], axis=-1)
        groups.setdefault(tex_index[name], []).append(tri)
        base += 3 * n
    P = np.concatenate(Ps)
    N = np.concatenate(Ns)
    T = np.concatenate(Ts)
    tmpl = bytearray(template)
    # the coordinate range: lower the position shift until the weapon fits (32767 / 2^shift)
    reach = np.abs(P).max()
    while reach > 32767.0 / float(1 << tmpl[0x28]) and tmpl[0x28] > 0:
        tmpl[0x28] -= 1
    hdr = bytearray(template_header(template))
    parts = []
    for ti in sorted(groups):
        h = bytearray(hdr)
        h[0xC] = ti
        parts.append((bytes(h), np.concatenate(groups[ti])))
    blob = HF.build_model(bytes(tmpl), P, N, T, parts)
    blob = bytearray(blob)
    struct.pack_into(">I", blob, 0x24, tex_count)  # u32 (cModelData::nTex; trans: at most 0xF7)
    # each position rigidly on its part (the first vertex at that quantised place names it)
    ps = float(1 << blob[0x28])
    where = {}
    for p, b in zip(P, bones):
        where.setdefault(tuple(int(round(c * ps)) for c in p), b)
    Q = G.positions(bytes(blob))
    per = [((where.get(tuple(int(round(c * ps)) for c in q), 0), 100.0),) for q in Q]
    H.write_weights(blob, per)
    return bytes(blob), sum(len(t) for _, t in parts)


# ---- main ------------------------------------------------------------------------------------------------------------

def convert(disc, maps, tex_dir, tris, side, budget, notes, palettes=None):
    gc = U.Archive(disc, '>')
    n = len(gc.offsets)
    entries = [gc.entry(i) for i in range(n)]
    types = list(gc.types)
    raw = {e: pmd_pieces(path) for e, path in maps.items()}
    # the palette the models draw with: the TPL entry the weapon module passes (the nearest TPL before or after)
    tpls = [i for i in range(n) if gc.kind(i) == 'TPL' and entries[i]]

    def palette_of(e):
        if palettes and e in palettes:
            return palettes[e]
        return min(tpls, key=lambda t: (abs(t - e), t > e))

    users = {}
    for e in maps:
        users.setdefault(palette_of(e), []).append(e)
    caps = {e: min(tris, sum(len(v[0]) for v in raw[e].values())) for e in maps}
    sides = {}
    for e in maps:
        for (_part, name) in raw[e]:
            sides[name] = side
    for _round in range(80):
        new = list(entries)
        report = []
        for t, es in users.items():
            images = tpl_images(entries[t])
            names = sorted({name for e in es for (_p, name) in raw[e]})
            index = {}
            for name in names:
                key = (name, sides[name], t)
                if key not in _ENC:
                    _ENC[key] = encode_texture(texture_image(tex_dir, name, notes, entries[t]), sides[name],
                                               images[-1][0])
                hdr, data, size = _ENC[key]
                index[name] = len(images)
                images.append((hdr, data))
                report.append("  texture %s -> %dx%d (palette entry %d, index %d)" % (name, size[0], size[1], t,
                                                                                       index[name]))
            new[t] = build_tpl(images)
            for e in es:
                pieces = {}
                total = sum(len(v[0]) for v in raw[e].values())
                for key, (P, N, T) in raw[e].items():
                    want = max(4, int(round(len(P) * caps[e] / float(total))))
                    ck = (maps[e], key, want)
                    if ck not in _REDUCED:
                        _REDUCED[ck] = reduce_piece(P, N, T, want)
                    pieces[key] = _REDUCED[ck]
                blob, count = build_weapon_model(entries[e], pieces, index, len(images))
                new[e] = blob
                report.append("  model entry %d: %d triangles (PMD %d), %d bytes (disc %d)" % (e, count, total,
                                                                                              len(blob),
                                                                                              len(entries[e])))
        body = U.build_body(new, types)
        if gc.rel:
            size = U.align(len(body)) + len(gc.body) - gc.rel
        else:
            size = len(body)
        report.append("  data part %d bytes of %d" % (size, budget))
        if size <= budget:
            return U.build_archive(gc, body), report
        # smaller, in steps that keep the two in balance: the triangles to 1500 a model, the textures to 128, the
        # triangles to 800, the textures to 64, the triangles to 400, the textures to 32, then the triangles further
        big = max(sides.values()) if sides else 0
        for floor, tex in ((1500, 128), (800, 64), (400, 32), (150, 8)):
            if any(caps[e] > floor for e in maps):
                for e in maps:
                    caps[e] = max(floor, int(caps[e] * 0.85))
                break
            if big > tex:
                for name in sides:
                    if sides[name] == big:
                        sides[name] //= 2
                break
        else:
            break
    raise SystemExit("the weapon does not fit its place (%d bytes)" % size)


def preview(out_dir, disc, new, entries, palettes=None):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    a, b = U.Archive(disc, '>'), U.Archive(new, '>')
    tpls = [i for i in range(len(b.offsets)) if b.kind(i) == 'TPL' and b.entry(i)]
    fig, axs = plt.subplots(len(entries), 2, figsize=(12, 3.2 * len(entries)), dpi=90, squeeze=False)
    for row, e in enumerate(entries):
        for col, arc in enumerate((a, b)):
            ax = axs[row, col]
            ax.axis('off')
            blob = arc.entry(e)
            t = (palettes or {}).get(e) or min(tpls, key=lambda x: (abs(x - e), x > e))
            imgs = U.disc_textures(arc.entry(t))
            polys, cols = [], []
            for k, P, N, T in model_triangles(blob):
                img = U.decode_gx(*imgs[k]) if k < len(imgs) else None
                arr = np.asarray(img.convert('RGB'), dtype=np.float64) / 255.0 if img is not None else None
                uv = T.mean(1)
                if arr is not None:
                    hh, ww = arr.shape[:2]
                    x = np.clip((uv[:, 0] % 1.0) * ww, 0, ww - 1).astype(int)
                    y = np.clip((uv[:, 1] % 1.0) * hh, 0, hh - 1).astype(int)
                    c = arr[y, x]
                else:
                    c = np.full((len(P), 3), 0.6)
                shade = 0.45 + 0.55 * np.abs(face_normals(P) @ np.array([0.0, 0.6, 0.8]))
                polys.append(P)
                cols.append(np.clip(c * shade[:, None], 0, 1))
            if not polys:
                continue
            P = np.concatenate(polys)
            C = np.concatenate(cols)
            k = np.argsort(P[:, :, 1].mean(1))  # seen from above (+y): the weapon's side faces the camera
            proj = P[k][:, :, [0, 2]]
            ax.add_collection(PolyCollection(proj, facecolors=C[k], edgecolors='none'))
            ax.autoscale()
            ax.set_aspect('equal')
            ax.set_title('%s entry %d, %d triangles' % ('disc' if col == 0 else 'new', e, len(P)), fontsize=9)
    plt.tight_layout()
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    out = Path(out_dir) / 'preview.png'
    plt.savefig(out)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('disc')
    ap.add_argument('out')
    ap.add_argument('--pmd', action='append', required=True,
                    help='ENTRY[:TPL]=FILE.pmd (entries in hex or decimal; TPL: the palette entry the module draws the '
                         'model with, default the nearest TPL entry, the one before on a tie)')
    ap.add_argument('--tex', required=True)
    ap.add_argument('--tris', type=int, default=4000)
    ap.add_argument('--size', type=int, default=256)
    ap.add_argument('--budget', type=lambda s: int(s, 0), default=LIMIT - 0x1000)
    ap.add_argument('--preview')
    a = ap.parse_args(argv)
    maps, palettes = {}, {}
    for spec in a.pmd:
        e, _, path = spec.partition('=')
        e, _, t = e.partition(':')
        maps[int(e, 0)] = path
        if t:
            palettes[int(e, 0)] = int(t, 0)
    disc = Path(a.disc).read_bytes()
    notes = []
    out, report = convert(disc, maps, a.tex, a.tris, a.size, a.budget, notes, palettes)
    Path(a.out).write_bytes(out)
    print("%s: %d bytes" % (a.out, len(out)))
    for line in report + ["  " + n for n in sorted(set(notes))]:
        print(line)
    if a.preview:
        print("preview:", preview(a.preview, disc, out, sorted(maps), palettes))


if __name__ == '__main__':
    main()
