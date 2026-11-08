// Free movement and moving while aiming (the app menu's Controls page, "Free movement" and "Move while aiming";
// patch 0219, a mod in the spirit of the 2023 remake). Compiled with the game's flags into libre4_main. Both off
// (the default): nothing here changes the game.
//
//  - "Free movement": the main stick moves Leon where it points, seen from the camera, instead of the tank
//    controls (up walks, left / right turn, down steps back). The C stick turns the shoulder camera round him
//    (its X; its Y keeps the game's look up / down), so the camera no longer sits fixed behind his back.
//    re4_player_input (cPlayer::keyConfig, before the game reads the keys) turns Leon toward the stick's
//    direction and leaves the game only "forward" (Key bit 0): the game's own walk, run and their motions do the
//    rest, the run key (or "Always run", patch 0156) still runs. Readying a weapon (or the knife) turns Leon at
//    once to where the camera looks, so he aims there. Only in the walk routines (r_no_1 0..4: footwork, walk,
//    back step, turn, run); everywhere else (aiming, ladders, actions, damage, events) the camera goes back
//    behind his back, smoothly, and exactly while aiming. The D-pad keeps the tank controls. The quick turn
//    (back + B) needs the D-pad then: with the stick, back is just a direction.
//    re4_free_cam_matrix (CameraQuasiFPS::calcBaseMatrix) builds the camera's base frame from that yaw
//    instead of Leon's.
//    The right stick pressed (R3, CoreBridge.recenterCamera: a toggling bit of the aim bits) turns the camera
//    back behind him, where his body faces, in a few frames (re4_camera_recenter).
//  - "Move while aiming": while a weapon or the knife is ready (r_no_1 6 / 0xB, cPlayer::isKamae) the C stick
//    walks Leon forward, back and sideways, relative to where he aims, at kAimWalk. The game has no strafe
//    motions; re4_player_after_routine (cPlayer::move, after the routines posed him) plays his own walk motion
//    (the back step when he moves back) on his legs only: the parts below the hips' IK chains of that motion
//    take its pose, faded in and out over a few frames, the hips and everything above keep the aiming pose. The
//    walk motion is evaluated in a work of its own (gWalk) with its own key histories, so the aiming motion and
//    its blend (cMot3, pMotionB) are not disturbed. The game's own collision (EmAtCheck, SatMgr: walls, then the
//    floor) runs after the routines, so walls and floors hold him as when he walks.
//    With "Alternate Controls" the app sends the left stick as the C stick while L1 / L2 hold the weapon ready
//    (CoreInput), so the left stick moves and the right one aims, as in the remake.
//
// For tests on the PC: RE4_TEST_AIM with bits 0x10 / 0x20 (runtime/aim_options.cpp).
#include "types.h"
#include "global.h"
#include "main.h"
#include "player.h"
#include "pl_wep.h"
#include "model.h"
#include "motion.h"

#include <atomic>
#include <math.h>
#include <string.h>

extern cPlayer* pPL;
extern "C" int re4_free_move_on(void);  // runtime/aim_options.cpp
extern "C" int re4_aim_move_on(void);
extern "C" void re4_quick_slots_input(cPlayer* pl);  // runtime/quick_slots.cpp

namespace {

const f32 kPi = 3.14159265f;
const f32 kMainRange = 56.0f;     // the main stick's reach after PADClamp
const f32 kCRange = 59.0f;        // the C stick's (CameraQuasiFPS::calcDepressionRatio's C_RANGE)
const f32 kMoveDead = 0.28f;      // below this share of the main stick Leon stands (the game's own: 30 of 56)
const f32 kOrbitDead = 0.2f;
const f32 kOrbitSpeed = 0.085f;   // the camera's turn per game frame (30 a second) at full C stick, ~146 degrees/s
const f32 kTurnStand = 0.42f;     // Leon's turn per game frame toward the stick: standing,
const f32 kTurnWalk = 0.26f;      // walking,
const f32 kTurnRun = 0.18f;       // running (the game's tank turn: SPEED_WALK_TURN 0.042)
const f32 kWalkStart = 1.05f;     // from standing he sets off once within 60 degrees of the stick's direction
const f32 kFollow = 0.12f;        // share per frame the camera goes back behind him outside the walk routines
const f32 kAimWalk = 38.0f;       // units per game frame at full C stick while aiming (0.99.2-mods: 26, too slow)
const f32 kLegFade = 0.25f;       // the legs' share of the walk pose per frame, in and out
const f32 kLegSpeed = 1.0f;       // the walk motion's frames per game frame at full C stick
const f32 kRecenter = 0.35f;      // share per frame the camera turns back behind him after R3
const int kMaxParts = 160;
const f32 kTeleport = 1500.0f;    // a jump of his position this far (a door, a new room, an event): camera reset

f32 gCamYaw = 0.0f;   // the free camera's yaw (the game's convention: forward (sin, cos), right = a smaller angle)
bool gCamValid = false;
bool gCamOn = false;  // re4_free_cam_matrix builds the camera from gCamYaw this frame
cPlayer* gLastPl = 0;
Vec gLastPos = {0.0f, 0.0f, 0.0f};
std::atomic<bool> gRecenterAsked(false);  // R3 pressed (any thread)
bool gRecentering = false;

// this frame's move while aiming (aimMove), for the legs (re4_player_after_routine)
bool gAimMoving = false;
f32 gAimSx = 0.0f;
f32 gAimSy = 0.0f;
f32 gAimMag = 0.0f;

// the walk motion on the legs
struct PartSave {
    Vec pos;
    Vec ang;
    Vec scale;
    MotionParts mp;
};
PartSave gSave[kMaxParts];
HERMITE_KEY gWalkHist[kMaxParts][6];
bool gLeg[kMaxParts];
MOTION_INFO gWalk;
void* gWalkMot = 0;
cPlayer* gWalkPl = 0;
f32 gLegW = 0.0f;

f32 wrap(f32 a)
{
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

f32 axis(s8 v, f32 range)
{
    const f32 f = (f32) v / range;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

// The walk routines of Pl_func_move_tbl (player.cpp): footwork, walk, back step, turn, run.
bool walkRoutine(cPlayer* pl)
{
    return pl->r_no_0 == 0 && pl->r_no_1 <= 4 && !pl->stat.check(cPlayer::F_EVENT)
        && !pl->stat.check(cPlayer::F_BINOCULAR);
}

void freeMove(cPlayer* pl)
{
    const f32 dx = pl->pos.x - gLastPos.x;
    const f32 dz = pl->pos.z - gLastPos.z;
    if (!gCamValid || pl != gLastPl || dx * dx + dz * dz > kTeleport * kTeleport) {
        gCamYaw = pl->ang.y;
        gCamValid = true;
    }
    if (!walkRoutine(pl)) {
        gRecentering = false;
        gRecenterAsked = false;
        if (pl->isKamae()) {
            gCamYaw = pl->ang.y;  // aiming: exactly behind him, as the game
            gCamOn = false;
        } else {
            gCamYaw = wrap(gCamYaw + wrap(pl->ang.y - gCamYaw) * kFollow);
            gCamOn = fabsf(wrap(pl->ang.y - gCamYaw)) > 0.001f;
        }
        return;
    }
    gCamOn = true;
    // the C stick's X turns the camera (its Y stays the game's look up / down)
    const f32 cx = axis(Key.substickX, kCRange);
    if (gRecenterAsked.exchange(false)) {
        gRecentering = true;
    }
    if (fabsf(cx) > kOrbitDead) {
        gCamYaw = wrap(gCamYaw - cx * kOrbitSpeed);
        gRecentering = false;  // turning it by hand ends the way back
    } else if (gRecentering) {
        const f32 d = wrap(pl->ang.y - gCamYaw);
        gCamYaw = wrap(gCamYaw + d * kRecenter);
        if (fabsf(d) < 0.01f) {
            gCamYaw = pl->ang.y;
            gRecentering = false;
        }
    }
    Key.substickX = 0;
    // (joyKamae asks the weapon; without one, between rooms, it would only log an error)
    if ((pl->Wep && pl->Wep->m_pWep && joyKamae()) || joyLKamae()) {
        pl->ang.y = gCamYaw;  // a weapon or the knife ready: he aims where the camera looks
        return;
    }
    const f32 sx = axis(Key.stickX, kMainRange);
    const f32 sy = axis(Key.stickY, kMainRange);
    if (sx * sx + sy * sy <= kMoveDead * kMoveDead) {
        return;  // the stick rests: the D-pad's tank controls as the game
    }
    const f32 target = wrap(gCamYaw - atan2f(sx, sy));
    const f32 diff = wrap(target - pl->ang.y);
    const bool moving = pl->r_no_1 == 1 || pl->r_no_1 == 3;
    const f32 rate = pl->r_no_1 == 3 ? kTurnRun : moving ? kTurnWalk : kTurnStand;
    const f32 step = diff > rate ? rate : diff < -rate ? -rate : diff;
    pl->ang.y = wrap(pl->ang.y + step);
    // only "forward" for the game: back, left and right (and with them the quick turn of keyConfigTypeA) stay off
    Key.on &= ~(u64) 0xF;
    Key.trg &= ~(u64) 0xF;
    if (moving || fabsf(diff - step) < kWalkStart) {
        Key.on |= 1;
    }
}

void aimMove(cPlayer* pl)
{
    if (pl->r_no_0 != 0 || (pl->r_no_1 != 6 && pl->r_no_1 != 0xB) || !pl->isKamae()) {
        return;
    }
    f32 sx = axis(Key.substickX, kCRange);
    f32 sy = axis(Key.substickY, kCRange);
    Key.substickX = 0;
    Key.substickY = 0;
    const f32 mag = sqrtf(sx * sx + sy * sy);
    if (mag <= kMoveDead) {
        return;
    }
    if (mag > 1.0f) {
        sx /= mag;
        sy /= mag;
    }
    gAimMoving = true;
    gAimSx = sx;
    gAimSy = sy;
    gAimMag = mag > 1.0f ? 1.0f : mag;
    const f32 s = sinf(pl->ang.y);
    const f32 c = cosf(pl->ang.y);
    const f32 fwd = sy * kAimWalk;
    const f32 side = sx * kAimWalk;
    // forward (sin, cos), right (-cos, sin)
    pl->pos.x += s * fwd - c * side;
    pl->pos.z += c * fwd + s * side;
}

bool weaponRoutine(cPlayer* pl)
{
    return pl->r_no_0 == 0 && (pl->r_no_1 == 6 || pl->r_no_1 == 0xB);
}

// The legs: the parts below the hips that hang off the walk motion's IK chain roots (kind 0x30, IKInit's;
// ik.cpp), or, without them, the hips' children below the hips. gLeg[i] by the parts chain's order.
void findLegs(cPlayer* pl, int n)
{
    cParts* roots[8];
    int nRoots = 0;
    cParts* hips = pl->pList;
    for (int i = 0; i < gWalk.Joint_num && nRoots < 8; i++) {
        if (!(gWalk.pJoint_kind[i] & 0x30)) {
            continue;
        }
        cParts* r = pl->getPartsPtr(gWalk.pJoint_no[i]);
        if (r && r != hips && r->world.y < hips->world.y + 100.0f) {
            roots[nRoots++] = r;
        }
    }
    if (nRoots == 0) {
        for (cParts* p = hips->pList; p && nRoots < 8; p = p->pList) {
            if (p->pParent == hips && p->world.y < hips->world.y - 50.0f) {
                roots[nRoots++] = p;
            }
        }
    }
    int i = 0;
    for (cParts* p = hips; p && i < n; p = p->pList, i++) {
        gLeg[i] = false;
        cCoord* c = p;
        for (int depth = 0; depth < 40 && c && c != (cCoord*) pl; depth++, c = c->pParent) {
            for (int r = 0; r < nRoots; r++) {
                if (c == roots[r]) {
                    gLeg[i] = true;
                }
            }
            if (gLeg[i]) {
                break;
            }
        }
    }
}

void legs(cPlayer* pl)
{
    const bool want = gAimMoving && weaponRoutine(pl) && pl->isKamae();
    gLegW = want ? (gLegW + kLegFade > 1.0f ? 1.0f : gLegW + kLegFade) : (gLegW - kLegFade < 0.0f ? 0.0f : gLegW - kLegFade);
    if (gLegW <= 0.0f || !weaponRoutine(pl) || !pl->pList || !pl->m_MotTbl) {
        gLegW = 0.0f;
        gWalkMot = 0;
        return;
    }
    // the walk motion, the back step when he moves back (as the routines choose them: dmMotCk, player.cpp)
    const bool back = gAimSy < -0.3f && -gAimSy > fabsf(gAimSx);
    void** tbl = pl->m_MotTbl;
    const bool own = dmMotCk() != 0;
    void* mot = own ? tbl[back ? 8 : 2] : tbl[back ? 0x63 : 0x61];
    void* seq = own ? tbl[back ? 9 : 3] : tbl[back ? 0x64 : 0x62];
    if (!mot) {
        gLegW = 0.0f;
        return;
    }
    int n = 0;
    for (cParts* p = pl->pList; p; p = p->pList) {
        if (++n > kMaxParts) {
            gLegW = 0.0f;
            return;
        }
    }
    int i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        gSave[i].pos = p->pos;
        gSave[i].ang = p->ang;
        gSave[i].scale = p->scale;
        gSave[i].mp = p->motParts;
    }
    if (mot != gWalkMot || pl != gWalkPl) {
        memset(&gWalk, 0, sizeof gWalk);
        // keep the model's blend motion (cMot3's pitch blend), no IKInit on the model
        gWalk.Mot_flag = 0x20000000 | 0x10000000;
        MotionSetCore(pl, &gWalk, mot, seq, 0, 4, 0);
        gWalk.Mot_flag |= 0x40000000;  // MotionMoveCore: leave the model's matrix alone
        memset(gWalkHist, 0, sizeof gWalkHist);
        gWalkMot = mot;
        gWalkPl = pl;
        findLegs(pl, n);
    }
    i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        memcpy(p->motParts.hist, gWalkHist[i], sizeof gWalkHist[i]);
    }
    gWalk.Seq_speed = kLegSpeed * (gAimMag > 0.4f ? gAimMag : 0.4f);
    gWalk.Hokan_cnt = 0;
    MotionMoveCore(pl, &gWalk, 0);
    MotionSequenceCtrl(&gWalk);
    const f32 w = gLegW;
    i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        const Vec wpos = p->pos;
        const Vec wang = p->ang;
        const Vec wscale = p->scale;
        memcpy(gWalkHist[i], p->motParts.hist, sizeof gWalkHist[i]);
        p->pos = gSave[i].pos;
        p->ang = gSave[i].ang;
        p->scale = gSave[i].scale;
        p->motParts = gSave[i].mp;
        if (!gLeg[i]) {
            continue;
        }
        p->ang.x = wrap(p->ang.x + wrap(wang.x - p->ang.x) * w);
        p->ang.y = wrap(p->ang.y + wrap(wang.y - p->ang.y) * w);
        p->ang.z = wrap(p->ang.z + wrap(wang.z - p->ang.z) * w);
        p->pos.x += (wpos.x - p->pos.x) * w;
        p->pos.y += (wpos.y - p->pos.y) * w;
        p->pos.z += (wpos.z - p->pos.z) * w;
        p->scale.x += (wscale.x - p->scale.x) * w;
        p->scale.y += (wscale.y - p->scale.y) * w;
        p->scale.z += (wscale.z - p->scale.z) * w;
        // the local matrix as partsMatCalc builds it; partsWorldCalc (cPlayer::move) takes it from there
        RotMatrix(p->l_mat, &p->ang);
        TransMatrix(p->l_mat, &p->pos);
        ScaleMatrix(p->l_mat, &p->scale);
    }
}

}  // namespace

extern "C" {

// Patch 0219 (cPlayer::move, right after the routines, the game thread): the walk motion on the legs while he
// moves aiming.
void re4_player_after_routine(cPlayer* pl)
{
    if (!pl || !re4_aim_move_on()) {
        gLegW = 0.0f;
        gWalkMot = 0;
        return;
    }
    legs(pl);
}

// The app (R3, the aim bits' toggling bit 6 through re4_aim_set; any thread): the free camera back behind Leon.
void re4_camera_recenter(void)
{
    gRecenterAsked = true;
}

// Patch 0219 (cPlayer::keyConfig, the game thread, every frame of Leon's move before the routines read the keys).
void re4_player_input(cPlayer* pl)
{
    gCamOn = false;
    gAimMoving = false;
    if (!pl) {
        return;
    }
    re4_quick_slots_input(pl);  // first: the D-pad's bits out of the keys when it equips weapons
    if (re4_free_move_on()) {
        freeMove(pl);
    } else {
        gCamValid = false;
    }
    if (re4_aim_move_on()) {
        aimMove(pl);
    }
    gLastPl = pl;
    gLastPos = pl->pos;
}

// Patch 0219 (CameraQuasiFPS::calcBaseMatrix, after it took Leon's matrix): the camera's frame turned to the free
// camera's yaw, at his position.
void re4_free_cam_matrix(Mtx mat)
{
    if (!gCamOn || !re4_free_move_on()) {
        return;
    }
    Vec t;
    Vec r;
    t.x = mat[0][3];
    t.y = mat[1][3];
    t.z = mat[2][3];
    r.x = 0.0f;
    r.y = gCamYaw;
    r.z = 0.0f;
    RotMatrix(mat, &r);
    TransMatrix(mat, &t);
}

}  // extern "C"
