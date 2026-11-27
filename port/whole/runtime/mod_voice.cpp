// A mod's voices for the cutscenes (wish of DevJunior, 26.11.2026: the Latin American dub of IG Studios, made for the
// Steam release RE4 UHD, as a mod pack of its own; his choice of the two ways: "la 1", the voices replaced while the game
// plays, the disc's files untouched): mods/<pack>/voice/bio4evt.xwb and bio4evt.xsb, the PC release's XACT wave bank of
// the event voices and its sound bank (found by dvd.cpp modFolder, the last pack's in the folder's order).
//
// The game plays an event's voice as stream `no` of block 1 (bgm/bio4evt.sbb, snd.cpp SndStrReq; patch 0958 tells
// re4_mod_voice_prepare). The sound bank's cue `no` names the wave bank's entry (a simple cue its wave directly, a
// complex one through its play-wave event, with its delay). While it plays, the game's own stream runs on silently
// (patch 0958, snd_str4.cpp: its volume 0 here) so that the event's timing, its waits for the stream and its end stay
// the game's; the dub starts in the AI block in which that stream starts sounding, at the stream's volume (its fades,
// the app's music volume), resampled (linear) to the AI's 32 kHz stereo and added to the blocks (ai.cpp play). It ends
// with its own end, a stop of the game's stream (its fade reaches 0), or when the stream is gone and no event runs
// any more (a skipped cutscene); a dub line longer than the game's goes on while the cutscene lasts.
//
// The wave bank: XACT 3 (version 46), entries of 24 bytes; PCM 8 / 16 bit and MS ADPCM, mono or stereo, any rate. Read
// from the file as it plays (pread, a few kB per block; the bank holds an hour of voices). XMA and xWMA are not read (a
// log line, the game's voice stays). Everything here runs on the game's thread (the stream requests, the sound driver's
// frame and the AI blocks come with the retraces, vi.cpp). This part is the platform's (libre4_platform, as
// mod_music.cpp); runtime/mod_voice_game.cpp asks the game's sound driver.
#include "re4_runtime.h"

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

const unsigned kRate = 32000;  // the AI's

struct Wave {
    uint32_t format;
    uint32_t offset;  // in the file
    uint32_t length;  // bytes
    unsigned tag, channels, rate, align, bits16;
};

struct Cue {
    int wave;       // -1 none
    unsigned delay;  // ms
};

struct Bank {
    char xwb[1600];
    char xsb[1600];
    bool tried;
    bool ok;
    int fd;
    Wave* waves;
    unsigned count;
    Cue* cues;
    unsigned cueCount;
};
Bank gBank;

unsigned le16(const unsigned char* p)
{
    return p[0] | (p[1] << 8);
}

uint32_t le32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

unsigned char* readAll(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = n > 0 ? (unsigned char*) malloc((size_t) n) : 0;
    if (d && fread(d, 1, (size_t) n, f) != (size_t) n) {
        free(d);
        d = 0;
    }
    fclose(f);
    *size = d ? (size_t) n : 0;
    return d;
}

// The sound bank's cues: cue n plays wave cues[n].wave after cues[n].delay ms.
bool readSoundBank(Bank& b)
{
    size_t n = 0;
    unsigned char* d = readAll(b.xsb, &n);
    if (!d || n < 0x8A || memcmp(d, "SDBK", 4) != 0) {
        re4_log("voice mod: %s is not an XACT sound bank", b.xsb);
        free(d);
        return false;
    }
    const unsigned simple = le16(d + 19);
    const unsigned complex = le16(d + 21);
    const uint32_t simpleAt = le32(d + 34);
    const uint32_t complexAt = le32(d + 38);
    b.cueCount = simple + complex;
    b.cues = (Cue*) calloc(b.cueCount ? b.cueCount : 1, sizeof(Cue));
    unsigned found = 0, fromEvents = 0;
    for (unsigned i = 0; i < b.cueCount; i++) {
        b.cues[i].wave = -1;
        uint32_t sound;
        if (i < simple) {
            if (simpleAt + 5 * (i + 1) > n) {
                break;
            }
            sound = le32(d + simpleAt + 5 * i + 1);
        } else {
            // a complex cue: its first variation's sound (15 bytes: flags u8, sound / variation table u32, ...)
            const uint32_t at = complexAt + 15 * (i - simple);
            if (complexAt == 0xFFFFFFFF || at + 15 > n || (d[at] & 0x04)) {
                continue;  // a cue with variations: not in the event bank
            }
            sound = le32(d + at + 1);
        }
        if (sound + 12 > n) {
            continue;
        }
        const unsigned char* s = d + sound;
        const unsigned size = le16(s + 7);
        if ((s[0] & 1) == 0) {
            b.cues[i].wave = (int) le16(s + 9);  // a simple sound: track index, wave bank index
            found++;
            continue;
        }
        // a complex sound: its play-wave event (header: type 1 in bits 0..4, the time in ms in bits 5..20; the wave
        // eight bytes on, then the wave bank 0)
        for (unsigned k = 9; k + 11 <= size && sound + k + 11 <= n; k++) {
            const uint32_t h = le32(s + k);
            const unsigned wave = le16(s + k + 8);
            if ((h & 0x1F) == 1 && s[k + 10] == 0 && wave < 0x8000 && ((h >> 5) & 0xFFFF) < 60000) {
                b.cues[i].wave = (int) wave;
                b.cues[i].delay = (h >> 5) & 0xFFFF;
                found++;
                fromEvents++;
                break;
            }
        }
    }
    free(d);
    re4_log("voice mod: %s: %u cues, %u with a wave (%u through an event)", b.xsb, b.cueCount, found, fromEvents);
    return found > 0;
}

bool readWaveBank(Bank& b)
{
    b.fd = open(b.xwb, O_RDONLY);
    unsigned char h[200];
    if (b.fd < 0 || pread(b.fd, h, sizeof(h), 0) != (ssize_t) sizeof(h) || memcmp(h, "WBND", 4) != 0) {
        re4_log("voice mod: %s is not an XACT wave bank", b.xwb);
        return false;
    }
    const uint32_t version = le32(h + 4);
    const uint32_t bankAt = le32(h + 12), metaAt = le32(h + 20), dataAt = le32(h + 44);
    unsigned char bank[96];
    if (version < 42 || pread(b.fd, bank, sizeof(bank), bankAt) != (ssize_t) sizeof(bank)) {
        re4_log("voice mod: %s: wave bank version %u not read", b.xwb, (unsigned) version);
        return false;
    }
    const uint32_t flags = le32(bank), count = le32(bank + 4), entrySize = le32(bank + 72);
    if ((flags & 0x20000) || entrySize < 24 || count == 0 || count > 65535) {
        re4_log("voice mod: %s: a compact or empty bank (flags %x, %u entries of %u bytes), not read", b.xwb,
                (unsigned) flags, (unsigned) count, (unsigned) entrySize);
        return false;
    }
    unsigned char* meta = (unsigned char*) malloc(count * entrySize);
    if (pread(b.fd, meta, count * entrySize, metaAt) != (ssize_t) (count * entrySize)) {
        free(meta);
        return false;
    }
    b.waves = (Wave*) calloc(count, sizeof(Wave));
    b.count = count;
    unsigned kinds[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < count; i++) {
        const unsigned char* e = meta + i * entrySize;
        Wave& w = b.waves[i];
        w.format = le32(e + 4);
        w.offset = dataAt + le32(e + 8);
        w.length = le32(e + 12);
        w.tag = w.format & 3;
        w.channels = (w.format >> 2) & 7;
        w.rate = (w.format >> 5) & 0x3FFFF;
        w.align = (w.format >> 23) & 0xFF;
        w.bits16 = w.format >> 31;
        kinds[w.tag]++;
    }
    free(meta);
    re4_log("voice mod: %s: %u waves (PCM %u, XMA %u, ADPCM %u, WMA %u), the first %u Hz, %u channel(s)", b.xwb,
            (unsigned) count, kinds[0], kinds[1], kinds[2], kinds[3], b.waves[0].rate, b.waves[0].channels);
    return true;
}

bool bankOpen()
{
    Bank& b = gBank;
    if (b.tried) {
        return b.ok;
    }
    b.tried = true;
    b.fd = -1;
    if (!b.xwb[0] || !b.xsb[0]) {
        if (b.xwb[0] || b.xsb[0]) {
            re4_log("voice mod: bio4evt.xwb and bio4evt.xsb are both needed (voice/ of a mod pack)");
        }
        return false;
    }
    b.ok = readSoundBank(b) && readWaveBank(b);
    return b.ok;
}

void bankClose()
{
    Bank& b = gBank;
    if (b.fd >= 0) {
        close(b.fd);
    }
    free(b.waves);
    free(b.cues);
    char xwb[sizeof(b.xwb)], xsb[sizeof(b.xsb)];
    memcpy(xwb, b.xwb, sizeof(xwb));
    memcpy(xsb, b.xsb, sizeof(xsb));
    memset(&b, 0, sizeof(b));
    memcpy(b.xwb, xwb, sizeof(xwb));
    memcpy(b.xsb, xsb, sizeof(xsb));
    b.fd = -1;
}

// ---- the playing line ------------------------------------------------------------------------------------------------

const unsigned kCache = 4096;  // source frames decoded ahead

struct Line {
    bool on;
    uint32_t sndId;
    const Wave* w;
    unsigned frames;      // the wave's
    double pos;           // in source frames
    unsigned delay;       // AI frames of silence still to go
    bool started;         // the game's stream sounded
    bool streamGone;      // ... and is over
    bool fadingOut;       // the game faded it out (a stop request)
    float gain;           // from the stream's volume
    int16_t cache[kCache * 2];
    unsigned cacheAt, cacheLen;  // source frames
};
Line gLine;

unsigned waveFrames(const Wave& w)
{
    if (w.channels == 0) {
        return 0;
    }
    if (w.tag == 0) {
        return w.length / (w.channels * (w.bits16 ? 2 : 1));
    }
    const unsigned block = (w.align + 22) * w.channels;
    const unsigned perBlock = (w.align + 22 - 7) * 2 + 2;
    return w.length / block * perBlock;
}

const int kCoef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

int16_t clamp16(int v)
{
    return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

// MS ADPCM: one block of `ch` channels into out (frames interleaved, stereo kept as L, R; mono doubled later)
unsigned adpcmBlock(const unsigned char* p, unsigned size, unsigned ch, int16_t* out)
{
    int pred[2], delta[2], s1[2], s2[2];
    for (unsigned c = 0; c < ch; c++) {
        pred[c] = p[c] < 7 ? p[c] : 0;
        delta[c] = (int16_t) le16(p + ch + 2 * c);
        s1[c] = (int16_t) le16(p + 3 * ch + 2 * c);
        s2[c] = (int16_t) le16(p + 5 * ch + 2 * c);
    }
    unsigned n = 0;
    for (unsigned c = 0; c < ch; c++) {
        out[c] = (int16_t) s2[c];
        out[ch + c] = (int16_t) s1[c];
    }
    n = 2;
    unsigned nib = 0;
    const unsigned char* q = p + 7 * ch;
    const unsigned total = (size - 7 * ch) * 2;
    while (nib < total) {
        for (unsigned c = 0; c < ch && nib < total; c++, nib++) {
            const unsigned char byte = q[nib >> 1];
            int code = (nib & 1) ? (byte & 0xF) : (byte >> 4);
            const int signedCode = code >= 8 ? code - 16 : code;
            int sample = (s1[c] * kCoef[pred[c]][0] + s2[c] * kCoef[pred[c]][1]) / 256 + signedCode * delta[c];
            sample = clamp16(sample);
            s2[c] = s1[c];
            s1[c] = sample;
            delta[c] = delta[c] * kAdapt[code] / 256;
            if (delta[c] < 16) {
                delta[c] = 16;
            }
            out[n * ch + c] = (int16_t) sample;
        }
        n++;
    }
    return n;
}

// the cache holds source frame `at` on (stereo L, R)
bool fill(Line& l, unsigned at)
{
    const Wave& w = *l.w;
    const unsigned ch = w.channels;
    l.cacheLen = 0;
    if (w.tag == 0) {
        const unsigned bytes = w.bits16 ? 2 : 1;
        unsigned take = l.frames - at < kCache ? l.frames - at : kCache;
        static unsigned char raw[kCache * 2 * 8];
        const ssize_t got = pread(gBank.fd, raw, take * ch * bytes, (off_t) w.offset + (off_t) at * ch * bytes);
        if (got <= 0) {
            return false;
        }
        take = (unsigned) got / (ch * bytes);
        for (unsigned i = 0; i < take; i++) {
            for (unsigned c = 0; c < 2; c++) {
                const unsigned k = (i * ch + (c < ch ? c : 0)) * bytes;
                l.cache[i * 2 + c] = bytes == 2 ? (int16_t) le16(raw + k) : (int16_t) ((raw[k] - 128) << 8);
            }
        }
        l.cacheAt = at;
        l.cacheLen = take;
        return take > 0;
    }
    if (w.tag == 2) {
        const unsigned block = (w.align + 22) * ch;
        const unsigned perBlock = (w.align + 22 - 7) * 2 + 2;
        unsigned b = at / perBlock;
        static unsigned char raw[256 * 8];
        static int16_t pcm[512 * 8];
        if (block > sizeof(raw) || perBlock * ch > sizeof(pcm) / sizeof(pcm[0])) {
            return false;
        }
        l.cacheAt = b * perBlock;
        while (l.cacheLen + perBlock <= kCache && (b + 1) * block <= w.length) {
            if (pread(gBank.fd, raw, block, (off_t) w.offset + (off_t) b * block) != (ssize_t) block) {
                break;
            }
            const unsigned got = adpcmBlock(raw, block, ch, pcm);
            for (unsigned i = 0; i < got; i++) {
                l.cache[(l.cacheLen + i) * 2] = pcm[i * ch];
                l.cache[(l.cacheLen + i) * 2 + 1] = pcm[i * ch + (ch > 1 ? 1 : 0)];
            }
            l.cacheLen += got;
            b++;
        }
        return l.cacheLen > 0 && at < l.cacheAt + l.cacheLen;
    }
    return false;
}

bool frameAt(Line& l, unsigned at, int* left, int* right)
{
    if (at >= l.frames) {
        return false;
    }
    if (at < l.cacheAt || at >= l.cacheAt + l.cacheLen) {
        if (!fill(l, at)) {
            return false;
        }
    }
    const unsigned k = at - l.cacheAt;
    *left = l.cache[k * 2];
    *right = l.cache[k * 2 + 1];
    return true;
}

void stop(const char* why)
{
    if (gLine.on) {
        re4_log("voice mod: line of stream %u ends (%s) at %.1f of %.1f s", (unsigned) gLine.sndId, why,
                gLine.pos / gLine.w->rate, (double) gLine.frames / gLine.w->rate);
    }
    gLine.on = false;
}

// the game's side (runtime/mod_voice_game.cpp, set at the first line): bit 0 the line's stream is there, bit 1 it
// sounds, bit 2 an event runs; NULL (tests): a stream that sounds
int (*gState)(unsigned id);

}  // namespace

extern "C" {

// runtime/mod_voice_game.cpp: how the game's streams stand (see gState)
void re4_mod_voice_set_state(int (*state)(unsigned id))
{
    gState = state;
}

// dvd.cpp applyMods: before the packs are read
void re4_mod_voice_reset(void)
{
    stop("the mods changed");
    bankClose();
    gBank.xwb[0] = gBank.xsb[0] = 0;
}

// dvd.cpp modFolder: a pack's voice/<name>; a later pack's replaces it
void re4_mod_voice_add(const char* name, const char* path)
{
    if (strcasecmp(name, "bio4evt.xwb") == 0) {
        snprintf(gBank.xwb, sizeof(gBank.xwb), "%s", path);
    } else if (strcasecmp(name, "bio4evt.xsb") == 0) {
        snprintf(gBank.xsb, sizeof(gBank.xsb), "%s", path);
    } else {
        re4_log("voice mod: %s is not read (only bio4evt.xwb and bio4evt.xsb for now)", path);
        return;
    }
    gBank.tried = false;
}

// Patch 0958 (snd.cpp SndStrReq, through runtime/mod_voice_game.cpp): the game starts stream `no` of block `blk` as `id`, `start` seconds in. 1: the
// mod's line replaces it.
int re4_mod_voice_prepare(unsigned id, int blk, int no, float start)
{
    if (blk != 1 || id == 0 || !bankOpen()) {
        return 0;
    }
    if (no < 0 || (unsigned) no >= gBank.cueCount || gBank.cues[no].wave < 0 ||
        (unsigned) gBank.cues[no].wave >= gBank.count) {
        re4_log("voice mod: event voice %d has no cue in the bank, the game's own", no);
        return 0;
    }
    const Cue& c = gBank.cues[no];
    const Wave& w = gBank.waves[c.wave];
    if ((w.tag != 0 && w.tag != 2) || w.rate < 4000 || w.channels == 0 || w.channels > 2) {
        static unsigned said;
        if (said++ < 8) {
            re4_log("voice mod: event voice %d: wave %d's format (tag %u, %u Hz, %u ch) is not read, the game's own",
                    no, c.wave, w.tag, w.rate, w.channels);
        }
        return 0;
    }
    stop("a new line");
    Line& l = gLine;
    l.on = true;
    l.sndId = id;
    l.w = &w;
    l.frames = waveFrames(w);
    const double skip = start * 1000.0 - c.delay;  // a line started part way (a cutscene's debug start)
    l.pos = skip > 0 ? skip / 1000.0 * w.rate : 0.0;
    l.delay = skip < 0 ? (unsigned) (-skip / 1000.0 * kRate) : 0;
    l.started = false;
    l.streamGone = false;
    l.fadingOut = false;
    l.gain = 0.0f;
    l.cacheAt = l.cacheLen = 0;
    re4_log("voice mod: event voice %d (stream %u) -> wave %d, %.1f s at %u Hz%s", no, id, c.wave,
            (double) l.frames / w.rate, w.rate, c.delay ? " after a delay" : "");
    return 1;
}

// Patch 0958 (snd_str4.cpp Snd_str_work_calc_ax_vol): the stream's AX volume (MIX dB * 10, after the game's and the app's
// volumes), its volume now (8.8) and its fade (fading: its target, 8.8); the replaced stream's is kept for the line and
// the stream itself made silent. The mute of a stream that ends (Snd_str_ax_voice_stop: volume 0 without a fade) leaves
// the line's volume as it was: a dub line longer than the game's goes on; a fade to 0 (a stop request) ends it.
int re4_mod_voice_stream_vol(unsigned id, int axVol, int nowVol, int fading, int fadeEnd)
{
    if (!gLine.on || id != gLine.sndId) {
        return axVol;
    }
    if (fading && fadeEnd == 0) {
        gLine.fadingOut = true;
    }
    if (nowVol != 0 || fading) {
        gLine.gain = axVol <= -904 ? 0.0f : powf(10.0f, (float) axVol / 200.0f);
    }
    return -904;
}

// ai.cpp play: on top of the game's block, 32 kHz stereo (L, R); lr NULL: the block was lost (the app does not read),
// the line goes on in step
void re4_mod_voice_mix(short* lr, unsigned frames)
{
    Line& l = gLine;
    if (!l.on) {
        return;
    }
    const int st = gState ? gState(l.sndId) : 3;
    if (!l.started) {
        if (!(st & 1)) {
            stop("the game's stream did not start");
            return;
        }
        if (!(st & 2)) {
            return;  // still buffering
        }
        l.started = true;
    }
    if (!(st & 1)) {
        l.streamGone = true;
    }
    if (l.streamGone && !(st & 4)) {
        stop("the cutscene is over");
        return;
    }
    if (l.streamGone && (l.fadingOut || l.gain <= 0.0f)) {
        stop("the game's stream was stopped");
        return;
    }
    const double step = (double) l.w->rate / kRate;
    const int g = (int) (l.gain * 256.0f + 0.5f);
    for (unsigned i = 0; i < frames; i++) {
        if (l.delay) {
            l.delay--;
            continue;
        }
        const unsigned at = (unsigned) l.pos;
        int a0, b0, a1, b1;
        if (!frameAt(l, at, &a0, &b0)) {
            stop("its end");
            return;
        }
        if (!frameAt(l, at + 1, &a1, &b1)) {
            a1 = a0;
            b1 = b0;
        }
        const double f = l.pos - at;
        if (lr) {
            const int left = (int) (a0 + (a1 - a0) * f), right = (int) (b0 + (b1 - b0) * f);
            lr[i * 2] = clamp16(lr[i * 2] + ((left * g) >> 8));
            lr[i * 2 + 1] = clamp16(lr[i * 2 + 1] + ((right * g) >> 8));
        }
        l.pos += step;
    }
}

}  // extern "C"
