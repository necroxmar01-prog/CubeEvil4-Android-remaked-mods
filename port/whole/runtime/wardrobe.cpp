// Leon's outfit (wish of DevJunior, 09.11.2026, the remake mods). Compiled with the game's flags into libre4_main
// (re4_gamehooks_obj); the app sends it with the aim bits (runtime/aim_options.cpp: bits 22..23).
//
// The story's rule (pl_sub.cpp PlSetCostume: the jacket until the r106 event, then the second outfit, the armour
// with its item), or always the first (pl00, the jacket) or the second (pl08, without it). Patch 0902 lets
// PlSetCostume ask re4_leon_outfit; a change while playing restarts the room where Leon stands, as the cheat menu's
// character does (runtime/cheats.cpp), and the room's start reads his archive anew (re4_wardrobe_room). The special
// costume (game_costume 1) and the other characters keep theirs.
//
// The weapon on the back (the last long weapon Leon held, an event body on his chest) was removed again on
// DevJunior's wish (09.11.2026, after several device tests: it did not lie flat on the back for every weapon).
#include "types.h"
#include "global.h"
#include "item.h"
#include "main.h"
#include "player.h"
#include "sce_at.h"

#include <atomic>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
int PlSetCostume();                                            // pl_sub.cpp

namespace {

std::atomic<int> gOutfit(0);     // 0 the story's rule, 1 the first outfit, 2 the second
std::atomic<bool> gHooked(false);

int gTried = -1;          // the outfit a room restart was asked for (once per choice)
bool gForced = false;     // an outfit of the app's was put on (back to the story's rule once it is chosen again)

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
}

}  // namespace

// The app's choice (aim bits 22..23, runtime/aim_options.cpp). Any thread.
extern "C" void re4_wardrobe_set(int outfit)
{
    outfit = outfit < 0 || outfit > 2 ? 0 : outfit;
    const int before = gOutfit.exchange(outfit);
    if (before != outfit) {
        re4_log("wardrobe: Leon's outfit %s",
                outfit == 1 ? "the first (jacket)" : outfit == 2 ? "the second (no jacket)" : "the story's");
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
// wants (PlSetCostume asks re4_leon_outfit and marks the archive to be read)
extern "C" void re4_wardrobe_room(void)
{
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
