// Leon's movements of the 2023 remake (wish of DevJunior, 13.11.2026: "remaster Leon's movements"; phase 1: stand, walk,
// run, walk back). Compiled with the game's flags into libre4_main (re4_gamehooks_obj). The app's "Remake movement"
// (the third aim word's bit 14, on by default) switches it; it needs a mod pack's melee/move.dat
// (port/tools/re9_finisher_pack.py --move: one seamless cycle of each movement from the PC mod's cha0_general).
//
// Patch 0923 (pl_wep.cpp cPlayer::weaponInit, after the weapon's module filled the motion table m_MotTbl) asks
// re4_move_table: with bare hands (weapon 0, the quick slots' "weapon put away") or a handgun (em_sub.cpp HandgunCk) the
// table's stand (0 / 1), walk (2 / 3), run (6 / 7) and back step (8 / 9) take the remake's motion and a sequence made
// here. The remake's motions are unarmed ones: with a rifle or shotgun in both hands the game's own stay, and so do the
// hurt motions (life under two thirds: m_MotTbl 0x5F..), the turns in place and everything else.
// The game's speed stays: per movement the game's own motion tells how far Leon goes a frame (its root joint, kind 1,
// over its sequence's frames); the remake's cycle is played faster or slower by the square root of the ratio (its
// sequence maps frames in 10.6 fixed point; at most kTimeMin..kTimeMax) and its root motion scaled by the rest, so the
// feet slide as little as the game's pace allows. The footstep sounds are the game's own two of that movement, on the
// frames the remake's feet strike.
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
extern cPlayer* pPL;

namespace {

const int kMoves = 4;
const int kSlots[kMoves] = {0, 2, 6, 8};  // m_MotTbl: stand, walk, run, back step (the sequence in the next slot)
const char* const kNames[kMoves] = {"stand", "walk", "run", "back step"};
// The head's turn toward an enemy (cPlNeck) blends the body with the game's own "looking right / left" variant of the
// stand, the walk and the run (m_MotTbl 0x3F / 0x39 / 0x41 and the next slot): two different walks mixed, out of step
// (device report 13.11.2026: the walk hitched and repeated). With the remake's motion in a slot its variants are
// taken out (cPlNeck::init leaves invalid ones alone) and the blend's rate held at zero.
const int kNeck[kMoves] = {0x3F, 0x39, 0x41, 0};
const f32 kTimeMin = 0.75f, kTimeMax = 1.35f;
const unsigned kMaxBytes = 0x40000;
const int kSeqMax = 255;

struct Move {
    u8* mot;        // the remake's motion (0 none)
    u8 flags;       // 1: matched to the game's speed
    u16 last;       // its last frame (the first pose again)
    u16 foot[2];    // the frames the right and the left foot strike (0xFFFF none)
    f32 scale;      // the scale its root motion has now
    void* orig;     // the game's motion and sequence it stands in for now
    void* origSeq;
    void* neck[2];  // the game's looking variants taken out
    bool neckOut;
    u16 seq[2 + 2 * (kSeqMax + 1)];
};

std::atomic<int> gOn(-1);  // the app's switch; -1 none sent yet (the PC: RE4_REMAKE_MOVE, else on)
char gLoaded[1600];
u8* gPlace;
Move gMove[kMoves];
int gCount;
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
    ok = ok && n >= 1 && n <= 32 && 8 + n * 12 <= at[1] - at[0];
    Move found[kMoves];
    memset(found, 0, sizeof(found));
    for (u32 i = 0; ok && i < n; i++) {
        const u8* r = tab + 8 + i * 12;
        const u16 entry = be16At(r + 2), last = be16At(r + 4);
        if (entry == 0 || entry >= count || last < 2 || be16At(base + at[entry]) != last || !base[at[entry] + 2]) {
            ok = false;
            break;
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
        re4_log("move: %s is no archive of Leon's movements (%ld bytes): the game's", path, size);
        return;
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
        gMove[k].scale = 1.0f;
        gMove[k].orig = gMove[k].origSeq = 0;
        if (gMove[k].mot) {
            gCount++;
            re4_log("move: %s: the remake's %s, %u frames", path, kNames[k], (unsigned) gMove[k].last);
        }
    }
    gApplied = -1;
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
    for (int foot = 0; foot < 2; foot++) {
        if (mv.foot[foot] != 0xFFFF && k != 0) {
            int at = (int) ((f32) mv.foot[foot] * (f32) keys / frames + 0.5f);
            key[at >= keys ? keys - 1 : at].Se = se[foot];
        }
    }
    re4_log("move: the remake's %s for weapon %d: the game's pace %.0f a frame, the cycle's %.0f: played at %.2f (%d "
            "frames), its steps scaled %.2f",
            kNames[k], (int) pG->weapon_no, (double) pace, (double) own, (double) time, keys, (double) scale);
}

bool fits()
{
    return pG && pG->pl_type == 0 && (pG->weapon_no == 0 || HandgunCk(pG->weapon_no));
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
        const bool ours = tbl[slot] == mv.mot && mv.mot;
        if (fresh || !ours) {
            if (!ours) {
                mv.orig = tbl[slot];
                mv.origSeq = tbl[slot + 1];
            }
        }
        if (want && mv.mot && mv.orig) {
            fit(k, mv.orig, mv.origSeq);
            tbl[slot] = mv.mot;
            tbl[slot + 1] = mv.seq;
            if (kNeck[k] && (fresh || !mv.neckOut)) {
                mv.neck[0] = tbl[kNeck[k]];
                mv.neck[1] = tbl[kNeck[k] + 1];
                mv.neckOut = true;
            }
            if (kNeck[k]) {
                tbl[kNeck[k]] = tbl[kNeck[k] + 1] = 0;
            }
        } else if (ours) {
            tbl[slot] = mv.orig;
            tbl[slot + 1] = mv.origSeq;
            if (kNeck[k] && mv.neckOut) {
                tbl[kNeck[k]] = mv.neck[0];
                tbl[kNeck[k] + 1] = mv.neck[1];
            }
            mv.neckOut = false;
        } else if (fresh) {
            mv.neckOut = false;
        }
    }
    gApplied = want ? 1 : 0;
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

// For the tests: the movements the active archive holds
int re4_move_count(void)
{
    load();
    return gCount;
}

}  // extern "C"
