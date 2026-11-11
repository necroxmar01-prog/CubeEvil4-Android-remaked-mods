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
alike); the four nearest, weighted by inverse squared distance, faded out from 5 to 12 units away. The blink (slot 9)
is made for the skin's own lids instead (synth_blink; --blink event carries the event's over too). The table holds
the event's 29 slots (slot 0 empty, as in ev0001), deltas in the head's own shift; the head's model gets its shape
table (header 0x2C) appended. A head that already had shapes loses them (the game's expressions in play are the
face's, entry 4, not the head's). The event file may be the PC release's (little-endian, floats) or the disc's.

Disc and mod files never go into the repository; this tool only reads and writes the paths given."""
import argparse
import collections
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import uhd_skin_convert as U  # noqa: E402
import skin_graft as G  # noqa: E402
import skin_slim as SL  # noqa: E402

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
                 eye_r=(18.0, 11.0), mouth_gain=1.0, threshold=0.2, slots=None, inside=6.0):
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
    # the event head's inside of the mouth (its teeth, tongue and the mouth's bag, more than `inside` behind the face
    # around them) moves the jaw's inside: the skin's head has none, its lips would take that and hang into the
    # opening (DevJunior, 11.11.2026: the mouth hardly opened; a larger opening showed it)
    box = (np.abs(A[:, 0]) < 26) & (np.abs(A[:, 1] - m) < 16)
    surface = np.full(len(A), -1e9)
    for i in np.where(box)[0]:
        near_ = box & (np.abs(A[:, 0] - A[i, 0]) < 2.0) & (np.abs(A[:, 1] - A[i, 1]) < 3.0)
        surface[i] = A[near_, 2].max()
    outer = ~(box & (surface - A[:, 2] > inside))
    outer_idx = np.where(outer)[0]
    full_ = cKDTree(A[outer_idx])

    class _Full:
        def query(self, Q, k):
            d, j = full_.query(Q, k=k)
            return d, outer_idx[j]

    full = _Full()
    for gi in np.unique(gs):
        sel = gs == gi
        if gi == 0:
            d, j = full.query(S[sel], k=k)
            idx[sel], dist[sel] = j, d
            continue
        back = np.where((ge == gi) & outer)[0]
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


def mouth_inside(head, mouth, depth=2.0, band=3.0, half=24.0, reach=9.0):
    """The lips' inner faces turned away from the light (DevJunior, 11.11.2026: "the models hardly open the mouth"):
    the skin's head has no inside of the mouth, its lips' inner faces are what an opening shows, lit like the lips. Per
    normal used only by positions that lie more than `depth` behind the face around them (within 1.5 across and `band`
    up or down, between the mouth's corners, `reach` above or below the parting), the normal leans into the head, all
    of the way 3 units deeper; the game's light then leaves those faces in the dark, as a mouth's inside. Closed, they
    lie in the lips' fold and are not seen. Returns (head, normals turned)."""
    P = G.positions(head)
    b = bytearray(head)
    s8 = bool(struct.unpack_from(">I", b, 0x20)[0] & 0x20000000)
    base = struct.unpack_from(">I", b, 0x34)[0]
    lip = (np.abs(P[:, 0]) < half) & (np.abs(P[:, 1] - mouth) < reach) & (P[:, 2] > 30)
    behind = np.zeros(len(P))
    for i in np.where(lip)[0]:
        near = lip & (np.abs(P[:, 0] - P[i, 0]) < 1.5) & (np.abs(P[:, 1] - P[i, 1]) < band)
        behind[i] = P[near, 2].max() - P[i, 2]
    users = collections.defaultdict(set)
    for _, tris in SL.part_lists(bytes(b)):
        for t in tris:
            for r in t:
                users[r[1]].add(r[0])
    inward = np.array([0.0, 0.3, -1.0])
    inward /= np.linalg.norm(inward)
    turned = 0
    for n, ps in users.items():
        d = min(behind[q] for q in ps)
        if d <= depth:
            continue
        k = min(1.0, (d - depth) / 3.0)
        fmt, size, scale = (">bbb", 4, 64.0) if s8 else (">hhh", 8, 16384.0)
        v = np.array(struct.unpack_from(fmt, b, base + n * size), float) / scale
        v = (1 - k) * v + k * inward
        v /= np.linalg.norm(v) + 1e-9
        struct.pack_into(fmt, b, base + n * size, *[int(round(c * (scale - 1))) for c in v])
        turned += 1
    return bytes(b), turned


def _sphere(Q):
    """Least-squares sphere through Q: (centre, radius, mean distance off it)."""
    A = np.c_[2 * Q, np.ones(len(Q))]
    c = np.linalg.lstsq(A, (Q ** 2).sum(1), rcond=None)[0]
    r = float(np.sqrt(c[3] + (c[:3] ** 2).sum()))
    return c[:3], r, float(np.abs(np.linalg.norm(Q - c[:3], axis=1) - r).mean())


def _pieces(T, n):
    """Per position its connected piece (by the triangles T)."""
    parent = np.arange(n)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for t in T:
        a = find(t[0])
        for q in t[1:]:
            b = find(q)
            if b != a:
                parent[b] = a
    return np.array([find(i) for i in range(n)])


def synth_blink(head, eyes_y=1722.0, close=0.8, up=9.0, low=6.0, overshoot=1.2):
    """A blink made for the skin's own lids (the event's blink, carried over, tore lids shaped otherwise; DevJunior,
    10.11.2026: "a blink, so his face looks natural when he talks"). The eyeballs: the head's part whose positions on
    each side lie on a sphere. The lid rims: the skin's positions just outside that sphere (within 0.3..3 of it), in
    front, from the inner to the outer corner; per 1.5 units across, the upper rim's lowest and the lower rim's
    highest point. The lids then turn about the eyeball's centre (they slide over it, nothing goes into it): the upper
    lid's rim to `close` of the way down (and `overshoot` further, so no gap stays), the skin above it less and less up
    to `up` above the rim, the lower lid's rim the rest of the way up, fading out `low` below it; nothing at the
    corners. Lash cards (small pieces) turn as one, as their root does. Returns ({position: (dx, dy, dz)}, log)."""
    P = G.positions(head)
    parts = [np.array([[r[0] for r in t] for t in tris]).reshape(-1, 3) for _, tris in SL.part_lists(head)]
    best = None
    for k, t in enumerate(parts):
        if not len(t):
            continue
        u = np.unique(t)
        fits = []
        for side in (1, -1):
            Q = P[u]
            Q = Q[(Q[:, 0] * side > 5) & (np.abs(Q[:, 1] - eyes_y) < 35) & (Q[:, 2] > 30)]
            if len(Q) < 50:
                break
            c, r, e = _sphere(Q)
            if not 8 < r < 25 or e > 0.1 * r:
                break
            fits.append((c, r))
        if len(fits) == 2 and (best is None or len(u) > best[0]):
            best = (len(u), k, fits)
    if best is None:
        return {}, "blink: no eyeballs found (two spheres in one part)"
    _, ek, fits = best
    ball = set(np.unique(parts[ek]).tolist())
    T = np.concatenate([t for k, t in enumerate(parts) if k != ek and len(t)])
    used = np.array([v for v in np.unique(T) if v not in ball])
    root = _pieces(T, len(P))
    size = collections.Counter(root[used].tolist())
    out, log = {}, []
    for c, R in fits:
        q = P - c
        r = np.linalg.norm(q, axis=1)
        near = used[(np.abs(q[used, 0]) < 26) & (np.abs(q[used, 1]) < 20) & (q[used, 2] > 0)]
        rim = near[(r[near] > R + 0.3) & (r[near] < R + 3.0) & (q[near, 2] > 4)]
        rim = np.array([v for v in rim if size[root[v]] > 200])  # the skin, not the lash cards
        if len(rim) < 10:
            log.append("blink: the eye at x %.0f has no lid rim" % c[0])
            continue
        xs, ys = q[rim, 0], q[rim, 1]
        x0, x1 = xs.min(), xs.max()
        y0, y1 = ys[xs.argmin()], ys[xs.argmax()]

        def mid(x):
            return y0 + (y1 - y0) * (x - x0) / max(x1 - x0, 1e-6)

        bx, yu, yl = [], [], []
        for b in np.arange(np.floor(x0), np.ceil(x1) + 1, 1.5):
            sel = (xs >= b - 1.5) & (xs < b + 1.5)
            above, below = ys[sel & (ys > mid(xs))], ys[sel & (ys < mid(xs))]
            if len(above) and len(below):
                bx.append(b)
                yu.append(above.min())
                yl.append(below.max())
        if len(bx) < 3:
            log.append("blink: the eye at x %.0f: no opening found" % c[0])
            continue
        bx, yu, yl = np.array(bx), np.array(yu), np.array(yl)
        rho = R + 1.5

        def ang(y):
            return np.arcsin(np.clip(y / rho, -0.99, 0.99))

        near = near[(q[near, 0] > x0 - 2) & (q[near, 0] < x1 + 2)]
        x = np.clip(q[near, 0], bx.min(), bx.max())
        yu_, yl_ = np.interp(x, bx, yu), np.interp(x, bx, yl)
        meet = yl_ + (1 - close) * (yu_ - yl_)
        upper = q[near, 1] > mid(q[near, 0])
        wu = np.clip(1 - (q[near, 1] - yu_) / up, 0, 1)
        wl = np.clip(1 - (yl_ - q[near, 1]) / low, 0, 1)
        corner = np.clip(np.minimum(q[near, 0] - x0, x1 - q[near, 0]) / 1.5, 0, 1)
        th = np.zeros(len(P))
        th[near] = np.where(upper, (ang(meet - overshoot) - ang(yu_)) * wu, (ang(meet) - ang(yl_)) * wl) * corner
        moved = set(near.tolist())
        lashes = collections.defaultdict(list)
        for v in near:
            if size[root[v]] <= 200:
                lashes[root[v]].append(v)
        for pc, vs in lashes.items():
            vs = np.array(vs)
            allv = np.where(root == pc)[0]
            th[allv] = th[vs[r[vs].argmin()]]
            moved.update(allv.tolist())
        idx = np.array(sorted(moved))
        t_ = th[idx]
        dy = q[idx, 1] * np.cos(t_) + q[idx, 2] * np.sin(t_) - q[idx, 1]
        dz = -q[idx, 1] * np.sin(t_) + q[idx, 2] * np.cos(t_) - q[idx, 2]
        for v, a_, b_ in zip(idx, dy, dz):
            if max(abs(a_), abs(b_)) > 0.05:
                out[int(v)] = (0.0, float(a_), float(b_))
        log.append("blink: eye at (%.0f, %.0f, %.0f), radius %.1f, opening up to %.1f, %d positions" %
                   (c[0], c[1], c[2], R, (yu - yl).max(), len(idx)))
    return out, "; ".join(log)


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
    ap.add_argument("--slots", default="1-28", help="the shape slots to fill (the others stay empty; fewer bytes); "
                    "default all")
    ap.add_argument("--blink", choices=("own", "event"), default="own", help="slot 9, the blink: made for the skin's "
                    "own lids (synth_blink, default) or carried over from the event's head like the others (tears "
                    "lids shaped otherwise)")
    ap.add_argument("--no-mouth-inside", action="store_true", help="leave the lips' inner faces' normals as they are "
                    "(see mouth_inside; run the tool on a head without them turned, else they turn once more)")
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
    if a.blink == "own" and (slots is None or 9 in slots) and len(shapes) > 9:
        blink, line = synth_blink(head)
        print(line)
        if blink:
            shapes[9] = {v: d for v, d in blink.items() if max(abs(c) for c in d) > a.threshold}
    print("landmarks (event -> skin): " + ", ".join("%s %.0f -> %.0f" % (q, le[q], ls[q])
                                                   for q in ("chin", "mouth", "nose", "eyes")))
    if not a.no_mouth_inside:
        head, turned = mouth_inside(head, ls["mouth"])
        print("mouth: %d normals of the lips' inner faces turned into the head" % turned)
    table, counts = shape_table(shapes, head[0x28])
    print("shapes: %s positions moved (of %d)" % ("/".join(str(c) for c in counts), len(S)))
    old = struct.unpack_from(">I", head, 0x2C)[0]
    if old:
        head = bytearray(head)
        struct.pack_into(">I", head, 0x2C, 0)
        # a table this tool appended (behind every other section) goes; one inside the model stays, unused
        later = [o for o in SL.sections(bytes(head)).values() if o[0] > old]
        if not later:
            del head[old:]
        print("entry %d had a shape table of its own: replaced%s" % (a.entry, "" if later else " (its bytes dropped)"))
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
