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
//    across the back as a sling holds it (stock up over the right shoulder, 45 degrees), flat on the back's surface as
//    Leon's own model has it (a bulkier skin carries it as close). Hidden in sub screens and movies; in events
//    while Leon is drawn, if the app wants it.
//  - Its placement to taste (wish of DevJunior, 09.11.2026: "rotate it as I like, show it in cutscenes; it clashes
//    with the arm"): the app's second aim word (bit 31 set, runtime/aim_options.cpp) holds the slant, the height,
//    the distance from the back, the shoulder, turned over and "in cutscenes" (re4_back_adjust_set).
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
extern "C" int re4_player_modded(void);                        // runtime/dvd.cpp: the player's file is a mod
int PlSetCostume();                                            // pl_sub.cpp

namespace {

// the weapon's archive: behind the cheats' places (up to 0x824E0000), before the larger player archive's
// (RE4_MEM1_PLAYER_ALT_ADDR, 0x82600000)
const u32 kBackAddr = 0x82500000u;
const u32 kBackMax = 0x100000u;

std::atomic<int> gOutfit(0);     // 0 the story's rule, 1 the first outfit, 2 the second
std::atomic<bool> gBack(false);  // the weapon on the back
std::atomic<int> gAdjust(0);     // its placement (re4_back_adjust_set; 0: the defaults)
std::atomic<int> gAdjustSeen(0); // the placement the body on the back was put with
cModel* gCarrier = 0;            // whom it was put on (Leon, or the event's Leon in a cutscene)
int gWait = 0;                   // frames to wait after a body was destroyed before the next one is made
cObj* gLoggedEventLeon = 0;      // the event's Leon last described in the log
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

// The placement word (the app: CoreMenu, sent with bit 31 of the aim word): bits 0..6 the slant in degrees over the
// floor (10..80), bits 7..13 the height (v - 64) * 5 over the default, bits 14..20 the distance from the back
// (v - 64) * 2, bit 21 over the left shoulder, bit 22 turned over (the grip up), bit 23 also in cutscenes.
struct Adjust {
    float slant, height, dist;
    bool left, over, events;
};

Adjust adjustOf(int w)
{
    Adjust a = {45.0f, 0.0f, 0.0f, false, false, false};
    if (w == 0) {
        return a;
    }
    const int slant = w & 0x7F;
    a.slant = slant < 10 ? 10.0f : slant > 80 ? 80.0f : (float) slant;
    a.height = (float) (((w >> 7) & 0x7F) - 64) * 5.0f;
    a.dist = (float) (((w >> 14) & 0x7F) - 64) * 2.0f;
    a.left = (w >> 21) & 1;
    a.over = (w >> 22) & 1;
    a.events = (w >> 23) & 1;
    return a;
}

// Leon in a cutscene: the event's own body "pl0000" (event.cpp ExePacket_SetOm, an obj18 of type LEON with his
// archive's model; the player himself is not drawn then). 0 when the event has none.
cObj* eventLeon()
{
    for (u32 i = 0; i < ObjMgr.nArray; i++) {
        cObj* o = (cObj*) ((u8*) ObjMgr.pArray + ObjMgr.size * i);
        if (!o->isAlive() || o->id != cObjMgr::ID_EVENT || o->kindid != 1 || o == gObj || !o->pModelInfo) {
            continue;
        }
        const FREE_OBJ18* w = OBJ18_WK((cObj18*) o);
        if (w->obj18_type == OBJ18_TYPE_LEON && strncmp(w->NameMod, "pl0000", 6) == 0) {
            return o;
        }
    }
    return 0;
}

// Who carries the weapon now: Leon while playing (no sub screen or movie); in an event, if the app wants it, the
// event's Leon while he is drawn. 0: nobody.
cModel* backCarrier(const Adjust& a)
{
    if (!pG || !pPL || pG->Rno0 != 3 || StaFlagChk(pG, STA_SUB_SCRN) || StaFlagChk(pG, STA_MOVIE_ON)) {
        return 0;
    }
    if (StaFlagChk(pG, STA_EVENT)) {
        if (!a.events) {
            return 0;
        }
        cObj* ev = eventLeon();
        return ev && (ev->be_flag & 2) ? (cModel*) ev : 0;
    }
    return (cModel*) pPL;
}

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
    gCarrier = 0;
}

}  // namespace

// The place on the back for a weapon model of these bounds (centre c; the model's own space; device tests
// 09.11.2026, DevJunior's pictures of the remake and the weapons' files wep07/wep09 he sent): slung across the back,
// the stock up over a shoulder and the muzzle down to the other hip, `slant` degrees from the floor, its side flat on
// the back. The weapons' models (wepNN.drs, and the rocket launcher's in Leon's archive) lie as in his hand: the
// barrel along x (the stock towards +x), the profile (stock drop, grip, magazine, sight) along z (the grip towards
// -z), the width along y. A quarter turn about x first (ang x +-90 degrees, sign s: z -> -s y, y -> s z) lays the
// profile into the back's plane, the grip hanging below the barrel (or above it, `over`); then the turn about z sets
// the slant (RotMatrix: Rz * Ry * Rx): the stock up over the right shoulder (Leon's right is -x; `left` the other).
// pos puts the turned centre (c) on the spine at the height of the shoulder blades + `height`, in the chest's space
// (parts 2, its joint about 1360 high and 13 behind the hips), the model's face towards the body (`front`,
// re4_back_front with the same s) at `back` (the chest's z of the back's surface, measured on Leon's own model:
// re4_back_surface) and `dist` farther. Exported for the runtime tests.
extern "C" int re4_back_turn(int left, int over)
{
    return (left ? -1 : 1) * (over ? -1 : 1);
}

extern "C" void re4_back_place(const float c[3], float front, int left, int over, float slant, float height,
                               float dist, float back, float pos[3], float ang[3])
{
    const float d = slant * 3.14159265f / 180.0f;
    // the turn that takes the stock's axis (+x) up over the shoulder: (-cos d, sin d) right, (cos d, sin d) left
    const float a = left ? d : 3.14159265f - d;
    const float ca = cosf(a), sa = sinf(a);
    const float s = (float) re4_back_turn(left, over);
    const float tx = 0.0f, ty = 10.0f + height;  // shoulder-blade height (device test: -60 hung too low)
    // the centre after the quarter turn about x: (x, -s z, s y)
    const float qx = c[0], qy = -s * c[2];
    pos[0] = tx - (qx * ca - qy * sa);
    pos[1] = ty - (qx * sa + qy * ca);
    pos[2] = back - front - dist;
    ang[0] = s * 3.14159265f / 2.0f;
    ang[1] = 0.0f;
    ang[2] = a;
}

// A weapon model's face towards the back after the quarter turn about x (its s * y becomes z): 85 % of its points
// lie behind it (the barrel, the stock, the body; a sling swivel or a bolt handle that sticks out sideways does not
// hold the weapon off the back, as the bounds would)
extern "C" float re4_back_front(const void* model, int s)
{
    const cModelData* d = (const cModelData*) model;
    const s16* v = d ? (const s16*) d->vtxOrig : 0;
    if (!v || d->nVtx == 0) {
        return 0.0f;
    }
    // a histogram of s * y in 1/64 steps of its range (no sort, no allocation)
    int lo = 1 << 20, hi = -(1 << 20);
    for (u32 i = 0; i < d->nVtx; i++) {
        const int y = s * v[i * 4 + 1];
        lo = y < lo ? y : lo;
        hi = y > hi ? y : hi;
    }
    unsigned bins[64] = {0};
    const int span = hi - lo + 1;
    for (u32 i = 0; i < d->nVtx; i++) {
        bins[(s * v[i * 4 + 1] - lo) * 64 / span]++;
    }
    const u32 want = d->nVtx * 85 / 100;
    u32 seen = 0;
    int k = 0;
    for (; k < 63 && seen + bins[k] < want; k++) {
        seen += bins[k];
    }
    return (lo + (float) (k + 1) * span / 64.0f) / (float) (1 << d->shift);
}

// The back's surface in the chest's space: the farthest back point of Leon's models (his body and its parts, as
// drawn: the bind pose, model space) between the shoulder blades (1250..1450 high, within 120 of the spine), 5 into
// it (the cloth gives); -137 (the disc's Leon) when there is none
extern "C" float re4_back_surface(const void* firstInfo)
{
    float minZ = 1e9f;
    for (const cModelInfo* info = (const cModelInfo*) firstInfo; info && info->model_addr; info = info->pList) {
        const cModelData* d = info->model_addr;
        const s16* v = (const s16*) d->vtxOrig;
        const float sc = (float) (1 << d->shift);
        if (!v || d->nVtx == 0) {
            continue;
        }
        for (u32 i = 0; i < d->nVtx; i++, v += 4) {
            const float x = v[0] / sc, y = v[1] / sc, z = v[2] / sc;
            if (y > 1250.0f && y < 1450.0f && x > -120.0f && x < 120.0f && z < minZ) {
                minZ = z;
            }
        }
    }
    const float chestZ = -13.4f;  // parts 2's joint (its bind position)
    return minZ < 0.0f && minZ > -400.0f ? minZ - chestZ + 5.0f : -137.0f;
}

namespace {

// Puts our body on the carrier's chest (parts 2) as the placement word wants; measures the back on the carrier's
// own models (a skin, or the event's Leon with the event's parts)
void placeOn(cObj* obj, cModel* carrier, int adjustWord, bool log)
{
    const cBoundingBox& b = obj->pModelInfo->bound;
    const float c[3] = {b.offset.x, b.offset.y, b.offset.z};
    const float s[3] = {b.size.x, b.size.y, b.size.z};
    const Adjust adj = adjustOf(adjustWord);
    float p[3], a[3];
    re4_back_place(c, re4_back_front(obj->pModelInfo->model_addr, re4_back_turn(adj.left, adj.over)), adj.left,
                   adj.over, adj.slant, adj.height, adj.dist, re4_back_surface(carrier->pModelInfo), p, a);
    obj->pos.x = p[0];
    obj->pos.y = p[1];
    obj->pos.z = p[2];
    obj->ang.x = a[0];
    obj->ang.y = a[1];
    obj->ang.z = a[2];
    OyaSetObj18(obj, carrier, 2);
    gCarrier = carrier;
    gAdjustSeen.store(adjustWord);
    if (log) {
        re4_log("wardrobe: weapon 0x%x type %d on the back of %s (bounds centre %.0f %.0f %.0f, half %.0f %.0f %.0f; "
                "at %.0f %.0f %.0f, turned %.0f degrees)", gObjNo, gObjType,
                carrier == (cModel*) pPL ? "Leon" : "the event's Leon", c[0], c[1], c[2], s[0], s[1], s[2], p[0],
                p[1], p[2], a[2] * 57.2958f);
    }
}

void showBack(cModel* carrier)
{
    if (!carrier) {
        if (objOurs()) {
            gObj->setTrans(0);
        }
        return;
    }
    const BackModel m = backModel(gLastNo, gLastType);
    if (!m.bin) {
        return;
    }
    const int adjustWord = gAdjust.load(std::memory_order_relaxed);
    if (objOurs() && gObjNo == gLastNo && gObjType == gLastType) {
        // the same weapon: put anew on another carrier or with another placement, never destroyed for it (device
        // test 09.11.2026: destroying and making a body in the same frame crashed in LightSetModel)
        if (carrier != gCarrier || gAdjustSeen.load() != adjustWord) {
            placeOn(gObj, carrier, adjustWord, true);
        }
        gObj->setTrans(1);
        return;
    }
    if (objOurs()) {
        dropObj("another weapon");
        gWait = 3;  // the old body is gone first, the new one comes a few frames later
        return;
    }
    if (gWait > 0) {
        gWait--;
        return;
    }
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
    obj->atari.off();
    obj->setNoClip(1);
    obj->setNoSuspend(1);  // also while an event suspends the room's objects
    obj->setTrans(1);
    gObj = obj;
    gObjModel = (void*) obj->pModelInfo->model_addr;
    gObjNo = gLastNo;
    gObjType = gLastType;
    placeOn(obj, carrier, adjustWord, true);
}

// In a cutscene, what Leon is made of (once per event body; the report of DevJunior, 09.11.2026: his body vanishes
// in some cutscenes): his body from his archive or not, and each model on it
void describeEventLeon()
{
    if (!pG || !StaFlagChk(pG, STA_EVENT)) {
        gLoggedEventLeon = 0;
        return;
    }
    cObj* ev = eventLeon();
    if (!ev || ev == gLoggedEventLeon) {
        return;
    }
    gLoggedEventLeon = ev;
    const void* body = pG->pPlayer ? arcEntry(pG->pPlayer, 4) : 0;
    int n = 0;
    for (const cModelInfo* i = ev->pModelInfo; i && n < 16; i = i->pList, n++) {
        const cModelData* d = i->model_addr;
        re4_log("wardrobe: the event's Leon, model %d: %p%s, %u positions, %u textures", n, (const void*) d,
                (const void*) d == body ? " (his archive's body)" : "", d ? (unsigned) d->nVtx : 0u,
                d ? (unsigned) d->nTex : 0u);
    }
    re4_log("wardrobe: the event's Leon (r%03x): %d models, %s", (unsigned) G_ROOM_ID, n,
            (ev->be_flag & 2) ? "shown" : "hidden");
}

void backFrame()
{
    if (re4_player_modded()) {
        describeEventLeon();
    }
    if (!gBack.load(std::memory_order_relaxed) && !gObj) {
        return;
    }
    if (!pG || !pPL || pG->pl_type != 0) {
        showBack(0);
        return;
    }
    const int no = pG->weapon_no;
    if (longWeapon(no)) {
        gLastNo = no;
        gLastType = pG->weapon_type;
    }
    cModel* carrier = gBack.load(std::memory_order_relaxed) && gLastNo >= 0 && no != gLastNo
                          ? backCarrier(adjustOf(gAdjust.load(std::memory_order_relaxed)))
                          : 0;
    if (carrier) {
        const u16 id = WeaponNo2WeaponId((u8) gLastNo, (u8) gLastType);
        if (id == 0xFFFF || ItemMgr.num(id) == 0) {
            carrier = 0;  // sold, or never in the case
        }
    }
    showBack(carrier);
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

// The weapon on the back's placement (the app's second aim word, bit 31 set; adjustOf). Any thread; the body is put
// anew at the next frame.
extern "C" void re4_back_adjust_set(int word)
{
    word &= 0x7FFFFFFF;
    if (gAdjust.exchange(word) != word) {
        const Adjust a = adjustOf(word);
        re4_log("wardrobe: the weapon on the back %.0f degrees over the %s shoulder%s, height %+.0f, distance %+.0f%s",
                a.slant, a.left ? "left" : "right", a.over ? ", turned over" : "", a.height, a.dist,
                a.events ? ", also in cutscenes" : "");
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
