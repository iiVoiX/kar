// nn::movie on the Switch 2 SDK (Kirby Air Riders), as opposed to the older movie::Decoder API of runtime/ov_movie.c.
//
// The library's C allocator (MovieMalloc, MovieCalloc, MovieRealloc, MovieFree, ... in sdk) allocates through a
// per-thread context: nn::os::GetTlsValue on a slot nn::movie::Initialize allocates, then the context's allocator kind
// at +8. The library sets that slot on its own threads (looper and codec thread entries) and in
// MediaExtractorImpl::Initialize, but Kirby also calls MovieCalloc straight from a game worker thread (main+0xc964b0).
// On the console that worker evidently ran a MediaExtractor::Initialize before; here the job lands on another thread
// often enough that MovieCalloc reads the context at NULL+8. So: every non-zero value stored in the movie slot is
// remembered, and a thread entering a Movie* allocator function with the slot still empty gets that context first.
// The context is the allocator the library was initialised with (one per process in Kirby), the same one any of its
// threads uses. (Superseded: see private_ctx below; the last set value is still recorded for diagnostics.)

#include <stdint.h>
#include <string.h>

#include "host.h"

static uint64_t movie_slot_addr;   // guest address of the nn::os::TlsSlot the movie library allocated, 0 until found
static volatile uint64_t movie_ctx;   // the last non-zero value stored in that slot
static uint64_t fn_get_tls, fn_set_tls;

// MovieCalloc starts "adrp x20, page; ... ldr x20, [x20, #off]" for the GOT entry holding the slot's address
// (sdk+0x442078 and +0x442084 in Kirby 1.3.3): decoded rather than hard-coded so a different build finds it too.
static void find_slot(void) {
    if (movie_slot_addr) return;
    uint64_t f = ldr_symbol("MovieCalloc");
    fn_get_tls = ldr_symbol("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE");
    fn_set_tls = ldr_symbol("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm");
    if (!f || !fn_get_tls || !fn_set_tls) return;
    for (uint64_t p = f; p < f + 0x40; p += 4) {
        uint32_t i = *(const uint32_t *)(uintptr_t)p;
        if ((i & 0x9f00001f) != 0x90000014) continue;   // adrp x20
        int64_t imm = ((int64_t)((i >> 5) & 0x7ffff) << 2 | ((i >> 29) & 3)) << 12;
        imm = imm << 31 >> 31;   // sign-extend the 33-bit page offset
        uint64_t page = (p & ~0xfffull) + (uint64_t)imm;
        for (uint64_t q = p + 4; q < p + 0x20; q += 4) {
            uint32_t j = *(const uint32_t *)(uintptr_t)q;
            if ((j & 0xffc003ff) == 0xf9400294) {   // ldr x20, [x20, #imm12*8]
                uint64_t got = page + ((j >> 10) & 0xfff) * 8;
                movie_slot_addr = *(const uint64_t *)(uintptr_t)got;
                rt_log("movie: allocator context slot at %#llx (GOT %#llx)", (unsigned long long)movie_slot_addr,
                       (unsigned long long)got);
                return;
            }
        }
        return;
    }
}

static void obs_set_tls(Cpu *c) {
    find_slot();
    if (movie_slot_addr && c->x[1] && (uint32_t)c->x[0] == *(const uint32_t *)(uintptr_t)movie_slot_addr)
        __atomic_store_n(&movie_ctx, c->x[1], __ATOMIC_RELEASE);
    rt_original(c);
}

// The context lent to such threads: allocator kind 3 (sdk+0x441044's jump table: 1 StandardAllocator, 2 ExpHeap,
// 3 malloc/free, 4 custom functions), so their blocks come from the SDK's own malloc and go back to its free. Lending
// the last context the library set instead pointed the thread at a MediaExtractorImpl's allocator, which the extractor's
// end destroyed: a later MovieFree on that thread read a NULL heap (CentralHeap::Free) after A on the title screen.
static uint64_t private_ctx[2] = {0, 3};

static void ensure_ctx(Cpu *c) {
    find_slot();
    uint64_t ctx = (uint64_t)(uintptr_t)private_ctx;
    if (!movie_slot_addr) return;
    uint64_t save[8];
    memcpy(save, c->x, sizeof save);
    uint32_t slot = *(const uint32_t *)(uintptr_t)movie_slot_addr;
    if (!rt_call(c, fn_get_tls, 1, (uint64_t)slot)) {
        static volatile int32_t n;
        if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 4) {
            char lr[96];
            rt_log("movie: allocator called with no context on this thread (from %s); lending %#llx",
                   ldr_describe(c->x[30] - 4, lr, sizeof lr), (unsigned long long)ctx);
        }
        rt_call(c, fn_set_tls, 2, (uint64_t)slot, ctx);
    }
    memcpy(c->x, save, sizeof save);
}

#define MOVIE_ALLOC_OBS(name)                                                                                          \
    static void obs_##name(Cpu *c) { ensure_ctx(c); rt_original(c); }                                                  \
    HOST_OBSERVER(#name, obs_##name);

MOVIE_ALLOC_OBS(MovieMalloc)
MOVIE_ALLOC_OBS(MovieCalloc)
MOVIE_ALLOC_OBS(MovieRealloc)
MOVIE_ALLOC_OBS(MovieFree)
MOVIE_ALLOC_OBS(MovieAlignedAlloc)
MOVIE_ALLOC_OBS(MovieStrdup)
MOVIE_ALLOC_OBS(MovieMallocUsableSize)
HOST_OBSERVER("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", obs_set_tls);
