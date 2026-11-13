#!/usr/bin/env python3
"""Builds the finishers' archive of a mod pack (mods/<pack>/melee/finish.dat, runtime/finisher.cpp) from the PC mod
"RE9 Melee - Hatchet - Finishers for Leon" 1.0.1 (wish of DevJunior, 13.11.2026: "these animations for executions and
melee attacks, turn this into the remake, its controls too").

    re9_finisher_pack.py MOD_DIR OUT/melee/finish.dat --leon pl00.drs --ganado em10.drs:440 [--only h4,c2]
                         [--tolerance 0.004] [--check]

MOD_DIR: the mod's folder (or its natives/stm/_re9_melee_proto). --leon / --ganado: the game's (or a skin's) archives
whose first model gives the skeletons the motions are fitted to (Leon's body; a Ganado's body, em10.drs entry 440).

Each finisher is a pair: Leon's motion and the enemy's, played together (re_motion_retarget.py fits each to its
skeleton). Their root bones become the game's root motion (joint kinds 1 / 0x40): both move on the floor as in the
PC game, from where the catch puts them (the enemy's start in front of Leon, as the PC mod's placement). The PC game
runs at 60 frames per second, this one at 30: every second frame.

  h4         the hatchet finisher on a kneeling enemy (any weapon: Leon draws his knife, a skin's axe)
  knee-a/b/c the 2023 game's own knife decapitations of a kneeling Ganado (verA..C; the mod ships them in its
             _chainsaw folder, Leon's chc0_jack_pl_0th_com_1020/1025/1030 and the Ganado's chc0_grapple_0th_com_*):
             the kill on the cut (the hand's fastest frame after the grab), aligned on the grab
  c2         the handgun finisher on a standing stunned enemy
  c3c-*      the shotguns' and machine guns' (shotgun, riot gun, striker, TMP, Chicago Typewriter)
  c4b2-*     the rifles' (rifle, semi-auto rifle)
  c5b-*      the magnums' (Broken Butterfly, Killer7 / Handcannon)

The archive: a DAT (u32 count, three zero words, u32 offsets, 4-byte tags) whose entry 0 ("FIN") lists the finishers
(big-endian, 60 bytes each after an 8-byte head "FIN1" + count), then the motions ("FCV") and the enemies' sequences
("SEQ": one key per frame; Free 1 on the kill, 0x80 once the body is down, for em10.cpp Dm_NeckBreak). A finisher:

  u8 kind (0 a standing stunned enemy, 1 a kneeling one), u8 flags (1 the knife out, 2 the gun fires on the shot frame),
  u8 weapons[6] (the game's weapon numbers it plays with, 0-terminated; none: any),
  u16 Leon's motion, the enemy's motion, the enemy's sequence (entries), pad,
  u16 frames: Leon's end, the enemy's end, the shot, the kill, the body down, the cancel (aim or fire ends Leon's),
  f32 the enemy's start in Leon's frame (x, z, mm), Leon's turn from facing it (radians), the enemy's heading from
      Leon's (radians),
  char name[16].

--kicks OUT/melee/melee.dat also writes the mod's kicks as the melee archive of the pack (runtime/melee.cpp, patch 0904;
uhd_motion_convert.py's layout): its eight "fatal" kicks of a standing stunned Ganado (the 2023 game's cha0_general
0900..0907, which the mod ships) in the even places, its two low kicks of a kneeling one (9904 / 9905, RE9_Low_45 / 47)
in the odd ones. The game's kick routine (plem10Kick) hits on its frame 17: each kick is retimed so that the middle of
its contact window (the mod's lua: source frames 22..28) falls there, the rest at 30 frames per second. Entries 18..20:
the free hatchet attacks, the combo of the mod's knife moves (cha0_wp5000H 0630 / 0640 / 0650, attack 1st, 2nd, 3rd),
for the knife's slash (patch 0922, pl_knife.cpp knife_r3_fire00): retimed so the blade's fastest frame falls on the
slash's game frame 8 (its hits run on 6..10), cut KNIFE_TAIL source frames after it (the stance comes back sooner).
Entries 21..23: Leon's answer to a parried blow (runtime/parry.cpp): the mod's perfect parry and its two pushes back
(cha0_wp5000H 0800 parry_just, 0830 / 0840 parry_crash_L / R), at 30 frames per second, cut after PARRY_END source
frames; the slash's hits on its frames 6..10 stagger the attacker as the knife's answer did. Entries 24 / 25: the
chainsaw clash (runtime/parry.cpp, patch 0924; the 2023 game's chc0_jack_pl_2nd_wp5300 1200 / 1202: Leon holds the
saw off with his knife, then pushes it away).

--move OUT/melee/move.dat writes Leon's movements of the 2023 game (cha0_general, which the mod ships; wish of DevJunior,
13.11.2026: "remaster Leon's movements", phase 1) for runtime/locomotion.cpp (patch 0923): one seamless cycle each
(searched: the pair of frames LOOP_SEARCH apart whose poses differ least) of the stand, the walk, the jog (the game's
run), the walk back and the two turns in place (wheel loops); without fingers (the game's hands hold the weapon).
Entry 0 ("MOV1", big-endian): per movement u8 the motion table's slot (m_MotTbl: 0 stand, 2 walk, 6 run, 8 back), u8 flags (1 its speed matched to the game's at runtime: walk, run, back), u16 its entry, u16
its last frame, u16 the right and the left foot's strike (game frames, 0xFFFF none), u16 pad.

Game files and the PC mod's files never go into the repository; this tool only reads and writes the paths given.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent))
import re_motion_retarget as RT  # noqa: E402
import re_motlist as R  # noqa: E402
import uhd_skin_convert as U  # noqa: E402
from tools.motion import fcv, modelbin  # noqa: E402

# the PC mod's frames (60 per second; its lua: frames = shot, kill, ragdollFrom, cancel; endFrames)
H4 = dict(kill=115, down=206, cancel=160)
C2 = dict(shot=46, kill=48, down=122, cancel=130)
C3 = dict(shot=53, kill=56, down=116, cancel=130)
C5 = dict(shot=62, kill=64, down=90, cancel=155)

# the 2023 game's knife decapitations (60 per second; found from the motions: the grab, the cut, the body down)
KNEE = (("knee-a", "1020", dict(contact=44, kill=88, down=136, cancel=140)),
        ("knee-b", "1025", dict(contact=64, kill=100, down=144, cancel=150)),
        ("knee-c", "1030", dict(contact=60, kill=108, down=196, cancel=200)))
CHC0 = "_chainsaw/animation/ch/chc0/motlist/0th"
CHA0 = "_chainsaw/animation/ch/cha0/motlist/cha0_general.motlist.663"
KICKS_STANDING = ["0900", "0901", "0902", "0903", "0904", "0905", "0906", "0907"]
KICKS_KNEELING = ["9904", "9905"]
KICK_CONTACT = 25.0   # source frame: the middle of the mod's contact window (22..28)
KICK_HIT = 17         # plem10Kick's hit frame (m_Work0 0x11)
WP5000H = "_chainsaw/animation/ch/cha0/motlist/cha0_wp5000h.motlist.663"
KNIFE_COMBO = ["0630_attack_1st", "0640_attack_2nd", "0650_attack_3rd"]
KNIFE_HIT = 8         # knife_r3_fire10: hits on frames 6..10, full damage on 7..8
KNIFE_TAIL = 30       # source frames kept after the blade's fastest one
PARRIES = ["0800_parry_just", "0830_parry_crash_L", "0840_parry_crash_R"]
PARRY_END = 56        # source frames of a parry kept
# the 2023 game's chainsaw clash (Leon's side; wp5300 the chainsaw): his knife against the saw, then the push
CSAW = "_chainsaw/animation/ch/chc0/motlist/2nd/chc0_jack_pl_2nd_wp5300.motlist.663"
CSAW_MOVES = ["1200_knife_parry_F_start", "1202_knife_parry_F_end"]

HANDGUNS = [1, 2, 3, 4, 0x11]  # item.cpp wep_info: the handguns' weapon numbers (em_sub.cpp HandgunCk)


def finishers():
    out = [dict(name="h4", kind=1, flags=1, weapons=[], dir="hatchet_pair_h4", leon="ch4000g1000_leon_preview",
                enemy="ch4000g1000_enemy_preview", frames=H4),
           dict(name="c2", kind=0, flags=2, weapons=HANDGUNS, dir="gun_pair_c2", leon="ch4000g1110_leon_preview",
                enemy="ch4000g1110_enemy_preview", frames=C2)]
    for name, no, frames in KNEE:
        out.append(dict(name=name, kind=1, flags=1, weapons=[], dir=CHC0, leon="chc0_jack_pl_0th_com",
                        enemy="chc0_grapple_0th_com", leon_motion="_%s_knife" % no, enemy_motion="_%s_knife" % no,
                        frames=frames))
    # (weapon of the 2023 game, clip of the enemy, the game's weapon numbers)
    for group, frames, rows in (
            ("c3c", C3, (("wp4100", 1320, [7]), ("wp4101", 1300, [8]), ("wp4102", 1340, [0x21]),
                         ("wp4200", 1400, [0x0B]), ("wp4202", 1420, [0x0E]))),
            ("c4b2", C3, (("wp4400", 1500, [9]), ("wp4401", 1520, [0x0A]))),
            ("c5b", C5, (("wp4500", 1600, [5]), ("wp4501", 1600, [6])))):
        for wp, clip, weapons in rows:
            out.append(dict(name="%s-%s" % (group, wp), kind=0, flags=2, weapons=weapons, dir="gun_pair_" + group,
                            leon="re9_gun_pair_%s_leon" % group, enemy="re9_gun_pair_%s_enemy" % group,
                            leon_motion=wp, enemy_motion="g%d_" % clip, frames=frames))
    return out


def find_motion(path, key):
    _v, mots = R.read_motlist(path)
    if key is None:
        return mots[0]
    for m in mots:
        if key in m.name:
            return m
    raise SystemExit("%s: no motion named *%s*" % (path, key))


class Fit:
    """One motion fitted to a skeleton, its root kept apart (the root motion)."""

    def __init__(self, motion, model, map_name, step, tol, end, no_fingers=False, frames=None, loop=False):
        body, fingers, nexts = RT.MAPS[map_name]
        mapping = dict(body)
        if not no_fingers:
            mapping.update(fingers)
        mapping = {p: n for p, n in mapping.items() if p < model.n_parts}
        src = RT.Source(motion)
        self.motion_src = motion
        _r, bpos = src.pose(None)
        self.scale = model.parts[0].pos[1] / bpos[src.by_name["Hip"]][1]
        self.frames = frames if frames is not None else list(np.arange(0.0, min(end, motion.frames) + 1e-6, step))
        self.rows = RT.retarget(src, model, mapping, nexts, self.frames, self.scale, root_relative=True)
        self.order = [p for p in sorted(mapping) if mapping[p] in src.by_name and p != 0] + [0]
        self.map_name = map_name
        if loop:
            self.rows = RT.close_loop(self.rows)
        if map_name == "leon":
            self.rows, self.order = RT.split_hips(self.rows, self.order)
        self.model, self.mapping, self.nexts, self.tol = model, mapping, nexts, tol
        self.root = [r[3] for r in self.rows]  # (position in metres, heading) of the source's root

    def track(self, world_scale):
        """The root motion: per frame (x, y, z) in mm in the motion's start frame and the heading from the start."""
        p0, y0 = self.root[0]
        turn = RT.rot_y(y0).T
        track = []
        for p, y in self.root:
            d = turn @ (np.array(p) - np.array(p0)) * world_scale
            track.append((d[0], 0.0, d[2], y - y0))
        return track

    def motion(self, world_scale):
        fixed = None
        if self.map_name == "leon":
            fixed = {p: self.model.parts[p].pos for p in (10, 16, 20, 24) if p in self.order}
        return RT.build_motion(self.rows, self.order, self.tol, root=self.track(world_scale), fixed_pos=fixed)

    def world(self, data, world_scale, frame, part):
        """Where the game's playback puts `part` on `frame`, in the motion's start frame (mm)."""
        from tools.motion import evalhost
        pose = evalhost.Player(self.model, fcv.parse(data), ik=False).frame(frame)
        x, y, z, yaw = self.track(world_scale)[frame]
        return np.array([x, y, z]) + RT.rot_y(yaw) @ np.array(pose.world[part])

    def source(self, frame, part, origin, world_scale):
        """Where the PC motion has the bone of `part` on `frame`, in `origin`'s frame (its root's start; mm)."""
        p0, y0 = origin
        src = RT.Source(self.motion_src)
        _r, pos = src.pose(self.frames[frame])
        return RT.rot_y(y0).T @ (pos[src.by_name[self.mapping[part]]] - np.array(p0)) * world_scale

    def check(self, data):
        from tools.motion import evalhost
        pl = evalhost.Player(self.model, fcv.parse(data), ik=False)
        errs = []
        for fi in range(len(self.frames)):  # every frame: the curves between the keys too
            pose = pl.frame(fi)
            target = self.rows[fi][2]
            for part, nx in self.nexts.items():
                if part not in target or nx not in target:
                    continue
                g = np.array(pose.world[nx]) - np.array(pose.world[part])
                r = target[nx] - target[part]
                if np.linalg.norm(g) < 1e-6 or np.linalg.norm(r) < 1.0:  # a bone on its parent (the stock Spine_0)
                    continue
                c = float(np.dot(g, r) / np.linalg.norm(g) / np.linalg.norm(r))
                errs.append(math.degrees(math.acos(max(-1.0, min(1.0, c)))))
        return float(np.mean(errs)), float(np.max(errs))


def kick_frames(total, contact=KICK_CONTACT, hit=KICK_HIT):
    """Source frames for the game's: `contact` on game frame `hit`, every second one after it up to `total`."""
    step = contact / hit
    out = [i * step for i in range(hit)]
    f = contact
    while f <= total + 1e-6:
        out.append(f)
        f += 2.0
    return out


def fastest_hand(motion):
    """The source frame of the right hand's fastest move (the blade's swing)."""
    src = RT.Source(motion)
    best, at, prev = -1.0, 0, None
    for f in range(int(motion.frames) + 1):
        _r, pos = src.pose(f)
        h = pos[src.by_name["R_Hand"]]
        if prev is not None and np.linalg.norm(h - prev) > best:
            best, at = float(np.linalg.norm(h - prev)), f
        prev = h
    return at


BOX_KICK = "9904"  # the box strike's own kick when no other pack's is given: RE9's low kick
BOX_END = 10       # game frames it plays after the hit


def box_from(path):
    """Entries 16 and 17 (the box strike's motion and frames) of another pack's melee.dat (uhd_motion_convert.py --box;
    wish of DevJunior, 13.11.2026: the kick for boxes and barrels "we had", Jake's low kick, kept with the RE9 pack,
    since only the last pack's melee.dat counts)."""
    d = Path(path).read_bytes()
    n = struct.unpack_from(">I", d, 0)[0]
    if n < 18:
        raise SystemExit("%s: no box strike (entries 16 and 17)" % path)
    at = list(struct.unpack_from(">%dI" % n, d, 16)) + [len(d)]
    tag = d[16 + 4 * n + 4 * 17:16 + 4 * n + 4 * 18]
    mot, frames = d[at[16]:at[17]], d[at[17]:at[18]]
    if not mot or tag[:3] != b"BXF" or len(frames) < 3 or not frames[0] < frames[1] < frames[2]:
        raise SystemExit("%s: no box strike (entries 16 and 17)" % path)
    # the motion's own size: the next entry starts at a 32-byte boundary
    return mot, frames[:3] + bytes(13)


def kicks(root, model, tol, out_path, no_fingers, box=None):
    """The melee archive with the mod's kicks (see --kicks)."""
    _v, mots = R.read_motlist(root / CHA0)
    entries = []
    for i in range(8):
        for kind, names in ((0, KICKS_STANDING), (1, KICKS_KNEELING)):
            if i >= len(names):
                entries.append(b"")
                continue
            m = next(x for x in mots if "_%s_" % names[i] in x.name)
            fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers, frames=kick_frames(m.frames))
            data = fcv.serialise(fit.motion(fit.scale))
            c = fit.check(data)
            entries.append(data)
            print("kick %-28s on a %s enemy: %d game frames, %d bytes; check mean/worst %.1f/%.1f deg" % (
                m.name, "kneeling" if kind else "standing", len(fit.frames), len(data), c[0], c[1]))
    # 16 / 17: the box strike (patch 0916): another pack's (--box-from) or RE9's low kick, the box breaking on its hit
    if box:
        entries += list(box_from(box))
        print("box strike from %s: frames %d..%d, the box breaks on %d" % (box, entries[-1][0], entries[-1][2],
                                                                            entries[-1][1]))
    else:
        m = next(x for x in mots if "_%s_" % BOX_KICK in x.name)
        frames = kick_frames(m.frames)[:KICK_HIT + BOX_END + 1]
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers, frames=frames)
        entries += [fcv.serialise(fit.motion(fit.scale)), bytes([0, KICK_HIT, KICK_HIT + BOX_END]) + bytes(13)]
        print("box strike: %s, frames 0..%d, the box breaks on %d" % (m.name, KICK_HIT + BOX_END, KICK_HIT))
    _v, knife = R.read_motlist(root / WP5000H)
    for name in KNIFE_COMBO:
        m = next(x for x in knife if x.name.endswith(name))
        peak = fastest_hand(m)
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers,
                  frames=kick_frames(min(m.frames, peak + KNIFE_TAIL), peak, KNIFE_HIT))
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        print("hatchet %-28s blade fastest on source frame %d: %d game frames, %d bytes; check %.1f/%.1f deg" % (
            m.name, peak, len(fit.frames), len(data), c[0], c[1]))
    for name in PARRIES:
        m = next(x for x in knife if x.name.endswith(name))
        fit = Fit(m, model, "leon", 2.0, tol, min(m.frames, PARRY_END), no_fingers)
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        print("parry   %-28s %d game frames, %d bytes; check %.1f/%.1f deg" % (m.name, len(fit.frames), len(data),
                                                                          c[0], c[1]))
    _v, csaw = R.read_motlist(root / CSAW)
    for name in CSAW_MOVES:
        m = next(x for x in csaw if x.name.endswith(name))
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers)
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        print("chainsaw clash %-28s %d game frames, %d bytes; check %.1f/%.1f deg" % (
            name, len(fit.frames), len(data), c[0], c[1]))
    tags = [b"FCV\x00"] * len(entries)
    tags[17] = b"BXF\x00"  # the box strike's frames: read as bytes
    body = U.build_body(entries, tags)
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(body)
    print("%s: %d bytes, %d kicks on a standing enemy, %d on a kneeling one" % (out, len(body), len(KICKS_STANDING),
                                                                           len(KICKS_KNEELING)))


# Leon's movements (--move): (slot, the cha0_general motion, (first, last) cycle lengths searched in source frames or None
# for the whole motion, flags)
MOVES = [(0, "0160_stand_loop", (150, 300), 0), (2, "0403_walk_F_loop", (60, 70), 1),
         (6, "0520_jog_loop_VerA", (40, 50), 1), (8, "0443_walk_B_loop", (50, 170), 1)]
LOOP_BONES = ["Hip", "Spine_0", "Spine_2", "Neck_0", "Head", "R_UpperArm", "R_Forearm", "L_UpperArm", "L_Forearm",
              "R_Thigh", "R_Shin", "R_Foot", "L_Thigh", "L_Shin", "L_Foot"]


def find_loop(motion, lengths):
    """(first, length) of the cycle whose ends' poses (the main bones against the root's heading, the hips' place) differ
    least, both even (the game's frames are every second)."""
    src = RT.Source(motion)
    total = int(motion.frames)
    if lengths is None:
        return 0, total - total % 2
    feats = []
    for f in range(total + 1):
        rot, pos = src.pose(f)
        turn = RT.rot_y(RT.yaw_of(rot[0])).T
        feats.append(np.concatenate([(turn @ rot[src.by_name[b]]).ravel() for b in LOOP_BONES] +
                                    [turn @ (pos[src.by_name["Hip"]] - pos[0]) * 3.0]))
    feats = np.array(feats)
    best = (1e9, 0, lengths[0])
    for length in range(lengths[0], lengths[1] + 1, 2):
        for first in range(0, total - length + 1, 2):
            d = float(np.linalg.norm(feats[first] - feats[first + length]))
            if d < best[0]:
                best = (d, first, length)
    return best[1], best[2]


def plants(motion, first, length, back=False):
    """Game frames (from the cycle's start) where the right and the left foot strike: furthest ahead of the hips along
    the way (behind them walking back)."""
    src = RT.Source(motion)
    out = []
    for foot in ("R_Foot", "L_Foot"):
        ds = []
        for f in range(first, first + length + 1, 2):
            rot, pos = src.pose(f)
            turn = RT.rot_y(RT.yaw_of(rot[0])).T
            ds.append((turn @ (pos[src.by_name[foot]] - pos[src.by_name["Hip"]]))[2])
        ds = np.array(ds[:-1])  # the last frame is the first again
        out.append(int(np.argmin(ds) if back else np.argmax(ds)))
    return out


def moves(root, model, tol, out_path):
    """Leon's movements (see --move)."""
    _v, mots = R.read_motlist(root / CHA0)
    entries, table = [b""], []
    for slot, name, lengths, flags in MOVES:
        m = next(x for x in mots if x.name.endswith(name))
        first, length = find_loop(m, lengths)
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, True,
                  frames=list(np.arange(first, first + length + 1e-6, 2.0)), loop=True)
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        feet = plants(m, first, length, back=slot == 8) if flags & 1 else [0xFFFF, 0xFFFF]
        table.append(struct.pack(">BBHHHHH", slot, flags, len(entries) - 1, len(fit.frames) - 1, feet[0], feet[1], 0))
        dist = np.linalg.norm(np.array(fit.root[-1][0]) - np.array(fit.root[0][0])) * fit.scale
        print("move %-22s slot 0x%x: source %d..%d, %d game frames, %d bytes, %.0f mm a cycle, feet %s; check %.1f/%.1f "
              "deg" % (name, slot, first, first + length, len(fit.frames), len(data), dist, feet, c[0], c[1]))
    entries[0] = b"MOV1" + struct.pack(">I", len(table)) + b"".join(table)
    tags = [b"MOV\x00"] + [b"FCV\x00"] * (len(entries) - 1)
    body = U.build_body(entries, tags)
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(body)
    print("%s: %d bytes, %d movements" % (out, len(body), len(table)))


def sequence(frames, kill, down):
    keys = []
    for f in range(frames):
        free = (1 if f == kill else 0) | (0x80 if f >= down else 0)
        keys.append(fcv.SeqKey(frame=f * 64, se=0, free=free))
    return fcv.serialise_seq(fcv.Sequence(flags=0, keys=keys))


def dat(entries):
    """A DAT archive of (tag, bytes) entries, each at a 32-byte boundary."""
    n = len(entries)
    head = 0x10 + n * 8
    at = (head + 31) & ~31
    offs, body = [], bytearray()
    for _tag, data in entries:
        offs.append(at + len(body))
        body += data
        body += bytes((-len(body)) % 32)
    out = bytearray(struct.pack(">IIII", n, 0, 0, 0))
    out += struct.pack(">%dI" % n, *offs)
    for tag, _d in entries:
        out += tag.encode().ljust(4, b"\0")
    out += bytes(at - len(out))
    return bytes(out + body)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mod")
    ap.add_argument("out")
    ap.add_argument("--leon", required=True, help="Leon's archive (.drs[:entry], the body)")
    ap.add_argument("--ganado", required=True, help="a Ganado's archive (em10.drs:440)")
    ap.add_argument("--only", help="names to build (comma separated; default all)")
    ap.add_argument("--tolerance", type=float, default=0.004)
    ap.add_argument("--no-fingers", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--kicks", help="also write the mod's kicks as the pack's melee archive (melee/melee.dat)")
    ap.add_argument("--box-from", help="the box strike's kick from another pack's melee.dat (entries 16 / 17); "
                    "default RE9's low kick")
    ap.add_argument("--move", help="also write Leon's movements of the 2023 game (melee/move.dat)")
    a = ap.parse_args(argv)

    # the mod's natives/stm folder (its _re9_melee_proto and _chainsaw)
    root = Path(a.mod)
    for sub in ("natives/stm", "stm", "."):
        if (root / sub / "_re9_melee_proto").is_dir():
            root = root / sub
            break
    else:
        if root.name == "_re9_melee_proto":
            root = root.parent

    def model_of(spec):
        path, _, ent = spec.partition(":")
        arc = U.Archive(Path(path).read_bytes(), ">")
        m = modelbin.parse(arc.entry(int(ent) if ent else 0))
        m.check_tree()
        return m

    leon_model, ganado_model = model_of(a.leon), model_of(a.ganado)
    if a.kicks:
        kicks(root, leon_model, a.tolerance, a.kicks, a.no_fingers, a.box_from)
    if a.move:
        moves(root, leon_model, a.tolerance, a.move)
    want = set(a.only.split(",")) if a.only else None
    entries = [("FIN", b"")]
    made = {}
    table = []

    def add(tag, key, data):
        if key in made:
            return made[key]
        entries.append((tag, data))
        made[key] = len(entries) - 1
        return made[key]

    for f in finishers():
        if want and f["name"] not in want and f["name"].split("-")[0] not in want:
            continue
        d = root / (f["dir"] if f["dir"].startswith("_") else "_re9_melee_proto/" + f["dir"])
        lpath = d / (f["leon"] + ".motlist.663")
        epath = d / (f["enemy"] + ".motlist.663")
        lm = find_motion(lpath, f.get("leon_motion"))
        em = find_motion(epath, f.get("enemy_motion"))
        lf = Fit(lm, leon_model, "leon", 2.0, a.tolerance, lm.frames, a.no_fingers)
        ef = Fit(em, ganado_model, "ganado", 2.0, a.tolerance, em.frames)
        world = (lf.scale + ef.scale) / 2.0  # the floor between them: neither body's alone
        ldata = fcv.serialise(lf.motion(world))
        edata = fcv.serialise(ef.motion(world))
        (lp0, ly0), (ep0, ey0) = lf.root[0], ef.root[0]
        o = RT.rot_y(ly0).T @ (np.array(ep0) - np.array(lp0)) * world
        ang = -math.atan2(o[0], o[2])
        heading = math.remainder(ey0 - ly0, 2 * math.pi)
        fr = f["frames"]
        # the bodies' proportions differ from the PC characters': the enemy's start moved so that on the contact
        # frame (the kill) Leon's hand is where it was against the enemy's head in the PC game
        k = min(int(round(fr.get("contact", fr.get("shot", fr["kill"])) / 2.0)), len(lf.frames) - 1,
                len(ef.frames) - 1)
        lpart, epart = f.get("contact", (10, 4))
        goal = ef.source(k, epart, lf.root[0], world) - lf.source(k, lpart, lf.root[0], world)
        have = (o + RT.rot_y(heading) @ ef.world(edata, world, k, epart)) - lf.world(ldata, world, k, lpart)
        shift = goal - have
        o = o + np.array([shift[0], 0.0, shift[2]])
        ang = -math.atan2(o[0], o[2])
        ne = len(ef.frames)
        g = {key: int(round(v / 2.0)) for key, v in fr.items()}
        seq = sequence(ne, min(g["kill"], ne - 1), min(g["down"], ne - 1))
        li = add("FCV", ("L", f["dir"], lm.name), ldata)
        ei = add("FCV", ("E", f["dir"], em.name), edata)
        si = add("SEQ", ("S", f["dir"], em.name), seq)
        weapons = bytes(f["weapons"][:6]).ljust(6, b"\0")
        table.append(struct.pack(">BB6sHHHH6H4f16s", f["kind"], f["flags"], weapons, li, ei, si, 0,
                                 len(lf.frames) - 1, ne - 1, g.get("shot", 0), g["kill"], g["down"], g["cancel"],
                                 float(o[0]), float(o[2]), ang, heading, f["name"].encode()[:15]))
        line = "%-12s Leon %s (%d frames, %d bytes), enemy %s (%d frames, %d bytes); enemy at (%.0f, %.0f) mm, " \
               "turn %.1f, heading %.1f deg (contact on frame %d: moved %.0f, %.0f)" % (
                   f["name"], lm.name, len(lf.frames), len(ldata), em.name, ne, len(edata), o[0], o[2],
                   math.degrees(ang), math.degrees(heading), k, shift[0], shift[2])
        if a.check:
            lc, ec = lf.check(ldata), ef.check(edata)
            line += "; check mean/worst Leon %.1f/%.1f, enemy %.1f/%.1f deg" % (lc[0], lc[1], ec[0], ec[1])
        print(line)
    if not table:
        raise SystemExit("no finisher chosen")
    entries[0] = ("FIN", b"FIN1" + struct.pack(">I", len(table)) + b"".join(table))
    data = dat(entries)
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print("%s: %d finishers, %d entries, %d bytes" % (out, len(table), len(entries), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
