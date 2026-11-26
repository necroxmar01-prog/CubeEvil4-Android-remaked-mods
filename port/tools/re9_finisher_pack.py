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
  ground-*   on a Ganado lying on the floor (runtime/finisher.cpp, patch 0928): the 2023 game's knife decapitations of
             one face down (1000, 1005) and the RE9 mod's hatchet blows on the ground (1085 / 1095 face down, 1080 /
             1090 face up)

The archive: a DAT (u32 count, three zero words, u32 offsets, 4-byte tags) whose entry 0 ("FIN") lists the finishers
(big-endian, 72 bytes each after an 8-byte head "FIN2" + count; "FIN1": 60, without the last three), then the motions ("FCV") and the enemies' sequences
("SEQ": one key per frame; Free 1 on the kill, 0x80 once the body is down, for em10.cpp Dm_NeckBreak). A finisher:

  u8 kind (0 a standing stunned enemy, 1 a kneeling one, 2 one lying face down, 3 face up), u8 flags (1 the knife out, 2 the gun fires on the shot frame),
  u8 weapons[6] (the game's weapon numbers it plays with, 0-terminated; none: any),
  u16 Leon's motion, the enemy's motion, the enemy's sequence (entries), pad,
  u16 frames: Leon's end, the enemy's end, the shot, the kill, the body down, the cancel (aim or fire ends Leon's),
  f32 the enemy's start in Leon's frame (x, z, mm), Leon's turn from facing it (radians), the enemy's heading from
      Leon's (radians),
  char name[16],
  f32 the enemy's hips on its first frame in its own frame (x, z, mm) and the line from its hips to its neck (radians).

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

--react OUT/melee/react.dat writes the Ganados' hit reactions of the 2023 game (chc0_damage_0th_com, which the mod ships;
wish of DevJunior, 15.11.2026) for runtime/react.cpp (patch 0925, em10.cpp Dm_Small): the flinches by where the bullet
hit (REACTS: the class, the motions drawn among), fitted to the Ganado's skeleton, three source frames a game frame
(1.5 times the speed: 62..102 frames, as long as the game's own 55..97). Entry 0 ("RCT1", big-endian): per motion u8
its class, u8 0, u16 its entry, u16 its last frame, u16 0.

--move OUT/melee/move.dat writes Leon's movements of the 2023 game (cha0_general, which the mod ships; wish of DevJunior,
13.11.2026: "remaster Leon's movements", phase 1) for runtime/locomotion.cpp (patch 0923): one seamless cycle each
(searched: the pair of frames LOOP_SEARCH apart whose poses differ least) of the stand, the walk, the jog (the game's
run), the walk back and the two turns in place (wheel loops); without fingers (the game's hands hold the weapon).
Entry 0 ("MOV1", big-endian): per movement u8 the motion table's slot (m_MotTbl: 0 stand, 2 walk, 6 run, 8 back), u8 flags (1 its speed matched to the game's at runtime: walk, run, back), u16 its entry, u16
its last frame, u16 the right and the left foot's strike (game frames, 0xFFFF none), u16 pad. After the rows "STK1"
and per row u8 n and n u16: every footstep of the motion (contacts(): the foot coming down onto the floor; its game
frame, 0x4000 the left foot), for the footstep sounds of the clips and of loops of several strides (device report
18.11.2026: no footsteps while running).

Game files and the PC mod's files never go into the repository; this tool only reads and writes the paths given.
"""
import argparse
import copy
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
# (name, motion, kind, frames: the first blow from the hand's fastest move and the head's jolt, 60 per second)
GROUND = (("ground-a", "1000", 2, dict(contact=38, kill=38, down=0, cancel=160)),
          ("ground-b", "1005", 2, dict(contact=30, kill=30, down=0, cancel=130)),
          ("ground-c", "1085", 2, dict(contact=40, kill=40, down=0, cancel=160)),
          ("ground-d", "1095", 2, dict(contact=34, kill=34, down=0, cancel=130)),
          ("ground-e", "1080", 3, dict(contact=38, kill=38, down=0, cancel=160)),
          ("ground-f", "1090", 3, dict(contact=36, kill=36, down=0, cancel=130)))
# the stealth kills from behind (wish of DevJunior, 19.11.2026: "Leon's stealth kills", on a Ganado who has not seen
# him): the 2023 game's knife stealth kill (1101) and the RE9 mod's hatchet ones (1110, 1120, 1125; 1115 drops the body
# at once); kind 4; source frames (60 per second) of the blow (the hand's fastest move near it), the body on the floor
# (the hips below 35 % of their height) and Leon's cancel
STEALTH = (("stealth-a", "1101", dict(contact=57, kill=60, down=150, cancel=285)),
           ("stealth-b", "1110", dict(contact=160, kill=160, down=311, cancel=285)),
           ("stealth-c", "1120", dict(contact=140, kill=140, down=209, cancel=285)),
           ("stealth-d", "1125", dict(contact=115, kill=115, down=170, cancel=228)))
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
PARRY_BACK = 450.0    # mm Leon steps back at most in a parry answer
# the 2023 game's chainsaw clash (Leon's side; wp5300 the chainsaw): his knife against the saw, then the push
CSAW = "_chainsaw/animation/ch/chc0/motlist/2nd/chc0_jack_pl_2nd_wp5300.motlist.663"
CSAW_MOVES = ["1200_knife_parry_F_start", "1202_knife_parry_F_end"]
# the push's root faces away from its body (root heading 0, feet 146 degrees; the block's root 180, feet 163): turned
# half round so that Leon faces the Ganado as in the block (device video 16.11.2026: "he comes out with his back to
# the enemy")
CSAW_YAW = {"1202_knife_parry_F_end": math.pi}
# the push's game frames over which Leon's heading turns into his body's (Fit.settle; runtime/parry.cpp plays it from
# kPushFrom and ends it at kPushEnd)
CSAW_SETTLE = {"1202_knife_parry_F_end": (12, 62)}

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
    # on the floor (wish of DevJunior, 16.11.2026: "the finishers on the ground"): the 2023 game's knife decapitations of
    # a Ganado lying face down and the RE9 mod's hatchet blows on the ground (face down 1085 / 1095, face up 1080 /
    # 1090); kind 2 face down, 3 face up; the first blow kills
    for name, no, kind, frames in GROUND:
        out.append(dict(name=name, kind=kind, flags=1, weapons=[], dir=CHC0, leon="chc0_jack_pl_0th_com",
                        enemy="chc0_grapple_0th_com", leon_motion="_%s_" % no, enemy_motion="_%s_" % no,
                        frames=frames))
    for name, no, frames in STEALTH:
        out.append(dict(name=name, kind=4, flags=1, weapons=[], dir=CHC0, leon="chc0_jack_pl_0th_com",
                        enemy="chc0_grapple_0th_com", leon_motion="_%s_" % no, enemy_motion="_%s_" % no,
                        frames=frames))
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

    def __init__(self, motion, model, map_name, step, tol, end, no_fingers=False, frames=None, loop=False,
                 game_like=False, yaw_offset=0.0):
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
        self.rows = RT.retarget(src, model, mapping, nexts, self.frames, self.scale, root_relative=True,
                                yaw_offset=yaw_offset)
        self.order = [p for p in sorted(mapping) if mapping[p] in src.by_name and p != 0] + [0]
        self.map_name = map_name
        self.hold_back = False
        if loop:
            self.rows = RT.close_loop(self.rows)
        self.game_like = game_like or map_name == "leon"
        if map_name == "leon":
            self.rows, self.order = RT.split_hips(self.rows, self.order)
        elif game_like:  # a Ganado's own reactions: his motions do not turn part 0 either (em10.drs)
            self.rows, self.order = RT.split_hips(self.rows, self.order, rest_parts=(9, 15))
        self.model, self.mapping, self.nexts, self.tol = model, mapping, nexts, tol
        self.root = [r[3] for r in self.rows]  # (position in metres, heading) of the source's root

    def track(self, world_scale):
        """The root motion: per frame (x, y, z) in mm in the motion's start frame and the heading from the start."""
        p0, y0 = self.root[0]
        turn = RT.rot_y(y0).T
        track = []
        for p, y in self.root:
            d = turn @ (np.array(p) - np.array(p0)) * world_scale
            # an answer to a parried blow: a step forward turns into one back, at most PARRY_BACK (device report
            # 15.11.2026: "the parries that look powerful should go backwards")
            z = max(-abs(d[2]), -PARRY_BACK) if self.hold_back else d[2]
            track.append((d[0], 0.0, z, y - y0))
        return track

    def keep_low(self):
        """An answer to a parried blow (device report 15.11.2026: in one parry he stepped into the enemy, in one he rose
        taller than him): its steps go back, never forward (track), the hips no higher than at rest."""
        self.hold_back = True
        top = float(self.model.parts[0].pos[1])
        self.rows = [(r[0], np.array([r[1][0], min(r[1][1], top), r[1][2]])) + tuple(r[2:]) for r in self.rows]

    def settle(self, start, end):
        """From game frame `start` to `end` the root heading turns into the body's (the pelvis' facing against the
        root at `end`) and the body back by as much: the pose stays, but at `end` Leon's own heading is where his body
        faces, so the motion after it starts there (device report 18.11.2026: after the chainsaw clash's push he
        swung sideways into his stand, also into his aim)."""
        last = min(end, len(self.rows) - 1)
        turn = RT.yaw_of(self.rows[last][0][17]) if 17 in self.rows[last][0] else 0.0
        rows, root = [], []
        for i, (row, (p, y)) in enumerate(zip(self.rows, self.root)):
            t = min(max((i - start) / float(max(1, end - start)), 0.0), 1.0)
            d = turn * t * t * (3.0 - 2.0 * t)
            back = RT.rot_y(-d)
            local = dict(row[0])
            for part in (1, 17):
                if part in local:
                    local[part] = back @ local[part]
            pts = {k: back @ v for k, v in row[2].items()} if isinstance(row[2], dict) else row[2]
            rows.append((local, back @ np.array(row[1]), pts, (p, y + d)) + tuple(row[4:]))
            root.append((p, y + d))
        self.rows, self.root = rows, root
        return math.degrees(turn)

    def game_wrists(self, wrists):
        """The hands turned as in a motion of the game's own (`wrists`: per game frame {part: 3x3 local rotation}),
        taken at the same share of the motion (Mods, 21.11.2026, DevJunior: "the hands look odd when I shoot the
        Ganados"): the remake's Ganados flinch empty-handed, their wrists bent up to 120 degrees off any the game
        uses, and the axe or sickle in the hand pointed every which way."""
        n, m = len(self.rows), len(wrists)
        rows = []
        for i, row in enumerate(self.rows):
            src = wrists[min(m - 1, int(round(i * (m - 1) / float(max(1, n - 1)))))]
            local = dict(row[0])
            for part, rot in src.items():
                if part in local:
                    local[part] = rot
            rows.append((local,) + tuple(row[1:]))
        self.rows = rows

    def feet_down(self, start, ramp=8, lift=15.0):
        """(Mods, 26.11.2026, DevJunior: Ganados left "with the legs up" after a finisher) from game frame `start` (the
        body down) on, each foot no higher than the ankle stands at rest plus `lift`: thigh and shin turned (two-bone,
        the knee kept on its side) so that the ankle comes down onto the floor; faded in over `ramp` frames. The PC
        bodies end on their backs with the knees drawn up (ankles 230-290 mm above the floor against 180-220 for the
        hips); the game then holds that last pose while the body lies there."""
        par = [p.parent for p in self.model.parts]
        rest = {}
        for q in range(self.model.n_parts):
            rest[q] = (rest[par[q]] if par[q] >= 0 else np.zeros(3)) + np.array(self.model.parts[q].pos, dtype=float)
        rows, moved = [], 0
        for i, row in enumerate(self.rows):
            t = min(max((i - start) / float(max(1, ramp)), 0.0), 1.0)
            if t <= 0.0:
                rows.append(row)
                continue
            local = dict(row[0])
            for th, sh, ft in ((18, 19, 20), (22, 23, 24)):
                if th not in local or sh not in local:
                    continue
                rot, pos = fk(self.model, (local,) + tuple(row[1:]))
                floor = rest[ft][1] + lift
                if pos[ft][1] <= floor:
                    continue
                want = pos[ft].copy()
                want[1] = pos[ft][1] + (floor - pos[ft][1]) * t
                s0, k0, f0 = pos[th], pos[sh], pos[ft]
                a, b = np.linalg.norm(k0 - s0), np.linalg.norm(f0 - k0)
                v = want - s0
                d = min(np.linalg.norm(v), a + b - 1.0)
                v /= np.linalg.norm(v)
                pole = (k0 - s0) - np.dot(k0 - s0, v) * v
                if np.linalg.norm(pole) < 1e-6:
                    continue
                pole /= np.linalg.norm(pole)
                ca = max(-1.0, min(1.0, (a * a + d * d - b * b) / (2.0 * a * d)))
                knee = s0 + v * a * ca + pole * a * math.sqrt(1.0 - ca * ca)
                w = rot[par[th]]
                local[th] = w.T @ rotation_between(k0 - s0, knee - s0) @ w @ local[th]
                rot, pos = fk(self.model, (local,) + tuple(row[1:]))
                w = rot[par[sh]]
                local[sh] = w.T @ rotation_between(pos[ft] - pos[sh], want - pos[sh]) @ w @ local[sh]
                moved += 1
            rows.append((local,) + tuple(row[1:]))
        self.rows = rows
        return moved

    def motion(self, world_scale):
        fixed = None
        if self.game_like:
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


BOX_SLASH = "0600_attack"  # the box strike when no other pack's is given: the remake's slash (cha0_wp5000H; wish of
BOX_HIT = 6                # DevJunior, 16.11.2026: "the remake's animations for breaking barrels and boxes": the
BOX_END = 12               # remake breaks them with the knife's slash); its blade's fastest frame the hit (game frame)


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


# the boxes' kick (entries 26 / 27; wish of DevJunior, 16-17.11.2026: "alternate by the object", "the kick for boxes
# and barrels done right"): a crate kicked, a barrel or a vase slashed (runtime/melee.cpp). From a motlist the user
# gives (--box-kick, e.g. the "Kick That Box" mod's cha0_general2: its low kick in place of the 2023 game's stomp);
# the foot's fastest frame on game frame KICK_BOX_HIT
KICK_BOX = "0531_attack_lower_VerB"
KICK_BOX_HIT = 8
KICK_BOX_END = 16


def fastest_foot(motion):
    """The source frame of a foot's fastest move (the kick)."""
    src = RT.Source(motion)
    best, at = -1.0, 0
    for name in ("R_Foot", "L_Foot"):
        prev = None
        for f in range(int(motion.frames) + 1):
            _r, pos = src.pose(f)
            p = pos[src.by_name[name]]
            if prev is not None and np.linalg.norm(p - prev) > best:
                best, at = float(np.linalg.norm(p - prev)), f
            prev = p
    return at


def kicks(root, model, tol, out_path, no_fingers, box=None, box_kick=None):
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
    _v, knife = R.read_motlist(root / WP5000H)
    if not box:
        m = next(x for x in knife if x.name.endswith(BOX_SLASH))
        peak = fastest_hand(m)
        frames = kick_frames(min(m.frames, peak + 40), peak, BOX_HIT)[:BOX_HIT + BOX_END + 1]
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers, frames=frames)
        entries += [fcv.serialise(fit.motion(fit.scale)), bytes([0, BOX_HIT, BOX_HIT + BOX_END]) + bytes(13)]
        print("box strike: %s (blade fastest on source frame %d), frames 0..%d, the box breaks on %d" % (
            m.name, peak, BOX_HIT + BOX_END, BOX_HIT))
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
        fit.keep_low()
        data = fcv.serialise(fit.motion(fit.scale))
        c = (0.0, 0.0)
        entries.append(data)
        print("parry   %-28s %d game frames, %d bytes; check %.1f/%.1f deg" % (m.name, len(fit.frames), len(data),
                                                                          c[0], c[1]))
    _v, csaw = R.read_motlist(root / CSAW)
    for name in CSAW_MOVES:
        m = next(x for x in csaw if x.name.endswith(name))
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers, yaw_offset=CSAW_YAW.get(name, 0.0))
        settled = fit.settle(*CSAW_SETTLE[name]) if name in CSAW_SETTLE else 0.0
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        print("chainsaw clash %-28s %d game frames, %d bytes, heading settled %.0f deg; check %.1f/%.1f deg" % (
            name, len(fit.frames), len(data), settled, c[0], c[1]))
    if box_kick:
        path, _, name = box_kick.partition(":")
        _v, km = R.read_motlist(Path(path))
        m = next(x for x in km if x.name.endswith(name or KICK_BOX))
        peak = fastest_foot(m)
        frames = kick_frames(m.frames, peak, KICK_BOX_HIT)[:KICK_BOX_HIT + KICK_BOX_END + 1]
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, no_fingers, frames=frames)
        entries += [fcv.serialise(fit.motion(fit.scale)),
                    bytes([0, KICK_BOX_HIT, KICK_BOX_HIT + KICK_BOX_END]) + bytes(13)]
        print("the boxes' kick: %s (the foot fastest on source frame %d), frames 0..%d, the box breaks on %d" % (
            m.name, peak, KICK_BOX_HIT + KICK_BOX_END, KICK_BOX_HIT))
    tags = [b"FCV\x00"] * len(entries)
    tags[17] = b"BXF\x00"  # the box strike's frames: read as bytes
    if box_kick:
        tags[27] = b"BXF\x00"
    body = U.build_body(entries, tags)
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(body)
    print("%s: %d bytes, %d kicks on a standing enemy, %d on a kneeling one" % (out, len(body), len(KICKS_STANDING),
                                                                           len(KICKS_KNEELING)))


# Leon's movements (--move): (slot, the cha0_general motion, (first, last) cycle lengths searched in source frames or None
# for the whole motion, flags)
# The jog: its whole motion (360 source frames, eight strides; its last pose is its first): one stride of it (46
# frames) closes only within 0.33 of a pose's distance, its strides differ (device report 18.11.2026: the run looked
# cut at one point and started over).
MOVES = [(0, "0160_stand_loop", (150, 300), 0), (2, "0403_walk_F_loop", (60, 70), 1),
         (6, "0520_jog_loop_VerA", None, 1), (8, "0443_walk_B_loop", (50, 170), 1)]
# the transitions (wish of DevJunior, 16.11.2026: "starts, stops and turns as in the remake", Leon still turning to
# where he goes): (code of runtime/locomotion.cpp, the cha0_general motion). 0x80 / 0x81 setting off / stopping from a
# walk, 0x82 / 0x83 from a run, 0x84 / 0x85 turning about to the left / right while running, 0x86 / 0x87 standing
CLIPS = [(0x80, "0400_walk_F_start"), (0x81, "0406_walk_F_end"), (0x82, "0510_jog_start_R0"), (0x83, "0540_jog_end"),
         (0x84, "0532_jog_turn_L180"), (0x85, "0536_jog_turn_R180"), (0x86, "0170_turn_L180"),
         (0x87, "0171_turn_R180"),
         # the sprint (wish of DevJunior, 17.11.2026: "jog with the stick, really run with the run button, as the
         # remake"): its loop (0x88, played round), setting off from standing (0x89), stopping (0x8A), from the jog
         (0x88, "0720_dash_loop"), (0x89, "0710_dash_start_R0"), (0x8A, "0740_dash_end"),
         (0x8B, "0718_dash_from_jog_R")]
CLIP_LOOPS = {0x88}
# the side steps while aiming (wish of DevJunior, 21.11.2026: "the movements to the sides"; runtime/free_move.cpp,
# "Move while aiming"): Leon's own walk loops of cha0_general to the left, right, forward-left and back-right, one
# seamless cycle each (find_loop); forward-right and back-left are the last two mirrored (the game's flip table)
STRAFES = [(0x8C, "0433_walk_L_loop", (40, 90)), (0x8D, "0473_walk_R_loop", (40, 90)),
           (0x8E, "0413_walk_FL_loop", (40, 90)), (0x8F, "0453_walk_BR_loop", (40, 90))]
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


def contacts(motion, frames, low=0.035, lift=0.07):
    """Game frames (indices into `frames`) where a foot comes down onto the floor, with the foot (0 right, 1 left): its
    height below its lowest plus `low` after it was raised above its lowest plus `lift` (metres)."""
    src = RT.Source(motion)
    hs = ([], [])
    for f in frames:
        _rot, pos = src.pose(f)
        hs[0].append(pos[src.by_name["R_Foot"]][1])
        hs[1].append(pos[src.by_name["L_Foot"]][1])
    out = []
    for foot, h in enumerate(hs):
        base = min(h)
        up = h[0] > base + lift
        for i in range(1, len(h)):
            if h[i] > base + lift:
                up = True
            elif up and h[i] < base + low:
                out.append((i, foot))
                up = False
    return sorted(out)


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


# The shotgun's reload of the 2023 game (wish of DevJunior, 21.11.2026: Leon's own weapon animations, from the
# "Tactical First-Person Mode" mod's cha0_wp4100H): start, loop (one shell a cycle), end (with the pump). The game's
# reload stays as long as it is at each level (its frame for the rounds, its sounds and the gun's own motion: the two
# shells, then the pump), the remake's played over it in pieces: (segment, source frame) -> game frame. Per level
# (weapon_lv_reload 0..2): the first shell, the second's end, the pump's start and end, the length (wep07 motions
# 0x27 / 0x29 / 0x2B, the gun's part 2 the shell, parts 1 / 4 the fore-end).
WEP_RELOAD_SHOTGUN = "cha0_wp4100H.motlist.663"
SHOTGUN_LEVELS = [(0x26, 12, 48, 63, 78, 90), (0x28, 12, 40, 54, 64, 73), (0x2A, 9, 27, 36, 46, 50)]


# Taking an item (wish of DevJunior, 21.11.2026: "add the animation of taking objects"): the remake's hand reaching for
# it, on the upper body (runtime/locomotion.cpp re4_move_pickup): on the move (0xA0), standing for a low item (0xA1),
# for one at a table's height (0xA2). The remake has them only as Ada's (cha8_general2, "General Animations Corrector").
PICKUPS = [(0xA0, "0406_item_up_run"), (0xA1, "0408_item_up_lower"), (0xA2, "0410_item_up_middle_F")]


def motlist_in(folder, name):
    """A motlist of a folder whatever its case (REtool writes the remake's names in small letters)."""
    p = Path(folder) / name
    if p.exists():
        return p
    for q in Path(folder).iterdir():
        if q.name.lower() == name.lower():
            return q
    raise SystemExit("%s: no %s" % (folder, name))


def rest_from(motion, ref):
    """A motion of a mod whose bones stand at a hundredth of their length (its exporter's units): the reference's rest
    translations by name, the others times 100."""
    rb = {b.name: b for b in ref.bones}
    for b in motion.bones:
        if b.name in rb:
            b.trans = rb[b.name].trans
        else:
            b.trans = tuple(x * 100.0 for x in b.trans[:3]) + tuple(b.trans[3:])


RELOAD_FACING = -35.0


def face_ahead(rows, model, part, yaw=0.0):
    """`rows` turned about the upright (on the root's children: the waist 1, the legs' hip 17) so that `part` faces
    `yaw` degrees from ahead (+z, + to the left) on the mean."""
    z = np.array([0.0, 0.0, 1.0])
    yaws = [fk(model, r)[0][part] @ z for r in rows]
    t = math.radians(yaw) - math.atan2(sum(v[0] for v in yaws), sum(v[2] for v in yaws))
    q = rot_axis(np.array([0.0, 1.0, 0.0]), t)
    kids = [i for i, pp in enumerate(model.parts) if pp.parent == 0]
    out = []
    for r in rows:
        r0 = r[0].get(0, np.eye(3))
        qq = r0.T @ q @ r0
        out.append(({**r[0], **{c: qq @ r[0].get(c, np.eye(3)) for c in kids}},) + tuple(r[1:]))
    return out


# the legs (under the root, part 0, through the hip 17) held still in the reload
RELOAD_STILL = (17, 18, 19, 20, 21, 22, 23, 24, 25, 31)


def shotgun_reloads(motdir, ref, model, tol, wep, hand_off=None):
    """The rows (code, entry data) of the shotgun's reload per level for move.dat (codes 0x90 + level)."""
    _v, mots = R.read_motlist(motlist_in(motdir, WEP_RELOAD_SHOTGUN))
    seg = {}
    for key, name in (("start", "0805_reload_start"), ("loop", "0810_reload_loop"), ("end", "0815_reload_end")):
        seg[key] = next(x for x in mots if x.name.endswith(name))
        rest_from(seg[key], ref)
    out = []
    for lv, (entry, s1, e2, p0, p1, n) in enumerate(SHOTGUN_LEVELS):
        # (game frame, segment, source frame) keypoints, linear between them
        hold = max(0, p0 - 8 - e2)
        keys = [(0, "start", 48.0), (s1, "start", 80.0), (s1, "loop", 0.0), (e2, "loop", 76.0),
                (e2 + hold, "loop", min(190.0, 76.0 + hold * 2.0)), (p0 - 8, "end", 0.0), (p0, "end", 32.0),
                (p1, "end", 48.0), (n, "end", 87.0)]
        plan = []
        for g in range(n + 1):
            k = max(i for i in range(len(keys)) if keys[i][0] <= g and (i + 1 == len(keys) or keys[i + 1][0] > g
                                                                        or keys[i][0] == g))
            g0, sg, f0 = keys[k]
            if k + 1 < len(keys) and keys[k + 1][1] == sg and keys[k + 1][0] > g0:
                g1, _s, f1 = keys[k + 1]
                f = f0 + (f1 - f0) * (g - g0) / float(g1 - g0)
            else:
                f = f0
            plan.append((sg, min(f, seg[sg].frames)))
        rows, order = [], None
        i = 0
        while i < len(plan):
            j = i
            while j < len(plan) and plan[j][0] == plan[i][0]:
                j += 1
            fit = Fit(seg[plan[i][0]], model, "leon", 2.0, tol, seg[plan[i][0]].frames, True,
                      frames=[f for _s, f in plan[i:j]])
            rows += fit.rows if hand_off is None else [
                ({**r[0], 10: r[0].get(10, np.eye(3)) @ hand_off},) + tuple(r[1:]) for r in fit.rows]
            order = order or fit.order
            i = j
        fixed = {p: model.parts[p].pos for p in (10, 16, 20, 24) if p in order}
        # (v30, device report 23.11.2026: "it still turns to reload"; v26 turned the reload some 77 deg to face as the
        # game's does, which turned Leon round from and back to the remake's forward-facing carry) the remake's
        # facing kept, its legs and hips' place held still as at the reload's first frame (the three pieces put
        # together stepped about, v25: "the reload turns the legs very oddly")
        rows = [({**r[0], **{q: rows[0][0][q] for q in RELOAD_STILL if q in rows[0][0]}}, rows[0][1]) + tuple(r[2:])
                for r in rows]
        # and the whole body turned so that the legs stand as in the remake's stand with the gun (carry 0x98: some
        # 35 deg to the right; the reload's first frame stood 23 deg to the left): no turn from it into the reload
        rows = face_ahead(rows, model, 18, RELOAD_FACING)
        mot = RT.build_motion(rows, order, tol, root=None, fixed_pos=fixed)
        game = fcv.parse(wep.entry(entry))
        # the game's own hand-held parts (32, 110: the shell and the left hand's prop) at the game's times
        # (ours keep part 32 at rest: one joint per part, else the game's key histories clash, "Invalid key history")
        mot.joints = [j for j in mot.joints if j.parts_no not in (32, 110)]
        mot.joints += [j for j in game.joints if j.parts_no in (32, 110)]
        mot.layout = list(range(len(mot.joints)))
        data = fcv.serialise(mot)
        print("shotgun reload level %d: %d game frames (the game's %d), %d bytes; shells by %d / %d, pump %d..%d; "
              "the legs still" % (lv, len(rows) - 1, n, len(data), s1, e2, p0, p1))
        out.append((0x90 + lv, data))
    return out


# The handguns' reload of the 2023 game (wish of DevJunior, 25.11.2026: "now Leon's other animations, the other
# weapons", handguns first): cha0_wp4000H 0810_reload (the SG-09 R's, 195 frames at 60 a second) played over the game's
# own reload at each level (weapon_lv_reload 0..2; the game's handgun archives' PL_ARC 0x2D..0x2F = entries 0x29..0x2B),
# stretched to its length so that the gun's own motion (the magazine) and the game's frames for the rounds stay as they
# are. Two sets of lengths among the disc's handguns: 52/45/28 frames (wep01, wep02, wep04) as move.dat codes 0xB0 +
# level, 56/47/29 (wep06) as 0xB4 + level (runtime/locomotion.cpp re4_move_reload takes the set as long as the game's).
WEP_PISTOL = "cha0_wp4000H.motlist.663"
PISTOL_SETS = [(0xB0, "wep01"), (0xB4, "wep06")]


def game_yaw(model, data, part, frame=0):
    """The game motion's facing (degrees from ahead, + to the left) of `part` at `frame`."""
    from tools.motion import evalhost
    p = evalhost.Player(model, fcv.parse(data), ik=False).frame(frame)
    h = np.array(p.mat[part]).reshape(3, 4)[:, :3]
    v = h @ np.array([0.0, 0.0, 1.0])
    return math.degrees(math.atan2(v[0], v[2]))


def hold_part_yaw(rows, model, part, yaws):
    """`rows` with `part` turned about the upright each frame so that it faces yaws[frame] (degrees, as game_yaw)."""
    z = np.array([0.0, 0.0, 1.0])
    out = []
    for i, r in enumerate(rows):
        world = fk(model, r)[0]
        v = world[part] @ z
        d = math.radians(yaws[min(i, len(yaws) - 1)]) - math.atan2(v[0], v[2])
        par = model.parts[part].parent
        rp = world[par] if par is not None and par >= 0 else np.eye(3)
        q = rp.T @ rot_axis(np.array([0.0, 1.0, 0.0]), d) @ rp
        out.append(({**r[0], part: q @ r[0].get(part, np.eye(3))},) + tuple(r[1:]))
    return out


def pistol_reloads(motdir, ref, model, tol, archives):
    """The rows (code, entry data) of the handguns' reloads for move.dat. `archives`: name -> the disc's handgun
    archive (em/wepNN.drs) of PISTOL_SETS."""
    _v, mots = R.read_motlist(motlist_in(motdir, WEP_PISTOL))
    clip = next(x for x in mots if x.name.endswith("0810_reload"))
    rest_from(clip, ref)
    hold = next(x for x in mots if x.name.endswith("0160_hold_loop"))
    rest_from(hold, ref)
    out = []
    for base, name in PISTOL_SETS:
        if name not in archives:
            continue
        warc = U.Archive(Path(archives[name]).read_bytes(), ">")
        aim = warc.entry(0x27 - 4)  # the level aim idle (PL_ARC 0x27)
        grip = grip_frame(model, aim, range(0, fcv.parse(aim).max_frame + 1, 4))
        # how the game's gun lies in the remake's right hand: from the remake's hold loop (both hands on it)
        hf = Fit(hold, model, "leon", 2.0, tol, hold.frames, True, frames=list(np.arange(0, min(hold.frames, 120.0), 4.0)))
        fixed_rows = grip_fix(hf.rows, model, grip)
        acc = np.zeros((3, 3))
        for r0, r1 in zip(hf.rows, fixed_rows):
            acc += fk(model, r0)[0][10].T @ fk(model, r1)[0][10]
        u_, _s, vt = np.linalg.svd(acc)
        off = u_ @ vt
        print("%s grip: the game's gun in the remake's hand turned %.0f deg" % (name, math.degrees(axis_angle(off)[1])))
        for lv in range(3):
            game = fcv.parse(warc.entry(0x29 + lv))
            n = game.max_frame
            frames = [min(clip.frames - 1.0, g * (clip.frames - 1.0) / n) for g in range(n + 1)]
            fit = Fit(clip, model, "leon", 2.0, tol, clip.frames, True, frames=frames)
            rows = [({**r[0], 10: r[0].get(10, np.eye(3)) @ off},) + tuple(r[1:]) for r in fit.rows]
            # the legs and hips held as at the first frame, the whole body facing as the game's reload stands
            rows = [({**r[0], **{q: rows[0][0][q] for q in RELOAD_STILL if q in rows[0][0]}}, rows[0][1]) + tuple(r[2:])
                    for r in rows]
            rows = face_ahead(rows, model, 18, game_yaw(model, warc.entry(0x29 + lv), 18))
            # (device report 25.11.2026, video 117886: "the reload makes him turn") the remake's reload twists the
            # trunk some 30..40 deg further round than the game's: the waist (part 1) turned back each frame to face
            # as the game's own reload faces at that frame
            gy = [game_yaw(model, warc.entry(0x29 + lv), 1, g) for g in range(n + 1)]
            rows = hold_part_yaw(rows, model, 1, gy)
            fixed = {q: model.parts[q].pos for q in (10, 16, 20, 24) if q in fit.order}
            mot = RT.build_motion(rows, fit.order, tol, root=None, fixed_pos=fixed)
            # the game's own hand-held part (32: the gun's place) at the game's times
            mot.joints = [j for j in mot.joints if j.parts_no != 32] + [j for j in game.joints if j.parts_no == 32]
            mot.layout = list(range(len(mot.joints)))
            data = fcv.serialise(mot)
            print("handgun reload (%s) level %d: %d game frames, %d bytes" % (name, lv, n, len(data)))
            out.append((base + lv, data))
    return out


# (Mods, 26.11.2026, DevJunior: "the Remake's weapon animations", pistols first) the handguns' aim idle and fire from the
# remake (cha0_wp4000H 0160_hold_loop, 0500_shoot) in place of the game's three-way poses (mot3: down, level, up =
# PL_ARC 0x26..0x28 aim, 0x29..0x2B fire): the remake's legs and torso, the game's arms (the hands on the gun as the game
# holds it, the laser from its muzzle), the torso turned until the right hand (the gun) lies as the game's; aim and fire
# both from the remake so that the body does not jump between them (the shotgun's try of 23.11.2026: the remake's aim
# with the game's fire turned the head away on firing). Stretched to the game's lengths (aim 40, fire 21 or 23):
# move.dat 0xB8 + k aim, 0xBC + k fire for the 21-frame fire (wep01, 02, 03, 04, 38), 0xC0 + k / 0xC4 + k for the
# 23-frame one (wep05, wep06); k 0 down, 1 level, 2 up (runtime/locomotion.cpp re4_move_hg_pose, patch 0956).
PISTOL_POSE_SETS = [(0xB8, 0xBC, "wep01"), (0xC0, 0xC4, "wep06")]


def hand_turn(model, data, frames, part=10):
    """The mean model-space turn of `part` over `frames` of the motion `data`."""
    from tools.motion import evalhost
    pl = evalhost.Player(model, fcv.parse(data), ik=False)
    acc = np.zeros((3, 3))
    for f in frames:
        h = np.array(pl.frame(f).mat[part]).reshape(3, 4)[:, :3]
        acc += h / np.linalg.norm(h, axis=0)
    u_, _s, vt = np.linalg.svd(acc)
    return u_ @ vt


def turn_torso_to(rows, model, order, tol, fixed, game, want_rot, frames, steps=4):
    """`rows` with the waist and chest turned (half each) until the game's arms on them hold the right hand as
    `want_rot` (model space)."""
    for _i in range(steps):
        have = hand_turn(model, with_game_arms(rows, order, tol, fixed, game), frames)
        axis, ang = axis_angle(want_rot @ have.T)
        half = rot_axis(axis, ang * 0.5)
        rows = [({**row[0], 1: half @ row[0][1],
                  2: (half @ row[0][1]).T @ half @ (half @ row[0][1]) @ row[0][2]},) + tuple(row[1:])
                for row in rows]
    return rows


def head_as_game(rows, model, gdata, part=4):
    """`rows` with the head (part 4) turned each frame to face as the game's motion `gdata` holds it at that frame
    (device report 26.11.2026: "when he aims he turns the neck elsewhere": the remake's head on a torso turned to the
    game's gun line looked off to the side; the game's own aim also has its neck control on top of this pose)."""
    from tools.motion import evalhost
    pl = evalhost.Player(model, fcv.parse(gdata), ik=False)
    par = model.parts[part].parent
    out = []
    for i, r in enumerate(rows):
        h = np.array(pl.frame(min(i, fcv.parse(gdata).max_frame)).mat[part]).reshape(3, 4)[:, :3]
        h = h / np.linalg.norm(h, axis=0)
        rot = fk(model, r)[0]
        out.append(({**r[0], part: rot[par].T @ h},) + tuple(r[1:]))
    return out


def pistol_poses(motdir, ref, model, tol, archives):
    """The rows (code, entry data) of the handguns' aim idles and fire motions for move.dat (PISTOL_POSE_SETS)."""
    _v, mots = R.read_motlist(motlist_in(motdir, WEP_PISTOL))
    hold = next(x for x in mots if x.name.endswith("0160_hold_loop"))
    shoot = next(x for x in mots if x.name.endswith("0500_shoot"))
    rest_from(hold, ref)
    rest_from(shoot, ref)
    out = []
    for aim_base, fire_base, name in PISTOL_POSE_SETS:
        if name not in archives:
            continue
        warc = U.Archive(Path(archives[name]).read_bytes(), ">")
        for k in range(3):
            for kind, base, pl, src in (("aim", aim_base, 0x26, hold), ("fire", fire_base, 0x29, shoot)):
                gdata = warc.entry(pl + k - 4)
                game = fcv.parse(gdata)
                n = game.max_frame
                if kind == "aim":
                    first, length = find_loop(src, (2 * n, 2 * n))
                    frames = list(np.arange(first, first + length + 1e-6, 2.0))
                    fit = Fit(src, model, "leon", 2.0, tol, src.frames, True, frames=frames, loop=True)
                    stance = fit.rows[0]
                    rows = fit.rows
                else:
                    frames = [min(src.frames - 1.0, g * (src.frames - 1.0) / n) for g in range(n + 1)]
                    fit = Fit(src, model, "leon", 2.0, tol, src.frames, True, frames=frames)
                    # the shot is the remake's upper body: its hips' place is not kept (0), the legs and the hips
                    # stand as in the aim (its first frame)
                    rows = [({**r[0], **{q: stance[0][q] for q in RELOAD_STILL if q in stance[0]}}, stance[1])
                            + tuple(r[2:]) for r in fit.rows]
                fixed = {q: model.parts[q].pos for q in (10, 16, 20, 24) if q in fit.order}
                gf = list(range(0, n + 1, 4))
                rows = turn_torso_to(rows, model, fit.order, tol, fixed, game, hand_turn(model, gdata, gf), gf)
                rows = head_as_game(rows, model, gdata)
                data = with_game_arms(rows, fit.order, tol, fixed, game)
                got = hand_turn(model, data, gf)
                off = math.degrees(axis_angle(got @ hand_turn(model, gdata, gf).T)[1])
                print("handgun %s %d (%s, PL_ARC 0x%x): %d game frames, %d bytes, the gun %.1f deg off the game's" % (
                    kind, k, name, pl + k, n, len(data), off))
                out.append((base + k, data))
    return out


# The shotgun's aim (wish of DevJunior, 21.11.2026: "the idle positions of the weapons", "do it"): the remake's hold
# loop (cha0_wp4100H 0160) in place of the game's three-way aim idle (mot3: level, up, down = wep07 0x16, 0x1C, 0x1E,
# PL_ARC 0x1A / 0x20 / 0x22), one cycle as long as the game's (60 frames). The remake has one aim pose (its aiming
# turns the body by itself): up and down are it with the upper body turned from the waist so that the gun (the line
# from the right hand to the left one) points where the game's own pose points it; the level one so too.
SHOTGUN_AIMS = [(0x94, 0x16), (0x95, 0x1C), (0x96, 0x1E)]


def rotation_between(a, b):
    """The smallest rotation turning direction a into b."""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    s = float(np.linalg.norm(v))
    if s < 1e-9:
        return np.eye(3)
    k = v / s
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    ang = math.atan2(s, c)
    return np.eye(3) + math.sin(ang) * K + (1.0 - math.cos(ang)) * (K @ K)


def axis_angle(m):
    """The axis and angle of a rotation matrix."""
    ang = math.acos(max(-1.0, min(1.0, (np.trace(m) - 1.0) / 2.0)))
    if ang < 1e-9:
        return np.array([0.0, 1.0, 0.0]), 0.0
    v = np.array([m[2, 1] - m[1, 2], m[0, 2] - m[2, 0], m[1, 0] - m[0, 1]])
    return v / np.linalg.norm(v), ang


def rot_axis(axis, ang):
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + math.sin(ang) * K + (1.0 - math.cos(ang)) * (K @ K)


# how the aim's turn spreads: the waist (1), the chest (2), the shoulders (5, 11); the neck (3) turns back by a share
# of the body's (the head follows the gun less than the arms, as the game's poses do)
AIM_SPREAD = (0.35, 0.35, 0.30)
AIM_NECK_BACK = 0.5


def spread_aim(local, axis, ang):
    """The rows' local turns with the upper body turned by `ang` about `axis` (model space), spread over its joints."""
    a1, a2, a3 = (ang * f for f in AIM_SPREAD)
    out = dict(local)
    w1 = local[1]
    w2 = w1 @ local[2]
    out[1] = rot_axis(axis, a1) @ w1
    out[2] = w1.T @ rot_axis(axis, a2) @ w1 @ local[2]
    for sh in (5, 11):
        if sh in local:
            out[sh] = w2.T @ rot_axis(axis, a3) @ w2 @ local[sh]
    if 3 in local:
        out[3] = w2.T @ rot_axis(axis, -(a1 + a2) * AIM_NECK_BACK) @ w2 @ local[3]
    return out


def gun_line(model, data, frames):
    """The mean direction from the right hand (part 10) to the left one (16) over `frames` (model space)."""
    from tools.motion import evalhost
    pl = evalhost.Player(model, fcv.parse(data), ik=False)
    d = np.zeros(3)
    for f in frames:
        p = pl.frame(f)
        v = np.array(p.world[16]) - np.array(p.world[10])
        d += v / np.linalg.norm(v)
    return d / np.linalg.norm(d)


# (Mods, 23.11.2026, DevJunior: "and running with the shotgun too? ... it looks odd like this") the remake's way of
# carrying the W-870 on the move: its arm layer (cha0_wp4100M "arm_N": shoulders, arms, hands and fingers, the hands
# 48 cm apart on the gun) over the stand, walk, jog and back step of cha0_general, as the remake plays it; move.dat
# 0x98 + the index of MOVES, the same frames as the movement (in step with it). runtime/locomotion.cpp plays them on
# the upper body in place of the game's (heavyUpper).
WEP_CARRY_SHOTGUN = "cha0_wp4100M.motlist.663"
CARRY_ARMS = {0: "0000_arm_1", 2: "0002_arm_3", 6: "0004_arm_5", 8: "0002_arm_3"}


class _Shifted:
    """A track of the arm layer sampled from the movement's first frame on, looped."""

    def __init__(self, track, first, length):
        self.track, self.first, self.length = track, first, length

    def sample(self, frame):
        return self.track.sample((frame - self.first) % self.length)


class _Frozen:
    """A track of the arm layer held at one frame (the "Classic Stance" mod's freeze)."""

    def __init__(self, track, frame):
        self.track, self.frame = track, frame

    def sample(self, frame):
        return self.track.sample(self.frame)


# (Mods, 26.11.2026, DevJunior: "while the pistol is equipped, the classic stance with the remake's torso, hips and
# legs"; the "Resident Evil 4 Remake Classic Stance" REFramework mod he sent plays the SG-09 R's arm layer motion 0 of
# bank 1000 held at frame 60: the gun up before the chest as in the 2005 game) cha0_wp4000M "0000_arm_1" held at frame
# 60 on the arm bones of each movement, the right hand turned to hold the gun as the game does; move.dat 0xC8 + the
# index of MOVES (runtime/locomotion.cpp gPistolCarry, played on the arms while a handgun is out and not aimed)
WEP_CARRY_PISTOL = "cha0_wp4000M.motlist.663"
PISTOL_STANCE = ("0000_arm_1", 60.0)
# (Mods, 26.11.2026, DevJunior, device test of the classic stance: "better go back to the remake's normal animation for
# the pistol and for running": hunched while running with the gun held up before the chest) the classic stance off:
# the remake's carry instead (cha0_wp4000M arm_1 / arm_3 / arm_5 of the stand, walk and jog, as CARRY_ARMS)
PISTOL_CLASSIC = False


def frozen(base, arm, frame):
    """`base` with the arm layer `arm` held at `frame` on its arm bones."""
    out = copy.copy(base)
    out.tracks = {k: v for k, v in base.tracks.items() if not any(k[1] == h for (_b, h) in arm.tracks)}
    by_hash = {b.hash: i for i, b in enumerate(base.bones)}
    for (bi, h), per in arm.tracks.items():
        out.tracks[(by_hash.get(h, bi), h)] = {kind: _Frozen(t, frame) for kind, t in per.items()}
    return out


def carried(base, arm, first):
    """`base` (a movement of cha0_general) with the arm layer `arm` on its arm bones from `first` on."""
    out = copy.copy(base)
    out.tracks = {k: v for k, v in base.tracks.items() if not any(k[1] == h for (_b, h) in arm.tracks)}
    by_hash = {b.hash: i for i, b in enumerate(base.bones)}
    for (bi, h), per in arm.tracks.items():
        out.tracks[(by_hash.get(h, bi), h)] = {kind: _Shifted(t, first, arm.frames) for kind, t in per.items()}
    return out


# (Mods, 23.11.2026, DevJunior's videos 117199..117207: "it does not hold the gun well aiming, nor shooting") the arms:
# Leon's right shoulder (5) and left one (11) and every part under them (hands, fingers, the gun's grips)
ARM_PARTS = (5, 6, 7, 8, 9, 10, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 117, 113,
             11, 12, 13, 14, 15, 16, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 118, 114)


def with_game_arms(rows, order, tol, fixed, game, keep_game=(32,)):
    """FCV of `rows` with the game's own arms (ARM_PARTS: the hands on the gun as the game holds it) and the game's
    parts `keep_game`; ours for the rest."""
    mot = RT.build_motion(rows, order, tol, root=None, fixed_pos=fixed)
    mot.joints = [j for j in mot.joints if j.parts_no not in ARM_PARTS and j.parts_no not in keep_game]
    ours = set(j.parts_no for j in mot.joints)
    mot.joints += [j for j in game.joints if j.parts_no not in ours or j.parts_no in keep_game]
    mot.layout = list(range(len(mot.joints)))
    return fcv.serialise(mot)


def shotgun_aims(motdir, ref, model, tol, wep):
    """The rows (code, entry data) of the shotgun's three aim poses for move.dat: the remake's hold loop of the W-870 on
    the legs and the torso, the game's own arms (the hands on the gun as the game holds it; device report 23.11.2026:
    the left hand above the gun), the torso turned until the gun lies on the game's line."""
    _v, mots = R.read_motlist(motlist_in(motdir, WEP_RELOAD_SHOTGUN))
    m = next(x for x in mots if x.name.endswith("0160_hold_loop"))
    rest_from(m, ref)
    out = []
    for code, entry in SHOTGUN_AIMS:
        game = fcv.parse(wep.entry(entry))
        n = game.max_frame
        first, length = find_loop(m, (2 * n, 2 * n))
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, True,
                  frames=list(np.arange(first, first + length + 1e-6, 2.0)), loop=True)
        fixed = {p: model.parts[p].pos for p in (10, 16, 20, 24) if p in fit.order}
        want = gun_line(model, wep.entry(entry), [0, n // 2])
        rows = fit.rows
        # the waist and the chest turned until the game's arms on our torso hold the gun on the game's line
        for _i in range(4):
            have = gun_line(model, with_game_arms(rows, fit.order, tol, fixed, game), range(0, n + 1, 6))
            axis, ang = axis_angle(rotation_between(have, want))
            half = rot_axis(axis, ang * 0.5)
            rows = [({**row[0], 1: half @ row[0][1],
                      2: (half @ row[0][1]).T @ half @ (half @ row[0][1]) @ row[0][2]},) + tuple(row[1:])
                    for row in rows]
        data = with_game_arms(rows, fit.order, tol, fixed, game)
        got = gun_line(model, data, range(0, n + 1, 6))
        print("shotgun aim 0x%x (game 0x%x): %d frames, %d bytes, the game's arms, the gun %.1f deg off the game's" % (
            code, entry, n, len(data), math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(got, want))))))))
        out.append((code, data))
    return out


def grip_frame(model, data, frames, hand=10):
    """How the game's right hand holds the gun: in the hand's own frame (part 10) the gun's line (to the left hand, 16)
    and its up (the world's up across the line), averaged over `frames` of the game's motion `data`."""
    from tools.motion import evalhost
    pl = evalhost.Player(model, fcv.parse(data), ik=False)
    d, u = np.zeros(3), np.zeros(3)
    for f in frames:
        p = pl.frame(f)
        h = np.array(p.mat[hand]).reshape(3, 4)[:, :3]
        h = h / np.linalg.norm(h, axis=0)
        line = np.array(p.world[16]) - np.array(p.world[10])
        line /= np.linalg.norm(line)
        up = np.array([0.0, 1.0, 0.0]) - line[1] * line
        up /= np.linalg.norm(up)
        d += h.T @ line
        u += h.T @ up
    d /= np.linalg.norm(d)
    u -= np.dot(u, d) * d
    u /= np.linalg.norm(u)
    return d, u


# where the carried gun points (model space of a movement, its root's: x the side the game's aim leans to, y up, z
# ahead): ahead and to the left by 45 degrees, its muzzle 25 degrees down, the remake's "low ready". The remake's arm
# layer over the plain movements held it flat across the belly (device report 23.11.2026; in the remake the layer sits
# on a torso turned with the gun, which the plain movements lack)
CARRY_LINE = np.array([math.sin(math.radians(45)) * math.cos(math.radians(25)), -math.sin(math.radians(25)),
                       math.cos(math.radians(45)) * math.cos(math.radians(25))])


def fk(model, row):
    """World rotations and positions of every part for one row (local turns, the hips' position)."""
    par = [p.parent for p in model.parts]
    local = row[0]
    rot, pos = [None] * model.n_parts, [None] * model.n_parts
    for p in range(model.n_parts):
        lr = local.get(p, np.eye(3))
        if par[p] < 0:
            rot[p], pos[p] = lr, np.array(row[1], dtype=np.float64)
        else:
            rot[p] = rot[par[p]] @ lr
            pos[p] = pos[par[p]] + rot[par[p]] @ np.array(model.parts[p].pos)
    return rot, pos


def point_arms(rows, model, target, steps=3):
    """Both arms turned from the shoulders (5, 11) until the line from the right hand to the left lies on `target`."""
    out = []
    for row in rows:
        local = dict(row[0])
        for _i in range(steps):
            rot, pos = fk(model, (local,) + tuple(row[1:]))
            line = pos[16] - pos[10]
            q = rotation_between(line, target)
            for sh in (5, 11):
                w = rot[model.parts[sh].parent]
                local[sh] = w.T @ q @ w @ local.get(sh, np.eye(3))
        out.append((local,) + tuple(row[1:]))
    return out


def grip_fix(rows, model, grip, hand=10):
    """The right hand turned so that the gun it holds as the game does (`grip`: grip_frame) lies on the line to the
    left hand, its top up (device report 23.11.2026: the carried shotgun sideways, flat across the belly): the
    remake's hands are on its gun, but its hand bones hold the gun otherwise than the game's hand does."""
    d, u = grip
    fc = np.column_stack([d, u, np.cross(d, u)])
    par = [p.parent for p in model.parts]
    out = []
    for row in rows:
        local = row[0]
        rot, pos = {}, {}

        def world(p):
            if p in rot:
                return rot[p], pos[p]
            lr = local.get(p, np.eye(3))
            if par[p] < 0:
                rot[p], pos[p] = lr, np.array(row[1], dtype=np.float64)
            else:
                r0, p0 = world(par[p])
                rot[p], pos[p] = r0 @ lr, p0 + r0 @ np.array(model.parts[p].pos)
            return rot[p], pos[p]
        _r10, p10 = world(10)
        _r16, p16 = world(16)
        line = p16 - p10
        line /= np.linalg.norm(line)
        up = np.array([0.0, 1.0, 0.0]) - line[1] * line
        up /= np.linalg.norm(up)
        ft = np.column_stack([line, up, np.cross(line, up)])
        r9, _p9 = world(par[hand])
        new = dict(local)
        new[hand] = r9.T @ (ft @ fc.T)
        out.append((new,) + tuple(row[1:]))
    return out


# (Mods, 23.11.2026, DevJunior's videos 117215/117216 and a picture of the remake: "it does not look like the remake",
# "the reload still fails") the carried shotgun as the remake holds it on the move: across the chest, the stock at the
# right breast, the muzzle down to the left, the left hand on the fore-end by the left hip. Where the hands go, in the
# chest's frame (part 2: x Leon's left, y up, z ahead, mm), and where the elbows point; the arms bent to them (two
# bones: the upper arm 7 / 13, the forearm 8 / 14), the shoulders (5, 11) left as the remake moves them (turning
# them hunched Leon over, v24).
# (v26, device picture 117224: "centre the gun a little more, the arms look off to one side") the right hand on the
# breastbone, the left one a little more ahead; (v27, pictures 117232..117234: "the trigger arm a little more open,
# more angle at the elbow, so that the hand on the fore-end is not so stretched") the right hand higher, its elbow out
# to the side, the left hand higher and nearer (its arm at nine tenths of its reach, v26 at all of it)
# (v29, video 117248: "move that elbow away from the body, it looks too closed") the left hand 5 cm further out, its
# elbow 70 % of the way out (LEFT_ELBOW_OUT): ~26 cm to the side of the spine, the wrist ~32 deg
CARRY_HANDS = {10: np.array([-30.0, 60.0, 250.0]), 16: np.array([260.0, -170.0, 310.0])}
CARRY_POLES = {10: np.array([-1.0, -0.25, -0.4]), 16: np.array([1.0, -1.0, -0.3])}
ARM_CHAIN = {10: (7, 8, 6), 16: (13, 14, 12)}


def carry_ik(rows, model):
    """The arms of `rows` bent so that the hands lie at CARRY_HANDS (see there)."""
    par = [p.parent for p in model.parts]
    out = []
    for row in rows:
        local = dict(row[0])
        for hand, (sh, el, twist) in ARM_CHAIN.items():
            rot, pos = fk(model, (local,) + tuple(row[1:]))
            want = pos[2] + rot[2] @ CARRY_HANDS[hand]
            s0, e0, w0 = pos[sh], pos[el], pos[hand]
            a, b = np.linalg.norm(e0 - s0), np.linalg.norm(w0 - e0)
            t = want - s0
            d = min(np.linalg.norm(t), a + b - 1.0)
            t /= np.linalg.norm(t)
            pole = rot[2] @ CARRY_POLES[hand]
            perp = pole - np.dot(pole, t) * t
            perp /= np.linalg.norm(perp)
            ca = max(-1.0, min(1.0, (a * a + d * d - b * b) / (2.0 * a * d)))
            elbow = s0 + t * a * ca + perp * a * math.sqrt(1.0 - ca * ca)
            w = rot[par[sh]]
            q = w.T @ rotation_between(e0 - s0, elbow - s0) @ w
            local[sh] = q @ local.get(sh, np.eye(3))
            local[twist] = q @ local.get(twist, np.eye(3))
            rot, pos = fk(model, (local,) + tuple(row[1:]))
            w = rot[par[el]]
            local[el] = w.T @ rotation_between(pos[hand] - pos[el], want - pos[el]) @ w @ local.get(el, np.eye(3))
        out.append((local,) + tuple(row[1:]))
    return out


LEFT_ELBOW_OUT = 0.7


def left_on_gun(rows, model, grip16):
    """(v28, device pictures 117244..117246: "the hand on the fore-end crumpled, move that arm back a little") the left
    hand slid along the gun (the line from the right hand to CARRY_HANDS[16]) to where its forearm, straight from the
    hand held as the game holds the fore-end (grip16: grip_frame of part 16), meets the upper arm: the wrist unbent;
    the elbow there. Runs after carry_ik (the right hand in place)."""
    d16, u16 = grip16
    fc = np.column_stack([d16, u16, np.cross(d16, u16)])
    par = [p.parent for p in model.parts]
    out, slid = [], []
    for row in rows:
        local = dict(row[0])
        rot, pos = fk(model, (local,) + tuple(row[1:]))
        r10 = pos[10]
        line = rot[2] @ (CARRY_HANDS[16] - CARRY_HANDS[10])
        line /= np.linalg.norm(line)
        up = np.array([0.0, 1.0, 0.0]) - line[1] * line
        up /= np.linalg.norm(up)
        hand = np.column_stack([line, up, np.cross(line, up)]) @ fc.T
        fore = hand @ np.array([1.0, 0.0, 0.0])
        s0 = pos[13]
        a, b = np.linalg.norm(pos[14] - s0), np.linalg.norm(pos[16] - pos[14])
        best = None
        for t in np.arange(280.0, 520.0, 2.0):
            elbow = r10 + line * t - fore * b
            miss = abs(np.linalg.norm(elbow - s0) - a)
            if best is None or miss < best[0]:
                best = (miss, t, elbow)
        _miss, t, elbow = best
        slid.append(t)
        want = r10 + line * t
        # the elbow that keeps the wrist straight sits in front of the belly: turned about the shoulder-hand line
        # LEFT_ELBOW_OUT of the way to where the pole puts it (out at the side), the wrist bending the rest
        axis = want - s0
        axis /= np.linalg.norm(axis)
        pole = rot[2] @ CARRY_POLES[16]
        e0 = elbow - s0 - np.dot(elbow - s0, axis) * axis
        ep = pole - np.dot(pole, axis) * axis
        turn = math.atan2(np.dot(np.cross(e0, ep), axis), np.dot(e0, ep))
        elbow = s0 + rot_axis(axis, turn * LEFT_ELBOW_OUT) @ (elbow - s0)
        w = rot[par[13]]
        local[13] = w.T @ rotation_between(pos[14] - s0, elbow - s0) @ w @ local.get(13, np.eye(3))
        local[12] = w.T @ rotation_between(pos[14] - s0, elbow - s0) @ w @ local.get(12, np.eye(3))
        rot, pos = fk(model, (local,) + tuple(row[1:]))
        w = rot[par[14]]
        local[14] = w.T @ rotation_between(pos[16] - pos[14], want - pos[14]) @ w @ local.get(14, np.eye(3))
        out.append((local,) + tuple(row[1:]))
    print("shotgun carry: the left hand %.0f..%.0f mm along the gun from the right" % (min(slid), max(slid)))
    return out


def unwind(rows, model, share=0.6):
    """The hands' turn about the forearm (device picture 117224: the left hand on the fore-end looked upside down, its
    wrist twisted 140 deg) moved into the forearm: `share` of it into the forearm's twist bone (9, 15), the rest into
    the elbow (8, 14), both about the forearm's own line (the hand stays where and as it is)."""
    out = []
    for row in rows:
        local = dict(row[0])
        for el, tw, hand in ((8, 9, 10), (14, 15, 16)):
            rot, _pos = fk(model, (local,) + tuple(row[1:]))
            hw = rot[hand]
            lh = rot[tw].T @ hw
            t = math.atan2(lh[2, 1] - lh[1, 2], lh[1, 1] + lh[2, 2])
            local[el] = local.get(el, np.eye(3)) @ rot_axis(np.array([1.0, 0.0, 0.0]), t * (1.0 - share))
            local[tw] = local.get(tw, np.eye(3)) @ rot_axis(np.array([1.0, 0.0, 0.0]), t * share)
            rot, _pos = fk(model, (local,) + tuple(row[1:]))
            local[hand] = rot[tw].T @ hw
        out.append((local,) + tuple(row[1:]))
    return out


def hand_offset(motdir, ref, model, tol, grip):
    """The turn that makes the game's gun lie in our right hand (part 10) as the remake's lies in its: from the remake's
    hold loop (both hands on the gun), the hand's turn of grip_fix there, averaged (the remake's hand bone holds the
    gun otherwise than the game's hand; device report 23.11.2026: the gun off in the reload)."""
    _v, mots = R.read_motlist(motlist_in(motdir, WEP_RELOAD_SHOTGUN))
    m = next(x for x in mots if x.name.endswith("0160_hold_loop"))
    rest_from(m, ref)
    fit = Fit(m, model, "leon", 2.0, tol, m.frames, True, frames=list(np.arange(0, m.frames, 4.0)))
    fixed = grip_fix(fit.rows, model, grip)
    acc, each = np.zeros((3, 3)), []
    for r0, r1 in zip(fit.rows, fixed):
        c = fk(model, r0)[0][10].T @ fk(model, r1)[0][10]
        acc += c
        each.append(c)
    u, _s, vt = np.linalg.svd(acc)
    off = u @ vt
    spread = max(math.degrees(axis_angle(off.T @ c)[1]) for c in each)
    print("shotgun grip: the game's gun in the remake's hand turned %.0f deg, within %.0f deg over the hold loop" % (
        math.degrees(axis_angle(off)[1]), spread))
    return off


def moves(root, model, tol, out_path, weapons=None, wep07=None, general2=None, general=None, pistols=None):
    """Leon's movements (see --move)."""
    _v, mots = R.read_motlist(Path(general) if general else root / CHA0)
    entries, table, steps = [b""], [], []
    carry = []
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
        # the strikes of a loop: all of them (a loop of several strides), the last frame being the first again
        steps.append([] if not flags & 1 else [(i, f) for i, f in contacts(m, fit.frames) if i < len(fit.frames) - 1])
        if weapons and slot in CARRY_ARMS:
            _v, arms = R.read_motlist(motlist_in(weapons, WEP_CARRY_SHOTGUN))
            arm = next(x for x in arms if x.name.endswith(CARRY_ARMS[slot]))
            cf = Fit(carried(m, arm, first), model, "leon", 2.0, tol, m.frames, True, frames=fit.frames, loop=True)
            if wep07:
                warc7 = U.Archive(Path(wep07).read_bytes(), ">")
                g7 = warc7.entry(SHOTGUN_AIMS[0][1])  # the level aim
                gf = range(0, fcv.parse(g7).max_frame + 1, 6)
                g16 = grip_frame(model, g7, gf, 16)
                cf.rows = unwind(grip_fix(grip_fix(left_on_gun(carry_ik(cf.rows, model), model, g16), model,
                                                   grip_frame(model, g7, gf)), model, g16, 16), model)
            carry.append((0x98 + MOVES.index((slot, name, lengths, flags)), fcv.serialise(cf.motion(cf.scale)),
                          len(cf.frames) - 1))
        if weapons and pistols and "wep01" in pistols and slot in CARRY_ARMS:
            _v, parms = R.read_motlist(motlist_in(weapons, WEP_CARRY_PISTOL))
            if PISTOL_CLASSIC:
                arm = next(x for x in parms if x.name.endswith(PISTOL_STANCE[0]))
                pistol_base = frozen(m, arm, PISTOL_STANCE[1])
            else:  # the remake's own carry: its arm layer of this movement, played in step with it
                arm = next(x for x in parms if x.name.endswith(CARRY_ARMS[slot]))
                pistol_base = carried(m, arm, first)
            pf = Fit(pistol_base, model, "leon", 2.0, tol, m.frames, True, frames=fit.frames, loop=True)
            warc1 = U.Archive(Path(pistols["wep01"]).read_bytes(), ">")
            g1 = warc1.entry(0x27 - 4)  # the level aim idle
            gf1 = range(0, fcv.parse(g1).max_frame + 1, 4)
            pf.rows = grip_fix(pf.rows, model, grip_frame(model, g1, gf1))
            carry.append((0xC8 + MOVES.index((slot, name, lengths, flags)), fcv.serialise(pf.motion(pf.scale)),
                          len(pf.frames) - 1))
        dist = np.linalg.norm(np.array(fit.root[-1][0]) - np.array(fit.root[0][0])) * fit.scale
        print("move %-22s slot 0x%x: source %d..%d, %d game frames, %d bytes, %.0f mm a cycle, feet %s, steps %s; "
              "check %.1f/%.1f deg" % (name, slot, first, first + length, len(fit.frames), len(data), dist, feet,
                                       [i for i, _f in steps[-1]], c[0], c[1]))
    for code, name in CLIPS:
        m = next(x for x in mots if x.name.endswith(name))
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, True, loop=code in CLIP_LOOPS)
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        table.append(struct.pack(">BBHHHHH", code, 1 if code in CLIP_LOOPS else 0, len(entries) - 1, len(fit.frames) - 1, 0xFFFF, 0xFFFF, 0))
        st = contacts(m, fit.frames)
        if code in CLIP_LOOPS:
            st = [(i, f) for i, f in st if i < len(fit.frames) - 1]
        steps.append(st)
        end = fit.track(fit.scale)[-1]
        print("clip %-22s 0x%x: %d game frames, %d bytes, ends %.0f mm ahead, %.0f mm aside, turned %.0f deg, steps %s; "
              "check %.1f/%.1f deg" % (name, code, len(fit.frames), len(data), end[2], end[0], math.degrees(end[3]),
                                       [i for i, _f in st], c[0], c[1]))
    for code, name, lengths in STRAFES:
        m = next(x for x in mots if x.name.endswith(name))
        first, length = find_loop(m, lengths)
        fit = Fit(m, model, "leon", 2.0, tol, m.frames, True,
                  frames=list(np.arange(first, first + length + 1e-6, 2.0)), loop=True)
        data = fcv.serialise(fit.motion(fit.scale))
        c = fit.check(data)
        entries.append(data)
        table.append(struct.pack(">BBHHHHH", code, 1, len(entries) - 1, len(fit.frames) - 1, 0xFFFF, 0xFFFF, 0))
        st = [(i, f) for i, f in contacts(m, fit.frames) if i < len(fit.frames) - 1]
        steps.append(st)
        end = fit.track(fit.scale)[-1]
        print("side step %-18s 0x%x: source %d..%d, %d game frames, %d bytes, a cycle %.0f mm ahead, %.0f mm aside, "
              "steps %s; check %.1f/%.1f deg" % (name, code, first, first + length, len(fit.frames), len(data), end[2],
                                                  end[0], [i for i, _f in st], c[0], c[1]))
    if general2:
        _v, g2 = R.read_motlist(Path(general2))
        for code, name in PICKUPS:
            m = next(x for x in g2 if x.name.endswith(name))
            fit = Fit(m, model, "leon", 2.0, tol, m.frames, True)
            data = fcv.serialise(fit.motion(fit.scale))
            c = fit.check(data)
            entries.append(data)
            table.append(struct.pack(">BBHHHHH", code, 0, len(entries) - 1, len(fit.frames) - 1, 0xFFFF, 0xFFFF, 0))
            steps.append([])
            print("pick-up %-24s 0x%x: %d game frames, %d bytes; check %.1f/%.1f deg" % (
                name, code, len(fit.frames), len(data), c[0], c[1]))
    if weapons and wep07:
        warc = U.Archive(Path(wep07).read_bytes(), ">")
        # the aims stay the game's (device report 23.11.2026, v24: the head turned away on firing: the game's fire
        # motions follow its own aim pose); shotgun_aims is kept for a later try
        g7 = warc.entry(SHOTGUN_AIMS[0][1])
        off = hand_offset(weapons, mots[0], model, tol, grip_frame(model, g7, range(0, fcv.parse(g7).max_frame + 1, 6)))
        for code, data in shotgun_reloads(weapons, mots[0], model, tol, warc, off):
            entries.append(data)
            table.append(struct.pack(">BBHHHHH", code, 0, len(entries) - 1, fcv.parse(data).max_frame, 0xFFFF, 0xFFFF,
                                     0))
            steps.append([])
    if weapons and pistols:
        for code, data in pistol_reloads(weapons, mots[0], model, tol, pistols) + pistol_poses(weapons, mots[0], model,
                                                                                                tol, pistols):
            entries.append(data)
            table.append(struct.pack(">BBHHHHH", code, 0, len(entries) - 1, fcv.parse(data).max_frame, 0xFFFF, 0xFFFF,
                                     0))
            steps.append([])
    for code, data, last in carry:
        entries.append(data)
        table.append(struct.pack(">BBHHHHH", code, 0, len(entries) - 1, last, 0xFFFF, 0xFFFF, 0))
        steps.append([])
        print("%s carry 0x%x: %d game frames, %d bytes" % ("handgun" if code >= 0xC8 else "shotgun", code, last + 1,
                                                        len(data)))
    if len(table) > 64:  # runtime/locomotion.cpp takes 64 rows at most (32 before 23.11.2026)
        raise SystemExit("move.dat: %d movements, the game takes 64" % len(table))
    stk = b"STK1"
    for st in steps:
        st = st[:24]
        stk += struct.pack(">B", len(st)) + b"".join(struct.pack(">H", i | (0x4000 if f else 0)) for i, f in st)
    entries[0] = b"MOV1" + struct.pack(">I", len(table)) + b"".join(table) + stk
    tags = [b"MOV\x00"] + [b"FCV\x00"] * (len(entries) - 1)
    body = U.build_body(entries, tags)
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    if len(body) > 0x200000:  # runtime/locomotion.cpp kMaxBytes (0x80000 before 23.11.2026)
        raise SystemExit("move.dat: %d bytes, the game takes 2 MB" % len(body))
    out.write_bytes(body)
    print("%s: %d bytes, %d movements" % (out, len(body), len(table)))


DAMAGE = "_chainsaw/animation/ch/chc0/motlist/0th/chc0_damage_0th_com.motlist.663"
# (class of runtime/react.cpp, the motions): 0 body from the front, 1 / 2 right / left forearm, 3 / 4 right / left hand,
# 5 / 6 right shin (light, heavier), 7 / 8 left shin, 9 body from behind, 10 / 11 right / left arm from behind
REACTS = [(0, ["0100_body_small_F_verA", "0101_body_small_FL_verB", "0102_body_small_FR_verC"]),
          (1, ["0300_armR_small_F_verA"]), (2, ["0200_armL_small_F_verA"]),
          (3, ["0310_armR_medium_F_verA", "0311_armR_medium_F_verB"]),
          (4, ["0210_armL_medium_F_verA", "0211_armL_medium_F_verB"]),
          (5, ["0500_LegR_small_F_verA"]), (6, ["0510_LegR_medium_F_verA"]),
          (7, ["0400_LegL_small_F_verA"]), (8, ["0410_LegL_medium_F_verA"]),
          (9, ["0130_body_small_B_verA", "0141_body_medium_BL_verB", "0142_body_medium_BR_verC"]),
          (10, ["0330_armR_small_B_verA"]), (11, ["0230_armL_small_B_verA"])]
REACT_STEP = 3.0
REACT_TRAVEL = 1600.0  # mm a reaction carries the Ganado at most (the game's own: up to 1.5 m; the remake's: 3 m)


# em10.drs: the game's own motion whose hands the reactions keep. (Mods, 26.11.2026, DevJunior: "los ganados aparecen
# con las manos de puerco", hands bent back at the wrist like hooves) 0x37 was a fall, its hands flung back over the
# wrist; now the stand (entry 1): hands in line with the forearm, as the Ganados hold them
REACT_WRISTS = 0x01


def game_wrists(model, data):
    """Per game frame the hands' (parts 10, 16) local rotations of a motion of the game's own."""
    from tools.motion import evalhost
    player = evalhost.Player(model, fcv.parse(data), ik=False)
    out = []
    for f in range(int(player.lib.mot_model_max_frame(player.handle)) + 1):
        pose = player.frame(f)
        out.append({p: np.array(pose.l_mat[p], dtype=np.float64).reshape(3, 4)[:, :3] for p in (10, 16)})
    return out


def reacts(root, model, tol, out_path, ganado_arc=None):
    """The Ganados' hit reactions (see --react)."""
    _v, mots = R.read_motlist(root / DAMAGE)
    wrists = game_wrists(model, ganado_arc.entry(REACT_WRISTS)) if ganado_arc is not None else None
    entries, table = [b""], []
    for cls, names in REACTS:
        for name in names:
            m = next(x for x in mots if x.name.endswith(name))
            fit = Fit(m, model, "ganado", REACT_STEP, tol, m.frames, game_like=True)
            if wrists:
                fit.game_wrists(wrists)
            far = max(math.hypot(t[0], t[2]) for t in fit.track(fit.scale))
            k = min(1.0, REACT_TRAVEL / far) if far > 1.0 else 1.0
            data = fcv.serialise(fit.motion(fit.scale * k))
            c = fit.check(data)
            entries.append(data)
            table.append(struct.pack(">BBHHH", cls, 0, len(entries) - 1, len(fit.frames) - 1, 0))
            end = fit.track(fit.scale * k)[-1]
            print("react class %2d %-26s %3d game frames, %6d bytes, ends %5.0f, %5.0f mm away; check %.1f/%.1f deg" % (
                cls, name, len(fit.frames), len(data), end[0], end[2], c[0], c[1]))
    entries[0] = b"RCT1" + struct.pack(">I", len(table)) + b"".join(table)
    body = U.build_body(entries, [b"RCT\x00"] + [b"FCV\x00"] * (len(entries) - 1))
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(body)
    print("%s: %d bytes, %d reactions" % (out, len(body), len(table)))


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
    ap.add_argument("--box-kick", help="the boxes' kick (melee.dat entries 26 / 27) from a motlist: PATH[:MOTION], "
                    "default motion *%s (the 'Kick That Box' mod's cha0_general2)" % KICK_BOX)
    ap.add_argument("--react", help="also write the Ganados' hit reactions of the 2023 game (melee/react.dat)")
    ap.add_argument("--move", help="also write Leon's movements of the 2023 game (melee/move.dat)")
    ap.add_argument("--weapons", help="with --move: the folder of Leon's weapon motlists (cha0_wp4100H …, the "
                    "'Tactical First-Person Mode' mod's _Chainsaw/Animation/ch/cha0/motlist)")
    ap.add_argument("--wep07", help="with --weapons: the disc's shotgun archive (em/wep07.drs)")
    ap.add_argument("--pistols", help="with --weapons: the disc's handgun archives wep01.drs and wep06.drs, comma "
                    "separated (their reloads, move.dat 0xB0.. / 0xB4..)")
    ap.add_argument("--general2", help="with --move: the remake's cha0_general2.motlist.663 (taking items; before 23.11.2026 "
                    "Ada's cha8_general2 of the 'General Animations Corrector' mod)")
    ap.add_argument("--general", help="with --move: the remake's own cha0_general.motlist.663 (else the one under ROOT)")
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
        kicks(root, leon_model, a.tolerance, a.kicks, a.no_fingers, a.box_from, a.box_kick)
    if a.react:
        reacts(root, ganado_model, a.tolerance, a.react, U.Archive(Path(a.ganado.partition(":")[0]).read_bytes(), ">"))
    if a.move:
        pist = {Path(x).stem.lower(): x for x in a.pistols.split(",")} if a.pistols else None
        moves(root, leon_model, a.tolerance, a.move, a.weapons, a.wep07, a.general2, a.general, pist)
    want = set(a.only.split(",")) if a.only else None
    ganado_wrists = game_wrists(ganado_model, U.Archive(Path(a.ganado.partition(":")[0]).read_bytes(), ">").entry(
        REACT_WRISTS))
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
        if ganado_wrists:
            ef.game_wrists(ganado_wrists)  # hands in line with the forearms (as the reactions'; DevJunior, 26.11.2026)
        fr0 = f["frames"]
        if fr0.get("down"):
            ef.feet_down(min(int(round(fr0["down"] / 2.0)), len(ef.rows) - 1))
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
        # the enemy's body on its first frame in its own frame: the hips' place and the line from the hips to the
        # neck (part 4): a Ganado on the floor is put so that the pair's body lies where his does (runtime)
        hip = ef.world(edata, world, 0, 0)
        neck = ef.world(edata, world, 0, 4)
        line_dir = math.atan2(neck[0] - hip[0], neck[2] - hip[2])
        table.append(struct.pack(">BB6sHHHH6H4f16s3f", f["kind"], f["flags"], weapons, li, ei, si, 0,
                                 len(lf.frames) - 1, ne - 1, g.get("shot", 0), g["kill"], g["down"], g["cancel"],
                                 float(o[0]), float(o[2]), ang, heading, f["name"].encode()[:15],
                                 float(hip[0]), float(hip[2]), line_dir))
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
    entries[0] = ("FIN", b"FIN2" + struct.pack(">I", len(table)) + b"".join(table))
    data = dat(entries)
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print("%s: %d finishers, %d entries, %d bytes" % (out, len(table), len(entries), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
