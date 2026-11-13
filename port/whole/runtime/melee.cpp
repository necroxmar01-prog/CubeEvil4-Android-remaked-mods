// Leon's melee moves (wish of DevJunior, 09.11.2026: "a system for different melee animations", the remake mods).
// Compiled with the game's flags into libre4_main (re4_gamehooks_obj); patch 0904 lets em10.cpp ask here, the app
// sends its switches with the second aim word (runtime/aim_options.cpp: bit 24 "Neck break", bit 25 the mods'
// motions off, bit 26 "Axe strike", bit 27 the random mix off).
//
//  - Custom motions: a mod pack's melee/melee.dat (port/tools/uhd_motion_convert.py, from the PC release's FCV
//    files) is a DAT of FCV entries, the even ones moves on a standing stunned Ganado (the kick, the enemy
//    archives' entry 0x29D), the odd ones on a kneeling one (Leon's archive, entry 0x25), up to kVariants of
//    each; an empty entry is none. It is
//    read once to RE4_MEM1_MELEE_ADDR (one larger than its RE4_MEM1_MELEE_MAX, up to kBigMax: to the mods' place, the
//    RE9 mod's kicks, port/tools/re9_finisher_pack.py --kicks) and converted as a read archive is (under the name mods/melee.drs: the
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
//  - Box strike (wish of DevJunior, 12.11.2026: "break the barrels with one hit as in the Remake, with a button like
//    the strike's, but not as long"): near an intact box, barrel or vase (game/embox.cpp, cEmBox) that Leon faces,
//    the action button (patch 0916, emBoxActEvtCk: the prompt the game shows there only in its tutorial rooms, now
//    as "Kick" in every room) makes him slash it with his knife (or a skin's axe): the low slash 0x86, its blade at
//    the box on frame kBoxHit, which breaks it as a shot does (emBoxSetBreak: debris, sound, the item); kBoxEnd
//    frames in all, an aim or fire key ends it after the hit. Switched with the third aim word's bit 12. A mod pack's
//    melee.dat entry 16 stands in for the slash (DevJunior, 12.11.2026: Jake's low kick without the frames where he
//    raises his hand; uhd_motion_convert.py --box), played from its entry 17's start to its end frame, the box
//    breaking on its hit frame. Reach (wish of DevJunior, 12.11.2026: "in the Remake, depending on how near they are,
//    a slash or a kick breaks 3 or 2 boxes at once"): on the hit frame up to kBoxMore other intact boxes, barrels or
//    vases break with the struck one, the nearest first: those whose side lies within kBoxSplash of Leon, in front
//    of him (kBoxCone) and from his feet up to kBoxHigh (a stack's upper crates).
//  - Hatchet combo (wish of DevJunior, 13.11.2026: "the hatchet attacks seen freely" in the RE9 mod; "held, the classic,
//    a click, the remake's"): a tap of the knife key (released within kTap frames) slashes at once with the mod's
//    three-slash combo, melee.dat entries 18..20 (patch 0922, re4_knife_tap_input / re4_knife_slash), the next slash
//    for the next tap; the key held is the knife's stance as ever, its slash the game's three-way one by the aim.
//    A parried blow (runtime/parry.cpp) is answered with one of the mod's parries (entries 21..23).
//  - Finishers (wish of DevJunior, 13.11.2026: the PC mod "RE9 Melee - Hatchet - Finishers for Leon"): a mod pack's
//    melee/finish.dat (runtime/finisher.cpp) adds the hatchet finisher on a kneeling Ganado and the guns' finishers
//    on a standing one to the moves drawn (with the mix off they go first); they play through the neck break's enemy
//    routine (patch 0921).
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
#include "embox.h"
#include "joy.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void* re4_melee_hunk_entry(unsigned no);  // runtime/cheats.cpp
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" int re4_finish_ready(int kind);                    // runtime/finisher.cpp
extern "C" int re4_finish_arm(int kind);
extern "C" void* re4_finish_kneel_routine(void* kick);
extern "C" void* re4_finish_motion(int kind);
extern "C" void re4_finish_clear(void);
extern "C" unsigned re4_pad_frame(void);                      // runtime/pad.cpp
extern cPlayer* pPL;

namespace {

// the app's switches: bit 0 "Neck break", bit 1 the mods' motions off, bit 2 "Axe strike", bit 3 the random mix off
std::atomic<int> gBits(0);
const int kVariants = 8;      // a mod's moves of each kind
char gLoaded[1600];           // the archive at RE4_MEM1_MELEE_ADDR ("" none; a path that failed counts as read)
void* gMod[2][kVariants];     // its motions: on a standing enemy, on a kneeling one
void* gBoxMot;                // its box strike's motion (entry 16; patch 0916), played from gBoxFrames[0] to [2]
u8 gBoxFrames[3];             // entry 17 ("BXF"): start, the frame that breaks the box, end
int gMods[2];                 // how many of each
int gSaid;                    // what the log told already (bits by kind)

// What the last prompt drew (re4_melee_neck / re4_melee_kneel), for the routine that plays it: -1 the game's own,
// 0..kVariants-1 a mod's, kNeck / kAxe; gDrawn the prompt's (cleared when taken)
enum { kGame = -1, kNeck = 100, kAxe = 101, kExec = 102, kFinish = 103 };
int gPick[2] = {kGame, kGame};
int gLast[2] = {-2, -2};
bool gDrawn[2];
u32 gSeed = 0x2545F491u;
const int kCombo = 3;                    // the knife's combo of a mod (entries 18..20): the slashes in turn
void* gCombo[kCombo];
int gCombos;
const int kParry = 3;                    // the answers to a parried blow (entries 21..23)
void* gParry[kParry];
int gParries;
int gLastParry = -1;
const int kTap = 8;                      // a press of the knife key at most this long is a tap: the combo's slash
const int kTapQueue = 20;                // frames a tap waits for the knife's stance (or the slash before it)
int gHeld;                               // frames the knife key has been down
int gTapQueued;                          // frames left for a tap's slash to start
bool gTapSlash;                          // the next slash is a tap's (the combo)
bool gParryNext;                         // the next slash answers a parried blow
int gComboStep = -1;                     // the last slash of the combo played, and when it started (pad frame)
unsigned gComboAt;
unsigned gComboLen;                      // its frames
const unsigned kComboGap = 10;           // a slash within this many frames after the last one's end goes on with it
const unsigned long kBigMax = 0x100000;  // a melee archive in the mods' place at most (16 moves of 64 KB)
u8* gBig;                                // that place, once taken (given back with no pack's archive)
unsigned long gBigSize;

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
    gBoxMot = nullptr;
    gCombos = 0;
    gComboStep = -1;
    gParries = 0;
    gSaid &= ~3;
    if (!*path) {
        if (gBig) {
            re4_mod_em_release(gBig);
            gBig = 0;
            gBigSize = 0;
        }
        return;
    }
    FILE* f = fopen(path, "rb");
    long fileSize = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    // a larger archive (the RE9 mod's kicks: 0.4 MB, port/tools/re9_finisher_pack.py --kicks) goes to the place for the
    // mods' character archives, at most kBigMax bytes; the fixed place stays for the small ones
    u8* base = (u8*) (unsigned long) RE4_MEM1_MELEE_ADDR;
    size_t room = RE4_MEM1_MELEE_MAX;
    if (fileSize > (long) RE4_MEM1_MELEE_MAX && fileSize <= (long) kBigMax) {
        if (!gBig || gBigSize < (unsigned long) fileSize) {
            if (gBig) {
                re4_mod_em_release(gBig);
            }
            gBig = (u8*) re4_mod_place_alloc((unsigned long) fileSize, "the mods' melee motions (melee/melee.dat)");
            gBigSize = gBig ? (unsigned long) fileSize : 0;
        }
        if (gBig) {
            base = gBig;
            room = (size_t) gBigSize;
        }
    }
    const size_t size = f ? fread(base, 1, room, f) : 0;
    const bool whole = f && fgetc(f) == EOF;
    if (f) {
        fclose(f);
    }
    const u32 count = size >= 0x20 ? be32At(base) : 0;
    // the moves, the box strike's motion and its frames, the hatchet's combo (entries 18..20, patch 0922), the parries
    enum { kMoves = 2 * kVariants, kMax = kMoves + 2 + kCombo + kParry };
    u32 at[kMax], len[kMax];
    bool ok = whole && count >= 1 && count <= kMax && 0x10 + count * 8 <= size;
    for (u32 i = 0; ok && i < count; i++) {
        at[i] = be32At(base + 0x10 + i * 4);
        const u32 end = i + 1 < count ? be32At(base + 0x10 + (i + 1) * 4) : (u32) size;
        ok = at[i] >= 0x10 + count * 8 && at[i] <= end && end <= size && !(at[i] & 3);
        len[i] = ok ? end - at[i] : 0;
        // a motion: u16 last frame, u8 joints, the table and at least one offset
        if (ok && len[i] && i != kMoves + 1 && (len[i] < 0x10 || base[at[i] + 2] == 0)) {
            ok = false;
        }
    }
    if (!ok) {
        re4_log("melee: %s is no melee archive of at most %u bytes and %d entries (%u read, %u entries): the game's "
                "moves",
                path, (unsigned) kBigMax, (int) kMax, (unsigned) size, (unsigned) count);
        return;
    }
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = (u32) size;
    h.dest = (u32) (unsigned long) base;
    re4_formats_part_loaded("mods/melee.drs", 0, 0, &h, (unsigned) (unsigned long) base);
    if (count >= kMoves + 2 && len[kMoves] && len[kMoves + 1] >= 3) {  // the box strike's motion and frames
        const u8* fr = base + at[kMoves + 1];
        const unsigned last = get16(base + at[kMoves]);  // converted: host order
        if (fr[0] < fr[1] && fr[1] < fr[2] && fr[2] <= last) {
            gBoxMot = base + at[kMoves];
            memcpy(gBoxFrames, fr, 3);
            re4_log("melee: %s entry %d: the box strike's motion, frames %u..%u, the box breaks on %u", path,
                    (int) kMoves, (unsigned) fr[0], (unsigned) fr[2], (unsigned) fr[1]);
        } else {
            re4_log("melee: %s: the box strike's frames %u/%u/%u do not fit its %u frames: the knife's slash", path,
                    (unsigned) fr[0], (unsigned) fr[1], (unsigned) fr[2], last);
        }
    }
    for (u32 i = kMoves + 2; i < count; i++) {  // the hatchet's combo and the parries: Leon's knife (patch 0922)
        if (!len[i]) {
            continue;
        }
        if (i < kMoves + 2 + kCombo) {
            gCombo[gCombos++] = base + at[i];
            re4_log("melee: %s entry %u: the knife's slash %d of the combo, %u frames", path, (unsigned) i, gCombos,
                    (unsigned) get16(base + at[i]));
        } else {
            gParry[gParries++] = base + at[i];
            re4_log("melee: %s entry %u: an answer to a parried blow, %u frames", path, (unsigned) i,
                    (unsigned) get16(base + at[i]));
        }
    }
    for (u32 i = 0; i < count && i < kMoves; i++) {
        if (len[i]) {
            const int k = (int) (i & 1);
            gMod[k][gMods[k]++] = base + at[i];
            re4_log("melee: %s entry %u: a move on a %s enemy, %u bytes (%u frames, %u joints)", path, (unsigned) i,
                    k ? "kneeling" : "standing", (unsigned) len[i], (unsigned) get16(base + at[i]),
                    (unsigned) base[at[i] + 2]);
        }
    }
    re4_log("melee: %s read to 0x%08x: %d moves on a standing enemy, %d on a kneeling one", path,
            (unsigned) (unsigned long) base, gMods[0], gMods[1]);
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

// The move of kind k (0 on a standing enemy, 1 on a kneeling one) for this prompt; `extra` / `extra2` the neck break,
// the axe strike or the axe execution when switched on and possible, `finish` a finisher (runtime/finisher.cpp)
int pick(int k, int extra, int extra2 = kGame, int finish = kGame)
{
    const int bits = gBits.load();
    if (!(bits & 2)) {
        load();
    }
    const int mods = (bits & 2) ? 0 : gMods[k];
    int cand[kVariants + 4];
    int n = 0;
    if (bits & 8) {  // the mix off: the app's choice
        cand[n++] = finish != kGame ? finish : extra != kGame ? extra : extra2 != kGame ? extra2 : mods ? 0 : kGame;
    } else {
        cand[n++] = kGame;
        for (int i = 0; i < mods; i++) {
            cand[n++] = i;
        }
        if (extra != kGame) {
            cand[n++] = extra;
        }
        if (extra2 != kGame) {
            cand[n++] = extra2;
        }
        if (finish != kGame) {
            cand[n++] = finish;
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
                c == kGame ? "the game's" : c == kNeck ? "the neck break" : c == kAxe ? "the axe strike"
                : c == kExec ? "the axe execution" : c == kFinish ? "a finisher" : "the mod's move",
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
const int kAxeHitLead = 3;       // and at most this many before its first hit mark (Free 1)
const int kAxeLead = 3;          // frames of the Ganado's wind-up Leon keeps before the swing (its swing mark, Free 0x20:
                                 // the sequence's first; DevJunior, 12.11.2026: "it pauses at the start of the axe
                                 // animation": the Ganados hold the axe up a while, a warning to the player; 8 / 7
                                 // until the third device test: "Leon still points up, stuck with the axe": the blend
                                 // of 4 frames reaches the raised axe and the swing follows at once)
const int kAxeFollow = 6;        // frames after the hit Leon keeps: the blade goes on down to its lowest, then he returns
                                 // to his stance (DevJunior, 13.11.2026: "after the cut it sways": the Ganado's motion
                                 // swings the axe back up and rocks before kAxeEnd; a downward cut ends low)
const f32 kRootMax = 350.0f;     // the root's step forward at most (Leon's own knee kick: 325)
const int kWindUp = 9;           // the knife's fallback: frames of the wind-up (the up stance, blended in over 6)
const int kHitFrame = 8;         // frame of its chop (0x86) that hits: the blade at its lowest
const int kCancel = 18;          // frames of its chop before an aim or fire key may end it
bool gAxeLive = false;           // the knife is out for the axe strike (the gun hidden)

// whether the axe strike's swing (step 1) is over at `frame`: the motion's end, kAxeEnd, or kAxeFollow frames after
// the hit (`hitAt`, the frame it landed on; negative before it)
bool axeDone(f32 frame, int hitAt, bool end)
{
    return end || frame >= (f32) kAxeEnd || (hitAt >= 0 && frame >= (f32) (hitAt + kAxeFollow));
}

// A key value of a motion curve: they stay big-endian in memory (formats_model.cpp; motion.cpp reads them byte-wise,
// FccF32 of patch 0018), counts and frame numbers are host order
f32 keyF32(const u8* p)
{
    const u32 u = be32At(p);
    f32 v;
    memcpy(&v, &u, 4);
    return v;
}

void putKeyF32(u8* p, f32 v)
{
    u32 u;
    memcpy(&u, &v, 4);
    p[0] = (u8) (u >> 24);
    p[1] = (u8) (u >> 16);
    p[2] = (u8) (u >> 8);
    p[3] = (u8) u;
}

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
                const f32 v = keyF32(p + 2 + cnt * 2 + c * stride);
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
                const f32 v = keyF32(axes[a] + 2 + counts[a] * 2 + c * 12);
                most = fabsf(v) > most ? fabsf(v) : most;
            }
        }
        const f32 scale = most > kRootMax ? kRootMax / most : 1.0f;
        for (int a = 0; a < 3 && scale < 1.0f; a += 2) {
            for (unsigned c = 0; c < counts[a]; c++) {
                for (int w = 0; w < 3; w++) {
                    u8* at = axes[a] + 2 + counts[a] * 2 + c * 12 + w * 4;
                    putKeyF32(at, keyF32(at) * scale);
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

bool gExecSwing_reset();

void axeFinish(cPlayer* pl)
{
    gExecSwing_reset();
    axeEnd(pl);
    EndPlDamage();
    pl->dmg.set(0, 0xF);
}

// Steps in r_no_2: 0 start, 1 the Ganado's swing (m_Work0 the hit frames seen, m_Work2 the hit's frame + 1), 10 / 11 the
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
            // from kAxeLead frames before the swing: the Ganado's wait with the axe up left out
            // (12.11.2026, second device test: "it still pauses with the axe up": the swing mark alone did not cut it;
            // the first hit mark, Free 1, is surely there, the hit counts on it)
            const SEQUENCE_DATA* seq = pl->Motion.pSeq_top;
            int swing = -1, hit = -1;
            for (int f = 0; seq && f < (int) pl->Motion.Seq_frame_num && f < kAxeCancel; f++) {
                if (swing < 0 && (seq[f].Free & 0x20)) {
                    swing = f;
                }
                if (hit < 0 && (seq[f].Free & 1)) {
                    hit = f;
                }
            }
            int start = swing > kAxeLead ? swing - kAxeLead : 0;
            if (hit > kAxeHitLead && hit - kAxeHitLead > start) {
                start = hit - kAxeHitLead;
            }
            if (start > 0) {
                MotionSetCore(pl, &pl->Motion, mot, PL_ARC_PTR(em->subArc, kAxeSeq), 4, 1, start);
            }
            re4_log("melee: the axe strike from frame %d (swing mark %d, first hit mark %d, %u frames)", start, swing,
                    hit, (unsigned) pl->Motion.Seq_frame_num);
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
            pl->m_Work2 = (u32) pl->Motion.Seq_frame + 1;
            axeHit(pl);
            re4_log("melee: the axe strike hit on frame %d, ends %d frames later", (int) pl->m_Work2 - 1, kAxeFollow);
        }
        if (axeDone(pl->Motion.Seq_frame, (int) pl->m_Work2 - 1, end != 0)) {
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

// The axe execution of a standing stunned Ganado (patch 0904, em10KickAction: the Ganado drops to its knees as when
// shot in the leg): Leon swings the axe strike (plAxeStrike) at once while it drops. Until 12.11.2026 he drew the knife
// in its up stance first and waited for the knee motion's kneel mark (Free 2, at most 24, then 8 frames): DevJunior saw
// that wait, the knife up, as a stuck pose in three device tests; the Ganado did not kneel in time anyway, and the
// strike's own wind-up (kAxeLead) covers its drop
bool gExecSwing = false;

bool gExecSwing_reset()
{
    const bool was = gExecSwing;
    gExecSwing = false;
    return was;
}

void plAxeExec(cPlayer* pl)
{
    if (!gExecSwing) {
        gExecSwing = true;
        pl->r_no_2 = 0;
    }
    plAxeStrike(pl);
}

// ---- The box strike (see the head) ----

const int kBoxMot = 0x86;        // the knife's low slash (Leon's archive; the root sinks to 746: down to a box)
const int kBoxHit = 7;           // its frame that breaks the box (the blade low, a frame before the chop's)
const int kBoxEnd = 16;          // frames in all (the slash's recovery cut short: "not as long")
const f32 kBoxReach = 700.0f;    // Leon this near the box's side (its size) for the prompt: the knife reaches
std::atomic<int> gBoxOn(-1);     // the app's switch; -1 none sent yet (the PC: RE4_BOX_STRIKE, else on)
const f32 kBoxSplash = 900.0f;   // other boxes whose side lies this near Leon break with the struck one
const f32 kBoxCone = 1.05f;      // within 60 degrees of where he faces (he turned to the struck box)
const f32 kBoxLow = 500.0f;      // from this below his feet (a slope) ...
const f32 kBoxHigh = 1600.0f;    // ... to this above them (the upper crates of a stack)
const int kBoxMore = 2;          // at most this many besides the struck one (3 in all)
const int kBoxId = 0x43;         // cEmBox (SetBox: EmMgr.create(0x43))

bool boxOn()
{
    int on = gBoxOn.load();
    if (on < 0) {
        const char* env = getenv("RE4_BOX_STRIKE");
        on = env ? atoi(env) != 0 : 1;
    }
    return on != 0;
}

// How far `other` lies beyond the strike's reach from Leon (its side's distance; negative: out of reach)
f32 boxReach(cEmBox* target, cEm* other)
{
    if (!pPL || !other || other == target || other->id != kBoxId || other->hp <= 0 || other->type == 4) {
        return -1.0f;  // type 4: a bird's nest in a tree
    }
    const f32 dy = other->pos.y - pPL->pos.y;
    if (dy < -kBoxLow || dy > kBoxHigh) {
        return -1.0f;
    }
    const FREE_EMBOX* w = EMBOX_WK(other);
    const f32 dx = other->pos.x - pPL->pos.x;
    const f32 dz = other->pos.z - pPL->pos.z;
    const f32 d = sqrtf(dx * dx + dz * dz);
    const f32 side = w->size.x > w->size.z ? w->size.x : w->size.z;
    if (d - side > kBoxSplash) {
        return -1.0f;
    }
    if (d > side && fabsf(GetXZAngleLocal(&pPL->pos, &other->pos, pPL->ang.y)) > kBoxCone) {
        return -1.0f;
    }
    return d > side ? d - side : 0.0f;
}

// The boxes the strike reaches besides the struck one, the nearest first: broken as it is
int breakNear(cEmBox* target)
{
    cEm* pick[kBoxMore] = {};
    f32 dist[kBoxMore] = {};
    int n = 0;
    for (cEm* e = EmMgr.getActiveWork(); e; e = EmMgr.getNext(e)) {
        const f32 d = boxReach(target, e);
        if (d < 0.0f) {
            continue;
        }
        int k = n < kBoxMore ? n++ : kBoxMore;
        while (k > 0 && dist[k - 1] > d) {
            if (k < kBoxMore) {
                pick[k] = pick[k - 1], dist[k] = dist[k - 1];
            }
            k--;
        }
        if (k < kBoxMore) {
            pick[k] = e, dist[k] = d;
        }
    }
    for (int i = 0; i < n; i++) {
        emBoxSetBreak((cEmBox*) pick[i], 0);
    }
    return n;
}

void plBoxStrike(cPlayer* pl)
{
    cEmBox* box = (cEmBox*) pl->pEmCatch;
    switch (pl->r_no_2) {
    case 0:
        setWepTrans(pl, 0);
        gAxeLive = true;
        load();
        if (gBoxMot && !(gBits.load() & 2)) {  // a mod's kick (wish of DevJunior, 12.11.2026: Jake's low kick)
            MotionSetCore(pl, &pl->Motion, gBoxMot, 0, 4, 1, gBoxFrames[0]);
            pl->m_Work0 = gBoxFrames[1] - gBoxFrames[0];
            pl->m_Work2 = gBoxFrames[2] - gBoxFrames[0];
            say(11, "the box strike plays the mod's motion");
        } else {
            pl->Body->setKnife(true);
            MotionSetCore(pl, &pl->Motion, PL_ARC_PTR(pG->pPlayer, kBoxMot), 0, 3, 1, 0);
            axeSwing(pl);
            pl->m_Work0 = kBoxHit;
            pl->m_Work2 = kBoxEnd;
        }
        pl->m_Work1 = 6;
        pl->r_no_2 = 1;
        break;
    case 1:
        MotionMove(pl, 0);
        if (pl->m_Work0 && --pl->m_Work0 == 0 && box && box->hp > 0) {
            emBoxSetBreak(box, 0);
            say(10, "Leon's box strike broke a box");
            if (const int more = breakNear(box)) {
                re4_log("melee: the box strike broke %d more box%s within its reach", more, more > 1 ? "es" : "");
            }
        }
        if (!pl->m_Work2 || --pl->m_Work2 == 0 || (!pl->m_Work0 && (joyKamae() || (Key.on & 0x10F)))) {
            axeEnd(pl);
            EndPlDamage();
            return;
        }
        break;
    }
    if (pl->m_Work1 > 0 && box) {
        pl->m_Work1--;
        pl->ang.y += Muku(&pl->pos, &box->pos, pl->ang.y, 0.19634955f);
        pl->ang.y = LIMIT_ANGLE(pl->ang.y);
    }
}

void boxStrikeAction(cEmBox* box)
{
    SetPlDamage(box, plBoxStrike);
}

// A grab or a hit that cut the axe strike short: the knife away and the gun back
void axeWatch(unsigned)
{
    if (gAxeLive && pPL && (pPL->r_no_0 != 4 || (Pl_func_tbl[4] != plAxeStrike && Pl_func_tbl[4] != plAxeExec &&
                                                 Pl_func_tbl[4] != plBoxStrike))) {
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
    re4_finish_clear();
    const int c = pick(1, axe ? kAxe : kGame, kGame, re4_finish_ready(1) ? kFinish : kGame);
    if (c == kFinish && re4_finish_arm(1)) {
        gDrawn[1] = false;
        return re4_finish_kneel_routine(game);
    }
    if (c != kAxe) {
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

// Patch 0916 (embox.cpp emBoxActEvtCk, the box Leon faces and nothing between them): 1 when the action button
// strikes it (the switch on, Leon, the knife's reach), and the prompt's routine
int re4_box_strike(void* em)
{
    cEmBox* box = (cEmBox*) em;
    if (!box || !boxOn() || !leon() || !pPL || !pPL->Body || box->hp <= 0) {
        return 0;
    }
    const FREE_EMBOX* w = EMBOX_WK(box);
    const f32 dx = pPL->pos.x - box->pos.x;
    const f32 dz = pPL->pos.z - box->pos.z;
    const f32 r = (w->size.x > w->size.z ? w->size.x : w->size.z) + kBoxReach;
    return dx * dx + dz * dz <= r * r;
}

void* re4_box_strike_func(void)
{
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        re4_add_frame_hook(axeWatch);
    }
    return (void*) boxStrikeAction;
}

// For the tests: whether the axe strike's swing is over at `frame` with the hit on `hit_at` (< 0: not yet)
int re4_axe_done(float frame, int hit_at, int end)
{
    return axeDone(frame, hit_at, end != 0) ? 1 : 0;
}

// For the tests: whether the box strike on `target` breaks `other` too (its distance beyond the reach, < 0 not)
float re4_box_reach(void* target, void* other)
{
    return boxReach((cEmBox*) target, (cEm*) other);
}

// For the tests: the active mods' box strike frames (start << 16 | hit << 8 | end), -1 none (the knife's slash)
int re4_box_strike_motion(void)
{
    load();
    return gBoxMot ? gBoxFrames[0] << 16 | gBoxFrames[1] << 8 | gBoxFrames[2] : -1;
}

void re4_box_strike_set(int on)
{
    on = on ? 1 : 0;
    if (gBoxOn.exchange(on) != on) {
        re4_log("melee: box strike %s (the action button breaks a box, barrel or vase Leon faces)", on ? "on" : "off");
    }
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
        if (void* fin = re4_finish_motion(kind)) {  // a finisher armed (patch 0921)
            return fin;
        }
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
    const bool exec = (gBits.load() & 4) && !special && pPL && pPL->Body;
    re4_finish_clear();
    const int c = pick(0, neck ? kNeck : kGame, exec ? kExec : kGame, !special && re4_finish_ready(0) ? kFinish : kGame);
    if (c == kFinish && re4_finish_arm(0)) {
        gDrawn[0] = false;
        say(12, "a finisher on a standing stunned Ganado (runtime/finisher.cpp)");
        return 1;
    }
    if (c == kExec) {
        gDrawn[0] = false;
        say(7, "Leon's axe execution: a standing stunned Ganado drops to its knees and takes the axe strike");
        return 2;
    }
    if (c != kNeck) {
        return 0;
    }
    gDrawn[0] = false;
    say(4, "Leon breaks a stunned Ganado's neck (HUNK's motions from em/em60.drs)");
    return 1;
}

// Patch 0922 (pl_knife.cpp knife_r3_fire00): 1 when Leon's knife slash plays the mod's hatchet combo (melee.dat entries
// 18..20, port/tools/re9_finisher_pack.py --kicks): the next slash of it when he slashes again within kComboGap frames
// after the last one, else the first; its hits stay the knife's (knife_r3_fire10: frames 6..10, the blade's fastest
// frame put on 8). The aim's three-way blend (high, middle, low) gives way to it, as in the 2023 remake. Root motion
// on: the combo steps forward.
int re4_knife_slash(void* p)
{
    cPlayer* pl = (cPlayer*) p;
    const bool tap = gTapSlash, parry = gParryNext;
    gTapSlash = gParryNext = false;
    if (!leon() || !pl || (gBits.load() & 2) || !(tap || parry)) {
        return 0;
    }
    load();
    void* mot = 0;
    if (parry && gParries) {
        int k = 0;
        if (gParries > 1) {
            gSeed = gSeed * 1664525u + 1013904223u;
            k = (int) ((gSeed >> 16) % (u32) gParries);
            if (k == gLastParry) {
                k = (k + 1) % gParries;
            }
        }
        gLastParry = k;
        mot = gParry[k];
        say(14, "a parried blow answered with the mod's parry");
    } else if (tap && gCombos) {
        const unsigned now = re4_pad_frame();
        int step = 0;
        if (gComboStep >= 0 && now - gComboAt <= gComboLen + kComboGap) {
            step = (gComboStep + 1) % gCombos;
        }
        gComboStep = step;
        gComboAt = now;
        mot = gCombo[step];
    }
    if (!mot) {
        return 0;
    }
    MotionSetCore(pl, &pl->Motion, mot, 0, 3, 1, 0);
    gComboLen = pl->Motion.Seq_frame_num;
    MotionMove(pl, 0);
    pl->Waist->set(pl->m_Fwork0, 0.4f);
    EstSet(pl, -1, 0, 0, EFF_CORE, 0x2B, 0, ESP_CORE_KIND_PL_WEP, 0, 0);
    pl->Body->waistMove();
    pl->partsWorldCalc();
    if (!(gSaid & (1 << 13))) {
        gSaid |= 1 << 13;
        re4_log("melee: the knife's slash plays the mod's hatchet combo (%d slashes)", gCombos);
    }
    return 1;
}

// For the tests: the combo's slashes the active archive holds (and its parries, << 8)
int re4_knife_combo_count(void)
{
    load();
    return gCombos | gParries << 8;
}

// runtime/parry.cpp: the slash it starts now answers a parried blow
void re4_knife_parry_mark(void)
{
    gParryNext = true;
    gTapQueued = 0;
}

// Before the routines read the keys (runtime/parry.cpp re4_parry_input, patch 0900): a tap of the knife key (down at
// most kTap frames) slashes with the combo: the knife's stance (routine 0xB, drawn by the press) goes into its slash
// step as on the fire key; one tapped during a slash waits for its end (kTapQueue frames at most).
void re4_knife_tap_input(void* p)
{
    cPlayer* pl = (cPlayer*) p;
    if (!pl || !leon() || (gBits.load() & 2)) {
        gHeld = gTapQueued = 0;
        return;
    }
    load();
    if (!gCombos) {
        gHeld = gTapQueued = 0;
        return;
    }
    const bool down = (Key.on & 0x800) || (Joy[0].on & 0x40);
    if (down) {
        gHeld++;
    } else {
        if (gHeld > 0 && gHeld <= kTap) {
            gTapQueued = kTapQueue;
        }
        gHeld = 0;
    }
    if (gTapQueued <= 0) {
        return;
    }
    gTapQueued--;
    if (pl->r_no_0 != 0) {
        gTapQueued = 0;  // hurt or in an event
        return;
    }
    // only from the stance itself (step 1): its first step draws the knife and puts the gun away (device video
    // 13.11.2026: a tap's slash came with the pistol still in the hand)
    if (pl->r_no_1 == 0xB && pl->r_no_2 == 1) {
        pl->r_no_2 = 2;  // the slash, as pl_knife's stance step sets it on the fire key
        pl->r_no_3 = 0;
        gTapSlash = true;
        gTapQueued = 0;
    } else if (pl->r_no_1 != 0xB || pl->r_no_2 == 0) {
        Key.on |= 0x800;  // the knife key held until his stance is up
        Joy[0].on |= 0x40;
    }
}

void* re4_melee_execute(void)
{
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        re4_add_frame_hook(axeWatch);
    }
    gExecSwing = false;
    return (void*) plAxeExec;
}

// The runtime test's (game_probe.cpp re4probe_melee): the next draws start over
void re4_melee_reset(void)
{
    gLast[0] = gLast[1] = -2;
    gDrawn[0] = gDrawn[1] = false;
}

}  // extern "C"
