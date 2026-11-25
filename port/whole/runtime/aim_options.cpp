// The app menu's aim settings for touch and controllers (wish of the project owner, 30.10.2026: the
// Controls page, "Invert vertical" and "Aim speed" beside each other, for aiming the knife and the guns
// with a stick, the touch controls' or a controller's, not with the gyro). Compiled with the game's
// flags into libre4_main; the app sends both at its start and at every change (re4_core_set_aim).
//
//  - "Invert vertical" is the game's own option (its Options, Controller: "reverse", CFG_AIM_REVERSE in
//    pSys->Config_flg; PlWepLockCtrl, the scope, the binoculars and the harpoon read it). The app's switch
//    sets it at the next game frame (the game thread, a frame hook) and again after the system file was
//    read (card.cpp through runtime/game_language.cpp re4_game_language_after_card): the app's choice wins
//    over the saved one, as the language does. A change in the game's own options holds until then.
//  - "Aim speed" scales the stick's turn while aiming (patch 0151: PlWepLockCtrl and CameraScope::move),
//    0.5 .. 3, 1 as the game.
//  - "Auto aim" (wish of the project owner, 30.10.2026) is the game's own lock-on, CFG_LOCK_ON in
//    pSys->Config_flg, until now only in the debug menu's config page (AUTO_LOCK_ON): at aim start the
//    gun turns to the best enemy in front (cPlWep::lockInit, SearchLockEm, PlSetLockPitch) and follows it
//    for 10 frames (lockMove, PlWepAutoTrack); set as "Invert vertical" is.
//  - "Always run" (wish of the project owner, 30.10.2026): Leon runs without the run key and walks while
//    it is held (patch 0156: the four run checks of player.cpp ask re4_run_held). Only there: the key
//    keeps its other uses (cancel, quick turn, menus).
//  - "Knife aim speed" (wish of DevJunior, 11.11.2026: "the knife's sensitivity apart from the gun's"): the
//    "Aim speed" while the knife is ready (its routine, or the knife as the weapon in hand), from a third aim
//    word (bits 31 and 30 set, bits 0..11 percent, 0 as the "Aim speed"). The gyro and the swipes do not aim
//    the knife (runtime/gyro.cpp), so the sticks' turn is all it changes.
//  - "Reload without aiming" (1.1.0, keyboard; the idea from a user's "ModernTPS" mod, our own code): the app asks for
//    one reload when the keyboard's reload key is pressed without the aim (re4_core_quick_reload); patch 0241 lets
//    cPlayer::keyReload answer yes once, within 15 game frames. The game itself then reloads through its own way
//    (actionSelect while standing or walking, setRno 6/4; without the aim key PlReloadDirect 1, the direct reload of
//    its control type 1), only when the weapon can reload (reloadable), never in a scene or another routine.
//  - "Vibration" (1.1.0, a user's report 19.11.2026: no rumble on a DualSense nor the phone): the game's own
//    switch (its Options, Controller: "vibration", CFG_VIBRATION in pSys->Config_flg) follows the app's menu:
//    on unless the app's "Vibration" is Off. The game's switch was off in most testers' system files (the
//    prototype's new system file has it off, and its Options apply a change only with A), so the game never
//    started the motor and the app had nothing to pass on. Set as "Invert vertical" is.
#include "types.h"
#include "global.h"
#include "main.h"
#include "player.h"
#include "pl_sub.h"

extern cPlayer* pPL;

#include <atomic>
#include <cstdlib>
#include <string.h>
#include <time.h>

extern "C" void re4_log(const char* fmt, ...);
extern "C" void re4_add_frame_hook(void (*hook)(unsigned));  // runtime/pad.cpp
extern "C" unsigned re4_pad_frame(void);                      // runtime/pad.cpp
extern "C" void re4_health_bars_set(int on);                 // runtime/health_bars.cpp (1.1.0)
extern "C" void re4_stack_items_set(int step);               // runtime/cheats.cpp (1.1.0, patch 0254)

namespace {

std::atomic<int> gInvert(-1);       // the app's "Invert vertical", -1 before it sent anything
std::atomic<int> gLock(-1);         // the app's "Auto aim", -1 before it sent anything
std::atomic<int> gVib(-1);          // the app's "Vibration" not Off, -1 before it sent anything (third word bits 17/18)
std::atomic<bool> gRun(false);      // the app's "Always run"
std::atomic<bool> gFar(false);      // the app's "Far camera" (patch 0171)
std::atomic<int> gFov(0);           // the app's "Field of view" in degrees, -15 .. 15 (patch 0205)
std::atomic<bool> gFreeMove(false); // the app's "Free movement" (patch 0900, runtime/free_move.cpp)
std::atomic<bool> gAimMove(false);  // the app's "Move while aiming" (patch 0900, runtime/free_move.cpp)
std::atomic<bool> gQuickSlots(false); // the app's "D-pad quick slots" (runtime/quick_slots.cpp)
std::atomic<int> gRecenterBit(-1);
std::atomic<int> gReloadBit(-1);    // bit 20 as last sent; each change is one press of the touch RLOAD button
std::atomic<bool> gSkipIntro(false); // the app's "Skip intro logos" (the second word's bit 28, patch 0905)
std::atomic<bool> gParry(false);    // the app's "Knife parry" (bit 21, runtime/parry.cpp)  // bit 6 as last sent (-1: nothing yet); each change is one press of R3

// The field of view from the aim bits: bits 24..29 hold degrees + 32 (0: an app without it, as the game).
int fovOf(int bits)
{
    const int field = (bits >> 24) & 63;
    if (field == 0) {
        return 0;
    }
    const int fov = field - 32;
    return fov < -15 ? -15 : fov > 15 ? 15 : fov;
}
std::atomic<int> gSpeed(100);       // the app's "Aim speed" in percent
std::atomic<int> gKnifeSpeed(0);    // the app's "Knife aim speed" in percent, 0 as the "Aim speed"
std::atomic<bool> gPending(false);  // the game's option to be set at the next game frame
std::atomic<bool> gHooked(false);
std::atomic<int> gFromGame(-1);     // the game's own options changed "reverse" (patch 0188): 0 / 1 for the app
std::atomic<long> gReloadAt{-1};    // when the app asked for a reload (re4_core_quick_reload), ms; -1 none waiting
unsigned gReloadLines;

long nowMs()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
std::atomic<int> gVibFromGame(-1);  // the game's own options changed "vibration" (1.1.0): 0 / 1 for the app

void applyLock(const char* why)
{
    const int lock = gLock.load();
    if (lock < 0 || !pSys) {
        return;
    }
    if ((CfgFlagChk(pSys, CFG_LOCK_ON) != 0) == (lock != 0)) {
        return;
    }
    if (lock) {
        CfgFlagOn(pSys, CFG_LOCK_ON);
    } else {
        CfgFlagOff(pSys, CFG_LOCK_ON);
    }
    re4_log("aim: auto aim (the game's lock-on) %s (%s)", lock ? "on" : "off", why);
}

void applyVibration(const char* why)
{
    const int vib = gVib.load();
    if (vib < 0 || !pSys) {
        return;
    }
    if ((CfgFlagChk(pSys, CFG_VIBRATION) != 0) == (vib != 0)) {
        return;
    }
    if (vib) {
        CfgFlagOn(pSys, CFG_VIBRATION);
    } else {
        CfgFlagOff(pSys, CFG_VIBRATION);
    }
    re4_log("pad: the game's vibration %s (%s)", vib ? "on" : "off", why);
}

void apply(const char* why)
{
    applyLock(why);
    applyVibration(why);
    const int invert = gInvert.load();
    if (invert < 0 || !pSys) {
        return;
    }
    const bool now = CfgFlagChk(pSys, CFG_AIM_REVERSE) != 0;
    if (now == (invert != 0)) {
        return;
    }
    if (invert) {
        CfgFlagOn(pSys, CFG_AIM_REVERSE);
    } else {
        CfgFlagOff(pSys, CFG_AIM_REVERSE);
    }
    re4_log("aim: vertical aiming %s (%s)", invert ? "inverted" : "normal", why);
}

void hook(unsigned)
{
    if (gPending.exchange(false)) {
        apply("the app's switch");
    }
}

// For tests on the PC: RE4_TEST_AIM=bits at the start, as the app's re4_core_set_aim (bit 1 auto aim, bit 2
// always run, bit 5 vibration sent with bit 4 on, bits 8.. the speed; 0 there: 100 %), applied at the first game
// frame
struct StartSetting {
    StartSetting()
    {
        if (const char* v = getenv("RE4_TEST_AIM")) {
            const int bits = (int) strtol(v, 0, 0);
            const int speed = (bits >> 8) & 0xFFF;
            gSpeed = speed >= 50 && speed <= 300 ? speed : 100;
            gFov = fovOf(bits);
            gInvert = bits & 1;
            gLock = (bits >> 1) & 1;
            gRun = (bits & 4) != 0;
            gFar = (bits & 8) != 0;
            gFreeMove = (bits & 0x10) != 0;
            gAimMove = (bits & 0x20) != 0;
            gQuickSlots = (bits & 0x80) != 0;
            gHooked = true;
            gPending = true;
            re4_add_frame_hook(hook);
        }
    }
} gStartSetting;

// For tests on the PC: RE4_TEST_QUICK_RELOAD="F[,F...]": at PADRead frame F the app's reload without aiming
unsigned gQuickTest[8];
int gQuickTests;

void quickHook(unsigned frame)
{
    for (int i = 0; i < gQuickTests; i++) {
        if (gQuickTest[i] == frame) {
            re4_log("aim: RE4_TEST_QUICK_RELOAD at frame %u", frame);
            gReloadAt.store(nowMs());
        }
    }
}

struct QuickTest {
    QuickTest()
    {
        const char* v = getenv("RE4_TEST_QUICK_RELOAD");
        for (const char* p = v; p && *p && gQuickTests < 8;) {
            gQuickTest[gQuickTests++] = (unsigned) strtoul(p, 0, 10);
            p = strchr(p, ',');
            p = p ? p + 1 : 0;
        }
        if (gQuickTests) {
            re4_add_frame_hook(quickHook);
        }
    }
} gQuickTestSetting;

}  // namespace

extern "C" {

// The app (core_api.cpp re4_core_set_aim): bit 0 "Invert vertical", bit 1 "Auto aim", bit 2 "Always run",
// bit 3 "Far camera", bit 4 "Free movement", bit 5 "Move while aiming" (patch 0900), bit 6 toggled by each press of
// R3 (the camera back behind Leon, patch 0900), bit 7 "D-pad quick slots" (runtime/quick_slots.cpp), bit 20 toggled by each press of the touch RLOAD button (reload without aiming, runtime/free_move.cpp), bit 21 "Knife parry" (runtime/parry.cpp), bits 8..19 the aim speed in percent, bits 24..29 "Field of view"
// (degrees + 32; 0.98.4), bits 22..23 "Leon's outfit" and bit 30 "Weapon on the back" (runtime/wardrobe.cpp). A word
// with bit 31 set is the second one: the weapon on the back's placement (re4_back_adjust_set). Any thread.
extern "C" void re4_camera_recenter(void);  // runtime/free_move.cpp
extern "C" void re4_reload_request(void);   // runtime/free_move.cpp
extern "C" void re4_wardrobe_set(int outfit, int back);  // runtime/wardrobe.cpp
extern "C" void re4_melee_set(int bits);                  // runtime/melee.cpp
extern "C" void re4_box_strike_set(int on);               // runtime/melee.cpp
extern "C" void re4_finish_set(int on);                   // runtime/finisher.cpp
extern "C" void re4_move_set(int on);                     // runtime/locomotion.cpp
extern "C" void re4_react_set(int on);                    // runtime/react.cpp
extern "C" int re4_move_sprint(int held);                 // runtime/locomotion.cpp
extern "C" int re4_codec_walk_only(void);                  // runtime/codec_call.cpp
extern "C" void re4_move_jog_press(void);                 // runtime/locomotion.cpp
extern "C" void re4_back_adjust_set(int word);           // runtime/wardrobe.cpp
extern "C" void re4_ashley_gun_set(int on);             // runtime/ashley_gun.cpp
// Patch 0905 (title.cpp titleWait): 1 = the title starts at its main menu (the app's "Skip intro logos"; on the PC
// RE4_SKIP_INTRO=1).
int re4_skip_intro(void)
{
    static const bool env = getenv("RE4_SKIP_INTRO") != 0 && atoi(getenv("RE4_SKIP_INTRO")) != 0;
    return env || gSkipIntro.load();
}

void re4_aim_set(int bits)
{
    if (bits < 0 && (bits & 0x40000000)) {  // bits 31 and 30: the third word, the knife's aim speed (bits 0..11)
        re4_box_strike_set((bits >> 12) & 1);  // bit 12 "Box strike" (patch 0916, runtime/melee.cpp)
        re4_finish_set((bits >> 13) & 1);      // bit 13 "Finishers" (patch 0921, runtime/finisher.cpp)
        re4_move_set((bits >> 14) & 1);        // bit 14 "Remake movement" (patch 0923, runtime/locomotion.cpp)
        re4_react_set((bits >> 15) & 1);       // bit 15 "Remake reactions" (patch 0925, runtime/react.cpp)
        // bit 16 toggled by each press of L3, the app's "Jog" (runtime/locomotion.cpp)
        static std::atomic<int> jogBit(-1);
        const int jog = (bits >> 16) & 1;
        const int jogBefore = jogBit.exchange(jog);
        if (jogBefore >= 0 && jogBefore != jog) {
            re4_move_jog_press();
        }
        // (the mods' fork: upstream's 1.1.0 bits of the first word live here, the first word is full with the mods')
        // bits 17/18 the game's vibration (bit 18 clear: the game's own switch), bit 19 "Enemy health bars", bits 20..21
        // the cheat "Stack items"
        const int vib = (bits & (1 << 18)) ? (bits >> 17) & 1 : -1;
        if (gVib.exchange(vib) != vib) {
            re4_log("pad: the app's vibration %s", vib < 0 ? "not sent (the game's own switch)" : vib ? "on" : "off");
            if (!gHooked.exchange(true)) {
                re4_add_frame_hook(hook);
            }
            gPending.store(true);  // the hook sets the game's switch
        }
        re4_health_bars_set((bits >> 19) & 1);
        re4_stack_items_set((bits >> 20) & 3);
        int knife = bits & 0xFFF;
        knife = knife == 0 ? 0 : knife < 50 ? 50 : knife > 300 ? 300 : knife;
        if (gKnifeSpeed.exchange(knife) != knife) {
            re4_log("aim: knife aim speed %d %% (0: as the aim speed)", knife);
        }
        return;
    }
    if (bits < 0) {  // bit 31: the second word
        re4_back_adjust_set(bits & 0x00FFFFFF);
        re4_melee_set((bits >> 24) & 15);  // bit 24 "Neck break", bit 25 the mods' melee motions off, bit 26 "Axe strike",
                                           // bit 27 the random mix off
        re4_ashley_gun_set((bits >> 29) & 1);  // bit 29 "Ashley's pistol" (patch 0911)
        const bool skip = (bits >> 28) & 1;  // bit 28 "Skip intro logos" (patch 0905)
        if (gSkipIntro.exchange(skip) != skip) {
            re4_log("title: intro logos %s", skip ? "skipped" : "shown");
        }
        return;
    }
    // bits 22..23 "Leon's outfit" (0 the story's, 1 the first, 2 the second), bit 30 "Weapon on the back"
    re4_wardrobe_set((bits >> 22) & 3, (bits >> 30) & 1);
    const int recenter = (bits >> 6) & 1;
    const int before = gRecenterBit.exchange(recenter);
    if (before >= 0 && before != recenter) {
        re4_camera_recenter();
    }
    const int reload = (bits >> 20) & 1;
    const int reloadBefore = gReloadBit.exchange(reload);
    if (reloadBefore >= 0 && reloadBefore != reload) {
        re4_reload_request();
    }
    const bool parry = (bits & (1 << 21)) != 0;
    if (gParry.exchange(parry) != parry) {
        re4_log("aim: knife parry %s", parry ? "on" : "off");
    }
    int speed = (bits >> 8) & 0xFFF;
    if (speed < 50) speed = 50;
    if (speed > 300) speed = 300;
    const int invert = bits & 1;
    const int lock = (bits >> 1) & 1;
    const bool run = (bits & 4) != 0;
    const bool far = (bits & 8) != 0;
    const bool inverted = gInvert.exchange(invert) != invert;
    const bool faster = gSpeed.exchange(speed) != speed;
    const bool locked = gLock.exchange(lock) != lock;
    const bool ran = gRun.exchange(run) != run;
    const bool farther = gFar.exchange(far) != far;
    const bool freeMove = (bits & 0x10) != 0;
    const bool aimMove = (bits & 0x20) != 0;
    const bool freed = gFreeMove.exchange(freeMove) != freeMove;
    const bool aimMoved = gAimMove.exchange(aimMove) != aimMove;
    const bool quick = (bits & 0x80) != 0;
    const bool quicked = gQuickSlots.exchange(quick) != quick;
    const int fov = fovOf(bits);
    const bool wider = gFov.exchange(fov) != fov;
    if (inverted || faster || locked || ran || farther || wider || freed || aimMoved || quicked) {
        re4_log("aim: stick aiming %s, speed %d %%, auto aim %s, always run %s, far camera %s, field of view %+d degrees, "
                "free movement %s, move while aiming %s, D-pad quick slots %s",
                invert ? "inverted" : "normal", speed, lock ? "on" : "off", run ? "on" : "off", far ? "on" : "off", fov,
                freeMove ? "on" : "off", aimMove ? "on" : "off", quick ? "on" : "off");
    }
    if (!gHooked.exchange(true)) {
        re4_add_frame_hook(hook);
    }
    gPending.store(true);
}

// Patch 0171 (cam_qfps.cpp): the shoulder camera further back while walking and standing.
int re4_far_camera(void)
{
    return gFar.load(std::memory_order_relaxed) ? 1 : 0;
}

// Patch 0900 (runtime/free_move.cpp): "Free movement", the stick moves Leon where it points, seen from a camera the
// C stick turns round him.
int re4_free_move_on(void)
{
    return gFreeMove.load(std::memory_order_relaxed) ? 1 : 0;
}

// Patch 0900 (runtime/free_move.cpp): "Move while aiming", the C stick walks Leon while a weapon is ready.
int re4_aim_move_on(void)
{
    return gAimMove.load(std::memory_order_relaxed) ? 1 : 0;
}

// runtime/parry.cpp: "Knife parry", the knife key at the right moment turns a blow aside.
int re4_parry_on(void)
{
    return gParry.load(std::memory_order_relaxed) ? 1 : 0;
}

// runtime/quick_slots.cpp: "D-pad quick slots", the D-pad equips weapons by their kind.
int re4_quick_slots_on(void)
{
    return gQuickSlots.load(std::memory_order_relaxed) ? 1 : 0;
}

// Patch 0205 (cam_qfps.cpp): degrees added to the shoulder camera's vertical field of view (0: as the game).
f32 re4_fov_offset(void)
{
    return (f32) gFov.load(std::memory_order_relaxed);
}

// Patch 0151: the factor of the stick's turn while aiming (1: as the game); while the knife is ready the "Knife aim
// speed", if the app set one.
f32 re4_aim_speed(void)
{
    const int knife = gKnifeSpeed.load(std::memory_order_relaxed);
    if (knife > 0 && pPL && pPL->r_no_0 == 0 && (pPL->r_no_1 == 0xB || (pPL->r_no_1 == 6 && PlGetWeaponNo() == 0x10))) {
        return (f32) knife / 100.0f;
    }
    return (f32) gSpeed.load(std::memory_order_relaxed) / 100.0f;
}

// Patch 0934 (pl_wep.cpp PlWepLockCtrl, his body's turn at the waist's limit while aiming): the remake's turn
// acceleration (Mods, 19.11.2026, DevJunior: "standing among enemies I have to turn on the spot slowly, it left me
// exposed"): the stick held far to one side (kBoostFrom of its reach) for kBoostWait frames speeds the turn up, to
// kBoostMax times over kBoostRamp frames more; let go, turned the other way or less far: as before.
f32 re4_aim_turn_boost(int stickX)
{
    const int kBoostFrom = 46;  // of the main stick's 56
    const int kBoostWait = 6, kBoostRamp = 12;
    const f32 kBoostMax = 2.5f;
    static unsigned at = ~0u;
    static int frames, side;
    const unsigned now = re4_pad_frame();
    if (now != at) {
        at = now;
        const int s = stickX >= kBoostFrom ? 1 : stickX <= -kBoostFrom ? -1 : 0;
        frames = s != 0 && s == side ? frames + 1 : s != 0 ? 1 : 0;
        side = s;
        if (frames == kBoostWait + kBoostRamp) {
            re4_log("aim: the turn speeds up (stick held to the %s)", s > 0 ? "right" : "left");
        }
    }
    const int over = frames - kBoostWait;
    if (over <= 0) {
        return 1.0f;
    }
    const f32 t = over >= kBoostRamp ? 1.0f : (f32) over / (f32) kBoostRamp;
    return 1.0f + t * (kBoostMax - 1.0f);
}

// Patch 0156 (player.cpp): the run key as Leon's walk and run routines see it; with "Always run" turned
// around (run without it, walk while it is held).
int re4_run_held(int held)
{
    // a remake call (runtime/codec_call.cpp): Leon walks
    if (re4_codec_walk_only()) {
        return 0;
    }
    // the remake's sprint (runtime/locomotion.cpp): the stick jogs, the run key sprints
    const int sprint = re4_move_sprint(held);
    if (sprint >= 0) {
        return sprint;
    }
    return gRun.load(std::memory_order_relaxed) ? !held : held != 0;
}

// Patch 0188 (OptionScreen::quit, game thread): the game's own options set "reverse"; that is the app's
// "Invert vertical" from now on, and the app keeps it (re4_aim_from_game).
void re4_aim_options_closed(void)
{
    if (!pSys || gPending.load()) {
        return;
    }
    // "vibration" too (1.1.0): the app's "Vibration" becomes Off, or Auto when it was Off
    const int vib = CfgFlagChk(pSys, CFG_VIBRATION) != 0;
    const int sent = gVib.load();
    if (sent >= 0 && sent != vib) {
        gVib = vib;
        gVibFromGame = vib;
        re4_log("pad: the game's vibration %s (the game's own options; the app keeps it)", vib ? "on" : "off");
    }
    const int now = CfgFlagChk(pSys, CFG_AIM_REVERSE) != 0;
    if (gInvert.load() == now) {
        return;
    }
    gInvert = now;
    gFromGame = now;
    re4_log("aim: vertical aiming %s (the game's own options; the app keeps it)", now ? "inverted" : "normal");
}

// The app (core_api.cpp re4_core_aim_from_game): -1, or the game's own choice since the last call (0 / 1).
int re4_aim_from_game(void)
{
    return gFromGame.exchange(-1);
}

// The app (core_api.cpp re4_core_vibration_from_game): -1, or the game's own "vibration" since the last call.
int re4_vibration_from_game(void)
{
    return gVibFromGame.exchange(-1);
}

// The app (core_api.cpp re4_core_quick_reload, any thread): the keyboard's reload key without the aim
void re4_quick_reload_request(void)
{
    gReloadAt.store(nowMs());
}

// Patch 0241 (cPlayer::keyReload, game thread): 1 once for a request of the last half second (one pressed during a
// scene or a menu lapses); PlReloadDirect as the game's control type 1 sets it (1 without the aim key: direct)
int re4_quick_reload_take(int aimHeld)
{
    const long at = gReloadAt.load();
    if (at < 0) {
        return 0;
    }
    gReloadAt.store(-1);
    if (nowMs() - at > 500) {
        return 0;
    }
    if (gReloadLines < 30) {
        gReloadLines++;
        re4_log("aim: reload without aiming asked (the keyboard), %s", aimHeld ? "while aiming" : "direct");
    }
    return 1;
}

// After the system file was read into pSys (game thread): the app's switch, not the saved one.
void re4_aim_after_card(void)
{
    apply("the app's switch over the system file");
}

}  // extern "C"
