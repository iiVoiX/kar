// nn::codec video decoding (the Switch 2 SDK's VP9 path) on the host with libvpx, for Donkey Kong Bananza's tutorial
// guide movies (Movie/Guide*.webm, 960x540). The SDK's nn::codec::video::Initialize opens the console's video service
// over IPC, which the host does not serve (the first guide movie crashed in nn::codec::video::detail::ipc::Initialize),
// so the decoder calls are host functions. The demuxer (nn::movie::MediaExtractor) stays guest code.
// What the game's movie player (main+0xdfb8xx..0xdfe9xx) expects, read from its code:
// - each output buffer i is an image pool of its own, tied to i by RegisterVp9DecoderBuffer; the player reads the
//   pool's size at +0x20 / +0x22 (u16 width, height), which InitializeDecoderImagePool takes from the preference
//   (MakeDecodeImagePreference, SDK code: +8 u16 width - 1, +10 u16 height - 1);
// - it acquires an output buffer before it submits input (its wrapper aborts on any failure there): Acquire reserves
//   the buffer the next shown picture goes to and returns a fence (NativeSyncHandle, which the caller zeroed; the
//   host writes a token in its first word); the player waits for it through nn::syncpt::sdk::RequestSyncPointEvent,
//   signalled when SubmitVp9DecoderInputBuffer has decoded a picture into that buffer (at once if it already has), or
//   polls it through nn::syncpt::sdk::TryWaitSyncPoint, answered from the same buffer state;
// - a submission with no reserved buffer to decode into answers "not ready" (the wrapper's retry code 0x76);
// - the picture is read with ReadDecoderOutputImageNv12 (Y plane, then interleaved UV, each with the caller's pitch).
//   GetDecoderImagePoolNativeImageHandle (the player's GPU path) has no image yet and returns 0.
// libvpx is BSD-3-Clause and already linked for ov_movie.c.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "host.h"
#include "platform/platform.h"

enum { R_NOT_READY = 0x1c266f };   // (result & 0x3fffff) - 0x1c2000 == 0x66f: "try again" for the game's wrapper
// QUEUED: packets the decoder takes ahead of the buffers for their pictures, as Eglantine's video session does
// (nx2_gsv.h, INx2VideoSession::MaxQueuedInputs); a submission past them answers "not ready".
enum { SLOTS = 16, DECODERS = 4, WAITERS = 4, QUEUED = 8 };
enum { S_FREE, S_WAIT, S_FILLED };   // registered; acquired with no picture yet; acquired holding a picture
enum { FENCE_TAG = 0xc0de0000u };    // fence token: tag | decoder << 8 | buffer

// TOTK_TRACE=codec: a line for each decoder call that moves a buffer or a picture, to follow a player that stops.
#define TRACE(...)                                                                                                   \
    do {                                                                                                             \
        if (rt_trace & RT_TRACE_CODEC) rt_log("codec: " __VA_ARGS__);                                                \
    } while (0)

#define LOG_FIRST(...)                                                                                               \
    do {                                                                                                             \
        static volatile int32_t logged;                                                                              \
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) rt_log(__VA_ARGS__);                                 \
    } while (0)

typedef struct {
    uint64_t pool;     // guest VideoDecoderImagePoolType registered for this index; 0: not registered
    int state;
    uint32_t seq;      // acquisition order: pictures fill the oldest waiting buffer
    uint64_t waiter[WAITERS];   // guest system events of sync point events requested before the picture came
    uint16_t w, h;
    int64_t pts;       // nanoseconds (nn::TimeSpan)
    int eos;
    uint8_t *nv12;     // w*h luma, then cw*ch interleaved UV pairs
} Slot;

// A packet submitted while no buffer was reserved for its picture, decoded once one is (ov_vp9_acquire).
typedef struct { uint8_t *data; size_t size; int64_t pts; int end; } Packet;

typedef struct {
    uint64_t key;      // guest Vp9DecoderType; 0: unused
    vpx_codec_ctx_t vpx;
    int vpx_ok;
    uint32_t seq;
    Slot slot[SLOTS];
    Slot last;         // the newest picture, for packets that show none
    Packet queue[QUEUED];
    int qhead, qn;
} Dec;

static Dec decs[DECODERS];
static PlatMutex lock = PLAT_MUTEX_INIT;   // every decoder call; held across a decode (one 960x540 frame, a few ms)

static Dec *dec_of(uint64_t key) {
    for (int i = 0; i < DECODERS; i++)
        if (decs[i].key == key) return &decs[i];
    return NULL;
}

static uint32_t cw(uint16_t w) { return (w + 1u) / 2; }
static uint32_t ch(uint16_t h) { return (h + 1u) / 2; }

// Copies a decoded I420 picture into s as NV12.
static void put_picture(Slot *s, const vpx_image_t *img) {
    uint16_t w = (uint16_t)img->d_w, h = (uint16_t)img->d_h;
    if (!s->nv12 || s->w != w || s->h != h) {
        free(s->nv12);
        s->nv12 = malloc((size_t)w * h + 2 * (size_t)cw(w) * ch(h));
        if (!s->nv12) rt_fatal("codec: out of memory for a %ux%u picture", w, h);
        s->w = w, s->h = h;
    }
    for (uint32_t y = 0; y < h; y++) memcpy(s->nv12 + (size_t)y * w, img->planes[0] + (size_t)y * img->stride[0], w);
    uint8_t *uv = s->nv12 + (size_t)w * h;
    for (uint32_t y = 0; y < ch(h); y++) {
        const uint8_t *u = img->planes[1] + (size_t)y * img->stride[1], *v = img->planes[2] + (size_t)y * img->stride[2];
        uint8_t *o = uv + (size_t)y * 2 * cw(w);
        for (uint32_t x = 0; x < cw(w); x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
    }
}

static void copy_slot(Slot *to, const Slot *from) {
    if (!from->nv12) return;
    size_t n = (size_t)from->w * from->h + 2 * (size_t)cw(from->w) * ch(from->h);
    if (!to->nv12 || to->w != from->w || to->h != from->h) {
        free(to->nv12);
        if (!(to->nv12 = malloc(n))) rt_fatal("codec: out of memory for a picture");
        to->w = from->w, to->h = from->h;
    }
    memcpy(to->nv12, from->nv12, n);
}

static void queue_drop(Dec *d) {
    for (int i = 0; i < d->qn; i++) free(d->queue[(d->qhead + i) % QUEUED].data);
    d->qhead = d->qn = 0;
}

// The oldest reserved buffer still waiting for a picture: pictures fill buffers in acquisition order.
static Slot *oldest_wait(Dec *d) {
    Slot *s = NULL;
    for (int i = 0; i < SLOTS; i++)
        if (d->slot[i].state == S_WAIT && (!s || (int32_t)(d->slot[i].seq - s->seq) < 0)) s = &d->slot[i];
    return s;
}

// Decodes one packet into the waiting buffer s (lock held). A packet that shows nothing leaves s waiting, except the
// last one: the player then gets the previous picture (or black) marked as the end. Returns the number of events to
// signal, stored in wake.
static int decode_into(Dec *d, Slot *s, const uint8_t *data, size_t size, int64_t pts, int end, uint64_t wake[WAITERS]) {
    const vpx_image_t *img = NULL, *f;
    if (d->vpx_ok && data && size) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK)
            LOG_FIRST("codec: VP9 packet of %zu bytes failed: %s", size, vpx_codec_error_detail(&d->vpx));
        vpx_codec_iter_t it = NULL;
        while ((f = vpx_codec_get_frame(&d->vpx, &it))) img = f;   // a superframe shows at most one; keep the last
    }
    int filled = 1;
    if (img) put_picture(s, img), copy_slot(&d->last, s);
    else if (end && d->last.nv12) copy_slot(s, &d->last);
    else if (end) {
        vpx_image_t black = {.d_w = 16, .d_h = 16};
        static uint8_t y[16 * 16], uv[8 * 8];
        if (!uv[0]) memset(y, 16, sizeof y), memset(uv, 128, sizeof uv);
        black.planes[0] = y, black.planes[1] = black.planes[2] = uv, black.stride[0] = 16, black.stride[1] = black.stride[2] = 8;
        put_picture(s, &black);
    } else filled = 0;
    TRACE("decode %zu bytes pts %lld%s: buffer %d %s", size, (long long)pts, end ? " end" : "", (int)(s - d->slot),
          filled ? "filled" : "still waiting (no picture shown)");
    LOG_FIRST("codec: first VP9 packet: %zu bytes, picture %ux%u", size, s->w, s->h);
    int n = 0;
    if (filled) {
        s->state = S_FILLED, s->pts = pts, s->eos = end;
        for (int k = 0; k < WAITERS; k++)
            if (s->waiter[k]) wake[n++] = s->waiter[k];
        memset(s->waiter, 0, sizeof s->waiter);
    }
    return n;
}

// Result nn::codec::video::Initialize(), Finalize(): nothing to open on the host
static void ov_video_init(Cpu *c) { ret_x(c, 0); }
static void ov_video_fini(Cpu *c) { ret_x(c, 0); }

static void vp9_init(Cpu *c, uint64_t key, unsigned w, unsigned h) {
    plat_mutex_lock(&lock);
    Dec *d = dec_of(key);
    if (!d) d = dec_of(0);
    if (!d) rt_fatal("codec: more than %d VP9 decoders", DECODERS);
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    for (int i = 0; i < SLOTS; i++) free(d->slot[i].nv12);
    free(d->last.nv12);
    queue_drop(d);
    memset(d, 0, sizeof *d);
    d->key = key;
    vpx_codec_dec_cfg_t cfg = {.threads = 2, .w = w, .h = h};
    d->vpx_ok = vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    plat_mutex_unlock(&lock);
    rt_log("codec: VP9 decoder %#llx %ux%u on the host (libvpx%s)", (unsigned long long)key, w, h,
           d->vpx_ok ? "" : " failed to start: pictures stay black");
    ret_x(c, 0);
}
// Result InitializeVp9Decoder(Vp9DecoderType *, void *work, size_t, size_t, u16 w, u16 h, u32, input, output fmt)
static void ov_vp9_init(Cpu *c) { vp9_init(c, arg_x(c, 0), arg_x(c, 4) & 0xffff, arg_x(c, 5) & 0xffff); }
// Result InitializeVp9Decoder(Vp9DecoderType *, size_t, size_t, u16 w, u16 h, u32, input, output fmt)
static void ov_vp9_init_nowork(Cpu *c) { vp9_init(c, arg_x(c, 0), arg_x(c, 3) & 0xffff, arg_x(c, 4) & 0xffff); }
static void ov_vp9_init_nowork_update(Cpu *c) { vp9_init(c, arg_x(c, 0), arg_x(c, 2) & 0xffff, arg_x(c, 3) & 0xffff); }

// Result FinalizeVp9Decoder(Vp9DecoderType *)
static void ov_vp9_fini(Cpu *c) {
    plat_mutex_lock(&lock);
    Dec *d = dec_of(arg_x(c, 0));
    if (d) {
        if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
        for (int i = 0; i < SLOTS; i++) free(d->slot[i].nv12);
        free(d->last.nv12);
        queue_drop(d);
        memset(d, 0, sizeof *d);
    }
    plat_mutex_unlock(&lock);
    ret_x(c, 0);
}

// Result FlushVp9Decoder(Vp9DecoderType *): packets not decoded yet are dropped. A picture already decoded into a
// reserved buffer stays: the player was told its fence is signalled (RequestSyncPointEvent), and withdrawing it would
// leave that fence unsatisfiable, as the picture is gone and no packet is left to decode another one.
static void ov_vp9_flush(Cpu *c) {
    plat_mutex_lock(&lock);
    Dec *d = dec_of(arg_x(c, 0));
    // The base build keeps the old rule (a decoded picture is dropped); only the update reserves it.
    if (d && strcmp(rt_game_target, "dkb458752"))
        for (int i = 0; i < SLOTS; i++)
            if (d->slot[i].state == S_FILLED) d->slot[i].state = S_WAIT;
    if (d) queue_drop(d);   // the packets not decoded yet
    plat_mutex_unlock(&lock);
    TRACE("flush");
    ret_x(c, 0);
}

// Result Start/StopVp9Decoder(Vp9DecoderType *), FinalizeDecoderImagePool(pool): nothing to do
static void ov_ok(Cpu *c) { ret_x(c, 0); }

// Result RegisterVp9DecoderBuffer(Vp9DecoderType *, u32 index, VideoDecoderImagePoolType *, u32)
static void ov_vp9_register(Cpu *c) {
    uint32_t i = (uint32_t)arg_x(c, 1);
    plat_mutex_lock(&lock);
    Dec *d = dec_of(arg_x(c, 0));
    if (d && i < SLOTS) d->slot[i].pool = arg_x(c, 2), d->slot[i].state = S_FREE;
    plat_mutex_unlock(&lock);
    if (!d || i >= SLOTS) rt_log("codec: RegisterVp9DecoderBuffer index %u of an unknown decoder or past %d", i, SLOTS);
    ret_x(c, 0);
}

// Result InitializeDecoderImagePool(pool, void *, size_t, const VideoDecoderImagePreference &, u32, CodecMode): the
// size the player reads back at +0x20 / +0x22
static void ov_pool_init(Cpu *c) {
    uint8_t *pool = arg_p(c, 0);
    const uint8_t *pref = arg_p(c, 3);
    uint16_t w, h;
    memcpy(&w, pref + 8, 2), memcpy(&h, pref + 10, 2);
    w++, h++;
    memcpy(pool + 0x20, &w, 2), memcpy(pool + 0x22, &h, 2);
    ret_x(c, 0);
}

// Result CalculateDecoderImagePoolBuffer(size_t *size, size_t *align, const preference &, u32 count),
// CalculateVp9DecoderWorkBuffer(size_t *size, size_t *align, size_t, u16 w, u16 h): the buffers hold nothing on the
// host (the pictures are host memory), so one page each
static void ov_calc_size(Cpu *c) {
    *(uint64_t *)arg_p(c, 0) = 0x1000;
    *(uint64_t *)arg_p(c, 1) = 0x1000;
    ret_x(c, 0);
}

static void clear_event(Cpu *c, uint64_t ev);

// A guest SystemEventType (manual clear) standing for the game object at `key`: the same one for the same object, so
// movie after movie reuses the events instead of filling the kernel's table of bound events. Guest thread only.
// ponytail: 64 objects, linear scan; the player has one decoder and 15 sync point events.
static uint64_t event_for(Cpu *c, uint64_t key) {
    static struct { uint64_t key, ev; } tab[64];
    static int n, victim;
    static PlatMutex tlock = PLAT_MUTEX_INIT;
    plat_mutex_lock(&tlock);
    uint64_t ev = 0;
    for (int i = 0; i < n && !ev; i++)
        if (tab[i].key == key) ev = tab[i].ev;
    if (!ev) {
        if (n == 64) {
            // The player allocates a new decoder and 15 new sync point events for every movie, at new addresses (the
            // Online-Match notice loops a background movie): the oldest object's event is handed to this one, already
            // bound, and cleared (RELEASE_PLAN 7.2: this was a FATAL after a few minutes in the notice).
            ev = tab[victim].ev;
            tab[victim].key = key;
            victim = (victim + 1) % 64;
            plat_mutex_unlock(&tlock);
            clear_event(c, ev);
            return ev;
        }
        ev = (uint64_t)(uintptr_t)mem_kernel_alloc(0x100, 16);
        tab[n].key = key, tab[n++].ev = ev;
    }
    plat_mutex_unlock(&tlock);
    kern_bind_system_event(c, ev, 0);   // once per event; leaves the event's handles in x0-x2
    return ev;
}

// Result GetVp9DecoderSubmissionReadyEvent(nn::os::SystemEventType **out, Vp9DecoderType *): signalled while the
// decoder can take a packet, which is when an output buffer is reserved for its picture. The player clears it after a
// submission comes back not ready, so AcquireVp9DecoderOutputBuffer signals it again (ov_vp9_acquire).
static void ov_vp9_ready_event(Cpu *c) {
    uint64_t *out = arg_p(c, 0), dec = arg_x(c, 1);
    uint64_t ev = event_for(c, dec);
    kern_signal_system_event(ev);
    *out = ev;
    TRACE("submission-ready event %#llx handed out (signalled)", (unsigned long long)ev);
    ret_x(c, 0);
}

// Result SubmitVp9DecoderInputBuffer(u64 *id, Vp9DecoderType *, const void *data, size_t size, TimeSpan pts, bool end):
// decoded at once into the oldest waiting buffer, else queued for the next one reserved; "not ready" once QUEUED
// packets wait. After the end of a guide movie the player flushes and starts over with the buffers it still holds,
// submitting its third packet before it releases one: without the queue that packet was refused, nothing released a
// buffer again, and the window kept the movie's first picture, a flat yellow card.
static void ov_vp9_submit(Cpu *c) {
    uint64_t *id = arg_p(c, 0);
    const uint8_t *data = arg_p(c, 2);
    size_t size = arg_x(c, 3);
    int64_t pts = (int64_t)arg_x(c, 4);
    int end = (int)(arg_x(c, 5) & 1);
    plat_mutex_lock(&lock);
    Dec *d = dec_of(arg_x(c, 1));
    Slot *s = d && !d->qn ? oldest_wait(d) : NULL;
    if (!s) {
        int queued = d && d->qn < QUEUED;
        if (queued) {
            Packet *q = &d->queue[(d->qhead + d->qn++) % QUEUED];
            *q = (Packet){size ? malloc(size) : NULL, size, pts, end};
            if (size && !q->data) rt_fatal("codec: out of memory for a %zu-byte packet", size);
            if (size) memcpy(q->data, data, size);
            if (id) *id = d->seq;
        }
        int waiting = d ? d->qn : 0;
        plat_mutex_unlock(&lock);
        TRACE("submit %zu bytes pts %lld%s: %s (%d queued)", size, (long long)pts, end ? " end" : "",
              queued ? "queued for the next reserved buffer" : "not ready", waiting);
        ret_x(c, queued ? 0 : R_NOT_READY);
        return;
    }
    uint64_t wake[WAITERS];
    int n = decode_into(d, s, data, size, pts, end, wake);
    if (id) *id = d->seq;
    plat_mutex_unlock(&lock);
    for (int k = 0; k < n; k++) kern_signal_system_event(wake[k]);
    ret_x(c, 0);
}

// Result AcquireVp9DecoderOutputBuffer(u32 *index, NativeSyncHandle *fence, Vp9DecoderType *): reserves a free buffer
// for the next picture, decodes queued packets into the waiting buffers; the fence carries the token
// RequestSyncPointEvent waits on
static void ov_vp9_acquire(Cpu *c) {
    uint32_t *index = arg_p(c, 0), *fence = arg_p(c, 1);
    uint64_t key = arg_x(c, 2);
    uint64_t wake[SLOTS * WAITERS];
    int n = 0;
    plat_mutex_lock(&lock);
    Dec *d = dec_of(key);
    int got = -1;
    for (int i = 0; d && i < SLOTS && got < 0; i++)
        if (d->slot[i].pool && d->slot[i].state == S_FREE) got = i;
    if (got >= 0) {
        Slot *s = &d->slot[got];
        s->state = S_WAIT, s->seq = d->seq++, s->eos = 0;
        memset(s->waiter, 0, sizeof s->waiter);
        *index = (uint32_t)got;
        if (fence) *fence = FENCE_TAG | (uint32_t)(d - decs) << 8 | (uint32_t)got;
        Slot *w;
        while (d->qn && (w = oldest_wait(d)) && n <= SLOTS * WAITERS - WAITERS) {
            Packet q = d->queue[d->qhead];
            d->qhead = (d->qhead + 1) % QUEUED, d->qn--;
            n += decode_into(d, w, q.data, q.size, q.pts, q.end, wake + n);
            free(q.data);
        }
    }
    plat_mutex_unlock(&lock);
    if (got < 0) rt_log("codec: AcquireVp9DecoderOutputBuffer with every registered buffer reserved");
    else TRACE("acquire: buffer %d", got);
    for (int k = 0; k < n; k++) kern_signal_system_event(wake[k]);
    // A reserved buffer lets the next packet in: wake a player that waits for the submission-ready event after a not
    // ready submission. Without this, the tutorial's guide movies stopped at their second picture.
    if (got >= 0) kern_signal_system_event(event_for(c, key));
    ret_x(c, got < 0 ? R_NOT_READY : 0);
}

// Result ReleaseVp9DecoderOutputBuffer(Vp9DecoderType *, u32 index, NativeSyncHandle)
static void ov_vp9_release(Cpu *c) {
    uint32_t i = (uint32_t)arg_x(c, 1);
    plat_mutex_lock(&lock);
    Dec *d = dec_of(arg_x(c, 0));
    if (d && i < SLOTS && d->slot[i].state != S_FREE) d->slot[i].state = S_FREE;
    plat_mutex_unlock(&lock);
    TRACE("release: buffer %u", i);
    ret_x(c, 0);
}

static void vp9_props(Cpu *c, uint64_t key, uint32_t i, int64_t *pts, uint8_t *end) {
    plat_mutex_lock(&lock);
    Dec *d = dec_of(key);
    Slot s = d && i < SLOTS ? d->slot[i] : (Slot){0};
    plat_mutex_unlock(&lock);
    for (int k = 0; k < 4; k++) {
        uint16_t *p = arg_p(c, k);
        if (p) *p = k & 1 ? s.h : s.w;
    }
    LOG_FIRST("codec: output properties: %ux%u", s.w, s.h);
    if (pts) *pts = s.pts;
    if (end) *end = (uint8_t)s.eos;
    ret_x(c, d ? 0 : R_NOT_READY);
}

static void ov_vp9_props(Cpu *c) {
    if (arg_p(c, 4)) *(uint32_t *)arg_p(c, 4) = 0;
    vp9_props(c, arg_x(c, 7), (uint32_t)arg_x(c, 8), arg_p(c, 5), arg_p(c, 6));
}

// The newer ABI omits ColorSpace, moving the decoder and index back into x6/x7 instead of x7/the stack.
static void ov_vp9_props_update(Cpu *c) {
    vp9_props(c, arg_x(c, 6), (uint32_t)arg_x(c, 7), arg_p(c, 4), arg_p(c, 5));
}

// Result ReadDecoderOutputImageNv12(const VideoDecoderImagePoolType *, u32 image, u16 x, u16 y, u16 w, u16 h,
// void *luma, size_t luma_pitch, size_t luma_size, void *chroma, size_t chroma_pitch, size_t chroma_size)
static void ov_read_nv12(Cpu *c) {
    uint64_t pool = arg_x(c, 0);
    uint32_t x0 = arg_x(c, 2) & 0xffff, y0 = arg_x(c, 3) & 0xffff, w = arg_x(c, 4) & 0xffff, h = arg_x(c, 5) & 0xffff;
    uint8_t *ly = arg_p(c, 6), *luv = arg_p(c, 9);
    size_t yp = arg_x(c, 7), ys = arg_x(c, 8), uvp = arg_x(c, 10), uvs = arg_x(c, 11);
    plat_mutex_lock(&lock);
    const Slot *s = NULL;
    for (int k = 0; k < DECODERS && !s; k++)
        for (int i = 0; decs[k].key && i < SLOTS && !s; i++)
            if (decs[k].slot[i].pool == pool && decs[k].slot[i].nv12) s = &decs[k].slot[i];
    if (s) {
        uint32_t cx = x0 < s->w ? x0 : s->w, cy = y0 < s->h ? y0 : s->h;
        uint32_t rw = w < s->w - cx ? w : s->w - cx, rh = h < s->h - cy ? h : s->h - cy;
        size_t n = rw < yp ? rw : yp;   // each row stays inside the caller's pitch and buffer
        for (uint32_t r = 0; r < rh && ly && r * yp + n <= ys; r++)
            memcpy(ly + r * yp, s->nv12 + (size_t)(cy + r) * s->w + cx, n);
        const uint8_t *uv = s->nv12 + (size_t)s->w * s->h;
        uint32_t uw = 2 * ((rw + 1) / 2), uh = (rh + 1) / 2, ux = 2 * (cx / 2), uy = cy / 2;
        n = uw < uvp ? uw : uvp;
        for (uint32_t r = 0; r < uh && luv && r * uvp + n <= uvs; r++)
            memcpy(luv + r * uvp, uv + (size_t)(uy + r) * 2 * cw(s->w) + ux, n);
    }
    int64_t pts = s ? s->pts : -1;
    plat_mutex_unlock(&lock);
    LOG_FIRST("codec: first NV12 read %ux%u (%s)", w, h, s ? "a decoded picture" : "no picture for this pool");
    TRACE("read pool %#llx: pts %lld", (unsigned long long)pool, (long long)pts);
    ret_x(c, 0);
}

// NativeImageHandle GetDecoderImagePoolNativeImageHandle(const VideoDecoderImagePoolType *, u32): no GPU image
static void ov_pool_image(Cpu *c) {
    LOG_FIRST("codec: GetDecoderImagePoolNativeImageHandle %p #%u: no GPU image (the player's NV12 path is served)",
              arg_p(c, 0), (unsigned)arg_x(c, 1));
    ret_x(c, 0);
}

// ---- nn::syncpt::sdk: GPU sync point events (the decoder's output fences), not in the SDK's loaded modules. Each is a
// guest system event; a request is signalled at once, as the host has no sync point to wait for.
// Result CreateSyncPointEvent(SyncPointEventType *)
static void ov_sp_create(Cpu *c) {
    event_for(c, arg_x(c, 0));
    ret_x(c, 0);
}
// void InitializeMultiWaitHolder(nn::os::MultiWaitHolderType *, SyncPointEventType *): the system event's holder
static void ov_sp_holder(Cpu *c) {
    static uint64_t init;
    if (!init && !(init = ldr_symbol("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE")))
        rt_fatal("codec: nn::os::InitializeMultiWaitHolder(SystemEventType *) is not loaded");
    uint64_t holder = arg_x(c, 0), ev = event_for(c, arg_x(c, 1));
    rt_call(c, init, 2, holder, ev);
}
// bool TryWaitSyncPointEvent(const SyncPointEventType *)
static void ov_sp_trywait(Cpu *c) {
    static uint64_t trywait;
    if (!trywait && !(trywait = ldr_symbol("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE")))
        rt_fatal("codec: nn::os::TryWaitSystemEvent is not loaded");
    uint64_t ev = event_for(c, arg_x(c, 0));
    ret_x(c, rt_call(c, trywait, 1, ev) & 1);
}
static void clear_event(Cpu *c, uint64_t ev) {
    static uint64_t clear;
    if (!clear && !(clear = ldr_symbol("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE")))
        rt_fatal("codec: nn::os::ClearSystemEvent is not loaded");
    rt_call(c, clear, 1, ev);
}

// void RequestSyncPointEvent(SyncPointEventType *, const SyncPointHandle &): re-arms the event (cleared, as the last
// request may have left it signalled); a decoder fence (FENCE_TAG) fires when its buffer holds a picture, anything
// else at once, as the host has no sync point to wait for
static void ov_sp_request(Cpu *c) {
    const uint32_t *h = arg_p(c, 1);
    uint32_t tok = h ? *h : 0;
    uint64_t ev = event_for(c, arg_x(c, 0));
    clear_event(c, ev);
    int now = 1;
    if ((tok & 0xffff0000u) == FENCE_TAG && (tok >> 8 & 0xff) < DECODERS && (tok & 0xff) < SLOTS) {
        plat_mutex_lock(&lock);
        Slot *s = &decs[tok >> 8 & 0xff].slot[tok & 0xff];
        if (s->state == S_WAIT)
            for (int k = 0; k < WAITERS; k++)
                if (!s->waiter[k] || s->waiter[k] == ev) {
                    s->waiter[k] = ev, now = 0;
                    break;
                }
        plat_mutex_unlock(&lock);
    }
    TRACE("sync point request %#x: %s", tok, now ? "signalled at once" : "waits for its picture");
    if (now) kern_signal_system_event(ev);
}
// bool TryWaitSyncPoint(const SyncPointHandle &): the poll form of the request above, which the guide-movie player
// reaches through nn::gs::TryWaitNativeSync when it advances its queue of shown pictures. Eglantine
// (its nvdrv syncpoint code, the NativeSyncHandle handling) has a NativeSyncHandle carry the sync point and the value
// its frame is finished at; the host has no sync point, so a decoder fence answers from its own buffer and anything else at once.
static void ov_sp_trywait_point(Cpu *c) {
    const uint32_t *h = arg_p(c, 0);
    uint32_t tok = h ? *h : 0;
    int now = 1;
    if ((tok & 0xffff0000u) == FENCE_TAG && (tok >> 8 & 0xff) < DECODERS && (tok & 0xff) < SLOTS) {
        plat_mutex_lock(&lock);
        now = decs[tok >> 8 & 0xff].slot[tok & 0xff].state != S_WAIT;   // the rule of RequestSyncPointEvent
        plat_mutex_unlock(&lock);
    }
    TRACE("sync point try wait %#x: %s", tok, now ? "signalled" : "waits for its picture");
    ret_x(c, now);
}
// void CancelSyncPointEvent(SyncPointEventType *): the request is dropped and the event cleared
static void ov_sp_cancel(Cpu *c) {
    uint64_t ev = event_for(c, arg_x(c, 0));
    plat_mutex_lock(&lock);
    for (int k = 0; k < DECODERS; k++)
        for (int i = 0; i < SLOTS; i++)
            for (int j = 0; j < WAITERS; j++)
                if (decs[k].slot[i].waiter[j] == ev) decs[k].slot[i].waiter[j] = 0;
    plat_mutex_unlock(&lock);
    clear_event(c, ev);
}
// void DestroySyncPointEvent(SyncPointEventType *): the event stays bound for the next Create on the same object
static void ov_sp_nop(Cpu *c) { (void)c; }

HOST_OVERRIDE("_ZN2nn5codec5video10InitializeEv", ov_video_init);
HOST_OVERRIDE("_ZN2nn5codec5video8FinalizeEv", ov_video_fini);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_"
              "24VideoDecoderOutputFormatE", ov_vp9_init);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_"
              "24VideoDecoderOutputFormatE", ov_vp9_init_nowork);
HOST_OVERRIDE("_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_"
              "24VideoDecoderOutputFormatE", ov_vp9_init_nowork_update);
HOST_OVERRIDE("_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_fini);
HOST_OVERRIDE("_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_ok);
HOST_OVERRIDE("_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_ok);
HOST_OVERRIDE("_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_flush);
HOST_OVERRIDE("_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj",
              ov_vp9_register);
HOST_OVERRIDE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImage"
              "PreferenceEjNS0_9CodecModeE", ov_pool_init);
HOST_OVERRIDE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", ov_ok);
HOST_OVERRIDE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", ov_calc_size);
HOST_OVERRIDE("_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt", ov_calc_size);
HOST_OVERRIDE("_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE",
              ov_vp9_ready_event);
HOST_OVERRIDE("_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", ov_vp9_submit);
HOST_OVERRIDE("_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE",
              ov_vp9_acquire);
HOST_OVERRIDE("_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE",
              ov_vp9_release);
HOST_OVERRIDE("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_"
              "14Vp9DecoderTypeEj", ov_vp9_props);
HOST_OVERRIDE("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj",
              ov_vp9_props_update);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", ov_read_nv12);
HOST_OVERRIDE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", ov_pool_image);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", ov_sp_create);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE",
              ov_sp_holder);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", ov_sp_trywait);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk16TryWaitSyncPointERKNS1_15SyncPointHandleE", ov_sp_trywait_point);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE",
              ov_sp_request);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", ov_sp_cancel);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", ov_sp_nop);
