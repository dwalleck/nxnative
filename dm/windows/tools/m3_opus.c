// Single-stream C ABI: decode with the software implementation in the loaded
// SDK. The HW object need not be large enough for the software state.
#include "m2_host.h"
#include <stdlib.h>

enum { WORK_SIZE, INIT, FINI, RATE, CHANNELS, RESET, DECODE, NOPS };
static const char *const names[NOPS] = {
    "_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii",
    "_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm",
    "_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE",
    "_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE",
    "_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE",
    "_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE",
    "_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm",
};
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static uintptr_t entries[NOPS];
static BOOL CALLBACK resolve(PINIT_ONCE token, PVOID arg, PVOID *ctx) {
    (void)token; (void)arg; (void)ctx;
    for (unsigned i = 0; i < NOPS; ++i) {
        entries[i] = dm_guest_sdk_symbol(names[i]);
        if (!entries[i]) dm_fail("Opus: SDK lacks %s", names[i]);
    }
    return TRUE;
}
static uint64_t invoke(unsigned op, const uint64_t args[8]) {
    struct dm_guest_result result;
    if (!InitOnceExecuteOnce(&once, resolve, NULL, NULL) || !dm_guest_call(entries[op], args, &result))
        dm_fail("Opus: could not enter %s", names[op]);
    return result.x0;
}
struct decoder {
    struct decoder *next;
    uintptr_t hw;
    uint64_t state[0x200 / 8]; // Same SDK software state allowance as hle_codec.c.
};
static SRWLOCK registry = SRWLOCK_INIT;
static struct decoder *decoders;
// The SDK caller owns the decoder lifetime and synchronizes calls on the
// same object. No registry lock may be held while re-entering guest code.
static struct decoder *lookup(uintptr_t hw, int remove) {
    if (remove) AcquireSRWLockExclusive(&registry); else AcquireSRWLockShared(&registry);
    struct decoder **p = &decoders;
    while (*p && (*p)->hw != hw) p = &(*p)->next;
    struct decoder *d = *p;
    if (d && remove) *p = d->next;
    if (remove) ReleaseSRWLockExclusive(&registry); else ReleaseSRWLockShared(&registry);
    if (!d) dm_fail("Opus: decoder %p was not initialized", (void *)hw);
    return d;
}
size_t opus_size(unsigned rate, unsigned channels) {
    return invoke(WORK_SIZE, (uint64_t[8]){rate, channels});
}
uint32_t opus_init(uintptr_t hw, unsigned rate, unsigned channels, uintptr_t work, size_t size) {
    if (!dm_guest_range((void *)hw, 8, 1) || !dm_guest_range((void *)work, size, 1)) dm_fail("Opus: invalid decoder or work buffer");
    struct decoder *d = calloc(1, sizeof(*d));
    if (!d) dm_fail("Opus: cannot allocate software decoder state");
    d->hw = hw;
    uint32_t result = (uint32_t)invoke(INIT, (uint64_t[8]){(uintptr_t)d->state, rate, channels, work, size});
    if (result) { free(d); return result; }
    AcquireSRWLockExclusive(&registry);
    for (const struct decoder *p = decoders; p; p = p->next) if (p->hw == hw) dm_fail("Opus: decoder initialized twice");
    d->next = decoders;
    decoders = d;
    // The inlined SDK IsInitialized reads the hardware state's first word.
    // Use a guest-owned opaque identity; never publish the host decoder pointer.
    *(uintptr_t *)hw = work;
    ReleaseSRWLockExclusive(&registry);
    return 0;
}
void opus_fini(uintptr_t hw) {
    struct decoder *d = lookup(hw, 1);
    invoke(FINI, (uint64_t[8]){(uintptr_t)d->state});
    if (!dm_guest_range((void *)hw, sizeof(uintptr_t), 1)) dm_fail("Opus: invalid decoder on finalize");
    *(uintptr_t *)hw = 0;
    free(d);
}
uint32_t opus_rate(uintptr_t hw) { return (uint32_t)invoke(RATE, (uint64_t[8]){(uintptr_t)lookup(hw, 0)->state}); }
uint32_t opus_channels(uintptr_t hw) { return (uint32_t)invoke(CHANNELS, (uint64_t[8]){(uintptr_t)lookup(hw, 0)->state}); }
void opus_reset(uintptr_t hw) { invoke(RESET, (uint64_t[8]){(uintptr_t)lookup(hw, 0)->state}); }
uint32_t opus_decode(uintptr_t hw, uintptr_t consumed, uintptr_t samples, uintptr_t out, size_t cap, uintptr_t in, size_t size) {
    if (!dm_guest_range((void *)consumed, 8, 1) || !dm_guest_range((void *)samples, 4, 1) ||
        !dm_guest_range((void *)out, cap, 1) || !dm_guest_range((void *)in, size, 0)) dm_fail("Opus: invalid packet or output span");
    return (uint32_t)invoke(DECODE, (uint64_t[8]){(uintptr_t)lookup(hw, 0)->state, consumed, samples, out, cap, in, size});
}
