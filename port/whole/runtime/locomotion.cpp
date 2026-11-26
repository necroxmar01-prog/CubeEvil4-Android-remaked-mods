// Leon's movements of the 2023 remake (wish of DevJunior, 13.11.2026: "remaster Leon's movements"; phase 1: stand, walk,
// run, walk back). Compiled with the game's flags into libre4_main (re4_gamehooks_obj). The app's "Remake movement"
// (the third aim word's bit 14, on by default) switches it; it needs a mod pack's melee/move.dat
// (port/tools/re9_finisher_pack.py --move: one seamless cycle of each movement from the PC mod's cha0_general).
//
// Patch 0923 (pl_wep.cpp cPlayer::weaponInit, after the weapon's module filled the motion table m_MotTbl) asks
// re4_move_table: with bare hands (weapon 0, the quick slots' "weapon put away"), a handgun (em_sub.cpp HandgunCk) or a
// grenade the
// table's stand (0 / 1), walk (2 / 3), run (6 / 7) and back step (8 / 9) take the remake's motion and a sequence made
// here, in the hurt pairs (life under two thirds: m_MotTbl 0x5F..) too. The remake's motions are unarmed ones: with a
// rifle or shotgun in both hands the game's own stay, their legs the remake's (heavyLegs); the turns in place and
// everything else stay the game's.
// The game's speed stays: per movement the game's own motion tells how far Leon goes a frame (its root joint, kind 1,
// over its sequence's frames); the remake's cycle is played faster or slower by the square root of the ratio (its
// sequence maps frames in 10.6 fixed point; at most kTimeMin..kTimeMax) and its root motion scaled by the rest, so the
// feet slide as little as the game's pace allows. The footstep sounds are the game's own two of that movement, on the
// frames the remake's feet strike.
//
// The transitions (wish of DevJunior, 16.11.2026: "starts, stops and turns as in the remake"; Leon still turns his body
// to where he goes): move.dat's clips (codes 0x80..0x87 in its table) play between the game's own motions. A frame
// hook (before the routines) sees the game set a movement's motion and puts the remake's clip first: setting off
// into a walk or a run from standing, stopping into the stand from a walk or a run; when a clip ends the movement's
// loop follows (hokan kLoopHokan). With "Free movement" (runtime/free_move.cpp asks re4_move_turn before it turns
// Leon toward the stick) a stick pointing back (beyond kTurnStart) turns him about with the remake's turn: running,
// the jog's 180-degree turn, standing, the turn in place; their root turn (joint kind 0x40) turns Leon, free_move
// leaves his heading alone meanwhile. The clips play at their loop's pace (the walk's or the run's speed and root
// scale; the turns kTurnSpeed); any routine change of the game cuts them (its own motion replaces the clip).
#include "types.h"
#include "global.h"
#include "main.h"
#include "dvd.h"
#include "re4_runtime.h"
#include "player.h"
#include "motion.h"
#include "em.h"
#include "em_sub.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" const char* re4_mod_move_path(void);              // runtime/dvd.cpp
extern "C" int re4_free_move_on(void);                       // runtime/aim_options.cpp
extern cPlayer* pPL;

namespace {

const int kMoves = 4;
const int kSlots[kMoves] = {0, 2, 6, 8};  // m_MotTbl: stand, walk, run, back step (the sequence in the next slot)
// their hurt pairs (cPlayer::motionSet takes them while dmMotCk() is 0: life under two thirds; wish of DevJunior,
// 18.11.2026: "the movement animations do not work when hurt"): the same remake motions
const int kHurt[kMoves] = {0x5F, 0x61, 0x65, 0x63};
const char* const kNames[kMoves] = {"stand", "walk", "run", "back step"};
// The head's turn toward an enemy (cPlNeck) blends the body with the game's own "looking right / left" variant of the
// stand, the walk and the run (m_MotTbl 0x3F / 0x39 / 0x41 and the next slot): two different walks mixed, out of step
// (device report 13.11.2026: the walk hitched and repeated). With the remake's motion in a slot its variants are
// taken out (cPlNeck::init leaves invalid ones alone) and the blend's rate held at zero.
const int kNeck[kMoves] = {0x3F, 0x39, 0x41, 0};
const f32 kTimeMin = 0.75f, kTimeMax = 1.35f;
const unsigned kMaxBytes = 0x200000;  // (pack v23 with the shotgun: 0.5 MB, one over the 0x80000 before 23.11.2026)
const int kSeqMax = 255;

const int kStepsMax = 24;

struct Move {
    u8* mot;        // the remake's motion (0 none)
    u8 flags;       // 1: matched to the game's speed
    u16 last;       // its last frame (the first pose again)
    u16 foot[2];    // the frames the right and the left foot strike (0xFFFF none)
    u8 nSteps;      // every footstep (move.dat's "STK1": frame, 0x4000 the left foot; 0 none: foot[] only)
    u16 steps[kStepsMax];
    u8 se[2];       // the game's footstep sounds of this movement (right, left) as fit() found them
    f32 scale;      // the scale its root motion has now
    f32 time;       // its playing speed (1: the remake's own)
    void* orig;     // the game's motion and sequence it stands in for now
    void* origSeq;
    void* origH;    // and the hurt pair's (kHurt)
    void* origHSeq;
    void* neck[2];  // the game's looking variants taken out
    bool neckOut;
    u16 seq[2 + 2 * (kSeqMax + 1)];
};

std::atomic<int> gOn(-1);  // the app's switch; -1 none sent yet (the PC: RE4_REMAKE_MOVE, else on)
char gLoaded[1600];
u8* gPlace;
Move gMove[kMoves];
int gCount;
// the transitions' clips: 0 / 1 setting off into / stopping from a walk, 2 / 3 a run, 4 / 5 the run's turn about to the
// left / right, 6 / 7 the turn about standing (move.dat codes 0x80 + i)
// 8..11 the sprint: its loop, setting off into it from standing, stopping from it, going into it from the jog
// 12..15 the side steps while aiming (Mods, 21.11.2026): walking left, right, forward-left, back-right (re4_move_strafe)
const int kClips = 16;
const char* const kClipNames[kClips] = {"walk start",   "walk stop",      "run start",  "run stop",
                                        "run turn left", "run turn right", "turn left",  "turn right",
                                        "sprint",        "sprint start",   "sprint stop", "sprint from the jog",
                                        "side step left", "side step right", "side step forward-left",
                                        "side step back-right"};
// The sprint (wish of DevJunior, 17.11.2026: "jog with the stick, really run with the run button, as the remake"):
// with the remake's movements and Free movement the stick walks; the left stick pressed (L3, the app's "Jog" action,
// a toggling bit of the aim words: re4_move_jog_press) while he walks makes him jog (the game's run routine: the
// remake's jog) until he stops or L3 is pressed again (device report 18.11.2026: "to jog you press the left stick while
// walking"; until then the stick pushed beyond 0.6 jogged: a stick near it switched walk and run back and forth, the
// run looked cut and Leon went forward and back); the run key sprints (its dash, its own start, stop and the way into
// it from the jog). aim_options.cpp re4_run_held asks re4_move_sprint.
// The jog at kJogPace of the game's run (device report 18.11.2026: "lower the speed by at least 10 %": the jog 140 a frame
// then, the sprint 1.1 times its pace, its own strides 1.2 times as long: 180); the sprint at the jog's pace and its own
// longer strides (about 143 a frame, the game's run); (Mods, 21.11.2026, DevJunior: "lower the running speed by 5 %")
// the sprint played 5 % slower: about 136 a frame
const f32 kJogPace = 0.85f;
const f32 kSprintSpeed = 0.95f;
bool gSprintKey;  // the run key held (the sprint)
std::atomic<int> gJogPresses(0);  // L3 presses not yet seen (any thread)
bool gJog;                        // he jogs (L3 while walking)
int gJogIdle;                     // frames he has stood (gJog ends after kJogIdle)
const int kJogIdle = 6;
Move gClip[kClips];
// (Mods, 21.11.2026) Leon's own weapon motions of the remake: the shotgun's reload per level (move.dat codes 0x90 +
// level; re4_move_reload, patch 0944)
u8* gReload[3];
u16 gReloadLast[3];
// (Mods, 25.11.2026, DevJunior: "the other weapons' animations", handguns first) the handguns' reload per level in two
// sets of lengths (move.dat 0xB0 + level: 52/45/28 frames, wep01/02/04; 0xB4 + level: 56/47/29, wep06), taken by the
// game motion's own length (pl_handgun.cpp wep02_r2_reload, patch 0955)
u8* gHgReload[2][3];
u16 gHgReloadLast[2][3];
// (Mods, 26.11.2026, DevJunior: "the Remake's weapon animations") the handguns' aim idle and fire, down / level / up
// (pl_handgun.cpp mot3: PL_ARC 0x26..0x28, 0x29..0x2B), the remake's body with the game's arms: set 0 (move.dat 0xB8 +
// k aim, 0xBC + k fire; wep01..04, 38: fire 21 frames), set 1 (0xC0 + k, 0xC4 + k; wep05, 06: fire 23); re4_move_hg_pose,
// patch 0956
u8* gHgPose[2][2][3];      // set, 0 aim / 1 fire, k
u16 gHgPoseLast[2][2][3];
// (Mods, 23.11.2026) the shotgun's three aim poses (move.dat codes 0x94..0x96 for the game's WEP entries 0x16, 0x1C,
// 0x1E: PL_ARC 0x1A, 0x20, 0x22 of wep07_r3_set00 / fire00), the remake's hold loop with the gun on the game's line;
// re4_move_aim, patch 0950
u8* gAim[3];
u16 gAimLast[3];
// (Mods, 23.11.2026) the remake's way of carrying the shotgun on the move: its arm layer over the stand, walk, jog and
// back step (move.dat 0x98 + k, k as gMove, the same frames as gMove[k]); heavyUpper plays them on the upper body in
// place of the game's own while the shotgun is out
Move gCarry[kMoves];
// (Mods, 26.11.2026, DevJunior: "while the pistol is equipped, the classic stance with the remake's torso, hips and
// legs") the handguns' high ready (the "Classic Stance" mod's pose: cha0_wp4000M arm_1 held at its frame 60, the gun
// up before the chest) as an arm layer over the stand, walk, jog and back step (move.dat 0xC8 + k, the frames of
// gMove[k]); heavyUpper plays it on the arms while a handgun is out and not aimed
Move gPistolCarry[kMoves];
// (Mods, 21.11.2026, DevJunior: "add the animation of taking objects") the remake's hand reaching for an item, on the
// upper body (move.dat codes 0xA0 on the move, 0xA1 standing for a low item, 0xA2 at a table's height; re4_move_pickup)
Move gPick[3];
int gPickNow = -1;     // the one playing, -1 none
f32 gPickFrame;        // its frame
const f32 kPickSpeed = 0.85f;  // its frames per game frame (the remake's at 30 a second a little hurried)
const f32 kPickFade = 4.0f;    // frames its share takes in and out
int gPlaying = -1;     // the clip Leon plays (-1 none)
int gWasMoving = 0;    // the movement he was in (1 walk, 3 run; 0 none): the stop's clip
const f32 kTurnStart = 2.4f;   // radians the stick points away from him for a turn about (about 140 degrees)
const f32 kTurnSpeed = 1.35f;  // the turns' speed against the remake's own (its 0.9 / 1.5 s too slow beside the game)
// the turn in place standing (Mods, 19.11.2026, DevJunior: "standing among enemies the turn on the spot is slow, it
// left me exposed"): faster still
const f32 kTurnInPlace = 2.1f;
const int kClipHokan = 5;      // frames a clip blends in
const int kLoopHokan = 6;      // frames the loop after it blends in
// (Mods, 21.11.2026, DevJunior: "the change from jog to run and from walk to run smooth, for the eye, but without it
// getting slow") into the sprint while moving and out of it: the loop at the frame with the same foot forward, its pose
// blended over kSprintBlend frames and its speed too (Mot_attr 0x400: MotionMoveCore blends the step from the last
// motion's), from the first frame on: no waiting, no foot that jumps
const int kSprintBlend = 8;
int gApplied = -1;         // what the table holds: 1 the remake's, 0 the game's, -1 unknown

u32 be32At(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

u16 be16At(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

u16 get16(const u8* p)
{
    u16 v;
    memcpy(&v, p, 2);
    return v;
}

f32 keyF32(const u8* p)
{
    const u32 u = be32At(p);
    f32 v;
    memcpy(&v, &u, 4);
    return v;
}

void putKeyF32(u8* p, f32 v)
{
    u32 u;
    memcpy(&u, &v, 4);
    p[0] = (u8) (u >> 24), p[1] = (u8) (u >> 16), p[2] = (u8) (u >> 8), p[3] = (u8) u;
}

int keyBytes(int fmt)
{
    static const int kBytes[16] = {12, 8, 6, 0, 10, 6, 4, 0, 9, 5, 3, 0, 0, 0, 0, 4};
    return kBytes[fmt & 15];
}

bool on()
{
    int v = gOn.load();
    if (v < 0) {
        const char* env = getenv("RE4_REMAKE_MOVE");
        v = env ? atoi(env) != 0 : 1;
    }
    return v != 0;
}

// The root position's curve of a converted motion (its joint of kind 1: three axes of u16 count, u16 frames, keys whose
// values stay big-endian), 0 without one; the offsets are relative until the game first plays the motion
u8* rootCurve(u8* m, int* fmt)
{
    const unsigned n = m[2];
    const u32 table = (3 + n * 3 + 3) & ~3u;
    for (unsigned j = 0; j < n; j++) {
        const u16 kind = get16(m + 3 + j * 2);
        if ((kind & 0xFF) != 1) {
            continue;
        }
        u32 off;
        memcpy(&off, m + table + 4 + j * 4, 4);
        *fmt = kind >> 12;
        return (s32) off >= 0 ? m + off : (u8*) (unsigned long) off;
    }
    return 0;
}

// How far the root goes over the motion on the floor (x, z: the last key against the first); false when it cannot be read
bool travel(u8* m, f32* dx, f32* dz)
{
    int fmt = 0;
    u8* p = m ? rootCurve(m, &fmt) : 0;
    if (!p || fmt > 2) {
        return false;
    }
    f32 d[3] = {0.0f, 0.0f, 0.0f};
    for (int axis = 0; axis < 3; axis++) {
        const unsigned count = get16(p);
        if (count == 0 || count > 0x400) {
            return false;
        }
        const u8* keys = p + 2 + count * 2;
        d[axis] = keyF32(keys + (count - 1) * keyBytes(fmt)) - keyF32(keys);
        p = (u8*) keys + count * keyBytes(fmt);
    }
    *dx = d[0], *dz = d[2];
    return d[0] == d[0] && d[2] == d[2] && fabsf(d[0]) < 100000.0f && fabsf(d[2]) < 100000.0f;
}

// The remake's root motion (ours: f32 keys with f32 slopes) scaled on the floor
void scaleRoot(Move& mv, f32 to)
{
    int fmt = 0;
    u8* p = rootCurve(mv.mot, &fmt);
    if (!p || fmt != 0 || mv.scale <= 0.0f || to <= 0.0f || to == mv.scale) {
        return;
    }
    const f32 k = to / mv.scale;
    for (int axis = 0; axis < 3; axis++) {
        const unsigned count = get16(p);
        u8* keys = p + 2 + count * 2;
        if (axis != 1) {
            for (unsigned i = 0; i < count * 3; i++) {
                putKeyF32(keys + i * 4, keyF32(keys + i * 4) * k);
            }
        }
        p = keys + count * 12;
    }
    mv.scale = to;
}

void load()
{
    const char* path = re4_mod_move_path();
    if (strcmp(path, gLoaded) == 0) {
        return;
    }
    snprintf(gLoaded, sizeof(gLoaded), "%s", path);
    gCount = 0;
    for (Move& m : gMove) {
        m.mot = 0;
    }
    for (Move& m : gClip) {
        m.mot = 0;
    }
    gPlaying = -1;
    if (gPlace) {
        re4_mod_em_release(gPlace);
        gPlace = 0;
    }
    if (!*path) {
        return;
    }
    FILE* f = fopen(path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    if (f && size >= 0x40 && size <= (long) kMaxBytes) {
        gPlace = (u8*) re4_mod_place_alloc((unsigned long) size, "Leon's remake movements (melee/move.dat)");
    }
    u8* const base = gPlace;
    const bool whole = base && fread(base, 1, (size_t) size, f) == (size_t) size;
    if (f) {
        fclose(f);
    }
    const u32 count = whole ? be32At(base) : 0;
    bool ok = count >= 2 && count <= 64 && 0x10 + count * 8 <= (u32) size;
    u32 at[65];
    for (u32 i = 0; ok && i < count; i++) {
        at[i] = be32At(base + 0x10 + i * 4);
        ok = at[i] >= 0x10 + count * 8 && at[i] < (u32) size && !(at[i] & 3) && (i == 0 || at[i] >= at[i - 1]);
    }
    const u8* tab = ok ? base + at[0] : 0;
    const u32 n = ok && at[1] - at[0] >= 8 && memcmp(tab, "MOV1", 4) == 0 ? be32At(tab + 4) : 0;
    ok = ok && n >= 1 && n <= 64 && 8 + n * 12 <= at[1] - at[0];  // (23.11.2026: 33 rows with the shotgun carried; 32 before)
    Move found[kMoves];
    memset(found, 0, sizeof(found));
    static Move clips[kClips];
    memset(clips, 0, sizeof(clips));
    u8* reload[3] = {0, 0, 0};
    u8* hg[2][3] = {};
    u16 hgLast[2][3] = {};
    u8* hgPose[2][2][3] = {};
    u16 hgPoseLast[2][2][3] = {};
    u8* aim[3] = {0, 0, 0};
    u8* carry[kMoves] = {};
    u16 carryLast[kMoves] = {};
    u8* pcarry[kMoves] = {};
    u16 pcarryLast[kMoves] = {};
    u16 aimLast[3] = {0, 0, 0};
    u16 reloadLast[3] = {0, 0, 0};
    u8* pick[3] = {0, 0, 0};
    u16 pickLast[3] = {0, 0, 0};
    for (u32 i = 0; ok && i < n; i++) {
        const u8* r = tab + 8 + i * 12;
        const u16 entry = be16At(r + 2), last = be16At(r + 4);
        if (entry == 0 || entry >= count || last < 2 || be16At(base + at[entry]) != last || !base[at[entry] + 2]) {
            ok = false;
            break;
        }
        if (r[0] >= 0xA0 && r[0] < 0xA3 && last <= kSeqMax) {
            pick[r[0] - 0xA0] = base + at[entry];
            pickLast[r[0] - 0xA0] = last;
        }
        if (r[0] >= 0xC8 && r[0] < 0xC8 + kMoves && last <= kSeqMax) {
            pcarry[r[0] - 0xC8] = base + at[entry];
            pcarryLast[r[0] - 0xC8] = last;
        }
        if (r[0] >= 0x98 && r[0] < 0x98 + kMoves && last <= kSeqMax) {
            carry[r[0] - 0x98] = base + at[entry];
            carryLast[r[0] - 0x98] = last;
        }
        if (r[0] >= 0x94 && r[0] < 0x97) {
            aim[r[0] - 0x94] = base + at[entry];
            aimLast[r[0] - 0x94] = last;
        }
        if (r[0] >= 0x90 && r[0] < 0x93) {
            reload[r[0] - 0x90] = base + at[entry];
            reloadLast[r[0] - 0x90] = last;
        }
        if (r[0] >= 0xB8 && r[0] < 0xC7 && (r[0] & 3) != 3) {
            const int c = r[0] - 0xB8, set = c / 8, fire = (c / 4) & 1, k = c & 3;
            hgPose[set][fire][k] = base + at[entry];
            hgPoseLast[set][fire][k] = last;
        }
        if ((r[0] >= 0xB0 && r[0] < 0xB3) || (r[0] >= 0xB4 && r[0] < 0xB7)) {
            const int set = r[0] >= 0xB4 ? 1 : 0, lv = r[0] - (set ? 0xB4 : 0xB0);
            hg[set][lv] = base + at[entry];
            hgLast[set][lv] = last;
        }
        if (r[0] >= 0x80 && r[0] < 0x80 + kClips && last <= kSeqMax) {
            Move& c = clips[r[0] - 0x80];
            c.mot = base + at[entry];
            c.flags = r[1];
            c.last = last;
            c.scale = 1.0f;
            c.time = 1.0f;
        }
        for (int k = 0; k < kMoves; k++) {
            if (kSlots[k] == r[0]) {
                found[k].mot = base + at[entry];
                found[k].flags = r[1];
                found[k].last = last;
                found[k].foot[0] = be16At(r + 6);
                found[k].foot[1] = be16At(r + 8);
                found[k].scale = 1.0f;
            }
        }
    }
    if (!ok) {
        re4_log("move: %s is no archive of Leon's movements (%ld bytes%s): the game's", path, size,
                size > (long) kMaxBytes ? ", larger than the place for it" : !whole ? ", not read" : "");
        return;
    }
    // every footstep, after the rows: "STK1", per row u8 n and n u16 (packs before 18.11.2026: none, foot[] only)
    int nStk = 0;
    const u8* stk = tab + 8 + n * 12;
    const u8* const stkEnd = base + at[1];
    if (stk + 4 <= stkEnd && memcmp(stk, "STK1", 4) == 0) {
        const u8* q = stk + 4;
        for (u32 i = 0; i < n && q < stkEnd; i++) {
            const unsigned c = *q++;
            if (q + c * 2 > stkEnd) {
                break;
            }
            const u8* r = tab + 8 + i * 12;
            Move* m = 0;
            if (r[0] >= 0x80 && r[0] < 0x80 + kClips) {
                m = &clips[r[0] - 0x80];
            }
            for (int k = 0; k < kMoves; k++) {
                if (kSlots[k] == r[0]) {
                    m = &found[k];
                }
            }
            if (m && m->mot) {
                m->nSteps = (u8) (c < (unsigned) kStepsMax ? c : kStepsMax);
                for (int j = 0; j < m->nSteps; j++) {
                    m->steps[j] = be16At(q + j * 2);
                }
                nStk += m->nSteps;
            }
            q += c * 2;
        }
    }
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = (u32) size;
    h.dest = (u32) (unsigned long) base;
    re4_formats_part_loaded("mods/move.drs", 0, 0, &h, (unsigned) (unsigned long) base);
    for (int k = 0; k < kMoves; k++) {
        gMove[k].mot = found[k].mot;
        gMove[k].flags = found[k].flags;
        gMove[k].last = found[k].last;
        gMove[k].foot[0] = found[k].foot[0];
        gMove[k].foot[1] = found[k].foot[1];
        gMove[k].nSteps = found[k].nSteps;
        memcpy(gMove[k].steps, found[k].steps, sizeof(gMove[k].steps));
        gMove[k].scale = 1.0f;
        gMove[k].orig = gMove[k].origSeq = gMove[k].origH = gMove[k].origHSeq = 0;
        if (gMove[k].mot) {
            gCount++;
            re4_log("move: %s: the remake's %s, %u frames", path, kNames[k], (unsigned) gMove[k].last);
        }
    }
    int nReload = 0;
    for (int i = 0; i < 3; i++) {
        gReload[i] = reload[i];
        gReloadLast[i] = reloadLast[i];
        nReload += reload[i] ? 1 : 0;
    }
    if (nReload) {
        re4_log("move: %s: the remake's shotgun reload, %d levels", path, nReload);
    }
    int nHg = 0;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 3; i++) {
            gHgReload[k][i] = hg[k][i];
            gHgReloadLast[k][i] = hgLast[k][i];
            nHg += hg[k][i] ? 1 : 0;
        }
    }
    if (nHg) {
        re4_log("move: %s: the remake's handgun reloads, %d motions", path, nHg);
    }
    memcpy(gHgPose, hgPose, sizeof gHgPose);
    memcpy(gHgPoseLast, hgPoseLast, sizeof gHgPoseLast);
    int nAim = 0;
    for (int i = 0; i < 3; i++) {
        gAim[i] = aim[i];
        gAimLast[i] = aimLast[i];
        nAim += aim[i] ? 1 : 0;
    }
    if (nAim) {
        re4_log("move: %s: the remake's shotgun aim, %d poses", path, nAim);
    }
    int nCarry = 0;
    for (int i = 0; i < kMoves; i++) {
        Move& m = gCarry[i];
        memset(&m, 0, sizeof(m));
        m.mot = carry[i];
        m.last = carryLast[i];
        m.scale = m.time = 1.0f;
        if (m.mot) {
            nCarry++;
            m.seq[0] = m.last;
            SEQUENCE_DATA* key = (SEQUENCE_DATA*) (m.seq + 2);
            for (int k = 0; k < m.last; k++) {
                key[k].Frame = (u16) (k * 64);
            }
        }
    }
    if (nCarry) {
        re4_log("move: %s: the remake's shotgun carried on the move, %d movements", path, nCarry);
    }
    int nPistol = 0;
    for (int i = 0; i < kMoves; i++) {
        Move& m = gPistolCarry[i];
        memset(&m, 0, sizeof(m));
        m.mot = pcarry[i];
        m.last = pcarryLast[i];
        m.scale = m.time = 1.0f;
        if (m.mot) {
            nPistol++;
            m.seq[0] = m.last;
            SEQUENCE_DATA* key = (SEQUENCE_DATA*) (m.seq + 2);
            for (int k = 0; k < m.last; k++) {
                key[k].Frame = (u16) (k * 64);
            }
        }
    }
    if (nPistol) {
        re4_log("move: %s: the handguns' classic stance, %d movements", path, nPistol);
    }
    int nPick = 0;
    for (int i = 0; i < 3; i++) {
        Move& m = gPick[i];
        memset(&m, 0, sizeof(m));
        m.mot = pick[i];
        m.last = pickLast[i];
        m.scale = m.time = 1.0f;
        if (m.mot) {
            nPick++;
            m.seq[0] = m.last;
            SEQUENCE_DATA* key = (SEQUENCE_DATA*) (m.seq + 2);
            for (int k = 0; k < m.last; k++) {
                key[k].Frame = (u16) (k * 64);
            }
        }
    }
    gPickNow = -1;
    if (nPick) {
        re4_log("move: %s: the remake's taking of items, %d ways", path, nPick);
    }
    int nClips = 0;
    for (int i = 0; i < kClips; i++) {
        gClip[i].mot = clips[i].mot;
        gClip[i].flags = clips[i].flags;
        gClip[i].last = clips[i].last;
        gClip[i].nSteps = clips[i].nSteps;
        memcpy(gClip[i].steps, clips[i].steps, sizeof(gClip[i].steps));
        gClip[i].scale = 1.0f;
        gClip[i].time = 1.0f;
        nClips += gClip[i].mot ? 1 : 0;
    }
    if (nClips) {
        re4_log("move: %s: %d of the remake's transitions (starts, stops, turns)", path, nClips);
    }
    if (nStk) {
        re4_log("move: %s: %d footsteps", path, nStk);
    }
    gApplied = -1;
}

// The footstep sounds on a made sequence's keys (`perFrame` keys a motion frame): every strike of the pack, else the
// right and the left foot's one (packs before 18.11.2026); se: the right foot's sound, the left one's
void putSteps(const Move& mv, SEQUENCE_DATA* key, int keys, f32 perFrame, const u8 se[2])
{
    if (mv.nSteps) {
        for (int i = 0; i < mv.nSteps; i++) {
            const int at = (int) ((f32) (mv.steps[i] & 0x3FFF) * perFrame + 0.5f);
            key[at >= keys ? keys - 1 : at].Se = se[(mv.steps[i] & 0x4000) ? 1 : 0];
        }
        return;
    }
    for (int foot = 0; foot < 2; foot++) {
        if (mv.foot[foot] != 0xFFFF) {
            const int at = (int) ((f32) mv.foot[foot] * perFrame + 0.5f);
            key[at >= keys ? keys - 1 : at].Se = se[foot];
        }
    }
}

// The remake's movement k in place of the game's (`mot`, `seq`): its root motion scaled and its sequence made
void fit(int k, void* mot, void* seq)
{
    Move& mv = gMove[k];
    const f32 frames = (f32) mv.last;
    f32 time = 1.0f, scale = 1.0f, pace = 0.0f, own = 0.0f;
    if (mv.flags & 1) {
        f32 gx = 0.0f, gz = 0.0f, ox = 0.0f, oz = 0.0f;
        const u8* gm = (const u8*) mot;
        const unsigned gFrames = seq ? get16((const u8*) seq) : (unsigned) (get16(gm) & 0x3FFF);
        scaleRoot(mv, 1.0f);
        if (gFrames && travel((u8*) mot, &gx, &gz) && travel(mv.mot, &ox, &oz)) {
            pace = sqrtf(gx * gx + gz * gz) / (f32) gFrames;
            own = sqrtf(ox * ox + oz * oz) / frames;
        }
        if (k == 2) {
            pace *= kJogPace;  // the jog slower than the game's run (the sprint is the fast one)
        }
        if (pace > 5.0f && own > 5.0f) {
            time = sqrtf(pace / own);
            time = time < kTimeMin ? kTimeMin : time > kTimeMax ? kTimeMax : time;
        }
    }
    int keys = (int) (frames / time + 0.5f);
    keys = keys < 2 ? 2 : keys > kSeqMax ? kSeqMax : keys;
    if (pace > 5.0f && own > 5.0f) {
        scale = pace * (f32) keys / (own * frames);
        scaleRoot(mv, scale);
    }
    mv.time = time;
    // the footsteps: the game's own two sounds of this movement, in their order
    u8 se[2] = {(u8) (k == 1 ? 3 : 1), (u8) (k == 1 ? 4 : 2)};
    if (seq) {
        const u8* s = (const u8*) seq;
        int got = 0;
        for (unsigned i = 0; i < get16(s) && got < 2; i++) {
            if (s[4 + i * 4 + 2]) {
                se[got++] = s[4 + i * 4 + 2];
            }
        }
    }
    memset(mv.seq, 0, sizeof(mv.seq));
    mv.seq[0] = (u16) keys;
    SEQUENCE_DATA* key = (SEQUENCE_DATA*) (mv.seq + 2);
    for (int i = 0; i < keys; i++) {
        key[i].Frame = (u16) ((f32) i * frames / (f32) keys * 64.0f + 0.5f);
    }
    mv.se[0] = se[0];
    mv.se[1] = se[1];
    if (k != 0) {
        putSteps(mv, key, keys, (f32) keys / frames, se);
    }
    re4_log("move: the remake's %s for weapon %d: the game's pace %.0f a frame, the cycle's %.0f: played at %.2f (%d "
            "frames), its steps scaled %.2f",
            kNames[k], (int) pG->weapon_no, (double) pace, (double) own, (double) time, keys, (double) scale);
}

// A transition's clip at its loop's pace (the walk's for 0 / 1, the run's for 2..5, kTurnSpeed for the turns): its
// root motion scaled as the loop's, its sequence made (no events)
void fitClip(int i)
{
    Move& c = gClip[i];
    if (!c.mot) {
        return;
    }
    if (i >= 12) {
        // the side steps: at their own pace (runtime/free_move.cpp plays them on the legs at the aiming walk's speed)
        scaleRoot(c, 1.0f);
        const int keys = c.last < 2 ? 2 : c.last > kSeqMax ? kSeqMax : c.last;
        c.time = 1.0f;
        memset(c.seq, 0, sizeof(c.seq));
        c.seq[0] = (u16) keys;
        SEQUENCE_DATA* key = (SEQUENCE_DATA*) (c.seq + 2);
        for (int k = 0; k < keys; k++) {
            key[k].Frame = (u16) ((f32) k * (f32) c.last / (f32) keys * 64.0f + 0.5f);
        }
        return;
    }
    const Move* base = i < 2 ? &gMove[1] : i < 6 || i >= 8 ? &gMove[2] : 0;
    f32 time = base && base->mot ? base->time : 1.0f;
    if (i >= 4 && i < 6) {
        time *= kTurnSpeed;
    } else if (i >= 6 && i < 8) {
        time *= kTurnInPlace;
    }
    if (i == 8 || i == 9 || i == 11) {
        time *= kSprintSpeed;
    }
    scaleRoot(c, base && base->mot ? base->scale : 1.0f);
    const f32 frames = (f32) c.last;
    int keys = (int) (frames / time + 0.5f);
    keys = keys < 2 ? 2 : keys > kSeqMax ? kSeqMax : keys;
    c.time = time;
    memset(c.seq, 0, sizeof(c.seq));
    c.seq[0] = (u16) keys;
    SEQUENCE_DATA* key = (SEQUENCE_DATA*) (c.seq + 2);
    // a loop (the sprint's, flags 1) leaves its last frame out: it is its first again, played twice it stood one
    // frame still at every turn of the loop (device report 19.11.2026: the run "looks choppy, as if it repeats");
    // the others end on their last frame
    const bool loop = (c.flags & 1) || i == 8;
    const int span = loop ? keys : keys - 1;
    for (int k = 0; k < keys; k++) {
        key[k].Frame = (u16) ((f32) k * frames / (f32) span * 64.0f + 0.5f);
    }
    // its footsteps with the sounds of its movement (the walk's for the walk's start and stop and the turns in place,
    // the run's for the rest; device report 18.11.2026: "no footsteps are heard" while sprinting)
    const Move& snd = i < 2 || i == 6 || i == 7 ? gMove[1] : gMove[2];
    if (snd.mot && snd.se[0]) {
        putSteps(c, key, keys, (f32) span / frames, snd.se);
    }
}

// The throwables (read.cpp groups them for the other characters: hand, incendiary and flash grenades, the eggs), held in
// one hand as a handgun is (wish of DevJunior, 13.11.2026: "with grenades the remake's movements go away")
bool throwable(int no)
{
    return no == 0x13 || no == 0x16 || no == 0x17 || no == 0x19 || no == 0x1F || no == 0x20;
}

// Leon with bare hands, a handgun or a throwable: the remake's motions as they are
bool light()
{
    return pG && pG->pl_type == 0 && (pG->weapon_no == 0 || HandgunCk(pG->weapon_no) || throwable(pG->weapon_no));
}

// The remake's movements for every weapon of Leon's: with a big one (rifle, shotgun, TMP, launcher) its upper body is
// the game's (heavyUpper; device report 19.11.2026: "with shotguns and long weapons he does not move well" with the
// remake's legs under the game's walk, 18.11: the game's pace and steps, the remake's legs out of step)
bool fits()
{
    return pG && pG->pl_type == 0;
}

// The table's four movements: the remake's (the game's kept aside) or the game's back
void apply(cPlayer* pl, bool fresh)
{
    if (!pl || !pl->m_MotTbl) {
        return;
    }
    load();
    const bool want = on() && gCount && fits();
    void** tbl = pl->m_MotTbl;
    for (int k = 0; k < kMoves; k++) {
        Move& mv = gMove[k];
        const int slot = kSlots[k];
        const int hurt = kHurt[k];
        const bool ours = tbl[slot] == mv.mot && mv.mot;
        const bool oursH = tbl[hurt] == mv.mot && mv.mot;
        if (!ours) {
            mv.orig = tbl[slot];
            mv.origSeq = tbl[slot + 1];
        }
        if (!oursH) {
            mv.origH = tbl[hurt];
            mv.origHSeq = tbl[hurt + 1];
        }
        if (want && mv.mot && mv.orig) {
            fit(k, mv.orig, mv.origSeq);
            tbl[slot] = mv.mot;
            tbl[slot + 1] = mv.seq;
            if (mv.origH) {
                tbl[hurt] = mv.mot;  // hurt too (the same movement; the game's hurt ones are slower)
                tbl[hurt + 1] = mv.seq;
            }
            if (kNeck[k] && (fresh || !mv.neckOut)) {
                mv.neck[0] = tbl[kNeck[k]];
                mv.neck[1] = tbl[kNeck[k] + 1];
                mv.neckOut = true;
            }
            if (kNeck[k]) {
                tbl[kNeck[k]] = tbl[kNeck[k] + 1] = 0;
            }
        } else {
            // the hurt pair is Leon's own (cPlLeon::setMotion, his archive): a new weapon (weaponInit refills only
            // 0..0x5E) leaves it ours, it goes back to his
            if (oursH) {
                tbl[hurt] = mv.origH;
                tbl[hurt + 1] = mv.origHSeq;
            }
            // the walk's slot and its neck: only when they are still ours. After a new weapon the neck slots hold
            // the new module's, the ones kept aside the old module's, unloaded (device report 19.11.2026: putting
            // them back crashed the game in MotionMoveCore, "Invalid parts 248", on the second change to the shotgun)
            if (ours) {
                tbl[slot] = mv.orig;
                tbl[slot + 1] = mv.origSeq;
                if (kNeck[k] && mv.neckOut) {
                    tbl[kNeck[k]] = mv.neck[0];
                    tbl[kNeck[k] + 1] = mv.neck[1];
                }
            }
            if (ours || fresh) {
                mv.neckOut = false;
            }
        }
    }
    if (want) {
        for (int i = 0; i < kClips; i++) {
            fitClip(i);
        }
    }
    gPlaying = -1;
    gApplied = want ? 1 : 0;
}

bool isClip(const void* m)
{
    for (const Move& c : gClip) {
        if (c.mot && m == (const void*) c.mot) {
            return true;
        }
    }
    return false;
}

bool rightStrike(const Move& m, f32* at);

// The key of `dst` with the same foot forward as `from` at its sequence frame `seqFrame` (64ths); -1 when unknown
int phaseKey(const Move& from, u16 seqFrame, const Move& dst)
{
    f32 rf = 0.0f, rt = 0.0f;
    if (!from.last || !dst.last || !rightStrike(from, &rf) || !rightStrike(dst, &rt)) {
        return -1;
    }
    f32 frac = ((f32) seqFrame / 64.0f - rf) / (f32) from.last;
    frac -= floorf(frac);
    f32 at = rt + frac * (f32) dst.last;
    at -= at >= (f32) dst.last ? (f32) dst.last : 0.0f;
    const int keys = dst.seq[0];
    const int i = (int) (at * (f32) keys / (f32) dst.last + 0.5f);
    return i >= keys ? 0 : i;
}

// Motion `dst` (a loop: gMove or the sprint's clip) taken over from the current one in step, pose and speed blended
void blendInStep(cPlayer* pl, const Move* from, Move& dst)
{
    const int i = from ? phaseKey(*from, pl->Motion.Seq.Frame, dst) : -1;
    MotionSetCore(pl, &pl->Motion, dst.mot, dst.seq, kSprintBlend, 5 | 0x400, i < 0 ? 0 : i);
    if (pl->pMotionB) {
        pl->pMotionB->Brate = 0.0f;
    }
    static int said;
    if (said < 4) {
        said++;
        re4_log("move: %s, at its frame %d of %d (the same foot forward), pose and speed blended over %d frames",
                &dst == &gClip[8] ? "into the sprint" : "out of the sprint into the jog", i, (int) dst.seq[0],
                kSprintBlend);
    }
}

void play(cPlayer* pl, int i)
{
    Move& c = gClip[i];
    MotionSetCore(pl, &pl->Motion, c.mot, c.seq, kClipHokan, 5, 0);
    if (pl->pMotionB) {
        pl->pMotionB->Brate = 0.0f;
    }
    gPlaying = i;
    static int said;
    if (!(said & (1 << i))) {
        said |= 1 << i;
        re4_log("move: the remake's %s (%u frames at %.2f)", kClipNames[i], (unsigned) c.seq[0], (double) c.time);
    }
}

// Before the routines: the clips around the game's own movements (see the head)
void transitions(cPlayer* pl)
{
    const void* cur = (const void*) pl->Motion.pMot;
    const int r1 = pl->r_no_1;
    if (pl->r_no_0 != 0 || r1 > 4) {
        gPlaying = -1;
        gWasMoving = 0;
        return;
    }
    if (gPlaying == 8 && cur == (const void*) gClip[8].mot && (r1 != 3 || !gSprintKey)) {
        // the sprint let go: back to the jog (the run routine), its stop when the game stands him
        if (r1 == 3 && gMove[2].mot) {
            blendInStep(pl, &gClip[8], gMove[2]);
            gPlaying = -1;
            gWasMoving = 3;
            return;
        }
    }
    if (gPlaying >= 0) {
        Move& c = gClip[gPlaying];
        if (cur != (const void*) c.mot) {
            gPlaying = -1;  // the game set its own motion: the clip is cut
            if (c.mot == gClip[8].mot) {
                gWasMoving = 4;  // the sprint's stop when the game stands him
            }
        } else {
            if (gPlaying == 8) {
                return;  // the sprint's loop plays round until the key or the run ends (below)
            }
            if (pl->Motion.Seq_frame >= (f32) c.seq[0] - 2.0f) {
                const int k = r1 == 1 ? 1 : r1 == 3 ? 2 : r1 == 2 ? 3 : 0;
                if (r1 == 3 && gSprintKey && gClip[8].mot) {
                    play(pl, 8);
                    gWasMoving = 4;
                    return;
                }
                if (gMove[k].mot) {
                    MotionSetCore(pl, &pl->Motion, gMove[k].mot, gMove[k].seq, kLoopHokan, 5, 0);
                }
                gPlaying = -1;
                gWasMoving = r1 == 1 || r1 == 3 ? r1 : 0;
            }
            return;
        }
    }
    const bool fresh = pl->Motion.Seq_frame < 3.0f;
    if (cur == (const void*) gMove[1].mot && gMove[1].mot) {
        if (r1 == 1 && fresh && gWasMoving == 0 && gClip[0].mot) {
            play(pl, 0);
        }
        gWasMoving = 1;
    } else if (cur == (const void*) gMove[2].mot && gMove[2].mot) {
        if (r1 == 3 && gSprintKey && gClip[8].mot) {
            // into the sprint: from standing its start; from the jog or the walk (the game's run routine has just set
            // the jog in step with the walk) the sprint's loop in step, blended (before 21.11.2026: its way in from
            // frame 0, the feet jumped)
            if (fresh && gWasMoving == 0 && gClip[9].mot) {
                play(pl, 9);
            } else {
                blendInStep(pl, &gMove[2], gClip[8]);
                gPlaying = 8;
            }
            gWasMoving = 4;
            return;
        }
        if (r1 == 3 && fresh && gWasMoving == 0 && gClip[2].mot) {
            play(pl, 2);
        }
        gWasMoving = 3;
    } else if (cur == (const void*) gMove[0].mot && gMove[0].mot) {
        if (r1 == 0 && fresh && gWasMoving) {
            const int i = gWasMoving == 4 ? (gClip[10].mot ? 10 : 3) : gWasMoving == 3 ? 3 : 1;
            if (gClip[i].mot) {
                play(pl, i);
            }
        }
        gWasMoving = 0;
    } else {
        gWasMoving = 0;  // another motion (aiming, the back step, an action): no stop's clip after it
    }
}

// The motion frame of a movement's or a clip's first right footstep; false without one
bool rightStrike(const Move& m, f32* at)
{
    for (int i = 0; i < m.nSteps; i++) {
        if (!(m.steps[i] & 0x4000)) {
            *at = (f32) (m.steps[i] & 0x3FFF);
            return true;
        }
    }
    if (!m.nSteps && m.foot[0] != 0xFFFF) {
        *at = (f32) m.foot[0];
        return true;
    }
    return false;
}

// Between the walk and the jog (the run key, L3 or the sprint let go) the game carries the step on: the new motion
// starts at the old one's share of its cycle measured with the run's length for both (pl_R1_Walk / pl_R1_Run: r_no_3
// 4, m_Frame), right for its own walk and run, not for the remake's (the walk's 33 frames, the jog's 181, eight strides,
// the sprint's 18): the feet changed and the motion jumped. Here, before the routine sets it, the remake's motion is
// set at the frame that has the same foot forward, as the routine would (its neck, the run's waist and breath count).
void matchPhase(cPlayer* pl)
{
    if (pl->r_no_0 != 0 || !(pl->r_no_3 & 4)) {
        return;
    }
    const int r1 = pl->r_no_1;
    const int to = r1 == 1 && pl->r_no_2 == 0 ? 1 : r1 == 3 && (pl->r_no_2 == 0 || pl->r_no_2 == 2) ? 2 : 0;
    if (!to || !gMove[to].mot) {
        return;
    }
    const void* cur = (const void*) pl->Motion.pMot;
    const Move* from = cur == (const void*) gMove[1].mot ? &gMove[1] : cur == (const void*) gMove[2].mot ? &gMove[2]
                     : cur == (const void*) gClip[8].mot ? &gClip[8] : 0;
    const Move& dst = gMove[to];
    f32 rf = 0.0f, rt = 0.0f;
    if (!from || from == &dst || !from->last || !dst.last || !rightStrike(*from, &rf) || !rightStrike(dst, &rt)) {
        return;
    }
    f32 frac = ((f32) pl->Motion.Seq.Frame / 64.0f - rf) / (f32) from->last;
    frac -= floorf(frac);
    f32 at = rt + frac * (f32) dst.last;
    at -= at >= (f32) dst.last ? (f32) dst.last : 0.0f;
    const int keys = dst.seq[0];
    int i = (int) (at * (f32) keys / (f32) dst.last + 0.5f);
    i = i >= keys ? 0 : i;
    MotionSetCore(pl, &pl->Motion, dst.mot, (void*) dst.seq, pl->m_Hokan, 5, i);
    const bool own = dmMotCk() != 0;
    if (to == 2) {
        pl->Neck->init(own ? pl->m_MotTbl[0x41] : PL_ARC_PTR(pG->pPlayer, 0x44),
                       own ? pl->m_MotTbl[0x42] : PL_ARC_PTR(pG->pPlayer, 0x45), i);
        pl->Waist->reset();
        pl->m_Work0 = 0;
        pl->r_no_2 = 3;
    } else {
        pl->Neck->init(own ? pl->m_MotTbl[0x39] : PL_ARC_PTR(pG->pPlayer, 0x42),
                       own ? pl->m_MotTbl[0x3A] : PL_ARC_PTR(pG->pPlayer, 0x43), i);
        pl->r_no_2 = 1;
    }
    static int said;
    if (said < 4) {
        said++;
        re4_log("move: %s into the %s at its frame %d of %d (the same foot forward)",
                from == &gMove[1] ? "walk" : from == &gMove[2] ? "jog" : "sprint", to == 2 ? "jog" : "walk", i, keys);
    }
}

// Big weapons (wish of DevJunior, 18.11.2026: "the movement animations do not work with the big weapons"): the
// remake's motions are unarmed; with a rifle or shotgun in both hands Leon moves with the remake's movements (their
// pace, steps, starts, stops, sprint) and his upper body, from the waist up, takes the game's own walk, run, back step
// or stand of that weapon (free_move.cpp re4_legs_overlay with `upper`), in step with the remake's (the first right
// footsteps of both; the sequence's sound of the game's). Until 19.11.2026 the other way round (the game's walk, the
// remake's legs on it): out of step.
extern "C" int re4_legs_overlay(void* pl, void* mot, void* seq, int start, float speed, float at, float legW,
                                float sway, float shift, int upper);
const f32 kHeavyFade = 0.25f;
f32 gHeavyW;
int gHeavyK = 1;

// The share of its cycle where a game's sequence has its first right footstep (its sound 1, 3, 0xE; seqSeCtrl); -1 none
f32 gameStrike(const void* seq)
{
    static const void* last;
    static f32 share = -1.0f;
    if (seq == last) {
        return share;
    }
    last = seq;
    share = -1.0f;
    const u8* s = (const u8*) seq;
    const unsigned n = s ? get16(s) : 0;
    for (unsigned i = 0; i < n; i++) {
        const u8 se = s[4 + i * 4 + 2];
        if (se == 1 || se == 3 || se == 0xE) {
            share = (f32) i / (f32) n;
            break;
        }
    }
    return share;
}

// The movement whose game motion holds the upper body now (0 stand, 1 walk, 2 run, 3 back step), -1 none: one of the
// remake's motions or clips plays in the field's plain routines with a big weapon
// a handgun out with the classic stance loaded (its arms as gPistolCarry)
bool pistolStance()
{
    return pG && pG->pl_type == 0 && pG->weapon_no != 0 && HandgunCk(pG->weapon_no) && gPistolCarry[0].mot;
}

int heavy(cPlayer* pl)
{
    if (!pl || pl != pPL || !pG || gApplied != 1 || (light() && !pistolStance()) || !on() || !gCount || !pl->m_MotTbl ||
        pl->r_no_0 != 0 || pl->r_no_1 > 3) {
        return -1;
    }
    const void* cur = (const void*) pl->Motion.pMot;
    for (int k = 0; k < kMoves; k++) {
        if (cur && cur == (const void*) gMove[k].mot) {
            return k;
        }
    }
    // the clips: the walk's start and stop, the turns in place (the stand), the rest the run's
    static const int kClipMove[kClips] = {1, 1, 2, 2, 2, 2, 0, 0, 2, 2, 2, 2};
    for (int i = 0; i < kClips; i++) {
        if (cur && cur == (const void*) gClip[i].mot) {
            return kClipMove[i];
        }
    }
    return -1;
}

// After the routines: the big weapon's upper body (see heavy); 1 when posed
int heavyUpper(cPlayer* pl)
{
    const int k = heavy(pl);
    if (k >= 0) {
        gHeavyK = k;
    }
    gHeavyW = k >= 0 ? (gHeavyW + kHeavyFade > 1.0f ? 1.0f : gHeavyW + kHeavyFade)
                     : (gHeavyW - kHeavyFade < 0.0f ? 0.0f : gHeavyW - kHeavyFade);
    const Move& mv = gMove[gHeavyK];
    if (pistolStance() && gHeavyW > 0.0f && pl && pl->pList && pl->m_MotTbl) {
        // the handgun's classic stance: its arm layer over the remake's movement (the torso, hips and legs the remake's)
        const Move& pm = gPistolCarry[gHeavyK];
        if (pm.mot && pm.last == mv.last) {
            const f32 cat = (const void*) pl->Motion.pMot == (const void*) mv.mot ? (f32) pl->Motion.Seq.Frame / 64.0f
                                                                                    : -1.0f;
            static bool saidPistol;
            if (!saidPistol) {
                saidPistol = true;
                re4_log("move: the handgun in the classic stance (weapon %d, its %s)", (int) pG->weapon_no,
                        kNames[gHeavyK]);
            }
            return re4_legs_overlay(pl, pm.mot, (void*) pm.seq, 0, 1.0f, cat, gHeavyW, 0.0f, 0.0f, 2);
        }
        return 0;
    }
    if (gHeavyW <= 0.0f || !pl || !pl->pList || !pl->m_MotTbl || !mv.orig || light()) {
        gHeavyW = 0.0f;
        return 0;
    }
    // the game's motion at the remake's step: the remake's share of its cycle from its first right footstep, the game's
    // from its own; the stand and the clips: the game's runs on by itself
    f32 at = -1.0f;
    const u8* gseq = (const u8*) mv.origSeq;
    const unsigned gkeys = gseq ? get16(gseq) : 0;
    if (k >= 1 && (const void*) pl->Motion.pMot == (const void*) mv.mot && gkeys && mv.last) {
        f32 rt = 0.0f;
        f32 share = (f32) pl->Motion.Seq.Frame / 64.0f / (f32) mv.last;
        const f32 gs = gameStrike(mv.origSeq);
        if (gs >= 0.0f && rightStrike(mv, &rt)) {
            share += gs - rt / (f32) mv.last;
        }
        share -= floorf(share);
        at = share * (f32) gkeys;
    }
    // the shotgun: the remake's own carrying (its arm layer over the remake's movement, the same frames: in step)
    const Move& cm = gCarry[gHeavyK];
    if (pG->weapon_no == 7 && cm.mot && cm.last == mv.last) {
        const f32 cat = (const void*) pl->Motion.pMot == (const void*) mv.mot ? (f32) pl->Motion.Seq.Frame / 64.0f : -1.0f;
        static bool saidCarry;
        if (!saidCarry) {
            saidCarry = true;
            re4_log("move: the shotgun carried as in the remake (its %s)", kNames[gHeavyK]);
        }
        // the arms only (2): the torso, neck and head move with the remake's movement that plays (the sprint, the starts,
        // stops and turns too), the arms hold the gun as the remake's layer does
        return re4_legs_overlay(pl, cm.mot, (void*) cm.seq, 0, 1.0f, cat, gHeavyW, 0.0f, 0.0f, 2);
    }
    static bool said;
    if (!said) {
        said = true;
        re4_log("move: weapon %d: the remake's movements, the game's upper body (its %s)", (int) pG->weapon_no,
                kNames[gHeavyK]);
    }
    return re4_legs_overlay(pl, mv.orig, mv.origSeq, 0, 1.0f, at, gHeavyW, 0.0f, 0.0f, 1);
}

// The switch or the pack changed while Leon walks: the table follows (in the field's plain routines only)
void watch(unsigned)
{
    if (!pPL || !pG || pG->pl_type != 0 || gApplied < 0) {
        return;
    }
    load();
    const bool want = on() && fits() && gCount;
    if (gApplied == 1 && pPL->pMotionB) {  // one of the remake's motions plays: no blend with a game's variant
        for (const Move& mv : gMove) {
            if (mv.mot && (void*) pPL->Motion.pMot == (void*) mv.mot) {
                pPL->pMotionB->Brate = 0.0f;
            }
        }
        if (isClip((const void*) pPL->Motion.pMot)) {
            pPL->pMotionB->Brate = 0.0f;
        }
    }
    // the jog (L3) is a state (DevJunior, 18.11.2026: "pressed it is on, pressed again off, and it goes when you stop
    // completely"): it ends once he has stood (the footwork routine) for a few frames; aiming or reloading on the move
    // keep it
    if (pPL->r_no_0 != 0 || pPL->r_no_1 != 0) {
        gJogIdle = 0;
    } else if (++gJogIdle > kJogIdle) {
        gJogIdle = kJogIdle;
        gJogPresses = 0;
        if (gJog) {
            gJog = false;
            re4_log("move: L3: walk (he stopped)");
        }
    }
    if (gApplied == 1 && want) {
        matchPhase(pPL);
        transitions(pPL);
    } else {
        gPlaying = -1;
        gWasMoving = 0;
    }
    if ((want ? 1 : 0) != gApplied && pPL->r_no_0 == 0 && pPL->r_no_1 == 0) {
        apply(pPL, false);
        pPL->r_no_2 = 0;  // the footwork sets its motion anew
        pPL->r_no_3 = 0;
    }
}

}  // namespace

extern "C" {

void re4_move_set(int v)
{
    v = v ? 1 : 0;
    if (gOn.exchange(v) != v) {
        re4_log("move: the remake's movements %s (a mod pack's melee/move.dat)", v ? "on" : "off");
    }
}

// Patch 0923 (cPlayer::weaponInit): the weapon's module has filled the motion table
void re4_move_table(void* p)
{
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        re4_add_frame_hook(watch);
    }
    cPlayer* pl = (cPlayer*) p;
    if (!pG || pG->pl_type != 0) {
        return;
    }
    apply(pl, true);
}

// runtime/free_move.cpp (before it turns Leon toward the stick; `diff` the stick's direction against his heading,
// radians, positive to his left; `armed`: the stick held firmly back for a few frames, free_move.cpp): 1 = a turn
// about plays (he keeps his heading, the clip turns him)
// The app's "Remake movement" (runtime/ashley_ladder.cpp: Ashley's own way down ladders goes with it)
int re4_move_on(void) { return on() ? 1 : 0; }

int re4_move_turn(void* p, float diff, int armed)
{
    cPlayer* pl = (cPlayer*) p;
    if (!pl || gApplied != 1 || !on() || pl->r_no_0 != 0) {
        return 0;
    }
    const void* cur = (const void*) pl->Motion.pMot;
    if (gPlaying >= 0 && gPlaying != 8) {
        return gPlaying >= 4 && gPlaying < 8 && cur == (const void*) gClip[gPlaying].mot;
    }
    if (!armed || fabsf(diff) < kTurnStart) {
        return 0;
    }
    const bool left = diff > 0.0f;
    if (pl->r_no_1 == 3 && gMove[2].mot && gClip[left ? 4 : 5].mot &&
        (cur == (const void*) gMove[2].mot || (gClip[8].mot && cur == (const void*) gClip[8].mot))) {
        play(pl, left ? 4 : 5);
        return 1;
    }
    if (pl->r_no_1 == 0 && gClip[left ? 6 : 7].mot &&
        ((gMove[0].mot && cur == (const void*) gMove[0].mot) || cur == (const void*) gClip[1].mot ||
         cur == (const void*) gClip[3].mot)) {
        play(pl, left ? 6 : 7);
        return 1;
    }
    return 0;
}

// free_move.cpp re4_player_after_routine (after the routines, when "Move while aiming" walks no legs): the remake's
// legs under a big weapon; 1 when they are posed
// runtime/free_move.cpp (the legs while aiming, wish of DevJunior, 21.11.2026: "the movements to the sides"): for
// the move relative to the aim (sx right, sy forward, both -1..1) Leon's own side step of the remake (left, right,
// forward-left, back-right; forward-right and back-left mirrored when his model has the flip table, else the nearest
// one of the others): its motion, sequence, Mot_attr flip (0x40), the speed to play it at for `want` mm a frame, and
// the key to start at with the same foot forward as `cur` at `curFrame` (sequence frames). 0: forward or back (the
// caller's walk and back step), or no side steps.
int re4_move_strafe(void* p, float sx, float sy, float want, const void* cur, float curFrame, void** mot, void** seq,
                    int* flip, float* speed, int* start)
{
    static int last = -1;
    cPlayer* pl = (cPlayer*) p;
    if (!pl || !on() || !gClip[12].mot || !gClip[13].mot) {
        last = -1;
        return 0;
    }
    // 0 forward, 90 right, -90 left, 180 back; a sector of 45 degrees each, kept 10 degrees beyond its edge
    const f32 a = atan2f(sx, sy) * 57.29578f;
    static const f32 kMid[8] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, -135.0f, -90.0f, -45.0f};
    int sector = (int) floorf((a + 22.5f) / 45.0f);
    sector = ((sector % 8) + 8) % 8;
    if (last >= 0 && last != sector) {
        f32 d = a - kMid[last];
        d -= d > 180.0f ? 360.0f : d < -180.0f ? -360.0f : 0.0f;
        if (fabsf(d) < 32.5f) {
            sector = last;
        }
    }
    last = sector;
    // sector -> clip (-1: the caller's), mirrored
    static const int kClip[8] = {-1, 14, 13, 15, -1, 15, 12, 14};
    static const bool kFlip[8] = {false, true, false, false, false, true, false, false};
    int c = kClip[sector];
    bool mirror = kFlip[sector];
    if (c >= 0 && mirror && !pl->pXFlip) {
        c = sector == 1 ? 13 : 12;  // forward-right: right; back-left: left
        mirror = false;
    }
    if (c < 0 || !gClip[c].mot) {
        return 0;
    }
    Move& m = gClip[c];
    f32 dx = 0.0f, dz = 0.0f;
    const f32 per = travel(m.mot, &dx, &dz) && m.last ? sqrtf(dx * dx + dz * dz) / (f32) m.last : 0.0f;
    *speed = per > 1.0f ? want / per : 1.0f;
    *speed = *speed < 0.3f ? 0.3f : *speed > 1.6f ? 1.6f : *speed;
    *mot = m.mot;
    *seq = m.seq;
    *flip = mirror ? 0x40 : 0;
    const Move* from = 0;
    for (int i = 12; i < 16; i++) {
        if (gClip[i].mot && cur == (const void*) gClip[i].mot) {
            from = &gClip[i];
        }
    }
    const int k = from && from != &m ? phaseKey(*from, (u16) (curFrame * 64.0f), m) : -1;
    *start = k < 0 ? 0 : k;
    static int said;
    if (!(said & (1 << c))) {
        said |= 1 << c;
        re4_log("move: the remake's %s while aiming%s (%u frames, %.0f mm a frame of its own, played at %.2f)",
                kClipNames[c], mirror ? " (mirrored)" : "", (unsigned) m.last, (double) per, (double) *speed);
    }
    return 1;
}

// Patch 0944 (pl_shotgun.cpp wep07_r2_reload, the game thread): the motion of the shotgun's reload at `level`: the
// remake's (Leon's own, the game's length) for the shotgun (weapon_no 7) when move.dat has it and the remake's movements
// are on, else the game's `mot`. Only when the game's motion is as long as the one it was made for (another disc or
// another pack: the game's).
// Patch 0956 (pl_handgun.cpp wep02_r3_set00, the return from the reload, wep02_r3_fire00): the handgun's aim idle
// (PL_ARC 0x26..0x28) and fire (0x29..0x2B) of the remake for Leon, the set of the weapon (wep05/06 set 1, wep01..04
// and 38 set 0), when the game's motion is as long as the one made for it; else the game's
void* re4_move_hg_pose(int weapon, int index, void* mot)
{
    const int fire = index >= 0x29 ? 1 : 0, k = index - (fire ? 0x29 : 0x26);
    const int set = weapon == 5 || weapon == 6 ? 1 : (weapon >= 1 && weapon <= 4) || weapon == 0x38 ? 0 : -1;
    if (set < 0 || k < 0 || k > 2 || !mot || !on() || !gHgPose[set][fire][k]) {
        return mot;
    }
    const unsigned own = get16((const u8*) mot) & 0x3FFF;
    static int told;
    const int bit = 1 << (set * 6 + fire * 3 + k);
    if (own != gHgPoseLast[set][fire][k]) {
        if (!(told & (bit << 12))) {
            told |= bit << 12;
            re4_log("move: the handgun's %s 0x%x (weapon %d) is %u frames, the remake's %u: the game's",
                    fire ? "fire" : "aim", index, weapon, own, (unsigned) gHgPoseLast[set][fire][k]);
        }
        return mot;
    }
    if (!(told & bit)) {
        told |= bit;
        re4_log("move: the remake's handgun %s 0x%x (weapon %d, %u frames)", fire ? "fire" : "aim", index, weapon, own);
    }
    return gHgPose[set][fire][k];
}

void* re4_move_reload(int weapon, int level, void* mot)
{
    if (weapon != 7 && level >= 0 && level <= 2 && on() && mot) {
        // a handgun (patch 0955): the set made for a reload as long as the game's
        const unsigned own = get16((const u8*) mot) & 0x3FFF;
        static int told;
        for (int k = 0; k < 2; k++) {
            if (gHgReload[k][level] && gHgReloadLast[k][level] == own) {
                if (!(told & (1 << (k * 3 + level)))) {
                    told |= 1 << (k * 3 + level);
                    re4_log("move: the remake's handgun reload (weapon %d, level %d, %u frames)", weapon, level, own);
                }
                return gHgReload[k][level];
            }
        }
        if (!(told & (0x100 << level))) {
            told |= 0x100 << level;
            re4_log("move: the handgun reload (weapon %d, level %d) is %u frames: no remake's reload that long, the "
                    "game's", weapon, level, own);
        }
        return mot;
    }
    if (weapon != 7 || level < 0 || level > 2 || !gReload[level] || !on() || !mot) {
        return mot;
    }
    const unsigned own = get16((const u8*) mot) & 0x3FFF;
    static int said;
    if (own != gReloadLast[level]) {
        if (!(said & (8 << level))) {
            said |= 8 << level;
            re4_log("move: the shotgun's reload at level %d is %u frames on this disc, the remake's was made for %u: "
                    "the game's", level, own, (unsigned) gReloadLast[level]);
        }
        return mot;
    }
    if (!(said & (1 << level))) {
        said |= 1 << level;
        re4_log("move: the remake's shotgun reload (level %d, %u frames)", level, own);
    }
    return gReload[level];
}

// Patch 0950 (pl_shotgun.cpp wep07_r3_set00 / wep07_r3_fire00, the game thread): the shotgun's aim pose of PL_ARC
// `index` (0x1A, 0x20, 0x22: the three of mot3): the remake's (Leon's hold loop, the gun on the game's line) for the
// shotgun (weapon_no 7) when move.dat has it and the remake's movements are on, else the game's `mot`; only when the
// game's motion is as long as the one it was made for.
void* re4_move_aim(int weapon, int index, void* mot)
{
    const int i = index == 0x1A ? 0 : index == 0x20 ? 1 : index == 0x22 ? 2 : -1;
    if (weapon != 7 || i < 0 || !gAim[i] || !on() || !mot) {
        return mot;
    }
    const unsigned own = get16((const u8*) mot) & 0x3FFF;
    static int said;
    if (own != gAimLast[i]) {
        if (!(said & (8 << i))) {
            said |= 8 << i;
            re4_log("move: the shotgun's aim 0x%x is %u frames on this disc, the remake's was made for %u: the game's",
                    index, own, (unsigned) gAimLast[i]);
        }
        return mot;
    }
    if (!(said & (1 << i))) {
        said |= 1 << i;
        re4_log("move: the remake's shotgun aim 0x%x (%u frames)", index, own);
    }
    return gAim[i];
}

// Patch 0945 (sce_at.cpp sceAtQuickItem, the game thread): an item taken on the move (the remake's quick pick-up):
// Leon's hand reaches for it (on the move the running grab; standing the low one, or the one at a table's height
// when the item lies 60 cm or more above his feet), on his upper body over what he does (re4_move_heavy_legs)
void re4_move_pickup(void* item)
{
    cPlayer* pl = pPL;
    if (!pl || !on() || !gPick[0].mot || pl->r_no_0 != 0 || pl->r_no_1 > 4) {
        return;
    }
    const cModel* m = (const cModel*) item;
    const bool moving = pl->r_no_1 == 1 || pl->r_no_1 == 3;
    const f32 dy = m ? m->pos.y - pl->pos.y : 0.0f;
    const int i = moving ? 0 : (dy >= 600.0f && gPick[2].mot) ? 2 : gPick[1].mot ? 1 : 0;
    gPickNow = i;
    gPickFrame = 0.0f;
    static int said;
    if (!(said & (1 << i))) {
        said |= 1 << i;
        re4_log("move: the remake's taking of an item (%s, %u frames)",
                i == 0 ? "on the move" : i == 1 ? "low, standing" : "at a table's height", (unsigned) gPick[i].last);
    }
}

int re4_move_heavy_legs(void* p)
{
    cPlayer* pl = (cPlayer*) p;
    if (gPickNow >= 0) {
        Move& m = gPick[gPickNow];
        if (!pl || !m.mot || pl->r_no_0 != 0 || pl->r_no_1 > 4 || gPickFrame >= (f32) m.last) {
            gPickNow = -1;  // over, or another action took him
        } else {
            const f32 in = gPickFrame / kPickFade, out = ((f32) m.last - gPickFrame) / (kPickFade * kPickSpeed);
            f32 w = in < out ? in : out;
            w = w > 1.0f ? 1.0f : w < 0.15f ? 0.15f : w;
            const int ok = re4_legs_overlay(pl, m.mot, m.seq, 0, 0.0f, gPickFrame, w, 0.0f, 0.0f, 1);
            gPickFrame += kPickSpeed;
            if (ok) {
                return 1;
            }
            gPickNow = -1;
        }
    }
    return heavyUpper(pl);
}

// For the tests: the movements the active archive holds
int re4_move_count(void)
{
    load();
    return gCount;
}

// aim_options.cpp re4_run_held (the game's run checks, patch 0156): -1 without the sprint (the game's / "Always
// run"'s way), else whether Leon runs: the run key (the sprint) or the jog L3 started
int re4_move_sprint(int held)
{
    gSprintKey = held != 0;
    if (gApplied != 1 || !on() || !gClip[8].mot || !re4_free_move_on()) {
        gJog = false;
        gJogPresses = 0;
        return -1;
    }
    if (gJogPresses.exchange(0) & 1) {
        gJog = !gJog;
        gJogIdle = 0;
        re4_log("move: L3: %s", gJog ? "jog" : "walk");
    }
    return held || gJog ? 1 : 0;
}

// The app (L3, the third aim word's toggling bit 16 through re4_aim_set; any thread): jog / walk
void re4_move_jog_press(void)
{
    gJogPresses.fetch_add(1);
}

// For the tests: the transitions' clips the active archive holds
int re4_move_clips(void)
{
    load();
    int n = 0;
    for (const Move& c : gClip) {
        n += c.mot ? 1 : 0;
    }
    return n;
}

}  // extern "C"
