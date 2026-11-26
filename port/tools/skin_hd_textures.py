#!/usr/bin/env python3
"""The disc's own textures of a player archive in the PC release's HD versions (wish of DevJunior, 26.11.2026:
"the classic shirt with the strap, but remastered"): the PC game (RE4 UHD) keeps every texture of em/plXX in one
pack BIO4/imagePackHD/0100000X.pack, in the order the disc file's palettes (TPL entries, archive order) hold them;
this tool puts the pack's pictures in place of the disc's colour textures (CMPR), larger:

    skin_hd_textures.py DISC.drs OUT.drs --packs DIR [--id 0x01000008] [--tpls 1,13] [--skip 1:5] [--scale 4] [--max 1024]

DISC.drs   the disc's archive (its palettes in their own order: the pack's index counts through them);
--packs    the folder with the unpacked .pack (port/tools/re4lfs.py unpacks a .pack.lfs);
--tpls     the palettes to replace (Leon: 1 body, 13 hands; 3 the head, left alone by default);
--scale    each texture this many times the disc's size, at most --max a side and never above the pack's own;
           a full mipmap chain (skin_hair_fbx.encode_levels).
A picture whose shape differs from the disc's (another aspect) is left as the disc's; every replacement is
checked against the disc's picture (mean difference at 32 x 32 printed, above --max-diff (15) left out: not the same texture or a mod's repaint).
Pack and disc files never go into the repository; this tool only reads and writes the paths given."""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_graft as G  # noqa: E402
import skin_hair_fbx as H  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

TPL_MAGIC = b"\x00\x20\xaf\x30"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc")
    ap.add_argument("out")
    ap.add_argument("--packs", required=True)
    ap.add_argument("--id", default="0x01000008", help="the pack's id (0x01000000 + the player file's number)")
    ap.add_argument("--tpls", default="1,13")
    ap.add_argument("--skip", default="", help="TPL:IMAGE,... left as the disc's (a pack from a mod: pl08's shirt "
                    "in its navy, 1:5)")
    ap.add_argument("--max-diff", type=float, default=15.0, help="the pack's picture only if it differs this little "
                    "from the disc's (mean at 32 x 32; pl00's pack image 5 is an eye where the disc has the jacket's "
                    "back, pl08's shirt a navy repaint)")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--max", type=int, default=1024)
    a = ap.parse_args(argv)
    arc = U.Archive(open(a.disc, "rb").read(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    packs = U.Packs(a.packs)
    pid = int(a.id, 0)
    want = {int(x) for x in a.tpls.split(",") if x}
    skip = {tuple(int(v) for v in x.split(":")) for x in a.skip.split(",") if x}
    n = 0
    for e, blob in enumerate(entries):
        if not blob or blob[:4] != TPL_MAGIC:
            continue
        imgs = G.tpl_images(blob)
        if e in want:
            changed = False
            for k, (hdr, data) in enumerate(imgs):
                h, w, f = struct.unpack_from(">HHI", hdr)
                pic = packs.image(pid, n + k)
                if f != 0xE or pic is None or (e, k) in skip:
                    continue
                pw, ph = pic.size
                if pw * h != ph * w:
                    print("tpl %d image %d: pack %d is %dx%d, the disc's %dx%d: left" % (e, k, n + k, pw, ph, w, h))
                    continue
                old = U.decode_gx(h, w, f, data).convert("RGB")
                diff = np.abs(np.asarray(old.resize((32, 32), Image.BOX), dtype=float) -
                              np.asarray(pic.convert("RGB").resize((32, 32), Image.BOX), dtype=float)).mean()
                if diff > a.max_diff:
                    print("tpl %d image %d: pack %d differs by %.0f: left" % (e, k, n + k, diff))
                    continue
                s = min(a.scale, a.max // max(w, h), pw // w) or 1
                img = pic.convert("RGBA").resize((w * s, h * s), Image.LANCZOS)
                levels = [img] + [img.resize(sz, Image.BOX) for sz in H.mip_sizes(*img.size)[1:]]
                imgs[k] = H.encode_levels(levels, 0xE, hdr, H.enc_cmpr)
                changed = True
                print("tpl %d image %d: %dx%d -> %dx%d from pack %d (%dx%d, difference %.1f)" %
                      (e, k, w, h, w * s, h * s, n + k, pw, ph, diff))
            if changed:
                entries[e] = G.build_tpl(imgs)
        n += len(imgs)
    body = U.build_body(entries, arc.types)
    data = U.build_archive(arc, body)
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's places hold %d)" % (a.out, len(data), len(body), 0x800000))


if __name__ == "__main__":
    main()
