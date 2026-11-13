#!/usr/bin/env python3
"""Builds a lighter skin out of two converted ones (wish of DevJunior, 09.11.2026: "optimise the Remake Leon with
the other model, take pieces of the new one, and give him the same muscles").

    skin_slim.py BASE.drs OUT.drs --take NEW.drs:2,3,4,5,13-21 --cards 5:0.5 --fit 0=NEW.drs:0 --carry 0:6,12

Both archives are written by uhd_skin_convert.py (or the disc's) for the same skeleton.

--take SRC:ENTRIES   copies entries of SRC as they are (a model with the palette it draws with: Leon's head, face and
                     hair with palette 3, his hands 14..21 with palette 13; a model alone keeps BASE's palette).
--cards ENTRY:KEEP   thins out a hair made of cards (strips of a few quads, one connected piece each): keeps the share
                     KEEP of them, the outer ones first (the inner layers hardly show), and drops the positions,
                     normals, colours and texture coordinates nothing draws any more. Its parts' drawing lists are
                     written anew as triangle lists.
--fit ENTRY=SRC:E    grows BASE's model ENTRY to the surface of SRC's model E where SRC's lies further out from the
                     skeleton (the torso from the spine, the arms from their bones, the legs from theirs): the other
                     model's muscles on the lighter one. Where BASE's model lies further out (pouches, straps) it stays;
                     head, neck, hands and shoes stay. The push is smoothed over neighbouring positions.
                     Two rounds by default (--passes): the smoothing leaves some of the way after the first.
--collar ENTRY=SRC:E moves model ENTRY's collar onto the collar of SRC's model E, the body the taken head was made for
                     (DevJunior, 09.11.2026: "the neck": a turtleneck made for another head stands off the new
                     neck): each position to the nearest point of E around the neck, in full from y 1580, fading out
                     below; the shoulders stay.
--tuck HEAD:BODY     keeps the base of head HEAD's neck under the collar of model BODY (after --fit and --collar):
                     a head position below y 1660 further from the neck's axis than the body's inner side around it
                     goes 4 inside it (skin made for a lower neckline showed through at the trapezius).
--hide ENTRY[@MATS]:COVER  drops ENTRY's triangles that model COVER hides (a body under a jacket): a position is
                     hidden when COVER lies in front of it, out from the skeleton within 60. With @MATS only the parts
                     with these colour textures, and COVER = ENTRY: the model's other parts cover them (a shirt inside
                     its jacket's model). Options after a third colon: front=X keeps the positions in front within
                     |x| < X (an open jacket's front), minx=X hides only at |x| >= X (e.g. 0@3:6:minx=500).
--inside ENTRY:COVER pushes ENTRY's positions above y 1150 that poke out through COVER (within 25 of it, outside
                     it) 5 under it (a shirt through a jacket's shoulders); the jacket's open front, further away,
                     and the legs stay.
--cover HAIR:HEAD    lifts hair model HAIR over head model HEAD's skull (a hair made for another head): above
                     y 1700, every hair position at least 4 further out from the skull's centre than the head there.
--cut ENTRY:Y        drops ENTRY's triangles that reach above y Y (the body's upper half under a jacket model that
                     brings its own shirt).
--back ENTRY:Z:Y     drops ENTRY's triangles wholly behind z Z and below y Y (a shirt's back under a closed coat).
--compact ENTRY      drops what nothing in ENTRY draws any more (after skin_graft.py --drop).
--prune TPL          keeps only the textures of palette TPL its models draw (after a graft or a hide).
--carry ENTRY:LIST   moves the models LIST (straps, a harness) as the nearest positions of ENTRY moved in --fit.
"""
import argparse
import collections
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import skin_graft as G  # noqa: E402
import uhd_skin_convert as U  # noqa: E402

# ---- the model ------------------------------------------------------------------------------------------------------


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def sections(b):
    """The model's sections: name -> (offset, length), the lengths up to the next section."""
    names = {"head": 0x00, "clr": 0x0C, "tex": 0x10, "wgt": 0x14, "parts": 0x1C, "shape": 0x2C, "vtx": 0x30,
             "nrm": 0x34}
    if be32(b, 0x3C) == 0x20030818:
        names.update(blend=0x40, flip=0x44)
    found = {k: be32(b, o) for k, o in names.items() if be32(b, o)}
    order = sorted(found.items(), key=lambda kv: kv[1])
    out = {}
    for i, (k, o) in enumerate(order):
        end = order[i + 1][1] if i + 1 < len(order) else len(b)
        out[k] = (o, end - o)
    return out


def record_sizes(b):
    flags = be32(b, 0x20)
    s8 = flags & 0x20000000
    return {"vtx": 8, "nrm": 4 if s8 else 8, "clr": 4, "tex": 4}, (4 if flags & 0x80000000 else 3)


def part_lists(b):
    """Per part: (its header offset, [triangles of index records (pos, nrm[, clr], tex)])."""
    _, per = record_sizes(b)
    out = []
    for p in G.parts(b):
        size = be32(b, p + 0x18)
        s, e = p + 0x20, p + 0x20 + size
        tris = []
        while s < e and b[s] != 0:
            op, cnt = b[s], be16(b, s + 1)
            v = [struct.unpack_from(">%dH" % per, b, s + 3 + k * per * 2) for k in range(cnt)]
            kind = op & 0xF8
            if kind == 0x90:
                tris += [(v[k], v[k + 1], v[k + 2]) for k in range(0, cnt - 2, 3)]
            elif kind == 0x98:
                tris += [(v[k], v[k + 1], v[k + 2]) if k % 2 == 0 else (v[k + 1], v[k], v[k + 2]) for k in range(cnt - 2)]
            elif kind == 0xA0:
                tris += [(v[0], v[k], v[k + 1]) for k in range(1, cnt - 1)]
            elif kind == 0x80:
                for k in range(0, cnt - 3, 4):
                    tris += [(v[k], v[k + 1], v[k + 2]), (v[k], v[k + 2], v[k + 3])]
            else:
                raise ValueError("unknown primitive %#x" % op)
            s += 3 + cnt * per * 2
        out.append((p, tris))
    return out


def rebuild(b, kept):
    """The model with only the triangles `kept` (per part, as part_lists gives them): the positions, normals, colours
    and texture coordinates they use, in their old order, the drawing lists as triangle lists, every other section as
    it was. Returns the new model."""
    sec = sections(b)
    size, per = record_sizes(b)
    used = [set() for _ in range(per)]
    for tris in kept:
        for t in tris:
            for r in t:
                for k in range(per):
                    used[k].add(r[k])
    kinds = ["vtx", "nrm", "clr", "tex"] if per == 4 else ["vtx", "nrm", "tex"]
    remap = []
    arrays = {}
    for k, name in enumerate(kinds):
        order = sorted(used[k])
        remap.append({old: new for new, old in enumerate(order)})
        o, _ = sec[name]
        rs = size[name]
        arrays[name] = b"".join(b[o + i * rs:o + i * rs + rs] for i in order)
    out = bytearray(0x60)
    where = {}

    def put(name, data, a=32):
        out.extend(bytes(U.align(len(out), a) - len(out)))
        where[name] = len(out)
        out.extend(data)

    put("head", b[sec["head"][0]:sec["head"][0] + sec["head"][1]], 16)
    put("vtx", arrays["vtx"])
    put("nrm", arrays["nrm"])
    if "clr" in arrays:
        put("clr", arrays["clr"])
    put("tex", arrays["tex"])
    put("wgt", b[sec["wgt"][0]:sec["wgt"][0] + sec["wgt"][1]])
    parts = bytearray()
    for (p, _), tris in zip(part_lists(b), kept):
        stream = bytearray()
        flat = [r for t in tris for r in t]
        for c in range(0, len(flat), 0xFFFF // 3 * 3):
            chunk = flat[c:c + 0xFFFF // 3 * 3]
            stream += struct.pack(">BH", 0x90, len(chunk))
            for r in chunk:
                stream += struct.pack(">%dH" % per, *(remap[k][r[k]] for k in range(per)))
        stream += bytes(U.align(len(stream)) - len(stream))
        parts += b[p:p + 0x18] + struct.pack(">II", len(stream), len(tris)) + stream
    put("parts", parts)
    for name in ("blend", "flip"):
        if name in sec:
            o, n = sec[name]
            put(name, b[o:o + n], 4)
    if "shape" in sec:
        raise ValueError("a model with shapes (the face) is not thinned out here")
    out.extend(bytes(U.align(len(out)) - len(out)))
    out[0:0x60] = b[0:0x60]
    for name, off in (("head", 0), ("clr", 0xC), ("tex", 0x10), ("wgt", 0x14), ("parts", 0x1C), ("vtx", 0x30),
                      ("nrm", 0x34)):
        struct.pack_into(">I", out, off, where.get(name, 0))
    if "blend" in where:
        struct.pack_into(">II", out, 0x40, where["blend"], where["flip"])
    struct.pack_into(">HH", out, 0x38, len(arrays["vtx"]) // 8, len(arrays["nrm"]) // size["nrm"])
    return bytes(out)


# ---- --cards --------------------------------------------------------------------------------------------------------


def thin_cards(b, keep, centre=(0.0, 1720.0, 0.0)):
    """Keeps the share `keep` of a card hair's pieces, the outer ones (by their distance from the head's centre, in
    turns over the head so no side goes bald), and every triangle of the parts with few of them (a scalp cap)."""
    lists = part_lists(b)
    P = G.positions(b)
    nv = len(P)
    parent = list(range(nv))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for _, tris in lists:
        for t in tris:
            a = find(t[0][0])
            for r in t[1:]:
                c = find(r[0])
                if c != a:
                    parent[c] = a
    root = np.array([find(i) for i in range(nv)])
    pieces = collections.defaultdict(list)
    for i, r in enumerate(root):
        pieces[r].append(i)
    c = np.array(centre)
    info = {}
    for r, idx in pieces.items():
        q = P[idx].mean(0) - c
        dist = np.linalg.norm(P[idx] - c, axis=1).max()
        ang = np.degrees(np.arctan2(q[0], q[2]))
        height = q[1]
        info[r] = (dist, int((ang + 180) // 30), int(height // 40))
    # in each cell of the head (30 degrees around, 40 units high) the outer pieces first
    cells = collections.defaultdict(list)
    for r, (dist, a, h) in info.items():
        cells[(a, h)].append((dist, r))
    kept_roots = set()
    for cell in cells.values():
        cell.sort(reverse=True)
        n = max(1, int(round(len(cell) * keep)))
        kept_roots.update(r for _, r in cell[:n])
    kept = []
    for _, tris in lists:
        if len(tris) < 0.05 * sum(len(t) for _, t in lists):
            kept.append(tris)  # a small part (the scalp under the cards): all of it
        else:
            kept.append([t for t in tris if root[t[0][0]] in kept_roots])
    return rebuild(b, kept), len(pieces), len(kept_roots)


# ---- --fit ----------------------------------------------------------------------------------------------------------


def outward(P):
    """For each position its direction away from Leon's skeleton (T-pose) and a weight: 1 where the body's shape may
    change (torso, arms, legs), 0 at the head and neck, the hands and the shoes, fading in between."""
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    ax = np.abs(x)
    clip = lambda v: np.clip(v, 0.0, 1.0)
    dt = np.stack([x, np.zeros_like(x), z + 12.0], 1)
    da = np.stack([np.zeros_like(x), y - 1508.0, z + 34.0], 1)
    dl = np.stack([x - 92.0 * np.sign(x), np.zeros_like(x), z], 1)
    arm = clip((ax - 190) / 40) * (y > 1350)
    leg = clip((1000 - y) / 80) * (1 - arm)
    torso = (1 - arm) * (1 - leg)
    d = dt * torso[:, None] + da * arm[:, None] + dl * leg[:, None]
    d /= np.linalg.norm(d, axis=1, keepdims=True) + 1e-9
    w = np.ones(len(P))
    w *= 1 - clip((y - 1500) / 40) * clip((120 - ax) / 40)  # the neck and the collar
    w *= clip((1640 - y) / 40)  # the head
    w *= clip((650 - ax) / 40)  # the hands
    w *= clip((y - 160) / 60)  # the shoes
    return d, w


def fit(target, ref, most=70.0, smooth=30.0, target_mats=None, ref_mats=None, top=None):
    """Moves target's positions out to ref's surface along `outward` (see --fit); returns (new model, the push per
    position). target_mats / ref_mats: only the positions of target's parts with these textures move, only ref's
    parts with these textures are its surface (DevJunior, 12.11.2026: the RE9 shirt as full as the Lite shirt under
    the same harness: the harness's parts stay, and are no surface to push to)."""
    from scipy.spatial import cKDTree
    P = G.positions(target)
    Q = G.positions(ref)
    # ref's surface: its positions and the centres of its triangles, dense enough to stand for it
    cents, used = [], set()
    for p, tris in part_lists(ref):
        if tris and (ref_mats is None or ref[p + 0xC] in ref_mats):
            idx = np.array([[r[0] for r in t] for t in tris])
            cents.append(Q[idx].mean(1))
            used.update(idx.ravel().tolist())
    S = np.concatenate([Q[sorted(used)] if ref_mats is not None else Q] + cents)
    tree = cKDTree(S)
    dirs, w = outward(P)
    if top is not None:  # only below this height (fading over 40): the shoulders' tops and the collar keep their shape
        w = w * np.clip((top - P[:, 1]) / 40.0, 0.0, 1.0)
    if target_mats is not None:
        mine = np.zeros(len(P), bool)
        own = []
        for p, tris in part_lists(target):
            if tris and target[p + 0xC] in target_mats:
                idx = np.array([[r[0] for r in t] for t in tris])
                mine[idx.ravel()] = True
                own.append(P[idx].astype(float))
        # only the outer layer: a position with more of these parts outside it along its direction lies inside (the
        # RE9 shirt's inner fans behind the collar, pushed out, stood out as flaps)
        import skin_patch as SP
        cand = np.nonzero(mine & (w > 0))[0]
        inner = SP.ray_hit_dist(P[cand] + dirs[cand] * 2.0, dirs[cand], np.concatenate(own), 300.0)
        moves = mine.copy()  # these move (by the smoothed push of the outer ones around them) ...
        fade = w.copy()
        mine[cand[inner]] = False
        w = w * mine  # ... these find the push
    push = np.zeros(len(P))
    # how far out ref's surface lies along the direction: the nearest surface point's distance along it, if it lies
    # roughly that way (a point sideways is another limb or a fold)
    dist, near = tree.query(P, k=8)
    for i in range(len(P)):
        if w[i] <= 0:
            continue
        best = 0.0
        for j in near[i]:
            d = S[j] - P[i]
            along = float(d @ dirs[i])
            if along <= 0:
                continue
            side = np.linalg.norm(d - along * dirs[i])
            if side < 0.6 * along + 12.0:
                best = max(best, along)
        push[i] = min(best, most) * w[i]
    # smoothed: each position takes the mean of the pushes around it
    ptree = cKDTree(P)
    if target_mats is None:
        smoothed = np.array([push[ptree.query_ball_point(P[i], smooth)].mean() for i in range(len(P))])
    else:
        # a push field of the outer positions' pushes, the same for every layer at a place (the shirt's layers move
        # together, no tears); only these parts' positions move, by their own fade
        samples = np.nonzero(w > 0)[0]
        stree = cKDTree(P[samples])
        smoothed = np.zeros(len(P))
        for i in np.nonzero(moves & (fade > 0))[0]:
            near_s = stree.query_ball_point(P[i], smooth)
            if near_s:
                smoothed[i] = push[samples[near_s]].mean() / max(w[samples[near_s]].mean(), 1e-6) * fade[i]
    out = bytearray(target)
    for i in range(len(P)):
        if smoothed[i] > 0.05:
            G.set_position(out, i, P[i] + dirs[i] * smoothed[i])
    return bytes(out), P, dirs * smoothed[:, None]


def far_hit(origins, dirs, T, chunk=128):
    """Per ray the distance to its farthest hit among triangles T (n x 3 x 3), 0 for none."""
    e1 = T[:, 1] - T[:, 0]
    e2 = T[:, 2] - T[:, 0]
    out = np.zeros(len(origins))
    for c in range(0, len(origins), chunk):
        o, d = origins[c:c + chunk, None, :], dirs[c:c + chunk, None, :]
        p = np.cross(d, e2[None])
        det = np.einsum("rtk,tk->rt", p, e1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s_ = o - T[None, :, 0]
        u = np.einsum("rtk,rtk->rt", s_, p) * inv
        q = np.cross(s_, e1[None])
        v = np.einsum("rtk,rtk->rt", d, q) * inv
        t = np.einsum("tk,rtk->rt", e2, q) * inv
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0)
        out[c:c + chunk] = np.where(hit, t, 0.0).max(1)
    return out


def girth(target, ref, target_mats, ref_mats, top=1470.0, fade=40.0, reach=175.0, most=1.45, smooth=35.0):
    """Widens the torso of target's parts with target_mats to the girth of ref's parts with ref_mats (DevJunior,
    12.11.2026: "more bulk" for the RE9 shirt under the Lite harness, the harness sat out from it): for every position
    of these parts, from the body's axis (x 0, z -12) at its height, the direction through it, and along it the outer
    radius of both models (their farthest triangle there); the position moves out from the axis by the ratio (at
    least 1, at most `most`), smoothed over the positions within `smooth`: the layers of the shirt at a place move
    alike (no tears). In full below `top`, fading to nothing over `fade` above it (the collar and the V neck keep
    their shape), and only within |x| `reach` (the sleeves are the Lite's). Returns (model, positions moved, ratios)."""
    from scipy.spatial import cKDTree
    P = G.positions(target).astype(float)
    Q = G.positions(ref).astype(float)

    def tris_of(m, X, mats):
        T, k = [], np.zeros(len(X), bool)
        for p, tris in part_lists(m):
            if tris and m[p + 0xC] in mats:
                idx = np.array([[r[0] for r in t] for t in tris])
                T.append(X[idx])
                k[idx.ravel()] = True
        return np.concatenate(T), k

    TT, mine = tris_of(target, P, target_mats)
    TR, _ = tris_of(ref, Q, ref_mats)
    TR = TR[np.abs(TR.mean(1)[:, 0]) < reach]
    idx = np.nonzero(mine & (np.abs(P[:, 0]) < reach + 20.0) & (P[:, 1] < top + fade))[0]
    A = np.stack([np.zeros(len(idx)), P[idx, 1], np.full(len(idx), -12.0)], 1)
    d = P[idx] - A
    d[:, 1] = 0.0
    r = np.linalg.norm(d, axis=1)
    d /= np.maximum(r, 1e-6)[:, None]
    rt, rr = far_hit(A, d, TT), far_hit(A, d, TR)
    ratio = np.where((rt > 1.0) & (rr > 1.0), rr / np.maximum(rt, 1.0), 1.0)
    ratio = np.clip(ratio, 1.0, most)
    tree = cKDTree(P[idx])
    sm = np.array([ratio[tree.query_ball_point(P[i], smooth)].mean() for i in idx])
    wgt = np.clip((top + fade - P[idx, 1]) / fade, 0, 1) * np.clip((reach + 20.0 - np.abs(P[idx, 0])) / 20.0, 0, 1)
    k = 1.0 + (sm - 1.0) * wgt
    out = bytearray(target)
    moved = 0
    for n, i in enumerate(idx):
        if k[n] > 1.002:
            x, y, z = P[i]
            G.set_position(out, i, np.array([x * k[n], y, (z + 12.0) * k[n] - 12.0]))
            moved += 1
    return bytes(out), moved, k


def collar(target, ref, low=1545.0, full=1580.0, most=60.0, smooth=20.0):
    """Moves target's collar onto the collar of `ref` (the model the head was made for: its collar wraps that neck),
    each position to the nearest point of ref's surface around the neck, in full from y `full`, fading out to `low`;
    the shoulders (more than 125 from the spine) stay. Returns (model, positions before, the move per position)."""
    from scipy.spatial import cKDTree
    P = G.positions(target)
    Q = G.positions(ref)
    zone = lambda X: (X[:, 1] > low - 30) & (np.hypot(X[:, 0], X[:, 2] + 30) < 140)
    cents = []
    for _, tris in part_lists(ref):
        if tris:
            idx = np.array([[r[0] for r in t] for t in tris])
            cents.append(Q[idx].mean(1))
    S = np.concatenate([Q] + cents)
    S = S[zone(S)]
    tree = cKDTree(S)
    move = np.zeros_like(P)
    w = np.clip((P[:, 1] - low) / (full - low), 0, 1) * np.clip((125 - np.hypot(P[:, 0], P[:, 2] + 30)) / 25, 0, 1)
    for i in np.nonzero(w > 0)[0]:
        d, j = tree.query(P[i])
        if d <= most:
            move[i] = (S[j] - P[i]) * w[i]
    ptree = cKDTree(P)
    sm = np.array([move[ptree.query_ball_point(P[i], smooth)].mean(0) if w[i] > 0 else move[i] for i in range(len(P))])
    out = bytearray(target)
    for i in np.nonzero(np.linalg.norm(sm, axis=1) > 0.05)[0]:
        G.set_position(out, i, P[i] + sm[i])
    return bytes(out), P, sm


def tuck(head, body, top=1660.0, inset=4.0, axis=(0.0, -30.0)):
    """Keeps the base of a head's neck under the body's collar: below `top`, a head position that lies further from the
    neck's axis (x, z = `axis`) than the body around it (the same height and direction) goes `inset` inside it.
    Returns (model, how many moved)."""
    P = G.positions(head)
    B = G.positions(body)
    B = B[(B[:, 1] > 1450) & (np.hypot(B[:, 0] - axis[0], B[:, 2] - axis[1]) < 220)]
    bd = np.stack([B[:, 0] - axis[0], B[:, 2] - axis[1]], 1)
    br = np.linalg.norm(bd, axis=1)
    ba = np.arctan2(bd[:, 0], bd[:, 1])
    out = bytearray(head)
    n = 0
    for i, p in enumerate(P):
        if p[1] >= top:
            continue
        d = np.array([p[0] - axis[0], p[2] - axis[1]])
        r = np.linalg.norm(d)
        if r < 1e-3:
            continue
        a = np.arctan2(d[0], d[1])
        near = (np.abs(B[:, 1] - p[1]) < 20) & (np.abs((ba - a + np.pi) % (2 * np.pi) - np.pi) < np.radians(12))
        if not near.any():
            continue
        # the body's inner side around here: the collar's ring nearest the neck
        rb = br[near].min()
        if r <= rb - inset:
            continue
        nr = rb - inset
        q = p.copy()
        q[0], q[2] = axis[0] + d[0] / r * nr, axis[1] + d[1] / r * nr
        G.set_position(out, i, q)
        n += 1
    return bytes(out), n


def hide(model, cover, reach=60.0, mats=None, front=0.0, minx=0.0, hem=0.0):
    """Drops the triangles of `model` that `cover` hides (a body under a jacket): a position is hidden when the cover
    lies in front of it, out along `outward` within `reach` and about that way; a triangle goes when all three
    positions are hidden. `mats`: only the parts with these colour textures, and `cover` None: the model's other
    parts cover them (a shirt inside the same model as its jacket). `front`: positions in front (z > 0) within
    |x| < front stay (an open jacket's front: the shirt and the buckle seen through it); `minx`: only positions at
    |x| >= minx go (wristbands under the sleeves, not the boots). `hem`: also hidden is what the cover hangs over
    (belt pouches poking out at a jacket's sides; DevJunior, 10.11.2026): around the body's axis, within 8 degrees,
    the cover reaches lower than the position by `hem` and higher, whatever its distance. Returns (model, triangles
    before, after)."""
    from scipy.spatial import cKDTree
    P = G.positions(model)
    lists = part_lists(model)
    if cover is None:
        idx = sorted({r[0] for p, tris in lists if model[p + 0xC] not in mats for t in tris for r in t})
        C = P[idx]
    else:
        C = G.positions(cover)
    tree = cKDTree(C)
    dirs, _ = outward(P)
    hidden = np.zeros(len(P), bool)
    if hem:
        ca = np.degrees(np.arctan2(C[:, 0], C[:, 2] + 10.0))
    for i in range(len(P)):
        if (front and abs(P[i][0]) < front and P[i][2] > 0) or abs(P[i][0]) < minx:
            continue
        if hem:
            a = np.degrees(np.arctan2(P[i][0], P[i][2] + 10.0))
            band = np.abs((ca - a + 180.0) % 360.0 - 180.0) < 8.0
            if band.any() and C[band, 1].min() < P[i][1] - hem and C[band, 1].max() > P[i][1] + hem:
                hidden[i] = True
                continue
        near = tree.query_ball_point(P[i], reach)
        if not near:
            continue
        d = C[near] - P[i]
        along = d @ dirs[i]
        side = np.linalg.norm(d - along[:, None] * dirs[i], axis=1)
        hidden[i] = bool(((along > 0) & (side < 0.35 * along + 10.0)).any())
    kept = [[t for t in tris if (mats is not None and model[p + 0xC] not in mats) or not all(hidden[r[0]] for r in t)]
            for p, tris in lists]
    before = sum(len(t) for _, t in lists)
    after = sum(len(t) for t in kept)
    return (rebuild(model, kept) if after < before else model), before, after


def inside(model, cover, near=25.0, inset=5.0, low=1150.0):
    """Pushes `model`'s positions that poke out through `cover` (a shirt through a jacket's shoulders) back under it:
    a position whose nearest point of `cover` lies within `near` and inwards of it (towards the skeleton) goes `inset`
    under that point. Where the cover is further (the jacket's open front) the model stays. Returns (model, how many)."""
    from scipy.spatial import cKDTree
    P = G.positions(model)
    C = G.positions(cover)
    d, j = cKDTree(C).query(P)
    dirs, _ = outward(P)
    out = bytearray(model)
    n = 0
    for i in range(len(P)):
        if d[i] > near or P[i][1] < low:
            continue  # far from the cover, or below the jacket's body (the legs by its holsters stay)
        along = float((P[i] - C[j[i]]) @ dirs[i])
        if along <= -inset:
            continue
        G.set_position(out, i, P[i] - dirs[i] * (along + inset))
        n += 1
    return bytes(out), n


def cover(hair, head, centre=(0.0, 1715.0, -15.0), gap=4.0, low=1700.0, cone=12.0):
    """Lifts a hair made for another head over this one's skull (DevJunior, 09.11.2026: the Remake's head under
    Black Coat's hair, its scalp showed through): seen from the skull's centre, every hair position above y `low`
    lies at least `gap` further out than the head's surface in its direction (within `cone` degrees); the push is
    shared with the neighbouring positions so the cards keep their shape. Returns (model, how many moved, the most)."""
    from scipy.spatial import cKDTree
    c = np.array(centre)
    P = G.positions(hair)
    H = G.positions(head)
    H = H[H[:, 1] > low - 30]
    hd = H - c
    hr = np.linalg.norm(hd, axis=1)
    hu = hd / hr[:, None]
    cos = np.cos(np.radians(cone))
    push = np.zeros(len(P))
    for i, p in enumerate(P):
        if p[1] < low:
            continue
        d = p - c
        r = np.linalg.norm(d)
        u = d / r
        sel = hu @ u > cos
        if not sel.any():
            continue
        want = hr[sel].max() + gap
        if r < want:
            push[i] = want - r
    tree = cKDTree(P)
    smooth = np.array([push[tree.query_ball_point(P[i], 15.0)].max() if P[i][1] >= low - 20 else 0.0
                       for i in range(len(P))])
    out = bytearray(hair)
    n = 0
    for i in np.nonzero(smooth > 0.05)[0]:
        d = P[i] - c
        G.set_position(out, i, P[i] + d / np.linalg.norm(d) * smooth[i])
        n += 1
    return bytes(out), n, float(smooth.max())


def cut(model, top):
    """Drops the triangles of `model` that reach above y `top` (the body's upper half under a jacket model that brings
    its own shirt). Returns (model, triangles before, after)."""
    P = G.positions(model)
    lists = part_lists(model)
    kept = [[t for t in tris if max(P[r[0]][1] for r in t) <= top] for _, tris in lists]
    before = sum(len(t) for _, t in lists)
    after = sum(len(t) for t in kept)
    return (rebuild(model, kept) if after < before else model), before, after


def back(model, z, top):
    """Drops the triangles of `model` lying wholly behind z `z` and below y `top`: a shirt's back under a jacket whose
    back is closed (DevJunior, 12.11.2026: "the blue spot of the shirt on the back": it poked through the coat as he
    moved, where --hide and --inside leave it). Returns (model, triangles before, after)."""
    P = G.positions(model)
    lists = part_lists(model)
    kept = [[t for t in tris if not all(P[r[0]][2] < z and P[r[0]][1] < top for r in t)] for _, tris in lists]
    before = sum(len(t) for _, t in lists)
    after = sum(len(t) for t in kept)
    return (rebuild(model, kept) if after < before else model), before, after


def neck(model, low, high, scale, axis=None):
    """The neck thickened: positions below `high` move away from the neck's axis (x, z; by default the middle of the
    positions between `low` and `high`) in x and z by `scale`, in full below `low`, fading to nothing at `high`
    (DevJunior, 12.11.2026, on v13: "the neck is not in proportion": the RE9 head's neck stood thin over the Lite's
    wide shoulders). Only the front and sides (from 10 behind the axis, in full 20 in front of it): the nape stays.
    Shapes are offsets and move along. Returns (model, positions moved, axis)."""
    import numpy as np
    model = bytearray(model)
    P = G.positions(model)
    if axis is None:
        band = P[(P[:, 1] > low - 60.0) & (P[:, 1] < low)]
        axis = ((band[:, 0].min() + band[:, 0].max()) / 2, (band[:, 2].min() + band[:, 2].max()) / 2)
    # front and sides only: the nape stays (thickened, it came through the back of the shirt's collar)
    k = 1.0 + (scale - 1.0) * np.clip((high - P[:, 1]) / max(high - low, 1e-6), 0.0, 1.0) * \
        np.clip((P[:, 2] - axis[1] + 10.0) / 30.0, 0.0, 1.0)
    moved = 0
    for i in np.flatnonzero(k > 1.0 + 1e-4):
        q = P[i].copy()
        q[0] = axis[0] + (q[0] - axis[0]) * k[i]
        q[2] = axis[1] + (q[2] - axis[1]) * k[i]
        G.set_position(model, i, q)
        moved += 1
    return bytes(model), moved, axis


def hug(model, head, low, margin=6.0, fade=40.0, top=1660.0, bins=48, reach=45.0):
    """A garment's collar drawn in to the neck: positions of `model` above `low` (to `top`) lying farther from the
    neck's axis than the neck itself in their direction (head positions at that height, +- 12, per angle bin) plus
    `margin` (at most `reach` beyond it: the collar, not the shoulders) move in to that distance, in full `fade` above
    `low` (DevJunior, 13.11.2026, on v14: "a little hole
    inside the collar at some angles": the shirt's opening was wider than the RE9 neck, the sky showed between them
    when the body leant). Returns (model, positions moved, most)."""
    import numpy as np
    model = bytearray(model)
    P, H = G.positions(model), G.positions(head)
    band = H[(H[:, 1] > low - 60.0) & (H[:, 1] < low)]
    ax, az = (band[:, 0].min() + band[:, 0].max()) / 2, (band[:, 2].min() + band[:, 2].max()) / 2
    ha = np.arctan2(H[:, 2] - az, H[:, 0] - ax)
    hr = np.hypot(H[:, 0] - ax, H[:, 2] - az)
    hb = ((ha + np.pi) / (2 * np.pi) * bins).astype(int) % bins
    moved, most = 0, 0.0
    for i in np.flatnonzero((P[:, 1] > low) & (P[:, 1] < top)):
        q = P[i].copy()
        a = np.arctan2(q[2] - az, q[0] - ax)
        b = int((a + np.pi) / (2 * np.pi) * bins) % bins
        near = (np.abs(H[:, 1] - q[1]) < 12.0) & ((hb == b) | (hb == (b + 1) % bins) | (hb == (b - 1) % bins))
        if not near.any():
            continue
        r, want = np.hypot(q[0] - ax, q[2] - az), hr[near].max() + margin
        if r <= want or r - want > reach:  # the collar only, not the shoulders
            continue
        k = min(1.0, (q[1] - low) / fade)
        nr = r + (want - r) * k
        q[0], q[2] = ax + (q[0] - ax) * nr / r, az + (q[2] - az) * nr / r
        G.set_position(model, i, q)
        moved += 1
        most = max(most, r - nr)
    return bytes(model), moved, most


def prune(entries, tpl, users):
    """Palette `tpl` with only the textures its models `users` draw; the parts renumbered. Returns (bytes before,
    after)."""
    imgs = G.tpl_images(entries[tpl])
    used = set()
    for e in users:
        b = entries[e]
        if not b or len(b) < 0x60:
            continue
        for p in G.parts(b):
            if G.is_empty(b, p):
                continue
            fields = [0xC, 0xD, 0xE] + ([0x17] if b[p + 0xB] & 0x10 else [])
            used.update(b[p + f] for f in fields if b[p + f] != 0xFF)
    order = sorted(t for t in used if t < len(imgs))
    remap = {old: new for new, old in enumerate(order)}
    for e in users:
        b = bytearray(entries[e]) if entries[e] else None
        if not b or len(b) < 0x60:
            continue
        for p in G.parts(b):
            if G.is_empty(b, p):
                continue
            fields = [0xC, 0xD, 0xE] + ([0x17] if b[p + 0xB] & 0x10 else [])
            for f in fields:
                if b[p + f] != 0xFF and b[p + f] in remap:
                    b[p + f] = remap[b[p + f]]
        struct.pack_into(">I", b, 0x24, len(order))
        entries[e] = bytes(b)
    before = len(entries[tpl])
    entries[tpl] = G.build_tpl([imgs[t] for t in order])
    return before, len(entries[tpl]), len(imgs), len(order)


def carry(model, P, moves, radius=40.0):
    """Moves model's positions as the nearest of P moved (moves: the push per position of P), by the pushes within
    `radius` of the nearest, averaged."""
    from scipy.spatial import cKDTree
    tree = cKDTree(P)
    R = G.positions(model)
    out = bytearray(model)
    d, j = tree.query(R)
    n = 0
    for i in range(len(R)):
        near = tree.query_ball_point(P[j[i]], radius)
        m = moves[near].mean(0) if near else moves[j[i]]
        if np.linalg.norm(m) > 0.05:
            G.set_position(out, i, R[i] + m)
            n += 1
    return bytes(out), n


# ---- main -----------------------------------------------------------------------------------------------------------


def entries_of(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += range(int(a), int(b) + 1)
        else:
            out.append(int(part))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("out")
    ap.add_argument("--take", action="append", default=[], metavar="SRC:ENTRIES")
    ap.add_argument("--cards", action="append", default=[], metavar="ENTRY:KEEP")
    ap.add_argument("--fit", action="append", default=[], metavar="ENTRY[@MATS]=SRC:E[@MATS]",
                    help="push ENTRY's positions (of its parts with MATS) out to the surface of SRC's model E (of its "
                         "parts with MATS)")
    ap.add_argument("--collar", action="append", default=[], metavar="ENTRY=SRC:E")
    ap.add_argument("--tuck", action="append", default=[], metavar="HEAD:BODY",
                    help="keep the base of head HEAD's neck under the collar of model BODY")
    ap.add_argument("--carry", action="append", default=[], metavar="ENTRY:LIST")
    ap.add_argument("--hide", action="append", default=[], metavar="ENTRY[@MATS]:COVER[:front=X,minx=X]",
                    help="drop ENTRY's triangles (with @MATS: of these parts) that model COVER hides")
    ap.add_argument("--inside", action="append", default=[], metavar="ENTRY:COVER",
                    help="push ENTRY's positions that poke out through model COVER back under it")
    ap.add_argument("--cover", action="append", default=[], metavar="HAIR:HEAD",
                    help="lift hair model HAIR over the skull of head model HEAD (a hair made for another head)")
    ap.add_argument("--cut", action="append", default=[], metavar="ENTRY:Y",
                    help="drop ENTRY's triangles that reach above y Y")
    ap.add_argument("--back", action="append", default=[], metavar="ENTRY:Z:Y",
                    help="drop ENTRY's triangles wholly behind z Z and below y Y (a shirt's back under a closed coat)")
    ap.add_argument("--compact", action="append", default=[], metavar="ENTRY",
                    help="drop the positions, normals, colours and texture coordinates nothing in ENTRY draws (after "
                         "skin_graft.py --drop emptied parts)")
    ap.add_argument("--prune", action="append", default=[], metavar="TPL",
                    help="keep only the textures of palette TPL that its models draw (Leon's 1: 0, 6, 9, 12; 3: 2, 4, 5)")
    ap.add_argument("--girth", action="append", default=[], metavar="ENTRY@MATS=SRC:E@MATS",
                    help="widen ENTRY's parts with MATS around the body's axis to the girth of SRC's model E's parts "
                         "with MATS (see girth; --fit-top: its top, default 1470)")
    ap.add_argument("--hug", action="append", default=[], metavar="ENTRY:HEAD:LOW[:MARGIN]",
                    help="draw ENTRY's collar in to HEAD's neck above LOW (after --neck)")
    ap.add_argument("--neck", action="append", default=[], metavar="ENTRY:LOW:HIGH:SCALE",
                    help="thicken the neck: x and z around its axis by SCALE below LOW, fading out at HIGH")
    ap.add_argument("--fit-top", type=float, help="--fit only below this height (fading over 40 units)")
    ap.add_argument("--most", type=float, default=70.0, help="--fit's largest push (default 70)")
    ap.add_argument("--passes", type=int, default=2, help="--fit's rounds (default 2: the smoothing leaves some of the "
                    "way after the first)")
    a = ap.parse_args(argv)
    base = U.Archive(open(a.base, "rb").read(), ">")
    entries = [base.entry(i) for i in range(len(base.offsets))]
    archives = {}

    def arc(path):
        if path not in archives:
            archives[path] = U.Archive(open(path, "rb").read(), ">")
        return archives[path]

    for spec in a.take:
        src, which = spec.rsplit(":", 1)
        for e in entries_of(which):
            entries[e] = arc(src).entry(e)
        print("entries %s from %s" % (which, src))
    for spec in a.cards:
        e, keep = spec.split(":")
        before = len(G.positions(entries[int(e)]))
        entries[int(e)], pieces, kept = thin_cards(entries[int(e)], float(keep))
        print("entry %s: %d of %d pieces kept, %d -> %d positions, %d bytes" %
              (e, kept, pieces, before, len(G.positions(entries[int(e)])), len(entries[int(e)])))
    moved = {}
    for spec in a.fit:
        e, ref = spec.split("=")
        e, _, tm = e.partition("@")
        src, re_ = ref.rsplit(":", 1)
        re_, _, rm = re_.partition("@")
        tmats = {int(x) for x in tm.split(",")} if tm else None
        rmats = {int(x) for x in rm.split(",")} if rm else None
        P0 = G.positions(entries[int(e)])
        for _ in range(max(1, a.passes)):
            entries[int(e)], _, _ = fit(entries[int(e)], arc(src).entry(int(re_)), a.most, target_mats=tmats,
                                        ref_mats=rmats, top=a.fit_top)
        push = G.positions(entries[int(e)]) - P0
        P = P0
        moved[int(e)] = (P, push)
        n = np.linalg.norm(push, axis=1)
        print("entry %s fitted to %s's %s: %d of %d positions pushed out, at most %.0f, mean %.0f" %
              (e, src, re_, (n > 0.05).sum(), len(n), n.max(), n[n > 0.05].mean() if (n > 0.05).any() else 0))
    for spec in a.girth:
        e, ref = spec.split("=")
        e, _, tm = e.partition("@")
        src, re_ = ref.rsplit(":", 1)
        re_, _, rm = re_.partition("@")
        entries[int(e)], n, ratio = girth(entries[int(e)], arc(src).entry(int(re_)), {int(x) for x in tm.split(",")},
                                          {int(x) for x in rm.split(",")}, top=a.fit_top or 1470.0)
        print("entry %s widened to the girth of %s's %s: %d positions, ratio up to %.2f (mean %.2f where > 1)" %
              (e, src, re_, n, ratio.max(), ratio[ratio > 1.001].mean() if (ratio > 1.001).any() else 1.0))
    for spec in a.collar:
        e, ref = spec.split("=")
        src, re_ = ref.rsplit(":", 1)
        entries[int(e)], P, mv = collar(entries[int(e)], arc(src).entry(int(re_)))
        n = np.linalg.norm(mv, axis=1)
        print("entry %s: collar moved onto %s's model %s: %d positions moved, at most %.0f" %
              (e, src, re_, (n > 0.05).sum(), n.max()))
        # the straps keep to the body's --fit only: the collar's move would pull their ends near the neck out of it
    for spec in a.tuck:
        h, b = (int(x) for x in spec.split(":"))
        entries[h], n = tuck(entries[h], entries[b])
        print("entry %d: the neck's base kept under entry %d's collar, %d positions moved" % (h, b, n))
    for spec in a.carry:
        e, which = spec.split(":")
        P, push = moved[int(e)]
        for k in entries_of(which):
            entries[k], n = carry(entries[k], P, push)
            print("entry %d carried along with entry %s: %d positions moved" % (k, e, n))
    for spec in a.hide:
        left, c, *opts = spec.split(":")
        e, _, ms = left.partition("@")
        e, c = int(e), int(c)
        mats = {int(x) for x in ms.split(",")} if ms else None
        kw = {k: float(v) for k, v in (o.split("=") for o in (opts[0].split(",") if opts else []))}
        entries[e], before, after = hide(entries[e], None if c == e else entries[c], mats=mats, **kw)
        print("entry %d: %d of %d triangles hidden under entry %d dropped, %d positions left" %
              (e, before - after, before, c, len(G.positions(entries[e]))))
    for spec in a.cover:
        h, hd = (int(x) for x in spec.split(":"))
        entries[h], n, most = cover(entries[h], entries[hd])
        print("entry %d: %d positions lifted over entry %d's skull, at most %.0f" % (h, n, hd, most))
    for spec in a.inside:
        e, c = (int(x) for x in spec.split(":"))
        entries[e], n = inside(entries[e], entries[c])
        print("entry %d: %d positions pushed back under entry %d" % (e, n, c))
    for spec in a.cut:
        e, y = spec.split(":")
        entries[int(e)], before, after = cut(entries[int(e)], float(y))
        print("entry %s: %d of %d triangles above y %s dropped, %d positions left" %
              (e, before - after, before, y, len(G.positions(entries[int(e)]))))
    for spec in a.back:
        e, z, y = spec.split(":")
        entries[int(e)], before, after = back(entries[int(e)], float(z), float(y))
        print("entry %s: %d of %d triangles behind z %s below y %s dropped" % (e, before - after, before, z, y))
    for spec in a.neck:
        e, lo, hi, sc = spec.split(":")
        entries[int(e)], n, ax = neck(entries[int(e)], float(lo), float(hi), float(sc))
        print("entry %s: neck thickened %s times below y %s (fading out at %s) around x %.0f z %.0f, %d positions" %
              (e, sc, lo, hi, ax[0], ax[1], n))
    for spec in a.hug:
        f = spec.split(":")
        e, h = int(f[0]), int(f[1])
        entries[e], n, most = hug(entries[e], entries[h], float(f[2]), float(f[3]) if len(f) > 3 else 6.0)
        print("entry %d: collar drawn in to entry %d's neck above y %s: %d positions, at most %.0f units" %
              (e, h, f[2], n, most))
    for spec in a.compact:
        e = int(spec)
        before = len(G.positions(entries[e]))
        entries[e] = rebuild(entries[e], [t for _, t in part_lists(entries[e])])
        print("entry %d: %d -> %d positions" % (e, before, len(G.positions(entries[e]))))
    for spec in a.prune:
        t = int(spec)
        before, after, n, kept = prune(entries, t, U.PLAYER_TPL_USERS.get(t, (t - 1,)))
        print("palette %d: %d of %d textures kept, %d -> %d bytes" % (t, kept, n, before, after))
    data = U.build_archive(base, U.build_body(entries, base.types))
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's place holds %d)" %
          (a.out, len(data), len(U.build_body(entries, base.types)), 0x800000))


if __name__ == "__main__":
    main()
