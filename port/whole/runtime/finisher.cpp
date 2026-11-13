// Leon's finishers (wish of DevJunior, 13.11.2026: the PC mod "RE9 Melee - Hatchet - Finishers for Leon", "turn this
// into the remake, its controls too"). Compiled with the game's flags into libre4_main (re4_gamehooks_obj); the app's
// "Finishers" (the third aim word's bit 13, runtime/aim_options.cpp) switches them, on by default; they need a mod
// pack's melee/finish.dat (port/tools/re9_finisher_pack.py, from the PC mod's motions), read once to the place for the
// mods' character archives (runtime/mod_place.cpp) and converted as a read archive is (under the name
// mods/finish.drs).
//
// The controls are the game's own, as in the 2023 remake: the action button on the prompt over a stunned Ganado.
//  - On a kneeling one (em10KneeDownAction, the kick's prompt): the hatchet finisher (h4): Leon draws his knife (a
//    skin's axe), strikes the neck and finishes him on the floor.
//  - On a standing stunned one (em10KickAction): a finisher with the weapon in Leon's hands (handguns c2, shotguns
//    and machine guns c3c, rifles c4b2, magnums c5b) when its magazine holds a round: he grabs the Ganado and fires
//    once (the weapon object's own shot: muzzle flash, sound, a round spent).
// runtime/melee.cpp draws them among the other moves (its random mix); HUNK's rule holds as for the neck break: a
// Ganado with a parasite or a core gets the game's kick.
//
// Both play as the game's own neck break does (em10.cpp Dm_NeckBreak, R1 0x15: the enemy dies, its motion plays, at
// its end the body lies dead): patch 0921 lets re4_finish_catch put Leon at the PC pair's distance in front of the
// Ganado (EmCatchPLSet: type 1 Leon steps there, the kneeling enemy stays; type 2 both close half the gap) and give
// him plFinish; re4_melee_motion (patch 0904) hands Dm_NeckBreak the enemy's motion and sequence (Free 1 on the kill:
// the game's crack and the weapon falling; 0x80 once the body is down: no foot IK). Both motions carry the PC pair's
// root motion (joint kinds 1 / 0x40, played with MotionSetCore's flags bit 0), so they move on the floor together.
#include "types.h"
#include "global.h"
#include "main.h"
#include "dvd.h"
#include "re4_runtime.h"
#include "player.h"
#include "pl_body.h"
#include "pl_sub.h"
#include "pl_wep.h"
#include "motion.h"
#include "math_sub_decl.h"
#include "snd.h"
#include "esp.h"
#include "rnd.h"
#include "game.h"
#include "em.h"
#include "em_sub.h"
#include "em10.h"
#include "joy.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" const char* re4_mod_finish_path(void);            // runtime/dvd.cpp
extern cPlayer* pPL;

namespace {

const unsigned kMaxBytes = 0x200000;  // the archive at most (11 finishers: 0.7 MB)
const int kMax = 32;                  // finishers at most
const int kCatchFrames = 8;           // frames in which Leon closes the catch's gap (EmCatchMotionMove)
const f32 kCatchRate = 0.35f;         // the part of what is left he closes in each of them
const int kHokan = 4;                 // frames Leon's motion blends in

struct Finish {
    u8 kind;        // 0 a standing stunned enemy, 1 a kneeling one
    u8 flags;       // 1 the knife out, 2 the gun fires on the shot frame
    u8 weapons[6];  // the game's weapon numbers (pG->weapon_no), 0-terminated; none: any
    void* leon;     // Leon's motion
    void* enemy;    // the enemy's motion
    void* seq;      // the enemy's sequence
    u16 leonEnd, enemyEnd, shot, kill, down, cancel;
    f32 x, z, ang, heading;
    char name[17];
};

std::atomic<int> gOn(-1);  // the app's switch; -1 none sent yet (the PC: RE4_FINISHERS, else on)
char gLoaded[1600];        // the archive read ("" none; a path that failed counts as read)
u8* gPlace;                // where it lies (the mods' place)
unsigned gPlaceSize;
Finish gFin[kMax];
int gCount;
int gActive = -1;          // the finisher the last prompt drew, until Leon's routine ends
int gLastDrawn[2] = {-1, -1};
bool gLive;                // Leon's routine runs: the knife out / the gun hidden to restore
void* gKick;               // the game's kick on a kneeling enemy (plFinishStart's way back)
u32 gSeed = 0x9E3779B9u;
int gSaid;

u32 be32At(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

u16 be16At(const u8* p)
{
    return (u16) (p[0] << 8 | p[1]);
}

f32 beF32At(const u8* p)
{
    const u32 u = be32At(p);
    f32 v;
    memcpy(&v, &u, 4);
    return v;
}

void say(int bit, const char* what)
{
    if (!(gSaid & (1 << bit))) {
        gSaid |= 1 << bit;
        re4_log("finisher: %s", what);
    }
}

bool on()
{
    int v = gOn.load();
    if (v < 0) {
        const char* env = getenv("RE4_FINISHERS");
        v = env ? atoi(env) != 0 : 1;
    }
    return v != 0;
}

// The place given back (no pack holds finishers any more)
void release()
{
    if (gPlace) {
        re4_mod_em_release(gPlace);
        gPlace = 0;
        gPlaceSize = 0;
    }
}

// The active mods' finishers, read anew when another pack holds them
void load()
{
    const char* path = re4_mod_finish_path();
    if (strcmp(path, gLoaded) == 0) {
        return;
    }
    snprintf(gLoaded, sizeof(gLoaded), "%s", path);
    gCount = 0;
    gActive = -1;
    if (!*path) {
        release();
        return;
    }
    FILE* f = fopen(path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    if (!f || size < 0x40 || size > (long) kMaxBytes) {
        re4_log("finisher: %s: %s (at most %u bytes): no finishers", path, f ? "no archive of its size" : "unreadable",
                kMaxBytes);
        if (f) {
            fclose(f);
        }
        return;
    }
    if (!gPlace || gPlaceSize < (unsigned) size) {
        release();
        gPlace = (u8*) re4_mod_place_alloc((unsigned long) size, "the finishers' motions (melee/finish.dat)");
        gPlaceSize = gPlace ? (unsigned) size : 0;
    }
    if (!gPlace) {
        fclose(f);
        return;
    }
    u8* const base = gPlace;
    const bool whole = fread(base, 1, (size_t) size, f) == (size_t) size;
    fclose(f);
    const u32 count = whole ? be32At(base) : 0;
    bool ok = count >= 2 && count <= 0x400 && 0x10 + count * 8 <= (u32) size && !be32At(base + 4);
    static u32 at[0x401];
    for (u32 i = 0; ok && i < count; i++) {
        at[i] = be32At(base + 0x10 + i * 4);
        ok = at[i] >= 0x10 + count * 8 && at[i] < (u32) size && !(at[i] & 3) && (i == 0 || at[i] >= at[i - 1]);
    }
    at[count] = (u32) size;
    const u8* fin = ok ? base + at[0] : 0;
    const u32 n = ok && at[1] - at[0] >= 8 && memcmp(fin, "FIN1", 4) == 0 ? be32At(fin + 4) : 0;
    ok = ok && n >= 1 && n <= (u32) kMax && 8 + n * 60 <= at[1] - at[0];
    // the table first: the conversion swaps the archive's head
    Finish table[kMax];
    for (u32 i = 0; ok && i < n; i++) {
        const u8* r = fin + 8 + i * 60;
        Finish& t = table[i];
        memset(&t, 0, sizeof(t));
        t.kind = r[0];
        t.flags = r[1];
        memcpy(t.weapons, r + 2, 6);
        const u16 li = be16At(r + 8), ei = be16At(r + 10), si = be16At(r + 12);
        if (li == 0 || ei == 0 || si == 0 || li >= count || ei >= count || si >= count || t.kind > 1) {
            ok = false;
            break;
        }
        t.leon = base + at[li];
        t.enemy = base + at[ei];
        t.seq = base + at[si];
        t.leonEnd = be16At(r + 16);
        t.enemyEnd = be16At(r + 18);
        t.shot = be16At(r + 20);
        t.kill = be16At(r + 22);
        t.down = be16At(r + 24);
        t.cancel = be16At(r + 26);
        t.x = beF32At(r + 28);
        t.z = beF32At(r + 32);
        t.ang = beF32At(r + 36);
        t.heading = beF32At(r + 40);
        memcpy(t.name, r + 44, 16);
        t.name[16] = 0;
        // a motion's head: its last frame (big-endian here) and its joints
        ok = be16At(base + at[li]) >= t.leonEnd && base[at[li] + 2] && be16At(base + at[ei]) >= t.enemyEnd &&
             base[at[ei] + 2] && be16At(base + at[si]) == t.enemyEnd + 1u && t.kill <= t.enemyEnd &&
             t.cancel <= t.leonEnd && t.x == t.x && t.z == t.z && fabsf(t.x) < 5000.0f && fabsf(t.z) < 5000.0f;
    }
    if (!ok) {
        re4_log("finisher: %s is no finishers' archive (%ld bytes, %u entries): none", path, size, (unsigned) count);
        return;
    }
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = (u32) size;
    h.dest = (u32) (unsigned long) base;
    re4_formats_part_loaded("mods/finish.drs", 0, 0, &h, (unsigned) (unsigned long) base);
    memcpy(gFin, table, sizeof(Finish) * n);
    gCount = (int) n;
    for (int i = 0; i < gCount; i++) {
        re4_log("finisher: %s: %s on a %s enemy, %u / %u frames (kill %u%s), the enemy %.0f, %.0f mm in front", path,
                gFin[i].name, gFin[i].kind ? "kneeling" : "standing", (unsigned) gFin[i].leonEnd,
                (unsigned) gFin[i].enemyEnd, (unsigned) gFin[i].kill, gFin[i].flags & 2 ? ", a shot" : "",
                (double) gFin[i].x, (double) gFin[i].z);
    }
    re4_log("finisher: %s read to %p: %d finishers", path, (void*) base, gCount);
}

bool leon()
{
    return pG && pG->pl_type == 0 && pPL && pPL->Body;
}

bool weaponFits(const Finish& t)
{
    if (!t.weapons[0]) {
        return true;
    }
    for (int i = 0; i < 6 && t.weapons[i]; i++) {
        if (t.weapons[i] == pG->weapon_no) {
            return true;
        }
    }
    return false;
}

// The finishers of `kind` that fit now (Leon's weapon, a round in its magazine for a shot)
int candidates(int kind, int* out)
{
    int n = 0;
    if (!on() || !leon()) {
        return 0;
    }
    load();
    for (int i = 0; i < gCount; i++) {
        const Finish& t = gFin[i];
        if (t.kind != kind || !weaponFits(t)) {
            continue;
        }
        if ((t.flags & 2) && !(pPL->Wep && WEP_OBJ(pPL) && WEP_OBJ(pPL)->bulletNum())) {
            continue;
        }
        out[n++] = i;
    }
    return n;
}

void bloodAt(cEm* em, Vec* from)
{
    if (!em) {
        return;
    }
    Vec pos = em->getPartsPtr(4)->world;
    Vec dir;
    PSVECSubtract(&pos, from, &dir);
    const f32 len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1.0f) {
        dir.x /= len, dir.y /= len, dir.z /= len;
    }
    EstSet(0, -1, &pos, &dir, EFF_CORE, 1, 0, ESP_CORE_KIND_NONE, 0, 0);
    for (int i = 0; i < 2; i++) {
        Vec p = {pos.x + fRand1_1() * 150.0f, pos.y + fRand1_1() * 150.0f, pos.z + fRand1_1() * 150.0f};
        EstSet(0, -1, &p, &dir, EFF_CORE, 2, 0, ESP_CORE_KIND_NONE, 0, 0);
    }
}

void restore(cPlayer* pl)
{
    if (gLive) {
        gLive = false;
        if (gActive >= 0 && (gFin[gActive].flags & 1)) {
            pl->Body->setKnife(false);
        }
        pl->Wep->setTrans(1, 0);
    }
    gActive = -1;
}

void finish(cPlayer* pl)
{
    restore(pl);
    EndPlDamage();
    pl->dmg.set(0, 0xF);
}

// Leon's routine (set by re4_finish_catch through EmCatchPLSet): r_no_2 0 start, 1 the pair plays
void plFinish(cPlayer* pl)
{
    cEm* em = pl->pEmCatch;
    pl->dmg.set(0, 0x1E);
    StaFlagOn(pG, STA_PL_EM_ACTION);
    if (gActive < 0) {  // cut off from its archive (another pack): ends at once
        finish(pl);
        return;
    }
    const Finish& t = gFin[gActive];
    switch (pl->r_no_2) {
    case 0:
        MotionSetCore(pl, &pl->Motion, t.leon, 0, kHokan, 1, 0);
        GameAddPoint(LVADD_CRITICALHIT);
        if (t.flags & 1) {
            pl->Wep->setTrans(0, 0);
            pl->Body->setKnife(true);
        }
        gLive = true;
        pl->m_Work0 = kCatchFrames;
        pl->m_Work1 = 0;  // bit 0 the shot, bit 1 the kill
        pl->r_no_2 = 1;
        re4_log("finisher: %s plays (%u frames)", t.name, (unsigned) t.leonEnd);
    case 1: {
        int end;
        if (pl->m_Work0) {
            pl->m_Work0--;
            end = EmCatchMotionMove(pl, kCatchRate, kCatchRate);
        } else {
            end = MotionMove(pl, 0);
        }
        const int frame = (int) pl->Motion.Seq_frame;
        if ((t.flags & 2) && !(pl->m_Work1 & 1) && frame >= t.shot) {
            pl->m_Work1 |= 1;
            if (pl->Wep && WEP_OBJ(pl) && WEP_OBJ(pl)->bulletNum()) {
                cObjWep* obj = WEP_OBJ(pl);
                obj->trigger();
                obj->r_no_0 = 2;
                obj->r_no_1 = 0;
                pl->m_Work4 = 1;
                pl->m_Work5 = 1;
            }
            Vec hand = pl->getPartsPtr(10)->world;
            bloodAt(em, &hand);
        }
        if (!(pl->m_Work1 & 2) && frame >= t.kill) {
            pl->m_Work1 |= 2;
            if (!(t.flags & 2)) {
                Vec hand = pl->getPartsPtr(10)->world;
                bloodAt(em, &hand);
                SndCall(1, 0xF, &pl->pos, 0, 0, pPL);
            }
        }
        if (end || frame >= t.leonEnd) {
            finish(pl);
            break;
        }
        if (frame >= t.cancel && (joyKamae() || (Key.on & 0x10F))) {
            finish(pl);
            break;
        }
        break;
    }
    }
    pl->Catch_at_adj = pl->pos;
}

// Leon's routine of the kneeling enemy's prompt (em10KneeDownAction): the enemy goes into its finisher (Dm_NeckBreak),
// whose catch gives Leon plFinish; a Ganado with a parasite or a core gets the game's kick
void plFinishStart(cPlayer* pl)
{
    cEm* em = pl->pEmCatch;
    if (gActive < 0 || !em) {
        restore(pl);
        Pl_func_tbl[4] = (void (*)(cPlayer*)) gKick;
        Pl_func_tbl[4](pl);
        return;
    }
    FREE_EM10* w = EM10_WK(em);
    if (pl->r_no_2 == 0) {
        if (w->pCore || w->pParasite || (w->Be_flg & 0x80)) {
            say(3, "a Ganado with a parasite or a core: the game's kick (HUNK's rule)");
            gActive = -1;
            Pl_func_tbl[4] = (void (*)(cPlayer*)) gKick;
            Pl_func_tbl[4](pl);
            return;
        }
        em->setRno(2, 0x15, 0, 0);
        em->dmg.set(0, 0x1E);
        pl->r_no_2 = 1;
        pl->m_Work0 = 10;
    }
    MotionMove(pl, 0);
    if (pl->m_Work0 && --pl->m_Work0 == 0) {  // the enemy never took it (another routine held it): the kick
        re4_log("finisher: the kneeling enemy did not take the finisher: the game's kick");
        gActive = -1;
        Pl_func_tbl[4] = (void (*)(cPlayer*)) gKick;
        pl->r_no_2 = 0;
        pl->r_no_3 = 1;  // the kick on a kneeling enemy (em10KneeDownAction)
    }
}

// A grab or a hit that cut the finisher short: the knife away and the gun back
void watch(unsigned)
{
    if (gLive && pPL &&
        (pPL->r_no_0 != 4 || (Pl_func_tbl[4] != plFinish && Pl_func_tbl[4] != plFinishStart))) {
        re4_log("finisher: cut short (routine %d): knife away, gun shown", (int) pPL->r_no_0);
        restore(pPL);
    }
}

void hook()
{
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        re4_add_frame_hook(watch);
    }
}

}  // namespace

extern "C" {

void re4_finish_set(int v)
{
    v = v ? 1 : 0;
    if (gOn.exchange(v) != v) {
        re4_log("finisher: %s (a mod pack's melee/finish.dat on stunned Ganados)", v ? "on" : "off");
    }
}

// runtime/melee.cpp: 1 when a finisher of `kind` (0 standing, 1 kneeling) may be drawn now
int re4_finish_ready(int kind)
{
    int c[kMax];
    return candidates(kind, c) > 0;
}

// runtime/melee.cpp drew a finisher of `kind`: one of those that fit (not the last one when there are others)
int re4_finish_arm(int kind)
{
    int c[kMax];
    const int n = candidates(kind, c);
    if (!n) {
        gActive = -1;
        return 0;
    }
    int pick = c[0];
    for (int tries = 0; n > 1 && tries < 8; tries++) {
        gSeed = gSeed * 1664525u + 1013904223u;
        pick = c[(gSeed >> 16) % (u32) n];
        if (pick != gLastDrawn[kind]) {
            break;
        }
    }
    gLastDrawn[kind] = pick;
    gActive = pick;
    hook();
    re4_log("finisher: %s drawn for a %s enemy (%d fit, weapon %d)", gFin[pick].name, kind ? "kneeling" : "standing",
            n, (int) pG->weapon_no);
    return 1;
}

// The kneeling enemy's prompt: Leon's routine that hands the enemy its finisher (`kick`: the game's, the way back)
void* re4_finish_kneel_routine(void* kick)
{
    gKick = kick;
    hook();
    return (void*) plFinishStart;
}

// Patch 0921 (em10.cpp Dm_NeckBreak): 1 when a finisher is armed: Leon's place and routine set (EmCatchPLSet) and the
// enemy turned as in the PC pair; 0 leaves the neck break's own catch
int re4_finish_catch(void* p)
{
    cEm* em = (cEm*) p;
    if (gActive < 0 || !leon() || !em) {
        return 0;
    }
    const Finish& t = gFin[gActive];
    EmCatchPLSet(em, t.ang, t.kind ? 1 : 2, t.x, 0.0f, t.z, plFinish);
    // EmCatchPLSet turns the enemy to face Leon; the pair's own heading differs by this much
    em->Catch_dir = LIMIT_ANGLE(em->Catch_dir + t.heading - PI + t.ang);
    return 1;
}

// re4_melee_motion (patch 0904): the enemy's motion (3) or sequence (4) of the armed finisher, 0 none
void* re4_finish_motion(int kind)
{
    if (gActive < 0 || !leon()) {
        return 0;
    }
    return kind == 3 ? gFin[gActive].enemy : kind == 4 ? gFin[gActive].seq : kind == 2 ? gFin[gActive].leon : 0;
}

int re4_finish_armed(void)
{
    return gActive;
}

// For the tests: the finishers read from `path` ("" none): their count, and each one's kind << 8 | flags
int re4_finish_probe(int i)
{
    load();
    if (i < 0) {
        return gCount;
    }
    return i < gCount ? gFin[i].kind << 8 | gFin[i].flags : -1;
}

// A new prompt's draw: nothing armed unless it draws a finisher (while none plays)
void re4_finish_clear(void)
{
    if (!gLive) {
        gActive = -1;
    }
}

// The runtime test's: the next draws start over
void re4_finish_reset(void)
{
    release();
    gLoaded[0] = 0;
    gCount = 0;
    gActive = -1;
    gLive = false;
    gLastDrawn[0] = gLastDrawn[1] = -1;
}

}  // extern "C"
