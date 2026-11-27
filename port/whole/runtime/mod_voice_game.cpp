// The game's side of the cutscenes' voice mod (runtime/mod_voice.cpp, the platform's part): patch 0958 (snd.cpp
// SndStrReq) starts a line through here, and here the platform's part learns how the game's stream stands each AI
// block: its stream work there (Snd_str_work, the same sound id), sounding (status 0x10), and whether an event (a
// cutscene, STA_EVENT) still runs and whether the player skips it (STA_EVENT_CANCEL).
#include "types.h"
#include "global.h"
#include "re4_runtime.h"
#include "snd_drv.h"

extern "C" void re4_mod_voice_set_state(int (*state)(unsigned id));
extern "C" int re4_mod_voice_prepare(unsigned id, int blk, int no, float start);

namespace {

int state(unsigned id)
{
    int s = 0;
    for (int i = 0; i < SND_STR_MAX; i++) {
        const SND_STR* str = &Snd_str_work[i];
        if (str->be_flag != 0 && str->snd_id == id) {
            s |= 1;
            if (str->be_flag & 0x10) {
                s |= 2;
            }
            break;
        }
    }
    if (pG && StaFlagChk(pG, STA_EVENT)) {
        s |= 4;
    }
    if (pG && StaFlagChk(pG, STA_EVENT_CANCEL)) {
        s |= 8;  // the player skips the scene (START)
    }
    return s;
}

}  // namespace

extern "C" int re4_mod_voice_prepare_game(unsigned id, int blk, int no, float start)
{
    re4_mod_voice_set_state(state);
    return re4_mod_voice_prepare(id, blk, no, start);
}
