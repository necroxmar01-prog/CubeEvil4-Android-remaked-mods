// Leon's outfit and the weapon on his back (wishes of DevJunior, 09.11.2026, the remake mods). Compiled with the
// game's flags into libre4_main (re4_gamehooks_obj); the app sends both with the aim bits (runtime/aim_options.cpp:
// bits 22..23 the outfit, bit 30 the weapon on the back).
//
//  - "Leon's outfit": the story's rule (pl_sub.cpp PlSetCostume: the jacket until the r106 event, then the
//    second outfit, the armour with its item), or always the first (pl00, the jacket) or the second (pl08, without
//    it). Patch 0902 lets PlSetCostume ask re4_leon_outfit; a change while playing restarts the room where Leon
//    stands, as the cheat menu's character does (runtime/cheats.cpp), and the room's start reads his archive anew
//    (re4_wardrobe_room). The special costume (game_costume 1) and the other characters keep theirs.
//  - "Weapon on the back": the last long weapon Leon held (shotguns, rifles, TMP, Chicago Typewriter, rocket
//    launcher, mine thrower) hangs on his back while it is still in the case and not in his hands. Its model comes
//    from the weapon's own file (the first part, the archive the weapon module reads; read here with DVDRead, so
//    none of the file's sounds reaches the weapon's sound block) to a place of its own in the development half of
//    MEM1, the rocket launcher's from Leon's archive (objRocket.cpp: entries 0x76 / 0x75). It is an event body
//    (SetObj18, as the events show weapons) that follows his chest (parts 2, OyaSetObj18): the barrel up over the
//    right shoulder at 60 degrees, between the shoulder blades, behind the back by the model's own bounds. Hidden in events, sub screens and movies.
#include "types.h"
#include "global.h"
#include "item.h"
#include "main.h"
#include "dvd.h"
#include "player.h"
#include "sce_at.h"
#include "em.h"
#include "obj.h"
#include "obj18.h"
#include "model.h"
#include <dolphin/dvd.h>

#include <atomic>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_formats_part_loaded(const char* name, int nest, int index, const void* part, unsigned addr);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
int PlSetCostume();                                            // pl_sub.cpp

namespace {

// the weapon's archive: behind the cheats' places (up to 0x824E0000), before the larger player archive's
// (RE4_MEM1_PLAYER_ALT_ADDR, 0x82600000)
const u32 kBackAddr = 0x82500000u;
const u32 kBackMax = 0x100000u;

std::atomic<int> gOutfit(0);     // 0 the story's rule, 1 the first outfit, 2 the second
std::atomic<bool> gBack(false);  // the weapon on the back
std::atomic<bool> gHooked(false);

int gTried = -1;          // the outfit a room restart was asked for (once per choice)
bool gForced = false;     // an outfit of the app's was put on (back to the story's rule once it is chosen again)
int gLastNo = -1;         // the last long weapon Leon held, -1 none
int gLastType = 0;
int gLoadedNo = -1;       // the weapon whose archive is at kBackAddr (the rocket launcher needs none)
int gLoadedType = -1;
cObj* gObj = 0;           // the body on the back
void* gObjModel = 0;      // its model data (to know it among the event bodies)
int gObjNo = -1, gObjType = -1;

bool storyGame()
{
    return !SysFlagChk(pG, SYS_OMAKE_ADA_GAME) && !SysFlagChk(pG, SYS_OMAKE_ETC_GAME);
}

// Leon's costume by the story's rule (pl_sub.cpp PlSetCostume, game_costume 0)
int storyCostume()
{
    if (ItemMgr.num(0xFE, 0)) {
        return 2;
    }
    return ScfFlagChk(pG, SCF_R106_EVENT) ? 1 : 0;
}

bool outfitApplies()
{
    return pG && pG->pl_type == 0 && pG->game_costume != 1 && storyGame();
}

bool playerFree()
{
    return pG && pPL && pG->Rno0 == 3 && !StaFlagChk(pG, STA_EVENT) && !StaFlagChk(pG, STA_SUB_SCRN) &&
           !StaFlagChk(pG, STA_MOVIE_ON);
}

// ---- the weapon on the back ----------------------------------------------------------------------------------

bool longWeapon(int no)
{
    return no >= 0x7 && no <= 0xE;
}

// The weapon's file (read.cpp ReadWepData, Leon's table) and its model's entries (bin, tpl) as the weapon's
// object takes them; the rocket launcher's from the player's archive (file 0).
struct BackModel {
    const char* file;
    int bin, tpl;
};

BackModel backModel(int no, int type)
{
    switch (no) {
    case 0x7:
        return {"em/wep07.drs", 5, 6};  // wep07.cpp cObjShotgun
    case 0x8:
        return {"em/wep08.drs", 5, 6};  // wep08.cpp cObjStriker
    case 0x9:                           // objSniper.cpp
        return {type == 1 ? "em/wep21.drs" : type == 2 ? "em/wep24.drs" : "em/wep09.drs", 0xA, 0x9};
    case 0xA:                           // objHkSniper.cpp
        return {type == 1 ? "em/wep31.drs" : type == 2 ? "em/wep32.drs" : "em/wep10.drs", 0xA, 0x9};
    case 0xB:                           // objMachinegun.cpp: the model by the weapon's type
        return {type > 1 ? "em/wep20.drs" : "em/wep11.drs", 6 + (type < 0 ? 0 : type > 3 ? 3 : type), 5};
    case 0xC:
        return {"em/wep12.drs", 6, 5};  // objTompson.cpp
    case 0xD:
        return {0, 0x76, 0x75};         // objRocket.cpp cObjLauncher: Leon's archive
    case 0xE:
        return {type == 1 ? "em/wep25.drs" : "em/wep14.drs", 8, 7};  // objMine.cpp
    }
    return {0, 0, 0};
}

u32 be32At(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

// The weapon's archive (the file's first part) to kBackAddr, converted as a read archive is
bool loadArchive(const char* file)
{
    DVDFileInfo fi;
    if (!DVDOpen((char*) file, &fi)) {
        re4_log("wardrobe: %s not readable: no weapon on the back", file);
        return false;
    }
    u8* const base = (u8*) (unsigned long) kBackAddr;
    u32 partOfs = 0, partSize = 0;
    if (DVDReadPrio(&fi, base, 0x400, 0, 2) == 0x400) {
        for (u32 at = 0x20; at + 0x20 <= 0x400; at += 0x20) {
            u32 kind = be32At(base + at);
            if (kind == 0xFFFFFFFFu) {
                break;
            }
            if (kind == TRANS_MRAM) {
                partSize = be32At(base + at + 4);
                partOfs = be32At(base + at + 0xC);
                break;
            }
        }
    }
    const u32 len = (partSize + 0x1F) & ~0x1Fu;
    const bool ok = partSize >= 0x20 && len <= kBackMax && DVDReadPrio(&fi, base, (s32) len, (s32) partOfs, 2) == (s32) len;
    DVDClose(&fi);
    if (!ok) {
        re4_log("wardrobe: %s: no archive of at most %u bytes (part of %u bytes at 0x%x)", file, (unsigned) kBackMax,
                (unsigned) partSize, (unsigned) partOfs);
        return false;
    }
    DvdHeader h;
    memset(&h, 0, sizeof(h));
    h.type = TRANS_MRAM;
    h.size = partSize;
    h.dest = kBackAddr;
    re4_formats_part_loaded(file, 0, 0, &h, kBackAddr);
    re4_log("wardrobe: %s (%u bytes) read to 0x%08x for the weapon on the back", file, (unsigned) partSize,
            (unsigned) kBackAddr);
    return true;
}

// An archive's entry (PL_ARC_PTR) when the archive has it, else 0
void* arcEntry(const PlArc* arc, int no)
{
    if (!arc) {
        return 0;
    }
    const u32 count = arc->ofs[0];
    if (no < 4 || (u32) (no - 4) >= count || count > 0x400 || arc->ofs[no] == 0) {
        return 0;
    }
    return PL_ARC_PTR(arc, no);
}

// Ours among the objects: still alive and still showing our model
bool objOurs()
{
    if (!gObj || !gObj->isAlive() || gObj->id != cObjMgr::ID_EVENT || !gObj->pModelInfo) {
        return false;
    }
    return (void*) gObj->pModelInfo->model_addr == gObjModel;
}

void dropObj(const char* why)
{
    if (objOurs()) {
        ObjMgr.destroy(gObj);
        re4_log("wardrobe: the weapon on the back removed (%s)", why);
    }
    gObj = 0;
    gObjModel = 0;
    gObjNo = gObjType = -1;
}

}  // namespace

// The place on the back for a weapon model of these bounds (centre c, half extents s; the model's own space): the
// weapons' files hold them as Leon's right hand does, the barrel along -x (his arm points along -x in the bind pose;
// device test 09.11.2026: turned for +x the Chicago Typewriter hung with its stock up), the rocket launcher's model
// (Leon's archive) along +x (barrelX). ang.z turns the barrel up over the right shoulder (Leon's right is -x), 60
// degrees from the floor as a sling holds it; pos puts the turned centre between the shoulder blades, in the
// chest's space (parts 2: its joint about 1360 high, the back's surface about 125 behind it), the model's front just
// behind the back. Exported for the runtime tests.
extern "C" void re4_back_place(const float c[3], const float s[3], int barrelX, float pos[3], float ang[3])
{
    const float d = 60.0f * 3.14159265f / 180.0f;  // the barrel's angle over the floor
    // the turn that takes the barrel's axis (+x or -x) to (-cos d, sin d), up over the right shoulder
    const float a = barrelX > 0 ? 3.14159265f - d : -d;
    const float ca = cosf(a), sa = sinf(a);
    const float tx = 0.0f, ty = -60.0f, back = -125.0f - 15.0f;
    pos[0] = tx - (c[0] * ca - c[1] * sa);
    pos[1] = ty - (c[0] * sa + c[1] * ca);
    pos[2] = back - (c[2] + s[2]);
    ang[0] = 0.0f;
    ang[1] = 0.0f;
    ang[2] = a;
}

namespace {

void showBack(bool want)
{
    if (!want) {
        if (objOurs()) {
            gObj->setTrans(0);
        }
        return;
    }
    const BackModel m = backModel(gLastNo, gLastType);
    if (!m.bin) {
        return;
    }
    if (objOurs() && gObjNo == gLastNo && gObjType == gLastType) {
        gObj->setTrans(1);
        return;
    }
    dropObj("another weapon");
    const PlArc* arc = 0;
    if (m.file) {
        if (gLoadedNo != gLastNo || gLoadedType != gLastType) {
            gLoadedNo = gLoadedType = -1;
            if (!loadArchive(m.file)) {
                gLastNo = -1;  // not again until Leon holds a long weapon anew
                return;
            }
            gLoadedNo = gLastNo;
            gLoadedType = gLastType;
        }
        arc = (const PlArc*) (unsigned long) kBackAddr;
    } else {
        arc = pG->pPlayer;
    }
    void* bin = arcEntry(arc, m.bin);
    void* tpl = arcEntry(arc, m.tpl);
    if (!bin || !tpl) {
        re4_log("wardrobe: weapon 0x%x type %d: no model at entries 0x%x / 0x%x", gLastNo, gLastType, m.bin, m.tpl);
        gLastNo = -1;
        return;
    }
    Vec pos = {0.0f, 0.0f, 0.0f};
    Vec rot = {0.0f, 0.0f, 0.0f};
    cObj* obj = SetObj18(bin, tpl, &pos, &rot, OBJ18_TYPE_WEPXX);
    if (!obj || !obj->pModelInfo) {
        re4_log("wardrobe: weapon 0x%x: its body could not be made", gLastNo);
        gLastNo = -1;
        return;
    }
    const cBoundingBox& b = obj->pModelInfo->bound;
    const float c[3] = {b.offset.x, b.offset.y, b.offset.z};
    const float s[3] = {b.size.x, b.size.y, b.size.z};
    float p[3], a[3];
    re4_back_place(c, s, m.file ? -1 : 1, p, a);
    obj->pos.x = p[0];
    obj->pos.y = p[1];
    obj->pos.z = p[2];
    obj->ang.x = a[0];
    obj->ang.y = a[1];
    obj->ang.z = a[2];
    obj->atari.off();
    obj->setNoClip(1);
    obj->setTrans(1);
    OyaSetObj18(obj, (cModel*) pPL, 2);
    gObj = obj;
    gObjModel = (void*) obj->pModelInfo->model_addr;
    gObjNo = gLastNo;
    gObjType = gLastType;
    re4_log("wardrobe: weapon 0x%x type %d on the back (bounds centre %.0f %.0f %.0f, half %.0f %.0f %.0f)", gLastNo,
            gLastType, c[0], c[1], c[2], s[0], s[1], s[2]);
}

void backFrame()
{
    if (!gBack.load(std::memory_order_relaxed) && !gObj) {
        return;
    }
    if (!pG || !pPL || pG->pl_type != 0) {
        showBack(false);
        return;
    }
    const int no = pG->weapon_no;
    if (longWeapon(no)) {
        gLastNo = no;
        gLastType = pG->weapon_type;
    }
    bool want = gBack.load(std::memory_order_relaxed) && gLastNo >= 0 && no != gLastNo && playerFree();
    if (want) {
        const u16 id = WeaponNo2WeaponId((u8) gLastNo, (u8) gLastType);
        if (id == 0xFFFF || ItemMgr.num(id) == 0) {
            want = false;  // sold, or never in the case
        }
    }
    showBack(want);
}

// A changed outfit while playing: the room starts again where Leon stands (once per choice), its start reads his
// archive (re4_wardrobe_room)
void outfitFrame()
{
    if (!outfitApplies() || !playerFree()) {
        return;
    }
    const int outfit = gOutfit.load(std::memory_order_relaxed);
    if (outfit == 0 && !gForced) {
        return;  // the story's rule, as the game keeps it
    }
    const int want = outfit == 1 ? 0 : outfit == 2 ? 1 : storyCostume();
    if (want == pG->pl_costume) {
        gForced = outfit != 0;
        gTried = -1;
        return;
    }
    if (gTried == outfit) {
        return;
    }
    gTried = outfit;
    Vec pos = pPL->pos;
    Vec ang = pPL->ang;
    SceAtExecRoomJump(G_ROOM_ID, &pos, &ang, pG->Part);
    re4_log("wardrobe: Leon's outfit %d (costume %d, was %d): r%03x starts again", outfit, want, pG->pl_costume,
            (unsigned) G_ROOM_ID);
}

void hook(unsigned)
{
    outfitFrame();
    backFrame();
}

}  // namespace

// The app's choice (aim bits 22..23 and 30, runtime/aim_options.cpp). Any thread.
extern "C" void re4_wardrobe_set(int outfit, int back)
{
    outfit = outfit < 0 || outfit > 2 ? 0 : outfit;
    const int before = gOutfit.exchange(outfit);
    const bool backBefore = gBack.exchange(back != 0);
    if (before != outfit || backBefore != (back != 0)) {
        re4_log("wardrobe: Leon's outfit %s, weapon on the back %s",
                outfit == 1 ? "the first (jacket)" : outfit == 2 ? "the second (no jacket)" : "the story's",
                back ? "on" : "off");
    }
    if (!gHooked.exchange(true)) {
        re4_add_frame_hook(hook);
    }
}

// Patch 0902 (pl_sub.cpp PlSetCostume): Leon's costume after the story's rule, as the app's choice wants it
extern "C" int re4_leon_outfit(int costume)
{
    if (!outfitApplies()) {
        return costume;
    }
    const int outfit = gOutfit.load(std::memory_order_relaxed);
    return outfit == 1 ? 0 : outfit == 2 ? 1 : costume;
}

// Patch 0195's room start (runtime/cheats.cpp re4_story_char_room, before ReadPlayerData): the outfit the app
// wants (PlSetCostume asks re4_leon_outfit and marks the archive to be read), and the weapon on the back forgets
// its body (the room's objects go with the old room)
extern "C" void re4_wardrobe_room(void)
{
    dropObj("a room starts");
    if (!outfitApplies()) {
        return;
    }
    const int want = re4_leon_outfit(storyCostume());
    if (want != pG->pl_costume) {
        re4_log("wardrobe: Leon's costume %d from r%03x on (was %d)", want, (unsigned) pG->room_id, pG->pl_costume);
        PlSetCostume();
    }
    gForced = gOutfit.load(std::memory_order_relaxed) != 0;
}
