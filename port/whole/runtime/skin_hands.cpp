// A whole-body skin's own hands (patch 0937, pl_leon.cpp cPlLeon::setRightHand; wish of DevJunior, 19.11.2026: the
// GTA skin over Leon showed Leon's hand over its glove when he held a weapon). Compiled with the game's flags into
// libre4_main.
//
// The game draws the weapon's grip hand (type 1: Body->pWepHand, from the weapon's own file) in place of Leon's right
// hand. A skin made by CubeEvil4 Studio from another game (cpp/studio_dff.inc) has its hands in its body and folds
// Leon's hand models into one point, marking them with 0xF0 in byte 0x29 (cModelData::pad_29, 0 on every disc model;
// the head's blink factor of runtime/blink.cpp, 5..40, lies elsewhere). With the player's file a mod's, Leon the
// player and his own right hand (archive 0x12) so marked, the grip hand is that folded one: nothing drawn. The
// cutscenes' parts of Leon (runtime/dvd.cpp re4_mod_event_head: the event's hands, jacket, head) go the same way.
#include "types.h"
#include "global.h"

extern "C" int re4_player_modded(void);                        // runtime/dvd.cpp
extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_set_whole_body_check(int (*check)(void));  // runtime/dvd.cpp

// 1 while the player's file is such a whole-body skin (Leon the player, his own right hand folded and marked)
extern "C" int re4_skin_whole_body(void)
{
    if (!pG || pG->pl_type != 0 || !re4_player_modded() || !pG->pPlayer) {
        return 0;
    }
    const u8* own = (const u8*) PL_ARC_PTR(pG->pPlayer, 0x12);
    return own && own[0x29] == 0xF0;
}

// (Mods, 25.11.2026, DevJunior: "Leon still has his hand bugged", Leon Remake Lite holding the handgun: a pale band at
// the right wrist) the weapon's grip hand is the disc's hand from the weapon's own file, drawn with the player's hand
// palette (pl_leon.cpp setRightHand: PL_ARC 0x11): with a skin's palette its texture coordinates hit the skin's glove
// and arm pictures wrongly. With the player's file a mod's, the skin's own closed right hand (archive 0x13, the grip of
// the disc's Leon as well) holds the weapon instead.
extern "C" void* re4_skin_hand(void* wep)
{
    if (re4_skin_whole_body()) {
        return PL_ARC_PTR(pG->pPlayer, 0x12);
    }
    if (wep && pG && pG->pl_type == 0 && re4_player_modded() && pG->pPlayer && pG->pPlayer->ofs[0x13]) {
        static bool said;
        if (!said) {
            said = true;
            re4_log("skin: the weapon's grip hand is the skin's own closed right hand (archive 0x13)");
        }
        return PL_ARC_PTR(pG->pPlayer, 0x13);
    }
    return wep;
}

namespace {
// the platform's event parts (runtime/dvd.cpp re4_mod_event_head) ask this
struct Register {
    Register() { re4_set_whole_body_check(re4_skin_whole_body); }
} gRegister;
}  // namespace
