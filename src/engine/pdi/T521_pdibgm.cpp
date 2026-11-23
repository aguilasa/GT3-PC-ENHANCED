/* T521 - pdibgm.cxx.  0x0025C830..0x0025CE00.
 *
 * Client of the pdibgm IOP module (streamed background music): one object
 * of the anonymous-namespace class RPC (_GLOBAL_$N$.._.._pdibgm.cxxpLWmzt::RPC)
 * at 0x004A72E0, server 'PBGM' (0x5042474D).  Functions: 1 start, 2 stop,
 * 3 volume, 4 pause, 5 resume, 6 status (copied to g_002FCE00 by
 * pdibgm_update, read by pdibgm_get_status).  Users: RaceBGMPS2 (T130),
 * ads_play / ads_set_volume / ads_stop / music_thread (T466, 0x0022AC98 ..
 * 0x0022AFF8), mpegif::feed (0x0025EF00).
 *
 * PC: there is no IOP.  pdistr.irx's 'PBGM' service (the .ads reader) and
 * pdispu2.irx's stream voices are the native BGM player of
 * engine/sound/native (sound_native.h, docs/sound.md); each function keeps
 * its semantics and calls it directly.  The stream's bytes are read from
 * gt3.vol (platform/disc.h) or from a host: file (platform/host_file.h) by
 * the player's feeder thread, a native thread: the read callback below
 * uses no game kernel call. */
#include "engine/pdi/T521_pdibgm.h"
#include "engine/pdi/pdi_internal.h"
#include "engine/media/native/mp4_movie.h"      /* PC: Mp4AudioReader, MfScope: an .mp3 */
#include "engine/sound/native/bgm_files.h"      /* PC: bgm_probe (an .mp3's length) */
#include "engine/sound/native/sound_native.h"
#include "platform/disc.h"
#include "platform/host_file.h"
#include "platform/settings.h"                  /* PC: the player's music volumes */

#include <algorithm>
#include <vector>

/* x clamped to [lo, hi]: max.s then min.s, volatile asm statements as every
 * max.s / min.s of the image behaves (u018's T055, u032's T077); an inline
 * clamp, as the listing loads both bounds before the max.s.  GCC 14: the C
 * ternaries (max.s / min.s differ only for -0.0 against 0.0). */
static inline f32 bgm_maxf(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 bgm_minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 bgm_clampf(f32 x, f32 lo, f32 hi)
{
    return bgm_minf(bgm_maxf(x, lo), hi);
}

/* The stream status (pdibgm_update copies the server's answer here).  A
 * public C++ global without an initialiser: .data, at the end of the file's
 * variables; aligned (at least 16: the 8 bytes from 0x002FCDF8, after
 * pdispu2's frame info, are padding). */
alignas(64) BgmStatus g_002FCE00;    /* 0x002FCE00 */

/* PC: native, no counterpart in the original. */
namespace {

/* PC: what the IOP's 'PBGM' start request carried ({lsn, sectors, host
 * name}), turned into a byte source for the native player: gt3.vol from
 * sector lsn, or the host: file from byte lsn; `sectors' x 2048 bytes. */
struct BgmSource {
    bool     host;
    int      hostFd;
    u64      base;
    u32      size;
};

BgmSource g_bgmSource = { false, -1, 0, 0 };

/* PC: whose music plays (pdibgm_pc_music); the game's volume last set; the
 * volume sent (the game's x the player's volume of that music); the one
 * last logged.  The game threads, one at a time. */
int g_pcMusic = PDIBGM_MUSIC_NONE;
f32 g_pcGameVolume = 1.0f;
f32 g_pcSent = 1.0f;
f32 g_pcVolume = -1.0f;
/* PC: where pdibgm_start begins the song (pdibgm_pc_start_from, for its call
 * alone; NULL: its start). */
const SndNativeBgmFrom *g_pcFrom = NULL;

/* The player's volume of the music playing (1: as it is). */
f32 pc_music_volume(void)
{
    switch (g_pcMusic) {
    case PDIBGM_MUSIC_MENU:
        return settings::volume(settings::Volume::menuBgm) / 100.0f;
    case PDIBGM_MUSIC_RACE:
        return settings::volume(settings::Volume::raceBgm) / 100.0f;
    case PDIBGM_MUSIC_REPLAY:
        return settings::volume(settings::Volume::replayBgm) / 100.0f;
    }
    return 1.0f;
}

/* Sends the game's volume x the player's ('PBGM' fno 3) and logs a change
 * (a fade: its steps of 0.05 or more, and its end) in percent. */
void pc_send_volume(void)
{
    const f32 v = g_pcGameVolume * pc_music_volume();
    snd_native_bgm_set_volume((int)(v * 32768.0f));
    g_pcSent = v;
    if (v != g_pcVolume && (v == 0.0f || g_pcGameVolume == 1.0f || g_pcVolume < 0.0f || v - g_pcVolume >= 0.05f ||
                            g_pcVolume - v >= 0.05f)) {
        static const char *const music[] = {"", " (menu music)", " (race music)", " (replay music)"};
        printf("pdibgm: volume %d%%%s\n", (int)(v * 100.0f + 0.5f), music[g_pcMusic & 3]);
        g_pcVolume = v;
    }
}

/* Called on the BGM player's feeder thread (native). */
int bgm_source_read(void *user, uint32_t offset, void *dst, uint32_t size)
{
    BgmSource *s = (BgmSource *)user;
    if (offset >= s->size)
        return 0;
    if (size > s->size - offset)
        size = s->size - offset;
    if (s->host)
        return host_file_read_at(s->hostFd, s->base + offset, dst, size);
    return disc::read_bytes(s->base + offset, dst, size) ? (int)size : -1;
}

/* PC: an .mp3 (docs/sound.md 2.4): Media Foundation decodes it
 * (Mp4AudioReader), on the BGM player's feeder thread, which opens it too
 * (snd_native_bgm_start_decoded: nothing of it on the game thread but its
 * head). */
struct Mp3Bgm {
    gt3media::Mp4AudioReader reader;
#ifdef _WIN32
    wchar_t path[1024];                         /* (UTF-16: a song's name can be any) */
#else
    char path[4096];                            /* (UTF-8: a song's name can be any) */
#endif
    std::vector<int16_t> pcm;
    size_t pos;
};

Mp3Bgm *g_mp3 = NULL;

/* On the feeder thread: opened, its length the frames counted (bgm_probe:
 * the whole file read when its head has no count). */
int mp3_open(void *user, int *rate, int *channels, uint64_t *frames)
{
    static thread_local gt3media::MfScope mf;  /* (COM on the feeder thread) */
    Mp3Bgm *m = (Mp3Bgm *)user;
    if (!mf.ok() || !m->reader.open(m->path))
        return 0;
    *rate = m->reader.rate();
    *channels = m->reader.channels();
    spu2::BgmFileInfo info;
    *frames = spu2::bgm_probe(bgm_source_read, &g_bgmSource, g_bgmSource.size, &info) && info.rate != 0
                  ? (uint64_t)info.frames * (uint64_t)*rate / info.rate
                  : 0;
    return 1;
}

int mp3_read(void *user, float *out, int max)
{
    Mp3Bgm *m = (Mp3Bgm *)user;
    const int ch = m->reader.channels();
    int n = 0;
    while (n < max) {
        if (m->pos >= m->pcm.size()) {
            m->pcm.clear();
            m->pos = 0;
            if (!m->reader.next(m->pcm))
                break;                          /* (its end) */
            continue;
        }
        const int k = (int)std::min<size_t>((m->pcm.size() - m->pos) / (size_t)ch, (size_t)(max - n));
        for (int i = 0; i < k * ch; i++)
            out[n * ch + i] = m->pcm[m->pos + (size_t)i] * (1.0f / 32768.0f);
        m->pos += (size_t)k * ch;
        n += k;
    }
    return n;
}

int mp3_rewind(void *user)
{
    Mp3Bgm *m = (Mp3Bgm *)user;
    m->pcm.clear();
    m->pos = 0;
    return m->reader.rewind() ? 1 : 0;
}

/* On the feeder thread, before the first read (g_pcFrom). */
int mp3_seek(void *user, uint64_t frame)
{
    Mp3Bgm *m = (Mp3Bgm *)user;
    m->pcm.clear();
    m->pos = 0;
    const int rate = m->reader.rate();
    return rate > 0 && m->reader.seek((int64_t)(frame * 10000000u / (uint64_t)rate)) ? 1 : 0;
}

/* The source an .mp3 (info: its head's probe); started through Media
 * Foundation, opened by the feeder thread (mp3_open), from `from': 0, -1
 * when it is not one.  (One that does not open plays nothing, as a file
 * that is no music: Mp4AudioReader logs why.) */
int mp3_start(const char *hostName, const spu2::BgmFileInfo &info, int loop, const SndNativeBgmFrom *from)
{
    if (info.kind != spu2::BgmKind::mp3)
        return -1;
    /* (COM on this game thread as well: the reader, made on the feeder
     * thread, is deleted here once the feeder has ended) */
    static thread_local gt3media::MfScope mf;
    if (!mf.ok())
        return -1;
    g_mp3 = new Mp3Bgm();
    g_mp3->pos = 0;
#ifdef _WIN32
    const int full = host_file_full_path_w(hostName, g_mp3->path, sizeof(g_mp3->path) / sizeof(wchar_t));
#else
    const int full = host_file_full_path_utf8(hostName, g_mp3->path, sizeof(g_mp3->path));
#endif
    if (full != 0) {
        delete g_mp3;
        g_mp3 = NULL;
        return -1;
    }
    SndNativeBgmDecoder dec = {};
    dec.user = g_mp3;
    dec.open = mp3_open;
    dec.read = mp3_read;
    dec.rewind = mp3_rewind;
    dec.seek = mp3_seek;
    return snd_native_bgm_start_decoded_from(&dec, loop, from);
}

} // namespace

/* Caller: RaceIOPLoadModules::cmdBindClients.
 * PC: nothing to bind (the native player is started with the platform). */
// FUNC 0x0025C8A0 pdibgm_init
void pdibgm_init(void)
{
}

/* Caller: ads_play (0x0022AC98), with bufSize 0x40000.  Resets the status
 * time to -1 and starts the stream (NOWAIT); hostName is copied into the
 * request when given.  `arg' is the loop flag (pdistr: at the end the
 * reader seeks back to the body).
 * PC: the player of sound_native.h, reading the file through
 * bgm_source_read; bufSize was the IOP's FIFO (the player has its own). */
// FUNC 0x0025C8C8 pdibgm_start
void pdibgm_start(int lsn, int sectors, int arg, int bufSize, const char *hostName)
{
    (void)bufSize;
    g_002FCE00.time  = -1;
    g_002FCE00.ended = 0;

    /* the previous stream's feeder ends before its source is reused */
    snd_native_bgm_stop();
    delete g_mp3;                               /* PC: (its feeder ended) */
    g_mp3 = NULL;
    if (g_bgmSource.host && g_bgmSource.hostFd >= 0)
        host_file_close(g_bgmSource.hostFd);
    g_bgmSource.host = hostName != NULL && hostName[0] != '\0';
    g_bgmSource.size = (u32)sectors * disc::kSectorSize;
    if (g_bgmSource.host) {
        g_bgmSource.hostFd = host_file_open(hostName);
        g_bgmSource.base = (u32)lsn;
        if (g_bgmSource.hostFd < 0) {
            printf("pdibgm: %s not found\n", hostName);
            return;
        }
        /* a loose file ends before the size rounded up to sectors */
        s64 bytes = host_file_size(g_bgmSource.hostFd);
        if (bytes >= (s64)g_bgmSource.base && (u64)bytes - g_bgmSource.base < g_bgmSource.size)
            g_bgmSource.size = (u32)((u64)bytes - g_bgmSource.base);
    } else {
        g_bgmSource.hostFd = -1;
        g_bgmSource.base = (u64)(u32)lsn * disc::kSectorSize;
    }
    /* PC: a .wav (sound_native.h) or an .mp3 (mp3_start) too, known by its
     * head alone (the quick probe: an .mp3's frames are counted on the
     * feeder thread, which reads the samples too) */
    spu2::BgmFileInfo info;
    const bool known = spu2::bgm_probe(bgm_source_read, &g_bgmSource, g_bgmSource.size, &info, true);
    if (snd_native_bgm_start_from(bgm_source_read, &g_bgmSource, arg, g_pcFrom) != 0 &&
        !(g_bgmSource.host && known && mp3_start(hostName, info, arg, g_pcFrom) == 0)) {
        if (g_bgmSource.host)
            printf("pdibgm: %s is not a .wav, an .mp3 or an .ads the game plays\n", hostName);
        else
            printf("pdibgm: the stream at lsn %d is not an .ads\n", lsn);
        return;
    }
    /* PC: what plays, at what volume, from where (the log; an .mp3 without
     * a count in its head: its length not known yet; the voices' ADPCM .ads
     * plays from its start) */
    if (known) {
        std::string what = spu2::bgm_describe(info);
        if (info.frames == 0)
            what = what.substr(what.find(", ") + 2);
        char from[64] = "";
        if (g_pcFrom != NULL && (info.kind != spu2::BgmKind::ads || info.ads.type == spu2::ADS_TYPE_PCM))
            snprintf(from, sizeof(from), ", from %d%% faded in over %g s", (int)(g_pcFrom->at * 100.0f + 0.5f),
                     (double)g_pcFrom->fadeIn);
        printf("pdibgm: %s: %s, volume %d%%%s%s\n", g_bgmSource.host ? hostName : "(the disc)", what.c_str(),
               (int)(g_pcSent * 100.0f + 0.5f), from, arg ? ", looping" : "");
    }
}

/* PC: pdibgm_start from `at' of the song's length (0 .. 1), faded in over
 * `fadeIn' seconds (sound_native.h SndNativeBgmFrom). */
void pdibgm_pc_start_from(int lsn, int sectors, int arg, int bufSize, const char *hostName, f32 at, f32 fadeIn)
{
    const SndNativeBgmFrom from = {at, fadeIn};
    g_pcFrom = &from;
    pdibgm_start(lsn, sectors, arg, bufSize, hostName);
    g_pcFrom = NULL;
}

/* Caller: RaceBGMPS2::pause (0x0017E3C0, pause). */
// FUNC 0x0025C980 pdibgm_pause
void pdibgm_pause(void)
{
    snd_native_bgm_pause();                     /* PC: 'PBGM' fno 4 */
}

/* Caller: RaceBGMPS2::resume (0x0017E3E0, resume). */
// FUNC 0x0025C9A8 pdibgm_resume
void pdibgm_resume(void)
{
    snd_native_bgm_resume();                    /* PC: 'PBGM' fno 5 */
}

/* Caller: ads_stop (0x0022AE18). */
// FUNC 0x0025C9D0 pdibgm_stop
void pdibgm_stop(void)
{
    snd_native_bgm_stop();                      /* PC: 'PBGM' fno 2 */
    delete g_mp3;                               /* PC: an .mp3's reader (its feeder ended) */
    g_mp3 = NULL;
}

/* Callers: RaceBGMPS2::update (0x0017DDF0), ads_set_volume (0x0022AC78),
 * mpegif::feed (0x0025EF00).  volume is clamped to [-1, 1] (max.s / min.s)
 * and sent as volume * 32768 (cvt.w.s truncates). */
// FUNC 0x0025C9F8 pdibgm_set_volume
void pdibgm_set_volume(f32 volume)
{
    volume = bgm_clampf(volume, -1.0f, 1.0f);
    /* PC: 'PBGM' fno 3, times the player's volume of the music playing
     * (pdibgm_pc_music); logged */
    g_pcGameVolume = volume;
    pc_send_volume();
}

/* PC: whose music plays (T521_pdibgm.h); a change sends the volume with
 * the player's volume of the new one. */
void pdibgm_pc_music(int music)
{
    if (music == g_pcMusic)
        return;
    g_pcMusic = music;
    pc_send_volume();
}

/* Caller: music_thread (0x0022AFF8), per frame.  RPC::updateStatus
 * (0x0025CB88): 'PBGM' fno 6, the answer copied with interrupts off. */
// FUNC 0x0025CA78 pdibgm_update
void pdibgm_update(void)
{
    SndNativeBgmStatus answer;                  /* PC: the native player's status */

    /* PC: the player's volume of the music changed (the settings menu) */
    if (g_pcGameVolume * pc_music_volume() != g_pcSent)
        pc_send_volume();
    snd_native_bgm_get_status(&answer);
    int wasEnabled = intr_suspend();
    memcpy(&g_002FCE00, &answer, sizeof(BgmStatus));
    intr_resume(wasEnabled);
}

/* Caller: RaceBGMPS2::update.  Reads the status with interrupts off and
 * returns its time in seconds (__floatdisf, then / 1e6). */
// FUNC 0x0025CA98 pdibgm_get_status
f32 pdibgm_get_status(int *pos, int *id, int *ended)
{
    int wasEnabled = intr_suspend();
    f32 seconds;

    if (pos != NULL)
        *pos = g_002FCE00.pos;
    if (id != NULL)
        *id = g_002FCE00.id;
    if (ended != NULL)
        *ended = g_002FCE00.ended;
    seconds = (f32)g_002FCE00.time / 1000000.0f;   /* implicit: __floatdisf (s64 -> float) */
    intr_resume(wasEnabled);
    return seconds;
}

/* PC: the native status block is the IOP's answer, byte for byte. */
static_assert(sizeof(SndNativeBgmStatus) == sizeof(BgmStatus), "BgmStatus");
