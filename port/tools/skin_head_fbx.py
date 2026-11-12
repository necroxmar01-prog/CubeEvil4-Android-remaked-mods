#!/usr/bin/env python3
"""A head from an FBX for a skin (wish of DevJunior, 12.11.2026: "the head of Leon from Resident Evil Requiem on the
Leon Requiem model"; an FBX exported with RE-Mesh-Editor and the face's RE Engine textures):

    skin_head_fbx.py SKIN.drs OUT.drs --fbx Leon.fbx --albedo Face_ALBD.tga [--scot Face_SCOT.tga]
                     [--normal Face_NRMR.tga] [--beard-albedo Beard_ALBS.tga] [--tex 1024] [--preview DIR]

The skin's head (archive entry 2, drawn with palette 3) keeps its eyes (the parts drawing with --keep-mats, the
eyeballs) and takes the FBX's face mesh ("Face_Mat": face, ears, neck) for its skin part:

- Where: as skin_hair_fbx.place, the transform the hair from the same FBX already took (the FBX's eyeballs onto the
  game's eye sockets, then its scalp onto the skin's): the face, its eyes and the hair fit one another. Below
  --neck (default the old head's lowest point) the neck is cut off (the collar hides the seam).
- Skinning: every position takes the weights of the old head's nearest four (skin_hair_fbx.skin_weights; merged to
  the 255 entries s8 normals allow).
- Texture: the skin part's texture (palette 3, the part's own index) anew, --tex square (CMPR): the FBX face's albedo
  with its cavity (--scot, blue channel) as shade; its normal map (--normal) as a light from above and in front;
  the eyebrows (--scot red: their mask) darkened to a brow colour; the FBX's eyebrow, eyelash and beard cards
  (meshes "Eyebrow_Mat", "Eyelash_Mat", "Beard_Mat") painted onto the skin where they lie (a card texture of their
  own is not drawn: the game's one texture per part, and thousands of cards the device cannot afford); the scalp
  under the hair in the hair's colour (skin_hair_fbx.scalp).
- No face shapes: run skin_shapes.py on the result for the cutscenes' mouth and blinking.

Every other entry stays as it was. Skins, FBX files and textures never go into the repository; this tool only reads
and writes the paths given.
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
import skin_slim as S  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

HEAD, HEAD_TPL, FACE, HAIR = 2, 3, 4, 5


def build_model(template, P, N, T, groups):
    """A GameCube model with the template's header, bones and tables, these positions (game units), normals (unit),
    texture coordinates (v up) and per part (template part offset, triangles of (position, normal, uv) corners) the
    template part's header and those triangles as triangle lists. s8 normals; weights set afterwards."""
    sec = S.sections(template)
    flags = struct.unpack_from(">I", template, 0x20)[0] | 0x20000000
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

    allt = np.concatenate([t for _, t in groups])
    used_p, used_n, used_t = (np.unique(allt[:, :, k]) for k in range(3))
    if np.abs(P[used_p]).max() > 32767.0 / ps:
        raise ValueError("the head reaches beyond the template's coordinate range")
    Pq, pref = unique([tuple(int(round(c * ps)) for c in P[i]) for i in used_p])
    Nn = N[used_n] / (np.linalg.norm(N[used_n], axis=1, keepdims=True) + 1e-12)
    Nq, nref = unique([tuple(int(max(-128, min(127, round(c * 64)))) for c in n) for n in Nn])
    uv_scale, lo, hi = (256.0, -32768, 32767) if has_clr else (32768.0, 0, 65535)
    Tq, tref = unique([(int(max(lo, min(hi, round(u * uv_scale)))), int(max(lo, min(hi, round((1.0 - v) * uv_scale)))))
                       for u, v in T[used_t]])
    if max(len(Pq), len(Nq), len(Tq)) > 0xFFFF:
        raise ValueError("more than 65535 positions, normals or texture coordinates")
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
    parts = bytearray()
    for p, tris in groups:
        stream = bytearray()
        corners = [(pmap[a], nmap[b], tmap[c]) for t in tris for a, b, c in t]
        step = 0xFFFF // 3 * 3
        for c0 in range(0, len(corners), step):
            chunk = corners[c0:c0 + step]
            stream += struct.pack(">BH", 0x90, len(chunk))
            for pi, ni, ti in chunk:
                stream += struct.pack(">4H", pi, ni, 0, ti) if has_clr else struct.pack(">3H", pi, ni, ti)
        stream += bytes(U.align(len(stream)) - len(stream))
        parts += template[p:p + 0x18] + struct.pack(">II", len(stream), len(tris)) + stream
    put("parts", parts)
    for name in ("blend", "flip"):
        if name in sec:
            o, n = sec[name]
            put(name, template[o:o + n], 4)
    out.extend(bytes(U.align(len(out)) - len(out)))
    out[0:0x60] = template[0:0x60]
    for name, off in (("head", 0), ("clr", 0xC), ("tex", 0x10), ("wgt", 0x14), ("parts", 0x1C), ("vtx", 0x30),
                      ("nrm", 0x34)):
        struct.pack_into(">I", out, off, where.get(name, 0))
    struct.pack_into(">I", out, 0x20, flags)
    struct.pack_into(">I", out, 0x2C, 0)  # no shapes (skin_shapes.py)
    struct.pack_into(">H", out, 0x1A, len(groups))
    if "blend" in where:
        struct.pack_into(">II", out, 0x40, where["blend"], where["flip"])
    struct.pack_into(">HH", out, 0x38, len(Pq), len(Nq))
    return out


def corner_mesh(Pc, Nc, Tc):
    """Per-corner arrays (n x 3 x k) as (positions, triangles of (p, n, t) indices, normals, uvs)."""
    n = len(Pc)
    idx = np.arange(n * 3).reshape(n, 3)
    tris = np.stack([idx, idx, idx], 2)
    return Pc.reshape(-1, 3), tris, Nc.reshape(-1, 3), Tc.reshape(-1, 2)


def density(points, texel_pos, radius):
    """Per texel: how many of `points` lie within `radius` of its model position (0 where it has none)."""
    from scipy.spatial import cKDTree
    out = np.zeros(texel_pos.shape[:2])
    ok = np.isfinite(texel_pos[..., 0])
    if len(points) and ok.any():
        tree = cKDTree(points)
        out[ok] = [len(r) for r in tree.query_ball_point(texel_pos[ok], radius)]
    return out


def mirror_clean(img, texel_pos, below, darker=18.0):
    """The face texture's marks on one side only (the Virus face's growth on the neck) from the other side: below
    height `below` on the model, a texel darker than its mirror (u -> 1 - u: the face's texture is symmetric) by more
    than `darker` takes the mirror's colour, faded in over 12 and softened. Returns how many texels."""
    from scipy.ndimage import gaussian_filter
    lum = img[..., :3] @ np.array([0.3, 0.59, 0.11])
    mir = img[:, ::-1]
    diff = gaussian_filter(lum[:, ::-1] - lum, 2.0)
    with np.errstate(invalid="ignore"):
        low = np.where(np.isfinite(texel_pos[..., 1]), texel_pos[..., 1] < below, False)
    w = np.clip((diff - darker) / 12.0, 0, 1) * low
    w = np.clip(gaussian_filter(w, 3.0) * 1.6, 0, 1)
    img[..., :3] = img[..., :3] * (1 - w[..., None]) + mir[..., :3] * w[..., None]
    return int((w > 0.5).sum())


def skin_mean(img, texel_pos):
    """The cheeks' mean colour: texels on the face's front between the nose and the eyes' height, off the middle."""
    x, y, z = texel_pos[..., 0], texel_pos[..., 1], texel_pos[..., 2]
    with np.errstate(invalid="ignore"):
        m = (y > 1660) & (y < 1715) & (np.abs(x) > 18) & (np.abs(x) < 50) & (z > 60)
    return img[..., :3][m].mean(0) if m.sum() > 20 else None


def face_texture(a, size, texel_pos, cards, brow_colour, beard_colour, tone=None):
    """The face's colour texture (RGBA, size x size) from the FBX face's textures, see the module text."""
    load = lambda p: Image.open(p).convert("RGBA").resize((size, size), Image.LANCZOS)
    img = np.asarray(load(a.albedo), dtype=np.float64)
    if a.clean_below:
        print("face: %d texels marked on one side taken from the other" % mirror_clean(img, texel_pos, a.clean_below))
    if tone is not None and a.match > 0:
        have = skin_mean(img, texel_pos)
        if have is not None:
            k = (1 - a.match) + a.match * np.asarray(tone) / np.maximum(have, 1.0)
            img[..., :3] *= k[None, None, :]
            print("face: skin tone %s towards the skin's %s" % (tuple(int(v) for v in have), tuple(int(v) for v in tone)))
    if a.scot:
        sc = np.asarray(load(a.scot), dtype=np.float64) / 255.0
        cav = sc[..., 2]
        img[..., :3] *= (1.0 - a.cavity + a.cavity * cav)[..., None]
        brow = np.clip(1.0 - sc[..., 0], 0, 1) * a.brows  # red channel: 0 under the eyebrows
        img[..., :3] = img[..., :3] * (1 - brow[..., None]) + np.array(brow_colour)[None, None, :] * brow[..., None]
    if a.normal:
        nm = np.asarray(load(a.normal), dtype=np.float64) / 127.5 - 1.0
        light = np.array([0.0, 0.55, 0.0])  # tangent space: up the texture (above the face), slightly
        shade = 1.0 + a.relief * (nm[..., 0] * light[0] + nm[..., 1] * light[1])
        img[..., :3] *= np.clip(shade, 0.6, 1.4)[..., None]
    # the cards painted where they lie
    for key, colour, amount, radius, cap in (("Eyebrow_Mat", brow_colour, a.brow_cards, 1.2, 8.0),
                                              ("Eyelash_Mat", (25, 20, 18), a.lashes, 0.9, 6.0),
                                              ("Beard_Mat", beard_colour, a.beard, 1.0, 10.0)):
        if amount <= 0 or key not in cards:
            continue
        d = density(cards[key], texel_pos, radius)
        w = np.clip(d / cap, 0, 1) * amount
        img[..., :3] = img[..., :3] * (1 - w[..., None]) + np.array(colour)[None, None, :] * w[..., None]
        print("face: %s painted on %d texels" % (key, int((w > 0.2).sum())))
    img[..., 3] = 255
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--fbx", required=True)
    ap.add_argument("--albedo", required=True, help="the face's albedo (..._Face_..._ALBD.tga)")
    ap.add_argument("--scot", help="the face's SCOT texture: red the eyebrows' mask, blue the cavity")
    ap.add_argument("--normal", help="the face's normal map (..._NRMR.tga: red and green)")
    ap.add_argument("--beard-albedo", help="the beard's albedo (its average colour paints the beard)")
    ap.add_argument("--tex", type=int, default=1024, help="the face texture's size (power of two, default 1024)")
    ap.add_argument("--keep-mats", default="2", help="the old head's parts (texture indices) to keep: its eyes")
    ap.add_argument("--eyes-follow", type=int, default=1, help="1: the kept eyes moved onto the FBX's eyeballs")
    ap.add_argument("--neck", type=float, help="cut the neck below this height (default: the old head's lowest)")
    ap.add_argument("--cavity", type=float, default=0.35, help="the cavity's shade (0..1)")
    ap.add_argument("--relief", type=float, default=0.25, help="the normal map's light (0..1)")
    ap.add_argument("--brows", type=float, default=0.75, help="the eyebrow mask darkened by this (0..1)")
    ap.add_argument("--brow-cards", type=float, default=0.6, help="the eyebrow cards painted this strong")
    ap.add_argument("--lashes", type=float, default=0.5, help="the eyelash cards painted this strong")
    ap.add_argument("--beard", type=float, default=0.35, help="the beard cards painted this strong")
    ap.add_argument("--brow-colour", default="48,38,30")
    ap.add_argument("--beard-colour", default="40,32,26", help="the beard cards' colour (--beard-albedo: its hue)")
    ap.add_argument("--clean-below", type=float, default=1660.0,
                    help="below this height marks on one side of the face only are taken from the other (0: off)")
    ap.add_argument("--match", type=float, default=0.6, help="the skin tone moved towards the old face's (0..1)")
    ap.add_argument("--neck-fit", type=float, default=1625.0,
                    help="below this height the neck no wider than the old head's (0: off): it fits the collar")
    ap.add_argument("--tuck", type=int, default=-1, help="the body entry whose collar the neck stays under (-1: off)")
    ap.add_argument("--hairline", type=float, help="the scalp painted above this height (skin_hair_fbx.scalp)")
    ap.add_argument("--hair-colour", default="44,35,25", help="the scalp under the hair (skin_hair_fbx.scalp)")
    ap.add_argument("--scalp-rules", action="store_true",
                    help="also skin_hair_fbx.scalp's fixed regions (made for the disc's head; the FBX face has a hairline)")
    ap.add_argument("--scalp-depth", type=float, default=0.8)
    ap.add_argument("--scalp-reach", type=float, default=0.0)
    a = ap.parse_args(argv)

    arc = U.Archive(open(a.skin, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    head = entries[HEAD]
    fbx = H.meshes(H.load_fbx(a.fbx))
    s, t = H.place(fbx, head)
    print("FBX head on the skin's: scale %.3f, shift (%.1f, %.1f, %.1f)" % (s, *t))
    keep = [int(x) for x in a.keep_mats.split(",") if x]
    skin_part = [p for p in G.parts(head) if not G.is_empty(head, p) and head[p + 0xC] not in keep]
    if len(skin_part) != 1:
        raise SystemExit("the old head has %d parts besides %s: give --keep-mats" % (len(skin_part), keep))
    skin_part = skin_part[0]
    skin_mat = head[skin_part + 0xC]
    old = G.positions(head)
    neck = a.neck if a.neck is not None else float(old[:, 1].min())
    # the face
    Pf, trisf, Nf, Tf = fbx["Face_Mat"]
    Pf = Pf * s + t
    keep_t = Pf[trisf[:, :, 0], 1].max(1) >= neck
    if a.neck is None:  # around the neck, each side as low as the old head reaches there (its back sits higher)
        ang = lambda X: ((np.arctan2(X[..., 0], X[..., 2] + 30.0) + np.pi) / (2 * np.pi) * 24).astype(int) % 24
        low = np.full(24, np.inf)
        np.minimum.at(low, ang(old), old[:, 1])
        for _ in range(3):
            low = np.where(np.isfinite(low), low, np.minimum(np.roll(low, 1), np.roll(low, -1)))
        C = Pf[trisf[:, :, 0]].mean(1)
        keep_t &= Pf[trisf[:, :, 0], 1].max(1) >= low[ang(C)]
        print("neck: cut as low as the old head reaches, per side %s" % np.round(low).astype(int).tolist())
    trisf = trisf[keep_t]
    # the kept parts of the old head, per corner; with the FBX's eyeballs each eye moved into their place (the box
    # centres): the new lids close over the old eyes as over their own
    shift = {1: np.zeros(3), -1: np.zeros(3)}
    eyes = [k for k in fbx if k.startswith("Eyeball_")]
    if a.eyes_follow and len(eyes) == 2:
        iris = U.gc_triangles(head, keep[0])[0].reshape(-1, 3)
        new_eyes = np.concatenate([fbx[k][0] * s + t for k in eyes])
        for side in (1, -1):
            o, n = iris[iris[:, 0] * side > 0], new_eyes[new_eyes[:, 0] * side > 0]
            if len(o) and len(n):
                shift[side] = (n.min(0) + n.max(0)) / 2 - (o.min(0) + o.max(0)) / 2
        print("eyes: moved by %s (left) and %s (right)" % (np.round(shift[1], 1).tolist(), np.round(shift[-1], 1).tolist()))
    Ps, Ns, Ts, groups = [Pf], [Nf], [Tf], []
    po, no, to = len(Pf), len(Nf), len(Tf)
    for p in G.parts(head):
        if G.is_empty(head, p) or head[p + 0xC] not in keep:
            continue
        Pc, Nc, Tc = U.gc_triangles(head, head[p + 0xC])
        Pc = Pc + np.where((Pc.mean(1)[:, 0] > 0)[:, None, None], shift[1], shift[-1])
        Tc = Tc.copy()
        Tc[..., 1] = 1.0 - Tc[..., 1]  # build_model takes v up, as the FBX
        P2, t2, N2, T2 = corner_mesh(Pc, Nc, Tc)
        groups.append((p, t2 + np.array([po, no, to])))
        Ps.append(P2), Ns.append(N2), Ts.append(T2)
        po, no, to = po + len(P2), no + len(N2), to + len(T2)
        print("kept: the old head's part with texture %d, %d triangles" % (head[p + 0xC], len(Pc)))
    groups.insert(0, (skin_part, trisf))
    P, N, T = np.concatenate(Ps), np.concatenate(Ns), np.concatenate(Ts)
    model = build_model(head, P, N, T, groups)
    if a.neck_fit:
        model, n = S.tuck(bytes(model), head, top=a.neck_fit, inset=0.0)
        model = bytearray(model)
        print("neck: %d positions below y %.0f kept within the old head's neck" % (n, a.neck_fit))
    if a.tuck >= 0 and entries[a.tuck]:
        model, n = S.tuck(bytes(model), entries[a.tuck])
        model = bytearray(model)
        print("neck: %d positions kept under the collar of entry %d" % (n, a.tuck))
    ne, step, bones = H.skin_weights(model, head, physics=0.0)
    print("head: %d positions, %d normals, %d face triangles (neck from y %.0f); weights from the old head: %d entries "
          "(%d%% steps), bones %s" % (struct.unpack_from(">H", model, 0x38)[0], struct.unpack_from(">H", model, 0x3A)[0],
                                      len(trisf), neck, ne, step, bones))
    entries[HEAD] = bytes(model)
    # the face texture
    imgs = G.tpl_images(entries[HEAD_TPL])
    hdr = bytearray(imgs[skin_mat][0])
    struct.pack_into(">HH", hdr, 0, a.tex, a.tex)
    texel_pos = F.uv_positions([entries[HEAD]], skin_mat, a.tex, a.tex)
    cards = {k: fbx[k][0] * s + t for k in ("Eyebrow_Mat", "Eyelash_Mat", "Beard_Mat") if k in fbx}
    beard_colour = np.array([float(x) for x in a.beard_colour.split(",")])
    if a.beard_albedo:  # the RE Engine tints its beards: the texture is pale, its hue at the beard's brightness
        b = np.asarray(Image.open(a.beard_albedo).convert("RGB"), dtype=np.float64).reshape(-1, 3).mean(0)
        beard_colour = b * (beard_colour @ [0.3, 0.59, 0.11]) / (b @ [0.3, 0.59, 0.11])
    beard_colour = tuple(int(x) for x in beard_colour)
    old_pos = F.uv_positions([head], skin_mat, *struct.unpack_from(">HH", imgs[skin_mat][0])[::-1])
    old_img = np.asarray(U.decode_gx(*struct.unpack_from(">HHI", imgs[skin_mat][0]), imgs[skin_mat][1])
                         .convert("RGBA"), dtype=np.float64)
    tone = skin_mean(old_img, old_pos)
    brow_colour = tuple(int(x) for x in a.brow_colour.split(","))
    face = face_texture(a, a.tex, texel_pos, cards, brow_colour, beard_colour, tone)
    imgs[skin_mat] = G.encode_image(face, bytes(hdr))
    hair = G.positions(entries[HAIR]) if entries[HAIR] else None
    colour = tuple(int(x) for x in a.hair_colour.split(","))
    for mc, n in H.scalp(imgs, entries[HEAD], entries[FACE], colour, depth=a.scalp_depth, hair=hair,
                         reach=a.scalp_reach, hairline=a.hairline,
                         rules=a.scalp_rules):
        print("scalp: texture %d, %d texels in the hair's colour" % (mc, n))
    entries[HEAD_TPL] = G.build_tpl(imgs)
    print("face texture: %d, %dx%d (beard colour %s)" % (skin_mat, a.tex, a.tex, beard_colour))
    body = U.build_body(entries, arc.types)
    data = U.build_archive(arc, body)
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's place holds %d)" % (a.out, len(data), len(body), 0x600000))
    if len(body) > 0x600000:
        raise SystemExit("too large: a smaller face texture (--tex 512)")


if __name__ == "__main__":
    main()
