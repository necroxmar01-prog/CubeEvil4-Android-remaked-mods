// A skin's blink (patch 0919, pl_class.cpp cPlayer::moveEyeNormal; wish of DevJunior, 12.11.2026: "the eyes do not
// blink correctly"). Compiled with the game's flags into libre4_main.
//
// The game blinks by turning the eyelid part (0x1C) about x, at most 0.349 rad: Leon's own lids close with that, a
// skin's head with eyes that open wider does not (its lids weighted to that part by port/tools/skin_blink.py). The
// skin's head carries the factor in its model's byte 0x29 (cModelData::pad_29, after the vertex shift; tenths, 0 on
// every disc model): while the player's file is a mod's and the player is Leon, the angle the step set is scaled by
// it. The last scaled value is remembered: a step of the blink timer that sets nothing leaves the scaled angle, which
// must not be scaled again.
#include "types.h"
#include "global.h"

extern "C" int re4_player_modded(void);  // runtime/dvd.cpp

namespace {

f32 gLastOut = 0.0f;  // the angle this gave last

}  // namespace

extern "C" {

// The skin head's factor (1 = the game's), for the runtime test too.
float re4_blink_scale(void)
{
    if (!pG || pG->pl_type != 0 || !re4_player_modded() || !pG->pPlayer) {
        return 1.0f;
    }
    const u8* head = (const u8*) PL_ARC_PTR(pG->pPlayer, 6);
    const int tenths = head ? head[0x29] : 0;
    return tenths >= 5 && tenths <= 40 ? (f32) tenths / 10.0f : 1.0f;
}

// Patch 0919: the eyelid's angle for this frame.
float re4_blink_angle(float set)
{
    if (set == gLastOut) {
        return set;  // nothing set this frame: already scaled
    }
    gLastOut = set * re4_blink_scale();
    return gLastOut;
}

}  // extern "C"
