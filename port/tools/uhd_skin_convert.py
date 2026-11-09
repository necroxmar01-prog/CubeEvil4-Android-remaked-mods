#!/usr/bin/env python3
"""Character skins of the PC release (RE4 UHD, Steam 2014) for the port: a player archive of the
PC (em/plXX.udas, little-endian, float vertices, textures in BIO4/imagePackHD) becomes one the
game reads from the GameCube discs (em/plXX.drs or .das, big-endian, GX display lists, textures
in the archive). Wish of DevJunior, 08.11.2026; analysis: the project note on skins.

    port/tools/uhd_skin_convert.py GC_ARCHIVE PC_ARCHIVE OUT [--packs DIR] [--tex 128]

GC_ARCHIVE  the disc's own file (em/pl00.drs of the debug discs, em/pl00.das of the releases):
            it gives the frame (part table, the sound bank of part 4) and every entry the PC file
            has no use for here (motions FCV/SEQ, effects EFF: the skeleton is the same).
PC_ARCHIVE  the mod's em/plXX.udas, or .udas.lfs (unpacked with port/tools/re4lfs.py).
OUT         the new file; copy it to game/mods/<name>/em/ with the disc's file name.
--packs     the mod's BIO4/imagePackHD folder: the textures, scaled down (.pack.lfs: unpack them
            with port/tools/re4lfs.py first); a palette none of whose textures are there (the PC
            game's own) stays the disc's; single missing textures become grey stand-ins.

What changes per model (BIN, game/model.cpp cModelData, include/model.h):
- vertices: float x/y/z -> s16 with the model's shift; normals float -> s8 (palette up to 255
  entries) or s16; texture coordinates float -> s16/8; positions, normals, colours and texture
  coordinates are each stored once (the PC file repeats every vertex per strip);
- display lists: the PC's runs over consecutive vertices (u32 count, then u16 type 6 strip /
  8 quads / 5 triangles and u16 vertex count) -> GX lists (0x98 / 0x80 / 0x90, index16 position,
  normal, colour when flags bit 31, texture coordinate);
- skinning weights: one per vertex on the PC (thousands); merged to at most --palette entries
  (rates rounded to coarser steps until they fit), the game's cWeight (<= 255) or cWeightExt
  (more: the port's locked cache must hold them, patch 0901);
- parts records, blend table: byte order (the PC keeps the x-flip table big-endian already).
The skeleton must be the disc's (checked: same parts, parents and positions).

The files are the player's (from the PC game and the mod): this tool only reads them; nothing of
them belongs in the repository or the app.
"""

import argparse
import io
import math
import os
import struct
import sys

# ---- archives (.das/.drs/.udas): part table at 0x20, part 0 is a DAT archive -------------------


class Archive:
    """The part table (32-byte records from 0x20: type, size, 0, offset; 0xFFFFFFFF ends) and the
    DAT body: u32 count, 12 bytes, u32 offsets[count], 4-character types[count]."""

    def __init__(self, data, e):
        self.data, self.e = data, e
        self.parts = []
        for i in range(31):
            t, size, x, off = struct.unpack_from(e + "4I", data, 0x20 + i * 0x20)
            if t == 0xFFFFFFFF:
                break
            self.parts.append([t, size, x, off, data[0x20 + i * 0x20 + 0x10:0x20 + i * 0x20 + 0x20]])
        t, size, _, off, _ = self.parts[0]
        if t != 0:
            raise ValueError("part 0 is not the data archive")
        self.body = data[off:off + size]
        n = struct.unpack_from(e + "I", self.body, 0)[0]
        self.offsets = list(struct.unpack_from(e + "%dI" % n, self.body, 16))
        self.types = [self.body[16 + n * 4 + i * 4:16 + n * 4 + i * 4 + 4] for i in range(n)]

    def entry(self, i):
        start = self.offsets[i]
        stop = self.offsets[i + 1] if i + 1 < len(self.offsets) else len(self.body)
        if start == 0 or start >= stop:
            return b""  # an empty entry: its offset is where the next one starts
        return self.body[start:stop]

    def kind(self, i):
        return self.types[i].rstrip(b"\0").decode("latin1").upper()


def align(n, a=32):
    return (n + a - 1) // a * a


def build_body(entries, types):
    """A DAT body (big-endian) from the entries: count, offsets, types, data 32-aligned; an empty
    entry points where the next one starts, as on the disc."""
    n = len(entries)
    head = align(16 + n * 8)
    out = bytearray(head)
    struct.pack_into(">I", out, 0, n)
    for i, (blob, t) in enumerate(zip(entries, types)):
        out[16 + n * 4 + i * 4:16 + n * 4 + i * 4 + 4] = t
        struct.pack_into(">I", out, 16 + i * 4, len(out))
        if not blob:
            continue
        out += blob
        out += bytes(align(len(out)) - len(out))
    return bytes(out)


def build_archive(gc, body):
    """The disc file's frame with the new body: filler, part table, body at 0x400, the other
    parts (Leon's sound bank, part 4) behind it as on the disc."""
    out = bytearray(gc.data[:0x20])
    table = bytearray(0x3E0)
    pos = 0x400 + align(len(body))
    rest = []
    for i, (t, size, x, off, tail) in enumerate(gc.parts):
        if i == 0:
            rec_off, rec_size = 0x400, len(body)
        else:
            rec_off, rec_size = pos, size
            rest.append(gc.data[off:off + size])
            pos = align(pos + size)
        struct.pack_into(">4I", table, i * 0x20, t, rec_size, x, rec_off)
        table[i * 0x20 + 0x10:i * 0x20 + 0x20] = tail
    struct.pack_into(">I", table, len(gc.parts) * 0x20, 0xFFFFFFFF)
    out += table
    out += body
    out += bytes(align(len(out)) - len(out))
    for blob in rest:
        out += blob
        out += bytes(align(len(out)) - len(out))
    return bytes(out)


# ---- models -----------------------------------------------------------------------------------

PRIM = {6: 0x98, 8: 0x80, 5: 0x90}  # PC run type -> GX primitive (strip, quads, triangles)


class Model:
    """A PC model (little-endian, the fields of cModelData)."""

    def __init__(self, blob):
        self.b = blob
        u = lambda o: struct.unpack_from("<I", blob, o)[0]
        h = lambda o: struct.unpack_from("<H", blob, o)[0]
        self.head, self.x04, self.x08 = u(0), u(4), u(8)
        self.clr, self.tex, self.wgt = u(0xC), u(0x10), u(0x14)
        self.wpal, self.nparts, self.nlist = blob[0x18], blob[0x19], h(0x1A)
        self.parts, self.flags, self.ntex = u(0x1C), u(0x20), u(0x24)
        self.shift, self.x29, self.wext = blob[0x28], blob[0x29], h(0x2A)
        self.shape, self.vtx, self.nrm = u(0x2C), u(0x30), u(0x34)
        self.nvtx, self.nnrm, self.version = h(0x38), h(0x3A), u(0x3C)
        self.blend = u(0x40) if self.version == 0x20030818 else 0
        self.flip = u(0x44) if self.version == 0x20030818 else 0
        self.vidx = u(0x48) if self.version == 0x20030818 else 0  # per vertex: weight entry (u16)
        self.nidx = u(0x4C) if self.version == 0x20030818 else 0  # per normal: weight entry (u16)
        if self.version not in (0x20010801, 0x20030817, 0x20030818):
            raise ValueError("not a model (version %#x)" % self.version)
        if self.shape:
            raise ValueError("shape (morph) tables are not converted yet")
        if self.nvtx != self.nnrm:
            raise ValueError("vertices %d, normals %d: the PC layout has one of each" % (self.nvtx, self.nnrm))

    def floats(self, off, n, k):
        return [struct.unpack_from("<%df" % k, self.b, off + i * 4 * k) for i in range(n)]

    def runs(self):
        """(part header bytes 0..0x17, nPoly, [(type, count)]) per part."""
        p = self.parts
        for _ in range(self.nlist):
            size, npoly = struct.unpack_from("<II", self.b, p + 0x18)
            cnt = struct.unpack_from("<I", self.b, p + 0x20)[0] if size >= 4 else 0
            runs = [struct.unpack_from("<HH", self.b, p + 0x24 + i * 4) for i in range(cnt)]
            yield self.b[p:p + 0x18], npoly, runs
            p += 0x20 + size

    def weights(self):
        n = self.wext if self.wext > 0xFF else self.wpal
        out = []
        for i in range(n):
            w = self.b[self.wgt + i * 8:self.wgt + i * 8 + 8]
            k = min(w[3], 3)
            out.append(tuple((w[j], w[4 + j]) for j in range(k)))
        return out

    def bones(self):
        out = []
        for i in range(self.nparts):
            r = self.b[self.head + i * 16:self.head + i * 16 + 16]
            out.append((r[0:4], struct.unpack_from("<3f", r, 4)))
        return out


def merge_weights(entries, cap):
    """Merges the weight entries to at most `cap`: rates (percent) rounded to steps of 1, 5, 10,
    ... until they fit; influences rounded to 0 go, the rest is scaled back to 100. Returns the new
    entries and, per old entry, its new index."""
    for step in (1, 2, 5, 10, 15, 20, 25, 34, 50, 100):
        index, new, remap = {}, [], []
        for e in entries:
            if not e:
                key = ((0, 100),)
            else:
                infl = [(b, max(0, int(round(w / step)) * step)) for b, w in e]
                infl = [(b, w) for b, w in infl if w > 0] or [max(e, key=lambda x: x[1])[:1] + (100,)]
                total = sum(w for _, w in infl)
                infl = sorted(((b, w * 100.0 / total) for b, w in infl), key=lambda x: -x[1])
                rates = [int(round(w)) for _, w in infl]
                rates[0] += 100 - sum(rates)
                key = tuple(sorted(zip((b for b, _ in infl), rates)))
            if key not in index:
                index[key] = len(new)
                new.append(key)
            remap.append(index[key])
        if len(new) <= cap:
            return new, remap, step
    raise ValueError("weights do not fit %d entries" % cap)


def quant(v, scale, lo=-32768, hi=32767):
    return max(lo, min(hi, int(round(v * scale))))


def convert_model(blob, cap, stats):
    m = Model(blob)
    nv = m.nvtx
    pos = m.floats(m.vtx, nv, 3)
    nrm = [tuple(c / (math.hypot(*n) or 1.0) for c in n) for n in m.floats(m.nrm, nv, 3)]  # unit length
    if m.flags & 0x80000000:
        uv = m.floats(m.tex, nv, 2)
        clr = [blob[m.clr + i * 4:m.clr + i * 4 + 4] for i in range(nv)]
        uv_scale, uv_lo, uv_hi = 256.0, -32768, 32767  # s16, 8 fraction bits
    else:
        uv = m.floats(m.tex, nv, 2)
        clr = None
        uv_scale, uv_lo, uv_hi = 32768.0, 0, 65535  # u16, 15 fraction bits
    vw = struct.unpack_from("<%dH" % nv, blob, m.vidx) if m.vidx else (0,) * nv
    weights = m.weights()
    skinned = len(weights) > 1 or m.nparts > 1
    if weights:
        merged, remap, step = merge_weights(weights, cap)
    else:
        merged, remap, step = [((0, 100),)], [0], 1
    vw = [remap[w] if w < len(remap) else 0 for w in vw]
    s8 = len(merged) <= 0xFF
    # the model's shift: as fine as the largest coordinate allows
    big = max([abs(c) for p in pos for c in p] + [1.0])
    shift = max(0, min(15, int(math.floor(math.log2(32767.0 / big)))))
    if m.shift and big * (1 << m.shift) <= 32767:
        shift = m.shift  # the PC model's own, when everything fits it
    ps = float(1 << shift)

    def unique(items):
        index, out, ref = {}, [], []
        for it in items:
            if it not in index:
                index[it] = len(out)
                out.append(it)
            ref.append(index[it])
        return out, ref

    P, pref = unique([(quant(x, ps), quant(y, ps), quant(z, ps), w) for (x, y, z), w in zip(pos, vw)])
    if s8:
        N, nref = unique([(quant(x, 64, -128, 127), quant(y, 64, -128, 127), quant(z, 64, -128, 127), w)
                          for (x, y, z), w in zip(nrm, vw)])
    else:
        N, nref = unique([(quant(x, 16384), quant(y, 16384), quant(z, 16384), w) for (x, y, z), w in zip(nrm, vw)])
    T, tref = unique([(quant(u, uv_scale, uv_lo, uv_hi), quant(v, uv_scale, uv_lo, uv_hi)) for u, v in uv])
    C, cref = unique(clr) if clr else ([], None)
    if max(len(P), len(N)) > 0xFFFF:
        raise ValueError("more than 65535 vertices after merging (%d)" % len(P))

    flags = m.flags & ~0x20000000 | (0x20000000 if s8 else 0)
    out = bytearray(0x60)
    sec = {}

    def put(name, data, a=32):
        out.extend(bytes(align(len(out), a) - len(out)))
        sec[name] = len(out)
        out.extend(data)

    # parts records: the bytes x0 as they are, the position big-endian
    head = bytearray()
    for raw, (x, y, z) in m.bones():
        head += raw + struct.pack(">3f", x, y, z)
    put("head", head, 16)
    put("vtx", b"".join(struct.pack(">3hH", *p) for p in P))
    if s8:
        put("nrm", b"".join(struct.pack(">3bB", *n) for n in N))
    else:
        put("nrm", b"".join(struct.pack(">3hH", *n) for n in N))
    if C:
        put("clr", b"".join(C))
    fmt = ">hh" if clr is not None else ">HH"
    put("tex", b"".join(struct.pack(fmt, *t) for t in T))
    if len(merged) <= 0xFF:
        wb = bytearray()
        for e in merged:
            ids = [b for b, _ in e] + [0] * (3 - len(e))
            rates = [w for _, w in e] + [0] * (3 - len(e))
            wb += struct.pack(">4B4B", ids[0], ids[1], ids[2], len(e), rates[0], rates[1], rates[2], 0)
    else:
        wb = bytearray()
        for e in merged:
            ids = [b for b, _ in e] + [0] * (3 - len(e))
            rates = [w for _, w in e] + [0] * (3 - len(e))
            wb += struct.pack(">4H4B", ids[0], ids[1], ids[2], len(e), rates[0], rates[1], rates[2], 0)
    put("wgt", wb)
    # display lists
    parts = bytearray()
    first = 0
    per = 4 if C else 3
    total_polys = 0
    for raw, npoly, runs in m.runs():
        stream = bytearray()
        for typ, cnt in runs:
            op = PRIM.get(typ)
            if op is None:
                raise ValueError("unknown primitive type %d" % typ)
            stream += struct.pack(">BH", op, cnt)
            for v in range(first, first + cnt):
                if C:
                    stream += struct.pack(">4H", pref[v], nref[v], cref[v], tref[v])
                else:
                    stream += struct.pack(">3H", pref[v], nref[v], tref[v])
            first += cnt
        stream += bytes(align(len(stream)) - len(stream))
        parts += raw + struct.pack(">II", len(stream), npoly)
        parts += stream
        total_polys += npoly
    if first != nv:
        raise ValueError("the runs use %d of %d vertices" % (first, nv))
    put("parts", parts)
    if m.blend:
        n = struct.unpack_from("<I", blob, m.blend)[0]
        put("blend", struct.pack(">I", n) + struct.pack(">%dH" % (n * 4), *struct.unpack_from("<%dH" % (n * 4), blob, m.blend + 4)), 4)
    if m.flip:
        # the PC keeps the x-flip table big-endian, as on the disc
        n = struct.unpack_from(">I", blob, m.flip)[0]
        put("flip", blob[m.flip:m.flip + 4 + n * 2], 4)
    out.extend(bytes(align(len(out)) - len(out)))
    hdr = struct.pack(">3I3I", sec["head"], m.x04, 0x50 if m.version == 0x20030818 else m.x08,
                      sec.get("clr", 0), sec["tex"], sec["wgt"])
    hdr += struct.pack(">BBH", len(merged) & 0xFF, m.nparts, m.nlist)
    hdr += struct.pack(">III", sec["parts"], flags, m.ntex)
    hdr += struct.pack(">BBH", shift, m.x29, len(merged))
    hdr += struct.pack(">III", 0, sec["vtx"], sec["nrm"])
    hdr += struct.pack(">HHI", len(P), len(N), m.version)
    if m.version == 0x20030818:
        hdr += struct.pack(">II", sec.get("blend", 0), sec.get("flip", 0))
    out[0:len(hdr)] = hdr
    stats.append("%d vertices (PC %d), %d normals, %d texture coordinates, %d polygons, weights %d -> %d "
                 "(step %d%%), shift %d, %s normals, %d bytes (PC %d)" %
                 (len(P), nv, len(N), len(T), total_polys, len(weights), len(merged), step, shift,
                  "s8" if s8 else "s16", len(out), len(blob)))
    return bytes(out), m


def verify_model(gc, m):
    """Reads the converted model back (as the game does: GX lists, index16 attributes) and compares
    every drawn vertex with the PC's: largest position error (units), normal angle error (degrees),
    texture coordinate error; the skinning entry of each vertex must keep its strongest bone."""
    u = lambda o: struct.unpack_from(">I", gc, o)[0]
    h = lambda o: struct.unpack_from(">H", gc, o)[0]
    flags, shift, nlist = u(0x20), gc[0x28], h(0x1A)
    s8 = bool(flags & 0x20000000)
    clr = bool(flags & 0x80000000)
    vtx, nrm, tex, wgt, parts = u(0x30), u(0x34), u(0x10), u(0x14), u(0x1C)
    nw = h(0x2A)
    ext = nw > 0xFF
    pos = m.floats(m.vtx, m.nvtx, 3)
    nrmf = m.floats(m.nrm, m.nvtx, 3)
    uv = m.floats(m.tex, m.nvtx, 2)
    pcw = m.weights()
    vw = struct.unpack_from("<%dH" % m.nvtx, m.b, m.vidx) if m.vidx else (0,) * m.nvtx
    worst = [0.0, 0.0, 0.0, 0]
    p = parts
    v = 0
    for _ in range(nlist):
        size = u(p + 0x18)
        s, e = p + 0x20, p + 0x20 + size
        while s < e and gc[s] != 0:
            cnt = struct.unpack_from(">H", gc, s + 1)[0]
            s += 3
            for _ in range(cnt):
                if clr:
                    pi, ni, ci, ti = struct.unpack_from(">4H", gc, s)
                    s += 8
                else:
                    pi, ni, ti = struct.unpack_from(">3H", gc, s)
                    s += 6
                x, y, z, w = struct.unpack_from(">3hH", gc, vtx + pi * 8)
                worst[0] = max(worst[0], math.dist((x / (1 << shift), y / (1 << shift), z / (1 << shift)), pos[v]))
                if s8:
                    n = [c / 64.0 for c in struct.unpack_from(">3b", gc, nrm + ni * 4)]
                else:
                    n = [c / 16384.0 for c in struct.unpack_from(">3h", gc, nrm + ni * 8)]
                a, b = math.hypot(*n), math.hypot(*nrmf[v])
                if a > 0 and b > 0:  # the PC has a few zero normals; they stay zero
                    cosv = max(-1.0, min(1.0, sum(i * j for i, j in zip(n, nrmf[v])) / (a * b)))
                    worst[1] = max(worst[1], math.degrees(math.acos(cosv)))
                tu, tv = struct.unpack_from(">hh" if clr else ">HH", gc, tex + ti * 4)
                sc = 256.0 if clr else 32768.0
                worst[2] = max(worst[2], abs(tu / sc - uv[v][0]), abs(tv / sc - uv[v][1]))
                if pcw and len(pcw) > 1:
                    if ext:
                        ids = struct.unpack_from(">3HH", gc, wgt + w * 12)
                        rates = gc[wgt + w * 12 + 8:wgt + w * 12 + 11]
                    else:
                        ids = tuple(gc[wgt + w * 8:wgt + w * 8 + 4])
                        rates = gc[wgt + w * 8 + 4:wgt + w * 8 + 7]
                    top_gc = ids[max(range(ids[3]), key=lambda k: rates[k])]
                    src = pcw[vw[v]]
                    if src and top_gc not in [b for b, r in src if r == max(r2 for _, r2 in src)]:
                        worst[3] += 1
                v += 1
        p = e
    if v != m.nvtx:
        raise ValueError("read back %d of %d vertices" % (v, m.nvtx))
    return worst


# ---- textures -------------------------------------------------------------------------------

class Packs:
    """BIO4/imagePackHD/*.pack: u32 id, u32 count, u32 offsets[count]; each entry a 16-byte
    record, then a DDS file."""

    def __init__(self, folder):
        self.packs = {}
        if not folder:
            return
        for name in sorted(os.listdir(folder)):
            if not name.lower().endswith(".pack"):
                continue
            path = os.path.join(folder, name)
            with open(path, "rb") as f:
                head = f.read(8)
                if len(head) < 8:
                    continue
                pid, n = struct.unpack("<II", head)
                offs = struct.unpack("<%dI" % n, f.read(4 * n))
            self.packs[pid] = (path, offs)

    def find(self, ref):
        if ref in self.packs:
            return self.packs[ref]
        same = [p for k, p in self.packs.items() if k >> 24 == ref >> 24]
        return same[0] if len(same) == 1 else None

    def image(self, ref, idx):
        found = self.find(ref)
        if not found or idx >= len(found[1]):
            return None
        path, offs = found
        start = offs[idx] + 16
        stop = next((o for o in sorted(offs) if o > offs[idx]), os.path.getsize(path))
        with open(path, "rb") as f:
            f.seek(start)
            dds = f.read(stop - start)
        try:
            from PIL import Image
            img = Image.open(io.BytesIO(dds))
            img.load()
            return img.convert("RGBA")
        except Exception as e:  # a cut pack or an unknown DDS kind: a stand-in
            print("  texture %#x/%d: %s" % (ref, idx, e), file=sys.stderr)
            return None


def blocks(w, h, bw, bh):
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            yield bx, by


def enc_cmpr(px, w, h):
    """GX CMPR: 8x8 tiles of four DXT1 blocks (BC1 big-endian colours, 2-bit indices MSB first)."""
    def c565(c):
        return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)

    out = bytearray()
    for tx, ty in blocks(w, h, 8, 8):
        for sx, sy in ((0, 0), (4, 0), (0, 4), (4, 4)):
            cols = [px[(ty + sy + y) * w + tx + sx + x] for y in range(4) for x in range(4)]
            transparent = any(c[3] < 128 for c in cols)
            opaque = [c for c in cols if c[3] >= 128] or cols
            lum = lambda c: c[0] * 3 + c[1] * 6 + c[2]
            a, b = max(opaque, key=lum), min(opaque, key=lum)
            c0, c1 = c565(a), c565(b)
            if transparent:
                if c0 > c1:
                    c0, c1 = c1, c0
            else:
                if c0 < c1:
                    c0, c1 = c1, c0
                if c0 == c1:
                    c1 = c0 - 1 if c0 else 0
                    if c0 == 0:
                        c0 = 1
            def rgb(c):
                return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
            p0, p1 = rgb(c0), rgb(c1)
            if c0 > c1:
                pal = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)),
                       tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
                n = 4
            else:
                pal = [p0, p1, tuple((x + y) // 2 for x, y in zip(p0, p1))]
                n = 3
            bits = 0
            for c in cols:
                if transparent and c[3] < 128:
                    k = 3
                else:
                    k = min(range(n), key=lambda i: sum((c[j] - pal[i][j]) ** 2 for j in range(3)))
                bits = bits << 2 | k
            out += struct.pack(">HHI", c0, c1, bits)
    return bytes(out)


def enc_i8(px, w, h):
    out = bytearray()
    for bx, by in blocks(w, h, 8, 4):
        for y in range(4):
            for x in range(8):
                c = px[(by + y) * w + bx + x]
                out.append((c[0] * 3 + c[1] * 6 + c[2]) // 10)
    return bytes(out)


def enc_ia8(px, w, h):
    out = bytearray()
    for bx, by in blocks(w, h, 4, 4):
        for y in range(4):
            for x in range(4):
                c = px[(by + y) * w + bx + x]
                out += bytes((c[3], (c[0] * 3 + c[1] * 6 + c[2]) // 10))
    return bytes(out)


ENCODERS = {0xE: (0xE, enc_cmpr), 0x0: (0x1, enc_i8), 0x1: (0x1, enc_i8), 0x3: (0x3, enc_ia8)}


def tpl_found(blob, packs):
    """How many of a PC palette's textures the packs at hand hold."""
    n = struct.unpack_from("<I", blob, 4)[0]
    found = 0
    for i in range(n):
        ho = struct.unpack_from("<I", blob, 12 + i * 8)[0]
        data = struct.unpack_from("<I", blob, ho + 8)[0]
        if data + 8 <= len(blob) and packs and packs.find(struct.unpack_from("<I", blob, data)[0]):
            found += 1
    return found


def convert_tpl(blob, packs, size, stats):
    """A PC texture palette (references to imagePackHD) -> a GameCube TPL with the images,
    scaled to at most `size` pixels a side (grey 8x8 stand-ins for missing ones)."""
    n = struct.unpack_from("<I", blob, 4)[0]
    images = []
    for i in range(n):
        ho = struct.unpack_from("<I", blob, 12 + i * 8)[0]
        h, w, fmt, data, ws, wt, mn, mg, bias = struct.unpack_from("<HHIIIIIIf", blob, ho)
        ref, idx = struct.unpack_from("<II", blob, data) if data + 8 <= len(blob) else (0, 0)
        img = packs.image(ref, idx) if packs else None
        gx, enc = ENCODERS.get(fmt, (0xE, enc_cmpr))
        if img is None:
            from PIL import Image
            img = Image.new("RGBA", (8, 8), (128, 128, 128, 255))
            missing = True
        else:
            missing = False
        scale = min(1.0, size / float(max(img.size)))
        tw = max(8, 1 << int(round(math.log2(max(1, img.size[0] * scale)))))
        th = max(8, 1 << int(round(math.log2(max(1, img.size[1] * scale)))))
        img = img.resize((tw, th))
        raw = img.tobytes()
        px = [tuple(raw[k:k + 4]) for k in range(0, len(raw), 4)]
        images.append((th, tw, gx, enc(px, tw, th), ws, wt, mn, mg, bias))
        stats.append("  texture %d: %s %dx%d -> %dx%d format %#x" %
                     (i, "missing, grey" if missing else "%#x/%d" % (ref, idx), w, h, tw, th, gx))
    head = 12 + n * 8
    hdrs = align(head + n * 0x24)
    out = bytearray(hdrs)
    struct.pack_into(">III", out, 0, 0x0020AF30, n, 12)
    for i, (h, w, gx, data, ws, wt, mn, mg, bias) in enumerate(images):
        out += bytes(align(len(out)) - len(out))
        struct.pack_into(">II", out, 12 + i * 8, head + i * 0x24, 0)
        struct.pack_into(">HHIIIIIIfBBBB", out, head + i * 0x24, h, w, gx, len(out), ws, wt, 1, 1, bias, 0, 0, 0, 0)
        out += data
    return bytes(out)


# ---- the archive -------------------------------------------------------------------------------

def check_skeleton(gc_model, pc_model):
    gb = []
    for i in range(gc_model[0x19]):
        r = gc_model[0x60 + i * 16:0x60 + i * 16 + 16]
        gb.append((r[0], r[1], struct.unpack_from(">3f", r, 4)))
    pb = [(raw[0], raw[1], xyz) for raw, xyz in Model(pc_model).bones()]
    if len(gb) != len(pb):
        return "parts %d (disc) and %d (PC)" % (len(gb), len(pb))
    for i, (a, b) in enumerate(zip(gb, pb)):
        if a[:2] != b[:2] or max(abs(x - y) for x, y in zip(a[2], b[2])) > 0.5:
            return "part %d differs (disc %s, PC %s)" % (i, a, b)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gc")
    ap.add_argument("pc")
    ap.add_argument("out")
    ap.add_argument("--packs", help="the mod's BIO4/imagePackHD folder")
    ap.add_argument("--tex", type=int, default=128, help="largest texture side (default 128)")
    ap.add_argument("--palette", type=int, default=2048,
                    help="most skinning matrices per model (default 2048: the port's locked cache holds "
                         "RE4_LOCKED_CACHE_SIZE / 0x30; the GameCube's held 341; above 255 the normals are s16)")
    ap.add_argument("--keep", default="", help="comma-separated entries to take from the disc file unchanged")
    ap.add_argument("--verify", action="store_true", help="read every model back and compare with the PC's")
    a = ap.parse_args(argv)

    gc = Archive(open(a.gc, "rb").read(), ">")
    pcdata = open(a.pc, "rb").read()
    if pcdata[:4] == b"RDLX":  # .lfs: the PC's compression
        import re4lfs
        pcdata = re4lfs.unpack(pcdata)
    pc = Archive(pcdata, "<")
    if len(gc.offsets) != len(pc.offsets):
        sys.exit("the archives differ: %d entries on the disc, %d in the PC file" % (len(gc.offsets), len(pc.offsets)))
    keep = {int(x) for x in a.keep.split(",") if x}
    packs = Packs(a.packs)
    if gc.kind(0) == "BIN" and pc.kind(0) == "BIN":
        why = check_skeleton(gc.entry(0), pc.entry(0))
        if why:
            sys.exit("the skeletons differ: %s; the disc's motions would not fit this model" % why)
    entries = []
    for i in range(len(gc.offsets)):
        kind_gc, kind_pc = gc.kind(i), pc.kind(i)
        blob_pc = pc.entry(i)
        if kind_gc != kind_pc:
            sys.exit("entry %d: %s on the disc, %s in the PC file" % (i, kind_gc, kind_pc))
        if i in keep or not blob_pc:
            entries.append(gc.entry(i))
            continue
        stats = []
        if kind_pc == "BIN":
            blob, model = convert_model(blob_pc, a.palette, stats)
            print("entry %3d BIN: %s" % (i, stats[0]))
            if a.verify:
                pe, ne, te, wb = verify_model(blob, model)
                print("          read back: position error %.3f, normal %.1f degrees, texture %.4f, %d vertices "
                      "with another main bone" % (pe, ne, te, wb))
        elif kind_pc == "TPL" and tpl_found(blob_pc, packs) == 0 and gc.entry(i):
            # textures of the PC game itself (its BIO4/imagePackHD, not the mod's): the disc's own
            blob = gc.entry(i)
            print("entry %3d TPL: none of its textures in the mod's packs, the disc's" % i)
        elif kind_pc == "TPL":
            blob = convert_tpl(blob_pc, packs, a.tex, stats)
            print("entry %3d TPL: %d textures, %d bytes" % (i, len(stats), len(blob)))
            for s in stats:
                print(s)
        else:
            blob = gc.entry(i)  # motions, effects: the disc's (the skeleton is the same)
        entries.append(blob)
    body = build_body(entries, gc.types)
    data = build_archive(gc, body)
    with open(a.out, "wb") as f:
        f.write(data)
    print("%s: %d bytes, data part %d bytes (disc %d; the game's place for Leon holds %d)" %
          (a.out, len(data), len(body), len(gc.body), 0x118000))


if __name__ == "__main__":
    main()
