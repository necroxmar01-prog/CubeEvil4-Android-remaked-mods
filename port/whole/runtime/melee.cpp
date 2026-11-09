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
// Only Leon (pl_type 0); the other characters keep their moves.
#include "types.h"
#include "global.h"
#include "main.h"
#include "dvd.h"
#include "re4_runtime.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void* re4_melee_hunk_entry(unsigned no);  // runtime/cheats.cpp

namespace {

std::atomic<int> gBits(0);  // the app's switches: bit 0 "Neck break", bit 1 the mods' motions off
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

}  // namespace

extern "C" {

void re4_melee_set(int bits)
{
    if (gBits.exchange(bits) != bits) {
        re4_log("melee: neck break %s, the mods' melee motions %s", bits & 1 ? "on" : "off", bits & 2 ? "off" : "on");
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
