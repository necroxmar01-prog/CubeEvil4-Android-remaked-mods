#!/usr/bin/env python3
"""Takes a model of one converted skin into another (wish of DevJunior, 09.11.2026: Leon Asia's hair on Leon
Requiem).

    skin_graft.py TARGET.drs SOURCE.drs OUT.drs --entry 5 --tpl 3 [--hue 28 --sat 1.1 --value 0.7]

Both are archives written by uhd_skin_convert.py (or the disc's) with the same skeleton. Entry --entry of
SOURCE replaces TARGET's; the textures its parts use (colour, bump, alpha, specular) are taken from SOURCE's
palette --tpl and appended to TARGET's, the part headers renumbered. --hue (degrees), --sat and --value tint the
colour textures (material_tex) of the grafted model only: the hue set, saturation and brightness scaled.
--alpha-ref sets the alpha compare reference of its alpha-textured parts (hair: clean strand edges).
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
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--entry", type=int, required=True, help="the model's DAT entry (Leon's hair: 5)")
    ap.add_argument("--tpl", type=int, required=True, help="its palette's DAT entry (Leon's head: 3)")
    ap.add_argument("--hue", type=float, help="hue of the colour textures, degrees (brown: about 25-30)")
    ap.add_argument("--sat", type=float, default=1.0)
    ap.add_argument("--value", type=float, default=1.0, help="brightness factor (below 1: darker)")
    ap.add_argument("--alpha-ref", type=int, help="alpha compare reference of the model's alpha-textured parts "
                    "(trans.cpp alphaSetup: alpha > ref is drawn; 0 on the disc, higher: no faint texels at the edges)")
    a = ap.parse_args(argv)

    tgt = U.Archive(open(a.target, "rb").read(), ">")
    src = U.Archive(open(a.source, "rb").read(), ">")
    model = bytearray(src.entry(a.entry))
    src_imgs = tpl_images(src.entry(a.tpl))
    imgs = tpl_images(tgt.entry(a.tpl))
    first = len(imgs)
    used, colour = {}, set()
    for p in parts(model):
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
    for t, new in sorted(used.items(), key=lambda kv: kv[1]):
        hdr, data = src_imgs[t]
        if t in colour and (a.hue is not None or a.sat != 1.0 or a.value != 1.0):
            data = tint(hdr, data, a.hue, a.sat, a.value)
        imgs.append((hdr, data))
    struct.pack_into(">I", model, 0x24, len(imgs))  # nTex: the palette's count
    entries = [tgt.entry(i) for i in range(len(tgt.offsets))]
    entries[a.entry] = bytes(model)
    entries[a.tpl] = build_tpl(imgs)
    data = U.build_archive(tgt, U.build_body(entries, tgt.types))
    open(a.out, "wb").write(data)
    print("%s: entry %d from %s, textures %s -> %s (colour %s), %d bytes" %
          (a.out, a.entry, a.source, sorted(used), [used[t] for t in sorted(used)], sorted(colour), len(data)))


if __name__ == "__main__":
    main()
