#!/usr/bin/env python3
"""A player archive's texture recoloured to a neutral grey (DevJunior, 26.11.2026, device pictures of the sweater
version: "you gave it the remake's shirt instead of the grey one"; his choice of 26.11.2026: the classic grey shirt
under the remake's sweater): the texels of one texture keep their light and shade (brightness) and lose their hue,
scaled so that their mean brightness becomes --grey; reddish texels (the fur collar sharing the texture) and the texels
used by no triangle stay as they are. Texels far more saturated than the cloth (a print, a badge) can be kept with --keep-saturated.

    skin_recolor.py SKIN.drs OUT.drs --mat 9 [--tpl 1] [--entry 0] [--grey 34] [--warm 0.03] [--keep-saturated 60]

--mat    the texture (the TEV texture number of the body's parts; the Remake Lite pl08: 9, the shirt)
--grey   the cloth's mean brightness afterwards (0..255; the disc's classic shirt, pl08 texture 5: about 28)
--warm   a slight warm tint of the grey (red up, blue down by this share)
Writes OUT.drs; prints the texels changed. Disc and mod files never go into the repository.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_face as F  # noqa: E402
import skin_graft as G  # noqa: E402
import uhd_skin_convert as U  # noqa: E402


def recolor(rgb, used, grey, warm, keep_saturated):
    lum = rgb @ np.array([0.3, 0.59, 0.11])
    sat = rgb.max(-1) - rgb.min(-1)
    # the cloth: not reddish (the jacket's fur collar shares the remake shirt's texture), not a saturated print
    on = used & (sat < keep_saturated) & (rgb[..., 0] < rgb[..., 2] + 12)
    if not on.any():
        return rgb, 0
    scale = grey / max(lum[on].mean(), 1.0)
    tint = np.array([1.0 + warm, 1.0, 1.0 - warm])
    out = rgb.copy()
    out[on] = np.clip(lum[on, None] * scale * tint[None, :], 0, 255)
    return out, int(on.sum())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skin")
    ap.add_argument("out")
    ap.add_argument("--mat", type=int, required=True)
    ap.add_argument("--tpl", type=int, default=1)
    ap.add_argument("--entry", type=int, default=0)
    ap.add_argument("--grey", type=float, default=34.0)
    ap.add_argument("--warm", type=float, default=0.03)
    ap.add_argument("--keep-saturated", type=float, default=60.0)
    a = ap.parse_args(argv)
    from PIL import Image
    arc = U.Archive(open(a.skin, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    imgs = G.tpl_images(entries[a.tpl])
    hdr, data = imgs[a.mat]
    h, w, fmt = struct.unpack_from(">HHI", hdr)
    img = np.asarray(U.decode_gx(h, w, fmt, data).convert("RGBA"), dtype=np.float64)
    used = np.isfinite(F.uv_positions([entries[a.entry]], a.mat, w, h)[..., 1])
    from scipy.ndimage import maximum_filter
    used = maximum_filter(used.astype(np.uint8), 3) > 0  # the seams' edge texels too
    rgb, n = recolor(img[..., :3], used, a.grey, a.warm, a.keep_saturated)
    img[..., :3] = rgb
    imgs[a.mat] = G.encode_image(Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA"), hdr)
    entries[a.tpl] = G.build_tpl(imgs)
    out = U.build_archive(arc, U.build_body(entries, arc.types))
    Path(a.out).write_bytes(out)
    print("recolor: texture %d (%dx%d), %d texels grey (mean %.0f); %s: %d bytes" % (a.mat, w, h, n, a.grey, a.out,
                                                                                  len(out)))


if __name__ == "__main__":
    main()
