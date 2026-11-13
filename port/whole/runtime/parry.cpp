// Knife parry (the app menu's Controls page, "Knife parry"; wish of DevJunior, 08.11.2026, in the spirit of the
// 2023 remake). Compiled with the game's flags into libre4_main. Off (the default): nothing here changes the game.
//
// The knife key (L; or, in the knife stance, the slash key A) pressed opens a short window (kWindow game frames).
// A blow that would land in it is turned aside: EmAtkHitCk2 (em_sub.cpp, every enemy's attack sphere against
// Leon; patch 0900) asks re4_parry_hit first and misses then, and so does every blow for kGuard frames after it
// (the rest of that swing). Leon answers with his own knife slash: the knife routine's slash step (r_no_1 0xB,
// r_no_2 2, as the routine sets it on the fire key), the knife key kept down until his stance is up; the slash
// hits the attacker as any knife slash does (he is at arm's length), so it staggers him by the game's own rules.
// Grabs and the like (SetPlDamage, PlSetDamage called straight) are not attack spheres and are not parried; but a
// Ganado's hold (em10: the front choke NeckHang, the hold from behind Backhold, the dynamite hold Bombhold; patch
// 0917; wish of DevJunior, 12.11.2026, the remake's knife escape) breaks with the knife key within kGrabWindow game
// frames of the grab: the Ganado goes at once into the game's own break-free move, as after a won button mash (Leon
// kicks or elbows him off, the Ganado takes the damage of that move and goes down). Later in the hold only the
// game's mash frees him.
// The clash rings as metal: the villagers' own "weapon knocked away" SE, 0x3F of the attacker's block (em10
// gives every axe, sickle and knife it throws setSeDamage(8, 0x3F, em->id): the clink when Leon shoots it out
// of the air), else their armour hit 0xD (em10 / em25 / em2c / em2d / em32 when a hit strikes armour); the
// attacker being the villager (em10's ids, those of Dmg_tbl_em10) nearest the blow, the sound at his position
// as the game plays its own (0.99.4-mods 6b5329d played only 0xD, at the blow, and stayed silent on the device:
// the plain villagers' block has no armour hit).
#include "types.h"
#include "global.h"
#include "main.h"
#include "joy.h"
#include "player.h"
#include "em.h"
#include "snd.h"
#include "pl_body.h"
#include "pl_sub.h"
#include "motion.h"
#include "game.h"
#include "em_sub.h"
#include "em10.h"

extern cPlayer* pPL;
extern "C" int re4_parry_on(void);  // runtime/aim_options.cpp
extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_knife_tap_input(void* pl);  // runtime/melee.cpp: a tap of the knife key slashes (patch 0922)
extern "C" void re4_knife_parry_mark(void);
extern "C" void* re4_csaw_motion(int i);  // runtime/melee.cpp: the mod's chainsaw clash (0 the block, 1 the push)

namespace {

const int kWindow = 8;   // game frames after the key's press in which a blow is parried (about a quarter second)
const int kGuard = 24;   // game frames after a parry in which no blow lands
const int kSlash = 10;   // game frames the slash may wait for his knife stance
const int kGrabWindow = 30;  // game frames after a grab in which the knife key breaks it (one second)

int gWindow = 0;
int gGuard = 0;
int gSlash = 0;
int gParries = 0;
const void* gGrabEm = 0;  // the enemy whose hold started last (patch 0917)
int gGrabFrames = 0;      // game frames of that hold so far
int gEscapes = 0;

// The villagers' family (em10, em_dm_val.cpp's Dmg_tbl_em10 ids): SE 0xD of their block is the armour hit.
bool villager(int id)
{
    return (id >= 0x10 && id <= 0x20) || id == 0x22 || id == 0x25 || id == 0x3A;
}

// The enemy whose blow was turned aside: the nearest active one to the blow (runtime/melee.cpp's parry answer hits
// him; the mod's parry moves do not swing the blade at him as the knife's slash does)
cEm* gAttacker;

void noteAttacker(Vec* pos)
{
    gAttacker = 0;
    f32 bestD = 1500.0f * 1500.0f;
    for (cEm* m = EmMgr.getActiveWork(); m != 0; m = EmMgr.getNext(m)) {
        const f32 dx = m->pos.x - pos->x;
        const f32 dz = m->pos.z - pos->z;
        const f32 d = dx * dx + dz * dz;
        if (d < bestD && m->hp > 0) {
            bestD = d;
            gAttacker = m;
        }
    }
}

// The metal clash at the blow: the armour hit of the nearest villager within reach (none: silent).
void clash(Vec* pos)
{
    cEm* best = 0;
    f32 bestD = 2500.0f * 2500.0f;
    for (cEm* m = EmMgr.getActiveWork(); m != 0; m = EmMgr.getNext(m)) {
        if (!villager(m->id)) {
            continue;
        }
        const f32 dx = m->pos.x - pos->x;
        const f32 dz = m->pos.z - pos->z;
        const f32 d = dx * dx + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = m;
        }
    }
    if (best) {
        const u32 se = SndCall(8, 0x3F, &best->pos, best->id, 0, best);
        if (se == 0) {
            SndCall(8, 0xD, &best->pos, best->id, 0, best);
        }
        re4_log("parry: clash SE %s (enemy id 0x%x)", se ? "0x3F" : "0xD", (unsigned) best->id);
    } else {
        re4_log("parry: no villager near the blow, no clash SE");
    }
}

}  // namespace

extern "C" {

// Patch 0900's hook (re4_player_input, cPlayer::keyConfig, before the routines read the keys).
void re4_parry_input(cPlayer* pl)
{
    re4_knife_tap_input(pl);
    if (!pl || !re4_parry_on() || pG->pl_type == 1) {
        gWindow = gGuard = gSlash = 0;
        return;
    }
    if (gWindow > 0) {
        gWindow--;
    }
    if (gGuard > 0) {
        gGuard--;
    }
    const bool knifeKey = (Key.trg & 0x800) || (Joy[0].trg & 0x40);
    const bool slashKey = pl->r_no_1 == 0xB && (Key.trg & 0x80);
    if (pl->r_no_0 == 0 && (knifeKey || slashKey)) {
        gWindow = kWindow;
    }
    if (gSlash > 0) {
        gSlash--;
        if (pl->r_no_0 != 0) {
            gSlash = 0;  // hurt or in an event after all
        } else if (pl->r_no_1 == 0xB) {
            if (pl->r_no_2 != 2) {
                pl->r_no_2 = 2;  // the slash, as pl_knife's stance step sets it on the fire key
                pl->r_no_3 = 0;
                re4_knife_parry_mark();  // a mod's parry answers it (runtime/melee.cpp, patch 0922)
            }
            gSlash = 0;
        } else {
            Key.on |= 0x800;  // the knife key held until his stance is up
            Joy[0].on |= 0x40;
        }
    }
}

// Patch 0900 (EmAtkHitCk2, em_sub.cpp, after an enemy's attack sphere at pos found Leon): 1 = this blow misses.
int re4_parry_hit(Vec* pos)
{
    if (!re4_parry_on()) {
        return 0;
    }
    if (gGuard > 0) {
        return 1;
    }
    if (gWindow > 0 && pPL && pPL->r_no_0 == 0) {
        gWindow = 0;
        gGuard = kGuard;
        gSlash = kSlash;
        gParries++;
        re4_log("parry: a blow turned aside (%d so far)", gParries);
        if (pos) {
            clash(pos);
            noteAttacker(pos);
        }
        return 1;
    }
    return 0;
}

}  // extern "C"

namespace {

// The chainsaw clash (wish of DevJunior, 13.11.2026: "the clash when you parry the chainsaw man, as in the remake";
// patch 0924). The game's own struggle when the saw catches Leon (em10 C_SawHit, R1 0x30: the button mash PlGacha,
// more than 30 presses or the end of the saw's motion break him free, the Ganado staggers back with 0xFE; a failed
// mash kills him) becomes the clash when the knife key's parry window is open as the saw connects: Leon holds the
// saw off with his knife (the mod's chc0_jack_pl_2nd_wp5300 1200, melee.dat entry 24: its block, held from kHold
// while he mashes, then its push), no blood, no death timer, the metal clash sound instead of the cutting one. The
// overhead swing (C_SawCriAtk, otherwise the one-shot decapitation C_SawCriHit) goes into the same clash when parried.
// Without the mod's motions Leon struggles with the game's own 0x101 / 0x102, still without dying.
const f32 kHold = 60.0f;     // the block's frame he holds while the mash goes on
const int kClashSe = 12;     // frames between the metal clash sounds
cEm* gClashEm;               // the chainsaw Ganado in the clash
int gClashSe;
bool gClashCut;              // the cutting sound stopped

void endClash(cPlayer* pl)
{
    pl->Body->setKnife(false);
    setWepTrans(pl, 1);
    EndPlDamage();
    pl->dmg.set(0, 30);
    gClashEm = 0;
}

// Leon's routine of the clash (in place of plem10_C_SawHit): r_no_2 0 start, 1 the block, 2 / 3 the push
void plCsawClash(cPlayer* pl)
{
    StaFlagOn(pG, STA_PL_CATCHED);
    pl->dmg.set(0, 10);
    pl->subArc = pl->pEmCatch->subArc;
    pl->dmg.set(0, 2);
    cEm* em = pl->pEmCatch;
    void* block = re4_csaw_motion(0);
    switch (pl->r_no_2) {
    case 0:
        MotionSetCore(pl, &pl->Motion, block ? block : PL_ARC_PTR(pl->subArc, 0x101), 0, 5, 1, 0);
        setWepTrans(pl, 0);
        pl->Body->setKnife(true);
        pl->atari.set(400.0f, 700.0f, 10);
        pl->r_no_2++;
    case 1:
        if (block && pl->Motion.Seq_frame >= kHold) {
            pl->Motion.Seq_speed = 0.0f;  // the knife against the saw while he mashes
        }
        EmCatchMotionMove(pl, 0.3f, 0.2f);
        if (em->r_no_0 != 1 || em->r_no_1 != 0x30) {
            endClash(pl);
            break;
        }
        if (em->r_no_2 >= 2) {
            pl->r_no_2 = 2;
        }
        break;
    case 2:
        if (block) {
            pl->Motion.Seq_speed = 1.0f;  // on into the push
        } else {
            MotionSetCore(pl, &pl->Motion, PL_ARC_PTR(pl->subArc, 0x102), 0, 5, 1, 0);
        }
        re4_log("parry: Leon broke free of the chainsaw clash");
        pl->r_no_2++;
    case 3:
        if (MotionMove(pl, 0)) {
            endClash(pl);
        }
        break;
    }
    pl->Catch_at_adj = pl->pos;
    pl->subArc = pl->subArc2;
}

}  // namespace

extern "C" {

// Patch 0924 (em10.cpp em10_R1_br_C_SawCriAtk): 1 when the knife key's parry window is open (the overhead swing then
// takes the clash, R1 0x30, instead of the decapitation)
int re4_csaw_parry_open(void)
{
    return re4_parry_on() && pG->pl_type == 0 && gWindow > 0 && pPL && pPL->r_no_0 == 0;
}

// Patch 0924 (em10_R1_C_SawHit, its first frame): Leon's routine for the catch: the clash's when parried, else `orig`
void* re4_csaw_clash_routine(void* em, void* orig)
{
    if (!re4_csaw_parry_open()) {
        gClashEm = 0;
        return orig;
    }
    gWindow = 0;
    gParries++;
    gClashEm = (cEm*) em;
    gClashSe = 0;
    gClashCut = false;
    re4_log("parry: the chainsaw parried: the clash (%d parries so far)", gParries);
    return (void*) plCsawClash;
}

// Patch 0924 (em10_R1_C_SawHit, its struggle step, each frame): in the clash no death timer, no blood on the saw, the
// metal clash instead of the cutting sound
void re4_csaw_clash_frame(void* p)
{
    cEm* em = (cEm*) p;
    if (em != gClashEm) {
        return;
    }
    FREE_EM10* w = EM10_WK(em);
    w->Timer = 2;   // the death comes when Timer and Timer2 have run out
    w->Timer3 = 2;  // the blood on the saw every other frame when it runs out
    if (!gClashCut) {
        gClashCut = true;
        SndStop(w->TmpU32, 0);
        w->TmpU32 = 0;
    }
    if (gClashSe-- <= 0) {
        gClashSe = kClashSe;
        SndCall(8, 0x3F, &em->pos, em->id, 0, em);
    }
}

// runtime/melee.cpp: the attacker of the last parried blow while still active and alive, else 0
void* re4_parry_attacker(void)
{
    for (cEm* m = EmMgr.getActiveWork(); m != 0; m = EmMgr.getNext(m)) {
        if (m == gAttacker) {
            return m->hp > 0 ? m : 0;
        }
    }
    return 0;
}

// Patch 0917 (em10.cpp, the first frame of a Ganado's hold on Leon: NeckHang, Backhold, Bombhold).
void re4_grab_start(void* em)
{
    gGrabEm = em;
    gGrabFrames = 0;
}

// Patch 0917 (the same holds, each frame Leon is held): 1 = the knife key broke the hold now; the enemy's routine then
// goes into its break-free step.
int re4_grab_escape(void* em)
{
    if (!re4_parry_on() || pG->pl_type == 1 || em != gGrabEm) {
        return 0;
    }
    if (gGrabFrames > kGrabWindow) {
        return 0;
    }
    gGrabFrames++;
    if ((Key.trg & 0x800) || (Joy[0].trg & 0x40)) {
        gGrabFrames = kGrabWindow + 1;  // once per hold
        gEscapes++;
        re4_log("parry: a hold broken with the knife (%d so far)", gEscapes);
        return 1;
    }
    return 0;
}

}  // extern "C"
