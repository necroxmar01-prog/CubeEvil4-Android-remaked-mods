// Ashley's pistol (wish of DevJunior, 11.11.2026: "Ashley self-defence: she can use a pistol as the partner ... she
// follows us and backs us up"; his choices: a switch in the Mods page, the arm aimed by code, she shoots while she
// follows and while she waits, ammunition that comes back with time). Stage 1: with the app's switch on she carries
// a pistol as the partner; giving it from the case and taking it back, and Ashley's own chapter, come later. Compiled with the game's flags into libre4_main
// (re4_gamehooks_obj); the switch arrives with the second aim word (bit 29, runtime/aim_options.cpp).
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
//  - Ammunition: kMag rounds; one comes back every kRegen frames.
//  - The pistol in her hand: the handgun's model (em/wep02.drs, entries 6 / 5 as wep02's objects take them) read
//    once to kGunAddr (Krauser's weapon buffer of cheats.cpp: only his own game uses it, and only Leon's story has
//    her as the partner here), an event body (SetObj18) hung on her hand part (OyaSetObj18), hidden in events,
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
#include <dolphin/dvd.h>

#include <atomic>
#include <math.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" void re4_formats_part_loaded(const char* name, int nest, int index, const void* part, unsigned addr);

namespace {

const u32 kGunAddr = 0x82200000u;  // cheats.cpp kWepPlaceAddr (Krauser's own game only)
const u32 kGunMax = 0x100000u;
const char* const kGunFile = "em/wep02.drs";
const int kGunBin = 6, kGunTpl = 5;

const f32 kRange = 12000.0f;   // 12 m
const f32 kCone = 1.4f;        // radians either side of where she faces, while she walks
const f32 kTurn = 0.12f;       // radians a frame she turns standing
const int kMag = 10;
const int kRegen = 90;         // frames for one round (3 s)
const int kFireGap = 14;       // frames between shots
const int kAimIn = 6;          // frames to raise the arm
const f32 kPi = 3.14159265f;

std::atomic<bool> gOn(false);
std::atomic<bool> gHooked(false);

int gAmmo = kMag;
int gRegenT = 0;
int gGap = 0;
f32 gW = 0.0f;           // the arm's share of the aim, 0..1
cEm* gTarget = 0;
cSubChar* gAshley = 0;   // she, as cSubChar::move last ran (frame gSeen)
u32 gSeen = 0;
cSubChar* gArmOf = 0;    // whose arm gUpper / gFore / gHand are
int gUpper = -1, gFore = -1, gHand = -1;
f32 gArmLen = 0.0f;
int gShots = 0;

bool gLoaded = false;
bool gLoadFailed = false;
cObj* gObj = 0;
void* gObjModel = 0;
cSubChar* gCarrier = 0;
cObj* gOld = 0;
void* gOldModel = 0;
int gOldWait = 0;

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

// Her right arm by her bind pose (once per Ashley)
void findArm(cSubChar* a)
{
    gArmOf = a;
    gUpper = gFore = gHand = -1;
    f32 side = -1.0f;  // Leon's right is -x (runtime/wardrobe.cpp)
    if (pPL && pPL->pList) {
        cParts* h = pPL->getPartsPtr(10);
        if (h) {
            side = bindPos(h).x < 0.0f ? -1.0f : 1.0f;
        }
    }
    int n = 0, far = -1;
    f32 best = 0.0f;
    for (cParts* p = a->pList; p && n < 200; p = p->pList, n++) {
        const f32 x = bindPos(p).x * side;
        if (x > best) {
            best = x;
            far = n;
        }
    }
    if (far < 0) {
        re4_log("ashley: %d parts, no arm found: no pistol", n);
        return;
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
        re4_log("ashley: %d parts, the arm chain from part %d is too short: no pistol", n, far);
        return;
    }
    gUpper = chain[u];
    gFore = chain[u - 1];
    gHand = chain[u - 2];
    const Vec pu = bindPos(a->getPartsPtr(gUpper));
    const Vec ph = bindPos(a->getPartsPtr(gHand));
    gArmLen = sqrtf((ph.x - pu.x) * (ph.x - pu.x) + (ph.y - pu.y) * (ph.y - pu.y) + (ph.z - pu.z) * (ph.z - pu.z));
    re4_log("ashley: %d parts; right arm: upper %d (%.0f %.0f %.0f), forearm %d, hand %d (%.0f %.0f %.0f), "
            "farthest part %d", n, gUpper, pu.x, pu.y, pu.z, gFore, gHand, ph.x, ph.y, ph.z, far);
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

bool valid(cSubChar* a, cEm* em, const Vec& eye, bool turn, f32* dist)
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
    if (!turn && fabsf(wrap(atan2f(dx, dz) - a->ang.y)) > kCone) {
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

cEm* pickTarget(cSubChar* a, const Vec& eye, bool turn)
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

void aimArm(cSubChar* a, const Vec& target)
{
    cParts* u = a->getPartsPtr(gUpper);
    cParts* f = a->getPartsPtr(gFore);
    cParts* h = a->getPartsPtr(gHand);
    if (!u || !f || !h || !u->pParent) {
        return;
    }
    const Vec pu = bindPos(u), ph = bindPos(h);
    Vec rest = {ph.x - pu.x, ph.y - pu.y, ph.z - pu.z};
    const f32 rl = sqrtf(rest.x * rest.x + rest.y * rest.y + rest.z * rest.z);
    if (rl < 1.0f) {
        return;
    }
    rest.x /= rl;
    rest.y /= rl;
    rest.z /= rl;
    Vec d = {target.x - u->world.x, target.y - u->world.y, target.z - u->world.z};
    const f32 dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dl < 1.0f) {
        return;
    }
    d.x /= dl;
    d.y /= dl;
    d.z /= dl;
    // into the parent's frame (its world rotation transposed; the model's scale is uniform)
    const cCoord* par = u->pParent;
    Vec v = {par->mat[0][0] * d.x + par->mat[1][0] * d.y + par->mat[2][0] * d.z,
             par->mat[0][1] * d.x + par->mat[1][1] * d.y + par->mat[2][1] * d.z,
             par->mat[0][2] * d.x + par->mat[1][2] * d.y + par->mat[2][2] * d.z};
    const f32 vl = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (vl < 1e-6f) {
        return;
    }
    v.x /= vl;
    v.y /= vl;
    v.z /= vl;
    Quaternion q, none = {0.0f, 0.0f, 0.0f, 1.0f};
    quatFromTo(rest, v, &q);
    turnLocal(u, q, gW);
    turnLocal(f, none, gW);  // the elbow straight
    turnLocal(h, none, gW);  // the hand in line
}

void fire(cSubChar* a, cEm* em)
{
    Vec tp;
    aimPoint(em, &tp);
    cParts* u = a->getPartsPtr(gUpper);
    Vec d = {tp.x - u->world.x, tp.y - u->world.y, tp.z - u->world.z};
    const f32 dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dl < 1.0f) {
        return;
    }
    const f32 reach = gArmLen + 150.0f;
    Vec muzzle = {u->world.x + d.x / dl * reach, u->world.y + d.y / dl * reach, u->world.z + d.z / dl * reach};
    const s16 hp = a->hp;
    a->hp = 0;
    const u32 hits = PlWepHitCheck2(0, &muzzle, &tp, 3, 2, 6000.0f);
    a->hp = hp;
    SndCall(2, 0, &muzzle, 0, 0, 0);
    gAmmo--;
    gShots++;
    if (gShots <= 20 || gShots % 50 == 0) {
        re4_log("ashley: shot %d at s%d (id 0x%x, hp %d) from %.1f m, %u hit, %d rounds left", gShots,
                slotOf(em), (unsigned) em->id, (int) em->hp, dl / 1000.0f, (unsigned) hits, gAmmo);
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
    cSubChar* a = gAshley && pG && pG->Frame_cnt - gSeen <= 2 && pSUB == (cEm*) gAshley ? gAshley : 0;
    if (!gOn.load(std::memory_order_relaxed) || !a || pG->pl_type != 0 || gHand < 0 || gArmOf != a) {
        if (gObj) {
            dropGun(!gOn.load() ? "switched off" : "no Ashley");
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
    const PlArc* arc = (const PlArc*) (unsigned long) kGunAddr;
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
    if (!gOn.load(std::memory_order_relaxed)) {
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
    f32 d;
    if (!gTarget || !valid(a, gTarget, eye, standing, &d)) {
        cEm* t = pickTarget(a, eye, standing);
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
        fire(a, gTarget);
        gGap = kFireGap;
    }
}

// The app (the second aim word's bit 29, runtime/aim_options.cpp; any thread): Ashley's pistol on or off.
void re4_ashley_gun_set(int on)
{
    if (gOn.exchange(on != 0) != (on != 0)) {
        re4_log("ashley: her pistol %s", on ? "on" : "off");
    }
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
