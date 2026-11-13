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

HANDGUNS = [1, 2, 3, 4, 0x11]  # item.cpp wep_info: the handguns' weapon numbers (em_sub.cpp HandgunCk)


def finishers():
    out = [dict(name="h4", kind=1, flags=1, weapons=[], dir="hatchet_pair_h4", leon="ch4000g1000_leon_preview",
                enemy="ch4000g1000_enemy_preview", frames=H4),
           dict(name="c2", kind=0, flags=2, weapons=HANDGUNS, dir="gun_pair_c2", leon="ch4000g1110_leon_preview",
                enemy="ch4000g1110_enemy_preview", frames=C2)]
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

    def __init__(self, motion, model, map_name, step, tol, end, no_fingers=False):
        body, fingers, nexts = RT.MAPS[map_name]
        mapping = dict(body)
        if not no_fingers:
            mapping.update(fingers)
        mapping = {p: n for p, n in mapping.items() if p < model.n_parts}
        src = RT.Source(motion)
        self.motion_src = motion
        _r, bpos = src.pose(None)
        self.scale = model.parts[0].pos[1] / bpos[src.by_name["Hip"]][1]
        self.frames = list(np.arange(0.0, min(end, motion.frames) + 1e-6, step))
        self.rows = RT.retarget(src, model, mapping, nexts, self.frames, self.scale, root_relative=True)
        self.order = [p for p in sorted(mapping) if mapping[p] in src.by_name and p != 0] + [0]
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
        return RT.build_motion(self.rows, self.order, self.tol, root=self.track(world_scale))

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
        for fi in range(0, len(self.frames), max(1, len(self.frames) // 30)):
            pose = pl.frame(fi)
            target = self.rows[fi][2]
            for part, nx in self.nexts.items():
                if part not in target or nx not in target:
                    continue
                g = np.array(pose.world[nx]) - np.array(pose.world[part])
                r = target[nx] - target[part]
                if np.linalg.norm(g) < 1e-6 or np.linalg.norm(r) < 1e-6:
                    continue
                c = float(np.dot(g, r) / np.linalg.norm(g) / np.linalg.norm(r))
                errs.append(math.degrees(math.acos(max(-1.0, min(1.0, c)))))
        return float(np.mean(errs)), float(np.max(errs))


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
    a = ap.parse_args(argv)

    root = Path(a.mod)
    for sub in ("natives/stm/_re9_melee_proto", "_re9_melee_proto"):
        if (root / sub).is_dir():
            root = root / sub
            break

    def model_of(spec):
        path, _, ent = spec.partition(":")
        arc = U.Archive(Path(path).read_bytes(), ">")
        m = modelbin.parse(arc.entry(int(ent) if ent else 0))
        m.check_tree()
        return m

    leon_model, ganado_model = model_of(a.leon), model_of(a.ganado)
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
        d = root / f["dir"]
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
        k = min(int(round(fr.get("shot", fr["kill"]) / 2.0)), len(lf.frames) - 1, len(ef.frames) - 1)
        lpart, epart = f.get("contact", (10, 4))
        goal = ef.source(k, epart, lf.root[0], world) - lf.source(k, lpart, lf.root[0], world)
        have = (o + RT.rot_y(heading) @ ef.world(edata, world, k, epart)) - lf.world(ldata, world, k, lpart)
        shift = goal - have
        o = o + np.array([shift[0], 0.0, shift[2]])
        ang = -math.atan2(o[0], o[2])
        ne = len(ef.frames)
        g = {k: int(round(v / 2.0)) for k, v in fr.items()}
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
