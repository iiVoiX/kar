// Kirby Air Riders: why a std::mutex lock failed (RELEASE_PLAN 3.9). A race crashed once in std::mutex::lock
// (sdk _ZNSt3__15mutex4lockEv), which throws std::system_error when the sdk's pthread_mutex_lock returns non-zero.
// That function (sdk+0x153d30) returns EDEADLK (0x23) for an error-checking mutex (type byte +1 == 2) the calling
// thread already holds, EINVAL (0x16) for a type byte above 2 (a mutex that was never initialised or was freed),
// and EAGAIN (0xb) when a recursive mutex's count (+2) overflows. The patch set karmutexlog (KAR_MUTEX_LOG, on) logs
// each of these returns with the mutex, its first 16 bytes and the caller, so the next crash names its mutex. It only
// runs on those error returns, never on a normal lock.
#include "host.h"

#include <string.h>

#define SLOT_MUTEX 0x3f0

static void mutex_err_hook(Cpu *c) {
    static int n;
    if (n >= 32) return;   // a log line per failure, at most 32
    n++;
    const uint8_t *m = (const uint8_t *)(uintptr_t)c->x[19];
    uint8_t b[16];
    memcpy(b, m, sizeof b);
    uint64_t lr = 0;   // pthread_mutex_lock's saved return address (stp x29, x30, [sp, #-0x30]!; mov x29, sp)
    memcpy(&lr, (const void *)(uintptr_t)(c->x[29] + 8), sizeof lr);
    rt_log("mutex: pthread_mutex_lock fails with %#x on %#llx (type %u, count %u), bytes "
           "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x, called from %#llx",
           (unsigned)c->x[0], (unsigned long long)c->x[19], b[1], (unsigned)(b[2] | b[3] << 8), b[0], b[1], b[2], b[3],
           b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
           (unsigned long long)(lr & 0xffffffffffull));
}

void kar_diag_init(void) {
    if (rt_patch_set("karmutexlog") < 0) return;
    rt_register_host((uint64_t)(uintptr_t)rt_patch_page() + SLOT_MUTEX, mutex_err_hook);
}
