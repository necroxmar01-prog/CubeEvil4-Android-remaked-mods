// Leon's melee moves (wish of DevJunior, 09.11.2026: "a system for different melee animations", the remake mods).
// Compiled with the game's flags into libre4_main (re4_gamehooks_obj); patch 0904 lets em10.cpp ask here, the app
// sends its switches with the second aim word (runtime/aim_options.cpp: bit 24 "Neck break", bit 25 the mods'
// motions off, bit 26 "Axe strike", bit 27 the random mix off).
//
//  - Custom motions: a mod pack's melee/melee.dat (port/tools/uhd_motion_convert.py, from the PC release's FCV
//    files) is a DAT of FCV entries, the even ones moves on a standing stunned Ganado (the kick, the enemy
//    archives' entry 0x29D), the odd ones on a kneeling one (Leon's archive, entry 0x25), up to kVariants of
//    each; an empty entry is none. It is
//    read once to RE4_MEM1_MELEE_ADDR and converted as a read archive is (under the name mods/melee.drs: the
//    converters go by the name). Only the animation changes: the hit
//    (frame 0x11, the sweep around Leon) stays the kick's.
//  - Neck break: Leon finishes a standing stunned Ganado as HUNK does in the Mercenaries (em10.cpp Dm_NeckBreak and
//    plem10NeckBreak, always fatal). The three motions (0x2B9 Leon's, 0x2BA / 0x2BB the Ganado's) are HUNK's from
//    em/em60.drs (runtime/cheats.cpp re4_melee_hunk_entry); without them Leon kicks as ever. HUNK's own rule
//    holds: an enemy with a parasite or a core is kicked.
//  - Axe strike (wishes of DevJunior, 09.11.2026: "his Requiem axe on kneeling enemies", "the Ganados' own axe
//    attack, nothing new"): on a kneeling stunned Ganado Leon draws his knife (whatever a skin made of it: the
//    axe, in his right hand, bone 10, where a Ganado holds his) and swings the Ganados' axe attack (em10.cpp
//    em10_R1_AxeAtk: the enemy archive's motion 0x80 and its sequence 0x81, the archive Leon's moves on the
//    Ganado come from as the kick does). The rigs share their part numbers; a copy of the motion
//    (RE4_MEM1_MELEE_AXE_ADDR) leaves out the joints Leon's skeleton uses for other things (parts 26..31 and 33:
//    the Ganado's jaw, eyes and weapon) and shortens the step forward of the root (the Ganado's reaches about a
//    metre; at most kRootMax). Its sequence marks the swing (0x20: the knife's swing SE) and the hit frames (1:
//    the kick's hit, PlWepHitCheck3 kind 0x14, 1200 around, on the third); Leon stops at frame kAxeEnd, before
//    the Ganado's long recovery. Without a usable motion (another archive) the knife's own: the up stance 0x83
//    as the wind-up, the low slash 0x86 as the chop. The gun is hidden meanwhile as the knife routine does
//    (setWepTrans) and shown again at the end, or by the frame hook when something cut the move short.
//  - Random mix (wish of DevJunior, 09.11.2026: "the random system of animations"): each move on a stunned
//    Ganado is drawn among the game's own and every one switched on (the mods' variants, the neck break, the axe
//    strike), not the same twice in a row; with the mix off the app's choice always plays (the neck break, the
//    axe, the mod's first, in that order).
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
#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void* re4_melee_hunk_entry(unsigned no);  // runtime/cheats.cpp
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern cPlayer* pPL;

namespace {

// the app's switches: bit 0 "Neck break", bit 1 the mods' motions off, bit 2 "Axe strike", bit 3 the random mix off
std::atomic<int> gBits(0);
const int kVariants = 8;      // a mod's moves of each kind
char gLoaded[1600];           // the archive at RE4_MEM1_MELEE_ADDR ("" none; a path that failed counts as read)
void* gMod[2][kVariants];     // its motions: on a standing enemy, on a kneeling one
int gMods[2];                 // how many of each
int gSaid;                    // what the log told already (bits by kind)

// What the last prompt drew (re4_melee_neck / re4_melee_kneel), for the routine that plays it: -1 the game's own,
// 0..kVariants-1 a mod's, kNeck / kAxe; gDrawn the prompt's (cleared when taken)
enum { kGame = -1, kNeck = 100, kAxe = 101 };
int gPick[2] = {kGame, kGame};
int gLast[2] = {-2, -2};
bool gDrawn[2];
u32 gSeed = 0x2545F491u;

u32 be32At(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

u16 get16(const u8* p)
{
    u16 v;
    memcpy(&v, p, 2);
    return v;
}

// The active mods' melee archive, read anew when another pack holds it
void load()
{
    const char* path = re4_mod_melee_path();
    if (strcmp(path, gLoaded) == 0) {
        return;
    }
    snprintf(gLoaded, sizeof(gLoaded), "%s", path);
    memset(gMod, 0, sizeof(gMod));
    gMods[0] = gMods[1] = 0;
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
    enum { kMax = 2 * kVariants };
    u32 at[kMax], len[kMax];
    bool ok = whole && count >= 1 && count <= kMax && 0x10 + count * 8 <= size;
    for (u32 i = 0; ok && i < count; i++) {
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
        re4_log("melee: %s is no melee archive of at most %u bytes and %d entries (%u read, %u entries): the game's "
                "moves",
                path, (unsigned) RE4_MEM1_MELEE_MAX, (int) kMax, (unsigned) size, (unsigned) count);
        return;
    }
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = (u32) size;
    h.dest = RE4_MEM1_MELEE_ADDR;
    re4_formats_part_loaded("mods/melee.drs", 0, 0, &h, RE4_MEM1_MELEE_ADDR);
    for (u32 i = 0; i < count; i++) {
        if (len[i]) {
            const int k = (int) (i & 1);
            gMod[k][gMods[k]++] = base + at[i];
            re4_log("melee: %s entry %u: a move on a %s enemy, %u bytes (%u frames, %u joints)", path, (unsigned) i,
                    k ? "kneeling" : "standing", (unsigned) len[i], (unsigned) get16(base + at[i]),
                    (unsigned) base[at[i] + 2]);
        }
    }
    re4_log("melee: %s read to 0x%08x: %d moves on a standing enemy, %d on a kneeling one", path,
            (unsigned) RE4_MEM1_MELEE_ADDR, gMods[0], gMods[1]);
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

bool hunkReady()
{
    return re4_melee_hunk_entry(0x2B9) && re4_melee_hunk_entry(0x2BA) && re4_melee_hunk_entry(0x2BB);
}

// One of the candidates (count > 0), not the last one drawn while there are others
int draw(int k, const int* cand, int count)
{
    if (count == 1) {
        return cand[0];
    }
    for (int tries = 0; tries < 8; tries++) {
        gSeed = gSeed * 1664525u + 1013904223u;
        const int c = cand[(gSeed >> 16) % (u32) count];
        if (c != gLast[k]) {
            return c;
        }
    }
    return cand[0] != gLast[k] ? cand[0] : cand[1];
}

// The move of kind k (0 on a standing enemy, 1 on a kneeling one) for this prompt; `extra` the neck break or the
// axe strike when switched on and possible
int pick(int k, int extra)
{
    const int bits = gBits.load();
    if (!(bits & 2)) {
        load();
    }
    const int mods = (bits & 2) ? 0 : gMods[k];
    int cand[kVariants + 2];
    int n = 0;
    if (bits & 8) {  // the mix off: the app's choice
        cand[n++] = extra != kGame ? extra : mods ? 0 : kGame;
    } else {
        cand[n++] = kGame;
        for (int i = 0; i < mods; i++) {
            cand[n++] = i;
        }
        if (extra != kGame) {
            cand[n++] = extra;
        }
    }
    const int c = draw(k, cand, n);
    gLast[k] = c;
    gPick[k] = c;
    gDrawn[k] = true;
    if (n > 1 || c != kGame) {
        char mod[24] = "";
        if (c >= 0 && c < kVariants) {
            snprintf(mod, sizeof(mod), " %d", c + 1);
        }
        re4_log("melee: on a %s enemy: %s%s (%d to draw from)", k ? "kneeling" : "standing",
                c == kGame ? "the game's" : c == kNeck ? "the neck break" : c == kAxe ? "the axe strike" : "the mod's move",
                mod, n);
    }
    return c;
}

// ---- The axe strike (see the head) ----

const int kAxeMot = 0x80;        // the Ganados' axe attack (em10_R1_AxeAtk) and its sequence
const int kAxeSeq = 0x81;
const int kAxeEnd = 50;          // Leon's last frame of it: the swing is over, the Ganado's recovery follows
const int kAxeCancel = 34;       // from this frame an aim or fire key ends it
const int kAxeHitAt = 3;         // the hit on the third of the sequence's hit frames
const f32 kRootMax = 350.0f;     // the root's step forward at most (Leon's own knee kick: 325)
const int kWindUp = 9;           // the knife's fallback: frames of the wind-up (the up stance, blended in over 6)
const int kHitFrame = 8;         // frame of its chop (0x86) that hits: the blade at its lowest
const int kCancel = 18;          // frames of its chop before an aim or fire key may end it
bool gAxeLive = false;           // the knife is out for the axe strike (the gun hidden)

// Bytes of a key in a curve's layout (motion.cpp Fcc_next_axis_addr), 0 for the layouts the game does not read
int keyBytes(int fmt)
{
    static const int kBytes[16] = {12, 8, 6, 0, 10, 6, 4, 0, 9, 5, 3, 0, 0, 0, 0, 4};
    return kBytes[fmt & 15];
}

// Leon's skeleton takes these parts of a Ganado's motion: the body 0..25 and 32 (an eye, his own motions move it)
bool leonPart(int part)
{
    return part <= 25 || part == 32;
}

// A copy of the Ganado's axe attack for Leon at RE4_MEM1_MELEE_AXE_ADDR (see the head), 0 when the archive has none
// that fits; the curves stay where they are in the copy, the joints table loses the ones he has no use for
void* axeMotion(PlArc* arc)
{
    if (!arc || arc->ofs[0] <= (u32) kAxeSeq || arc->ofs[0] > 0x1000) {
        return 0;
    }
    const u8* src = (const u8*) PL_ARC_PTR(arc, kAxeMot);
    const unsigned n = src[2];
    const u32 table = (3 + n * 3 + 3) & ~3u;
    u32 size;
    memcpy(&size, src + table, 4);
    const u32 head = table + 4 + n * 4;
    if (n == 0 || get16(src) < (u16) kAxeEnd || size < head || size > RE4_MEM1_MELEE_AXE_MAX) {
        say(8, "the Ganado's archive has no axe attack that fits (motion 0x80): the knife's chop");
        return 0;
    }
    u8* const dst = (u8*) (unsigned long) RE4_MEM1_MELEE_AXE_ADDR;
    memcpy(dst, src, size);
    u32 off[256];
    u16 kind[256];
    u8 part[256];
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        u32 o;
        memcpy(&o, src + table + 4 + i * 4, 4);
        if ((s32) o < 0) {  // relocated already (MotionSetCore ran on a Ganado)
            o -= (u32) (unsigned long) src;
        }
        const u16 k = get16(src + 3 + i * 2);
        const int ch = (k >> 8) & 0xF;
        if (o < head || o >= size || (!leonPart(src[3 + n * 2 + i]) && ch != 6 && ch != 7)) {
            continue;
        }
        off[m] = o;
        kind[m] = k;
        part[m] = src[3 + n * 2 + i];
        m++;
    }
    // every curve checked against the copy (motion.cpp HermiteInterpolation: per axis a key count, the keys' frames,
    // the keys in the curve's layout, Fcc_next_axis_addr): a curve that reaches past the motion, has more keys than
    // the motion has frames, or (float layouts) a value that is no number goes, rather than move Leon to nowhere
    // (DevJunior, 12.11.2026, device report: the step forward read 7.9e33 and the game hung in EmAtCheck)
    unsigned kept = 0;
    for (unsigned j = 0; j < m; j++) {
        const int fmt = kind[j] >> 12;
        const int stride = keyBytes(fmt);
        const u8* p = dst + off[j];
        bool fits = stride > 0;
        for (int a = 0; a < 3 && fits; a++) {
            const unsigned cnt = p + 2 <= dst + size ? get16(p) : 0xFFFFu;
            fits = cnt >= 1 && cnt <= (unsigned) get16(src) + 1u && p + 2 + cnt * 2 + cnt * stride <= dst + size;
            for (unsigned c = 0; fits && fmt <= 2 && c < cnt; c++) {
                f32 v;
                memcpy(&v, p + 2 + cnt * 2 + c * stride, 4);
                fits = v == v && fabsf(v) < 1.0e5f;
            }
            p += 2 + (fits ? cnt * 2 + cnt * stride : 0);
        }
        if (!fits) {
            re4_log("melee: the axe attack's curve %u (part %u, kind 0x%04x) does not read as a curve: left out", j,
                    (unsigned) part[j], (unsigned) kind[j]);
            continue;
        }
        off[kept] = off[j];
        kind[kept] = kind[j];
        part[kept] = part[j];
        kept++;
    }
    m = kept;
    // the root's step forward (kind 1, the model's travel; float keys of value and two slopes): at most kRootMax
    for (unsigned j = 0; j < m; j++) {
        if ((kind[j] & 0xFF) != 1 || (kind[j] >> 12) != 0) {
            continue;
        }
        u8* axes[3];
        unsigned counts[3];
        u8* p = dst + off[j];
        for (int a = 0; a < 3; a++) {
            counts[a] = get16(p);
            axes[a] = p;
            p += 2 + counts[a] * 2 + counts[a] * 12;
        }
        f32 most = 0.0f;
        for (int a = 0; a < 3; a += 2) {
            for (unsigned c = 0; c < counts[a]; c++) {
                f32 v;
                memcpy(&v, axes[a] + 2 + counts[a] * 2 + c * 12, 4);
                most = fabsf(v) > most ? fabsf(v) : most;
            }
        }
        const f32 scale = most > kRootMax ? kRootMax / most : 1.0f;
        for (int a = 0; a < 3 && scale < 1.0f; a += 2) {
            for (unsigned c = 0; c < counts[a]; c++) {
                for (int w = 0; w < 3; w++) {
                    u8* at = axes[a] + 2 + counts[a] * 2 + c * 12 + w * 4;
                    f32 v;
                    memcpy(&v, at, 4);
                    v *= scale;
                    memcpy(at, &v, 4);
                }
            }
        }
        static bool told = false;
        if (!told) {
            told = true;
            re4_log("melee: the axe attack's step forward %.0f, Leon's %.0f", (double) most,
                    (double) (most * scale));
        }
    }
    // the header with the joints kept (shorter than the Ganado's: the curves behind it stay)
    dst[2] = (u8) m;
    for (unsigned j = 0; j < m; j++) {
        memcpy(dst + 3 + j * 2, &kind[j], 2);
        dst[3 + m * 2 + j] = part[j];
    }
    const u32 t2 = (3 + m * 3 + 3) & ~3u;
    memset(dst + 3 + m * 3, 0, t2 - (3 + m * 3));
    memcpy(dst + t2, &size, 4);
    memcpy(dst + t2 + 4, off, m * 4);
    static bool logged = false;
    if (!logged) {
        logged = true;
        re4_log("melee: the Ganados' axe attack (motion 0x80, %u frames) for Leon: %u of %u joints, %u bytes at 0x%08x",
                (unsigned) get16(src), m, n, (unsigned) size, (unsigned) RE4_MEM1_MELEE_AXE_ADDR);
    }
    return dst;
}

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

void axeSwing(cPlayer* pl)
{
    EstSet(pl, -1, 0, 0, EFF_CORE, 0x2B, 0, ESP_CORE_KIND_PL_WEP, 0, 0);
    SndCall(1, 3, &pl->pos, 0, 0, pPL);
}

void axeFinish(cPlayer* pl)
{
    axeEnd(pl);
    EndPlDamage();
    pl->dmg.set(0, 0xF);
}

// Steps in r_no_2: 0 start, 1 the Ganado's swing (m_Work0 the hit frames seen, m_Work2 1 once hit), 10 / 11 the
// knife's wind-up and chop (m_Work0 the frames left); m_Work1 the frames of the turn to the enemy
void plAxeStrike(cPlayer* pl)
{
    cEm* em = pl->pEmCatch;
    PlArc* arc = pG->pPlayer;

    pl->subArc = em->subArc;
    pl->dmg.set(0, 0x1E);
    StaFlagOn(pG, STA_PL_EM_ACTION);
    switch (pl->r_no_2) {
    case 0: {
        setWepTrans(pl, 0);
        pl->Body->setKnife(true);
        gAxeLive = true;
        GameAddPoint(LVADD_CRITICALHIT);
        pl->m_Work0 = 0;
        pl->m_Work1 = 12;
        pl->m_Work2 = 0;
        void* mot = axeMotion(em->subArc);
        if (mot) {
            MotionSetCore(pl, &pl->Motion, mot, PL_ARC_PTR(em->subArc, kAxeSeq), 8, 1, 0);
            pl->r_no_2 = 1;
        } else {
            MotionSetCore(pl, &pl->Motion, PL_ARC_PTR(arc, 0x83), 0, 6, 1, 0);
            pl->m_Work0 = kWindUp;
            pl->r_no_2 = 10;
        }
        break;
    }
    case 1: {
        const int end = MotionMove(pl, 0);
        const u8 free = pl->Motion.Seq_old.Free;
        if (free & 0x20) {
            axeSwing(pl);
        }
        if (free & 1) {
            pl->m_Work0++;
        }
        // the third hit frame, or the first after fewer
        if (!pl->m_Work2 && pl->m_Work0 && (pl->m_Work0 >= (u32) kAxeHitAt || !(free & 1))) {
            pl->m_Work2 = 1;
            axeHit(pl);
        }
        if (end || pl->Motion.Seq_frame >= (f32) kAxeEnd) {
            axeFinish(pl);
        } else if (pl->Motion.Seq_frame >= (f32) kAxeCancel && (joyKamae() || (Key.on & 0x10F))) {
            axeFinish(pl);
        }
        break;
    }
    case 10:
        MotionMove(pl, 0);
        if (pl->m_Work0 && --pl->m_Work0) {
            break;
        }
        MotionSetCore(pl, &pl->Motion, PL_ARC_PTR(arc, 0x86), 0, 3, 1, 0);
        axeSwing(pl);
        pl->m_Work0 = kHitFrame;
        pl->m_Work2 = kCancel;
        pl->r_no_2 = 11;
        break;
    case 11:
        if (pl->m_Work0) {
            pl->m_Work0--;
            if (pl->m_Work0 == 0) {
                axeHit(pl);
            }
        }
        if (MotionMove(pl, 0)) {
            axeFinish(pl);
        } else if (pl->m_Work2) {
            pl->m_Work2--;
        } else if (joyKamae() || (Key.on & 0x10F)) {
            axeFinish(pl);
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
    if (!leon()) {
        return game;
    }
    const bool axe = (gBits.load() & 4) && pPL && pPL->Body;
    if (pick(1, axe ? kAxe : kGame) != kAxe) {
        return game;
    }
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        re4_add_frame_hook(axeWatch);
    }
    say(6, "Leon's axe strike on a kneeling enemy (the Ganados' axe attack, motion 0x80)");
    return (void*) plAxeStrike;
}

void re4_melee_set(int bits)
{
    if (gBits.exchange(bits) != bits) {
        re4_log("melee: neck break %s, the mods' melee motions %s, axe strike on kneeling enemies %s, random mix %s",
                bits & 1 ? "on" : "off", bits & 2 ? "off" : "on", bits & 4 ? "on" : "off", bits & 8 ? "off" : "on");
    }
}

void* re4_melee_motion(int kind, void* game)
{
    if (!leon()) {
        return game;
    }
    if (kind == 0 || kind == 1) {
        if (!gDrawn[kind]) {  // a move without a prompt of patch 0904's: drawn now
            pick(kind, kGame);
        }
        gDrawn[kind] = false;
        const int c = gPick[kind];
        if (c < 0 || c >= gMods[kind] || (gBits.load() & 2) || !gMod[kind][c]) {
            return game;
        }
        say(kind, kind ? "a mod's move on a kneeling enemy plays" : "a mod's move on a standing enemy plays");
        return gMod[kind][c];
    }
    if (kind >= 2 && kind <= 4) {
        void* own = re4_melee_hunk_entry(0x2B9 + (unsigned) (kind - 2));
        return own ? own : game;
    }
    return game;
}

int re4_melee_neck(int special)
{
    if (!leon()) {
        return 0;
    }
    bool neck = (gBits.load() & 1) && !special;
    if (neck && !hunkReady()) {
        say(5, "no neck break: HUNK's motions (em/em60.drs, entries 0x2B9..0x2BB) are not on the disc; Leon kicks");
        neck = false;
    }
    if (pick(0, neck ? kNeck : kGame) != kNeck) {
        return 0;
    }
    gDrawn[0] = false;
    say(4, "Leon breaks a stunned Ganado's neck (HUNK's motions from em/em60.drs)");
    return 1;
}

// The runtime test's (game_probe.cpp re4probe_melee): the next draws start over
void re4_melee_reset(void)
{
    gLast[0] = gLast[1] = -2;
    gDrawn[0] = gDrawn[1] = false;
}

}  // extern "C"
