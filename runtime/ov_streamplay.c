// nn::streamplay: the Switch 2's streaming of a game to other displays and their controllers (guests). The SDK opens
// it over IPC and the Switch 2 video service (nn::vi3::sdk::Initialize dereferences a null session in the lifted SDK).
// Kirby Air Riders imports 22 functions and calls CreateIndividualDisplays(3) at boot. The host has no second device and
// no network session, so streamplay is "no session, no guests": setup calls succeed and do nothing, a session never
// opens, connection state reads disconnected, guest displays and pads do not exist, guest audio is accepted and dropped
// (so nothing ever waits for room in it).

#include <stdint.h>
#include <string.h>

#include "host.h"

// A failure of the streamplay module for "no session"; the game only tests it against 0, the exact code is unverified.
#define STREAMPLAY_R_NO_SESSION ((uint32_t)(0x1d2 | (1 << 9)))

static void sp_nothing(Cpu *c) { (void)c; }
static void sp_ok(Cpu *c) { ret_x(c, 0); }
static void sp_no_session(Cpu *c) { ret_x(c, STREAMPLAY_R_NO_SESSION); }
static void sp_zero(Cpu *c) { ret_x(c, 0); }

// Result GetConnectionState(ConnectionState *out): 0, not connected (unverified enum value)
static void sp_connection_state(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, 0);
}

// os::SystemEventType *GetConnectionStateChangedEvent(): one event in guest memory, bound to a host event once and
// never signalled. 0x100 bytes covers SystemEventType with room to spare.
static uint64_t conn_event;
static void sp_connection_event(Cpu *c) {
    if (!__atomic_load_n(&conn_event, __ATOMIC_ACQUIRE)) {
        uint64_t ev = (uint64_t)(uintptr_t)mem_kernel_alloc(0x100, 16);
        kern_bind_system_event(c, ev, 0);
        uint64_t expected = 0;
        if (!__atomic_compare_exchange_n(&conn_event, &expected, ev, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rt_log("streamplay: a second connection event was bound and is left unused");
    }
    ret_x(c, __atomic_load_n(&conn_event, __ATOMIC_ACQUIRE));
}

// int GetGuestAudioStreamFreeFrameCount(GuestId): plenty of room; int WriteGuestAudioStream(GuestId, const void *,
// int frames): all of them written (and dropped)
static void sp_audio_free(Cpu *c) { ret_x(c, 4096); }
static void sp_audio_write(Cpu *c) { ret_x(c, (uint64_t)(uint32_t)arg_x(c, 2)); }

HOST_OVERRIDE("_ZN2nn10streamplay10InitializeENS0_14StreamPlayTypeENS0_13StreamingModeEPvm", sp_ok);
HOST_OVERRIDE("_ZN2nn10streamplay8FinalizeEv", sp_nothing);
HOST_OVERRIDE("_ZN2nn10streamplay24CreateIndividualDisplaysEi", sp_nothing);
HOST_OVERRIDE("_ZN2nn10streamplay21GetNativeWindowHandleENS0_7GuestIdE", sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay31OpenSessionWithMatchmakeSupportEPNS0_11SessionInfoERKNS0_15MatchmakeConfigE",
              sp_no_session);
HOST_OVERRIDE("_ZN2nn10streamplay14StartStreamingEv", sp_no_session);
HOST_OVERRIDE("_ZN2nn10streamplay12CloseSessionEv", sp_nothing);
HOST_OVERRIDE("_ZN2nn10streamplay18GetConnectionStateEPNS0_15ConnectionStateE", sp_connection_state);
HOST_OVERRIDE("_ZN2nn10streamplay30GetConnectionStateChangedEventEv", sp_connection_event);
HOST_OVERRIDE("_ZN2nn10streamplay19GetGuestPadStyleSetERKNS0_10GuestPadIdE", sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyLeftStateEEEiRKNS0_10GuestPadIdE",
              sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadJoyRightStateEEEiRKNS0_10GuestPadIdE",
              sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyDualStateEEEiRKNS0_10GuestPadIdE",
              sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadHandheldStateEEEiRKNS0_10GuestPadIdE",
              sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadFullKeyStateEEEiRKNS0_10GuestPadIdE",
              sp_zero);
HOST_OVERRIDE("_ZN2nn10streamplay29SetSupportedGuestNpadStyleSetENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE",
              sp_nothing);
HOST_OVERRIDE("_ZN2nn10streamplay23SetGuestNpadJoyHoldTypeENS_3hid15NpadJoyHoldTypeE", sp_nothing);
HOST_OVERRIDE("_ZN2nn10streamplay33GetGuestAudioStreamFreeFrameCountENS0_7GuestIdE", sp_audio_free);
HOST_OVERRIDE("_ZN2nn10streamplay21WriteGuestAudioStreamENS0_7GuestIdEPKvi", sp_audio_write);
HOST_OVERRIDE("_ZN2nn10streamplay52SetLayerNextHdrStandardContentLuminanceRangeMetadataENS0_7GuestIdEtt", sp_nothing);
HOST_OVERRIDE("_ZN2nn10streamplay52SetLayerNextHdrExtendedContentLuminanceRangeMetadataENS0_7GuestIdEtt", sp_nothing);
