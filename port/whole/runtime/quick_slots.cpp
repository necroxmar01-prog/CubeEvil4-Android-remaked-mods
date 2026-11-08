// D-pad quick slots (the app menu's Controls page, "D-pad quick slots"; patch 0219's hook, a mod in the spirit of
// the 2023 remake). Compiled with the game's flags into libre4_main. Off (the default): nothing here changes the
// game.
//
// In the field (Leon's walk and aiming routines, no sub screen, event, scope or binoculars) a press of the D-pad
// equips at once a weapon of the attache case by its kind, without opening the case:
//   up     handguns and magnums     right  shotguns, the TMP, the Chicago Typewriter, the mine thrower, the
//                                          rocket launcher
//   down   rifles (and the bow)     left   grenades (hand, incendiary, flash)
// Pressed again it takes the next one of that kind the case holds (the order of the lists below), round. The
// change is the inventory's own (SubScreenExit, sscrn.cpp): ItemMgr.arm, the weapon's sounds stopped, then
// weaponRelease / weaponLoad / weaponInit. weaponLoad reads the weapon's module from the disc and sleeps while it
// does (ReadWepData: TaskSleep), so the change runs in a task of its own (slot 1, as the inventory; not while
// that slot is busy) with the game's main task suspended for those frames, as the inventory suspends it.
//
// The D-pad no longer walks or turns meanwhile, and its fixed aiming steps are off: re4_quick_slots_input takes
// its bits out of Key (the stick's stay) and out of Joy[0].on for the rest of the frame. Its presses are found
// from the raw D-pad of each frame (gDpadPrev), not from Joy[0].trg, which would fire again every frame after the
// bits were taken out (PadRead's old = the cleared on).
#include "types.h"
#include "global.h"
#include "main.h"
#include "joy.h"
#include "item.h"
#include "player.h"
#include "scheduler.h"
#include "snd.h"

extern cPlayer* pPL;
extern u32 Key_type_tbl[2][64];          // game/pad.cpp
extern "C" int re4_quick_slots_on(void);  // runtime/aim_options.cpp
extern "C" void re4_log(const char* fmt, ...);

namespace {

// the slots' item ids (cItem::id, item.cpp wep_info), in the order a repeated press goes through them
const u16 kUp[] = {0x23, 0x24, 0x21, 0x22, 0x40, 0x25, 0x26, 0x27, 0x28, 0x03, 0x29, 0x2A, 0x2B, 0x37};
const u16 kRight[] = {0x2C, 0x94, 0x2D, 0x30, 0x31, 0x32, 0x33, 0x3E, 0x34, 0x36, 0xAB, 0x35, 0x17, 0x6D};
const u16 kDown[] = {0x2E, 0x6B, 0x99, 0x2F, 0x6C, 0x51, 0x52};
const u16 kLeft[] = {0x01, 0x02, 0x0E};

struct Slot {
    const u16* ids;
    int n;
    const char* name;
};
const Slot kSlots[4] = {
    {kUp, (int) (sizeof kUp / sizeof kUp[0]), "up"},
    {kRight, (int) (sizeof kRight / sizeof kRight[0]), "right"},
    {kDown, (int) (sizeof kDown / sizeof kDown[0]), "down"},
    {kLeft, (int) (sizeof kLeft / sizeof kLeft[0]), "left"},
};

u32 gDpadPrev = 0;
// the weapon the change task equips (a static, not the task's argument: TaskExec_hook passes that as an int,
// scheduler.cpp, which on LP64 is not a pointer)
cItem* gSwapItem = 0;

// The weapon of a slot to equip: the next one of its kind in the case after the one held, else the first; NULL
// when the case holds none of its kind, or only the one held.
cItem* pick(int slot)
{
    const Slot& s = kSlots[slot];
    cItem* held = ItemMgr.weapon();
    cItem* found[16];
    int n = 0;
    int heldAt = -1;
    for (int i = 0; i < s.n && n < 16; i++) {
        cItem* p = ItemMgr.search(s.ids[i]);
        if (!p || p->num == 0) {
            continue;
        }
        if (p == held) {
            heldAt = n;
        }
        found[n++] = p;
    }
    if (n == 0) {
        return 0;
    }
    cItem* p = found[heldAt < 0 ? 0 : (heldAt + 1) % n];
    return p == held ? 0 : p;
}

// The field: walk routines (0..4) and the weapon / knife routines (6, 0xB), nothing else running.
bool field(cPlayer* pl)
{
    if (pl->r_no_0 != 0) {
        return false;
    }
    if (pl->r_no_1 > 4 && pl->r_no_1 != 6 && pl->r_no_1 != 0xB) {
        return false;
    }
    if (pl->stat.check(cPlayer::F_EVENT) || pl->stat.check(cPlayer::F_BINOCULAR) || pl->stat.check(cPlayer::F_CROUCH)) {
        return false;
    }
    return !StaFlagChk(pG, STA_SUB_SCRN) && !StaFlagChk(pG, STA_EVENT) && !StaFlagChk(pG, STA_SCOPE_CAMERA)
        && !StaFlagChk(pG, STA_BINOCULAR) && !StaFlagChk(pG, STA_PL_BOAT);
}

// The change, in slot 1 (TaskExec), the game's main task suspended meanwhile (as SubScreenExit's).
void swapTask(int)
{
    cItem* p = gSwapItem;
    gSwapItem = 0;
    TaskSuspend(0);
    cPlayer* pl = pPL;
    if (pl && p && (p->flags & 1) && ItemMgr.arm(p)) {
        const int no = WeaponId2WeaponNo(ItemMgr.weaponId());
        const int type = WeaponId2WeaponType(ItemMgr.weaponId());
        const int lv = ItemMgr.weapon() ? ItemMgr.weapon()->getBulletType() : 0;
        if (pG->weapon_no != no || pG->weapon_type != type || pG->bullet_type != lv) {
            SndBlkStop(2);
            pl->weaponRelease();
            pl->weaponLoad(no, type);
            pG->bullet_type = lv;
            pl->weaponInit();
        }
        re4_log("quick slot: item 0x%x equipped (weapon %d, type %d)", (unsigned) p->id, no, type);
    }
    TaskSignal(0);
    TaskExit();
}

}  // namespace

extern "C" {

// Patch 0219's hook (re4_player_input, cPlayer::keyConfig, before the routines read the keys).
void re4_quick_slots_input(cPlayer* pl)
{
    if (!pl || !pSys || !re4_quick_slots_on() || pG->pl_type == 1 || !field(pl)) {
        gDpadPrev = Joy[0].on & 0xF;
        return;
    }
    const u32 dpad = Joy[0].on & 0xF;  // JOY_LEFT 1, JOY_RIGHT 2, JOY_DOWN 4, JOY_UP 8
    const u32 pressed = dpad & ~gDpadPrev;
    gDpadPrev = dpad;
    // the D-pad's share of Key's directions out (the stick's own bits through the same table, as PadRead)
    u64 stick = 0;
    for (int i = 0; i < 4; i++) {
        if (Joy[0].on & Key_type_tbl[pSys->pad_type][i] & ~0xFu) {
            stick |= (u64) 1 << i;
        }
    }
    if ((stick & 3) == 3) {
        stick &= ~(u64) 3;
    }
    if ((stick & 0xC) == 0xC) {
        stick &= ~(u64) 0xC;
    }
    Key.on = (Key.on & ~(u64) 0xF) | stick;
    Key.trg &= ~(u64) 0xF | stick;
    Joy[0].on &= ~0xFu;
    Joy[0].trg &= ~0xFu;
    int slot = -1;
    if (pressed & 8) {
        slot = 0;
    } else if (pressed & 2) {
        slot = 1;
    } else if (pressed & 4) {
        slot = 2;
    } else if (pressed & 1) {
        slot = 3;
    }
    if (slot < 0) {
        return;
    }
    cItem* p = pick(slot);
    if (!p) {
        return;
    }
    if (TaskStatus(1) != 0) {
        re4_log("quick slot %s: busy (task slot 1 in use)", kSlots[slot].name);
        return;
    }
    gSwapItem = p;
    if (!TaskExec(1, (TaskFunc) swapTask, 0)) {
        gSwapItem = 0;
    }
}

// For the runtime test: the item a press of slot 0..3 would equip (NULL: nothing to change).
cItem* re4_quick_slot_pick(int slot)
{
    return slot >= 0 && slot < 4 ? pick(slot) : 0;
}

}  // extern "C"
