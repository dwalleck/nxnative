// Per-present phase timing and byte counts for the Windows replay path.
//
// DM_M4_PHASES=1 prints one `[m4 phase]` line per presented frame with the
// time spent in each replay phase and the bytes each one moved, so a slow
// frame can be attributed instead of guessed. It is off by default: QPC calls
// and a log line per present are not free.
//
// DM_M4_TARGET_DIFF=0 is a diagnostic A/B, not a fix. It turns the
// render-target diff into a whole-plane writeback: the compare is dropped and
// the readback bytes are copied back unconditionally. The guest still sees
// every byte it would have seen, but the work moves from compare to copy.
#ifndef DM_WINDOWS_M4_PHASE_H
#define DM_WINDOWS_M4_PHASE_H

#include <stdint.h>

enum m4_phase_slot {
    M4_PHASE_DRAW,
    M4_PHASE_SUBMIT,
    M4_PHASE_WAIT,
    M4_PHASE_READBACK,
    M4_PHASE_DIFF,
    M4_PHASE_WRITEBACK,
    M4_PHASE_DESWIZZLE,
    M4_PHASE_SLOTS
};

enum m4_phase_bytes {
    M4_BYTES_READBACK,
    M4_BYTES_DIFF,
    M4_BYTES_WRITEBACK,
    M4_BYTES_DESWIZZLE,
    M4_BYTES_COUNTERS
};

void m4_phase_init(void);
int m4_phase_on(void);
int m4_phase_target_diff(void);
uint64_t m4_phase_ticks(void);
void m4_phase_add(int slot, uint64_t start);
void m4_phase_bytes(int counter, uint64_t bytes);
void m4_phase_draw(void);
void m4_phase_present(void);

#endif
