// Free movement and moving while aiming (the app menu's Controls page, "Free movement" and "Move while aiming";
// patch 0900, a mod in the spirit of the 2023 remake). Compiled with the game's flags into libre4_main. Both off
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
//    back behind him, where his body faces, in a few frames (re4_camera_recenter). While he walks or runs and the
//    right stick has rested for kFollowDelay frames, the camera follows his way by itself, as the remake's does
//    (wish of DevJunior, 08.11.2026): it turns toward his facing by a share of the difference (kFollowWalk /
//    kFollowRun, at most kFollowStep a frame), not when he walks toward the camera (beyond kFollowAngle).
//  - "Move while aiming": while a weapon or the knife is ready (r_no_1 6 / 0xB, cPlayer::isKamae) the C stick
//    walks Leon forward, back and sideways, relative to where he aims, at kAimWalk. The game has no strafe
//    motions; re4_player_after_routine (cPlayer::move, after the routines posed him) plays his own walk motion
//    (the back step when he moves back) on his legs only. The walk motion is posed the way MotionMove poses a
//    motion (MotionMoveCore, partsMatCalc, then the motion's leg IK: IKInit, InverseKinematics; its feet are IK
//    targets, not bone offsets, 0.99.4 fix of the stretched legs) on a copy of every part, which is put back
//    afterwards; only the legs' rotations are taken from it: the thighs as the walk pose turns them against the
//    model (not against the aiming pose's turned hips), the shins and feet as their local turn, faded in and out
//    by a quaternion blend (kLegFade, at most kLegMax); every bone keeps the aiming pose's offset. The hips take
//    the walk's sway (its bob and roll, device report 08.11.2026: the body stood stiff): the walk pose's hips
//    against their own running mean (gHipT, gHipQ), so only the swing is added, all of the bob, kSwayAim of the
//    turn while aiming (the gun steadier), all of it while reloading; everything above follows the hips. The walk motion runs in a work of its own (gWalk) with its own key
//    histories, so the aiming motion and its blend (cMot3, pMotionB) are not disturbed. The game's own collision (EmAtCheck, SatMgr: walls, then the
//    floor) runs after the routines, so walls and floors hold him as when he walks.
//    With "Alternate Controls" the app sends the left stick as the C stick while L1 / L2 hold the weapon ready
//    (CoreInput), so the left stick moves and the right one aims, as in the remake.
//  - Reloading without aiming (with "Move while aiming", wish of DevJunior, 08.11.2026, as in the remake): R
//    and B pressed together in the walk routines (the app's reload action or a mapped reload key press both at
//    once; aiming first and then B stays the game's reload), or the touch controls' RLOAD button (its own
//    request, re4_reload_request: running holds B already, so R + B would not be new together; also the app's
//    "Quick reload" key, L1 by default, and the fire key without aiming (R2 with the alternate controls, or a
//    mapping's fire key that is no action of its own), wish of DevJunior 08.11.2026, which work without "Move
//    while aiming" as well: Leon then stands while he reloads), start the
//    prototype's own direct reload: the weapon routine's reload step (setRno 0/6/4, m_Work0 1) with
//    PlReloadDirect = 1, as cPlayer::actionSelect does with the reload key type 1 (keyReload; isKamae is 0
//    then), so Leon does not aim; also while he runs. Meanwhile the main stick moves him at the speed he walks
//    or, while the run key stays held (or "Always run"), runs (gWalkSpeed / gRunSpeed: measured from his own
//    walk and run, 0.99.4-mods), his legs walking or running as above, seen from the camera with "Free
//    movement", else relative to where he faces; the right stick keeps turning the free camera. Started while he
//    walks or runs, his legs take the walk at once (no fade in), from the step he was in (his walk / run motion's
//    frame), and all of it (no aiming stance to keep; device report 11.11.2026: he still stopped for a moment).
//
// For tests on the PC: RE4_TEST_AIM with bits 0x10 / 0x20 (runtime/aim_options.cpp).
#include "types.h"
#include "global.h"
#include "main.h"
#include "joy.h"
#include "player.h"
#include "pl_wep.h"
#include "model.h"
#include "motion.h"
#include "item.h"

#include <atomic>
#include <math.h>
#include <string.h>

extern cPlayer* pPL;
extern "C" void re4_log(const char* fmt, ...);
extern "C" int re4_free_move_on(void);  // runtime/aim_options.cpp
extern "C" int re4_aim_move_on(void);
extern "C" void re4_quick_slots_input(cPlayer* pl);  // runtime/quick_slots.cpp
extern "C" int re4_run_held(int held);               // runtime/aim_options.cpp (patch 0156)
extern "C" void re4_parry_input(cPlayer* pl);         // runtime/parry.cpp
extern "C" int re4_ashley_gun_input(cPlayer* pl, int freeCam);  // runtime/ashley_gun.cpp (patch 0913)
extern "C" int re4_move_turn(void* pl, float diff, int armed);   // runtime/locomotion.cpp (the remake's turns)
extern "C" int re4_move_heavy_legs(void* pl);  // runtime/locomotion.cpp (the remake's legs under a big weapon)
extern "C" int re4_move_strafe(void* pl, float sx, float sy, float want, const void* cur, float curFrame, void** mot,
                               void** seq, int* flip, float* speed, int* start);  // runtime/locomotion.cpp
extern "C" int re4_quick_slots_on(void);       // runtime/aim_options.cpp

namespace {

const f32 kPi = 3.14159265f;
const f32 kMainRange = 56.0f;     // the main stick's reach after PADClamp
const f32 kCRange = 59.0f;        // the C stick's (CameraQuasiFPS::calcDepressionRatio's C_RANGE)
const f32 kMoveDead = 0.28f;      // below this share of the main stick Leon stands (the game's own: 30 of 56)
const int kQuickEnd = 4;          // frames a reload without aiming goes on after the weapon is full (patch 0909)
const f32 kOrbitDead = 0.2f;
const f32 kOrbitSpeed = 0.085f;   // the camera's turn per game frame (30 a second) at full C stick, ~146 degrees/s
const f32 kTurnStand = 0.42f;     // Leon's turn per game frame toward the stick: standing,
const f32 kTurnWalk = 0.26f;      // walking,
const f32 kTurnRun = 0.18f;       // running (the game's tank turn: SPEED_WALK_TURN 0.042)
const f32 kWalkStart = 1.05f;     // from standing he sets off once within 60 degrees of the stick's direction
const f32 kFollow = 0.12f;        // share per frame the camera goes back behind him outside the walk routines
const f32 kAimWalk = 38.0f;       // units per game frame at full C stick while aiming (0.99.2-mods: 26, too slow)
const f32 kLegFade = 0.2f;        // the legs' share of the walk pose per frame, in and out
const f32 kLegMax = 0.85f;        // at most this share (a little of the steady aiming stance stays)
const f32 kLegSpeed = 0.9f;       // the walk motion's frames per game frame at full C stick
const f32 kSwayAim = 0.5f;        // share of the walk's hip turn while aiming (all of it while reloading)
const f32 kSwayMean = 0.08f;      // the hips' running mean per frame (a walk step lasts about 15 frames)
const f32 kRecenter = 0.35f;      // share per frame the camera turns back behind him after R3
const int kFollowDelay = 15;      // frames the right stick rests before the camera follows his way (half a second)
const f32 kFollowWalk = 0.05f;    // share of the difference to his facing per frame, walking (0.025 until 12.11.2026)
const f32 kFollowRun = 0.09f;     // running (0.04)
const f32 kFollowStep = 0.08f;    // at most this turn per frame (about 140 degrees a second; 0.035, 60, until 12.11.2026)
const f32 kFollowAngle = 2.35f;   // walking: no following beyond this (135 degrees: he walks toward the camera);
                                  // running, the camera swings round behind him at any angle (12.11.2026: "when I run
                                  // backwards"), the way it started kept near 180 degrees (kFollowFlip)
const f32 kFollowFlip = 2.8f;
const int kMaxParts = 160;
const f32 kTeleport = 1500.0f;    // a jump of his position this far (a door, a new room, an event): camera reset
const f32 kWalkDefault = 42.0f;   // his walk and run per game frame until measured (gWalkSpeed, gRunSpeed)
const f32 kRunDefault = 95.0f;

f32 gCamYaw = 0.0f;   // the free camera's yaw (the game's convention: forward (sin, cos), right = a smaller angle)
bool gCamValid = false;
bool gCamOn = false;  // re4_free_cam_matrix builds the camera from gCamYaw this frame
cPlayer* gLastPl = 0;
Vec gLastPos = {0.0f, 0.0f, 0.0f};
std::atomic<bool> gRecenterAsked(false);  // R3 pressed (any thread)
bool gRecentering = false;
int gStickRest = 0;                       // frames since the right stick last turned the camera
int gFollowSign = 0;                      // the way the camera last followed (-1, +1; 0 none)
std::atomic<bool> gReloadAsked(false);    // the touch RLOAD button (any thread)
f32 gWalkSpeed = kWalkDefault;            // his walk and run as measured (units per game frame)
f32 gRunSpeed = kRunDefault;
bool gLegRun = false;                     // the legs run (the direct reload while he runs)
// a direct reload started while he walked or ran (DevJunior, 11.11.2026, device test: "when he reloads walking or
// running he still stops for a moment as if he were going to aim"): the legs take the walk at once, from the step
// he was in, instead of fading in from the reload motion's standing legs and starting a new step
bool gReloadFromMove = false;
bool gReloadFromRun = false;
f32 gReloadFrame = 0.0f;                  // his walk / run motion's sequence frame then

// this frame's move while aiming (aimMove), for the legs (re4_player_after_routine)
bool gAimMoving = false;
f32 gAimSx = 0.0f;
f32 gAimSy = 0.0f;
f32 gAimMag = 0.0f;

// the walk motion on the legs: every part as the aiming pose left it, the walk pose's rotations of the legs
alignas(16) u8 gSave[kMaxParts * sizeof(cParts)];
Mtx gWalkRot[kMaxParts];  // thighs: their turn against the model; shins, feet: their local turn
HERMITE_KEY gWalkHist[kMaxParts][6];
bool gLeg[kMaxParts];
bool gLegRoot[kMaxParts];
Mtx gWalkHips;          // the walk pose's hips (their local matrix)
Vec gHipT;              // their running mean: translation, turn
Quaternion gHipQ;
bool gHipMean = false;
MOTION_INFO gWalk;
void* gWalkMot = 0;
int gWalkFlags = 4;  // MotionSetCore's flags it was set with (0x40: mirrored, a side step)
cPlayer* gWalkPl = 0;
int gWalkUpper;  // the parts it was found for: 1 the upper body (overlay's `upper`), 2 the arms, 0 the legs
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

// p * q and the unit of q (the SDK's C_QUATMultiply / C_QUATNormalize are not declared for the port's build)
void quatMul(const Quaternion& p, const Quaternion& q, Quaternion* r)
{
    Quaternion t;
    t.w = p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z;
    t.x = p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y;
    t.y = p.w * q.y + p.y * q.w + p.z * q.x - p.x * q.z;
    t.z = p.w * q.z + p.z * q.w + p.x * q.y - p.y * q.x;
    *r = t;
}

void quatUnit(Quaternion* q)
{
    const f32 n = sqrtf(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    if (n > 0.0f) {
        q->x /= n;
        q->y /= n;
        q->z /= n;
        q->w /= n;
    }
}

f32 axis(s8 v, f32 range)
{
    const f32 f = (f32) v / range;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

// The prototype's direct reload (no aim key: PlReloadDirect) in the weapon routine's reload step.
bool directReload(cPlayer* pl)
{
    return pl->r_no_0 == 0 && pl->r_no_1 == 6 && pl->r_no_2 == 4 && PlReloadDirect == 1;
}

// The walk routines of Pl_func_move_tbl (player.cpp): footwork, walk, back step, turn, run.
bool walkRoutine(cPlayer* pl)
{
    return pl->r_no_0 == 0 && pl->r_no_1 <= 4 && !pl->stat.check(cPlayer::F_EVENT)
        && !pl->stat.check(cPlayer::F_BINOCULAR);
}

// the remake's turn about (runtime/locomotion.cpp): the stick this far from Leon's heading (radians), this firmly
// (of its range), this many frames in a row
const f32 kBackTurn = 2.4f;
const f32 kBackFirm = 0.7f;
const int kBackHold = 4;
int gBackFrames = 0;

void freeMove(cPlayer* pl)
{
    const f32 dx = pl->pos.x - gLastPos.x;
    const f32 dz = pl->pos.z - gLastPos.z;
    if (!gCamValid || pl != gLastPl || dx * dx + dz * dz > kTeleport * kTeleport) {
        gCamYaw = pl->ang.y;
        gCamValid = true;
        gStickRest = 0;
    }
    const bool reloading = directReload(pl);
    if (!walkRoutine(pl) && !reloading) {
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
        gStickRest = 0;
    } else if (gRecentering) {
        if (gStickRest < 1000) {
            gStickRest++;
        }
        const f32 d = wrap(pl->ang.y - gCamYaw);
        gCamYaw = wrap(gCamYaw + d * kRecenter);
        if (fabsf(d) < 0.01f) {
            gCamYaw = pl->ang.y;
            gRecentering = false;
        }
    } else if (gStickRest < 1000) {
        gStickRest++;
    }
    Key.substickX = 0;
    if (reloading) {
        return;  // reloadMove walks him; he keeps facing where he faces
    }
    // (joyKamae asks the weapon; without one, between rooms, it would only log an error)
    if ((pl->Wep && pl->Wep->m_pWep && joyKamae()) || joyLKamae()) {
        pl->ang.y = gCamYaw;  // a weapon or the knife ready: he aims where the camera looks
        return;
    }
    const f32 sx = axis(Key.stickX, kMainRange);
    const f32 sy = axis(Key.stickY, kMainRange);
    if (sx * sx + sy * sy <= kMoveDead * kMoveDead) {
        gBackFrames = 0;
        return;  // the stick rests: the D-pad's tank controls as the game
    }
    const f32 target = wrap(gCamYaw - atan2f(sx, sy));
    const f32 diff = wrap(target - pl->ang.y);
    // a turn about of the remake (runtime/locomotion.cpp): its clip turns him, his heading stays; running he keeps
    // running, standing he stays until it ends
    // (only a stick held firmly back for a few frames: one springing back to the middle passes the other side for a
    // frame or two; device video 17.11.2026: "he still turns about to go the other way at times")
    if (fabsf(diff) >= kBackTurn && sx * sx + sy * sy >= kBackFirm * kBackFirm) {
        if (gBackFrames < 100) {
            gBackFrames++;
        }
    } else {
        gBackFrames = 0;
    }
    // (while the hold is not yet sure he neither turns nor steps back: he keeps on as he was)
    if (re4_move_turn(pl, diff, gBackFrames >= kBackHold) || (gBackFrames > 0 && gBackFrames < kBackHold)) {
        Key.on &= ~(u64) 0xF;
        Key.trg &= ~(u64) 0xF;
        if (pl->r_no_1 == 3 || pl->r_no_1 == 1) {
            Key.on |= 1;
        }
        return;
    }
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
    // the camera follows his way by itself (as the remake's) once the right stick rested
    if (moving && !gRecentering && gStickRest >= kFollowDelay) {
        f32 d = wrap(pl->ang.y - gCamYaw);
        const bool running = pl->r_no_1 == 3;
        if (running && fabsf(d) > kFollowFlip && gFollowSign != 0 && (d > 0) != (gFollowSign > 0)) {
            d += gFollowSign > 0 ? 2.0f * kPi : -2.0f * kPi;  // near 180 degrees: keep swinging the same way
        }
        if (fabsf(d) < kFollowAngle || running) {
            const f32 mag = sqrtf(sx * sx + sy * sy);
            f32 turn = d * (running ? kFollowRun : kFollowWalk) * (mag > 1.0f ? 1.0f : mag);
            turn = turn > kFollowStep ? kFollowStep : turn < -kFollowStep ? -kFollowStep : turn;
            gCamYaw = wrap(gCamYaw + turn);
            gFollowSign = turn > 0.0f ? 1 : turn < 0.0f ? -1 : gFollowSign;
        }
    } else {
        gFollowSign = 0;
    }
}

// R and B pressed together in the walk routines: the prototype's direct reload (cPlayer::actionSelect with the
// reload key type 1: setRno(0, 6, 4, 0), m_Work0 1; PlReloadDirect 1 = not aiming). Their bits out of this frame
// (no run, no aim).
int gEatA;  // frames in which A stays from the game after a quick reload started

void eatA()
{
    Joy[0].on &= ~0x100u;
    Joy[0].trg &= ~0x100u;
    Key.on &= ~(u64) 0x80;
    Key.trg &= ~(u64) 0x80;
}

void reloadInput(cPlayer* pl)
{
    if (gEatA > 0) {
        gEatA--;
        eatA();  // held longer, A has no new press for the game afterwards
    }
    const u32 both = 0x20 | 0x200;  // R, B
    const bool together = re4_aim_move_on() && (Joy[0].trg & both) == both;
    const bool asked = gReloadAsked.exchange(false);
    // (Mods, 19.11.2026, DevJunior: "aiming, the button you put in the remake layout does not reload") aiming, the
    // quick reload is the game's own reload from the aim (cPlayer::keyReload: R held and B pressed): B for this frame
    // in the game's pad word only (not the run key), and he aims again after it as the game does
    if (asked && pl->r_no_0 == 0 && pl->r_no_1 == 6 && pl->r_no_2 == 1 && (Joy[0].on & 0x20) && pl->Wep
        && pl->Wep->m_pWep && pl->Wep->m_pWep->reloadable()) {
        Joy[0].on |= 0x200;
        Joy[0].trg |= 0x200;
        gEatA = 6;
        eatA();
        re4_log("reload: the quick reload while aiming (the game's reload from the aim)");
        return;
    }
    if ((!together && !asked) || !walkRoutine(pl) || !pl->Wep || !pl->Wep->m_pWep || !pl->Wep->m_pWep->reloadable()) {
        return;
    }
    gReloadFromMove = pl->r_no_1 == 1 || pl->r_no_1 == 3;
    gReloadFromRun = pl->r_no_1 == 3;
    gReloadFrame = pl->Motion.Seq_frame;
    if (gReloadFromMove) {
        gWalkMot = 0;  // the legs' walk anew, from that frame
    }
    pl->setRno(0, 6, 4, 0);
    pl->m_Work0 = 1;
    PlReloadDirect = 1;
    if (together) {
        Joy[0].on &= ~both;
        Joy[0].trg &= ~both;
        Key.on &= ~(u64) 0x40000000;  // the run key (B)
        Key.trg &= ~(u64) 0x40000000;
    }
    if (asked) {
        // the alternate controls' R2 asks with A pressed (the fire key without aiming), which reaches the game
        // in this frame or the next: no action or shot besides (eatA)
        gEatA = 6;
        eatA();
    }
}

// Cancelling a reload (wish of DevJunior, 19.11.2026: "the cancel of actions ... the reload"), as in the 2023 remake.
// In the weapon routine's reload step (any weapon, aimed or not), after kCancelFrom frames, a new press of
//  - the run key, the knife key, or a D-pad quick slot (runtime/quick_slots.cpp, which then changes the weapon)
//    ends it at once; before the weapon object loaded the rounds (ItemMgr.reloadable() still 1) the magazine stays
//    as it was and the rounds stay in the case (cObjWep::interrupt would load them: the weapon object leaves its
//    reload mode first);
//  - the aim key, once the rounds are in: the rest of the motion gives way to the aim at once.
// He goes to his footwork (the game's own interrupt, cPlayer::interrupt: the gun shown, the knife put away, its sound
// stopped), from where the held keys take him on (run, knife, aim) as from standing. Not crouching (the footwork would
// stand him up), not in an event.
const int kCancelFrom = 3;
int gReloadAge = 0;

void reloadCancel(cPlayer* pl)
{
    if (pl->r_no_0 != 0 || pl->r_no_1 != 6 || pl->r_no_2 != 4) {
        gReloadAge = 0;
        return;
    }
    if (gReloadAge < 1000) {
        gReloadAge++;
    }
    if (gReloadAge <= kCancelFrom || !pl->Wep || !pl->Wep->m_pWep || pl->stat.check(cPlayer::F_CROUCH)
        || pl->stat.check(cPlayer::F_EVENT) || StaFlagChk(pG, STA_EVENT)) {
        return;
    }
    const bool loaded = ItemMgr.reloadable() == 0;
    const bool run = (Key.trg & 0x40000000) != 0;
    const bool knife = (Key.trg & 0x800) != 0 || (Joy[0].trg & 0x40) != 0;
    const bool slot = re4_quick_slots_on() && (Joy[0].trg & 0xF) != 0;
    const bool aim = loaded && (Key.trg & 0x10) != 0;
    if (!run && !knife && !slot && !aim) {
        return;
    }
    cObjWep* gun = pl->Wep->m_pWep;
    if (!loaded && gun->getStat() == cObjWep::S_RELOAD) {
        gun->setStat(cObjWep::S_STAY);  // no rounds: interrupt would finish the reload
    }
    pl->interrupt();
    pl->m_Hokan = 6;
    pl->m_Frame = 0;
    pl->r_no_0 = 0;
    pl->r_no_1 = 0;
    pl->r_no_2 = 0;
    pl->r_no_3 = 1;  // pl_R1_Footwork: its motion blended in over m_Hokan frames
    re4_log("reload: cancelled by %s after %d frames, %s",
            run ? "running" : knife ? "the knife" : slot ? "a quick slot" : "aiming", gReloadAge,
            loaded ? "the rounds were in" : "no rounds loaded");
    gReloadAge = 0;
}

// His walk and run as the game moves him (root motion): the way of the last frame in the walk / run routines.
void measure(cPlayer* pl)
{
    if (pl != gLastPl || pl->r_no_0 != 0 || (pl->r_no_1 != 1 && pl->r_no_1 != 3)) {
        return;
    }
    const f32 dx = pl->pos.x - gLastPos.x;
    const f32 dz = pl->pos.z - gLastPos.z;
    const f32 d = sqrtf(dx * dx + dz * dz);
    if (d < 5.0f || d > 300.0f) {
        return;
    }
    f32& v = pl->r_no_1 == 3 ? gRunSpeed : gWalkSpeed;
    v = v * 0.9f + d * 0.1f;
}

// Walking during the direct reload: the main stick, seen from the free camera (or relative to his facing).
void reloadMove(cPlayer* pl)
{
    if (!directReload(pl)) {
        return;
    }
    const f32 sx = axis(Key.stickX, kMainRange);
    const f32 sy = axis(Key.stickY, kMainRange);
    f32 mag = sqrtf(sx * sx + sy * sy);
    if (mag <= kMoveDead) {
        return;
    }
    // the way in world yaw: the camera's (Free movement) or his own, turned by the stick
    const f32 base = re4_free_move_on() && gCamValid ? gCamYaw : pl->ang.y;
    const f32 dir = wrap(base - atan2f(sx, sy));
    if (mag > 1.0f) {
        mag = 1.0f;
    }
    // his own speed: running while the run key stays held (or "Always run"), else walking
    gLegRun = re4_run_held((Key.on & 0x40000000) != 0) != 0;
    const f32 speed = (gLegRun ? gRunSpeed : gWalkSpeed) * mag;
    pl->pos.x += sinf(dir) * speed;
    pl->pos.z += cosf(dir) * speed;
    // for the legs: the way relative to where he faces
    const f32 rel = wrap(dir - pl->ang.y);
    gAimMoving = true;
    gAimSx = -sinf(rel) * mag;  // right = a smaller angle
    gAimSy = cosf(rel) * mag;
    gAimMag = mag;
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

// The big guns' shots (device report 18.11.2026: "firing the big weapons stops you too long, leaving you exposed to
// enemies like the giant"): with "Free movement" the aim key let go ends the shotgun's recoil and pump
// (pl_shotgun.cpp wep07_r3_fire10, 40 frames, the Striker's 32: the game waits for all of it) and the bolt-action
// rifle's bolt cycle (pl_rifle.cpp wep09_r3_fire30: from its frame 25) from frame kShotEscape on; the gun goes down
// as the game puts it down then (the set state's wepDown, the bolt's down state). The semi-automatic rifles already
// let go from their frame 10 (wep09_r3_fire10).
const f32 kShotEscape = 16.0f;

void shotEscape(cPlayer* pl)
{
    if (pl->r_no_0 != 0 || pl->r_no_1 != 6 || pl->r_no_2 != 2 || !pl->Wep || !pl->Wep->m_pWep ||
        pl->Motion.Seq_frame < kShotEscape || joyKamae()) {
        return;
    }
    const int no = pG->weapon_no;
    if (pl->r_no_3 == 1 && (no == 7 || no == 8 || no == 0x21)) {
        pl->r_no_2 = 1;  // the end of the pump: the set state sees the aim key let go
        pl->r_no_3 = 4;
    } else if (pl->r_no_3 == 3 && no == 9 && !pl->stat.check(cPlayer::F_CROUCH)) {
        pl->setRno(0, 6, 3, 0);  // the bolt's own way out (wep09_r3_fire30)
        pl->m_Work0 = 1;
    } else {
        return;
    }
    static int said;
    if (!said) {
        said = 1;
        re4_log("free move: the shot's recovery let go (weapon %d, frame %.0f)", no, (double) pl->Motion.Seq_frame);
    }
}

bool weaponRoutine(cPlayer* pl)
{
    return pl->r_no_0 == 0 && (pl->r_no_1 == 6 || pl->r_no_1 == 0xB);
}

// The legs: the parts below the hips that hang off the walk motion's IK chain roots (kind 0x30, IKInit's;
// ik.cpp), or, without them, the hips' children below the hips. gLeg[i] by the parts chain's order.
void findLegs(cPlayer* pl, int n, int upper)
{
    cParts* roots[8];
    int nRoots = 0;
    cParts* hips = pl->pList;
    if (upper == 2) {
        // the arms (Mods, 23.11.2026: the shotgun carried as in the remake; DevJunior: "it looks very stiff running"
        // with the whole upper body of the carry): Leon's shoulders (parts 5 and 11) and all under them, in their own
        // turns under the chest the routines pose (the torso moves with whatever plays)
        roots[nRoots++] = pl->getPartsPtr(5);
        roots[nRoots++] = pl->getPartsPtr(11);
    } else if (upper) {
        // the upper body: Leon's waist (part 1) and all under it (spine, arms, head; runtime/locomotion.cpp's big
        // weapons), turned against the model under the routines' hips
        roots[nRoots++] = pl->getPartsPtr(1);
    }
    for (int i = 0; !upper && i < gWalk.Joint_num && nRoots < 8; i++) {
        if (!(gWalk.pJoint_kind[i] & 0x30)) {
            continue;
        }
        cParts* r = pl->getPartsPtr(gWalk.pJoint_no[i]);
        if (r && r != hips && r->world.y < hips->world.y + 100.0f) {
            roots[nRoots++] = r;
        }
    }
    if (nRoots == 0 && !upper) {
        // a motion without leg IK (a mod's remake walk, runtime/locomotion.cpp): the legs' first parts below the
        // hips, under the hips or under a part of theirs at hip height (Leon's pelvis, part 17, holds the thighs;
        // device report 13.11.2026: the legs stood still while he moved aiming or reloading)
        for (cParts* p = hips->pList; p && nRoots < 8; p = p->pList) {
            if (p->world.y >= hips->world.y - 50.0f || !p->pParent || p->pParent == (cCoord*) pl) {
                continue;
            }
            cParts* q = (cParts*) p->pParent;
            const bool underHips = q == hips;
            const bool underPelvis = q->pParent == hips && q->world.y >= hips->world.y - 50.0f &&
                                     q->world.y <= hips->world.y + 50.0f;
            if (underHips || underPelvis) {
                roots[nRoots++] = p;
            }
        }
    }
    int i = 0;
    for (cParts* p = hips; p && i < n; p = p->pList, i++) {
        gLeg[i] = false;
        gLegRoot[i] = false;
        for (int r = 0; r < nRoots; r++) {
            if (p == roots[r] && upper != 2) {  // the arms: the shoulders' local turns, as every part under them
                gLegRoot[i] = true;
            }
        }
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

// A walk-like motion on Leon's legs (`upper`: on his upper body instead, from the waist up; `mot`, `seq`; from sequence
// frame `start` when it is new): every part as the
// routines posed him is kept, the motion is posed on a copy (its own work gWalk and key histories), the legs take its
// turns (`legW` of them), the hips its swing against its running mean (`swayW` of the turn, `shiftW` of the bob).
// `at` >= 0: the motion at that sequence frame (the caller keeps it in step), else it advances by `speed`.
// Shared by the legs while aiming (legs) and the remake's legs under a big weapon (runtime/locomotion.cpp).
bool overlay(cPlayer* pl, void* mot, void* seq, int start, f32 speed, f32 at, f32 legW, f32 swayW, f32 shiftW,
             int upper = 0, int flags = 4)
{
    int n = 0;
    for (cParts* p = pl->pList; p; p = p->pList) {
        if (++n > kMaxParts) {
            return false;
        }
    }
    // a copy of every part (the routines' pose), the model's matrix
    int i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        memcpy(gSave + i * sizeof(cParts), (void*) p, sizeof(cParts));
    }
    Mtx modelMat;
    PSMTXCopy(pl->mat, modelMat);
    if (mot != gWalkMot || pl != gWalkPl || upper != gWalkUpper || flags != gWalkFlags) {
        memset(&gWalk, 0, sizeof gWalk);
        // keep the model's blend motion (cMot3's pitch blend); IKInit below, on the copy only
        gWalk.Mot_flag = 0x20000000 | 0x10000000;
        MotionSetCore(pl, &gWalk, mot, seq, 0, flags, start);
        gWalkFlags = flags;
        gWalk.Mot_flag |= 0x40000000;  // MotionMoveCore: leave the model's matrix alone
        memset(gWalkHist, 0, sizeof gWalkHist);
        gWalkMot = mot;
        gWalkPl = pl;
        gWalkUpper = upper;
        gHipMean = false;
        findLegs(pl, n, upper);
    }
    i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        memcpy(p->motParts.hist, gWalkHist[i], sizeof gWalkHist[i]);
    }
    // the walk pose as MotionMove makes it: keys, local matrices, the motion's leg IK on the floor
    gWalk.Hokan_cnt = 0;
    if (at >= 0.0f) {
        gWalk.Seq_frame = at;
        gWalk.Seq_speed = 0.0f;
        MotionSequenceCtrl(&gWalk);  // its keys at `at`
        MotionMoveCore(pl, &gWalk, 0);
    } else {
        gWalk.Seq_speed = speed;
        MotionMoveCore(pl, &gWalk, 0);
        MotionSequenceCtrl(&gWalk);
    }
    pl->partsMatCalc();
    IKInit(pl, &gWalk);
    pl->partsWorldCalc();
    InverseKinematics(pl, 1);
    pl->partsWorldCalc();
    PSMTXCopy(pl->pList->l_mat, gWalkHips);
    i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        memcpy(gWalkHist[i], p->motParts.hist, sizeof gWalkHist[i]);
        if (gLeg[i]) {
            PSMTXCopy(gLegRoot[i] ? p->mat : p->l_mat, gWalkRot[i]);
        }
    }
    // the routines' pose back, every part and the model
    i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        memcpy((void*) p, gSave + i * sizeof(cParts), sizeof(cParts));
    }
    PSMTXCopy(modelMat, pl->mat);
    // the hips: the walk's swing (the walk pose's hips against their running mean) on the routines' pose
    cParts* hips = pl->pList;
    Mtx hipsWorld;
    {
        Quaternion qw;
        C_QUATMtx(&qw, gWalkHips);
        const Vec tw = {gWalkHips[0][3], gWalkHips[1][3], gWalkHips[2][3]};
        if (!gHipMean) {
            gHipT = tw;
            gHipQ = qw;
            gHipMean = true;
        } else {
            gHipT.x += (tw.x - gHipT.x) * kSwayMean;
            gHipT.y += (tw.y - gHipT.y) * kSwayMean;
            gHipT.z += (tw.z - gHipT.z) * kSwayMean;
            const f32 dot = gHipQ.x * qw.x + gHipQ.y * qw.y + gHipQ.z * qw.z + gHipQ.w * qw.w;
            const f32 sgn = dot < 0.0f ? -1.0f : 1.0f;
            gHipQ.x += (qw.x * sgn - gHipQ.x) * kSwayMean;
            gHipQ.y += (qw.y * sgn - gHipQ.y) * kSwayMean;
            gHipQ.z += (qw.z * sgn - gHipQ.z) * kSwayMean;
            gHipQ.w += (qw.w * sgn - gHipQ.w) * kSwayMean;
            quatUnit(&gHipQ);
        }
        // the swing: this frame's turn against the mean (qw * conj(mean)), scaled
        Quaternion inv = {-gHipQ.x, -gHipQ.y, -gHipQ.z, gHipQ.w};
        Quaternion swing;
        quatMul(qw, inv, &swing);
        const Quaternion none = {0.0f, 0.0f, 0.0f, 1.0f};
        Quaternion part;
        C_QUATSlerp(&none, &swing, &part, swayW);
        Quaternion qa;
        Quaternion q;
        C_QUATMtx(&qa, hips->l_mat);
        quatMul(part, qa, &q);
        Vec t;
        t.x = hips->l_mat[0][3] + (tw.x - gHipT.x) * shiftW;
        t.y = hips->l_mat[1][3] + (tw.y - gHipT.y) * shiftW;
        t.z = hips->l_mat[2][3] + (tw.z - gHipT.z) * shiftW;
        PSMTXQuat(hips->l_mat, &q);
        TransMatrix(hips->l_mat, &t);
        ScaleMatrix(hips->l_mat, &hips->scale);
        // their world as partsWorldCalc will make it, for the thighs below
        if (hips->pParent == (cCoord*) pl || !hips->pParent) {
            PSMTXConcat(pl->mat, hips->l_mat, hipsWorld);
        } else {
            PSMTXConcat(hips->pParent->mat, hips->l_mat, hipsWorld);
        }
    }
    // the legs: the routines' turn toward the walk pose's, the bones' offsets kept
    i = 0;
    for (cParts* p = pl->pList; p; p = p->pList, i++) {
        if (!gLeg[i]) {
            continue;
        }
        Mtx walk;
        if (gLegRoot[i]) {
            // the thigh as the walk pose turns it against the model, under the routines' hips
            Mtx inv;
            PSMTXInverse(p->pParent == hips ? hipsWorld : p->pParent->mat, inv);
            PSMTXConcat(inv, gWalkRot[i], walk);
        } else {
            PSMTXCopy(gWalkRot[i], walk);
        }
        Quaternion qa;
        Quaternion qw;
        Quaternion q;
        C_QUATMtx(&qa, p->l_mat);
        C_QUATMtx(&qw, walk);
        C_QUATSlerp(&qa, &qw, &q, legW);
        Vec t;
        t.x = p->l_mat[0][3];
        t.y = p->l_mat[1][3];
        t.z = p->l_mat[2][3];
        PSMTXQuat(p->l_mat, &q);
        TransMatrix(p->l_mat, &t);
        ScaleMatrix(p->l_mat, &p->scale);
    }
    return true;
}

void legs(cPlayer* pl)
{
    const bool reload = directReload(pl);
    const bool want = gAimMoving && weaponRoutine(pl) && (pl->isKamae() || reload);
    gLegW = want ? (gLegW + kLegFade > 1.0f ? 1.0f : gLegW + kLegFade) : (gLegW - kLegFade < 0.0f ? 0.0f : gLegW - kLegFade);
    if (want && reload && gReloadFromMove) {
        gLegW = 1.0f;  // he was walking or running: no fade in (see gReloadFromMove)
    }
    if (gLegW <= 0.0f || !weaponRoutine(pl) || !pl->pList || !pl->m_MotTbl) {
        gLegW = 0.0f;
        gWalkMot = 0;
        return;
    }
    // (Mods, 21.11.2026, DevJunior: "the movements to the sides") aiming, sideways or aslant: Leon's own side steps of
    // the remake (runtime/locomotion.cpp re4_move_strafe), at the aiming walk's speed, with the same foot forward as
    // the step before; straight forward or back his walk and back step as before
    const bool runReload = gLegRun && directReload(pl);
    if (!runReload) {
        void* sm = 0;
        void* ss = 0;
        int flip = 0, start = 0;
        f32 sp = 1.0f;
        // the speed he really moves at: aiming kAimWalk, reloading on the move his walk (moveReload)
        const f32 want = (reload && !pl->isKamae() ? gWalkSpeed : kAimWalk) * (gAimMag > 0.2f ? gAimMag : 0.2f);
        if (re4_move_strafe(pl, gAimSx, gAimSy, want, gWalkMot,
                            (f32) gWalk.Seq.Frame / 64.0f, &sm, &ss, &flip, &sp, &start)) {
            if (!overlay(pl, sm, ss, start, sp, -1.0f, gLegW * (reload ? 1.0f : kLegMax),
                         gLegW * (reload ? 1.0f : kSwayAim), gLegW, false, 4 | flip)) {
                gLegW = 0.0f;
            }
            return;
        }
    }
    // the walk motion, the back step when he moves back (as the routines choose them: dmMotCk, player.cpp)
    const bool back = gAimSy < -0.3f && -gAimSy > fabsf(gAimSx);
    const bool run = gLegRun && !back && directReload(pl);  // running while reloading: his run (pl_R1_Run's)
    void** tbl = pl->m_MotTbl;
    const bool own = dmMotCk() != 0;
    void* mot = own ? tbl[run ? 6 : back ? 8 : 2] : tbl[run ? 0x65 : back ? 0x63 : 0x61];
    void* seq = own ? tbl[run ? 7 : back ? 9 : 3] : tbl[run ? 0x66 : back ? 0x64 : 0x62];
    if (!mot) {
        gLegW = 0.0f;
        return;
    }
    // a reload started on the move: the step he was in (his walk / run motion's frame), else the first
    const bool cont = gReloadFromMove && reload && run == gReloadFromRun && !back;
    if (!overlay(pl, mot, seq, cont ? (int) gReloadFrame : 0, kLegSpeed * (gAimMag > 0.4f ? gAimMag : 0.4f), -1.0f,
                 gLegW * (reload ? 1.0f : kLegMax), gLegW * (reload ? 1.0f : kSwayAim), gLegW)) {
        gLegW = 0.0f;
    }
}

}  // namespace

extern "C" {

// Patch 0900 (cPlayer::move, right after the routines, the game thread): the walk motion on the legs while he
// moves aiming.
void re4_player_after_routine(cPlayer* pl)
{
    if (pl && !directReload(pl)) {
        gReloadFromMove = false;
    }
    if (!pl || !re4_aim_move_on()) {
        gLegW = 0.0f;
    } else {
        legs(pl);
    }
    // a big weapon in his hands: the game's own movement, the remake's legs (runtime/locomotion.cpp)
    if (gLegW <= 0.0f && !(pl && re4_move_heavy_legs(pl))) {
        gWalkMot = 0;
    }
}

// runtime/locomotion.cpp (re4_move_heavy_legs): a motion on Leon's legs, see overlay
int re4_legs_overlay(void* pl, void* mot, void* seq, int start, float speed, float at, float legW, float sway,
                     float shift, int upper)
{
    return pl && mot && overlay((cPlayer*) pl, mot, seq, start, speed, at, legW, sway, shift, upper) ? 1 : 0;
}

// Patches 0909 / 0933 (the reload steps of the long guns and the handguns, the game thread): 1 when a reload without
// aiming (started so, or the aim key let go since) may end now, kQuickEnd frames after the weapon is full (ItemMgr.reloadable() 0: the weapon object loads the rounds
// on its motion's frame); the rest of the reload motion brings the gun back up to aim (DevJunior, 11.11.2026: "with
// the long guns, it still aims for an instant at the end"). Aiming, or infinite ammo (reloadable stays 1): 0.
int re4_reload_quick_end(void)
{
    static int loaded;
    // (Mods, 19.11.2026: also a reload started from the aim once the aim key is let go: device video, the pistol
    // came back up to aim after it)
    if (joyKamae() != 0 || ItemMgr.reloadable() != 0) {
        loaded = 0;
        return 0;
    }
    return ++loaded > kQuickEnd ? (loaded = 0, 1) : 0;
}

// The app (the touch RLOAD button, the aim bits' toggling bit 20 through re4_aim_set; any thread): reload
// without aiming.
void re4_reload_request(void)
{
    gReloadAsked = true;
}

// For the runtime test: his walk and run as measured (units per game frame).
void re4_move_speeds(f32* walk, f32* run)
{
    *walk = gWalkSpeed;
    *run = gRunSpeed;
}

// The app (R3, the aim bits' toggling bit 6 through re4_aim_set; any thread): the free camera back behind Leon.
void re4_camera_recenter(void)
{
    gRecenterAsked = true;
}

// Patch 0900 (cPlayer::keyConfig, the game thread, every frame of Leon's move before the routines read the keys).
void re4_player_input(cPlayer* pl)
{
    gCamOn = false;
    gAimMoving = false;
    if (!pl) {
        return;
    }
    measure(pl);
    gLegRun = false;
    // Ashley's own chapter: her pistol aimed (her keys out of the game's first)
    const int ashleyAims = re4_ashley_gun_input(pl, re4_free_move_on() && gCamValid && pl == gLastPl);
    reloadCancel(pl);           // before the quick slots take the D-pad's bits
    // (Mods, 26.11.2026, DevJunior: "when I reload while still aiming and let go of the aim key before the reload ends,
    // I cannot move", any weapon) a reload started from the aim (PlReloadDirect 0) whose aim key is let go becomes the
    // direct reload: he walks during the rest of it (reloadMove), it ends kQuickEnd frames after the weapon is full
    // and he goes on in the walk routines (patches 0909 / 0933), as a reload started without aiming
    if (pl->r_no_0 == 0 && pl->r_no_1 == 6 && pl->r_no_2 == 4 && PlReloadDirect == 0 && pl->Wep && pl->Wep->m_pWep &&
        !joyKamae()) {
        PlReloadDirect = 1;
        static int said;
        if (said < 4) {
            said++;
            re4_log("reload: the aim key let go during a reload from the aim: the direct reload (weapon %d, frame %.0f)",
                    (int) pG->weapon_no, (double) pl->Motion.Seq_frame);
        }
    }
    re4_quick_slots_input(pl);  // first: the D-pad's bits out of the keys when it equips weapons
    re4_parry_input(pl);
    reloadInput(pl);  // before the camera: a reload started now keeps the free camera
    if (re4_free_move_on()) {
        shotEscape(pl);
        freeMove(pl);
    } else {
        gCamValid = false;
    }
    if (ashleyAims && re4_free_move_on() && gCamValid) {
        pl->ang.y = gCamYaw;  // she aims where the camera looks, as Leon
    }
    if (re4_aim_move_on()) {
        aimMove(pl);
        reloadMove(pl);
    }
    gLastPl = pl;
    gLastPos = pl->pos;
}

// Patch 0900 (CameraQuasiFPS::calcBaseMatrix, after it took Leon's matrix): the camera's frame turned to the free
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
