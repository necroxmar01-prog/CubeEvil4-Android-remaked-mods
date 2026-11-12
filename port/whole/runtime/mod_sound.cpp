// A mod's sound for the D-pad quick slots (wish of DevJunior, 12.11.2026): mods/<pack>/sound/quickslot.wav
// (found by dvd.cpp modFolder, as the melee archive; the last pack's in the folder's order) plays once when a
// press of the D-pad equips a weapon or puts it away (runtime/quick_slots.cpp). No file of the disc: without the
// file nothing plays and nothing of the game's sound changes.
//
// The WAV: PCM, 16 bit, mono or stereo, any rate from 8 to 96 kHz, at most kMaxSeconds (longer ones are cut).
// It is read once per path (at the first press), resampled (linear) to the AI's 32 kHz stereo and added to the
// blocks the AI starts (ai.cpp play, after the DSP mixed them; the app's ring, not the check's WAV file), scaled
// by the effects volume of the app's AUDIO page (re4_sound_volume, passed by the caller). A new press starts it
// over. The core's sound runs on the game's thread (the AI blocks come with the retraces, vi.cpp); the playing
// position is atomic anyway. A sound replaced by another pack's stays in memory (a few hundred kB at most), so the
// mixer never reads freed memory.
#include "re4_runtime.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const unsigned kRate = 32000;
const unsigned kMaxSeconds = 4;

char gPath[1600];                         // set by dvd.cpp modFolder; "" none
char gLoaded[1600];                       // the path gSound was read from
std::atomic<const int16_t*> gSound(0);    // 32 kHz stereo frames (L, R)
std::atomic<unsigned> gFrames(0);
std::atomic<int> gPos(-1);                // the next frame to mix, -1 idle
std::atomic<int> gGain(0);                // 0 .. 256

unsigned le16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}

unsigned le32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned) p[3] << 24);
}

// The WAV at `path` as 32 kHz stereo; NULL (and a log line) when it is not one this reads.
int16_t* readWav(const char* path, unsigned* outFrames)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        re4_log("sound mod: %s cannot be read", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > 32L * 1024 * 1024) {
        fclose(f);
        re4_log("sound mod: %s is no WAV this reads (%ld bytes)", path, size);
        return 0;
    }
    unsigned char* b = (unsigned char*) malloc((size_t) size);
    const bool ok = b && fread(b, 1, (size_t) size, f) == (size_t) size;
    fclose(f);
    if (!ok || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) {
        free(b);
        re4_log("sound mod: %s is no RIFF WAVE file", path);
        return 0;
    }
    unsigned format = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char* data = 0;
    unsigned dataBytes = 0;
    for (long at = 12; at + 8 <= size;) {
        const unsigned len = le32(b + at + 4);
        const long body = at + 8;
        if (memcmp(b + at, "fmt ", 4) == 0 && len >= 16 && body + 16 <= size) {
            format = le16(b + body);
            channels = le16(b + body + 2);
            rate = le32(b + body + 4);
            bits = le16(b + body + 14);
            if (format == 0xFFFE && len >= 26 && body + 26 <= size) {
                format = le16(b + body + 24);  // WAVE_FORMAT_EXTENSIBLE: the sub format's first word
            }
        } else if (memcmp(b + at, "data", 4) == 0) {
            data = b + body;
            dataBytes = (unsigned) (len <= (unsigned) (size - body) ? len : size - body);
            break;
        }
        at = body + len + (len & 1);
    }
    if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 96000 || !data) {
        free(b);
        re4_log("sound mod: %s: only 16-bit PCM, mono or stereo, 8-96 kHz (format %u, %u bit, %u channels, %u Hz)",
                path, format, bits, channels, rate);
        return 0;
    }
    unsigned in = dataBytes / (2 * channels);
    if (in > rate * kMaxSeconds) {
        re4_log("sound mod: %s is longer than %u s, cut", path, kMaxSeconds);
        in = rate * kMaxSeconds;
    }
    const unsigned out = (unsigned) ((uint64_t) in * kRate / rate);
    if (out == 0) {
        free(b);
        re4_log("sound mod: %s holds no sound", path);
        return 0;
    }
    int16_t* s = (int16_t*) malloc(out * 2 * sizeof(int16_t));
    if (!s) {
        free(b);
        return 0;
    }
    for (unsigned i = 0; i < out; i++) {
        const uint64_t at16 = (uint64_t) i * rate * 65536 / kRate;  // the source position, 1/65536 frames
        const unsigned a = (unsigned) (at16 >> 16);
        const unsigned c = a + 1 < in ? a + 1 : a;
        const int t = (int) (at16 & 0xFFFF);
        for (unsigned ch = 0; ch < 2; ch++) {
            const unsigned k = channels == 2 ? ch : 0;
            const int va = (int16_t) le16(data + (a * channels + k) * 2);
            const int vc = (int16_t) le16(data + (c * channels + k) * 2);
            s[i * 2 + ch] = (int16_t) (va + (int) (((int64_t) (vc - va) * t) >> 16));
        }
    }
    free(b);
    re4_log("sound mod: %s: %u Hz, %u channel(s), %u ms", path, rate, channels, out * 1000 / kRate);
    *outFrames = out;
    return s;
}

int16_t clamp16(int v)
{
    return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

}  // namespace

extern "C" {

// dvd.cpp modFolder / applyMods: the active packs' sound for the quick slots ("" none).
void re4_mod_sound_path_set(const char* path)
{
    snprintf(gPath, sizeof(gPath), "%s", path ? path : "");
}

const char* re4_mod_sound_path(void)
{
    return gPath;
}

// runtime/quick_slots.cpp: a press equipped a weapon or put it away. volume: the effects' percent (0..100).
// 1 when a sound starts.
int re4_mod_sound_play(int volume)
{
    if (!gPath[0] || volume <= 0) {
        return 0;
    }
    if (strcmp(gPath, gLoaded) != 0) {
        snprintf(gLoaded, sizeof(gLoaded), "%s", gPath);
        unsigned frames = 0;
        int16_t* s = readWav(gPath, &frames);
        gPos.store(-1);
        gFrames.store(s ? frames : 0);
        gSound.store(s);  // the one before stays allocated (see above)
    }
    if (!gSound.load() || !gFrames.load()) {
        return 0;
    }
    gGain.store((volume > 100 ? 100 : volume) * 256 / 100);
    gPos.store(0);
    return 1;
}

// ai.cpp play: adds the playing sound to `frames` stereo frames (L, R) for the app's ring.
void re4_mod_sound_mix(short* lr, unsigned frames)
{
    int start = gPos.load(std::memory_order_relaxed);
    int pos = start;
    if (pos < 0) {
        return;
    }
    const int16_t* s = gSound.load();
    const unsigned total = gFrames.load();
    const int gain = gGain.load();
    unsigned i = 0;
    for (; i < frames && (unsigned) pos < total; i++, pos++) {
        lr[i * 2] = clamp16(lr[i * 2] + ((s[pos * 2] * gain) >> 8));
        lr[i * 2 + 1] = clamp16(lr[i * 2 + 1] + ((s[pos * 2 + 1] * gain) >> 8));
    }
    // a new press meanwhile (gPos set to 0 again) wins over this one's progress
    gPos.compare_exchange_strong(start, (unsigned) pos < total ? pos : -1);
}

}  // extern "C"
