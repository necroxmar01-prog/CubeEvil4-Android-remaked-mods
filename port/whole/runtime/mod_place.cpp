// A place for the mods' character archives (wish of DevJunior, 11.11.2026: skins for Ashley, Luis, Ada and the other
// characters; port/docs/npc-skins-plan.md, his approval of the extra 16 MB the same day).
//
// The game reads a character's archive (Ashley as the partner, Luis, the people of the events, every enemy:
// read.cpp readEmData through EmReadSearch) into a block of its heap as large as the file. A skin converted from the
// PC release is several MB: the room's heap has no such room. Patch 0914 asks re4_mod_em_place first: an archive a
// mod pack replaces (runtime/dvd.cpp, the mods/ folder) gets a block of this place instead, which patch 0914's
// InitModule gives back (re4_mod_em_release) when the game frees the module.
//
// The place: kSize bytes of address space above MEM1 (0x80000000..0x83000000, which the development half already
// fills), taken the first time a mod's archive asks, never given back: on LP64 from the game's window
// (game_window.cpp, below 4 GB), on ILP32 the first free 16 MB from 0x83000000 up (bit 31 set, as the game tells its
// own data by it). Without mods nothing is taken. Once it is, re4_game_top (the game's checks of "a pointer into
// game memory", 0x82FFFFFF on the GameCube; port/tools/whole_prepare.py rewrites them) reaches to its end, and the
// graphics processor and ARAM find it at the physical addresses kPhysical.. (gx_fifo.cpp, aram.cpp).
#include "re4_runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern "C" int re4_mod_file_bytes(const char* name);  // runtime/dvd.cpp: 0 when no mod replaces it

// The game's upper bound of its memory (src: the checks `<= 0x82FFFFFF`, VALID_PTR), the place's end once it is taken.
extern "C" __attribute__((visibility("default"))) unsigned int re4_game_top = 0x82FFFFFFu;

namespace {

const unsigned long kSize = RE4_MOD_PLACE_SIZE;
const int kBlocks = 8;

struct Block {
    unsigned long at;    // 0: free entry
    unsigned long size;
};

pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
unsigned long gBase;     // 0 until taken
bool gFailed;
Block gBlock[kBlocks];

bool take()
{
    if (gBase || gFailed) {
        return gBase != 0;
    }
#if __SIZEOF_POINTER__ == 4
    for (unsigned long hint = 0x83000000ul; hint + kSize <= 0xF0000000ul; hint += kSize) {
        void* p = mmap((void*) hint, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
        if (p == MAP_FAILED) {
            continue;
        }
        if (p == (void*) hint) {
            gBase = (unsigned long) p;
            break;
        }
        munmap(p, kSize);  // an old kernel that took the address as a hint
    }
#else
    gBase = (unsigned long) re4_window_map(kSize, PROT_READ | PROT_WRITE);
#endif
    if (!gBase || gBase < 0x83000000ul || gBase + kSize > 0x100000000ull) {
        if (gBase) {
            munmap((void*) gBase, kSize);
        }
        gBase = 0;
        gFailed = true;
        re4_log("mods: no %lu MB above MEM1 for the mods' character archives: they go to the game's heap", kSize >> 20);
        return false;
    }
    if (gBase + kSize - 1 > re4_game_top) {
        re4_game_top = (unsigned int) (gBase + kSize - 1);
    }
    re4_log("mods: %lu MB for the mods' character archives at 0x%08lx (game memory now up to 0x%08x)", kSize >> 20,
            gBase, re4_game_top);
    return true;
}

// The first free range of `size` bytes (32-byte aligned) between the blocks, 0 if none.
unsigned long firstFit(unsigned long size)
{
    unsigned long at = gBase;
    for (;;) {
        bool moved = false;
        for (const Block& b : gBlock) {
            if (b.at && at < b.at + b.size && b.at < at + size) {
                at = (b.at + b.size + 31) & ~31ul;
                moved = true;
            }
        }
        if (at + size > gBase + kSize) {
            return 0;
        }
        if (!moved) {
            return at;
        }
    }
}

}  // namespace

extern "C" {

// Patch 0914 (read.cpp readEmData, the game thread): where the archive `name` is read when a mod replaces it: a
// block of the place of at least the file and at least `need` (the room's own size for the block); 0 = as the game
// does (the disc's file, no place, or no room left: the heap).
void* re4_mod_em_place(const char* name, unsigned int need)
{
    const int bytes = name ? re4_mod_file_bytes(name) : 0;
    if (bytes <= 0) {
        return 0;
    }
    const unsigned long size = (((unsigned long) bytes > need ? (unsigned long) bytes : need) + 31) & ~31ul;
    pthread_mutex_lock(&gLock);
    void* result = 0;
    int slot = -1;
    for (int i = 0; i < kBlocks; i++) {
        if (!gBlock[i].at) {
            slot = i;
            break;
        }
    }
    if (slot >= 0 && take()) {
        const unsigned long at = firstFit(size);
        if (at) {
            gBlock[slot].at = at;
            gBlock[slot].size = size;
            result = (void*) at;
        }
    }
    pthread_mutex_unlock(&gLock);
    if (result) {
        re4_log("mods: %s (%d bytes, block of %lu) read to %p", name, bytes, size, result);
    } else if (gBase) {
        re4_log("mods: %s (%d bytes): no room left in the place for the mods' character archives, read to the heap",
                name, bytes);
    }
    return result;
}

// A block of `size` bytes of the place for the port's own use (runtime/dvd.cpp: the skins of the event bodies,
// patch 0915), 0 when none is left; given back with re4_mod_em_release.
void* re4_mod_place_alloc(unsigned long size, const char* what)
{
    size = (size + 31) & ~31ul;
    pthread_mutex_lock(&gLock);
    void* result = 0;
    for (Block& b : gBlock) {
        if (!b.at) {
            if (take()) {
                if (const unsigned long at = firstFit(size)) {
                    b.at = at;
                    b.size = size;
                    result = (void*) at;
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&gLock);
    if (result) {
        re4_log("mods: %s (%lu bytes) at %p", what, size, result);
    } else {
        re4_log("mods: %s (%lu bytes): no room left in the place for the mods' character archives", what, size);
    }
    return result;
}

// Patch 0914 (read.cpp InitModule): the game frees a module; its archive's block is free again. 1 = it was one.
int re4_mod_em_release(const void* p)
{
    if (!p) {
        return 0;
    }
    pthread_mutex_lock(&gLock);
    int found = 0;
    for (Block& b : gBlock) {
        if (b.at == (unsigned long) p) {
            madvise((void*) b.at, b.size, MADV_DONTNEED);  // its pages back to the system
            b.at = 0;
            b.size = 0;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&gLock);
    return found;
}

// 1 when `size` bytes from p lie in the place (taken).
int re4_mod_place_holds(const void* p, unsigned long size)
{
    const unsigned long a = (unsigned long) p;
    return gBase && a >= gBase && a - gBase <= kSize && size <= kSize - (a - gBase);
}

// The graphics processor's and ARAM's view (gx_fifo.cpp, aram.cpp): the place at the physical addresses
// RE4_MOD_PLACE_PHYSICAL.. (above MEM1 and the GX windows of port memory).
unsigned int re4_mod_place_physical(const void* p)
{
    return re4_mod_place_holds(p, 1) ? RE4_MOD_PLACE_PHYSICAL + (unsigned int) ((unsigned long) p - gBase) : 0;
}

void* re4_mod_place_at(unsigned int physical, unsigned int* available)
{
    if (!gBase || physical < RE4_MOD_PLACE_PHYSICAL || physical - RE4_MOD_PLACE_PHYSICAL >= kSize) {
        if (available) {
            *available = 0;
        }
        return 0;
    }
    const unsigned long offset = physical - RE4_MOD_PLACE_PHYSICAL;
    if (available) {
        *available = (unsigned int) (kSize - offset);
    }
    return (void*) (gBase + offset);
}

// For the runtime test: blocks from the place (taken for it), the graphics processor's addresses, freeing; a bit per
// failed check, 0 = all passed. Leaves the place empty.
int re4_mod_place_selftest(void)
{
    int bad = 0;
    pthread_mutex_lock(&gLock);
    const bool ok = take();
    pthread_mutex_unlock(&gLock);
    if (!ok) {
        return 1;
    }
    if (re4_game_top != gBase + kSize - 1 || (re4_game_top & 0x80000000u) == 0) bad |= 2;
    // three blocks by hand, as re4_mod_em_place gives them (no disc file in the test)
    pthread_mutex_lock(&gLock);
    const unsigned long a = firstFit(0x300000);
    gBlock[0] = {a, 0x300000};
    const unsigned long b = firstFit(0x500020);
    gBlock[1] = {b, 0x500020};
    const unsigned long c = firstFit(kSize);  // larger than what is left
    pthread_mutex_unlock(&gLock);
    if (a != gBase || b != gBase + 0x300000 || c != 0) bad |= 4;
    memset((void*) b, 0x5A, 0x500020);  // writable
    if (re4_mod_place_physical((void*) (b + 0x10)) != RE4_MOD_PLACE_PHYSICAL + 0x300010) bad |= 8;
    unsigned int left = 0;
    if (re4_mod_place_at(RE4_MOD_PLACE_PHYSICAL + 0x300010, &left) != (void*) (b + 0x10) || left != kSize - 0x300010)
        bad |= 0x10;
    if (re4_mod_place_at(RE4_MOD_PLACE_PHYSICAL + (unsigned int) kSize, &left) != 0) bad |= 0x20;
    if (!re4_mod_em_release((void*) a) || re4_mod_em_release((void*) a)) bad |= 0x40;
    pthread_mutex_lock(&gLock);
    const unsigned long again = firstFit(0x200000);  // the freed room first
    pthread_mutex_unlock(&gLock);
    if (again != gBase) bad |= 0x80;
    re4_mod_em_release((void*) b);
    if (re4_mod_em_place(0, 0) != 0) bad |= 0x100;
    return bad;
}

}  // extern "C"
