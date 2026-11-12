#!/usr/bin/env python3
"""A card hair from an FBX for a skin (wish of DevJunior, 10.11.2026: the Resident Evil Requiem Leon's hair, an FBX
exported with RE-Mesh-Editor, on Leon Black Coat):

    skin_hair_fbx.py SKIN.drs OUT.drs --fbx Leon.fbx --alpha Hair_ACOT.tga --albedo Hair_ALBS.tga
                     [--layers Hair00:0.35,Hair01:0.2,Hair02:1] [--colour R,G,B] [--preview DIR]

The skin's hair (archive entry 5, drawn with palette 3) is replaced by the FBX's hair meshes (named by their
material, "...__Hair00_Mat" and so on):

- Where: the FBX (metres, z up, facing -y) turned into the game's axes and put on the skin's head (entry 2) by a
  uniform scale and a shift: first by the eyeballs' centres (the FBX's "Eyeball_L/R" meshes onto the game's eye
  sockets), then fitted over the scalp (the FBX's face mesh above the eyes onto the head's, nearest points). What
  still lies inside the skull afterwards is lifted over it (skin_slim.cover, --cover GAP, in a narrow cone).
- How much: a card hair of a current game has tens of thousands of positions; per layer only the share given is
  kept, the outer cards first, in turns around the head (skin_slim.thin_cards' rule), so no side goes bald.
- Skinning: every position takes the weights of the skin's own hair (its nearest four positions, inverse squared
  distance, at most three bones; merged to the 255 entries s8 normals allow), so the hair keeps the game's hair
  physics if the skin's had it (skin_graft.py --hair-weights).
- Textures: the hair's colour and alpha textures (the ones its part draws with) anew: the alpha from the FBX
  texture's strand mask (--alpha, the red channel of an RE Engine "ACOT" texture, or --alpha-channel), 1024 x 512
  I8; the colour one colour (--colour, default the skin's old hair colour) shaded by the strands' brightness in
  --albedo (an RE Engine "ALBS": its colours are strand ids, only the brightness is used), 512 x 256 CMPR. Both
  with a full mipmap chain, the alpha's keeping the hair covered when it is seen small (coverage_mips, --thicken,
  --fill; DevJunior's device test of v28: without them only thin sheets of hair, the head showing through).
  The part's alpha compare reference --alpha-ref (default 64: clean strand tips). The head's scalp takes the
  hair's colour, darker (a thinned card hair has gaps).

Every other entry stays as it was. Skins, FBX files and textures never go into the repository; this tool only reads
and writes the paths given.
"""
import argparse
import collections
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_face as F  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_slim as S  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

HEAD, HEAD_TPL, HAIR = 2, 3, 5

# ---- a minimal binary FBX (7.x) reader -------------------------------------------------------------------------------

ARR = {b"f": "<f4", b"d": "<f8", b"l": "<i8", b"i": "<i4", b"b": "u1"}


class Node:
    def __init__(self, name, props, children):
        self.name, self.props, self.children = name, props, children

    def find(self, name):
        return [c for c in self.children if c.name == name]

    def first(self, name):
        r = self.find(name)
        return r[0] if r else None


def _prop(d, o):
    t = d[o:o + 1]
    o += 1
    fixed = {b"Y": ("<h", 2), b"I": ("<i", 4), b"F": ("<f", 4), b"D": ("<d", 8), b"L": ("<q", 8)}
    if t in fixed:
        f, n = fixed[t]
        return struct.unpack_from(f, d, o)[0], o + n
    if t == b"C":
        return d[o] != 0, o + 1
    if t in (b"S", b"R"):
        n = struct.unpack_from("<I", d, o)[0]
        v = d[o + 4:o + 4 + n]
        return (v.decode("utf8", "replace") if t == b"S" else v), o + 4 + n
    if t in ARR:
        n, enc, clen = struct.unpack_from("<III", d, o)
        raw = d[o + 12:o + 12 + clen]
        if enc:
            raw = zlib.decompress(raw)
        return np.frombuffer(raw, dtype=ARR[t], count=n), o + 12 + clen
    raise ValueError("FBX: property type %r at %d" % (t, o))


def _node(d, o, wide):
    if wide:
        end, n, _ = struct.unpack_from("<QQQ", d, o)
        o += 24
    else:
        end, n, _ = struct.unpack_from("<III", d, o)
        o += 12
    nl = d[o]
    o += 1
    if end == 0:
        return None, o
    name = d[o:o + nl].decode("ascii", "replace")
    o += nl
    props = []
    for _ in range(n):
        p, o = _prop(d, o)
        props.append(p)
    kids = []
    while o < end:
        c, o2 = _node(d, o, wide)
        o = o2
        if c is None:
            break
        kids.append(c)
    return Node(name, props, kids), end


def load_fbx(path):
    d = Path(path).read_bytes()
    if not d.startswith(b"Kaydara FBX Binary"):
        raise ValueError("%s: not a binary FBX (export it as binary)" % path)
    wide = struct.unpack_from("<I", d, 23)[0] >= 7500
    o, top = 27, []
    while o < len(d):
        n, o = _node(d, o, wide)
        if n is None:
            break
        top.append(n)
    return Node("root", [], top)


def meshes(root):
    """name (the material part of "Group_0_Sub_1__Hair01_Mat") -> (positions in the game's axes, triangles of
    (position, normal, uv) per corner, normals, uvs)."""
    out = {}
    for g in root.first("Objects").find("Geometry"):
        if len(g.props) < 3 or g.props[2] != "Mesh":
            continue
        name = g.props[1].split("\x00")[0].split("__")[-1]
        V = g.first("Vertices").props[0].reshape(-1, 3).astype(np.float64)
        pv = g.first("PolygonVertexIndex").props[0]
        corners = np.where(pv < 0, ~pv, pv)
        ends = np.nonzero(pv < 0)[0]
        tris, start = [], 0
        for e in ends:  # fans for polygons with more than three corners
            for k in range(start + 1, e):
                tris.append((start, k, k + 1))
            start = e + 1
        tris = np.array(tris, dtype=np.int64).reshape(-1, 3)

        def layer(kind, data, index):
            el = g.first(kind)
            vals = el.first(data).props[0]
            mapping = el.first("MappingInformationType").props[0]
            ref = el.first("ReferenceInformationType").props[0]
            idx = el.first(index).props[0] if ref == "IndexToDirect" else None
            if mapping == "ByPolygonVertex":
                return vals, (idx if idx is not None else np.arange(len(corners)))
            if mapping in ("ByVertice", "ByVertex"):
                return vals, (idx[corners] if idx is not None else corners)
            raise ValueError("%s: %s mapped %s" % (name, kind, mapping))

        Nv, nidx = layer("LayerElementNormal", "Normals", "NormalsIndex")
        UVv, tidx = layer("LayerElementUV", "UV", "UVIndex")
        N = Nv.reshape(-1, 3).astype(np.float64)
        T = UVv.reshape(-1, 2).astype(np.float64)
        # the game's axes: x, up = FBX z, forward = FBX -y; millimetres
        P = np.stack([V[:, 0], V[:, 2], -V[:, 1]], 1) * 1000.0
        N = np.stack([N[:, 0], N[:, 2], -N[:, 1]], 1)
        out[name] = (P, np.stack([corners[tris], nidx[tris], tidx[tris]], 2), N, T)
    return out


# ---- placing it on the skin's head -----------------------------------------------------------------------------------


# Leon's eye sockets in the game (the disc's head and the Remake mod's alike, port/tools/skin_face.py's profiles)
EYES = (np.array([28.0, 1722.0, 95.0]), np.array([-28.0, 1722.0, 95.0]))


def place(fbx, head):
    """(scale, shift) taking the FBX's head onto the skin's: the eyes first, then the scalp by nearest points."""
    from scipy.spatial import cKDTree
    H = G.positions(head)
    eyes = {k: v[0].mean(0) for k, v in fbx.items() if k.startswith("Eyeball_")}
    if len(eyes) == 2:
        fe = sorted(eyes.values(), key=lambda q: -q[0])
        se = EYES
        s = np.linalg.norm(se[0] - se[1]) / np.linalg.norm(fe[0] - fe[1])
        t = (se[0] + se[1]) / 2 - s * (fe[0] + fe[1]) / 2
        eye_y = (se[0][1] + se[1][1]) / 2
    else:
        s, t, eye_y = 1.0, np.zeros(3), 1722.0
    face = fbx.get("Face_Mat")
    if face is None:
        return s, t
    Fp = face[0]
    tree = cKDTree(H[H[:, 1] > eye_y + 5])
    for _ in range(30):
        Q = Fp * s + t
        src = Fp[Q[:, 1] > eye_y + 5]
        if len(src) < 50:
            break
        _, j = tree.query(src * s + t)
        dst = tree.data[j]
        sm, dm = src.mean(0), dst.mean(0)
        s = float(((src - sm) * (dst - dm)).sum() / ((src - sm) ** 2).sum())
        t = dm - s * sm
    return s, t


# ---- thinning --------------------------------------------------------------------------------------------------------


def pieces(P, tris):
    """Per triangle its card (connected by positions)."""
    parent = np.arange(len(P))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for t in tris[:, :, 0]:
        a = find(t[0])
        for q in t[1:]:
            b = find(q)
            if b != a:
                parent[b] = a
    root = np.array([find(i) for i in range(len(P))])
    return root[tris[:, 0, 0]], root


def thin(P, tris, keep, centre=(0.0, 1720.0, 0.0)):
    """The triangles of the share `keep` of the cards, the outer ones first in each cell of the head (30 degrees
    around, 40 high), as skin_slim.thin_cards."""
    if keep >= 1.0:
        return tris, None
    tri_piece, root = pieces(P, tris)
    c = np.array(centre)
    cells = collections.defaultdict(list)
    for r in np.unique(root):
        idx = np.nonzero(root == r)[0]
        q = P[idx].mean(0) - c
        dist = np.linalg.norm(P[idx] - c, axis=1).max()
        cells[(int((np.degrees(np.arctan2(q[0], q[2])) + 180) // 30), int(q[1] // 40))].append((dist, r))
    kept = set()
    for cell in cells.values():
        cell.sort(reverse=True)
        kept.update(r for _, r in cell[:max(1, int(round(len(cell) * keep)))])
    return tris[np.isin(tri_piece, list(kept))], (len(cells), len(np.unique(root)), len(kept))


# ---- the model -------------------------------------------------------------------------------------------------------


def build_hair(template, P, N, T, tris, colour=0):
    """A GameCube model with the template's header, bones, colours, weights, blend and flip tables and its first
    part's header, and these positions (game units), normals (unit), texture coordinates (FBX, v up) and triangles
    (three (position, normal, uv) corners each), one part, as triangle lists. Positions and normals take weight
    entry 0 (skin_graft.transfer_weights sets them)."""
    sec = S.sections(template)
    flags = struct.unpack_from(">I", template, 0x20)[0]
    if not flags & 0x20000000:
        raise ValueError("the skin's hair has s16 normals (more than 255 weights): not handled")
    has_clr = bool(flags & 0x80000000)
    shift = template[0x28]
    ps = float(1 << shift)

    def unique(rows):
        index, out, ref = {}, [], []
        for r in rows:
            if r not in index:
                index[r] = len(out)
                out.append(r)
            ref.append(index[r])
        return out, np.array(ref)

    used_p = np.unique(tris[:, :, 0])
    used_n = np.unique(tris[:, :, 1])
    used_t = np.unique(tris[:, :, 2])
    lim = 32767.0 / ps
    if np.abs(P[used_p]).max() > lim:
        raise ValueError("the hair reaches beyond the template's coordinate range (%.0f)" % lim)
    Pq, pref = unique([tuple(int(round(c * ps)) for c in P[i]) for i in used_p])
    Nn = N[used_n] / (np.linalg.norm(N[used_n], axis=1, keepdims=True) + 1e-12)
    Nq, nref = unique([tuple(int(max(-128, min(127, round(c * 64)))) for c in n) for n in Nn])
    uv_scale, lo, hi = (256.0, -32768, 32767) if has_clr else (32768.0, 0, 65535)
    Tq, tref = unique([(int(max(lo, min(hi, round(u * uv_scale)))), int(max(lo, min(hi, round((1.0 - v) * uv_scale)))))
                       for u, v in T[used_t]])
    if len(Pq) > 0xFFFF or len(Nq) > 0xFFFF or len(Tq) > 0xFFFF:
        raise ValueError("more than 65535 positions, normals or texture coordinates: keep fewer cards (--layers)")
    pmap = dict(zip(used_p.tolist(), pref.tolist()))
    nmap = dict(zip(used_n.tolist(), nref.tolist()))
    tmap = dict(zip(used_t.tolist(), tref.tolist()))
    out = bytearray(0x60)
    where = {}

    def put(name, data, a=32):
        out.extend(bytes(U.align(len(out), a) - len(out)))
        where[name] = len(out)
        out.extend(data)

    put("head", template[sec["head"][0]:sec["head"][0] + sec["head"][1]], 16)
    put("vtx", b"".join(struct.pack(">3hH", x, y, z, 0) for x, y, z in Pq))
    put("nrm", b"".join(struct.pack(">3bB", x, y, z, 0) for x, y, z in Nq))
    if has_clr:
        put("clr", template[sec["clr"][0]:sec["clr"][0] + sec["clr"][1]])
    put("tex", b"".join(struct.pack(">hh" if has_clr else ">HH", u, v) for u, v in Tq))
    put("wgt", template[sec["wgt"][0]:sec["wgt"][0] + sec["wgt"][1]])
    stream = bytearray()
    corners = [(pmap[a], nmap[b], tmap[c]) for t in tris for a, b, c in t]
    step = 0xFFFF // 3 * 3
    for c0 in range(0, len(corners), step):
        chunk = corners[c0:c0 + step]
        stream += struct.pack(">BH", 0x90, len(chunk))
        for pi, ni, ti in chunk:
            stream += struct.pack(">4H", pi, ni, colour, ti) if has_clr else struct.pack(">3H", pi, ni, ti)
    stream += bytes(U.align(len(stream)) - len(stream))
    p0 = list(G.parts(template))[0]
    put("parts", template[p0:p0 + 0x18] + struct.pack(">II", len(stream), len(tris)) + stream)
    for name in ("blend", "flip"):
        if name in sec:
            o, n = sec[name]
            put(name, template[o:o + n], 4)
    out.extend(bytes(U.align(len(out)) - len(out)))
    out[0:0x60] = template[0:0x60]
    for name, off in (("head", 0), ("clr", 0xC), ("tex", 0x10), ("wgt", 0x14), ("parts", 0x1C), ("vtx", 0x30),
                      ("nrm", 0x34)):
        struct.pack_into(">I", out, off, where.get(name, 0))
    struct.pack_into(">I", out, 0x2C, 0)  # no shapes
    struct.pack_into(">H", out, 0x1A, 1)  # one part
    if "blend" in where:
        struct.pack_into(">II", out, 0x40, where["blend"], where["flip"])
    struct.pack_into(">HH", out, 0x38, len(Pq), len(Nq))
    return out


def skin_weights(blob, ref, k=4, cap=0xFF, physics=1.0):
    """`blob` (bytearray, the new hair) skinned as `ref` (the skin's old hair), as skin_graft.transfer_weights but
    merged to `cap` entries (uhd_skin_convert.merge_weights: coarser steps until they fit). `physics` scales the
    shares of the hair's swinging bones (65..85, pl_cloth.cpp leonHair): 0 = the hair moves with the head alone
    (DevJunior, 11.11.2026, device test: "the bald head still shows": a card close to the skull, swung by its
    chain, sinks into it; the renders show the rest pose). Returns (entries, step, bones)."""
    from scipy.spatial import cKDTree
    P, Q = G.positions(blob), G.positions(ref)
    rw = G.model_weights(ref)
    vo = struct.unpack_from(">I", ref, 0x30)[0]
    qw = [rw[struct.unpack_from(">H", ref, vo + i * 8 + 6)[0]] for i in range(len(Q))]
    d, j = cKDTree(Q).query(P, k=min(k, len(Q)))
    d, j = d.reshape(len(P), -1), j.reshape(len(P), -1)
    swing = lambda b: 64 < b <= 85
    still = collections.Counter(b for w in qw for b, r in w if not swing(b))
    main = still.most_common(1)[0][0] if still else 4
    per = []
    for dd, jj in zip(d, j):
        acc = {}
        for dist, q in zip(dd, jj):
            f = 1.0 / (dist * dist + 1.0)
            for b, r in qw[q]:
                acc[b] = acc.get(b, 0.0) + f * r * (physics if swing(b) else 1.0)
        acc = {b: v for b, v in acc.items() if v > 1e-9} or {main: 1.0}
        top = sorted(acc.items(), key=lambda x: -x[1])[:3]
        total = sum(v for _, v in top)
        per.append(tuple((b, 100.0 * v / total) for b, v in top))
    return write_weights(blob, per, cap)


def write_weights(blob, per, cap=0xFF):
    """Per position of `blob` (bytearray) its weights ((bone, percent), at most three) merged to `cap` entries
    (uhd_skin_convert.merge_weights), set on the positions and on the normals each draws with (the first position a
    normal is drawn with gives it), the table appended. Returns (entries, step, bones)."""
    entries, remap, step = U.merge_weights(per, cap)
    P = G.positions(blob)
    flags = struct.unpack_from(">I", blob, 0x20)[0]
    vo, no = struct.unpack_from(">I", blob, 0x30)[0], struct.unpack_from(">I", blob, 0x34)[0]
    for i in range(len(P)):
        struct.pack_into(">H", blob, vo + i * 8 + 6, remap[i])
    rec = 4 if flags & 0x80000000 else 3
    done = set()
    for p in G.parts(blob):
        size = struct.unpack_from(">I", blob, p + 0x18)[0]
        s, e = p + 0x20, p + 0x20 + size
        while s < e and blob[s] != 0:
            cnt = struct.unpack_from(">H", blob, s + 1)[0]
            for c in range(cnt):
                pi, ni = struct.unpack_from(">HH", blob, s + 3 + c * rec * 2)
                if ni not in done:
                    done.add(ni)
                    blob[no + ni * 4 + 3] = remap[pi]
            s += 3 + cnt * rec * 2
    wb = bytearray()
    for e in entries:
        ids = [b for b, _ in e] + [0] * (3 - len(e))
        rates = [r for _, r in e] + [0] * (3 - len(e))
        wb += struct.pack(">4B4B", ids[0], ids[1], ids[2], len(e), rates[0], rates[1], rates[2], 0)
    blob.extend(bytes(U.align(len(blob)) - len(blob)))
    struct.pack_into(">I", blob, 0x14, len(blob))
    blob[0x18] = len(entries)
    struct.pack_into(">H", blob, 0x2A, len(entries))
    blob.extend(wb)
    blob.extend(bytes(U.align(len(blob)) - len(blob)))
    return len(entries), step, sorted({b for e in entries for b, _ in e})


# ---- textures --------------------------------------------------------------------------------------------------------


def hair_colour(imgs, mc, ma):
    """The skin's old hair colour: its colour texture's mean where its alpha mask is opaque."""
    dec = lambda k: np.asarray(U.decode_gx(*struct.unpack_from(">HHI", imgs[k][0]), imgs[k][1]).convert("RGBA"),
                               dtype=np.float64)
    col, msk = dec(mc), dec(ma)
    if msk.shape[:2] != col.shape[:2]:
        msk = np.asarray(Image.fromarray(msk.astype(np.uint8)).resize(col.shape[1::-1]), dtype=np.float64)
    w = msk[..., 0] / 255.0
    w = np.where(w > 0.5, 1.0, 0.0) if (w > 0.5).sum() > 100 else np.ones_like(w)
    return tuple(int(round((col[..., c] * w).sum() / w.sum())) for c in range(3))


def mip_sizes(w, h):
    """The sizes of a full GX mipmap chain from w x h down to 1 x 1."""
    out = [(w, h)]
    while out[-1] != (1, 1):
        w, h = max(1, w // 2), max(1, h // 2)
        out.append((w, h))
    return out


def coverage_mips(base, ref, fill=0.5):
    """An alpha mask's mipmap chain that keeps the hair covered as it is seen smaller (DevJunior, 10.11.2026, device
    test of the RE9 hair: "only thin sheets of hair, his whole head shows"): averaged alone, single strands fade to a
    grey under the alpha compare reference and the cards vanish; without mipmaps the few texels a pixel picks are
    mostly gaps. Each level is the coverage averaged over boxes and scaled so that as many texels pass `ref` as at
    the base, plus `fill` of the rest the further down (a card hair seen small stands for many layers).
    `base`: floats 0..1. Returns float levels."""
    f0 = float((base * 255 >= ref).mean())
    out = [base]
    for k, (w, h) in enumerate(mip_sizes(base.shape[1], base.shape[0])[1:], 1):
        avg = np.asarray(Image.fromarray(out[0].astype(np.float32), "F").resize((w, h), Image.BOX), dtype=np.float64)
        target = min(0.97, f0 + (1 - f0) * fill * (1 - 0.5 ** k))
        lo, hi = 0.5, 64.0
        for _ in range(30):
            g = (lo * hi) ** 0.5
            if (np.clip(avg * g, 0, 1) * 255 >= ref).mean() < target:
                lo = g
            else:
                hi = g
        out.append(np.clip(avg * hi, 0, 1))
    return out


def encode_levels(levels, fmt, hdr, encode):
    """A TPL image (header, data) from mipmap levels (PIL images, base first): each level padded to whole tiles,
    the header's size, format, trilinear filtering and LOD range set (texture.cpp / trans.cpp take a mipmapped
    texture where minLOD differs from maxLOD)."""
    data = bytearray()
    tw, th = G.GX_TILE[fmt]
    for im in levels:
        w, h = im.size
        pw, ph = -(-w // tw) * tw, -(-h // th) * th
        if (pw, ph) != (w, h):
            pad = Image.new(im.mode, (pw, ph))
            pad.paste(im, (0, 0))
            for x in range(w, pw):  # repeat the edge (no seams when filtered)
                pad.paste(im.crop((w - 1, 0, w, h)), (x, 0))
            for y in range(h, ph):
                pad.paste(pad.crop((0, h - 1, pw, h)), (0, y))
            im = pad
        data += encode(im)
    hdr = bytearray(hdr)
    w, h = levels[0].size
    struct.pack_into(">HHI", hdr, 0, h, w, fmt)
    struct.pack_into(">II", hdr, 0x14, 5 if len(levels) > 1 else 1, 1)  # LIN_MIP_LIN, linear
    hdr[0x21], hdr[0x22] = 0, len(levels) - 1
    return bytes(hdr), bytes(data)


def _px(im):
    raw = im.convert("RGBA").tobytes()
    return [tuple(raw[k:k + 4]) for k in range(0, len(raw), 4)], im.size[0], im.size[1]


def enc_i8(im):
    px, w, h = _px(Image.merge("RGBA", (im, im, im, im)))
    return U.enc_i8(px, w, h)


def enc_cmpr(im):
    return U.enc_cmpr(*_px(im))


def textures(albedo, alpha, channel, colour, ref, alpha_size=(1024, 512), colour_size=(512, 256), thicken=0.5,
             fill=0.5, solid=0.0, harden=1.0, contrast=1.0):
    """(colour levels RGBA, alpha levels L), each a full mipmap chain. The alpha: the strand mask (`channel` of
    `alpha`) at `alpha_size`, its strands `thicken`ed (a share of the mask spread one texel: the few cards of a
    GameCube hair stand for many layers), then coverage_mips. The colour: one `colour`, shaded by the brightness of
    `albedo` on the strands, plain around them (no dark fringes when filtered)."""
    from PIL import ImageFilter
    a_img = Image.open(alpha).convert("RGBA").split()["RGBA".index(channel)]
    m0 = a_img.resize(alpha_size, Image.LANCZOS)
    base = np.asarray(m0, dtype=np.float64) / 255.0
    if thicken > 0:
        grown = np.asarray(m0.filter(ImageFilter.MaxFilter(3)), dtype=np.float64) / 255.0
        base = np.maximum(base, grown * thicken)
    if solid > 0:
        # the strands' surroundings filled (DevJunior, 11.11.2026, device test of v29: "the hair is still see-through,
        # his scalp shows"): the RE engine lays hundreds of such cards over each other, the few kept here show the
        # background between their strands where the screen is large enough for the full-size texture (the
        # emulator's 3x resolution). Where the mask blurred over 3 texels is dense, the card is solid by `solid`.
        dens = np.asarray(m0.filter(ImageFilter.GaussianBlur(3)), dtype=np.float64) / 255.0
        base = np.maximum(base, np.clip(dens * 3.0, 0, 1) * solid)
    levels = coverage_mips(base, ref, fill)
    if harden > 1:
        # the game blends a hair part by its alpha (trans.cpp bl[0]: source alpha, after the alpha compare): a texel
        # just over `ref` shows the scalp through it. Over `ref`, alpha rises `harden` times as fast to opaque.
        r = ref / 255.0
        levels = [np.where(lv >= r, np.minimum(1.0, r + (lv - r) * harden), lv) for lv in levels]
    alpha_levels = [Image.fromarray((lv * 255 + 0.5).astype(np.uint8), "L") for lv in levels]
    c = np.array(colour, dtype=np.float64)
    if albedo:
        lum = np.asarray(Image.open(albedo).convert("L").resize(colour_size, Image.LANCZOS), dtype=np.float64)
        m = np.asarray(a_img.resize(colour_size, Image.LANCZOS), dtype=np.float64) / 255.0
        mid = np.median(lum[m > 0.5]) if (m > 0.5).sum() > 100 else lum.mean()
        shade = np.clip(lum / max(mid, 1.0), 0.65, 1.35)
        if contrast != 1.0:
            # softer strands, and the filled surroundings (solid) take the strands' shading blurred instead of the
            # plain colour (DevJunior, 11.11.2026: "the texture looks odd": flat patches between sharp strands)
            from scipy.ndimage import gaussian_filter
            shade = 1.0 + (shade - 1.0) * contrast
            sm = gaussian_filter(shade * m, 4.0)
            mm = gaussian_filter(m, 4.0)
            around = np.where(mm > 0.02, sm / np.maximum(mm, 0.02), 1.0)
            shade = shade * m + around * (1 - m)
        else:
            shade = shade * m + 1.0 * (1 - m)
    else:
        shade = np.ones(colour_size[::-1])
    rgb = np.clip(c[None, None, :] * shade[..., None], 0, 255).astype(np.uint8)
    col = Image.fromarray(np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)]), "RGBA")
    colour_levels = [col] + [col.resize(sz, Image.BOX) for sz in mip_sizes(*col.size)[1:]]
    return colour_levels, alpha_levels


def scalp(imgs, head, face, colour, depth=0.6, dark=40.0, hair=None, reach=0.0, brow=1770.0, hairline=None,
          rules=True):
    """The head's scalp in the hair's colour (darker by `depth`): a card hair has gaps, and a head made for a hair
    of its own (the Remake mod's) is painted near black there. The head's colour textures' texels that lie above
    y 1700 on the model (at the back, z < -20, above 1630: the nape), not on the brow (in front, z > 60, below
    y 1765), and are dark (brightness under `dark`, faded in over 20); and, whatever their brightness, the ones
    under the hair: above y 1800, and above 1700 behind z 30 (not the ears). With `hair` (its positions) and `reach`
    also every texel above y `brow` with a hair position within `reach` of it, faded out over the last third
    (DevJunior, 11.11.2026, device test: "the bald head still shows": the brow and temples under the fringe).
    With `hairline` (a height): everything above a hairline at that height over the brow's middle, 0.3 lower per unit
    to the side (the temples), faded in over 8 units: no skin above it shows between the cards.
    rules=False: only `hair` and `hairline`, not the fixed regions above (they fit the disc's head and the Remake
    mod's; a face with its own hairline painted, as an RE Engine face's albedo, keeps it, skin_head_fbx.py).
    Returns (texture, texels changed) pairs."""
    out = []
    for p in G.parts(head):
        mc = head[p + 0xC]
        if mc in [m for m, _ in out] or mc == 0xFF:
            continue
        h, w, f = struct.unpack_from(">HHI", imgs[mc][0])
        if f != 0xE or max(w, h) < 256:
            continue
        pos = F.uv_positions([head] + ([face] if face else []), mc, w, h)
        img = np.asarray(U.decode_gx(h, w, f, imgs[mc][1]).convert("RGBA"), dtype=np.float64)
        lum = img[..., 0] * 0.3 + img[..., 1] * 0.59 + img[..., 2] * 0.11
        y, z = pos[..., 1], pos[..., 2]
        x = pos[..., 0]
        with np.errstate(invalid="ignore"):
            where = ((y > 1700) | ((z < -20) & (y > 1630))) & ~((z > 60) & (y < 1765))
            # under the hair whatever its brightness: the crown and the back and sides of the skull (not the ears)
            under = ((y > 1800) | ((y > 1700) & (z < 30))) & ~((np.abs(x) > 62) & (y < 1745) & (z > -35))
        wgt = np.where(where, np.clip((dark + 20.0 - lum) / 20.0, 0, 1), 0.0)
        wgt = np.where(under, 1.0, wgt)
        if not rules:
            wgt = np.zeros_like(wgt)
        if hair is not None and reach > 0:
            from scipy.spatial import cKDTree
            ok = np.isfinite(y) & (y > brow)
            d = np.full(y.shape, np.inf)
            if ok.any():
                d[ok] = cKDTree(hair).query(pos[ok], k=1)[0]
            near = np.clip((reach - d) / (reach / 3.0), 0, 1)
            wgt = np.maximum(wgt, near)
        if hairline is not None:
            with np.errstate(invalid="ignore"):
                line = hairline - 0.3 * np.abs(x)
                top = np.where(np.isfinite(y), np.clip((y - line) / 8.0, 0, 1), 0.0)
                top = np.where((np.abs(x) > 62) & (y < 1745) & (z > -35), 0.0, top)  # not the ears
            wgt = np.maximum(wgt, top)
        if (wgt > 0.5).sum() < 50:
            continue
        rng = np.random.default_rng(5)
        streak = 0.85 + 0.3 * np.repeat(rng.random((1, w)), h, 0)  # a faint vertical grain
        tgt = np.array(colour, dtype=np.float64)[None, None, :] * depth * streak[..., None]
        img[..., :3] = img[..., :3] * (1 - wgt[..., None]) + tgt * wgt[..., None]
        imgs[mc] = G.encode_image(Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA"), imgs[mc][0])
        out.append((mc, int((wgt > 0.5).sum())))
    return out


# ---- main ------------------------------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--fbx", required=True)
    ap.add_argument("--alpha", required=True, help="the hair's strand mask texture (e.g. ..._Hair_ACOT.tga)")
    ap.add_argument("--alpha-channel", default="R", choices=list("RGBA"))
    ap.add_argument("--albedo", help="the hair's albedo (its brightness shades the colour)")
    ap.add_argument("--layers", default="Hair00:0.35,Hair01:0.2,Hair02:1",
                    help="FBX hair meshes (material names without _Mat) and the share of their cards to keep")
    ap.add_argument("--colour", help="R,G,B of the hair (default: the skin's old hair colour)")
    ap.add_argument("--alpha-ref", type=int, default=64)
    ap.add_argument("--alpha-size", default="1024x512", help="the alpha texture's size (WxH, default 1024x512)")
    ap.add_argument("--thicken", type=float, default=0.5, help="spread this share of the strand mask one texel")
    ap.add_argument("--fill", type=float, default=0.5,
                    help="the smaller mipmap levels cover this share of the gaps more (see coverage_mips)")
    ap.add_argument("--solid", type=float, default=0.0,
                    help="fill the strands' surroundings on each card by this (0..1, see textures)")
    ap.add_argument("--harden", type=float, default=1.0,
                    help="the alpha over the compare reference rises this many times as fast to opaque (the game "
                         "blends the hair by its alpha)")
    ap.add_argument("--shade-contrast", type=float, default=1.0,
                    help="the strands' shading from the albedo times this; other than 1 the surroundings take it "
                         "blurred (see textures)")
    ap.add_argument("--scalp-depth", type=float, default=0.6, help="the scalp in the hair's colour times this")
    ap.add_argument("--cover", type=float, default=1.0, metavar="GAP",
                    help="lift the hair positions inside the head's skull GAP over it (default 1; negative: "
                         "leave them)")
    ap.add_argument("--cover-low", type=float, default=1700.0, metavar="Y",
                    help="lift only hair positions above this height (default 1700; the temples and the fringe over "
                         "the brow lie lower)")
    ap.add_argument("--scalp-reach", type=float, default=0.0, metavar="UNITS",
                    help="also paint the head under the hair wherever a hair position lies within this distance, "
                         "above the brow (see scalp; default 0: off)")
    ap.add_argument("--hairline", type=float, metavar="Y",
                    help="paint the head above a hairline at this height (brow middle; lower to the sides) in the "
                         "hair's colour (see scalp)")
    ap.add_argument("--physics", type=float, default=1.0,
                    help="the swinging hair bones' shares times this (0: the hair is fixed to the head)")
    ap.add_argument("--no-scalp", action="store_true", help="keep the head's scalp as it is (see scalp())")
    a = ap.parse_args(argv)

    arc = U.Archive(open(a.skin, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    template, head = entries[HAIR], entries[HEAD]
    fbx = meshes(load_fbx(a.fbx))
    s, t = place(fbx, head)
    print("FBX head on the skin's: scale %.3f, shift (%.1f, %.1f, %.1f)" % (s, *t))
    allP, allN, allT, allTris = [], [], [], []
    po = no = to = 0
    for spec in a.layers.split(","):
        name, keep = spec.split(":")
        key = name if name.endswith("_Mat") else name + "_Mat"
        if key not in fbx:
            raise SystemExit("the FBX has no mesh %s (it has: %s)" % (key, ", ".join(sorted(fbx))))
        P, tris, N, T = fbx[key]
        P = P * s + t
        kept, info = thin(P, tris, float(keep))
        print("%s: %d of %d triangles%s" % (key, len(kept), len(tris),
                                              " (%d of %d cards)" % (info[2], info[1]) if info else ""))
        allP.append(P)
        allN.append(N)
        allT.append(T)
        allTris.append(kept + np.array([po, no, to]))
        po, no, to = po + len(P), no + len(N), to + len(T)
    P, N, T = np.concatenate(allP), np.concatenate(allN), np.concatenate(allT)
    tris = np.concatenate(allTris)
    hair = build_hair(template, P, N, T, tris)
    ne, step, bones = skin_weights(hair, template, physics=a.physics)
    print("hair: %d positions, %d normals, %d triangles; weights from the skin's hair: %d entries (%d%% steps), "
          "bones %s" % (struct.unpack_from(">H", hair, 0x38)[0], struct.unpack_from(">H", hair, 0x3A)[0], len(tris),
                        ne, step, bones))
    hair = bytes(hair)
    if a.cover >= 0:
        hair, n, most = S.cover(hair, head, gap=a.cover, cone=4.0, low=a.cover_low)
        print("hair: %d positions lifted over the skull, at most %.0f" % (n, most))
    # the part's textures anew
    p0 = list(G.parts(hair))[0]
    mc, ma = hair[p0 + 0xC], hair[p0 + 0xE]
    imgs = G.tpl_images(entries[HEAD_TPL])
    colour = tuple(int(x) for x in a.colour.split(",")) if a.colour else hair_colour(imgs, mc, ma)
    aw, ah = (int(v) for v in a.alpha_size.lower().split("x"))
    ref = max(0, min(254, a.alpha_ref))
    cols, masks = textures(a.albedo, a.alpha, a.alpha_channel, colour, ref, alpha_size=(aw, ah), thicken=a.thicken,
                           fill=a.fill, solid=a.solid, harden=a.harden, contrast=a.shade_contrast)
    imgs[mc] = encode_levels(cols, 0xE, imgs[mc][0], enc_cmpr)
    imgs[ma] = encode_levels(masks, 0x1, imgs[ma][0], enc_i8)
    if not a.no_scalp:
        for mc_, n in scalp(imgs, head, entries[4], colour, depth=a.scalp_depth, hair=G.positions(hair),
                            reach=a.scalp_reach, hairline=a.hairline):
            print("scalp: texture %d, %d texels in the hair's colour" % (mc_, n))
    hair = bytearray(hair)
    hair[p0 + 0xB] |= 4
    hair[p0 + 0x14] = ref
    entries[HAIR] = bytes(hair)
    entries[HEAD_TPL] = G.build_tpl(imgs)
    print("textures: colour %d (%d,%d,%d) %dx%d, alpha %d %dx%d, %d mipmap levels each" %
          (mc, *colour, *cols[0].size, ma, *masks[0].size, len(masks)))
    body = U.build_body(entries, arc.types)
    data = U.build_archive(arc, body)
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's place holds %d)" % (a.out, len(data), len(body), 0x600000))
    if len(body) > 0x600000:
        raise SystemExit("too large: keep fewer cards (--layers)")


if __name__ == "__main__":
    main()
