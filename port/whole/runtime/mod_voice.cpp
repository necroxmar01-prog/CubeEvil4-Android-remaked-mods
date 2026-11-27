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

#include <atomic>
#include <fcntl.h>
#include <pthread.h>
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
    int fd;           // its file (the bank's, or a movie track's own)
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
        w.fd = b.fd;
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

// ---- the playing lines -----------------------------------------------------------------------------------------------

const unsigned kCache = 4096;    // source frames decoded at a time
const unsigned kRing = 1u << 17; // source frames read ahead per line (4 s at 32 kHz)
const int kLines = 4;
const unsigned kAfter = 4 * 32000;  // AI frames a line plays on after its stream
const unsigned kFade = 9600;        // ... the last of them faded out            // as many as the sound driver's stream works (SND_STR_MAX)

// The reading: a thread of its own decodes each line's wave ahead into the line's ring (DevJunior, 26.11.2026, report
// 210643: "a veces hay cortes en los audios", the AI ring ran dry 43 times in four minutes of dubbed cutscenes: the
// first version read the file on the game's thread, a few ms per read on the phone's shared storage). The mixer only
// takes from the rings; a frame not read yet plays as silence, the line keeps its time.
// Several lines at once (DevJunior, 26.11.2026, 22:15: "el audio sigue entrecortándose"; the log of 210643: event
// voice 3 cut after 0.9 of 4.1 s when voice 140 started): a scene may start the next voice stream while one still
// plays, each in a stream work of its own; one line per stream now, up to kLines, a new one takes a free place (or the
// oldest).
struct Decoder {
    Wave w;
    unsigned frames;
    int16_t cache[kCache * 2];
    unsigned cacheAt, cacheLen;  // source frames
};

struct Reader {
    std::atomic<unsigned> want;      // the line's number the mixer wants (0 none)
    std::atomic<unsigned> serving;   // the line the ring holds
    std::atomic<unsigned> written;   // source frames in the ring: [start, written)
    std::atomic<unsigned> consumed;  // the mixer's position (source frames)
    Wave w;                          // the wanted line's wave and its first frame (set before `want`)
    unsigned start;
    int16_t ring[kRing * 2];
    std::atomic<unsigned> late;      // frames the mixer found not read yet
    Decoder dec;                     // the reading thread's
};

struct Line {
    bool on;
    unsigned number;      // its number for the reader
    unsigned age;         // when it started (lines started before)
    uint32_t sndId;
    const Wave* w;
    unsigned frames;      // the wave's
    double pos;           // in source frames
    unsigned delay;       // AI frames of silence still to go
    bool started;         // the game's stream sounded
    bool streamGone;      // ... and is over
    bool fadingOut;       // the game faded it out (a stop request, the scene's end)
    unsigned after;       // AI frames played since the stream ended or faded out
    float gain;           // from the stream's volume
    bool movie;           // a movie's own sound track (re4_mod_voice_movie)
    Reader r;
};
Line gLines[kLines];
std::atomic<bool> gThread(false);

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

// the decoder's cache holds source frame `at` on (stereo L, R)
bool fill(Decoder& l, unsigned at)
{
    const Wave& w = l.w;
    const unsigned ch = w.channels;
    l.cacheLen = 0;
    if (w.tag == 0) {
        const unsigned bytes = w.bits16 ? 2 : 1;
        unsigned take = l.frames - at < kCache ? l.frames - at : kCache;
        static unsigned char raw[kCache * 2 * 8];
        const ssize_t got = pread(w.fd, raw, take * ch * bytes, (off_t) w.offset + (off_t) at * ch * bytes);
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
            if (pread(w.fd, raw, block, (off_t) w.offset + (off_t) b * block) != (ssize_t) block) {
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

// one step of a line's reader: decodes its wave ahead into its ring; false when there is nothing to do
bool pump(Reader& r)
{
    Decoder& d = r.dec;
    const unsigned want = r.want.load(std::memory_order_acquire);
    if (want != r.serving.load(std::memory_order_relaxed)) {
        d.w = r.w;
        if (r.want.load(std::memory_order_acquire) != want) {
            return true;  // a newer line meanwhile: copy that one
        }
        d.frames = waveFrames(d.w);
        d.cacheAt = d.cacheLen = 0;
        r.written.store(r.start, std::memory_order_relaxed);
        r.serving.store(want, std::memory_order_release);
    }
    if (want == 0) {
        return false;
    }
    const unsigned at = r.written.load(std::memory_order_relaxed);
    // 2 s ahead at most (DevJunior, 27.11.2026: "a veces se cortan los diálogos y cinemáticas": the 4 s at once at a
    // line's start competed with the game's own reads of the scene)
    if (at >= d.frames || at - r.consumed.load(std::memory_order_acquire) + kCache > kRing / 2) {
        return false;
    }
    if (!fill(d, at) || at < d.cacheAt || at >= d.cacheAt + d.cacheLen) {
        d.frames = at;  // a read error: the line ends here
        return false;
    }
    const unsigned n = d.cacheAt + d.cacheLen - at;
    for (unsigned i = 0; i < n; i++) {
        const unsigned k = (at + i) & (kRing - 1);
        r.ring[k * 2] = d.cache[(at - d.cacheAt + i) * 2];
        r.ring[k * 2 + 1] = d.cache[(at - d.cacheAt + i) * 2 + 1];
    }
    if (r.want.load(std::memory_order_acquire) == want) {
        r.written.store(at + n, std::memory_order_release);
    }
    return true;
}

#ifndef RE4_VOICE_TEST
void* readerThread(void*)
{
    for (;;) {
        bool busy = false;
        for (int i = 0; i < kLines; i++) {
            busy |= pump(gLines[i].r);
        }
        usleep(busy ? 1000 : 4000);  // a pause between the reads too: the game's own file reads go first
    }
    return 0;
}
#endif

// frame `at` of the line from its ring: 1 there, 0 not read yet, -1 past its end
int frameAt(Line& l, unsigned at, int* left, int* right)
{
    if (at >= l.frames) {
        return -1;
    }
    Reader& r = l.r;
#ifdef RE4_VOICE_TEST
    while (pump(r)) {
    }
#endif
    if (r.serving.load(std::memory_order_acquire) != l.number || at < r.start ||
        at >= r.written.load(std::memory_order_acquire)) {
        return 0;
    }
    const unsigned k = at & (kRing - 1);
    *left = r.ring[k * 2];
    *right = r.ring[k * 2 + 1];
    return 1;
}

void stop(Line& l, const char* why)
{
    if (l.on) {
        re4_log("voice mod: line of stream %u ends (%s) at %.1f of %.1f s, %u frames not read in time",
                (unsigned) l.sndId, why, l.pos / l.w->rate, (double) l.frames / l.w->rate, l.r.late.exchange(0));
    }
    l.on = false;
    l.r.want.store(0, std::memory_order_release);
}

void stopAll(const char* why)
{
    for (int i = 0; i < kLines; i++) {
        stop(gLines[i], why);
    }
}

// the game's side (runtime/mod_voice_game.cpp, set at the first line): bit 0 the line's stream is there, bit 1 it
// sounds, bit 2 an event runs, bit 3 the player skips it; NULL (tests): a stream that sounds
int (*gState)(unsigned id);

// A line on a free place (else the oldest line's), `skip` ms into its wave (negative: that much silence first).
Line& startLine(unsigned id, const Wave& w, double skip)
{
    // a free place, else the oldest line's
    static unsigned ages;
    Line* pick = &gLines[0];
    for (int i = 0; i < kLines; i++) {
        Line& c2 = gLines[i];
        if (c2.on && c2.sndId == id) {
            pick = &c2;  // the same stream again
            break;
        }
        if (!c2.on && pick->on) {
            pick = &c2;
        } else if (c2.on && pick->on && c2.age < pick->age) {
            pick = &c2;
        }
    }
    stop(*pick, "a new line in its place");
    Line& l = *pick;
    l.age = ++ages;
    l.on = true;
    l.sndId = id;
    l.w = &w;
    l.frames = waveFrames(w);
    l.pos = skip > 0 ? skip / 1000.0 * w.rate : 0.0;
    l.delay = skip < 0 ? (unsigned) (-skip / 1000.0 * kRate) : 0;
    l.started = false;
    l.streamGone = false;
    l.fadingOut = false;
    l.after = 0;
    l.gain = 0.0f;
    l.movie = false;
    static unsigned numbers;
    l.number = ++numbers ? numbers : ++numbers;
    Reader& r = l.r;
    r.late = 0;
    r.w = w;
    r.start = (unsigned) l.pos;
    r.consumed.store(r.start, std::memory_order_relaxed);
    r.want.store(l.number, std::memory_order_release);
#ifndef RE4_VOICE_TEST
    if (!gThread.exchange(true)) {
        pthread_t t;
        if (pthread_create(&t, 0, readerThread, 0) == 0) {
            pthread_detach(t);
        } else {
            gThread = false;
            re4_log("voice mod: the reading thread cannot start");
        }
    }
#endif
    return l;
}

// ---- the movies' sound tracks (wish of DevJunior, 27.11.2026: the opening movie with the Latin American dub's
// narration): voice/<movie>.wav (PCM 8/16 bit, mono or stereo, any rate; made from the Steam release's movie with
// ffmpeg), played from the movie's first picture over it, the movie's own sound muted (runtime/movie_status.cpp);
// it ends with the movie (its end or START).

struct MovieTrack {
    char name[64];     // the movie's file name without folder and extension ("opening")
    char path[1600];
    bool tried, ok;
    Wave w;
};
const int kMovies = 8;
MovieTrack gMovies[kMovies];
int gMovieCount;
Line* gMovieLine;

bool movieOpen(MovieTrack& m)
{
    if (m.tried) {
        return m.ok;
    }
    m.tried = true;
    m.w.fd = open(m.path, O_RDONLY);
    unsigned char h[12];
    if (m.w.fd < 0 || pread(m.w.fd, h, 12, 0) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        re4_log("voice mod: %s is not a WAV file", m.path);
        return false;
    }
    off_t at = 12;
    bool fmt = false;
    for (;;) {
        unsigned char c[24];
        if (pread(m.w.fd, c, 8, at) != 8) {
            break;
        }
        const uint32_t len = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0 && len >= 16 && pread(m.w.fd, c + 8, 16, at + 8) == 16) {
            const unsigned tag = le16(c + 8), bits = le16(c + 22);
            m.w.channels = le16(c + 10);
            m.w.rate = le32(c + 12);
            m.w.tag = 0;
            m.w.bits16 = bits == 16;
            fmt = tag == 1 && (bits == 8 || bits == 16) && m.w.channels >= 1 && m.w.channels <= 2 && m.w.rate >= 4000;
        } else if (memcmp(c, "data", 4) == 0) {
            m.w.offset = (uint32_t) (at + 8);
            m.w.length = len;
            m.ok = fmt;
            break;
        }
        at += 8 + len + (len & 1);
    }
    re4_log("voice mod: %s: %s", m.path, m.ok ? "a movie's sound track" : "no PCM sound in it");
    return m.ok;
}

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
    stopAll("the mods changed");
    bankClose();
    gBank.xwb[0] = gBank.xsb[0] = 0;
    for (int i = 0; i < gMovieCount; i++) {
        if (gMovies[i].w.fd > 0) {
            close(gMovies[i].w.fd);
        }
    }
    memset(gMovies, 0, sizeof(gMovies));
    gMovieCount = 0;
    gMovieLine = 0;
}

// runtime/movie_status.cpp: a movie (its file name, e.g. "movie/opening.sfd") starts. 1: the mod has its sound track,
// the movie's own sound is to be muted.
int re4_mod_voice_movie_start(const char* fname)
{
    const char* base = strrchr(fname, '/') ? strrchr(fname, '/') + 1 : fname;
    const char* dot = strrchr(base, '.');
    const int n = dot ? (int) (dot - base) : (int) strlen(base);
    for (int i = 0; i < gMovieCount; i++) {
        MovieTrack& m = gMovies[i];
        if ((int) strlen(m.name) == n && strncasecmp(m.name, base, n) == 0 && movieOpen(m)) {
            Line& l = startLine(0xFFFFFFFFu, m.w, 0.0);
            l.movie = true;
            l.gain = 1.0f;
            gMovieLine = &l;
            re4_log("voice mod: movie %s -> %s, %.1f s at %u Hz", fname, m.path, (double) l.frames / m.w.rate,
                    m.w.rate);
            return 1;
        }
    }
    return 0;
}

// runtime/movie_status.cpp: the movie shows its first picture (the track starts) / ends (START or its end)
void re4_mod_voice_movie_shown(void)
{
    if (gMovieLine && gMovieLine->on && gMovieLine->movie) {
        gMovieLine->started = true;
    }
}

void re4_mod_voice_movie_end(int skipped)
{
    if (gMovieLine && gMovieLine->on && gMovieLine->movie) {
        stop(*gMovieLine, skipped ? "the movie is skipped" : "the movie ended");
    }
    gMovieLine = 0;
}

// dvd.cpp modFolder: a pack's voice/<name>; a later pack's replaces it
void re4_mod_voice_add(const char* name, const char* path)
{
    if (strcasecmp(name, "bio4evt.xwb") == 0) {
        snprintf(gBank.xwb, sizeof(gBank.xwb), "%s", path);
    } else if (strcasecmp(name, "bio4evt.xsb") == 0) {
        snprintf(gBank.xsb, sizeof(gBank.xsb), "%s", path);
    } else if (strlen(name) > 4 && strcasecmp(name + strlen(name) - 4, ".wav") == 0) {
        // a movie's sound track: voice/<movie>.wav
        MovieTrack* m = 0;
        char base[64];
        snprintf(base, sizeof(base), "%.*s", (int) (strlen(name) - 4), name);
        for (int i = 0; i < gMovieCount; i++) {
            if (strcasecmp(gMovies[i].name, base) == 0) {
                m = &gMovies[i];
            }
        }
        if (!m && gMovieCount < kMovies) {
            m = &gMovies[gMovieCount++];
        }
        if (m) {
            if (m->w.fd > 0) {
                close(m->w.fd);
            }
            memset(m, 0, sizeof(*m));
            snprintf(m->name, sizeof(m->name), "%s", base);
            snprintf(m->path, sizeof(m->path), "%s", path);
        }
        return;
    } else {
        re4_log("voice mod: %s is not read (bio4evt.xwb, bio4evt.xsb and <movie>.wav)", path);
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
    Line& l = startLine(id, w, start * 1000.0 - c.delay);
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
    Line* l = 0;
    for (int i = 0; i < kLines; i++) {
        if (gLines[i].on && gLines[i].sndId == id) {
            l = &gLines[i];
        }
    }
    if (!l) {
        return axVol;
    }
    if (fading && fadeEnd == 0) {
        // (DevJunior, 27.11.2026: "se siguen cortando las partes finales de los diálogos") the game fades its stream
        // out as the scene ends; a dub line runs longer than the game's: it keeps its volume and plays on (mixLine)
        l->fadingOut = true;
        return -904;
    }
    if (nowVol != 0 || fading) {
        l->gain = axVol <= -904 ? 0.0f : powf(10.0f, (float) axVol / 200.0f);
    }
    return -904;
}

// ai.cpp play: on top of the game's block, 32 kHz stereo (L, R); lr NULL: the block was lost (the app does not read),
// the line goes on in step
static void mixLine(Line& l, short* lr, unsigned frames)
{
    if (!l.on) {
        return;
    }
    if (l.movie && !l.started) {
        return;  // the movie's first picture not shown yet
    }
    const int st = l.movie ? 7 : gState ? gState(l.sndId) : 3;
    if (!l.started) {
        if (!(st & 1)) {
            stop(l, "the game's stream did not start");
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
    if (st & 8) {
        stop(l, "the scene is skipped");
        return;
    }
    // a line longer than the game's goes on after its stream (stopped, faded or over, the scene over or not) to its
    // own end, 4 s at most, the last 0.3 s faded out
    if (l.streamGone || l.fadingOut) {
        if (l.after >= kAfter) {
            stop(l, "4 s after the game's stream");
            return;
        }
    }
    const double step = (double) l.w->rate / kRate;
    const int g = (int) (l.gain * 256.0f + 0.5f);
    for (unsigned i = 0; i < frames; i++) {
        if (l.delay) {
            l.delay--;
            continue;
        }
        const unsigned at = (unsigned) l.pos;
        int a0 = 0, b0 = 0, a1, b1;
        const int got = frameAt(l, at, &a0, &b0);
        if (got < 0) {
            stop(l, "its end");
            return;
        }
        if (got == 0) {
            l.r.late++;
        }
        if (frameAt(l, at + 1, &a1, &b1) <= 0) {
            a1 = a0;
            b1 = b0;
        }
        const double f = l.pos - at;
        int gi = g;
        if (l.streamGone || l.fadingOut) {
            if (l.after >= kAfter) {
                break;
            }
            if (kAfter - l.after < kFade) {
                gi = (int) ((long long) g * (kAfter - l.after) / kFade);
            }
            l.after++;
        }
        if (lr && got > 0) {
            const int left = (int) (a0 + (a1 - a0) * f), right = (int) (b0 + (b1 - b0) * f);
            lr[i * 2] = clamp16(lr[i * 2] + ((left * gi) >> 8));
            lr[i * 2 + 1] = clamp16(lr[i * 2 + 1] + ((right * gi) >> 8));
        }
        l.pos += step;
    }
    l.r.consumed.store((unsigned) l.pos, std::memory_order_release);
}

void re4_mod_voice_mix(short* lr, unsigned frames)
{
    for (int i = 0; i < kLines; i++) {
        mixLine(gLines[i], lr, frames);
    }
}

}  // extern "C"
