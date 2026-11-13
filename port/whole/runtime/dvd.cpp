// Disc access (Dolphin DVD API) over a folder of extracted disc files.
//
// Layout of the data folder (what Dolphin's "Extract Entire Disc" writes, or
// port/tools/whole_disc_folder.py), one folder per disc:
//
//   <data>/disc0/sys/boot.bin   disc header (DVDDiskID in its first 32 bytes)
//   <data>/disc0/sys/fst.bin    file system table, big-endian as on the disc
//   <data>/disc0/files/...      the files
//   <data>/disc1/...            second disc
//
// A folder with sys/ and files/ directly inside is accepted as disc 0 alone.
// The import folder of the Android app counts from 1 (<data>/disc1 is the
// first disc, <data>/disc2 the second); without a disc0 folder that layout is
// used.
//
// As on the GameCube, the FST of the inserted disc is copied to the top of MEM1
// (DVDGetFSTLocation) and the disc id to 0x80000000. Entry numbers are FST
// indices. Reads complete immediately: the callback runs before
// DVDReadAsyncPrio returns, which the game's queue (dvd.cpp) handles like a
// very fast drive. A read started inside an interrupt handler (the sound
// driver's stream refill in the audio frame) completes when the handler has
// returned (os_thread.cpp re4_os_defer): the stream code marks the read busy
// only after starting it. At the start the queue does not wait a frame
// between its steps (patch 0085, re4_dvd_boot_fast below).
#include "re4_runtime.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>

#include <atomic>
#include <utility>
#include <vector>

namespace {

typedef unsigned u32;
typedef int s32;

struct DVDDiskID {
    char gameName[4];
    char company[2];
    unsigned char diskNumber;
    unsigned char gameVersion;
    unsigned char streaming;
    unsigned char streamingBufSize;
    unsigned char padding[22];
};

struct DVDCommandBlock;
typedef void (*DVDCBCallback)(s32 result, DVDCommandBlock* block);
struct DVDCommandBlock {
    DVDCommandBlock* next;
    DVDCommandBlock* prev;
    u32 command;
    s32 state;
    u32 offset;
    u32 length;
    void* addr;
    u32 currTransferSize;
    u32 transferredSize;
    DVDDiskID* id;
    DVDCBCallback callback;
    void* userData;
};

struct DVDFileInfo;
typedef void (*DVDCallback)(s32 result, DVDFileInfo* fileInfo);
struct DVDFileInfo {
    DVDCommandBlock cb;
    u32 startAddr;
    u32 length;
    DVDCallback callback;
};

struct DVDDir {
    u32 entryNum;
    u32 location;
    u32 next;
};

struct DVDDirEntry {
    u32 entryNum;
    int isDir;
    char* name;
};

static_assert(sizeof(DVDDiskID) == 0x20, "DVDDiskID layout");
// the SDK's form (include/dolphin/dvd.h), which the game allocates: the
// GameCube's on ILP32, pointers of 8 bytes on LP64 (milestone 12)
static_assert(sizeof(DVDCommandBlock) == (sizeof(void*) == 4 ? 0x30 : 0x48), "DVDCommandBlock layout");
static_assert(sizeof(DVDFileInfo) == (sizeof(void*) == 4 ? 0x3C : 0x58), "DVDFileInfo layout");

const int kStateEnd = 0;
const int kStateBusy = 1;
const int kStateCanceled = 10;
const int kResultCanceled = -3;
const int kCommandRead = 1;
const int kCommandChangeDisk = 3;

char gDataDir[1024] = "data";

struct Disc {
    bool loaded;
    char root[1100];          // folder holding sys/ and files/
    unsigned char id[0x20];   // first 32 bytes of boot.bin
    int release;              // RE4_DISC_DEBUG ... (releaseOf)
    bool korean;              // RE4_DISC_RETAIL_JP: the Korean translation's main.dol (koreanOf)
    unsigned char* fst;       // big-endian FST, host copy
    u32 fstSize;
    u32 count;                // FST entries
    u32* parent;              // per entry: parent directory index
    char** mod;               // per entry: the replacing file of a mod (mods/), else 0
    u32 mods;                 // files replaced
};

void applyMods(Disc& d, int no);
void freeDisc(Disc& d);
int gModsGeneration;  // counts applyMods: the skins of the cutscenes look again (patch 0915)

Disc gDiscs[2];
int gCurrent = -1;

// The first disc file the game could not read, for the app's message (re4_core_disc_problem):
// "<disc>\t<path on the disc>\t<why>"; written once on the game thread, read by any thread.
char gProblem[768];
std::atomic<int> gProblemSet(0);

u32 be32(const unsigned char* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

bool isDir(const Disc& d, u32 i)
{
    return d.fst[i * 12] != 0;
}

const char* entryName(const Disc& d, u32 i)
{
    return (const char*) d.fst + d.count * 12 + (be32(d.fst + i * 12) & 0xFFFFFF);
}

u32 word(const Disc& d, u32 i, int k)
{
    return be32(d.fst + i * 12 + k * 4);
}

unsigned char* readWhole(const char* path, u32* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*) malloc(n > 0 ? n : 1);
    if (n < 0 || fread(buf, 1, n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *size = (u32) n;
    return buf;
}

// opendir rather than stat: the i686 test sysroot lacks the kernel headers
// <sys/stat.h> pulls in.
bool isDirectory(const char* path)
{
    DIR* dir = opendir(path);
    if (dir) {
        closedir(dir);
    }
    return dir != 0;
}

// Validates the FST (bounds of names, directory ranges) and records parents.
bool parseFst(Disc& d)
{
    if (d.fstSize < 12 || d.fst[0] != 1) {
        return false;
    }
    d.count = word(d, 0, 2);
    if (d.count == 0 || d.count > d.fstSize / 12) {
        return false;
    }
    u32 namesSize = d.fstSize - d.count * 12;
    d.parent = (u32*) calloc(d.count, sizeof(u32));
    // Directory ranges nest: walk with an explicit stack of open directories.
    u32 stack[64];
    int depth = 0;
    stack[0] = 0;
    for (u32 i = 1; i < d.count; i++) {
        while (depth > 0 && i >= word(d, stack[depth], 2)) {
            depth--;
        }
        if ((be32(d.fst + i * 12) & 0xFFFFFF) >= namesSize) {
            return false;
        }
        d.parent[i] = stack[depth];
        if (isDir(d, i)) {
            u32 end = word(d, i, 2);
            if (end <= i || end > d.count || depth == 63) {
                return false;
            }
            stack[++depth] = i;
        }
    }
    return true;
}

// Which release a disc header belongs to (the rule of port/tools/disc_release.py): G4BE08 with
// the file system table at 0x28FE00 is the debug build of 2004-11-25 the port's code is (its
// main.dol is 2554432 bytes), G4BP08 the European release (port/docs/retail-eu-plan.md), G4BE08
// with the table elsewhere (0x231A00) the US release (port/docs/retail-us-plan.md), G4BJ08 the Japanese
// release and the Korean translation built on it (port/docs/retail-jp-plan.md). A header without the table's place
// (only the port's tests write one) counts as the debug build, as the port assumed until then.
int releaseOf(const unsigned char* boot, u32 size)
{
    if (size >= 6 && memcmp(boot, "G4BP08", 6) == 0) {
        return RE4_DISC_RETAIL_EU;
    }
    if (size >= 6 && memcmp(boot, "G4BJ08", 6) == 0) {
        return RE4_DISC_RETAIL_JP;
    }
    if (size < 6 || memcmp(boot, "G4BE08", 6) != 0) {
        return RE4_DISC_UNKNOWN;
    }
    if (size < 0x428) {
        return RE4_DISC_DEBUG;
    }
    return be32(boot + 0x424) == 0x28FE00 ? RE4_DISC_DEBUG : RE4_DISC_RETAIL_US;
}

const char* releaseName(int release)
{
    static const char* const names[] = {"debug build G4BE08", "European release G4BP08",
                                        "US release G4BE08", "unknown game",
                                        "Japanese release G4BJ08"};
    return names[release >= 0 && release <= RE4_DISC_RETAIL_JP ? release : RE4_DISC_UNKNOWN];
}

// The Korean fan translation of the Japanese discs: the same header, its main.dol reads no country in
// emWepPlHeadLost and obj16PlHeadLost (the Japanese 0x80017b7c / 0x8002ccd4, file offsets 0x14b7c and
// 0x29cd4: "li r30, 1" / "li r31, 1" for "lbz rX, 9(r9)", pSys->eff_country). Read from sys/main.dol.
bool koreanOf(const char* root)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/sys/main.dol", root);
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    unsigned char w[2][4] = {};
    bool ok = fseek(f, 0x14b7c, SEEK_SET) == 0 && fread(w[0], 1, 4, f) == 4 && fseek(f, 0x29cd4, SEEK_SET) == 0 &&
              fread(w[1], 1, 4, f) == 4;
    fclose(f);
    return ok && be32(w[0]) == 0x3BC00001u && be32(w[1]) == 0x3BE00001u;
}

bool loadDisc(int no)
{
    Disc& d = gDiscs[no];
    if (d.loaded) {
        return true;
    }
    char path[1200];
    snprintf(d.root, sizeof(d.root), "%s/disc0", gDataDir);
    int first = isDirectory(d.root) ? 0 : 1;  // the app's folders: disc1, disc2
    snprintf(d.root, sizeof(d.root), "%s/disc%d", gDataDir, no + first);
    if (!isDirectory(d.root)) {
        if (no != 0) {
            return false;
        }
        snprintf(d.root, sizeof(d.root), "%s", gDataDir);
    }
    snprintf(path, sizeof(path), "%s/sys/fst.bin", d.root);
    d.fst = readWhole(path, &d.fstSize);
    if (!d.fst) {
        return false;
    }
    if (!parseFst(d)) {
        re4_log("DVD: %s is not a valid file system table", path);
        free(d.fst);
        free(d.parent);
        d.fst = 0;
        d.parent = 0;
        return false;
    }
    u32 bootSize = 0;
    snprintf(path, sizeof(path), "%s/sys/boot.bin", d.root);
    unsigned char* boot = readWhole(path, &bootSize);
    memset(d.id, 0, sizeof(d.id));
    if (boot && bootSize >= sizeof(d.id)) {
        memcpy(d.id, boot, sizeof(d.id));
        d.release = releaseOf(boot, bootSize);
    } else {
        // No header: assume the debug build's id (G4BE08), disc `no`.
        memcpy(d.id, "G4BE08", 6);
        d.id[6] = (unsigned char) no;
        d.release = RE4_DISC_DEBUG;
    }
    free(boot);
    d.korean = d.release == RE4_DISC_RETAIL_JP && koreanOf(d.root);
    d.loaded = true;
    re4_log("DVD: disc %d: %s, %u FST entries, %s%s", no, d.root, d.count, releaseName(d.release),
            d.korean ? " (Korean translation)" : "");
    applyMods(d, no);
    const Disc& other = gDiscs[no ^ 1];
    if (other.loaded && other.release != d.release) {
        re4_log("DVD: the discs are of different releases (%s, %s): import both discs of one release",
                releaseName(gDiscs[0].release), releaseName(gDiscs[1].release));
    }
    return true;
}

// Makes disc `no` the inserted one: disc id at 0x80000000, FST at the top of MEM1.
bool insertDisc(int no)
{
    if (!loadDisc(no)) {
        return false;
    }
    Disc& d = gDiscs[no];
    gCurrent = no;
    if (!re4_mem1_init()) {
        return true;
    }
    memcpy((void*) (unsigned long) RE4_MEM1_BASE, d.id, sizeof(d.id));
    if (d.release == RE4_DISC_RETAIL_EU || d.release == RE4_DISC_RETAIL_JP) {
        // The European discs carry G4BP08 (the Japanese G4BJ08). The game's code is the debug build's, which writes its
        // saves itself: the memory card ties each file to the game id of the disc (CARD __CARDAccess),
        // so with the European id the saves of the debug discs (and the .gci of the app's menu,
        // runtime/gci.cpp) would be another game's. cDvd::DiscChange asks for "G4BE" as well. The
        // game sees the debug build's id; disc number and version stay the disc's.
        memcpy((void*) (unsigned long) RE4_MEM1_BASE, "G4BE08", 6);
        static bool told;
        if (!told) {
            told = true;
            re4_log("DVD: %s discs: the game sees the debug build's id G4BE08 (memory card, disc change)",
                    d.release == RE4_DISC_RETAIL_JP ? "Japanese" : "European");
        }
    }
    // the top of retail memory (ILP32), on LP64 above the higher heap end
    u32 top = RE4_MEM1_FST_ADDR + RE4_MEM1_FST_MAX;
    u32 fstAddr = (top - d.fstSize) & ~31u;
    if (d.fstSize > RE4_MEM1_FST_MAX) {
        re4_log("DVD: FST of disc %d is %u bytes, overlaps the game heap", no, d.fstSize);
    }
    memcpy((void*) (unsigned long) fstAddr, d.fst, d.fstSize);
    *(volatile u32*) (unsigned long) (RE4_MEM1_BASE + 0x38) = fstAddr;
    *(volatile u32*) (unsigned long) (RE4_MEM1_BASE + 0x3C) = d.fstSize;
    return true;
}

Disc* current()
{
    if (gCurrent < 0) {
        insertDisc(0);
    }
    return gCurrent >= 0 ? &gDiscs[gCurrent] : 0;
}

// Full path of entry i below files/.
void entryPath(const Disc& d, u32 i, char* out, size_t outSize)
{
    u32 chain[64];
    int n = 0;
    for (u32 e = i; e != 0 && n < 64; e = d.parent[e]) {
        chain[n++] = e;
    }
    size_t len = (size_t) snprintf(out, outSize, "%s/files", d.root);
    while (n > 0 && len < outSize) {
        len += (size_t) snprintf(out + len, outSize - len, "/%s", entryName(d, chain[--n]));
    }
}

// The file that holds entry i on this device: a mod's replacement (applyMods), else the disc's own.
void filePath(const Disc& d, u32 i, char* out, size_t outSize)
{
    if (d.mod && d.mod[i]) {
        snprintf(out, outSize, "%s", d.mod[i]);
        return;
    }
    entryPath(d, i, out, outSize);
}

// The first unreadable file: the app tells the player which file and what to do (reports of
// 25./26.10.2026: st2/r222.das read as zeros, the picture stood without a word).
void noteProblem(const Disc& d, const char* devicePath, const char* why)
{
    if (gProblemSet.load(std::memory_order_acquire)) {
        return;
    }
    char files[1200];
    snprintf(files, sizeof(files), "%s/files/", d.root);
    const char* onDisc = devicePath;
    if (strncmp(devicePath, files, strlen(files)) == 0) {
        onDisc = devicePath + strlen(files);
    }
    snprintf(gProblem, sizeof(gProblem), "%d\t%s\t%s", d.id[6] + 1, onDisc, why);
    gProblemSet.store(1, std::memory_order_release);
    re4_log("DVD: disc %d, %s: %s (said to the app)", d.id[6] + 1, onDisc, why);
}

bool sameName(const char* a, size_t alen, const char* b)
{
    size_t i = 0;
    for (; i < alen && b[i]; i++) {
        if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) {
            return false;
        }
    }
    return i == alen && b[i] == 0;
}

// DVDConvertPathToEntrynum: '/'-separated, case-insensitive, from the root
// (the game never changes the current directory); "." and ".." allowed.
s32 lookup(const Disc& d, const char* path)
{
    u32 dir = 0;
    const char* p = path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char* end = p;
        while (*end && *end != '/') {
            end++;
        }
        size_t len = (size_t) (end - p);
        if (len == 1 && p[0] == '.') {
            p = end;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            dir = d.parent[dir];
            p = end;
            continue;
        }
        u32 found = 0;
        for (u32 i = dir + 1; i < word(d, dir, 2);) {
            if (sameName(p, len, entryName(d, i))) {
                found = i;
                break;
            }
            i = isDir(d, i) ? word(d, i, 2) : i + 1;
        }
        if (!found) {
            return -1;
        }
        if (*end && !isDir(d, found)) {
            return -1;
        }
        dir = found;
        p = end;
    }
    return (s32) dir;
}

// Mods (wish of the project owner, 08.11.2026: character skins): <data>/mods/<pack>/ mirrors the
// disc's files/ folder (or holds it as <pack>/files/); every file there replaces the disc's file
// of that path (case-insensitive, like the game's own paths) on both discs. Packs apply in name
// order, a later pack wins; a pack holding the file .off is switched off (the app's menu). The replacement may differ in size: its size goes into the host FST
// (and with it the copy in MEM1, DVDFastOpen and the reads), so the game reads the whole file.
// Files without a counterpart on the disc are named in the log and left out; read when the disc
// is first loaded (a pack added later counts from the next start).
void putBe32(unsigned char* p, u32 v)
{
    p[0] = (unsigned char) (v >> 24);
    p[1] = (unsigned char) (v >> 16);
    p[2] = (unsigned char) (v >> 8);
    p[3] = (unsigned char) v;
}

int byName(const void* a, const void* b)
{
    return strcmp(*(const char* const*) a, *(const char* const*) b);
}

// The names in a folder, sorted (hidden ones left out); free each and the list.
char** listDir(const char* path, int* count)
{
    *count = 0;
    DIR* dir = opendir(path);
    if (!dir) {
        return 0;
    }
    int cap = 16;
    char** names = (char**) malloc(cap * sizeof(char*));
    while (struct dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (*count == cap) {
            cap *= 2;
            names = (char**) realloc(names, cap * sizeof(char*));
        }
        names[(*count)++] = strdup(e->d_name);
    }
    closedir(dir);
    qsort(names, *count, sizeof(char*), byName);
    return names;
}

// A mod's melee motions (mods/<pack>/melee/melee.dat, port/tools/uhd_motion_convert.py; patch 0904): no file
// of the disc, read by runtime/melee.cpp. The last pack's in the folder's order.
char gMeleePath[1600];
// A mod's finishers (mods/<pack>/melee/finish.dat, port/tools/re9_finisher_pack.py; patch 0921): read by
// runtime/finisher.cpp. The last pack's in the folder's order.
char gFinishPath[1600];

void modFolder(Disc& d, const char* pack, const char* full, const char* rel, int depth, int no)
{
    int count = 0;
    char** names = listDir(full, &count);
    for (int k = 0; k < count; k++) {
        char path[1600], name[600];
        snprintf(path, sizeof(path), "%s/%s", full, names[k]);
        snprintf(name, sizeof(name), "%s%s%s", rel, *rel ? "/" : "", names[k]);
        if (isDirectory(path)) {
            if (depth < 16) {
                modFolder(d, pack, path, name, depth + 1, no);
            }
            free(names[k]);
            continue;
        }
        if (strcmp(name, "melee/melee.dat") == 0) {
            if (strcmp(gMeleePath, path) != 0) {
                snprintf(gMeleePath, sizeof(gMeleePath), "%s", path);
                re4_log("DVD: mod %s: %s holds its melee motions", pack, name);
            }
            free(names[k]);
            continue;
        }
        if (strcmp(name, "melee/finish.dat") == 0) {
            if (strcmp(gFinishPath, path) != 0) {
                snprintf(gFinishPath, sizeof(gFinishPath), "%s", path);
                re4_log("DVD: mod %s: %s holds its finishers", pack, name);
            }
            free(names[k]);
            continue;
        }
        if (strcmp(name, "sound/quickslot.wav") == 0) {
            // the D-pad quick slots' sound (runtime/mod_sound.cpp), no file of the disc either
            re4_mod_sound_path_set(path);
            re4_log("DVD: mod %s: %s is the sound of the D-pad quick slots", pack, name);
            free(names[k]);
            continue;
        }
        s32 entry = lookup(d, name);
        errno = 0;
        FILE* f = fopen(path, "rb");
        const int openErr = f ? 0 : errno;
        long size = -1;
        if (f && fseek(f, 0, SEEK_END) == 0) {
            size = ftell(f);
        }
        if (f) {
            fclose(f);
        }
        if (entry <= 0 || isDir(d, (u32) entry)) {
            re4_log("DVD: mod %s: %s is not a file of disc %d, left out", pack, name, no + 1);
        } else if (size < 0 || size > 0x7FFFFFFFL) {
            // a file another app put into the app's folder may not be the app's to read (08.11.2026: a file
            // manager's copy, EACCES): the menu's "Add mod folder" copies it as the app's own
            re4_log("DVD: mod %s: %s cannot be read (%s), left out: add the mod through the app's menu (Mods)", pack,
                    name, openErr ? strerror(openErr) : "no size");
        } else {
            u32 was = word(d, (u32) entry, 2);
            if (!d.mod[entry]) {
                d.mods++;
            }
            free(d.mod[entry]);
            d.mod[entry] = strdup(path);
            putBe32(d.fst + entry * 12 + 8, (u32) size);
            re4_log("DVD: mod %s: %s replaces the disc's file (%u bytes, on the disc %u)", pack, name, (unsigned) size,
                    (unsigned) was);
        }
        free(names[k]);
    }
    free(names);
}

void applyMods(Disc& d, int no)
{
    gModsGeneration++;
    re4_mod_sound_path_set("");  // a pack switched off takes its sound along
    char mods[1200];
    snprintf(mods, sizeof(mods), "%s/mods", gDataDir);
    int count = 0;
    char** packs = listDir(mods, &count);
    for (int k = 0; k < count; k++) {
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", mods, packs[k]);
        char off[1500];
        snprintf(off, sizeof(off), "%s/.off", full);
        if (isDirectory(full) && access(off, F_OK) == 0) {
            // switched off in the app's menu (ModSwitch.java, wish of DevJunior, 09.11.2026: several skins kept)
            re4_log("DVD: mod %s is switched off", packs[k]);
        } else if (isDirectory(full)) {
            char files[1500];
            snprintf(files, sizeof(files), "%s/files", full);
            if (!d.mod) {
                d.mod = (char**) calloc(d.count, sizeof(char*));
            }
            modFolder(d, packs[k], isDirectory(files) ? files : full, "", 0, no);
        }
        free(packs[k]);
    }
    free(packs);
    if (d.mods) {
        re4_log("DVD: disc %d: %u files from mods", no + 1, (unsigned) d.mods);
    }
}

void freeDisc(Disc& d)
{
    if (d.mod) {
        for (u32 i = 0; i < d.count; i++) {
            free(d.mod[i]);
        }
    }
    free(d.mod);
    free(d.fst);
    free(d.parent);
    memset(&d, 0, sizeof(d));
}

s32 entryForStart(const Disc& d, u32 startAddr, u32 length)
{
    for (u32 i = 1; i < d.count; i++) {
        if (!isDir(d, i) && word(d, i, 1) == startAddr && word(d, i, 2) == length) {
            return (s32) i;
        }
    }
    return -1;
}

// Reads `length` bytes at `offset` of the file; beyond its end the drive
// would return the next sectors of the disc, the port returns zeros.
s32 readFile(DVDFileInfo* fi, void* dst, s32 length, s32 offset)
{
    Disc* d = current();
    if (!d || length < 0 || offset < 0) {
        return -1;
    }
    s32 entry = entryForStart(*d, fi->startAddr, fi->length);
    if (entry < 0) {
        // A file opened before the disc swap (a stream still fading out while a save from the other disc
        // loads: the title's music, a tester's report 01.11.2026) keeps reading its own file on the disc it
        // was opened on. The drive would read the new disc's sectors there; refused, the stream waited for
        // its data for ever and held its slot, and later room streams in that slot stayed silent.
        Disc& other = gDiscs[gCurrent ^ 1];
        if (other.loaded && (entry = entryForStart(other, fi->startAddr, fi->length)) >= 0) {
            static int toldOther;
            if (toldOther++ < 4) {
                re4_log("DVD: read at 0x%x of a file opened on disc %d (before the disc change)",
                        (unsigned) fi->startAddr, (gCurrent ^ 1) + 1);
            }
            d = &other;
        }
    }
    if (entry < 0) {
        static int toldEntry;
        if (toldEntry++ < 8) {
            re4_log("DVD: read of 0x%x bytes at 0x%x refused: no file starts at 0x%x with %u bytes",
                    (unsigned) length, (unsigned) offset, (unsigned) fi->startAddr, (unsigned) fi->length);
        }
        return -1;
    }
    char path[2048];
    filePath(*d, (u32) entry, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) {
        re4_log("DVD: cannot open %s", path);
        noteProblem(*d, path, "missing on this device (it cannot be opened)");
        return -1;
    }
    size_t got = 0;
    if (fseek(f, offset, SEEK_SET) == 0) {
        got = fread(dst, 1, (size_t) length, f);
    }
    long fileSize = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        fileSize = ftell(f);
    }
    fclose(f);
    if (length == 0x400) {
        // a part table that came back as zeros (patch 0113 reads it again): say where it came from
        static int toldZero;
        bool zeros = got > 0;
        for (size_t i = 0; i < got && zeros; i++) {
            zeros = ((const unsigned char*) dst)[i] == 0;
        }
        if (zeros && toldZero++ < 8) {
            re4_log("DVD: %s: 0x400 bytes at 0x%x read as zeros (got %u; the file is %ld bytes on the device, "
                    "%u on the disc)", path, (unsigned) offset, (unsigned) got, fileSize, (unsigned) fi->length);
        }
    }
    if (got < (size_t) length) {
        memset((char*) dst + got, 0, (size_t) length - got);
        // inside the file's length on the disc, the file on the device is shorter: an import
        // that did not finish, or a file changed since (Xiaomi report 23.10.2026: r325 read a part
        // table of zeros and the game read parts of size 0 for ever); a few lines at most
        static int told;
        if ((u32) offset + got < fi->length && told < 8) {
            told++;
            re4_log("DVD: %s: only %u of %u bytes at 0x%x on the device (the file has %u on the disc): "
                    "import the disc again",
                    path, (unsigned) got, (unsigned) length, (unsigned) offset, (unsigned) fi->length);
        }
        if ((u32) offset + got < fi->length) {
            char why[160];
            snprintf(why, sizeof(why), "shorter on this device than on the disc (%ld of %u bytes)", fileSize,
                     (unsigned) fi->length);
            noteProblem(*d, path, why);
        }
    }
    return length;
}

}  // namespace

extern "C" {

// A part table (0x400 bytes, host byte order, 32 entries of 8 words) must end in TRANS_EOF and may
// only hold known part types; entry 0 is the file's own record (patch 0113, cDvdQueue::readInit).
// 0 = good, 1 = read it again, 2 = give up. The counter belongs to the one table in flight: the
// game's header_buff is shared by all queues.
int re4_dvd_header_check(const void* table, const char* name, unsigned offset, unsigned length, int depth)
{
    static int retries;
    static unsigned retryOffset;  // the table the retries count for (a read cancelled meanwhile starts over)
    static char retryName[64];
    const u32* w = (const u32*) table;
    bool eof = false, known = true;
    for (int i = 1; i < 32 && !eof; i++) {
        u32 type = w[i * 8];
        if (type == 0xFFFFFFFFu) {
            eof = true;
        } else if (type > 4 && type != 0xFFFFFFFEu) {
            known = false;
        }
    }
    if (eof && known) {
        retries = 0;
        return 0;
    }
    if (retries && (retryOffset != offset || strncmp(retryName, name ? name : "", sizeof(retryName) - 1) != 0)) {
        retries = 0;
    }
    retryOffset = offset;
    snprintf(retryName, sizeof(retryName), "%s", name ? name : "");
    bool zeros = true;
    for (int i = 0; i < 256 && zeros; i++) {
        zeros = w[i] == 0;
    }
    char head[3 * 32 + 1];
    for (int i = 0; i < 32; i++) {
        snprintf(head + i * 3, 4, "%02x ", ((const unsigned char*) table)[i]);
    }
    re4_log("DVD: %s (depth %d, at 0x%x, file length %u): the part table is %s (%s); first bytes %s(try %d)",
            name ? name : "?", depth, (unsigned) offset, (unsigned) length,
            zeros ? "all zeros" : "no good", eof ? "unknown part type" : "no end marker", head, retries + 1);
    if (++retries < 3) {
        return 1;
    }
    retries = 0;
    re4_log("DVD: %s: giving up; import the disc again (the file on the device is damaged or unreadable)",
            name ? name : "?");
    Disc* d = current();
    s32 entry = d && name ? lookup(*d, name) : -1;
    if (entry > 0) {
        char path[2048];
        filePath(*d, (u32) entry, path, sizeof(path));
        noteProblem(*d, path, zeros ? "its start reads as zeros on this device (the file is damaged)"
                                    : "its start is damaged on this device (no valid part table)");
    }
    return 2;
}

void re4_set_data_dir(const char* dir)
{
    snprintf(gDataDir, sizeof(gDataDir), "%s", dir);
    for (Disc& d : gDiscs) {
        freeDisc(d);
    }
    gCurrent = -1;
}

const char* re4_data_dir(void)
{
    return gDataDir;
}

const char* re4_core_disc_problem(void)
{
    return gProblemSet.load(std::memory_order_acquire) ? gProblem : "";
}

int re4_dvd_inserted_disc(void)
{
    return current() ? gCurrent : -1;
}

int re4_disc_release(void)
{
    Disc* d = current();
    return d ? d->release : RE4_DISC_UNKNOWN;
}

int re4_disc_retail_eu(void)
{
    return re4_disc_release() == RE4_DISC_RETAIL_EU;
}

int re4_disc_retail_us(void)
{
    return re4_disc_release() == RE4_DISC_RETAIL_US;
}

int re4_disc_retail_jp(void)
{
    return re4_disc_release() == RE4_DISC_RETAIL_JP;
}

int re4_disc_retail_kr(void)
{
    Disc* d = current();
    return d && d->release == RE4_DISC_RETAIL_JP && d->korean;
}

int re4_disc_retail(void)
{
    int release = re4_disc_release();
    return release == RE4_DISC_RETAIL_EU || release == RE4_DISC_RETAIL_US || release == RE4_DISC_RETAIL_JP;
}

// The size of a file's body (part 0 of its part table, or the unpacked size of a yz2 body): what a
// read of the file writes to memory. 0: none to tell (no such file, no part table with the body first).
static u32 bodySize(const char* name)
{
    Disc* d = current();
    s32 entry = d ? lookup(*d, name) : -1;
    if (entry < 0 || isDir(*d, (u32) entry)) {
        return 0;
    }
    char path[1400];
    filePath(*d, (u32) entry, path, sizeof(path));
    unsigned char head[0x440];
    FILE* f = fopen(path, "rb");
    size_t got = f ? fread(head, 1, sizeof(head), f) : 0;
    if (f) {
        fclose(f);
    }
    if (got < 0x440 || be32(head + 0x20) != 0) {
        return 0;  // no part table with the body first: the game's read reports it
    }
    u32 size = be32(head + 0x24);
    unsigned long packed = 0, unpacked = 0;
    char text[0x40];
    memcpy(text, head + 0x400, sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    if (isxdigit((unsigned char) text[0]) && strchr(text, '\t') && strchr(text, '\n') &&
        sscanf(text, "%lx\t%lx", &packed, &unpacked) == 2 && unpacked > 0) {
        size = (u32) unpacked;  // a yz2 body: its header names the unpacked size
    }
    return size;
}

// The place of the core archive (patch 0117, read.cpp CoreDataRead): the debug build reads
// etc/core.das to the fixed 0x80578000 with room for CORE_DATA_MAX (0x234000) bytes, up to the
// option data at 0x807AC000. The European release's core is 0x23AC40 bytes: there it overran
// the option data ("CORE_DATA IS TOO LARGE", 26.10.2026). A larger core goes to a range of the
// development half of MEM1 nothing else uses (RE4_MEM1_CORE_ALT_ADDR); 0 keeps the fixed place,
// so the debug build's core stays where it was. The size is the body's (part 0 of the part
// table, unpacked size of a yz2 body), what the game's read writes to memory.
void* re4_core_data_place(const char* name, unsigned fixedMax)
{
    u32 size = bodySize(name);
    if (size <= fixedMax) {
        return 0;
    }
    if (size > RE4_MEM1_CORE_ALT_MAX) {
        re4_log("DVD: %s: core of %u bytes, larger than the %u bytes of either place", name, size,
                RE4_MEM1_CORE_ALT_MAX);
        return 0;
    }
    re4_log("DVD: %s: core of %u bytes (fixed place %u, %s), read to 0x%08x", name, size, fixedMax,
            releaseName(current()->release), RE4_MEM1_CORE_ALT_ADDR);
    return (void*) (unsigned long) RE4_MEM1_CORE_ALT_ADDR;
}

// The place of a player archive (patch 0901, read.cpp ReadPlayerData; mods, wish of DevJunior,
// 08.11.2026): the game reads the player's file to the fixed 0x807EC000 with room for 0x118000 bytes
// (Leon) up to the weapon data. A skin converted from the PC release (port/tools/uhd_skin_convert.py)
// is several times larger: it goes to the free end of the development half of MEM1
// (RE4_MEM1_PLAYER_ALT_ADDR); 0 keeps the fixed place, as for every file of the discs.
// The player's file came from a mod (re4_player_data_place, before each read of it): patch 0901 lets
// cutscenes draw Leon's body with his archive's textures then.
static int gPlayerModded;

// The active mods' melee archive (see gMeleePath), "" without one.
const char* re4_mod_melee_path(void)
{
    return gMeleePath;
}

// The runtime test's (game_probe.cpp re4probe_melee): the archive as a mod pack would name it
void re4_mod_melee_path_set(const char* path)
{
    snprintf(gMeleePath, sizeof(gMeleePath), "%s", path);
}

// The active mods' finishers (see gFinishPath), "" without them.
const char* re4_mod_finish_path(void)
{
    return gFinishPath;
}

// The runtime test's: the finishers' archive as a mod pack would name it
void re4_mod_finish_path_set(const char* path)
{
    snprintf(gFinishPath, sizeof(gFinishPath), "%s", path);
}

// Patch 0914 (runtime/mod_place.cpp): the bytes a read of `name` writes (its size, or its body's when larger) when a
// mod replaces it, 0 for the disc's own file.
int re4_mod_file_bytes(const char* name)
{
    Disc* d = current();
    s32 entry = d ? lookup(*d, name) : -1;
    if (entry <= 0 || isDir(*d, (u32) entry) || !d->mod || !d->mod[entry]) {
        return 0;
    }
    u32 size = word(*d, (u32) entry, 2);
    const u32 body = bodySize(name);
    return (int) (body > size ? body : size);
}

int re4_player_modded(void)
{
    return gPlayerModded;
}

// Patch 0901, ExePacket_SetParts: a part an event attaches to Leon. A skin (the PC mods replace pl08, Leon
// without the jacket) takes its own pl000e for the jacket parts of costume 0 (ev000e / ev001e), as costumes
// 1 and 2 do on the disc; 1 = take it. Each part is logged while the player's file is a mod.
int re4_mod_event_part(const char* name, const char* bin, const char* tpl, const char* oya, int costume)
{
    if (!gPlayerModded) {
        return 0;
    }
    const int jacket = costume == 0 && (strcmp(name, "ev000e") == 0 || strcmp(name, "ev001e") == 0);
    re4_log("Event: part %s (%s, %s) on %s%s", name, bin, tpl, oya, jacket ? ": the skin's pl000e" : "");
    return jacket;
}

// Patch 0906 (event.cpp ExePacket_SetPartsSub): the player archive's part to draw instead of an event's Leon head
// part while the player's file is a mod (DevJunior, 10.11.2026: the cutscenes showed the disc's face with every skin):
// ev0001 (the event's head with its face shapes) -> 6 (the skin's head), ev0002 (the event's eyes) -> 8 (the skin's
// face); 0 keeps the event's. Only on Leon's event body (pl0000).
int re4_mod_event_head(const char* name, const char* oya)
{
    if (!gPlayerModded || name == 0 || oya == 0 || strcmp(oya, "pl0000") != 0) {
        return 0;
    }
    const int own = strcmp(name, "ev0001") == 0 ? 6 : strcmp(name, "ev0002") == 0 ? 8 : 0;
    if (own != 0) {
        re4_log("Event: part %s on %s: drawn as the skin's %s (archive part %d)", name, oya,
                own == 6 ? "head" : "face", own);
    }
    return own;
}

// Patch 0910 (Sscrn/ss_model.cpp tel00ModelInit): 1 = the codec's Leon takes the skin's head and face (archive 6 and
// 8) in place of the codec's own head (DevJunior, 11.11.2026: the radio calls showed the disc's face), while the
// player's file is a mod; logged.
int re4_mod_codec_head(void)
{
    if (gPlayerModded) {
        re4_log("Codec: Leon's head and face are the skin's (archive parts 6, 8)");
    }
    return gPlayerModded;
}

// Patch 0907 (shape.cpp ShapeSet): an event's Leon head (ev0001) and the skin's head patch 0906 draws in its place,
// when the skin's head carries the event head's face shapes (port/tools/skin_shapes.py): the skin's head then plays
// each face animation the event sets on its head. A pair counts while both model infos still hold the models they
// held when paired (the game reuses model infos for other parts later). Few heads at a time: the oldest pair goes.
namespace {
struct ShapePair {
    void* event;
    void* eventModel;
    void* skin;
    void* skinModel;
};
ShapePair gShapePairs[4];
unsigned gShapePairNext;
} // namespace

void re4_mod_shape_pair(void* event_info, void* event_model, void* skin_info, void* skin_model)
{
    if (event_info == 0 || skin_info == 0 || event_info == skin_info) {
        return;
    }
    ShapePair* slot = &gShapePairs[gShapePairNext++ % 4];
    for (ShapePair& p : gShapePairs) {
        if (p.event == event_info || p.skin == event_info || p.event == skin_info || p.skin == skin_info) {
            slot = &p; // one pair per model info
            break;
        }
    }
    *slot = ShapePair{event_info, event_model, skin_info, skin_model};
    re4_log("Mods: the skin's head plays the face animation of the head it stands in for");
}

// The skin's head paired with an event head that still holds its model, and the model the skin's head held when
// paired (*skin_model; the game checks it on its model info); 0 none.
void* re4_mod_shape_twin(void* info, void* model, void** skin_model)
{
    if (info == 0) {
        return 0;
    }
    for (ShapePair& p : gShapePairs) {
        if (p.event == info) {
            if (p.eventModel != model) {
                p = ShapePair{}; // a model info used for something else since
                return 0;
            }
            if (skin_model != 0) {
                *skin_model = p.skinModel;
            }
            return p.skin;
        }
    }
    return 0;
}

void* re4_player_data_place(const char* name, unsigned fixedMax)
{
    {
        Disc* d = current();
        s32 entry = d ? lookup(*d, name) : -1;
        const int modded = entry > 0 && d->mod && d->mod[entry] ? 1 : 0;
        if (modded != gPlayerModded) {
            re4_log("DVD: %s: the player's file is %s", name, modded ? "a mod's" : "the disc's");
        }
        gPlayerModded = modded;
    }
    u32 size = bodySize(name);
    if (size <= fixedMax) {
        return 0;
    }
    if (size > RE4_MEM1_PLAYER_ALT_MAX) {
        // larger than the place in MEM1: behind the mods' character archives (runtime/mod_place.cpp, 12.11.2026)
        if (void* p = re4_mod_player_place(size)) {
            re4_log("DVD: %s: player data of %u bytes (fixed place %u), read to %p (the mods' place)", name, size,
                    fixedMax, p);
            return p;
        }
        re4_log("DVD: %s: player data of %u bytes, larger than the %u bytes of every place: the fixed one, "
                "it runs over the weapon data", name, size, RE4_MOD_PLAYER_MAX);
        return 0;
    }
    re4_log("DVD: %s: player data of %u bytes (fixed place %u), read to 0x%08x", name, size, fixedMax,
            RE4_MEM1_PLAYER_ALT_ADDR);
    return (void*) (unsigned long) RE4_MEM1_PLAYER_ALT_ADDR;
}

// The place of the option menu's data (patch 0121, read.cpp OptionDataRead): the debug build reads
// SS/<lang>/option.dat to the fixed 0x807AC000 with room for OPTION_DATA_MAX (0x40000) bytes, up to
// the player data at 0x807EC000. On the European discs only the English one fits (0x3FBA0 bytes);
// German, French, Spanish and Italian are 0x44000 to 0x4C2C0 (the European OptionDataRead reads to
// 0x80784000 and has no limit). A larger one goes behind the core's second place
// (RE4_MEM1_OPTION_ALT_ADDR); 0 keeps the fixed place. The file is read whole.
void* re4_option_data_place(const char* name, unsigned fixedMax)
{
    Disc* d = current();
    s32 entry = d ? lookup(*d, name) : -1;
    if (entry < 0 || isDir(*d, (u32) entry)) {
        return 0;
    }
    u32 size = word(*d, (u32) entry, 2);
    if (size <= fixedMax) {
        return 0;
    }
    if (size > RE4_MEM1_OPTION_ALT_MAX) {
        re4_log("DVD: %s: option data of %u bytes, larger than the %u bytes of either place", name, size,
                RE4_MEM1_OPTION_ALT_MAX);
        return 0;
    }
    re4_log("DVD: %s: option data of %u bytes (fixed place %u, %s), read to 0x%08x", name, size, fixedMax,
            releaseName(d->release), RE4_MEM1_OPTION_ALT_ADDR);
    return (void*) (unsigned long) RE4_MEM1_OPTION_ALT_ADDR;
}

// The start without the frame pace (patch 0085, cDvd::readProcMain; wish of
// the project owner, 19.10.2026): until the first SS/<lang>/title.dat is in,
// the game's read queue takes its next step at once instead of the next
// frame, with one frame's wait after every 64 steps (the other tasks, and a
// read that keeps failing). RE4_DVD_BOOT_FAST=0: the game's pace throughout.
static int gBootFast = -1;  // -1: not asked yet, 1: the start, 0: done or off
static unsigned gBootSteps, gBootWaits;

int re4_dvd_boot_fast(void)
{
    if (gBootFast < 0) {
        const char* v = getenv("RE4_DVD_BOOT_FAST");
        gBootFast = v && *v == '0' ? 0 : 1;
        re4_log(gBootFast ? "DVD: the start's reads without the frame pace (until title.dat)"
                          : "DVD: RE4_DVD_BOOT_FAST=0, the game's read pace throughout");
    }
    if (!gBootFast) {
        return 0;
    }
    if (++gBootSteps % 64 == 0) {
        gBootWaits++;
        return 0;
    }
    return 1;
}

// The queue finished a read (patch 0085): the first title.dat ends the start.
void re4_dvd_read_done(const char* name)
{
    static const char kTitle[] = "/title.dat";
    if (gBootFast != 1 || !name) {
        return;
    }
    size_t n = strlen(name), k = sizeof(kTitle) - 1;
    if (n < k || strcasecmp(name + n - k, kTitle) != 0) {
        return;
    }
    gBootFast = 0;
    re4_log("DVD: start reads done (%s): %u steps without a frame's wait, %u with; the game's pace from here",
            name, gBootSteps - gBootWaits, gBootWaits);
}

s32 DVDConvertPathToEntrynum(const char* path)
{
    Disc* d = current();
    return d ? lookup(*d, path) : -1;
}

int DVDFastOpen(s32 entrynum, DVDFileInfo* fi)
{
    Disc* d = current();
    if (!d || entrynum <= 0 || (u32) entrynum >= d->count || isDir(*d, (u32) entrynum)) {
        return 0;
    }
    fi->startAddr = word(*d, (u32) entrynum, 1);
    fi->length = word(*d, (u32) entrynum, 2);
    fi->callback = 0;
    fi->cb.state = kStateEnd;
    fi->cb.transferredSize = 0;
    return 1;
}

int DVDOpen(const char* path, DVDFileInfo* fi)
{
    s32 entry = DVDConvertPathToEntrynum(path);
    if (entry < 0) {
        re4_log("DVD: DVDOpen: %s not on disc", path);
        return 0;
    }
    return DVDFastOpen(entry, fi);
}

int DVDClose(DVDFileInfo* fi)
{
    (void) fi;
    return 1;
}

// The end of a read: state and callback (result -3: cancelled).
static void finishRead(void* object, int result)
{
    DVDFileInfo* fi = (DVDFileInfo*) object;
    fi->cb.state = result == kResultCanceled ? kStateCanceled : result < 0 ? -1 : kStateEnd;
    if (fi->callback) {
        fi->callback(result, fi);
    }
}

int DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
    (void) prio;
    fi->cb.command = kCommandRead;
    fi->cb.addr = addr;
    fi->cb.length = (u32) length;
    fi->cb.offset = (u32) offset;
    fi->callback = callback;
    fi->cb.state = kStateBusy;
    s32 result = readFile(fi, addr, length, offset);
    fi->cb.transferredSize = result > 0 ? (u32) result : 0;
    if (re4_os_in_interrupt() && re4_os_defer(finishRead, fi, result)) {
        return 1;  // busy until the interrupt handler has returned
    }
    finishRead(fi, result);
    return 1;
}

int DVDReadAsync(DVDFileInfo* fi, void* addr, s32 length, s32 offset, DVDCallback callback)
{
    return DVDReadAsyncPrio(fi, addr, length, offset, callback, 2);
}

s32 DVDReadPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, s32 prio)
{
    (void) prio;
    s32 result = readFile(fi, addr, length, offset);
    fi->cb.transferredSize = result > 0 ? (u32) result : 0;
    fi->cb.state = result < 0 ? -1 : kStateEnd;
    return result;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block)
{
    if (block->state == kStateBusy || block->state == kStateCanceled) {
        return block->state;
    }
    return block->state < 0 ? -1 : kStateEnd;
}

s32 DVDGetDriveStatus(void)
{
    return kStateEnd;
}

u32 DVDGetTransferredSize(DVDFileInfo* fi)
{
    return fi->cb.transferredSize;
}

int DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback)
{
    // a read still waiting for its completion ends as cancelled (its callback
    // gets -3, as from the drive)
    re4_os_deferred_set(finishRead, block, kResultCanceled);
    if (callback) {
        callback(0, block);
    }
    return 1;
}

// The synchronous form (gcci.c, the CRI file interface): as DVDCancelAsync.
s32 DVDCancel(volatile DVDCommandBlock* block)
{
    re4_os_deferred_set(finishRead, (void*) block, kResultCanceled);
    return 0;
}

s32 DVDCancelAll(void)
{
    return 0;
}

DVDDiskID* DVDGetCurrentDiskID(void)
{
    current();
    re4_mem1_init();
    return (DVDDiskID*) (unsigned long) RE4_MEM1_BASE;
}

void* DVDGetFSTLocation(void)
{
    current();
    re4_mem1_init();
    return (void*) (unsigned long) *(volatile u32*) (unsigned long) (RE4_MEM1_BASE + 0x38);
}

int DVDCompareDiskID(const DVDDiskID* a, const DVDDiskID* b)
{
    if (memcmp(a->gameName, b->gameName, 4) != 0 || memcmp(a->company, b->company, 2) != 0) {
        return 0;
    }
    if (a->diskNumber != 0xFF && b->diskNumber != 0xFF && a->diskNumber != b->diskNumber) {
        return 0;
    }
    if (a->gameVersion != 0xFF && b->gameVersion != 0xFF && a->gameVersion != b->gameVersion) {
        return 0;
    }
    return 1;
}

DVDDiskID* DVDGenerateDiskID(DVDDiskID* id, const char* game, const char* company, unsigned char diskNo,
                             unsigned char version)
{
    memset(id, 0, sizeof(*id));
    if (game) {
        memcpy(id->gameName, game, 4);
    }
    if (company) {
        memcpy(id->company, company, 2);
    }
    id->diskNumber = diskNo;
    id->gameVersion = version;
    return id;
}

// The disc swap of the two-disc game: the folder of the requested disc
// becomes the inserted one.
int DVDChangeDiskAsync(DVDCommandBlock* block, DVDDiskID* id, DVDCBCallback callback)
{
    int no = id->diskNumber == 0xFF ? 0 : id->diskNumber;
    block->command = kCommandChangeDisk;
    block->id = id;
    bool ok = no < 2 && insertDisc(no);
    if (!ok) {
        re4_log("DVD: disc %d requested, but %s/disc%d is missing", no + 1, gDataDir, no);
    }
    block->state = ok ? kStateEnd : -1;
    if (callback) {
        callback(ok ? 0 : -1, block);
    }
    return 1;
}

int DVDOpenDir(const char* path, DVDDir* dir)
{
    Disc* d = current();
    s32 entry = d ? lookup(*d, path) : -1;
    if (entry < 0 || !isDir(*d, (u32) entry)) {
        return 0;
    }
    dir->entryNum = (u32) entry;
    dir->location = (u32) entry + 1;
    dir->next = word(*d, (u32) entry, 2);
    return 1;
}

int DVDReadDir(DVDDir* dir, DVDDirEntry* out)
{
    Disc* d = current();
    if (!d || dir->location <= dir->entryNum || dir->location >= dir->next) {
        return 0;
    }
    u32 i = dir->location;
    out->entryNum = i;
    out->isDir = isDir(*d, i);
    out->name = (char*) entryName(*d, i);
    dir->location = out->isDir ? word(*d, i, 2) : i + 1;
    return 1;
}

int DVDCloseDir(DVDDir* dir)
{
    (void) dir;
    return 1;
}

}  // extern "C"

// ---- the skins in the cutscenes (patch 0915) ------------------------------------------------------------------
//
// Wish of DevJunior, 12.11.2026: "the skins of Ada and Ashley do not show in the cutscenes". An event brings its own
// copies of the characters' models (the bin table of its evd: "em/pl01/pl010a.bin" ...); only Leon's are taken from
// his archive (event.cpp ExeBeginEvt). Patch 0915 asks for each bin of an evd whether a mod replaces the character
// archive that bin is a copy of: every character archive a mod pack replaces (em/pl??.drs / .das) is looked at on the
// disc once (read, converted as the game's read converts it, its entries' sizes and fingerprints kept, the copy
// given back); a bin whose bytes are those of one of its entries is drawn from the skin's archive (its body read
// once into the mods' place, runtime/mod_place.cpp, and converted) at the same entry. A bin no skin has (the
// event's own models, an expression head) stays the event's.
namespace {

struct SkinEntry {
    u32 len;                  // 0: empty
    unsigned long long head;  // fingerprint of the first 64 bytes (as converted)
    unsigned long long full;  // of all of them
};

struct SkinFile {
    char name[32];             // "em/pl01.drs"
    bool unusable;             // a packed body, no part table, no room ...
    std::vector<SkinEntry> entries;
    std::vector<u32> skinStart;  // the skin's entries (0: empty)
    unsigned char* skin;       // the mod's body in the mods' place, 0 until a bin matched
    bool skinTried;
};

std::vector<SkinFile> gSkins;
int gSkinsScanned = -2;        // gCurrent the list was made for
int gSkinsGeneration = -1;     // gModsGeneration it was made for

unsigned long long fnv(const unsigned char* p, u32 n)
{
    unsigned long long h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

// The DAT body of an archive file read whole: part 0 of its part table (not packed); 0 when none.
const unsigned char* archiveBody(const unsigned char* file, u32 size, u32* bodyLen)
{
    if (size < 0x440 || be32(file + 0x20) != 0) {
        return 0;
    }
    const u32 len = be32(file + 0x24), off = be32(file + 0x2C);
    if (off > size || len > size - off || len < 16) {
        return 0;
    }
    char text[0x40];
    memcpy(text, file + off, sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;
    if (isxdigit((unsigned char) text[0]) && strchr(text, '\t') && strchr(text, '\n')) {
        return 0;  // a yz2 body
    }
    *bodyLen = len;
    return file + off;
}

// The entries of a DAT body as the disc has it (big-endian): [start, length) each (an empty one: 0, 0); the REL
// behind them is no entry.
void bodyEntries(const unsigned char* body, u32 len, std::vector<std::pair<u32, u32>>& out)
{
    out.clear();
    const u32 n = be32(body);
    if (n == 0 || n > 4096 || 16 + n * 8 > len) {
        return;
    }
    u32 rel = be32(body + 4);
    if (rel < 16 + n * 8 || rel >= len || rel % 32) {
        rel = len;
    }
    for (u32 i = 0; i < n; i++) {
        const u32 start = be32(body + 16 + i * 4);
        const u32 stop = i + 1 < n ? be32(body + 16 + (i + 1) * 4) : rel;
        out.push_back(start && start < stop && stop <= len ? std::make_pair(start, stop - start)
                                                           : std::make_pair(0u, 0u));
    }
}

// A body copied to the mods' place and converted as the game's read of `name` converts it; 0 when no room.
unsigned char* placeConverted(const char* name, const unsigned char* body, u32 len, const char* what)
{
    unsigned char* place = (unsigned char*) re4_mod_place_alloc(len, what);
    if (!place) {
        return 0;
    }
    memcpy(place, body, len);
    struct {
        u32 type, size, dest, ofs, sndType, sndArg, sndNo, x1C;
    } h = {0 /* TRANS_MRAM */, len, (u32) (unsigned long) place, 0, 0, 0, 0, 0};
    re4_formats_part_loaded(name, 0, 0, &h, (unsigned) (unsigned long) place);
    return place;
}

void scanSkins(Disc& d)
{
    for (SkinFile& f : gSkins) {
        if (f.skin) {
            re4_mod_em_release(f.skin);  // the mods changed: the place's blocks back
        }
    }
    gSkins.clear();
    gSkinsScanned = gCurrent;
    gSkinsGeneration = gModsGeneration;
    if (!d.mod) {
        return;
    }
    for (u32 i = 1; i < d.count; i++) {
        if (!d.mod[i] || isDir(d, i)) {
            continue;
        }
        char path[1400];
        entryPath(d, i, path, sizeof(path));  // the disc's own file
        const char* rel = strstr(path, "/files/");
        if (!rel || strncmp(rel + 7, "em/pl", 5) != 0 || strchr(rel + 7 + 3, '/') || strlen(rel + 7) >= 32) {
            continue;
        }
        SkinFile f;
        snprintf(f.name, sizeof(f.name), "%s", rel + 7);
        f.unusable = true;
        f.skin = 0;
        f.skinTried = false;
        u32 size = 0;
        unsigned char* file = readWhole(path, &size);
        u32 len = 0;
        const unsigned char* body = file ? archiveBody(file, size, &len) : 0;
        std::vector<std::pair<u32, u32>> ent;
        if (body) {
            bodyEntries(body, len, ent);
        }
        unsigned char* conv = ent.empty() ? 0 : placeConverted(f.name, body, len, "the disc's archive, looked at");
        if (conv) {
            for (auto& e : ent) {
                SkinEntry s = {e.second, 0, 0};
                if (e.second) {
                    s.head = fnv(conv + e.first, e.second < 64 ? e.second : 64);
                    s.full = fnv(conv + e.first, e.second);
                }
                f.entries.push_back(s);
            }
            f.unusable = false;
            re4_mod_em_release(conv);
        } else {
            re4_log("mods: %s: the disc's file could not be looked at, its skin stays out of the cutscenes", f.name);
        }
        free(file);
        gSkins.push_back(f);
    }
    if (!gSkins.empty()) {
        re4_log("mods: %u character archives with a skin: their models in the cutscenes too",
                (unsigned) gSkins.size());
    }
}

// The skin's entry `index` of f (its body read into the mods' place and converted on first use); 0 when none.
void* skinEntry(Disc& d, SkinFile& f, u32 index)
{
    if (!f.skin && !f.skinTried) {
        f.skinTried = true;
        s32 entry = lookup(d, f.name);
        u32 size = 0;
        unsigned char* file = entry > 0 && d.mod[entry] ? readWhole(d.mod[entry], &size) : 0;
        u32 len = 0;
        const unsigned char* body = file ? archiveBody(file, size, &len) : 0;
        std::vector<std::pair<u32, u32>> ent;
        if (body) {
            bodyEntries(body, len, ent);
        }
        if (!ent.empty()) {
            char what[64];
            snprintf(what, sizeof(what), "the skin %s for the cutscenes", f.name);
            f.skin = placeConverted(f.name, body, len, what);
            for (auto& e : ent) {
                f.skinStart.push_back(e.first);
            }
        }
        free(file);
    }
    return f.skin && index < f.skinStart.size() && f.skinStart[index] ? f.skin + f.skinStart[index] : 0;
}

u32 native32(const unsigned char* p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

}  // namespace

extern "C" {

// Patch 0915 (event.cpp EventMgr::SetEvd, the game thread): the bin `index` of the evd at `header` (as converted) is
// `data`; what the event draws for it: the skin's same model or palette when the bin is a copy of an entry of a
// character archive a mod replaces, else `data`.
void* re4_mod_event_bin(const void* header, int index, const char* name, void* data)
{
    Disc* d = current();
    if (!d || !d->mods || !name || !data || strncmp(name, "em/pl", 5) != 0) {
        return data;
    }
    if (gSkinsScanned != gCurrent || gSkinsGeneration != gModsGeneration) {
        scanSkins(*d);
    }
    if (gSkins.empty()) {
        return data;
    }
    // the bin's room: up to the next bin's data in the table (the last one: the fingerprints alone decide)
    const unsigned char* hdr = (const unsigned char*) header;
    const u32 nBin = native32(hdr + 0x48), binOfs = native32(hdr + 0x4C);
    const u32 own = native32(hdr + binOfs + (u32) index * 0x40 + 0x30);
    u32 room = 0;
    for (u32 i = 0; i < nBin && i < 4096; i++) {
        const u32 o = native32(hdr + binOfs + i * 0x40 + 0x30);
        if (o > own && (!room || o - own < room)) {
            room = o - own;
        }
    }
    const unsigned char* p = (const unsigned char*) data;
    static int told;
    for (SkinFile& f : gSkins) {
        if (f.unusable) {
            continue;
        }
        for (u32 i = 0; i < f.entries.size(); i++) {
            const SkinEntry& e = f.entries[i];
            if (!e.len || (room && (e.len > room || room - e.len >= 64))) {
                continue;
            }
            if (fnv(p, e.len < 64 ? e.len : 64) != e.head || fnv(p, e.len) != e.full) {
                continue;
            }
            void* skin = skinEntry(*d, f, i);
            if (told++ < 64) {
                re4_log("mods: event bin %s = %s entry %u: %s", name, f.name, (unsigned) i,
                        skin ? "the skin's" : "the event's (the skin has none)");
            }
            return skin ? skin : data;
        }
    }
    if (told++ < 64) {
        re4_log("mods: event bin %s (%u bytes) is no entry of a skinned character archive: the event's own", name,
                (unsigned) room);
    }
    return data;
}

}  // extern "C"
