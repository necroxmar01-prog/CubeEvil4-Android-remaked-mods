#!/usr/bin/env python3
"""The cutscenes' face movements for a skin's head (wish of DevJunior, 10.11.2026: "the skins should move the
mouth"): a cutscene draws Leon with its own head, ev0001 (event/model/ev0000/ev0001.bin, in every event file), whose
expressions are shapes (vertex morphs, game/shape.cpp): 28 of them, the mouth (1..8), the eyes (9..13), the brows
(14..18), cheeks, nose and the rest (19..28). Patch 0906 draws a skin's head instead (archive part 6, entry 2) and
patch 0907 plays the event's shape animation on it, by the same indexes. This tool gives the skin's head those
shapes:

    skin_shapes.py SKIN.drs OUT.drs --event r100s03.evd[.lfs] [--entry 2] [--preview DIR]

Each shape is carried over from the event's head to the skin's by the nearest points: the two faces are put on each
other first (heights piecewise by chin, mouth, nose tip, eyes and brow; depth by the front profile at that height, so
a Remake head with its lips further forward takes the lips' movement and not the teeth's), near the mouth an upper
lip point takes only upper lip points and a lower one only lower ones (else an open mouth would pull both lips
alike); the four nearest, weighted by inverse squared distance, faded out from 5 to 12 units away. The table holds
the event's 29 slots (slot 0 empty, as in ev0001), deltas in the head's own shift; the head's model gets its shape
table (header 0x2C) appended. A head that already had shapes loses them (the game's expressions in play are the
face's, entry 4, not the head's). The event file may be the PC release's (little-endian, floats) or the disc's.

Disc and mod files never go into the repository; this tool only reads and writes the paths given."""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402
import skin_graft as G  # noqa: E402

EVENT_HEAD = b"event/model/ev0000/ev0001.bin\x00"
VERSIONS = (0x20030818, 0x20030817, 0x20010801)


def event_head(path):
    """Positions (n x 3, model units) and shapes (list of {vertex: (dx, dy, dz)} in model units) of the event's head
    ev0001, from an event archive (.evd, or .evd.lfs of the PC release)."""
    data = Path(path).read_bytes()
    if path.lower().endswith(".lfs"):
        import re4lfs
        data = re4lfs.unpack(data)
    at = 0
    while True:
        at = data.find(EVENT_HEAD, at)
        if at < 0:
            raise ValueError("%s: no %s in its file table" % (path, EVENT_HEAD[:-1].decode()))
        for e in "<>":
            off, size = struct.unpack_from(e + "II", data, at + 0x30)
            if 0 < off and off + size <= len(data) and size > 0x50 and \
                    struct.unpack_from(e + "I", data, off + 0x3C)[0] in VERSIONS:
                blob = data[off:off + size]
                return (pc_head(blob) if e == "<" else gc_head(blob)) + (e,)
        at += 1


def pc_head(blob):
    m = U.Model(blob)
    P = np.array(m.floats(m.vtx, m.nvtx, 3), float)
    sc = float(1 << m.shift)
    shapes = [{v: tuple(c / sc for c in d) for v, d in s.items()} for s in m.shapes]
    return P, shapes


def gc_head(blob):
    P = G.positions(blob)
    sc = float(1 << blob[0x28])
    so = struct.unpack_from(">I", blob, 0x2C)[0]
    shapes = []
    if so:
        n = struct.unpack_from(">I", blob, so)[0]
        for k in range(n):
            ofs, cnt = struct.unpack_from(">Ii", blob, so + 4 + k * 8)
            d = {}
            for j in range(cnt):
                v, dx, dy, dz = struct.unpack_from(">Hhhh", blob, so + 4 + ofs + j * 8)
                d[v] = (dx / sc, dy / sc, dz / sc)
            shapes.append(d)
    return P, shapes


def front_profile(P, lo, hi, half=3.0):
    """The face's front (largest z) per height y lo..hi along the middle (|x| < half), nan where none."""
    ys = np.arange(lo, hi + 1)
    sel = np.abs(P[:, 0]) < half
    z = np.full(len(ys), np.nan)
    for i, y in enumerate(ys):
        q = sel & (np.abs(P[:, 1] - y) < 1.5)
        if q.any():
            z[i] = P[q, 2].max()
    # fill gaps
    ok = np.isfinite(z)
    if ok.any():
        z = np.interp(ys, ys[ok], z[ok])
    return ys, z


def landmarks(P, mouth=None, opening=None):
    """Chin, mouth (the lips' parting), nose tip, eyes, brow and the forehead, as heights; the front profile for the
    depths. The mouth: given, or where the jaw-opening shape `opening` ({vertex: delta}) stops moving the front of
    the lips (between the highest moving lower lip point and the lowest still upper lip point), else 36 under the
    nose tip (the Remake's head 1652, the disc's 1653)."""
    mid = np.abs(P[:, 0]) < 4
    cand = mid & (P[:, 1] > 1640) & (P[:, 1] < 1740)
    nose = P[cand][P[cand][:, 2].argmax()]
    ny, nz = float(nose[1]), float(nose[2])
    ys, z = front_profile(P, ny - 110, ny + 110)
    front = z > nz - 45
    chin = float(ys[front & (ys < ny - 40)].min()) if (front & (ys < ny - 40)).any() else ny - 72
    if mouth is None and opening:
        lips = np.where((np.abs(P[:, 0]) < 6) & (P[:, 2] > nz - 24) & (P[:, 1] > ny - 50) & (P[:, 1] < ny - 15))[0]
        moving = np.array([v for v in lips if opening.get(v, (0, 0, 0))[1] < -5])
        if len(moving):
            low = float(P[moving, 1].max())
            still = [v for v in lips if abs(opening.get(v, (0, 0, 0))[1]) < 1 and P[v, 1] > low]
            if still:
                mouth = (low + float(min(P[v, 1] for v in still))) / 2
    if mouth is None:
        mouth = ny - 36
    return dict(chin=chin, mouth=mouth, nose=ny, eyes=ny + 34, brow=ny + 50, top=ny + 100), (ys, z)


def carry_shapes(S, E, shapes, k=4, near=5.0, far=12.0, lip_x=20.0, lip_band=14.0, skin_mouth=None,
                 eye_r=(18.0, 11.0), mouth_gain=1.0, threshold=0.2, slots=None):
    """The event's shapes (on positions E) carried onto the skin's positions S. Returns a list of
    {skin vertex: (dx, dy, dz)} in model units, one per shape slot."""
    from scipy.spatial import cKDTree
    ls, (sys_, sz) = landmarks(S, skin_mouth)
    le, (eys, ez) = landmarks(E, opening=shapes[1] if len(shapes) > 1 else None)
    keys = ("chin", "mouth", "nose", "eyes", "brow", "top")
    xe = [le[q] for q in keys]
    xs = [ls[q] for q in keys]
    # the event's heights on the skin's scale, and its depth by the front profiles at the same place
    ymap = lambda y: np.interp(y, xe, xs, left=None, right=None) + np.where(y < xe[0], y - xe[0], 0) + \
        np.where(y > xe[-1], y - xe[-1], 0)
    slope = np.diff(xs) / np.maximum(np.diff(xe), 1e-6)
    ys_e = ymap(E[:, 1])
    dz = np.interp(ys_e, sys_, sz) - np.interp(E[:, 1], eys, ez)
    dz = np.where(np.isfinite(dz), dz, 0.0)
    # only where the face is: the back of the head keeps its depth
    front = np.clip((E[:, 2] - 0.0) / 30.0, 0, 1)
    A = np.stack([E[:, 0], ys_e, E[:, 2] + dz * front], 1)
    m = ls["mouth"]
    ey = ls["eyes"]
    # groups that must not mix: the upper and the lower lip (between the mouth's corners), each eye's upper and lower
    # lid (an open mouth or a blink would else pull both alike and tear the skin's lips and lids)
    def group(Q):
        g = np.zeros(len(Q), int)
        lip = (np.abs(Q[:, 0]) < lip_x) & (np.abs(Q[:, 1] - m) < lip_band) & (Q[:, 2] > 30)
        g[lip] = np.where(Q[lip, 1] >= m, 1, 2)
        for side, base in ((1, 3), (-1, 5)):
            eye = ((Q[:, 0] * side - 28.0) / eye_r[0]) ** 2 + ((Q[:, 1] - ey) / eye_r[1]) ** 2 < 1.0
            eye &= Q[:, 2] > 30
            g[eye] = np.where(Q[eye, 1] >= ey, base, base + 1)
        return g
    ge, gs = group(A), group(S)
    idx = np.zeros((len(S), k), int)
    dist = np.zeros((len(S), k))
    full = cKDTree(A)
    for gi in np.unique(gs):
        sel = gs == gi
        if gi == 0:
            d, j = full.query(S[sel], k=k)
            idx[sel], dist[sel] = j, d
            continue
        back = np.where(ge == gi)[0]
        if len(back) < k:
            d, j = full.query(S[sel], k=k)
            idx[sel], dist[sel] = j, d
            continue
        d, j = cKDTree(A[back]).query(S[sel], k=k)
        idx[sel], dist[sel] = back[j], d
    w = 1.0 / (dist + 0.5) ** 2
    w /= w.sum(1, keepdims=True)
    fade = 1.0 - np.clip((dist[:, 0] - near) / (far - near), 0, 1)
    # the heights' scale where each skin position lies (a taller lower face stretches the jaw's drop)
    seg = np.clip(np.searchsorted(xs, S[:, 1]) - 1, 0, len(slope) - 1)
    yscale = slope[seg]
    # the skin's own neighbours (within 2.5 units) for smoothing each shape: inside a lip or lid group only its own
    # group, elsewhere (the mouth's corners among them) any, so the corners blend both lips instead of tearing
    stree = cKDTree(S)
    nb = stree.query_ball_point(S, 2.5)
    nb = [np.array([j for j in lst if gs[j] == gs[i] or gs[i] == 0], int) for i, lst in enumerate(nb)]
    mouth_zone = (np.abs(S[:, 0]) < 40) & (np.abs(S[:, 1] - m) < 22) & (S[:, 2] > 30)
    out = []
    for slot, sh in enumerate(shapes):
        if slots is not None and slot not in slots:
            out.append({})
            continue
        D = np.zeros((len(E), 3))
        for v, d in sh.items():
            if v < len(E):
                D[v] = d
        got = (w[:, :, None] * D[idx]).sum(1) * fade[:, None]
        got[:, 1] *= yscale
        # around the mouth: the skin's head has no mouth inside to show, a smaller opening reads better
        got[mouth_zone] *= mouth_gain
        moving = np.where(np.abs(got).max(1) > 0.01)[0]
        for _ in range(2):
            new = got.copy()
            for i in moving:
                if len(nb[i]):
                    new[i] = 0.5 * got[i] + 0.5 * got[nb[i]].mean(0)
            got = new
        keep = np.abs(got).max(1) > threshold
        out.append({int(v): tuple(got[v]) for v in np.where(keep)[0]})
    return out, ls, le


def shape_table(shapes, shift):
    """The GC shape table (game/shape.cpp): u32 count, count x (u32 offset from the table + 4, s32 entries), then the
    entries u16 vertex, s16 dx, dy, dz in 1 / (1 << shift)."""
    sc = float(1 << shift)
    lists = []
    for sh in shapes:
        lst = []
        for v, d in sorted(sh.items()):
            q = [max(-32768, min(32767, int(round(c * sc)))) for c in d]
            if any(q):
                lst.append((v, q))
        lists.append(lst)
    tb = bytearray(struct.pack(">I", len(lists)))
    ofs = 8 * len(lists)
    data = bytearray()
    for lst in lists:
        tb += struct.pack(">Ii", ofs + len(data), len(lst))
        for v, (dx, dy, dz) in lst:
            data += struct.pack(">Hhhh", v, dx, dy, dz)
    return bytes(tb + data), [len(l) for l in lists]


def with_shapes(blob, table):
    """The model with `table` appended (32-byte aligned) as its shape table."""
    out = bytearray(blob)
    out += bytes((-len(out)) % 32)
    at = len(out)
    out += table
    out += bytes((-len(out)) % 32)
    struct.pack_into(">I", out, 0x2C, at)
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--event", required=True, help="an event file with Leon's head ev0001 (.evd, PC .evd.lfs)")
    ap.add_argument("--entry", type=int, default=2, help="the skin's head (Leon: 2, archive part 6)")
    ap.add_argument("--mouth", type=float, help="the height of the skin's lips' parting (default: 36 under its nose tip)")
    ap.add_argument("--mouth-gain", type=float, default=1.0, help="the mouth shapes (1..8) times this")
    ap.add_argument("--slots", default="1-8,10,11,13-28", help="the shape slots to carry (the others stay empty; "
                    "fewer bytes); default all but the blinks 9 and 12, which tear a head whose lids differ from the "
                    "event's (the eyes stay open)")
    ap.add_argument("--threshold", type=float, default=0.2, help="smaller movements are left out (model units)")
    ap.add_argument("--preview", metavar="DIR", help="also write one archive per shape at full weight (for renders)")
    a = ap.parse_args(argv)
    E, ev_shapes, endian = event_head(a.event)
    print("event head: %d positions, %d shape slots (%s)" % (len(E), len(ev_shapes),
                                                              "PC, little-endian" if endian == "<" else "disc"))
    arc = U.Archive(Path(a.inp).read_bytes(), ">")
    entries = [arc.entry(i) for i in range(len(arc.offsets))]
    head = entries[a.entry]
    S = G.positions(head)
    slots = None
    if a.slots != "all":
        slots = set()
        for part in a.slots.split(","):
            lo, _, hi = part.partition("-")
            slots.update(range(int(lo), int(hi or lo) + 1))
    shapes, ls, le = carry_shapes(S, E, ev_shapes, skin_mouth=a.mouth, mouth_gain=a.mouth_gain,
                                  threshold=a.threshold, slots=slots)
    print("landmarks (event -> skin): " + ", ".join("%s %.0f -> %.0f" % (q, le[q], ls[q])
                                                   for q in ("chin", "mouth", "nose", "eyes")))
    table, counts = shape_table(shapes, head[0x28])
    print("shapes: %s positions moved (of %d)" % ("/".join(str(c) for c in counts), len(S)))
    old = struct.unpack_from(">I", head, 0x2C)[0]
    if old:
        print("entry %d had a shape table of its own: replaced" % a.entry)
        head = bytearray(head)
        struct.pack_into(">I", head, 0x2C, 0)
        head = bytes(head)
    entries[a.entry] = with_shapes(head, table)
    data = U.build_archive(arc, U.build_body(entries, arc.types))
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's place holds 6291456)" %
          (a.out, len(data), U.Archive(data, ">").parts[0][1]))
    if a.preview:
        Path(a.preview).mkdir(parents=True, exist_ok=True)
        for k, sh in enumerate(shapes):
            if not sh:
                continue
            P = S.copy()
            for v, d in sh.items():
                P[v] += d
            blob = bytearray(head)
            for i, q in enumerate(P):
                G.set_position(blob, i, q)
            ent = list(entries)
            ent[a.entry] = bytes(blob)
            Path(a.preview, "shape%02d.drs" % k).write_bytes(U.build_archive(arc, U.build_body(ent, arc.types)))


if __name__ == "__main__":
    main()
