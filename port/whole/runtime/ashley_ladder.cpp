// Ashley climbs down ladders on her own (wish of DevJunior, 19.11.2026: "que Ashley pueda bajar escaleras sola", as
// in the 2023 remake). Compiled with the game's flags into libre4_main (re4_gamehooks_obj); patch 0936 calls
// re4_ashley_ladder_down at the start of each of her normal frames (pl_npc.cpp cSubChar::moveCore, after analyze),
// always (until 23.11.2026 only with the app's "Remake movement", re4_move_on).
//
// The game itself puts her on a ladder only while she stands in its area (cSubChar::ladder2Check: the scenario's
// ladder area, type 0x10, around her feet). Where Leon climbs down and her way to him ends at the edge, she waits
// there for him to catch her (routine 0x12, the "catch me" of a ledge) or stands at the top (routines 0, 0x13). Then
// a ladder going down (an area of level < -1, act "climb down") near her, at her height, with Leon below and off it:
// she walks to where the game would stand Leon to take it, facing it (SubCharMoveTo's walk: routine 1, F_MOVE_TO),
// and there the game's own climb starts, just as ladder2Check sets it up (routine 0xC, m_Work0 1: the motions down,
// m_Work1 the rungs). A ledge without a ladder stays the game's catch; a window, a fence, the player's "wait" stay
// the game's. Each step is logged ("ashley ladder: ...").
#include "types.h"
#include "global.h"
#include "main.h"
#include "em.h"
#include "player.h"
#include "pl_npc.h"
#include "atari.h"
#include "sce_at.h"

#include <math.h>

extern "C" void re4_log(const char* fmt, ...);

namespace {

// (23.11.2026, DevJunior: "Ashley cannot go down ladders, she should go down alone at all of them"; no ladder was
// found on his device: wider limits, the reasons logged, no longer only with "Remake movement")
const f32 kReach = 6000.0f;        // her distance to the ladder (horizontal)
const f32 kLevel = 900.0f;         // the ladder's top at her height
const f32 kBelow = 1000.0f;        // Leon this far below her
const f32 kLeonReach = 25000.0f;   // Leon this near the ladder (horizontal)
const int kStill = 45;             // frames standing at the top before she goes on her own
const int kGiveUp = 300;           // frames of walk to the ladder at most

int gWalk;          // frames walking to the ladder (0 none)
Vec gUse;           // where she takes it
f32 gAng;           // facing it
int gLevel;         // its level (< -1)
int gStillFrames;   // frames she has stood still with Leon below
Vec gLast;

f32 flat2(const Vec& a, const Vec& b)
{
    const f32 x = a.x - b.x, z = a.z - b.z;
    return x * x + z * z;
}

// The nearest ladder going down from her height that she can walk to and Leon is near; false none
int gWhyFrame = -100000;  // the frame the last "no ladder" line was logged

bool findLadder(cSubChar* a, Vec* use, f32* ang, int* level)
{
    const bool say = (int) pG->Frame_cnt - gWhyFrame > 600;
    int seen = 0;
    // the area list (sce_at.cpp's pS) is set up at every room start (SceAtInit), before she moves
    f32 best = kReach * kReach;
    bool found = false;
    SCE_AT_DATA* w = sceAtSetOtStart();
    while ((w = sceAtGetOtAddr(w)) != 0) {
        if (w->id != SCEAT_ID_LADDER) {
            continue;
        }
        Vec p;
        f32 r;
        sceAtGetLadderPos(&w->ladder, &p, &r);
        if (say && seen < 8) {
            seen++;
            re4_log("ashley ladder: a ladder, on %d, level %d, %.1f m away, %.1f m above her, Leon %.1f m from it",
                    w->be_flg & 1, (int) w->ladder.height, sqrtf(flat2(p, a->pos)) / 1000.0f,
                    (p.y - a->pos.y) / 1000.0f, sqrtf(flat2(p, pPL->pos)) / 1000.0f);
        }
        if (!(w->be_flg & 1) || (w->ladder.height >= -1 && w->ladder.height <= 1)) {
            continue;
        }
        if (w->ladder.height > 1) {
            // (device report 23.11.2026: the well's ladder has only its foot's area, level 5, 5 m below her) a
            // ladder going up from below her: she takes it from its top, on the ledge behind its line (the foot's
            // place mirrored over the ladder), facing the drop, and climbs down as many rungs
            const f32 drop = a->pos.y - p.y;
            if (drop < kBelow || drop > 20000.0f) {
                continue;
            }
            const Vec& rec = w->ladder.pos;
            p.x = rec.x - (p.x - rec.x);
            p.z = rec.z - (p.z - rec.z);
            p.y = a->pos.y;
            r = r + 3.14159265f;
            if (r > 3.14159265f) {
                r -= 6.2831853f;
            }
        } else if (fabsf(p.y - a->pos.y) > kLevel) {
            continue;
        }
        const f32 d = flat2(p, a->pos);
        if (d > best || flat2(p, pPL->pos) > kLeonReach * kLeonReach) {
            continue;
        }
        // a free way to it, at her waist
        Vec from = a->pos, to = p;
        from.y += 400.0f;
        to.y += 400.0f;
        if (d > 250000.0f && SatMgr.hitCheck(&from, &to, 0, 0, 0, 0)) {
            continue;
        }
        best = d;
        *use = p;
        *ang = r;
        *level = w->ladder.height > 1 ? -w->ladder.height : w->ladder.height;
        found = true;
    }
    if (say && !found) {
        gWhyFrame = (int) pG->Frame_cnt;
        re4_log("ashley ladder: waiting above Leon (routine 0x%x, %.1f m higher), no ladder down for her (%d seen)",
                (unsigned) a->r_no_1, (a->pos.y - pPL->pos.y) / 1000.0f, seen);
    }
    return found;
}

// The game's own climb down, set up as ladder2Check does
void climb(cSubChar* a)
{
    a->flg.off(cSubChar::F_MOVE_TO).off(cSubChar::F_DONT_RUN);
    a->status.off(cSubChar::S_ARRIVED);
    a->setPos(&gUse);
    Vec r = {0.0f, gAng, 0.0f};
    a->setAng(&r);
    a->m_Work0 = 1;
    a->m_Work1 = (gLevel < 0 ? -gLevel : gLevel) - 2;
    a->atari.off();
    a->dmg.set(0, 0x80);
    a->setRno(0, 0xC, 0, 0);
    re4_log("ashley ladder: she climbs down (%d rungs)", a->m_Work1);
}

void stop(cSubChar* a, const char* why)
{
    if (a->flg.check(cSubChar::F_MOVE_TO)) {
        a->flg.off(cSubChar::F_MOVE_TO).off(cSubChar::F_DONT_RUN);
        a->status.off(cSubChar::S_ARRIVED);
        if (a->r_no_0 == 0 && a->r_no_1 == 1) {
            a->setRno(0, 0, 0, 0);
        }
    }
    gWalk = 0;
    re4_log("ashley ladder: walk to the ladder ends (%s)", why);
}

}  // namespace

// (25.11.2026, DevJunior's report "noedcaleras": still no ladder taken, and not one "ashley ladder" line: the waits
// never reached findLadder) every 10 s while Leon is a metre or more below her, her state to the log
static void sayState(cSubChar* a)
{
    static int last = -100000;
    const int now = (int) pG->Frame_cnt;
    if (!pPL || pPL->pos.y > a->pos.y - kBelow || now - last < 300) {
        return;
    }
    last = now;
    re4_log("ashley ladder: %.1f m above Leon, routine %x/%x/%x, stay %d, move-to %d, sleep %d, event %d, window %d, "
            "Leon's routine %x/%x", (a->pos.y - pPL->pos.y) / 1000.0f, (unsigned) a->r_no_0, (unsigned) a->r_no_1,
            (unsigned) a->r_no_2, a->flg.check(cSubChar::F_STAY) ? 1 : 0, a->flg.check(cSubChar::F_MOVE_TO) ? 1 : 0,
            a->flg.check(cSubChar::F_SLEEP) ? 1 : 0, StaFlagChk(pG, STA_EVENT) ? 1 : 0,
            a->status.check(cSubChar::S_FALL_WINDOW) ? 1 : 0, (unsigned) pPL->r_no_0, (unsigned) pPL->r_no_1);
}

extern "C" void re4_ashley_ladder_down(cSubChar* a)
{
    if (a && pG && ((cModel*) a)->id == 3 && pG->pl_type == 0 && !gWalk) {
        sayState(a);
    }
    if (!a || !pPL || !pG || ((cModel*) a)->id != 3 || pG->pl_type != 0 || a->r_no_0 != 0) {
        gWalk = 0;
        gStillFrames = 0;
        return;
    }
    const bool leonOnLadder = pPL->r_no_0 == 0 && pPL->r_no_1 == 0x10;
    if (gWalk) {
        // the walk the player's call or an event took over: the game's
        if (!a->flg.check(cSubChar::F_MOVE_TO) || a->flg.check(cSubChar::F_STAY) || StaFlagChk(pG, STA_EVENT)) {
            gWalk = 0;
            re4_log("ashley ladder: walk to the ladder taken over");
            return;
        }
        if (++gWalk > kGiveUp || pPL->pos.y > a->pos.y - kBelow) {
            stop(a, gWalk > kGiveUp ? "too long" : "Leon came back up");
            return;
        }
        Vec d = gUse;
        const bool there = a->status.check(cSubChar::S_ARRIVED) || flat2(d, a->pos) < 150.0f * 150.0f;
        if (there && !leonOnLadder) {
            gWalk = 0;
            climb(a);
        }
        return;
    }
    if (a->flg.check(cSubChar::F_STAY) || a->flg.check(cSubChar::F_MOVE_TO) ||
        a->flg.check(cSubChar::F_SLEEP) || StaFlagChk(pG, STA_EVENT) || leonOnLadder ||
        pPL->pos.y > a->pos.y - kBelow) {
        gStillFrames = 0;
        gLast = a->pos;
        return;
    }
    // waiting at the top: for the catch (not at a window), at the ladder, or standing (still for kStill frames)
    bool waiting = false;
    if (a->r_no_1 == 0x12) {
        waiting = !a->status.check(cSubChar::S_FALL_WINDOW);
    } else if (a->r_no_1 == 0x13) {
        waiting = true;
    } else if (a->r_no_1 == 0 || a->r_no_1 == 1) {
        if (flat2(a->pos, gLast) < 30.0f * 30.0f) {
            gStillFrames++;
        } else {
            gStillFrames = 0;
            gLast = a->pos;
        }
        waiting = gStillFrames >= kStill;
    }
    if (!waiting || !findLadder(a, &gUse, &gAng, &gLevel)) {
        return;
    }
    gStillFrames = 0;
    if (flat2(gUse, a->pos) < 150.0f * 150.0f) {
        climb(a);
        return;
    }
    a->m_PlActTime = 0;  // not the ledge's way any more
    a->m_TargetPos = gUse;
    a->m_TargetDir = gAng;
    a->control(4);
    a->status.off(cSubChar::S_ARRIVED);
    gWalk = 1;
    re4_log("ashley ladder: she walks to the ladder (%.0f away, level %d)", sqrtf(flat2(gUse, a->pos)), gLevel);
}
