// Ashley's pistol (wish of DevJunior, 11.11.2026: "Ashley self-defence: she can use a pistol as the partner ... she
// follows us and backs us up"; his choices: a switch in the Mods page, the arm aimed by code, she shoots while she
// follows and while she waits, ammunition that comes back with time). Compiled with the game's flags into
// libre4_main (re4_gamehooks_obj); the switch arrives with the second aim word (bit 29, runtime/aim_options.cpp).
//
//  - Stage 3, patch 0913 (pl_ashley.cpp cPlAshley::moveMatCalcBefore; patch 0900's cPlayer::keyConfig hook): in
//    Ashley's own chapter (pl_type 1) the player shoots with the pistol she holds (DevJunior, 11.11.2026: "also when
//    playing as her"). The aim key held (R, Key 0x10; the game gives her none) in her walk routines: she stands
//    (the stick and the run key taken from the game), the stick turns her (with "Free movement" she faces the
//    camera, its C stick turns her), her arm rises over kAimIn frames to the nearest enemy she sees within
//    kLockCone of where she faces, else straight ahead; A (Key 0x80, her action key, taken from the game meanwhile)
//    fires, kFireGap frames apart: the same shot, the same rounds that come back with time; none left, the case's
//    "no" sound.
//
//  - Stage 2, patch 0912 (Sscrn/ss_pzzl.cpp): with the switch on and Ashley the partner, "Equip" on a handgun or a
//    magnum (the D-pad quick slots' up list) opens the case's own "use on" choice, Leon or Ashley (23..25.11.2026
//    "Equip" twice instead, while the window showed an empty dark box on the debug disc). Ashley: she
//    takes that pistol out of the case (DevJunior, 11.11.2026: "that it takes no room while she has it"): the
//    item's inventory type becomes kAway, a set no character's case shows (cItem::type, cItemMgr::m_char 0 Leon,
//    1 Ashley's chapter), so it goes with the save, its tune levels and its place kept; one pistol at a time (a
//    pistol she had goes back to Leon); Leon put to bare hands if it was his. Leon: plain "Equip". She is armed
//    while the switch is on and she holds a pistol.
//  - Asking for it back: the partner command button (Key 0x200, cPlLeon::checkXbutton) held kAskHold frames, or
//    the switch turned off: the pistol goes back to Leon's case at its old place (pzlPlayer::init with patch 0100
//    finds a free place, else the spare board, when the case opens), the "wait / follow" the press gave her
//    undone, the case's confirm sound.
//
//  - Patch 0911 (pl_npc.cpp cSubChar::move, before the parts' world matrices): re4_ashley_gun_move. While she is
//    the partner in play (the game running, no event, sub screen or movie, Leon as the player; her routine standing,
//    following or behind Leon) the nearest enemy within kRange that she sees (no scenario polygon on the line from
//    her chest to its lock-on point; not one locked off, nothing of the partner kind, no animal: id 0x10..0x3F with
//    more than 20 hit points) is her target; standing, she turns to it, else only one within kCone of where she
//    faces. Her right arm then points at it: the arm is found by her bind pose (cParts::lt_inv_mat: identity with
//    the bind position), the part farthest out on Leon's right-hand side (his hand, part 10, gives the side) and its
//    chain up to the shoulder; the upper arm turns so that the straight arm (the elbow and the hand in their bind
//    rotation) points at the target, blended in over kAimIn frames. Aimed, she fires every kFireGap frames:
//    PlWepHitCheck2 from the muzzle to the target's lock-on point, the weapon number Luis uses (3), her own hit
//    points set to 0 meanwhile as he does (pl14.cpp cRoutine::shot), and the weapon block's first sound.
//  - The hold and the damage (device report of DevJunior, 16.11.2026: "fix Ashley's animations with the gun; the hand
//    cannon does the same damage as the handgun"): she holds the pistol in both hands (aimArm: a two-bone reach of
//    each arm to a grip in front of her chest at her eyes' height, the elbows bent down, the hands along the aim with
//    the thumbs up, the left one cupping the right); her shot carries the held pistol's own weapon number (its damage
//    table, its piercing) and, through patch 0926 (GetWepDmVal), the pistol's own firepower level for the enemies it
//    hit; she fires at the pistol's own rate (PlShotFrameTbl at its speed level, at least kFireGap frames).
//  - Ammunition: kMag rounds; one comes back every kRegen frames.
//  - The pistol in her hand: the handgun's model (em/wep02.drs, entries 6 / 5 as wep02's objects take them) read
//    once to a block of the mods' place (runtime/mod_place.cpp; until 22.11.2026 a fixed MEM1 address the game's
//    heaps now reach), an event body (SetObj18) hung on her hand part (OyaSetObj18), hidden in events,
//    sub screens and movies; never destroyed while it is shown (runtime/wardrobe.cpp: hidden, destroyed later).
//  - Each step is logged ("ashley: ...").
#include "types.h"
#include "global.h"
#include "main.h"
#include "em.h"
#include "pl_npc.h"
#include "pl_wep.h"
#include "player.h"
#include "atari.h"
#include "snd.h"
#include "obj.h"
#include "obj18.h"
#include "model.h"
#include "dvd.h"
#include "item.h"
#include "joy.h"
#include "pl_sub.h"
#include "id_sys.h"
#include "texture.h"
#include <dolphin/dvd.h>

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void* re4_mod_place_alloc(unsigned long size, const char* what);  // runtime/mod_place.cpp
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" void re4_dsp_catch_next(void);                       // runtime/dsp.cpp: the next voices are a pistol's shot
extern "C" int re4_dsp_caught(const short** pcm, int* serial);   // runtime/dsp.cpp
extern "C" void re4_mod_pcm_play(const short* pcm, unsigned frames, int gain);  // runtime/mod_sound.cpp
extern "C" int re4_sound_volume(int music);                       // runtime/sound_volume.cpp
extern "C" const char* re4_data_dir(void);                        // runtime/dvd.cpp
extern "C" void* re4_gx_cached(unsigned int physical);            // runtime/gx_fifo.cpp
extern cTexSys* g_pIdTexSys;                                       // src/game/id_tex.cpp
extern "C" int GXGetTexObjFmt(const GXTexObj* to);                // src/lib/GXTexture.c
extern "C" u16 GXGetTexObjWidth(const GXTexObj* to);
extern "C" u16 GXGetTexObjHeight(const GXTexObj* to);
extern "C" int GXGetTlutObjFmt(const GXTlutObj* tlut);
extern "C" void re4_formats_part_loaded(const char* name, int nest, int index, const void* part, unsigned addr);

namespace {

const u32 kGunMax = 0x100000u;
u32 gGunAt;
// (Mods, 22.11.2026) a block of the mods' place (runtime/mod_place.cpp), taken once and kept: the fixed address of
// MEM1 used before lies in the game's heaps since upstream's 1.1.0 (re4_mem1_map.h, room heap 4 to 0x826F4000), where
// the archive overwrote the room's data (DevJunior's crash entering r118 with her pistol, 22.11.2026: Esp08_Trans)
u32 placeOf(u32& at, u32 size, const char* what)
{
    if (!at) {
        at = (u32) (unsigned long) re4_mod_place_alloc(size, what);
    }
    return at;
}
u32 gunAddr() { return placeOf(gGunAt, kGunMax, "Ashley's pistol (em/wep02.drs)"); }
const char* const kGunFile = "em/wep02.drs";
const int kGunBin = 6, kGunTpl = 5;

const u8 kAway = 0x41;         // cItem::type of the pistol Ashley holds: no case shows it (patch 0912)
const int kAskHold = 90;       // frames the partner command button is held to ask for it back (3 s; 1 s until
                               // 23.11.2026: DevJunior wants her to keep it, a plain press must not take it)
const u16 kPistols[] = {0x23, 0x24, 0x21, 0x22, 0x40, 0x25, 0x26, 0x27, 0x28, 0x03, 0x29, 0x2A, 0x2B, 0x37};

const f32 kRange = 7000.0f;    // 7 m (DevJunior, 23.11.2026: "not so much range, so that it is balanced"; 12 m before)
const f32 kCone = 1.4f;        // radians either side of where she faces, while she walks
const f32 kTurn = 0.12f;       // radians a frame she turns standing
const int kMag = 10;
const int kRegen = 90;         // frames for one round (3 s)
const int kFireGap = 21;       // frames between shots at least: 0.7 s (DevJunior, 23.11.2026: "the fire rate 7";
                               // 24 + 0..6 at random after "base it on Luis's", 14 before)
const int kFireJitter = 0;     // frames added at random
const int kAimIn = 6;          // frames to raise the arm
const f32 kPi = 3.14159265f;
const f32 kLockCone = 0.6f;    // radians either side of where she faces: her own aim locks on (stage 3)
const f32 kAimTurn = 0.06f;    // radians a frame at full stick while she aims (stage 3)

std::atomic<bool> gOn(false);
std::atomic<bool> gHooked(false);

int gAmmo = kMag;
int gRegenT = 0;
int gGap = 0;
f32 gW = 0.0f;           // the arm's share of the aim, 0..1
cEm* gTarget = 0;
cSubChar* gAshley = 0;   // she, as cSubChar::move last ran (frame gSeen)
u32 gSeen = 0;
cEm* gArmOf = 0;         // whose arm gUpper / gFore / gHand are
int gUpper = -1, gFore = -1, gHand = -1;
int gUpperL = -1, gForeL = -1, gHandL = -1;  // her left arm (the two-handed hold; -1: one-handed)
f32 gArmLen = 0.0f;
// the enemies her last shots hit, the pistol's firepower level and its weapon number with them (GetWepDmVal, patch
// 0926: the damage is the enemy's to work out a frame later, with Leon's own weapon's level by then)
struct HerHit {
    const cEm* em;
    u32 frame;
    u8 wep, lv;
};
HerHit gHits[16];
int gNextHit = 0;
int gShots = 0;

bool gLoaded = false;
bool gLoadFailed = false;
cObj* gObj = 0;
void* gObjModel = 0;
cEm* gCarrier = 0;
cPlayer* gPlayer = 0;    // she as the player (stage 3), as cPlAshley::moveMatCalcBefore last ran (frame gPlSeen)
u32 gPlSeen = 0;
bool gAiming = false;    // stage 3: the aim key held this frame (re4_ashley_gun_input)
bool gFireAsked = false; // stage 3: A pressed while aiming
cObj* gOld = 0;
void* gOldModel = 0;
int gOldWait = 0;

bool isPistol(u16 id)
{
    for (u16 k : kPistols) {
        if (k == id) {
            return true;
        }
    }
    return false;
}

// the pistol Ashley carries (an item of the set kAway), or NULL
cItem* givenItem()
{
    if (!ItemMgr.m_pItem) {
        return 0;
    }
    cItem* p = ItemMgr.m_pItem;
    for (int i = 0; i < ItemMgr.m_array_num && i < 0x400; i++, p++) {
        if ((p->flags & 1) && p->type == kAway) {
            return p;
        }
    }
    return 0;
}

// a few frames at random (her own numbers: the game's Rnd stays as it is for the enemies)
int jitter()
{
    static u32 x = 0x2545F491u;
    x = x * 1664525u + 1013904223u;
    return (int) ((x >> 16) % (kFireJitter + 1));
}

// frames between her shots: the pistol's own rate at its speed level (PlShotFrameTbl: the hand cannon slower than the
// handgun), at least kFireGap
int fireGap()
{
    cItem* it = givenItem();
    if (!it) {
        return kFireGap;
    }
    const u8 no = WeaponId2WeaponNo(it->id);
    if (no == 0xFF || no >= 45) {
        return kFireGap + jitter();
    }
    int lv = it->getSpeedLevel();
    lv = lv < 0 ? 0 : lv > 4 ? 4 : lv;
    const int f = (int) PlShotFrameTbl[no][lv];
    return (f > kFireGap ? f : kFireGap) + jitter();
}

// she gives p back: into Leon's case at its old place (the case finds another when that one is taken)
void giveBack(cItem* p, const char* why)
{
    p->type = 0;
    p->board = 1;
    re4_log("ashley: she gives pistol 0x%02x back to Leon (%s)", (unsigned) p->id, why);
}

int gAskFrames = 0;
bool gAskFollowing = false;  // she followed before the press that began the hold

// the partner command button held: she gives the pistol back; the "wait / follow" its press gave her undone
void askBack()
{
    const bool held = (Key.on & 0x200) != 0;
    if (!held) {
        gAskFrames = 0;
        gAskFollowing = pSUB && ((cModel*) pSUB)->id == 3 && (SubCharGetStatus() & 0x40000000) != 0;
        return;
    }
    if (++gAskFrames != kAskHold) {
        return;
    }
    cItem* p = givenItem();
    if (!p || !pSUB || ((cModel*) pSUB)->id != 3 || pG->pl_type != 0 || StaFlagChk(pG, STA_EVENT) ||
        StaFlagChk(pG, STA_SUB_SCRN)) {
        return;
    }
    giveBack(p, "asked");
    SubCharCtrl(gAskFollowing ? 0 : 1, 0);
    SndCall(0, 8, 0, 0, 0, 0);
}

// she shoots: the switch on and she holds a pistol
bool armed()
{
    return gOn.load(std::memory_order_relaxed) && givenItem() != 0;
}

u32 be32At(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

f32 wrap(f32 a)
{
    while (a > kPi) {
        a -= 2 * kPi;
    }
    while (a < -kPi) {
        a += 2 * kPi;
    }
    return a;
}

Vec bindPos(const cParts* p)
{
    Vec v = {-p->lt_inv_mat[0][3], -p->lt_inv_mat[1][3], -p->lt_inv_mat[2][3]};
    return v;
}

int partIndex(cModel* m, const cCoord* c)
{
    int i = 0;
    for (cParts* p = m->pList; p && i < 200; p = p->pList, i++) {
        if ((const cCoord*) p == c) {
            return i;
        }
    }
    return -1;
}

// An arm by her bind pose: the part farthest out to `side` (x, model space) and its chain up to the shoulder
bool armChain(cEm* a, f32 side, int* up, int* fore, int* hand, int* farthest, f32* reach)
{
    int n = 0, far = -1;
    f32 best = 0.0f;
    for (cParts* p = a->pList; p && n < 200; p = p->pList, n++) {
        const f32 x = bindPos(p).x * side;
        if (x > best) {
            best = x;
            far = n;
        }
    }
    *farthest = far;
    if (far < 0) {
        return false;
    }
    int chain[64];
    int len = 0;
    for (cParts* p = a->getPartsPtr(far); p && len < 64;) {
        chain[len++] = partIndex(a, p);
        if (!p->pParent || p->pParent == (cCoord*) a) {
            break;
        }
        p = (cParts*) p->pParent;
    }
    // the upper arm: the topmost part of the chain still out at the side (15 % of the hand's reach)
    int u = -1;
    for (int k = 0; k < len; k++) {
        if (chain[k] < 0) {
            break;
        }
        if (bindPos(a->getPartsPtr(chain[k])).x * side >= 0.15f * best) {
            u = k;
        } else {
            break;
        }
    }
    if (u < 2) {
        return false;
    }
    *up = chain[u];
    *fore = chain[u - 1];
    *hand = chain[u - 2];
    *reach = best;
    return true;
}

// Her arms by her bind pose (once per Ashley): the right one holds the pistol, the left one supports it
void findArm(cEm* a)
{
    gArmOf = a;
    gUpper = gFore = gHand = -1;
    gUpperL = gForeL = gHandL = -1;
    f32 side = -1.0f;  // Leon's right is -x (runtime/wardrobe.cpp)
    if (pG && pG->pl_type == 0 && pPL && pPL->pList) {
        cParts* h = pPL->getPartsPtr(10);
        if (h) {
            side = bindPos(h).x < 0.0f ? -1.0f : 1.0f;
        }
    }
    int far = -1, farL = -1;
    f32 reach = 0.0f, reachL = 0.0f;
    if (!armChain(a, side, &gUpper, &gFore, &gHand, &far, &reach)) {
        gUpper = gFore = gHand = -1;
        re4_log("ashley: no right arm found (farthest part %d): no pistol", far);
        return;
    }
    if (!armChain(a, -side, &gUpperL, &gForeL, &gHandL, &farL, &reachL)) {
        gUpperL = gForeL = gHandL = -1;
    }
    const Vec pu = bindPos(a->getPartsPtr(gUpper));
    const Vec ph = bindPos(a->getPartsPtr(gHand));
    gArmLen = sqrtf((ph.x - pu.x) * (ph.x - pu.x) + (ph.y - pu.y) * (ph.y - pu.y) + (ph.z - pu.z) * (ph.z - pu.z));
    re4_log("ashley: right arm: upper %d (%.0f %.0f %.0f), forearm %d, hand %d (%.0f %.0f %.0f), farthest part %d; "
            "left arm: upper %d, forearm %d, hand %d (two hands: %s)", gUpper, pu.x, pu.y, pu.z, gFore, gHand, ph.x,
            ph.y, ph.z, far, gUpperL, gForeL, gHandL, gHandL >= 0 ? "yes" : "no");
}

bool canShoot(cSubChar* a)
{
    if (!pG || pG->Rno0 != 3 || pG->pl_type != 0 || !pPL || StaFlagChk(pG, STA_EVENT) || StaFlagChk(pG, STA_SUB_SCRN) ||
        StaFlagChk(pG, STA_MOVIE_ON)) {
        return false;
    }
    if (a->r_no_0 != 0 || a->flg.check(cSubChar::F_SLEEP)) {
        return false;
    }
    const u8 r = a->r_no_1;
    return r == 0 || r == 1 || r == 2 || r == 5 || r == 6;
}

int slotOf(cEm* em)
{
    const u32 n = EmMgr.getArrayNum();
    for (u32 i = 0; i < n && i < 256; i++) {
        if (EmMgr.fastAt(i) == em) {
            return (int) i;
        }
    }
    return -1;
}

bool aimPoint(cEm* em, Vec* out)
{
    cParts* p = em->pList ? em->getPartsPtr(em->lockParts & 0x7F) : 0;
    if (p) {
        *out = p->world;
    } else {
        *out = em->pos;
        out->y += 1000.0f;
    }
    return true;
}

bool sees(const Vec& from, const Vec& to)
{
    Vec a = from, b = to, cross, nrm;
    return SatMgr.hitCheck(&a, &b, &cross, &nrm, 0, 0) == 0;
}

bool valid(cEm* a, cEm* em, const Vec& eye, f32 turn, f32* dist)
{
    if (!em || (cEm*) em == (cEm*) a || !em->isAlive() || em->hp <= 0 || em->hp_max <= 20 || em->id < 0x10 ||
        em->id > 0x3F || em->checkStatus(EM_STATUS_LOCKOFF)) {
        return false;
    }
    const f32 dx = em->pos.x - a->pos.x, dz = em->pos.z - a->pos.z;
    const f32 d = sqrtf(dx * dx + dz * dz);
    if (d > kRange || fabsf(em->pos.y - a->pos.y) > 3000.0f) {
        return false;
    }
    if (turn < kPi && fabsf(wrap(atan2f(dx, dz) - a->ang.y)) > turn) {
        return false;
    }
    Vec tp;
    aimPoint(em, &tp);
    if (!sees(eye, tp)) {
        return false;
    }
    *dist = d;
    return true;
}

cEm* pickTarget(cEm* a, const Vec& eye, f32 turn)
{
    cEm* best = 0;
    f32 bestD = 1e9f;
    const u32 n = EmMgr.getArrayNum();
    for (u32 i = 0; i < n && i < 256; i++) {
        cEm* em = EmMgr.fastAt(i);
        f32 d;
        if (valid(a, em, eye, turn, &d) && d < bestD) {
            bestD = d;
            best = em;
        }
    }
    return best;
}

void quatFromTo(const Vec& a, const Vec& b, Quaternion* q)
{
    const f32 d = a.x * b.x + a.y * b.y + a.z * b.z;
    Vec c = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    const f32 s = sqrtf((1.0f + (d > -0.9999f ? d : -0.9999f)) * 2.0f);
    q->x = c.x / s;
    q->y = c.y / s;
    q->z = c.z / s;
    q->w = s * 0.5f;
}

// a part's local rotation towards q by w, its translation and scale kept
void turnLocal(cParts* p, const Quaternion& q, f32 w)
{
    Quaternion qa, r;
    C_QUATMtx(&qa, p->l_mat);
    C_QUATSlerp(&qa, (Quaternion*) &q, &r, w);
    Vec t = {p->l_mat[0][3], p->l_mat[1][3], p->l_mat[2][3]};
    PSMTXQuat(p->l_mat, &r);
    TransMatrix(p->l_mat, &t);
    ScaleMatrix(p->l_mat, &p->scale);
}

// ---- the two-handed hold (wish of DevJunior, 16.11.2026: "fix Ashley's animations when she uses the gun": until
// then her right arm pointed straight out, alone). Her bind pose has no rotation (cParts::lt_inv_mat), so a part's
// bind offset is its local translation and "rotation W maps the bone's bind direction onto d" says all.

struct M3 {
    f32 m[3][3];
};

Vec v3(f32 x, f32 y, f32 z)
{
    Vec v = {x, y, z};
    return v;
}
Vec sub3(const Vec& a, const Vec& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec add3(const Vec& a, const Vec& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec mul3(const Vec& a, f32 k) { return v3(a.x * k, a.y * k, a.z * k); }
f32 dot3(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross3(const Vec& a, const Vec& b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
f32 len3(const Vec& a) { return sqrtf(dot3(a, a)); }
Vec unit3(const Vec& a)
{
    const f32 l = len3(a);
    return l > 1e-6f ? mul3(a, 1.0f / l) : v3(0.0f, 0.0f, 1.0f);
}
// a part's world rotation (its world matrix's columns made unit: the model's scale is uniform)
M3 worldRot(const cCoord* c)
{
    M3 r;
    for (int k = 0; k < 3; k++) {
        const Vec col = unit3(v3(c->mat[0][k], c->mat[1][k], c->mat[2][k]));
        r.m[0][k] = col.x;
        r.m[1][k] = col.y;
        r.m[2][k] = col.z;
    }
    return r;
}
Vec apply(const M3& r, const Vec& v)
{
    return v3(r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z, r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
              r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z);
}
Vec applyT(const M3& r, const Vec& v)
{
    return v3(r.m[0][0] * v.x + r.m[1][0] * v.y + r.m[2][0] * v.z, r.m[0][1] * v.x + r.m[1][1] * v.y + r.m[2][1] * v.z,
              r.m[0][2] * v.x + r.m[1][2] * v.y + r.m[2][2] * v.z);
}
M3 mulM(const M3& a, const M3& b)
{
    M3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}
M3 ofQuat(const Quaternion& q)
{
    Mtx t;
    PSMTXQuat(t, &q);
    M3 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = t[i][j];
        }
    }
    return r;
}
Quaternion axisAngle(const Vec& axis, f32 ang)
{
    const Vec a = unit3(axis);
    const f32 s = sinf(ang * 0.5f);
    Quaternion q = {a.x * s, a.y * s, a.z * s, cosf(ang * 0.5f)};
    return q;
}
Quaternion qmul(const Quaternion& a, const Quaternion& b)
{
    Quaternion r = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return r;
}

// The elbow of a two-bone chain from the shoulder s to the grip g (bones l1, l2), bent towards `pole`
Vec elbowOf(const Vec& s, const Vec& g, f32 l1, f32 l2, const Vec& pole)
{
    const Vec dv = sub3(g, s);
    f32 dist = len3(dv);
    const Vec dir = unit3(dv);
    if (dist > l1 + l2 - 1.0f) {
        dist = l1 + l2 - 1.0f;
    }
    if (dist < 1.0f) {
        dist = 1.0f;
    }
    const f32 along = (l1 * l1 - l2 * l2 + dist * dist) / (2.0f * dist);
    const f32 h2 = l1 * l1 - along * along;
    const f32 h = h2 > 0.0f ? sqrtf(h2) : 0.0f;
    const Vec p = unit3(sub3(pole, mul3(dir, dot3(pole, dir))));
    return add3(add3(s, mul3(dir, along)), mul3(p, h));
}

// One arm onto the grip g: the upper arm to the elbow, the forearm to g, the hand along `aim` with the thumb up (her
// bind pose is a T with the palms down and the thumbs forward, +z: the pistol in it lies along the fingers with its
// sights on the thumb's side, so thumb up = the pistol upright, the palm facing in), each blended in by gW from the
// walk's pose
void reachArm(cEm* a, int up, int fore, int hand, const Vec& g, const Vec& pole, const Vec& aim)
{
    cParts* u = a->getPartsPtr(up);
    cParts* f = a->getPartsPtr(fore);
    cParts* h = a->getPartsPtr(hand);
    if (!u || !f || !h || !u->pParent) {
        return;
    }
    const Vec bu = bindPos(u), bf = bindPos(f), bh = bindPos(h);
    const Vec restUF = sub3(bf, bu), restFH = sub3(bh, bf);
    const f32 l1 = len3(restUF), l2 = len3(restFH);
    if (l1 < 1.0f || l2 < 1.0f) {
        return;
    }
    const Vec s = u->world;
    const Vec e = elbowOf(s, g, l1, l2, pole);
    // the upper arm: in its parent's frame its bind direction onto the elbow
    const M3 par = worldRot(u->pParent);
    Quaternion qu, qf, qh;
    quatFromTo(unit3(restUF), unit3(applyT(par, sub3(e, s))), &qu);
    const M3 wu = mulM(par, ofQuat(qu));
    // the forearm: onto the grip (where the elbow put it)
    const Vec wristAt = add3(e, mul3(unit3(sub3(g, e)), l2));
    quatFromTo(unit3(restFH), unit3(applyT(wu, sub3(wristAt, e))), &qf);
    const M3 wf = mulM(wu, ofQuat(qf));
    // the hand: its bind direction (the forearm's) along the aim, then rolled about it so its thumb faces up
    quatFromTo(unit3(restFH), unit3(applyT(wf, aim)), &qh);
    const M3 wh = mulM(wf, ofQuat(qh));
    const Vec d = unit3(aim);
    const Vec upW = v3(0.0f, 1.0f, 0.0f);
    Vec want = sub3(upW, mul3(d, dot3(upW, d)));
    Vec now = apply(wh, v3(0.0f, 0.0f, 1.0f));
    now = sub3(now, mul3(d, dot3(now, d)));
    if (len3(want) > 0.05f && len3(now) > 0.05f) {
        want = unit3(want);
        now = unit3(now);
        const f32 ang = atan2f(dot3(d, cross3(now, want)), dot3(now, want));
        qh = qmul(qh, axisAngle(restFH, ang));  // about the hand's own bone: about the aim in the world
    }
    turnLocal(u, qu, gW);
    turnLocal(f, qf, gW);
    turnLocal(h, qh, gW);
}

// The pistol held in both hands at her eyes' height in front of her chest, pointed at the target; the elbows bent
// down and out (without a left arm: the right one alone, its elbow bent the same way)
Vec gGrip = {0.0f, 0.0f, 0.0f};  // where her hands hold the pistol this frame (fire's muzzle)
Vec gAim = {0.0f, 0.0f, 1.0f};

void aimArm(cEm* a, const Vec& target)
{
    cParts* ur = a->getPartsPtr(gUpper);
    if (!ur || !ur->pParent) {
        return;
    }
    cParts* ul = gUpperL >= 0 ? a->getPartsPtr(gUpperL) : 0;
    const Vec sr = ur->world;
    const Vec sl = ul ? ul->world : sr;
    // her right (from the left shoulder to the right one; one-handed: square to where she faces)
    Vec right = ul ? sub3(sr, sl) : v3(-cosf(a->ang.y), 0.0f, sinf(a->ang.y));
    right.y = 0.0f;
    right = unit3(right);
    const Vec chest = ul ? mul3(add3(sr, sl), 0.5f) : add3(sr, mul3(right, -150.0f));
    const Vec d = unit3(sub3(target, add3(chest, v3(0.0f, 120.0f, 0.0f))));
    // the grip: 0.8 of the arm's reach in front of her chest, a little above the shoulders (the sights at the eyes)
    const Vec grip = add3(add3(chest, mul3(d, 0.78f * gArmLen)), v3(0.0f, 70.0f, 0.0f));
    gGrip = grip;
    gAim = d;
    const Vec down = v3(0.0f, -1.0f, 0.0f);
    reachArm(a, gUpper, gFore, gHand, grip, add3(down, mul3(right, 0.7f)), d);
    if (ul) {
        // the left hand under and behind the right one, cupping the grip
        const Vec support = add3(add3(grip, mul3(d, -40.0f)), add3(mul3(right, -35.0f), v3(0.0f, -45.0f, 0.0f)));
        reachArm(a, gUpperL, gForeL, gHandL, support, add3(down, mul3(right, -0.7f)), d);
    }
}

// Her shot's sound (device report of DevJunior, 23.11.2026: "sometimes it sounds like the shotgun or the other
// weapons"): the weapon block (2) holds the sounds of the weapon Leon holds. Leon holding a pistol: its shot from
// that block, and runtime/dsp.cpp catches it (the loudest voice starting then) to play later, saved to the data
// folder (ashley_pistol_shot.pcm: 32 kHz mono 16 bit, from the player's own disc) for the next games; Leon holding
// another weapon: the caught shot through the mod sound mixer, at the effects volume; none caught yet: silence.
short* gShotPcm = 0;
int gShotN = 0;
int gShotSerial = -1;
bool gShotFileTried = false;

void shotPath(char* out, size_t n)
{
    snprintf(out, n, "%s/ashley_pistol_shot.pcm", re4_data_dir());
}

void shotSound(const Vec& muzzle)
{
    cItem* lw = ItemMgr.weapon();
    if (lw && isPistol(lw->id)) {
        re4_dsp_catch_next();
        SndCall(2, 0, (Vec*) &muzzle, 0, 0, 0);
        return;
    }
    const short* pcm = 0;
    int serial = 0;
    const int n = re4_dsp_caught(&pcm, &serial);
    if (n > 0) {
        if (serial != gShotSerial) {
            gShotSerial = serial;
            char path[1100];
            shotPath(path, sizeof path);
            if (FILE* f = fopen(path, "wb")) {
                fwrite(pcm, sizeof(short), (size_t) n, f);
                fclose(f);
                re4_log("ashley: the pistol's shot saved (%d ms)", n / 32);
            }
        }
        re4_mod_pcm_play(pcm, (unsigned) n, re4_sound_volume(0) * 200 / 100);
        return;
    }
    if (!gShotFileTried) {
        gShotFileTried = true;
        char path[1100];
        shotPath(path, sizeof path);
        if (FILE* f = fopen(path, "rb")) {
            static short buf[32000];
            gShotN = (int) fread(buf, sizeof(short), 32000, f);
            fclose(f);
            gShotPcm = buf;
            re4_log("ashley: the pistol's shot read (%d ms)", gShotN / 32);
        } else {
            re4_log("ashley: no pistol shot yet (Leon holding a pistol while she fires catches it)");
        }
    }
    if (gShotPcm && gShotN > 0) {
        re4_mod_pcm_play(gShotPcm, (unsigned) gShotN, re4_sound_volume(0) * 200 / 100);
    }
}

void fire(cEm* a, const Vec& tp, cEm* em)
{
    const Vec d = sub3(tp, gGrip);
    const f32 dl = len3(d);
    if (dl < 1.0f) {
        return;
    }
    Vec muzzle = add3(gGrip, mul3(d, 150.0f / dl));  // the barrel in front of her hands
    Vec to = tp;
    // the pistol she holds: its own weapon number (its damage against each enemy, its piercing) and its firepower
    // level (DevJunior, 16.11.2026: "the hand cannon does the same damage": until then always Luis's number 3 at
    // Leon's level)
    u8 wep = 3, lv = 0;
    if (cItem* it = givenItem()) {
        const u8 no = WeaponId2WeaponNo(it->id);
        if (no != 0xFF) {
            wep = no;
        }
        lv = (u8) it->getPowerLevel();
    }
    // the enemies not hit before the shot: those hit by it are hers
    const cEm* before[64];
    int nb = 0;
    const u32 count = EmMgr.getArrayNum();
    for (u32 i = 0; i < count && i < 256 && nb < 64; i++) {
        cEm* m = EmMgr.fastAt(i);
        if (m && m->isAlive() && !(m->dmg.m_Flag & 1)) {
            before[nb++] = m;
        }
    }
    const s16 hp = a->hp;
    a->hp = 0;
    const u32 hits = PlWepHitCheck2(0, &muzzle, &to, wep, 2, 6000.0f);
    a->hp = hp;
    for (int k = 0; k < nb; k++) {
        if ((before[k]->dmg.m_Flag & 1) && before[k]->dmg.m_Wep == wep) {
            gHits[gNextHit] = HerHit{before[k], pG->Frame_cnt, wep, lv};
            gNextHit = (gNextHit + 1) % 16;
        }
    }
    shotSound(muzzle);
    gAmmo--;
    gShots++;
    if (gShots <= 20 || gShots % 50 == 0) {
        if (em) {
            re4_log("ashley: shot %d at s%d (id 0x%x, hp %d) from %.1f m, weapon %u level %u, %u hit, %d rounds left",
                    gShots, slotOf(em), (unsigned) em->id, (int) em->hp, dl / 1000.0f, (unsigned) wep, (unsigned) lv,
                    (unsigned) hits, gAmmo);
        } else {
            re4_log("ashley: shot %d straight ahead, %u hit, %d rounds left", gShots, (unsigned) hits, gAmmo);
        }
    }
}

// ---- the pistol in her hand --------------------------------------------------------------------------------------

bool loadGun()
{
    DVDFileInfo fi;
    if (!DVDOpen((char*) kGunFile, &fi)) {
        re4_log("ashley: %s not readable: no pistol in her hand", kGunFile);
        return false;
    }
    const u32 kGunAddr = gunAddr();
    if (!kGunAddr) {
        DVDClose(&fi);
        re4_log("ashley: no room in the mods' place for %s: no pistol in her hand", kGunFile);
        return false;
    }
    u8* const base = (u8*) (unsigned long) kGunAddr;
    u32 partOfs = 0, partSize = 0;
    if (DVDReadPrio(&fi, base, 0x400, 0, 2) == 0x400) {
        for (u32 at = 0x20; at + 0x20 <= 0x400; at += 0x20) {
            const u32 kind = be32At(base + at);
            if (kind == 0xFFFFFFFFu) {
                break;
            }
            if (kind == TRANS_MRAM) {
                partSize = be32At(base + at + 4);
                partOfs = be32At(base + at + 0xC);
                break;
            }
        }
    }
    const u32 len = (partSize + 0x1F) & ~0x1Fu;
    const bool ok = partSize >= 0x20 && len <= kGunMax && DVDReadPrio(&fi, base, (s32) len, (s32) partOfs, 2) == (s32) len;
    DVDClose(&fi);
    if (!ok) {
        re4_log("ashley: %s: no archive of at most %u bytes", kGunFile, (unsigned) kGunMax);
        return false;
    }
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = partSize;
    h.dest = kGunAddr;
    re4_formats_part_loaded(kGunFile, 0, 0, &h, kGunAddr);
    re4_log("ashley: %s (%u bytes) read to 0x%08x for her pistol", kGunFile, (unsigned) partSize, (unsigned) kGunAddr);
    return true;
}

void* arcEntry(const PlArc* arc, int no)
{
    const u32 count = arc->ofs[0];
    if (no < 4 || (u32) (no - 4) >= count || count > 0x400 || arc->ofs[no] == 0) {
        return 0;
    }
    return PL_ARC_PTR(arc, no);
}

bool objOurs()
{
    return gObj && gObj->isAlive() && gObj->id == cObjMgr::ID_EVENT && gObj->pModelInfo &&
           (void*) gObj->pModelInfo->model_addr == gObjModel;
}

void retireOld()
{
    if (!gOld) {
        return;
    }
    const bool ours = gOld->isAlive() && gOld->id == cObjMgr::ID_EVENT && gOld->pModelInfo &&
                      (void*) gOld->pModelInfo->model_addr == gOldModel;
    if (!ours) {
        gOld = 0;
        return;
    }
    gOld->setTrans(0);
    if (--gOldWait <= 0) {
        ObjMgr.destroy(gOld);
        gOld = 0;
    }
}

void dropGun(const char* why)
{
    if (objOurs()) {
        if (gOld && gOld != gObj) {
            gOldWait = 0;
            retireOld();
        }
        gObj->setTrans(0);
        OyaSetObj18(gObj, 0, 0);
        gOld = gObj;
        gOldModel = gObjModel;
        gOldWait = 3;
        re4_log("ashley: the pistol put away (%s)", why);
    }
    gObj = 0;
    gObjModel = 0;
    gCarrier = 0;
}

void gunFrame(unsigned)
{
    retireOld();
    if (pG && pG->Rno0 == 3) {
        cItem* p = !gOn.load(std::memory_order_relaxed) ? givenItem() : 0;
        if (p) {
            giveBack(p, "switched off");
        } else {
            askBack();
        }
    }
    cEm* a = !pG                ? 0
             : pG->pl_type == 0 ? (gAshley && pG->Frame_cnt - gSeen <= 2 && pSUB == (cEm*) gAshley ? (cEm*) gAshley : 0)
             : pG->pl_type == 1 ? (gPlayer && pG->Frame_cnt - gPlSeen <= 2 && pPL == gPlayer ? (cEm*) gPlayer : 0)
                                : 0;
    if (!armed() || !a || gHand < 0 || gArmOf != a) {
        if (gObj) {
            dropGun(!gOn.load() ? "switched off" : !givenItem() ? "no pistol hers" : "no Ashley");
        }
        return;
    }
    const bool shown = pG->Rno0 == 3 && !StaFlagChk(pG, STA_EVENT) && !StaFlagChk(pG, STA_SUB_SCRN) &&
                       !StaFlagChk(pG, STA_MOVIE_ON) && (a->be_flag & 2);
    if (objOurs() && gCarrier == a) {
        gObj->setTrans(shown ? 1 : 0);
        return;
    }
    if (objOurs()) {
        dropGun("another Ashley");
        return;
    }
    if (!shown || gLoadFailed) {
        return;
    }
    if (!gLoaded) {
        gLoaded = loadGun();
        if (!gLoaded) {
            gLoadFailed = true;
            return;
        }
    }
    const PlArc* arc = (const PlArc*) (unsigned long) gGunAt;
    void* bin = arcEntry(arc, kGunBin);
    void* tpl = arcEntry(arc, kGunTpl);
    if (!bin || !tpl) {
        re4_log("ashley: %s has no model at entries %d / %d", kGunFile, kGunBin, kGunTpl);
        gLoadFailed = true;
        return;
    }
    Vec pos = {0.0f, 0.0f, 0.0f};
    Vec rot = {0.0f, 0.0f, 0.0f};
    cObj* obj = SetObj18(bin, tpl, &pos, &rot, OBJ18_TYPE_WEPXX);
    if (!obj || !obj->pModelInfo) {
        re4_log("ashley: her pistol's body could not be made");
        gLoadFailed = true;
        return;
    }
    obj->atari.off();
    obj->setNoClip(1);
    obj->setNoSuspend(1);
    obj->setTrans(1);
    OyaSetObj18(obj, a, gHand);
    gObj = obj;
    gObjModel = (void*) obj->pModelInfo->model_addr;
    gCarrier = a;
    re4_log("ashley: the pistol in her hand (part %d)", gHand);
}

}  // namespace

extern "C" {

// Patch 0926 (em_dm_val.cpp GetWepDmVal, after it takes Leon's firepower level): an enemy her own shot hit in the
// last frames (weapon `wep`) takes the level of her pistol instead
int re4_ashley_power_level(const void* em, unsigned wep, int lv)
{
    if (!pG || !em) {
        return lv;
    }
    for (HerHit& h : gHits) {
        if (h.em == em && h.wep == wep && pG->Frame_cnt - h.frame < 30) {
            return h.lv;
        }
    }
    return lv;
}

// Patch 0911 (pl_npc.cpp cSubChar::move, before partsWorldCalc; the game thread): her aim and shots.
void re4_ashley_gun_move(cSubChar* a)
{
    if (!a || !pG) {
        return;
    }
    gAshley = a;
    gSeen = pG->Frame_cnt;
    if (!gHooked.exchange(true)) {
        re4_add_frame_hook(gunFrame);
    }
    if (!armed()) {
        gW = 0.0f;
        gTarget = 0;
        return;
    }
    if (gArmOf != a) {
        findArm(a);
    }
    if (gAmmo < kMag && ++gRegenT >= kRegen) {
        gRegenT = 0;
        gAmmo++;
    }
    if (gGap > 0) {
        gGap--;
    }
    if (gHand < 0 || !canShoot(a)) {
        gW = 0.0f;
        gTarget = 0;
        return;
    }
    a->partsWorldCalc();  // this frame's pose (the game computes it again after us)
    Vec eye = a->pList->world;
    eye.y += 400.0f;
    const bool standing = a->r_no_1 == 0;
    const f32 turn = standing ? 2 * kPi : kCone;
    f32 d;
    if (!gTarget || !valid(a, gTarget, eye, turn, &d)) {
        cEm* t = pickTarget(a, eye, turn);
        if (t != gTarget && t) {
            re4_log("ashley: target s%d (id 0x%x, hp %d)", slotOf(t), (unsigned) t->id, (int) t->hp);
        }
        gTarget = t;
    }
    if (!gTarget) {
        gW = gW - 1.0f / kAimIn < 0.0f ? 0.0f : gW - 1.0f / kAimIn;
        return;
    }
    Vec tp;
    aimPoint(gTarget, &tp);
    if (standing) {
        const f32 want = atan2f(tp.x - a->pos.x, tp.z - a->pos.z);
        const f32 diff = wrap(want - a->ang.y);
        a->ang.y = wrap(a->ang.y + (diff > kTurn ? kTurn : diff < -kTurn ? -kTurn : diff));
        if (fabsf(diff) > 0.5f) {
            gW = gW - 1.0f / kAimIn < 0.0f ? 0.0f : gW - 1.0f / kAimIn;
            return;  // turning first
        }
    }
    gW = gW + 1.0f / kAimIn > 1.0f ? 1.0f : gW + 1.0f / kAimIn;
    aimArm(a, tp);
    if (gW >= 0.99f && gGap == 0 && gAmmo > 0) {
        fire(a, tp, gTarget);
        gGap = fireGap();
    }
}

// Patch 0900's cPlayer::keyConfig hook (runtime/free_move.cpp re4_player_input, the game thread, before the
// routines read the keys; stage 3): 1 = Ashley the player aims her pistol this frame. Her stick, run and action
// keys are then out of the game's keys; A asks for a shot; the stick turns her (unless `freeCam`: the free camera's
// yaw, which the caller gives her after the camera moved).
int re4_ashley_gun_input(cPlayer* pl, int freeCam)
{
    gAiming = false;
    if (!pl || !pG || pG->pl_type != 1 || pG->Rno0 != 3 || !armed() || StaFlagChk(pG, STA_EVENT) ||
        StaFlagChk(pG, STA_SUB_SCRN) || StaFlagChk(pG, STA_MOVIE_ON) || !(Key.on & 0x10) || pl->r_no_0 != 0 ||
        pl->r_no_1 > 4 || pl->stat.check(cPlayer::F_EVENT)) {
        gFireAsked = false;
        return 0;
    }
    gAiming = true;
    if (Key.trg & 0x80) {
        gFireAsked = true;
    }
    if (!freeCam) {
        const int sx = Key.stickX;
        if (sx > 20 || sx < -20) {
            pl->ang.y = wrap(pl->ang.y - (f32) sx / 127.0f * kAimTurn);
        }
    }
    Key.stickX = 0;
    Key.stickY = 0;
    Key.on &= ~(u64) (0xF | 0x80 | 0x40000000);
    Key.trg &= ~(u64) (0xF | 0x80 | 0x40000000);
    Joy[0].on &= ~0x100u;  // A, for what reads the pad itself
    Joy[0].trg &= ~0x100u;
    return 1;
}

// Patch 0913 (pl_ashley.cpp cPlAshley::moveMatCalcBefore: her motion on her parts, before their world matrices;
// the game thread; stage 3): the pistol in her hand, her arm while she aims, her shots.
void re4_ashley_gun_player(cPlayer* a)
{
    if (!a || !pG) {
        return;
    }
    gPlayer = a;
    gPlSeen = pG->Frame_cnt;
    if (!gHooked.exchange(true)) {
        re4_add_frame_hook(gunFrame);
    }
    if (!armed()) {
        gW = 0.0f;
        gTarget = 0;
        gFireAsked = false;
        return;
    }
    if (gArmOf != a) {
        findArm(a);
    }
    if (gAmmo < kMag && ++gRegenT >= kRegen) {
        gRegenT = 0;
        gAmmo++;
    }
    if (gGap > 0) {
        gGap--;
    }
    if (gHand < 0 || !gAiming) {
        gW = gW - 1.0f / kAimIn < 0.0f ? 0.0f : gW - 1.0f / kAimIn;
        gTarget = 0;
        gFireAsked = false;
        if (gW > 0.0f && gHand >= 0) {
            a->partsWorldCalc();
            Vec ahead = a->pos;
            ahead.x += sinf(a->ang.y) * 5000.0f;
            ahead.z += cosf(a->ang.y) * 5000.0f;
            ahead.y = a->getPartsPtr(gUpper)->world.y;
            aimArm(a, ahead);  // the arm back down
        }
        return;
    }
    a->partsWorldCalc();  // this frame's pose (the game computes it again after us)
    Vec eye = a->pList->world;
    eye.y += 400.0f;
    f32 d;
    if (!gTarget || !valid(a, gTarget, eye, kLockCone, &d)) {
        cEm* t = pickTarget(a, eye, kLockCone);
        if (t != gTarget && t) {
            re4_log("ashley: she aims at s%d (id 0x%x, hp %d)", slotOf(t), (unsigned) t->id, (int) t->hp);
        }
        gTarget = t;
    }
    Vec tp;
    if (gTarget) {
        aimPoint(gTarget, &tp);
    } else {
        tp = a->pos;
        tp.x += sinf(a->ang.y) * kRange;
        tp.z += cosf(a->ang.y) * kRange;
        tp.y = a->getPartsPtr(gUpper)->world.y;
    }
    gW = gW + 1.0f / kAimIn > 1.0f ? 1.0f : gW + 1.0f / kAimIn;
    aimArm(a, tp);
    if (gFireAsked && gW >= 0.99f && gGap == 0) {
        gFireAsked = false;
        if (gAmmo > 0) {
            fire(a, tp, gTarget);
            gGap = fireGap();
        } else {
            SndCall(0, 7, 0, 0, 0, 0);
        }
    }
}

// Patch 0951 (pl_npc.cpp cSubChar::moveBehind; wish of DevJunior, 23.11.2026: "when I reload Ashley goes back
// instead of stepping aside and shooting too"): while Leon aims or reloads, the game puts Ashley behind him (`t`, his
// model space, atckPos); armed, she stands at his left side instead (his right while that is walled off; behind him
// when both are), level with him, where she sees past him.
void re4_ashley_behind(cSubChar* a, Vec* t)
{
    static int said;
    if (!a || !t || !pPL || !armed()) {
        return;
    }
    const f32 kSideX = 750.0f;   // mm to his side (his right is -x)
    for (int s = 0; s < 2; s++) {
        Vec side = {s == 0 ? kSideX : -kSideX, t->y, -150.0f};
        Vec w;
        PSMTXMultVec(pPL->mat, &side, &w);
        Vec from = pPL->pos;
        from.y += 900.0f;
        w.y = from.y;
        if (sees(from, w)) {
            *t = side;
            if (!(said & (1 << s))) {
                said |= 1 << s;
                re4_log("ashley: at Leon's %s while he aims or reloads", s == 0 ? "left" : "right");
            }
            return;
        }
    }
}

// Patch 0952 (DevJunior, 23.11.2026: "that she at least dodges the blows, depending on the situation, so that she
// is not overpowered"): an enemy's blow on Ashley (cSubChar::setDamage) she may duck, crouching as when Leon aims at
// her (routine 4: down, held a second, up; her body's collision off meanwhile). Only in Leon's game, Ashley (id 3),
// with the switch on; only the light kinds of blow (0..3; not 4/5, thrown down or blown away); one in three
// (kDodgeChance), and not again within 8 s (kDodgeRest) after a duck or a blow taken.
const int kDodgeChance = 3;    // one in this many blows
const int kDodgeRest = 240;    // frames after a duck or a blow before the next duck

int re4_ashley_dodge(cSubChar* a, int kind, int arg)
{
    static int last = -100000;
    if (!a || !gOn.load(std::memory_order_relaxed) || !pG || pG->pl_type != 0 || ((cModel*) a)->id != 3 ||
        kind > 3 || arg <= 0 || StaFlagChk(pG, STA_EVENT) || a->r_no_0 != 0) {
        return 0;
    }
    const int now = (int) pG->Frame_cnt;
    if (now - last < kDodgeRest) {
        return 0;
    }
    last = now;
    static u32 x = 0x9E3779B9u;
    x = x * 1664525u + 1013904223u;
    if ((x >> 16) % kDodgeChance != 0) {
        return 0;
    }
    a->setRno(0, 4, 0, 0);
    re4_log("ashley: ducked a blow (kind %d, %d damage)", kind, arg);
    return 1;
}

// Patch 0952: 1 = Ashley (id 3) carries her pistol: she does not crouch when Leon aims at her, and stands up from a
// duck after its second even while he aims (DevJunior, 25.11.2026: "that she can still keep firing and does not
// crouch if I aim at her")
int re4_ashley_no_caution(cSubChar* a)
{
    return a && ((cModel*) a)->id == 3 && armed() ? 1 : 0;
}

// Patch 0951: her motion `no` behind the aiming player (0x1D..0x1F covering her ears, 0x20 flinching): armed, her
// alert stand instead (0x12; DevJunior, 23.11.2026: "she still hides or gets scared every time I reload, instead of
// being alert")
int re4_ashley_behind_mot(int no)
{
    static bool said;
    if (!armed()) {
        return no;
    }
    if (!said) {
        said = true;
        re4_log("ashley: alert beside Leon (her stand in place of covering her ears)");
    }
    return 0x12;
}

// The names in the case's "use on Leon / Ashley" window (DevJunior, 25.11.2026, "the window still does not draw
// right"): on the debug disc its two name textures (id 0x39 "Leon" 96x48, 0x40 "Ashley" 144x48, CMPR) hold no picture,
// every block a transparent one (colours 0000 0000, all texels index 3; the report of 16:37). Such a texture gets the
// name written into it: white letters (5x7 font, 4 texels a dot) with a black edge, the rest transparent.
static const unsigned char kFont[][8] = {
    // letter, then its 7 rows (5 bits each, the leftmost the highest)
    {'L', 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, {'e', 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    {'o', 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, {'n', 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11},
    {'A', 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {'s', 0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E},
    {'h', 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}, {'l', 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {'y', 0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},
};

static bool blankCmpr(const unsigned char* px, int bytes)
{
    for (int k = 0; k < bytes; k += 8) {
        if (px[k] | px[k + 1] | px[k + 2] | px[k + 3] || (px[k + 4] & px[k + 5] & px[k + 6] & px[k + 7]) != 0xFF) {
            return false;
        }
    }
    return true;
}

static void writeName(unsigned char* px, int w, int h, const char* name)
{
    // the picture: 1 letter, 2 edge, 0 nothing
    static unsigned char pic[48 * 160];
    if (w > 160 || h > 48) {
        return;
    }
    memset(pic, 0, sizeof pic);
    const int dot = 4, n = (int) strlen(name), tw = n * 6 * dot - dot, th = 7 * dot;
    const int x0 = (w - tw) / 2, y0 = (h - th) / 2;
    for (int c = 0; c < n; c++) {
        const unsigned char* g = 0;
        for (const auto& f : kFont) {
            if (f[0] == (unsigned char) name[c]) {
                g = f + 1;
            }
        }
        for (int r = 0; g && r < 7; r++) {
            for (int b = 0; b < 5; b++) {
                if (!(g[r] & (0x10 >> b))) {
                    continue;
                }
                for (int yy = 0; yy < dot; yy++) {
                    for (int xx = 0; xx < dot; xx++) {
                        const int x = x0 + (c * 6 + b) * dot + xx, y = y0 + r * dot + yy;
                        if (x >= 0 && x < w && y >= 0 && y < h) {
                            pic[y * w + x] = 1;
                        }
                    }
                }
            }
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (pic[y * w + x] == 1) {
                continue;
            }
            for (int dy = -2; dy <= 2 && pic[y * w + x] == 0; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && xx < w && yy >= 0 && yy < h && pic[yy * w + xx] == 1) {
                        pic[y * w + x] = 2;
                        break;
                    }
                }
            }
        }
    }
    // CMPR: 8x8 tiles row by row, each four 4x4 blocks (top left, top right, bottom left, bottom right); a block: colour
    // 0 = black 0x0000, colour 1 = white 0xFFFF (0 <= 1: three colours and transparent), 2 bits a texel, index 0 black,
    // 1 white, 3 transparent
    unsigned char* o = px;
    for (int ty = 0; ty < h; ty += 8) {
        for (int tx = 0; tx < w; tx += 8) {
            for (int sb = 0; sb < 4; sb++) {
                const int bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4;
                o[0] = 0x00, o[1] = 0x00, o[2] = 0xFF, o[3] = 0xFF;
                for (int r = 0; r < 4; r++) {
                    unsigned char bits = 0;
                    for (int c = 0; c < 4; c++) {
                        const int v = pic[(by + r) * w + bx + c];
                        bits = (unsigned char) (bits << 2 | (v == 1 ? 1 : v == 2 ? 0 : 3));
                    }
                    o[4 + r] = bits;
                }
                o += 8;
            }
        }
    }
}

static void fillNames(ID_UNIT** u, int n)
{
    for (int i = 0; i < n; i++) {
        const ID_UNIT* a = u[i];
        GXTexObj* tex = 0;
        if (!a || (a->texId != 0x39 && a->texId != 0x40) || !g_pIdTexSys ||
            !g_pIdTexSys->GetTexObj(a->texId, a->texNo, &tex) || !tex || GXGetTexObjFmt(tex) != 14) {
            continue;
        }
        const int w = GXGetTexObjWidth(tex), h = GXGetTexObjHeight(tex);
        unsigned char* px = (unsigned char*) re4_gx_cached((unsigned) (uintptr_t) GXGetTexObjData(tex));
        if (!px || w % 8 || h % 8 || !blankCmpr(px, w * h / 2)) {
            continue;
        }
        writeName(px, w, h, a->texId == 0x39 ? "Leon" : "Ashley");
        re4_log("case window: texture %02x (%dx%d) was empty: the name %s written into it", a->texId, w, h,
                a->texId == 0x39 ? "Leon" : "Ashley");
    }
}

// Patch 0912 (DevJunior, 25.11.2026: the case's "use on Leon / Ashley" window is an empty dark box, "make it
// work"): its 11 units to the log, the first frame of each opening and 20 frames later (when its fade has run), to
// see why the two names do not show (no texture error in the reports).
void re4_case_window_dump(ID_UNIT** u, int n, int cmd)
{
    static int last = -100, open = 0, dumps = 0;
    if (u) {
        fillNames(u, n);
    }
    const int now = pG ? (int) pG->Frame_cnt : 0;
    if (now - last > 2) {
        open = now;
    }
    last = now;
    if ((now != open && now != open + 20) || dumps >= 8 || !u) {
        return;
    }
    dumps++;
    re4_log("case window: command %d, frame %d of it", cmd, now - open);
    for (int i = 0; i < n; i++) {
        const ID_UNIT* a = u[i];
        if (!a) {
            continue;
        }
        re4_log("case window: %2d be %02x unit %02x class %02x type %d parent %02x tex %02x/%d ptn %d mask %02x "
                "texfl %02x pos %.0f,%.0f,%.0f size %.0fx%.0f col0 %02x%02x%02x%02x col1 %02x%02x%02x%02x "
                "col %.2f,%.2f,%.2f,%.2f blend %d trans %d pow %d ot %d/%d rev %02x anima %02x vtx %02x t %d,%d,%d,%d",
                i, a->be_flag, a->unitNo, a->classNo, a->type, a->parentNo, a->texId, a->texNo, a->tex_ptn_no,
                a->maskId, a->tex_flag, a->pos.x, a->pos.y, a->pos.z, a->size_W, a->size_H, a->col0[0], a->col0[1],
                a->col0[2], a->col0[3], a->col1[0], a->col1[1], a->col1[2], a->col1[3], a->col[0], a->col[1],
                a->col[2], a->col[3], a->blend_type, a->trans_type, a->pow, a->otType, a->otNo, a->rev_flag,
                a->anima_state, a->vtxType, a->timer[0], a->timer[1], a->timer[2], a->timer[3]);
        // its texture (25.11.2026: the names' units 5 and 7 are there, their textures 0x39 / 0x40 found, yet nothing
        // shows): format, size, palette, and how much of the image is not zero
        GXTexObj* tex = 0;
        if (a->texId != 0xFF && g_pIdTexSys && g_pIdTexSys->GetTexObj(a->texId, a->texNo, &tex) && tex) {
            const int fmt = GXGetTexObjFmt(tex);
            const int w = GXGetTexObjWidth(tex), h = GXGetTexObjHeight(tex);
            const int bits = fmt == 0 || fmt == 8 || fmt == 14 ? 4 : fmt == 1 || fmt == 2 || fmt == 9 ? 8 :
                             fmt == 6 ? 32 : 16;
            const unsigned phys = (unsigned) (uintptr_t) GXGetTexObjData(tex);
            const unsigned char* px = (const unsigned char*) re4_gx_cached(phys);
            const int bytes = w * h * bits / 8;
            int nonzero = 0, ff = 0;
            for (int k = 0; px && k < bytes && k < 65536; k++) {
                nonzero += px[k] != 0;
                ff += px[k] == 0xFF;
            }
            GXTlutObj* tl = 0;
            const int tlut = g_pIdTexSys->GetTlutObj(a->texId, &tl) && tl ? GXGetTlutObjFmt(tl) : -1;
            re4_log("case window: %2d texture %02x: format %d, %dx%d, palette format %d, at %08x, %d of %d bytes not "
                    "zero, %d 0xff", i, a->texId, fmt, w, h, tlut, phys, nonzero, bytes, ff);
        }
    }
}

// Patch 0912: 1 = "Equip" on p asks who takes it (Leon or Ashley): the switch on, Leon's game, Ashley the partner
// (id 3, as the case's "use on" choice asks), p a handgun or a magnum.
int re4_ashley_gun_menu(cItem* p)
{
    return p && (p->flags & 1) && gOn.load(std::memory_order_relaxed) && pG && pG->pl_type == 0 && pSUB &&
           ((cModel*) pSUB)->id == 3 && isPistol(p->id);
}

// Patch 0912: Ashley takes p out of Leon's case; a pistol she had goes back to Leon, Leon's own put away when it was
// p. 1 done.
int re4_ashley_gun_give(cItem* p)
{
    if (!p || !(p->flags & 1) || p->type != 0 || !isPistol(p->id)) {
        return 0;
    }
    cItem* had = givenItem();
    if (had) {
        giveBack(had, "another one given");
    }
    if (ItemMgr.weapon() == p) {
        ItemMgr.arm(0);
    }
    p->type = kAway;
    gAmmo = kMag;
    gRegenT = 0;
    re4_log("ashley: she takes pistol 0x%02x", (unsigned) p->id);
    return 1;
}

// The app (the second aim word's bit 29, runtime/aim_options.cpp; any thread): Ashley's pistol on or off.
void re4_ashley_gun_set(int on)
{
    if (gOn.exchange(on != 0) != (on != 0)) {
        re4_log("ashley: her pistol %s", on ? "on" : "off");
    }
}

// For the runtime test (patch 0912's give and take on a case of three items, the game's own swapped out
// meanwhile): a bit per failed check, 0 = all passed.
int re4_ashley_gun_selftest(void)
{
    cItem items[3];
    memset(items, 0, sizeof items);
    items[0].flags = 1, items[0].id = 0x23, items[0].board = 1, items[0].x = 4;  // the handgun
    items[1].flags = 1, items[1].id = 0x25, items[1].board = 1;                   // the Red9
    items[2].flags = 1, items[2].id = 0x2C, items[2].board = 1;                   // the shotgun
    cItem* const keep = ItemMgr.m_pItem;
    const int keepN = ItemMgr.m_array_num;
    const bool keepOn = gOn.load();
    ItemMgr.m_pItem = items;
    ItemMgr.m_array_num = 3;
    gOn.store(true);
    int bad = 0;
    if (givenItem() || armed()) bad |= 1;
    if (re4_ashley_gun_give(&items[2]) != 0 || givenItem()) bad |= 2;          // not a pistol
    if (re4_ashley_gun_give(&items[0]) != 1 || givenItem() != &items[0] || !armed() || items[0].isAlive(0)) bad |= 4;
    if (re4_ashley_gun_give(&items[1]) != 1 || givenItem() != &items[1] || !items[0].isAlive(0) || items[0].x != 4)
        bad |= 8;                                                               // the first one back at its place
    if (re4_ashley_gun_give(&items[1]) != 0) bad |= 0x10;                        // already hers
    giveBack(&items[1], "test");
    if (givenItem() || !items[1].isAlive(0)) bad |= 0x20;
    gOn.store(false);
    if (armed()) bad |= 0x40;
    ItemMgr.m_pItem = keep;
    ItemMgr.m_array_num = keepN;
    gOn.store(keepOn);
    return bad;
}

// For the runtime test: rounds left after `frames` frames without a shot, from `left`.
int re4_ashley_ammo_after(int left, int frames)
{
    int ammo = left, t = 0;
    for (int i = 0; i < frames; i++) {
        if (ammo < kMag && ++t >= kRegen) {
            t = 0;
            ammo++;
        }
    }
    return ammo;
}

}  // extern "C"
