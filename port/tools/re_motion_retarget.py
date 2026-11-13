#!/usr/bin/env python3
"""Retargets a motion of the 2023 Resident Evil 4 (RE Engine, read by re_motlist.py) onto a GameCube skeleton and
writes it as the game's MotionData ("FCV", tools/motion/fcv.py), for the RE9 melee finishers DevJunior brought
(13.11.2026: "these animations for executions and melee attacks, turn this into the remake, its controls too").

    re_motion_retarget.py MOTLIST OUT.fcv [--motion N] --model SKIN.drs[:ENTRY] --map leon|ganado
                          [--start F] [--end F] [--step 2] [--tolerance 0.004] [--no-fingers] [--check]

How: the source skeleton's bones are played forward (the tracks' local translations and rotations); for every
GameCube part the map names a bone, the part's world rotation is that bone's world rotation relative to its bind
pose, times the rotation that turns the part's rest direction (the GameCube models rest in a T pose, rotations
zero) into the bone's bind direction: G(t) = W(t) * W(bind)^-1 * A. Parts between mapped ones follow their parent
(local rotation zero); the model's blend table still sets the double joints (shoulders, elbows) as the game does.
Local rotations become the game's Euler angles (RotMatrix: Rz * Ry * Rx), unwrapped; part 0 gets the hips'
position (scaled from metres by the hips' heights). Frames: the source runs at 60 per second, the game at 30
(--step 2). Keys: one per frame, then thinned while the game's Hermite curve stays within --tolerance radians
(millimetres / 4 for the position); f32 values with s16 slopes (Fcc type 1).

--check plays the result with the game's own code (tools/motion/evalhost) and reports how far the wrists, ankles
and head end up from the source's (scaled) positions.

No IK: the motion keys rotations only (the game's own motions aim arms and legs with IK effector keys; these
do not need them). Game files never go into the repository; this tool only reads and writes the paths given.
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent))
import re_motlist as R  # noqa: E402
import uhd_skin_convert as U  # noqa: E402
from tools.motion import fcv, modelbin  # noqa: E402

# GameCube part -> RE Engine bone. Leon (pl00..pl10 bodies, 119 parts): 0 hips, 1 waist, 2 chest, 3 neck, 4 head,
# 5/11 clavicles, 7/13 upper arms, 8/14 forearms, 9/15 forearm twist, 10/16 hands, 34..48 / 49..63 the right /
# left fingers (thumb, index, middle, ring, little, three each), 17 pelvis, 18..21 / 22..25 right / left thigh,
# shin, foot, toe. The character's right side is -x on both skeletons, both face +z, y up.
FINGERS = ["Thumb", "IndexF", "MiddleF", "RingF", "PinkyF"]
LEON = {0: "Hip", 1: "Spine_0", 2: "Spine_2", 3: "Neck_0", 4: "Head",
        5: "R_Shoulder", 7: "R_UpperArm", 8: "R_Forearm", 10: "R_Hand",
        11: "L_Shoulder", 13: "L_UpperArm", 14: "L_Forearm", 16: "L_Hand",
        18: "R_Thigh", 19: "R_Shin", 20: "R_Foot", 21: "R_Toe",
        22: "L_Thigh", 23: "L_Shin", 24: "L_Foot", 25: "L_Toe"}
LEON_FINGERS = {}
for side, base in (("R_", 34), ("L_", 49)):
    for f, name in enumerate(FINGERS):
        for k in range(3):
            LEON_FINGERS[base + 3 * f + k] = "%s%s%d" % (side, name, k + 1)
# direction of each mapped part at rest: towards this part (its first child on the chain)
LEON_NEXT = {0: 1, 1: 2, 2: 3, 3: 4, 5: 7, 7: 8, 8: 10, 10: 40, 11: 13, 13: 14, 14: 16, 16: 55,
             18: 19, 19: 20, 20: 21, 22: 23, 23: 24, 24: 25}
# The Ganados (em10.drs bodies: 34..61 parts) share Leon's parts 0..25 and their rest pose; no finger parts.
GANADO_NEXT = {k: v for k, v in LEON_NEXT.items() if k not in (10, 16)}
MAPS = {"leon": (LEON, LEON_FINGERS, LEON_NEXT), "ganado": (LEON, {}, GANADO_NEXT)}


# ---------------------------------------------------------------- quaternions (x, y, z, w) and matrices
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qmat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def align(a, b):
    """The smallest rotation (matrix) turning direction a into direction b."""
    a = a / (np.linalg.norm(a) or 1.0)
    b = b / (np.linalg.norm(b) or 1.0)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if c < -0.999999:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1.0 + c)


def euler(m):
    """The game's angles (RotMatrix: m = Rz(z) * Ry(y) * Rx(x)) of a rotation matrix."""
    y = -math.asin(max(-1.0, min(1.0, m[2, 0])))
    if abs(m[2, 0]) < 0.99999:
        x = math.atan2(m[2, 1], m[2, 2])
        z = math.atan2(m[1, 0], m[0, 0])
    else:  # gimbal: z carries it
        x = 0.0
        z = math.atan2(-m[0, 1], m[1, 1])
    return x, y, z


# ---------------------------------------------------------------- the source skeleton played forward
class Source:
    def __init__(self, motion):
        self.m = motion
        self.bones = motion.bones
        self.by_name = {b.name: i for i, b in enumerate(self.bones)}
        self.track = {}
        for (bi, h), per in motion.tracks.items():
            idx = next((i for i, b in enumerate(self.bones) if b.hash == h), bi)
            self.track[idx] = per

    def pose(self, frame=None):
        """World rotation matrices and positions of every bone (frame None: the bind pose)."""
        n = len(self.bones)
        rot, pos = [None] * n, [None] * n
        for i, b in enumerate(self.bones):
            t, q = b.trans, b.quat
            if frame is not None and i in self.track:
                per = self.track[i]
                if R.TRANSLATION in per:
                    t = per[R.TRANSLATION].sample(frame)
                if R.ROTATION in per:
                    q = per[R.ROTATION].sample(frame)
            lr, lt = qmat(q), np.array(t[:3], dtype=np.float64)
            if b.parent < 0:
                rot[i], pos[i] = lr, lt
            else:
                rot[i] = rot[b.parent] @ lr
                pos[i] = pos[b.parent] + rot[b.parent] @ lt
        return rot, pos


def retarget(src, model, mapping, nexts, frames, scale):
    """Per frame: {part: local rotation matrix} and the hips' position (model space, mm)."""
    parts = model.parts
    rest_world = {}
    for p in parts:
        rest_world[p.no] = np.array(p.pos) + (rest_world[p.parent] if p.parent >= 0 else 0.0)
    bind_rot, bind_pos = src.pose(None)
    idx = {part: src.by_name[name] for part, name in mapping.items() if name in src.by_name}
    # A: the part's rest direction turned into the bone's bind direction (world)
    # first the whole character: the source's bind pose stands as its root bone turns it (the 2023 game's: turned
    # round, facing -z), then each bone's swing from the part's rest direction to its bind direction
    root = next(i for i, b in enumerate(src.bones) if b.parent < 0)
    Q = bind_rot[root]
    A = {}
    for part, bi in idx.items():
        nxt = nexts.get(part)
        A[part] = Q
        if nxt is None or nxt not in idx:
            continue
        d_gc = Q @ (rest_world[nxt] - rest_world[part])
        d_re = bind_pos[idx[nxt]] - bind_pos[bi]
        if np.linalg.norm(d_gc) > 1e-6 and np.linalg.norm(d_re) > 1e-9:
            A[part] = align(d_gc, d_re) @ Q
    inv_bind = {part: bind_rot[bi].T for part, bi in idx.items()}
    out = []
    for f in frames:
        rot, pos = src.pose(f)
        G = {}
        local = {}
        for p in parts:  # parents come first (modelbin.check_tree)
            par = G.get(p.parent, np.eye(3)) if p.parent >= 0 else np.eye(3)
            if p.no in idx:
                g = rot[idx[p.no]] @ inv_bind[p.no] @ A[p.no]
                local[p.no] = par.T @ g
            else:
                g = par
            G[p.no] = g
        hip = pos[idx[0]] * scale if 0 in idx else np.zeros(3)
        out.append((local, hip, {part: pos[bi] * scale for part, bi in idx.items()}))
    return out


# ---------------------------------------------------------------- curves: Hermite thinning
def hermite(p0, p1, m0, m1, t):
    t2, t3 = t * t, t * t * t
    return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1


def thin(values, tol):
    """Key frame indices of a dense curve (one value per frame) the game's Hermite curves follow within tol
    (slopes: central differences per frame, as stored; MotionMove scales them by the segment's length)."""
    n = len(values)
    if n <= 2:
        return list(range(n))
    v = np.asarray(values)
    slope = np.gradient(v)
    keys = [0]
    i = 0
    while i < n - 1:
        j = min(n - 1, i + 1)
        best = j
        while j < n:
            span = j - i
            ok = True
            for k in range(i + 1, j):
                t = (k - i) / span
                if abs(hermite(v[i], v[j], slope[i] * span, slope[j] * span, t) - v[k]) > tol:
                    ok = False
                    break
            if not ok:
                break
            best = j
            j += 1
        keys.append(best)
        i = best
    return keys


def axis_keys(values, tol):
    v = np.asarray(values, dtype=np.float64)
    slope = np.gradient(v) if len(v) > 1 else np.zeros(len(v))
    ks = thin(v, tol)
    return fcv.Axis(frames=ks, keys=[(float(v[k]), int(round(max(-3.2767, min(3.2767, slope[k])) * 10000)),
                                      int(round(max(-3.2767, min(3.2767, slope[k])) * 10000))) for k in ks])


def axis_keys_pos(values, tol):
    v = np.asarray(values, dtype=np.float64)
    slope = np.gradient(v) if len(v) > 1 else np.zeros(len(v))
    ks = thin(v, tol)
    return fcv.Axis(frames=ks, keys=[(float(v[k]), float(slope[k]), float(slope[k])) for k in ks])


def build_motion(rows, parts_order, tol):
    """FCV from per-frame local rotations and hips positions."""
    n = len(rows)
    joints = []
    # part 0: position (kind 4), f32 keys and slopes
    hip = np.array([r[1] for r in rows])
    j = fcv.Joint(kind=4, channel=0, fcc_type=0, parts_no=0)
    j.axes = [axis_keys_pos(hip[:, a], tol * 250.0) for a in range(3)]
    joints.append(j)
    for part in parts_order:
        ang = np.array([euler(r[0][part]) for r in rows])
        ang = np.unwrap(ang, axis=0)
        j = fcv.Joint(kind=2, channel=0, fcc_type=1, parts_no=part)
        j.axes = [axis_keys(ang[:, a], tol) for a in range(3)]
        joints.append(j)
    return fcv.Motion(max_frame=n - 1, joints=joints, layout=list(range(len(joints))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("motlist")
    ap.add_argument("out")
    ap.add_argument("--motion", type=int, default=0)
    ap.add_argument("--model", required=True, help="the target skin or character archive (.drs[:entry], body 0)")
    ap.add_argument("--map", default="leon", choices=sorted(MAPS))
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", type=float, default=None)
    ap.add_argument("--step", type=float, default=2.0, help="source frames per game frame (60 -> 30 per second)")
    ap.add_argument("--tolerance", type=float, default=0.004)
    ap.add_argument("--no-fingers", action="store_true")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    _ver, mots = R.read_motlist(a.motlist)
    m = mots[a.motion]
    path, _, ent = a.model.partition(":")
    arc = U.Archive(Path(path).read_bytes(), ">")
    model = modelbin.parse(arc.entry(int(ent) if ent else 0))
    model.check_tree()
    body, fingers, nexts = MAPS[a.map]
    mapping = dict(body)
    if not a.no_fingers:
        mapping.update(fingers)
    mapping = {p: n for p, n in mapping.items() if p < model.n_parts}
    src = Source(m)
    missing = sorted(set(mapping.values()) - set(src.by_name))
    if missing:
        print("not in the source: %s" % ", ".join(missing))
    _r, bpos = src.pose(None)
    hip_src = bpos[src.by_name["Hip"]][1]
    hip_gc = model.parts[0].pos[1]
    scale = hip_gc / hip_src
    end = m.frames if a.end is None else a.end
    frames = list(np.arange(a.start, end + 1e-6, a.step))
    rows = retarget(src, model, mapping, nexts, frames, scale)
    order = sorted(p for p in mapping if p in src.by_name.values() or True)
    order = [p for p in sorted(mapping) if mapping[p] in src.by_name and p != 0] + ([0] if "Hip" in src.by_name else [])
    mot = build_motion(rows, order, a.tolerance)
    data = fcv.serialise(mot)
    Path(a.out).write_bytes(data)
    keys = sum(len(ax.frames) for j in mot.joints for ax in j.axes)
    print("%s: %s, frames %g..%g every %g -> %d game frames, %d joints, %d keys, %d bytes (scale %.0f mm/m)" % (
        a.out, m.name, a.start, end, a.step, len(frames), len(mot.joints), keys, len(data), scale))
    if a.check:
        # the game's own playback: per chain bone (a part and the next on its chain) the angle between its direction
        # and the source bone's (the skeletons' proportions differ, the directions must agree)
        from tools.motion import evalhost
        pl = evalhost.Player(model, fcv.parse(data), ik=False)
        names = {p: mapping[p] for p in nexts if p in mapping and nexts[p] in mapping}
        worst, errs = {}, []
        for fi in range(0, len(frames), max(1, len(frames) // 40)):
            pose = pl.frame(fi)
            target = rows[fi][2]
            for part, nm in names.items():
                nx = nexts[part]
                if part not in target or nx not in target:
                    continue
                g = np.array(pose.world[nx]) - np.array(pose.world[part])
                r = target[nx] - target[part]
                if np.linalg.norm(g) < 1e-6 or np.linalg.norm(r) < 1e-6:
                    continue
                ang = math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(g, r) / np.linalg.norm(g) /
                                                                       np.linalg.norm(r))))))
                worst[nm] = max(worst.get(nm, 0.0), ang)
                errs.append(ang)
        print("check (the game's own playback; bone directions against the source's): mean %.1f deg, worst %s" % (
            float(np.mean(errs)) if errs else 0.0,
            ", ".join("%s %.1f" % kv for kv in sorted(worst.items(), key=lambda kv: -kv[1])[:6])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
