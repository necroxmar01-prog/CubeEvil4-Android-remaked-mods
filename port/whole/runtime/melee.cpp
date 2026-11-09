// Leon's melee moves (wish of DevJunior, 09.11.2026: "a system for different melee animations", the remake mods).
// Compiled with the game's flags into libre4_main (re4_gamehooks_obj); patch 0904 lets em10.cpp ask here, the app
// sends its two switches with the second aim word (runtime/aim_options.cpp: bit 24 "Neck break", bit 25 the mods'
// motions off).
//
//  - Custom motions: a mod pack's melee/melee.dat (port/tools/uhd_motion_convert.py, from the PC release's FCV
//    files) is a DAT of FCV entries, 0 the move on a standing stunned Ganado (the kick, the enemy archives' entry
//    0x29D), 1 the move on a kneeling one (Leon's archive, entry 0x25); an empty entry keeps the game's own. It is
//    read once to RE4_MEM1_MELEE_ADDR and converted as a read archive is (under the name mods/melee.drs: the
//    converters go by the name). Only the animation changes: the hit
//    (frame 0x11, the sweep around Leon) stays the kick's.
//  - Neck break: Leon finishes a standing stunned Ganado as HUNK does in the Mercenaries (em10.cpp Dm_NeckBreak and
//    plem10NeckBreak, always fatal). The three motions (0x2B9 Leon's, 0x2BA / 0x2BB the Ganado's) are HUNK's from
//    em/em60.drs (runtime/cheats.cpp re4_melee_hunk_entry); without them Leon kicks as ever. HUNK's own rule
//    holds: an enemy with a parasite or a core is kicked.
//  - Axe strike (wish of DevJunior, 09.11.2026: "his Requiem axe on kneeling enemies"): on a kneeling stunned
//    Ganado Leon draws his knife (whatever a skin made of it: the axe) and chops down instead of the kick, from
//    the game's own knife motions of his archive: the up stance 0x83 as the wind-up, then the low slash 0x86 (the
//    deep crouch with the blade down, the root falling from 1065 to 746). The hit is the kick's (PlWepHitCheck3,
//    kind 0x14, 1200 around), lower and further out, on the chop's lowest frame; the gun is hidden meanwhile as
//    the knife routine does (setWepTrans) and shown again at the end, or by the frame hook when something cut
//    the move short.
// Only Leon (pl_type 0); the other characters keep their moves.
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
#include "game.h"
#include "em.h"
#include "em_sub.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void* re4_melee_hunk_entry(unsigned no);  // runtime/cheats.cpp
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern cPlayer* pPL;

namespace {

std::atomic<int> gBits(0);  // the app's switches: bit 0 "Neck break", bit 1 the mods' motions off, bit 2 "Axe strike"
char gLoaded[1600];         // the archive at RE4_MEM1_MELEE_ADDR ("" none; a path that failed counts as read)
void* gMotion[2];           // its motions: on a standing enemy, on a kneeling one
int gSaid;                  // what the log told already (bits by kind)

u32 be32At(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

// The active mods' melee archive, read anew when another pack holds it
void load()
{
    const char* path = re4_mod_melee_path();
    if (strcmp(path, gLoaded) == 0) {
        return;
    }
    snprintf(gLoaded, sizeof(gLoaded), "%s", path);
    gMotion[0] = gMotion[1] = 0;
    gSaid &= ~3;
    if (!*path) {
        return;
    }
    u8* const base = (u8*) (unsigned long) RE4_MEM1_MELEE_ADDR;
    FILE* f = fopen(path, "rb");
    const size_t size = f ? fread(base, 1, RE4_MEM1_MELEE_MAX, f) : 0;
    const bool whole = f && fgetc(f) == EOF;
    if (f) {
        fclose(f);
    }
    const u32 count = size >= 0x20 ? be32At(base) : 0;
    u32 at[2] = {0, 0}, len[2] = {0, 0};
    bool ok = whole && count >= 1 && count <= 16 && 0x10 + count * 8 <= size;
    for (u32 i = 0; ok && i < count && i < 2; i++) {
        at[i] = be32At(base + 0x10 + i * 4);
        const u32 end = i + 1 < count ? be32At(base + 0x10 + (i + 1) * 4) : (u32) size;
        ok = at[i] >= 0x10 + count * 8 && at[i] <= end && end <= size && !(at[i] & 3);
        len[i] = ok ? end - at[i] : 0;
        // a motion: u16 last frame, u8 joints, the table and at least one offset
        if (ok && len[i] && (len[i] < 0x10 || base[at[i] + 2] == 0)) {
            ok = false;
        }
    }
    if (!ok) {
        re4_log("melee: %s is no melee archive of at most %u bytes (%u read, %u entries): the game's moves", path,
                (unsigned) RE4_MEM1_MELEE_MAX, (unsigned) size, (unsigned) count);
        return;
    }
    const unsigned frames[2] = {len[0] ? (unsigned) (base[at[0]] << 8 | base[at[0] + 1]) : 0,
                                len[1] ? (unsigned) (base[at[1]] << 8 | base[at[1] + 1]) : 0};
    const unsigned joints[2] = {len[0] ? base[at[0] + 2] : 0u, len[1] ? base[at[1] + 2] : 0u};
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = (u32) size;
    h.dest = RE4_MEM1_MELEE_ADDR;
    re4_formats_part_loaded("mods/melee.drs", 0, 0, &h, RE4_MEM1_MELEE_ADDR);
    for (int i = 0; i < 2; i++) {
        gMotion[i] = len[i] ? base + at[i] : 0;
    }
    re4_log("melee: %s read to 0x%08x: standing %u bytes (%u frames, %u joints), kneeling %u bytes (%u frames, "
            "%u joints)",
            path, (unsigned) RE4_MEM1_MELEE_ADDR, (unsigned) len[0], frames[0], joints[0], (unsigned) len[1],
            frames[1], joints[1]);
}

bool leon()
{
    return pG && pG->pl_type == 0;
}

void say(int kind, const char* what)
{
    if (!(gSaid & (1 << kind))) {
        gSaid |= 1 << kind;
        re4_log("melee: %s", what);
    }
}

// The axe strike (see the head): its steps in r_no_2, the frames left of each in m_Work0, the turn to the enemy in
// m_Work1
const int kWindUp = 9;     // frames of the wind-up (the up stance, blended in over 6)
const int kHitFrame = 8;   // frame of the chop (0x86) that hits: the blade at its lowest
const int kCancel = 18;    // frames of the chop before an aim or fire key may end it
bool gAxeLive = false;     // the knife is out for the axe strike (the gun hidden)

void axeEnd(cPlayer* pl)
{
    if (gAxeLive) {
        gAxeLive = false;
        pl->Body->setKnife(false);
        setWepTrans(pl, 1);
    }
}

void axeHit(cPlayer* pl)
{
    static const f32 kHeights[2] = {1000.0f, 550.0f};
    bool hit = false;
    for (int i = 0; i < 2; i++) {
        Vec v = {0.0f, kHeights[i], 450.0f};
        PSMTXMultVec(pl->mat, &v, &v);
        hit = PlWepHitCheck3(&v, 0x14, 1200.0f, 0xA) || hit;
    }
    if (hit) {
        SndCall(1, 0xF, &pl->pos, 0, 0, pPL);
    }
}

void plAxeStrike(cPlayer* pl)
{
    cEm* em = pl->pEmCatch;
    PlArc* arc = pG->pPlayer;

    pl->subArc = em->subArc;
    pl->dmg.set(0, 0x1E);
    StaFlagOn(pG, STA_PL_EM_ACTION);
    switch (pl->r_no_2) {
    case 0:
        MotionSetCore(pl, &pl->Motion, PL_ARC_PTR(arc, 0x83), 0, 6, 1, 0);
        setWepTrans(pl, 0);
        pl->Body->setKnife(true);
        gAxeLive = true;
        GameAddPoint(LVADD_CRITICALHIT);
        pl->m_Work0 = kWindUp;
        pl->m_Work1 = 10;
        pl->r_no_2 = 1;
        // fall through
    case 1:
        MotionMove(pl, 0);
        if (pl->m_Work0 && --pl->m_Work0) {
            break;
        }
        MotionSetCore(pl, &pl->Motion, PL_ARC_PTR(arc, 0x86), 0, 3, 1, 0);
        EstSet(pl, -1, 0, 0, EFF_CORE, 0x2B, 0, ESP_CORE_KIND_PL_WEP, 0, 0);
        SndCall(1, 3, &pl->pos, 0, 0, pPL);
        pl->m_Work0 = kHitFrame;
        pl->m_Work2 = kCancel;
        pl->r_no_2 = 2;
        break;
    case 2:
        if (pl->m_Work0) {
            pl->m_Work0--;
            if (pl->m_Work0 == 0) {
                axeHit(pl);
            }
        }
        if (MotionMove(pl, 0)) {
            axeEnd(pl);
            EndPlDamage();
            pl->dmg.set(0, 0xF);
        } else if (pl->m_Work2) {
            pl->m_Work2--;
        } else if (joyKamae() || (Key.on & 0x10F)) {
            axeEnd(pl);
            EndPlDamage();
            pl->dmg.set(0, 0xF);
        }
        break;
    }
    if (pl->m_Work1 > 0) {
        pl->m_Work1--;
        pl->ang.y += Muku(&pl->pos, &em->pos, pl->ang.y, 0.19634955f);
        pl->ang.y = LIMIT_ANGLE(pl->ang.y);
    }
    pl->subArc = pl->subArc2;
}

// A grab or a hit that cut the axe strike short: the knife away and the gun back
void axeWatch(unsigned)
{
    if (gAxeLive && pPL && (pPL->r_no_0 != 4 || Pl_func_tbl[4] != plAxeStrike)) {
        re4_log("melee: the axe strike was cut short (routine %d): knife away, gun shown", (int) pPL->r_no_0);
        axeEnd(pPL);
    }
}

}  // namespace

extern "C" {

void* re4_melee_kneel(void* game)
{
    if (!leon() || !(gBits.load() & 4) || !pPL || !pPL->Body) {
        return game;
    }
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        re4_add_frame_hook(axeWatch);
    }
    say(6, "Leon's axe strike on a kneeling enemy (his knife's motions 0x83 / 0x86)");
    return (void*) plAxeStrike;
}

void re4_melee_set(int bits)
{
    if (gBits.exchange(bits) != bits) {
        re4_log("melee: neck break %s, the mods' melee motions %s, axe strike on kneeling enemies %s",
                bits & 1 ? "on" : "off", bits & 2 ? "off" : "on", bits & 4 ? "on" : "off");
    }
}

void* re4_melee_motion(int kind, void* game)
{
    if (!leon()) {
        return game;
    }
    if (kind == 0 || kind == 1) {
        if (gBits.load() & 2) {
            return game;
        }
        load();
        if (!gMotion[kind]) {
            return game;
        }
        say(kind, kind ? "the mod's move on a kneeling enemy plays" : "the mod's move on a standing enemy plays");
        return gMotion[kind];
    }
    if (kind >= 2 && kind <= 4) {
        void* own = re4_melee_hunk_entry(0x2B9 + (unsigned) (kind - 2));
        return own ? own : game;
    }
    return game;
}

int re4_melee_neck(int special)
{
    if (!leon() || !(gBits.load() & 1) || special) {
        return 0;
    }
    if (!re4_melee_hunk_entry(0x2B9) || !re4_melee_hunk_entry(0x2BA) || !re4_melee_hunk_entry(0x2BB)) {
        say(5, "no neck break: HUNK's motions (em/em60.drs, entries 0x2B9..0x2BB) are not on the disc; Leon kicks");
        return 0;
    }
    say(4, "Leon breaks a stunned Ganado's neck (HUNK's motions from em/em60.drs)");
    return 1;
}

}  // extern "C"
