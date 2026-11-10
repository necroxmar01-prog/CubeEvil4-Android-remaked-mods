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


def fit(target, ref, most=70.0, smooth=30.0):
    """Moves target's positions out to ref's surface along `outward` (see --fit); returns (new model, the push per
    position)."""
    from scipy.spatial import cKDTree
    P = G.positions(target)
    Q = G.positions(ref)
    # ref's surface: its positions and the centres of its triangles, dense enough to stand for it
    cents = []
    for _, tris in part_lists(ref):
        if tris:
            idx = np.array([[r[0] for r in t] for t in tris])
            cents.append(Q[idx].mean(1))
    S = np.concatenate([Q] + cents)
    tree = cKDTree(S)
    dirs, w = outward(P)
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
    smoothed = np.array([push[ptree.query_ball_point(P[i], smooth)].mean() for i in range(len(P))])
    out = bytearray(target)
    for i in range(len(P)):
        if smoothed[i] > 0.05:
            G.set_position(out, i, P[i] + dirs[i] * smoothed[i])
    return bytes(out), P, dirs * smoothed[:, None]


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
    ap.add_argument("--fit", action="append", default=[], metavar="ENTRY=SRC:E")
    ap.add_argument("--collar", action="append", default=[], metavar="ENTRY=SRC:E")
    ap.add_argument("--tuck", action="append", default=[], metavar="HEAD:BODY",
                    help="keep the base of head HEAD's neck under the collar of model BODY")
    ap.add_argument("--carry", action="append", default=[], metavar="ENTRY:LIST")
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
        src, re_ = ref.rsplit(":", 1)
        P0 = G.positions(entries[int(e)])
        for _ in range(max(1, a.passes)):
            entries[int(e)], _, _ = fit(entries[int(e)], arc(src).entry(int(re_)), a.most)
        push = G.positions(entries[int(e)]) - P0
        P = P0
        moved[int(e)] = (P, push)
        n = np.linalg.norm(push, axis=1)
        print("entry %s fitted to %s's %s: %d of %d positions pushed out, at most %.0f, mean %.0f" %
              (e, src, re_, (n > 0.05).sum(), len(n), n.max(), n[n > 0.05].mean() if (n > 0.05).any() else 0))
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
    data = U.build_archive(base, U.build_body(entries, base.types))
    Path(a.out).write_bytes(data)
    print("%s: %d bytes, data part %d bytes (the player's place holds %d)" %
          (a.out, len(data), len(U.build_body(entries, base.types)), 0x600000))


if __name__ == "__main__":
    main()
