#!/usr/bin/env python3
"""Takes a model of one converted skin into another (wish of DevJunior, 09.11.2026: Leon Asia's hair on Leon
Requiem).

    skin_graft.py TARGET.drs SOURCE.drs OUT.drs --entry 5 --tpl 3 [--hue 28 --sat 1.1 --value 0.7]

Both are archives written by uhd_skin_convert.py (or the disc's) with the same skeleton. Entry --entry of
SOURCE replaces TARGET's; the textures its parts use (colour, bump, alpha, specular) are taken from SOURCE's
palette --tpl and appended to TARGET's, the part headers renumbered. --hue (degrees), --sat and --value tint the
colour textures (material_tex) of the grafted model only: the hue set, saturation and brightness scaled.
--alpha-ref sets the alpha compare reference of its alpha-textured parts (hair: clean strand edges).
--into puts it in another entry (Leon's pl000l, entry 12, drawn in costumes 1-3), --only-mats keeps some of its
parts, --drop ENTRY:MATS empties parts of the target (a jacket); emptied parts hold GX NOPs, offsets unchanged.
--colorize R,G,B tints by brightness in a mean colour, --source-image takes a grafted texture from the mod's own
larger image (less blocky once tinted), --inflate ENTRY:DIST makes the torso, arms and legs bulkier.
"""
import argparse
import colorsys
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402

TPL_MAGIC = 0x0020AF30


def tpl_images(tpl):
    """(header 0x24 bytes, data) per image of a GameCube TPL."""
    out = []
    n = struct.unpack_from(">I", tpl, 4)[0]
    for i in range(n):
        ho = struct.unpack_from(">I", tpl, 12 + i * 8)[0]
        h, w, f, data = struct.unpack_from(">HHII", tpl, ho)
        out.append((bytes(tpl[ho:ho + 0x24]), tpl[data:data + w * h * U.GX_SIZE[f] // 8]))
    return out


def build_tpl(images):
    n = len(images)
    head = 12 + n * 8
    out = bytearray(U.align(head + n * 0x24))
    struct.pack_into(">III", out, 0, TPL_MAGIC, n, 12)
    for i, (hdr, data) in enumerate(images):
        out += bytes(U.align(len(out)) - len(out))
        struct.pack_into(">II", out, 12 + i * 8, head + i * 0x24, 0)
        out[head + i * 0x24:head + i * 0x24 + 0x24] = hdr
        struct.pack_into(">I", out, head + i * 0x24 + 8, len(out))
        out += data
    return bytes(out)


def parts(blob):
    """Offsets of the part headers of a GameCube model."""
    p = struct.unpack_from(">I", blob, 0x1C)[0]
    for _ in range(struct.unpack_from(">H", blob, 0x1A)[0]):
        yield p
        p += 0x20 + struct.unpack_from(">I", blob, p + 0x18)[0]


def is_empty(blob, p):
    size = struct.unpack_from(">I", blob, p + 0x18)[0]
    return not any(blob[p + 0x20:p + 0x20 + size])


def empty_parts(blob, which):
    """Fills the primitive streams of the parts whose colour texture `which` picks with GX NOPs (0): the part
    draws nothing, every offset stays. Returns how many."""
    n = 0
    for p in list(parts(blob)):
        if which(blob[p + 0xC]):
            size = struct.unpack_from(">I", blob, p + 0x18)[0]
            blob[p + 0x20:p + 0x20 + size] = bytes(size)
            n += 1
    return n


def colorize(img, rgb):
    """The image's brightness in the mean colour rgb: out = brightness / mean brightness * rgb (alpha kept)."""
    import numpy as np
    a = np.asarray(img.convert("RGBA"), dtype=np.float32)
    lum = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    used = lum > 2
    mean = float(lum[used].mean()) if used.any() else 1.0
    out = a.copy()
    for k in range(3):
        out[..., k] = np.clip(lum / mean * rgb[k], 0, 255)
    from PIL import Image
    return Image.fromarray(out.astype(np.uint8), "RGBA")


def smooth(img, radius, keep=0.35):
    """Plain cloth: the brightness blurred (radius in texels; only over the painted texels, so the empty black around
    the UV islands does not darken their edges) with keep of its remaining contrast; bright details stay sharp."""
    import numpy as np
    from PIL import Image
    a = np.asarray(img.convert("RGBA"), dtype=np.float32)
    lum = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    used = (lum > 2).astype(np.float32)
    r = int(radius * 3 + 0.5)
    g = np.exp(-0.5 * (np.arange(-r, r + 1) / radius) ** 2)
    g /= g.sum()

    def blur(m):  # separable Gaussian, edges repeated
        m = np.pad(m, r, mode="edge")
        m = np.apply_along_axis(lambda v: np.convolve(v, g, "valid"), 0, m)
        return np.apply_along_axis(lambda v: np.convolve(v, g, "valid"), 1, m)
    soft = blur(lum * used) / np.maximum(blur(used), 1e-3)
    mean = float(lum[used > 0].mean()) if used.any() else 0.0
    flat = mean + (soft - mean) * keep
    k = np.clip((90 - lum) / 60, 0, 1) * used  # bright details and the empty texels as they are
    ratio = np.where(lum > 1e-3, (flat * k + lum * (1 - k)) / np.maximum(lum, 1e-3), 1.0)
    out = a.copy()
    for ch in range(3):
        out[..., ch] = np.where(lum > 1e-3, a[..., ch] * ratio, flat * k)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")


def recolor(img, rgb, radius=0.0):
    """colorize for the dark texels only: bright details (buckles, zippers, patches) fade back to their own colour
    from brightness 30 to 90, so they do not glare. radius > 0: plain cloth first (smooth)."""
    import numpy as np
    from PIL import Image
    if radius > 0:
        img = smooth(img, radius)
    a = np.asarray(img.convert("RGBA"), dtype=np.float32)
    c = np.asarray(colorize(img, rgb), dtype=np.float32)
    lum = a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11
    k = np.clip((90 - lum) / 60, 0, 1)[..., None]
    out = a.copy()
    out[..., :3] = c[..., :3] * k + a[..., :3] * (1 - k)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")


def encode_image(img, hdr):
    """img as CMPR with a header of its size (the rest of hdr kept)."""
    w, h = img.size
    raw = img.convert("RGBA").tobytes()
    px = [tuple(raw[k:k + 4]) for k in range(0, len(raw), 4)]
    hdr = bytearray(hdr)
    struct.pack_into(">HHI", hdr, 0, h, w, 0xE)
    return bytes(hdr), U.enc_cmpr(px, w, h)


def inflate(blob, dist, physique="even"):
    """Pushes the positions of a GameCube model of Leon (bind pose, T-pose: y up, arms along x) away from the body's
    axes: the torso from the spine (x 0, z -12; y 950..1560, fading to 0 at 850 and 1620), the arms from their axis
    (y 1508, z -34; |x| 200..620, fading out to the hands), the legs from theirs (x +-92, z 0; y 300..950, fading out
    to the shoes at 220 and into the torso up to 1050; wish of DevJunior, 09.11.2026: the legs too, in proportion).
    Away from an axis, not along normals: layers (shirt, vest, straps) move alike and thin double-sided straps do not
    turn. physique "athletic" (references and device test of DevJunior, 09.11.2026: strong, not fat; the neck stays):
    the torso mostly sideways (broad chest and back), forward only at the pectorals, the waist and belly hardly, the
    neck and collar (|x| < 80 above y 1440) not at all; the arms by their muscles along the bones of Leon's skeleton
    (shoulder 172, elbow 436, wrist 590): deltoid in full (its underside, the armpit, 0.4x), biceps 0.85x, elbow 0.3x,
    forearm 0.45x, the hands not; the legs 0.35x (thighs) to 0.2x (calves). Returns how many positions moved."""
    import numpy as np
    u = lambda o: struct.unpack_from(">I", blob, o)[0]
    shift = blob[0x28]
    nv = struct.unpack_from(">H", blob, 0x38)[0]
    vo = u(0x30)
    moved = 0
    clip = lambda v: float(np.clip(v, 0, 1))
    for i in range(nv):
        x, y, z = (c / float(1 << shift) for c in struct.unpack_from(">3h", blob, vo + i * 8))
        ax = abs(x)
        sx = 1.0 if x >= 0 else -1.0
        dt = np.array([x, 0.0, z + 12.0])
        da = np.array([0.0, y - 1508.0, z + 34.0])
        dl = np.array([x - 92.0 * sx, 0.0, z])
        if dl[0] * x < 0:
            dl[0] = 0.0  # the inner side of a leg not towards the other one (the knees nearly touch)
        dt /= np.linalg.norm(dt) or 1.0
        da /= np.linalg.norm(da) or 1.0
        dl /= np.linalg.norm(dl) or 1.0
        if physique == "even":
            wt = clip((y - 850) / 100) * clip((1620 - y) / 60) * clip((300 - ax) / 80)
            wa = clip((ax - 200) / 60) * clip((680 - ax) / 60) if 1380 < y < 1660 else 0.0
            wl = clip((y - 220) / 80) * clip((1050 - y) / 100) if ax < 260 else 0.0
            if wt <= 0 and wa <= 0 and wl <= 0:
                continue
            d = dt * wt + da * wa + dl * wl
            n = np.linalg.norm(d)
            if n < 1e-6:
                continue
            push = d / n * max(wt, wa, wl)
        elif physique == "athletic":
            wt = clip((y - 1000) / 80) * clip((1620 - y) / 60) * clip((280 - ax) / 80)
            neck = 1.0 - (1.0 - clip((ax - 80) / 70)) * clip((y - 1440) / 40)
            side = 0.1 + 0.9 * clip((y - 1100) / 200)  # waist hardly, chest and back in full
            front = 0.45 * clip((y - 1300) / 50) * clip((1500 - y) / 50)  # the pectorals only, no belly
            back = 0.4 * clip((y - 1200) / 150)  # lats and upper back
            pt = np.array([dt[0] * side, 0.0, dt[2] * (front if dt[2] > 0 else back)]) * wt * neck
            pa = np.zeros(3)
            if 1380 < y < 1660 and ax < 660:
                prof = float(np.interp(ax, [170, 230, 320, 380, 436, 500, 560, 600, 640],
                                       [0, 1.0, 0.85, 0.8, 0.3, 0.45, 0.3, 0.08, 0]))
                if ax < 330 and da[1] < 0:
                    prof *= 0.4  # the armpit under the deltoid
                pa = da * prof
            pl = np.zeros(3)
            if ax < 260:
                pl = dl * clip((y - 220) / 80) * clip((1050 - y) / 100) * (0.2 + 0.15 * clip((y - 500) / 200))
            push = pt + pa + pl
        else:
            raise ValueError("physique: even or athletic")
        if np.linalg.norm(push) < 1e-6:
            continue
        q = np.array([x, y, z]) + push * dist
        qi = [max(-32768, min(32767, int(round(c * (1 << shift))))) for c in q]
        struct.pack_into(">3h", blob, vo + i * 8, *qi)
        moved += 1
    return moved


def tint(hdr, data, hue, sat, value):
    h, w, f = struct.unpack_from(">HHI", hdr)
    if f != 0xE:
        return data
    img = U.decode_gx(h, w, f, data)
    px = []
    raw = img.tobytes()
    for k in range(0, len(raw), 4):
        r, g, b, a = raw[k:k + 4]
        hh, ss, vv = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        if hue is not None:
            hh = hue / 360.0
        rr, gg, bb = colorsys.hsv_to_rgb(hh, min(1.0, ss * sat), min(1.0, vv * value))
        px.append((int(rr * 255 + 0.5), int(gg * 255 + 0.5), int(bb * 255 + 0.5), a))
    return U.enc_cmpr(px, w, h)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("target")
    ap.add_argument("source", nargs="?", help="the skin to take a model from (none: only --drop / --inflate)")
    ap.add_argument("out")
    ap.add_argument("--entry", type=int, help="the model's DAT entry (Leon's hair: 5)")
    ap.add_argument("--tpl", type=int, help="its palette's DAT entry (Leon's head: 3)")
    ap.add_argument("--into", type=int, help="the target's entry it replaces (default: --entry; Leon's pl000l, "
                    "drawn in costumes 1-3 with palette 1: 12)")
    ap.add_argument("--into-tpl", type=int, help="the target's palette it draws with (default: --tpl)")
    ap.add_argument("--only-mats", help="comma-separated colour textures of the source model to keep (its parts "
                    "with others are emptied)")
    ap.add_argument("--drop", action="append", default=[], metavar="ENTRY:MATS",
                    help="empty the target model ENTRY's parts with these colour textures (e.g. 0:6,9: a jacket)")
    ap.add_argument("--hue", type=float, help="hue of the colour textures, degrees (brown: about 25-30)")
    ap.add_argument("--sat", type=float, default=1.0)
    ap.add_argument("--value", type=float, default=1.0, help="brightness factor (below 1: darker)")
    ap.add_argument("--colorize", help="R,G,B: the tinted textures by their brightness in this mean colour (no hue "
                    "noise in dark textures, unlike --hue)")
    ap.add_argument("--source-image", action="append", default=[], metavar="MAT=PATH[@WxH]",
                    help="a grafted colour texture from this image (the mod's own, larger), at WxH (default its size)")
    ap.add_argument("--inflate", action="append", default=[], metavar="ENTRY:DIST",
                    help="push a target model's torso, arms and legs out along their normals by DIST (bulkier; after the "
                         "graft, so a grafted model too)")
    ap.add_argument("--physique", choices=("even", "athletic"), default="even",
                    help="--inflate everywhere alike, or athletic: broad chest, back and deltoids, the waist, neck and "
                         "hands kept, less on the forearms and legs")
    ap.add_argument("--recolor", action="append", default=[], metavar="TPL:SLOTS=R,G,B",
                    help="colour textures SLOTS of the target's palette TPL by their brightness in this mean colour "
                         "(e.g. 1:2,13,16=40,43,52: a shirt and its matching parts)")
    ap.add_argument("--smooth", type=float, default=0.0, metavar="RADIUS",
                    help="with --recolor: plain cloth, the fabric's blotches blurred away (radius in texels, e.g. 6)")
    ap.add_argument("--tint-mats", help="comma-separated colour textures of the source to tint (default: all)")
    ap.add_argument("--alpha-ref", type=int, help="alpha compare reference of the model's alpha-textured parts "
                    "(trans.cpp alphaSetup: alpha > ref is drawn; 0 on the disc, higher: no faint texels at the edges)")
    a = ap.parse_args(argv)

    tgt = U.Archive(open(a.target, "rb").read(), ">")
    graft = a.entry is not None
    if graft:
        if a.source is None or a.tpl is None:
            sys.exit("a model to graft needs SOURCE and --tpl")
        src = U.Archive(open(a.source, "rb").read(), ">")
        into = a.entry if a.into is None else a.into
        into_tpl = a.tpl if a.into_tpl is None else a.into_tpl
        model = bytearray(src.entry(a.entry))
        if a.only_mats:
            keep = {int(x) for x in a.only_mats.split(",")}
            empty_parts(model, lambda m: m not in keep)
        src_imgs = tpl_images(src.entry(a.tpl))
        imgs = tpl_images(tgt.entry(into_tpl))
        first = len(imgs)
        used, colour = {}, set()
        for p in parts(model):
            if is_empty(model, p):
                continue
            flag = model[p + 0xB]
            fields = [(0xC, True), (0xD, False), (0xE, False)] + ([(0x17, False)] if flag & 0x10 else [])
            for off, is_colour in fields:
                t = model[p + off]
                if t == 0xFF:
                    continue
                if t not in used:
                    used[t] = first + len(used)
                if is_colour:
                    colour.add(t)
                model[p + off] = used[t]
            if a.alpha_ref is not None and flag & 4:
                model[p + 0x14] = max(0, min(254, a.alpha_ref))
        sources = {}
        for spec in a.source_image:
            mat, path = spec.split("=", 1)
            size = None
            if "@" in path:
                path, wh = path.rsplit("@", 1)
                size = tuple(int(v) for v in wh.lower().split("x"))
            sources[int(mat)] = (path, size)
        for t, new in sorted(used.items(), key=lambda kv: kv[1]):
            hdr, data = src_imgs[t]
            tinted = a.tint_mats is None or t in {int(x) for x in a.tint_mats.split(",")}
            if t in sources:
                from PIL import Image
                path, size = sources[t]
                img = Image.open(path).convert("RGBA")
                h0, w0 = struct.unpack_from(">HH", hdr)
                img = img.resize(size or (w0, h0), Image.LANCZOS)
                if tinted and a.colorize:
                    img = colorize(img, [float(v) for v in a.colorize.split(",")])
                hdr, data = encode_image(img, hdr)
            elif t in colour and tinted and a.colorize:
                hdr, data = encode_image(colorize(U.decode_gx(*struct.unpack_from(">HHI", hdr), data),
                                                  [float(v) for v in a.colorize.split(",")]), hdr)
            elif t in colour and tinted and (a.hue is not None or a.sat != 1.0 or a.value != 1.0):
                data = tint(hdr, data, a.hue, a.sat, a.value)
            imgs.append((hdr, data))
        struct.pack_into(">I", model, 0x24, len(imgs))  # nTex: the palette's count
    entries = [tgt.entry(i) for i in range(len(tgt.offsets))]
    for d in a.drop:
        e, mats = d.split(":")
        drop = {int(x) for x in mats.split(",")}
        blob = bytearray(entries[int(e)])
        n = empty_parts(blob, lambda m: m in drop)
        entries[int(e)] = bytes(blob)
        print("entry %s: %d parts with textures %s emptied" % (e, n, sorted(drop)))
    if graft:
        entries[into] = bytes(model)
        entries[into_tpl] = build_tpl(imgs)
    for spec in a.inflate:
        e, dist = spec.split(":")
        blob = bytearray(entries[int(e)])
        n = inflate(blob, float(dist), a.physique)
        entries[int(e)] = bytes(blob)
        print("entry %s: %d positions pushed out by up to %s" % (e, n, dist))
    for spec in a.recolor:
        where, rgb = spec.split("=")
        t, slots = where.split(":")
        imgs_t = tpl_images(entries[int(t)])
        for k in (int(x) for x in slots.split(",")):
            hdr, data = imgs_t[k]
            img = U.decode_gx(*struct.unpack_from(">HHI", hdr), data)
            imgs_t[k] = encode_image(recolor(img, [float(v) for v in rgb.split(",")], a.smooth), hdr)
        entries[int(t)] = build_tpl(imgs_t)
        print("palette %s: textures %s recoloured to %s" % (t, slots, rgb))
    data = U.build_archive(tgt, U.build_body(entries, tgt.types))
    open(a.out, "wb").write(data)
    if graft:
        print("%s: entry %d from %s as entry %d, textures %s -> %s (colour %s), %d bytes" %
              (a.out, a.entry, a.source, into, sorted(used), [used[t] for t in sorted(used)], sorted(colour), len(data)))
    else:
        print("%s: %d bytes" % (a.out, len(data)))


if __name__ == "__main__":
    main()
