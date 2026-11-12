// nn::audio AudioOut: IAudioOutManager and IAudioOut, reached on the Switch 2 through aud:u command 1 (audren.c
// aud_user_handler). Kirby Air Riders opens the default AudioOut at boot (nn::audio::OpenDefaultAudioOut) beside its
// audio renderer. Command numbers, arguments and AudioOutBuffer from Switchbrew, "Audio services".
//
// Timed like the console's: every appended buffer is released once its samples would have played (bytes / (channels *
// 2) / sample rate), on a host thread, and the buffer event is signalled, so a game that waits for released buffers is
// paced as on hardware and never waits forever. Kirby Air Riders mixes its own sound and plays it only through this
// AudioOut (it opens no audio renderer), so the samples of each appended buffer also go to an SDL3 audio stream on the
// default playback device, PCM16 at 48 kHz with the game's channel count (SDL converts 5.1 to the device). The queue
// is kept under AO_MAX_QUEUE_MS: if the device falls behind, what is over is dropped rather than letting latency grow.
// TOTK_AUDIO=none (or TOTK_AUDIO_OFF) keeps it silent, as for the audio renderer (audio_mixer.c).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audren.h"
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define AO_MAX_QUEUE_MS 250

#define AO_RATE 48000
#define AO_CHANNELS 2
#define AO_FORMAT_PCM16 2
#define AO_STATE_STARTED 0
#define AO_STATE_STOPPED 1
#define AO_MAX 64

typedef struct { uint64_t next, samples, capacity, size, offset; } AudioOutBuffer;   // 0x28 bytes
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer");

typedef struct {
    PlatMutex lock;
    PlatCond cv;
    PlatThread *thread;
    int running, started, channels;
    uint32_t event;                       // writable; RegisterBufferEvent hands out its readable side
    uint64_t queued[AO_MAX], queued_bytes[AO_MAX];   // client pointers in append order, and their data sizes
    int nqueued;
    uint64_t released[AO_MAX];
    int nreleased;
    uint64_t played_samples;
    float volume;
    SDL_AudioStream *stream;              // NULL: silent
    uint64_t dropped_bytes;
} AudioOut;

// The SDL stream of an AudioOut, or NULL (logged) when audio is off or no device opens.
static SDL_AudioStream *open_stream(int channels) {
    const char *mode = cfg_get("TOTK_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF")) {
        rt_log("audout: TOTK_AUDIO=none: silent");
        return NULL;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audout: SDL audio init failed (%s): silent", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, AO_RATE};
    SDL_AudioStream *s = SDL_OpenAudioDeviceStream(audio_pick_device(), &spec, NULL, NULL);
    if (!s) {
        rt_log("audout: no output device (%s): silent", SDL_GetError());
        return NULL;
    }
    if (!SDL_ResumeAudioStreamDevice(s)) {
        rt_log("audout: starting the output failed (%s): silent", SDL_GetError());
        SDL_DestroyAudioStream(s);
        return NULL;
    }
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s));
    rt_log("audout: playing on SDL device '%s', %d Hz x%d PCM16", name ? name : "?", AO_RATE, channels);
    return s;
}

// The port menu's volume (TOTK_VOLUME, percent, live) on top of the game's SetAudioOutVolume, as audio_mixer.c applies it
// to the audio renderer: Kirby Air Riders plays only through AudioOut, so the menu slider did nothing before.
// TOTK_MUTE_UNFOCUSED=1 (the Sound tab, live) makes it 0 while the game window is out of focus.
float audio_menu_gain(void) {
    static float v = 1.0f;
    static int mute_bg;
    static unsigned seen = ~0u;
    unsigned ver = cfg_version();
    if (ver != seen) {
        seen = ver;
        double pct = cfg_double("TOTK_VOLUME");
        v = (float)((pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100);
        mute_bg = cfg_bool("TOTK_MUTE_UNFOCUSED");
    }
    return mute_bg && !gfx_window_focused() ? 0.0f : v;
}

static float menu_volume(void) { return audio_menu_gain(); }

// The playback device TOTK_AUDIO_DEVICE names (the Sound tab, read when a stream opens), or the system default when it
// is unset or no device of that name is present now.
uint32_t audio_pick_device(void) {
    const char *want = cfg_get("TOTK_AUDIO_DEVICE");
    SDL_AudioDeviceID got = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!want || !*want) return got;
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
    for (int i = 0; ids && i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (name && !strcmp(name, want)) { got = ids[i]; break; }
    }
    SDL_free(ids);
    if (got == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) rt_log("audio: device '%s' not found: system default", want);
    return got;
}

static void apply_gain(AudioOut *o) {
    if (o->stream) SDL_SetAudioStreamGain(o->stream, o->volume * menu_volume());
}

// Sends an appended buffer's samples to the device. AudioOutBuffer.samples is the data pointer, size the bytes of
// sample data; a nonzero offset (not seen so far) skips that many bytes of it.
static void play(AudioOut *o, const AudioOutBuffer *ab) {
    if (!o->stream || !ab->samples || !ab->size || ab->offset >= ab->size || ab->size > (64u << 20)) return;
    uint64_t len = ab->size - ab->offset;
    const void *data = ipc_guest(ab->samples + ab->offset, len);
    if (!data) return;
    int bytes_per_ms = AO_RATE / 1000 * o->channels * 2;
    int queued = SDL_GetAudioStreamQueued(o->stream);
    if (queued > AO_MAX_QUEUE_MS * bytes_per_ms) {   // the device fell behind: start over from this buffer
        o->dropped_bytes += (uint64_t)queued;
        SDL_ClearAudioStream(o->stream);
        static int told;
        if (told++ < 4) rt_log("audout: device behind by %d ms: queue dropped", queued / bytes_per_ms);
    }
    static float last_gain = -1.0f;   // follow the menu slider while playing
    if (o->volume * menu_volume() != last_gain) last_gain = o->volume * menu_volume(), apply_gain(o);
    SDL_PutAudioStreamData(o->stream, data, (int)len);
    static int heard;
    if (!heard) {   // the first buffer with sound in it (silence is all zero)
        const int16_t *s = data;
        for (uint64_t i = 0; i < len / 2 && !heard; i++) heard = s[i] != 0;
        if (heard) rt_log("audout: first nonzero samples after %llu played", (unsigned long long)o->played_samples);
    }
}

static const char device_name[] = "DeviceOut";

// Releases the oldest queued buffer once its samples would have played, while started.
static void release_thread(void *p) {
    AudioOut *o = p;
    plat_mutex_lock(&o->lock);
    while (o->running) {
        if (!o->started || !o->nqueued) {
            plat_cond_wait(&o->cv, &o->lock);
            continue;
        }
        uint64_t bytes = o->queued_bytes[0];
        uint64_t frames = bytes / (uint64_t)(o->channels * 2);
        uint64_t ns = frames * 1000000000ull / AO_RATE;
        plat_mutex_unlock(&o->lock);
        plat_sleep_ns(ns ? ns : 1000000);
        plat_mutex_lock(&o->lock);
        if (!o->running || !o->started || !o->nqueued) continue;
        uint64_t tag = o->queued[0];
        memmove(o->queued, o->queued + 1, (size_t)(o->nqueued - 1) * sizeof *o->queued);
        memmove(o->queued_bytes, o->queued_bytes + 1, (size_t)(o->nqueued - 1) * sizeof *o->queued_bytes);
        o->nqueued--;
        if (o->nreleased < AO_MAX) o->released[o->nreleased++] = tag;
        o->played_samples += frames;
        kern_event_signal(o->event);
    }
    plat_mutex_unlock(&o->lock);
}

static void audout_close(void *p) {
    AudioOut *o = p;
    plat_mutex_lock(&o->lock);
    o->running = 0;
    plat_cond_broadcast(&o->cv);
    plat_mutex_unlock(&o->lock);
    plat_thread_join(o->thread);
    kern_handle_close(o->event);
    if (o->stream) SDL_DestroyAudioStream(o->stream);
    free(o);
}

static IpcBuffer in_buffer(const IpcRequest *rq) {
    if (rq->na && rq->a[0].size) return rq->a[0];
    return rq->nx ? rq->x[0] : (IpcBuffer){0};
}

static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}

static void audout_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: {   // GetAudioOutState
        plat_mutex_lock(&o->lock);
        uint32_t v = o->started ? AO_STATE_STARTED : AO_STATE_STOPPED;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 1: case 2:   // Start, Stop
        plat_mutex_lock(&o->lock);
        o->started = rq->cmd == 1;
        plat_cond_broadcast(&o->cv);
        plat_mutex_unlock(&o->lock);
        return;
    case 3: case 7: {   // AppendAudioOutBuffer(Auto)(AudioOutBuffer in a buffer, u64 client pointer)
        IpcBuffer b = in_buffer(rq);
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        AudioOutBuffer ab = {0};
        if (b.size >= sizeof ab) memcpy(&ab, ipc_guest(b.addr, sizeof ab), sizeof ab);
        plat_mutex_lock(&o->lock);
        if (o->nqueued < AO_MAX) {
            play(o, &ab);
            o->queued[o->nqueued] = tag;
            o->queued_bytes[o->nqueued] = ab.size;
            o->nqueued++;
            plat_cond_broadcast(&o->cv);
        }
        plat_mutex_unlock(&o->lock);
        return;
    }
    case 4:   // RegisterBufferEvent
        rs->copy[rs->ncopy++] = kern_event_readable(o->event);
        return;
    case 5: case 8: {   // GetReleasedAudioOutBuffers(Auto) -> client pointers, u32 count
        int pointer;
        IpcBuffer b = out_buffer(rq, &pointer);
        uint32_t max = (uint32_t)(b.size / sizeof(uint64_t)), n = 0;
        uint64_t *dst = max ? ipc_guest(b.addr, (uint64_t)max * sizeof(uint64_t)) : NULL;
        plat_mutex_lock(&o->lock);
        while (n < max && (int)n < o->nreleased) {
            dst[n] = o->released[n];
            n++;
        }
        memmove(o->released, o->released + n, (size_t)(o->nreleased - (int)n) * sizeof *o->released);
        o->nreleased -= (int)n;
        plat_mutex_unlock(&o->lock);
        if (pointer && n) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, n * sizeof(uint64_t), b.index};
        IPC_OUT(rs, n);
        return;
    }
    case 6: {   // ContainsAudioOutBuffer(u64 client pointer) -> bool
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t v = 0;
        plat_mutex_lock(&o->lock);
        for (int i = 0; i < o->nqueued && !v; i++) v = o->queued[i] == tag;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 9: {   // GetAudioOutBufferCount
        plat_mutex_lock(&o->lock);
        uint32_t v = (uint32_t)o->nqueued;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 10: {   // GetAudioOutPlayedSampleCount
        plat_mutex_lock(&o->lock);
        uint64_t v = o->played_samples;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 11: {   // FlushAudioOutBuffers -> bool pending: every queued buffer released now
        plat_mutex_lock(&o->lock);
        uint8_t pending = o->nqueued > 0;
        for (int i = 0; i < o->nqueued && o->nreleased < AO_MAX; i++) o->released[o->nreleased++] = o->queued[i];
        o->nqueued = 0;
        if (pending) kern_event_signal(o->event);
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, pending);
        return;
    }
    case 12:   // SetAudioOutVolume
        o->volume = IPC_ARG(rq, float, 0);
        apply_gain(o);
        return;
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }   // GetAudioOutVolume
    default: ipc_unimplemented(s, rq);
    }
}

static void put_device_name(const IpcRequest *rq, IpcResponse *rs) {
    int pointer;
    IpcBuffer b = out_buffer(rq, &pointer);
    if (!b.size) return;
    char *dst = ipc_guest(b.addr, b.size);
    memset(dst, 0, b.size);
    memcpy(dst, device_name, b.size < sizeof device_name ? b.size : sizeof device_name);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, b.size, b.index};
}

void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: {   // ListAudioOuts(Auto) -> names, u32 count
        put_device_name(rq, rs);
        uint32_t n = 1;
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 3: {   // OpenAudioOut(Auto)(AudioOutParameter, aruid; name in/out) -> IAudioOut, AudioOutParameterInternal
        uint32_t rate = IPC_ARG(rq, uint32_t, 0);
        uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        AudioOut *o = calloc(1, sizeof *o);
        if (!o) rt_fatal("audout: allocation");
        plat_mutex_init(&o->lock);
        plat_cond_init(&o->cv);
        o->running = 1;
        o->channels = channels == 6 ? 6 : AO_CHANNELS;
        o->volume = 1.0f;
        o->event = kern_event_new();
        o->thread = plat_thread_new(release_thread, o, "audout");
        if (!o->thread) rt_fatal("audout: cannot start the release thread");
        put_device_name(rq, rs);
        o->stream = open_stream(o->channels);
        rt_log("audout: AudioOut opened (asked %u Hz x%u; %d Hz x%d PCM16, %s, released in real time)", rate,
               channels, AO_RATE, o->channels, o->stream ? "played" : "silent");
        struct { uint32_t rate, channels, format, state; } out = {AO_RATE, (uint32_t)o->channels, AO_FORMAT_PCM16,
                                                                   AO_STATE_STOPPED};
        IPC_OUT(rs, out);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", audout_handler, o, audout_close);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
