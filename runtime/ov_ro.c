// nn::ro: the SDK maps NROs (relocatable modules from the RomFS) through the ldr:ro service, which is not emulated;
// the lifted SDK code writes through a null session object when it tries. Kirby Air Riders calls nn::ro::Initialize
// early in nnMain and maps romfs:/Bin/Module/EffectScript.nro. That NRO is lifted ahead of time
// (recomp/gen.py --nro, generated/<version>/nro_layout.h), so loading it means mapping the pre-lifted image at its
// fixed base, not loading code at run time.
//
// The calls that only keep books (Initialize, Finalize, ProtectRelro, Register/UnregisterModuleInfo, GetBufferSize)
// succeed. LoadModule maps the image the game read from the RomFS onto its pre-lifted twin (ldr_map_nro: copy to the
// fixed base, relocate, seal) and runs its DT_INIT once; LookupModuleSymbol answers from that module's exports;
// UnloadModule leaves it mapped (its lifted code stays in the dispatch table either way).

#include <stdint.h>
#include <string.h>

#include "host.h"

// nn::Result nn::ro::Initialize() / Finalize()
static void ov_ro_ok(Cpu *c) { ret_x(c, 0); }

// nn::Result nn::ro::ProtectRelro(const void*, const void*, const void*, const void*, const void*): the host keeps
// guest memory read-write, so there is nothing to protect.

// nn::Result nn::ro::RegisterModuleInfo(RegistrationInfo *info, const void *nrr) / UnregisterModuleInfo(info): the
// NRR lists the hashes of NROs the game may load; the host only ever maps the NROs it lifted, so the list is not
// needed. The RegistrationInfo is the game's own storage and stays as the game left it.

// nn::Result nn::ro::GetBufferSize(size_t *out, const void *nro_image): the size of the .bss buffer LoadModule takes.
// A lifted NRO keeps its .bss in the reserved module range, so the game's buffer is not used; any small non-zero
// size lets the game's allocation succeed.
static void ov_ro_buffer_size(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0x1000;
    ret_x(c, 0);
}

// A failed lookup: a result of the ro module (22). The game only tests it against 0; the exact description the
// console returns for a missing symbol is unverified.
#define RO_R_NOT_FOUND ((uint32_t)(22 | (1 << 9)))
#define RO_MAX 8
static const RtLoaded *initialised[RO_MAX];   // NROs whose DT_INIT has run
static int ninitialised;

// nn::Result nn::ro::LoadModule(Module *out, const void *nro_image, void *bss_buffer, size_t bss_size, int bind_flag).
// The NRO header gives the image length at 0x18. Module's first 8 bytes take the module's guest base, which
// LookupModuleSymbol reads back; the game itself does not look inside Module (Kirby Air Riders, main+0xea120c).
static void ov_ro_load(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    const uint8_t *image = arg_p(c, 1);
    char lr[96];
    if (!out || !image) rt_fatal("nn::ro::LoadModule from %s: null module or image", ldr_describe(c->x[30] - 4, lr,
                                                                                                    sizeof lr));
    uint32_t len;
    memcpy(&len, image + 0x18, 4);
    const RtLoaded *m = ldr_map_nro(image, len);
    int done = 0;
    for (int i = 0; i < ninitialised; i++) done |= initialised[i] == m;
    if (!done) {
        if (ninitialised == RO_MAX) rt_fatal("nn::ro::LoadModule: more than %d NROs", RO_MAX);
        initialised[ninitialised++] = m;
        rt_log("ro: LoadModule %s from %s: DT_INIT at %#llx", m->name, ldr_describe(c->x[30] - 4, lr, sizeof lr),
               (unsigned long long)m->init);
        if (m->init) rt_call(c, m->init, 0);
    }
    out[0] = m->base;
    ret_x(c, 0);
}

// nn::Result nn::ro::LookupModuleSymbol(uintptr_t *out, const Module *module, const char *name)
static void ov_ro_lookup(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    const uint64_t *module = arg_p(c, 1);
    const char *name = arg_p(c, 2);
    if (!out || !module || !name) {
        ret_x(c, RO_R_NOT_FOUND);
        return;
    }
    const RtLoaded *m = NULL;
    for (int i = 0; i < ldr_count() && !m; i++)
        if (ldr_module(i)->base == module[0]) m = ldr_module(i);
    uint64_t a = m ? ldr_module_symbol(m, name) : 0;
    if (!a) {
        char lr[96];
        rt_log("ro: LookupModuleSymbol %s in %s from %s: not found", name, m ? m->name : "an unknown module",
               ldr_describe(c->x[30] - 4, lr, sizeof lr));
        ret_x(c, RO_R_NOT_FOUND);
        return;
    }
    *out = a;
    ret_x(c, 0);
}

// nn::Result nn::ro::UnloadModule(Module *module): the module stays mapped; loading it again reuses it.
static void ov_ro_unload(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn2ro10InitializeEv", ov_ro_ok);
HOST_OVERRIDE("_ZN2nn2ro8FinalizeEv", ov_ro_ok);
HOST_OVERRIDE("_ZN2nn2ro12ProtectRelroEPKvS2_S2_S2_S2_", ov_ro_ok);
HOST_OVERRIDE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ov_ro_ok);
HOST_OVERRIDE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ov_ro_ok);
HOST_OVERRIDE("_ZN2nn2ro13GetBufferSizeEPmPKv", ov_ro_buffer_size);
HOST_OVERRIDE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ov_ro_load);
HOST_OVERRIDE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ov_ro_unload);
HOST_OVERRIDE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ov_ro_lookup);
