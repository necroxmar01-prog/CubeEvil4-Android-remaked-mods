// Knife parry (the app menu's Controls page, "Knife parry"; wish of DevJunior, 08.11.2026, in the spirit of the
// 2023 remake). Compiled with the game's flags into libre4_main. Off (the default): nothing here changes the game.
//
// The knife key (L; or, in the knife stance, the slash key A) pressed opens a short window (kWindow game frames).
// A blow that would land in it is turned aside: EmAtkHitCk2 (em_sub.cpp, every enemy's attack sphere against
// Leon; patch 0900) asks re4_parry_hit first and misses then, and so does every blow for kGuard frames after it
// (the rest of that swing). Leon answers with his own knife slash: the knife routine's slash step (r_no_1 0xB,
// r_no_2 2, as the routine sets it on the fire key), the knife key kept down until his stance is up; the slash
// hits the attacker as any knife slash does (he is at arm's length), so it staggers him by the game's own rules.
// Grabs and the like (SetPlDamage, PlSetDamage called straight) are not attack spheres and are not parried.
// The clash rings as metal (device report 08.11.2026: no sound): the enemies' own armour hit, SE 0xD of the
// attacker's block (SndCall(8, 0xD, pos, em->id, 0, em), as em10 / em25 / em2c / em2d / em32 play it when a hit
// strikes armour), the attacker being the enemy nearest the blow; for the villagers' family (em10's ids, those
// of Dmg_tbl_em10) only, where that SE is the armour's for sure.
#include "types.h"
#include "global.h"
#include "main.h"
#include "joy.h"
#include "player.h"
#include "em.h"
#include "snd.h"

extern cPlayer* pPL;
extern "C" int re4_parry_on(void);  // runtime/aim_options.cpp
extern "C" void re4_log(const char* fmt, ...);

namespace {

const int kWindow = 8;   // game frames after the key's press in which a blow is parried (about a quarter second)
const int kGuard = 24;   // game frames after a parry in which no blow lands
const int kSlash = 10;   // game frames the slash may wait for his knife stance

int gWindow = 0;
int gGuard = 0;
int gSlash = 0;
int gParries = 0;

// The villagers' family (em10, em_dm_val.cpp's Dmg_tbl_em10 ids): SE 0xD of their block is the armour hit.
bool villager(int id)
{
    return (id >= 0x10 && id <= 0x20) || id == 0x22 || id == 0x25 || id == 0x3A;
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
        SndCall(8, 0xD, pos, best->id, 0, best);
    }
}

}  // namespace

extern "C" {

// Patch 0900's hook (re4_player_input, cPlayer::keyConfig, before the routines read the keys).
void re4_parry_input(cPlayer* pl)
{
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
        }
        return 1;
    }
    return 0;
}

}  // extern "C"
