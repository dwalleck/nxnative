// Per-present phase timing and byte counts; see m4_phase.h for why this exists,
// for the two switches and for what DM_M4_TARGET_DIFF=0 does to the work.
#include "m4_phase.h"

#include <windows.h>
#include <stdio.h>

static int enabled;
static int target_diff = 1;
static uint64_t frequency;
static uint64_t ticks[M4_PHASE_SLOTS];
static uint64_t bytes[M4_BYTES_COUNTERS];
static uint64_t draws;
static uint64_t presents;

void m4_phase_init(void) {
    LARGE_INTEGER counter;
    if (QueryPerformanceFrequency(&counter)) frequency = (uint64_t)counter.QuadPart;
    char value[8];
    enabled = GetEnvironmentVariableA("DM_M4_PHASES", value, sizeof value) == 1 && value[0] == '1';
    if (GetEnvironmentVariableA("DM_M4_TARGET_DIFF", value, sizeof value) == 1 && value[0] == '0') target_diff = 0;
}

int m4_phase_on(void) { return enabled; }

int m4_phase_target_diff(void) { return target_diff; }

uint64_t m4_phase_ticks(void) {
    LARGE_INTEGER counter;
    return QueryPerformanceCounter(&counter) ? (uint64_t)counter.QuadPart : 0;
}

static double milliseconds(uint64_t value) {
    return frequency ? 1000.0 * (double)value / (double)frequency : 0.0;
}

void m4_phase_add(int slot, uint64_t start) {
    if (!enabled || slot < 0 || slot >= M4_PHASE_SLOTS) return;
    ticks[slot] += m4_phase_ticks() - start;
}

void m4_phase_bytes(int counter, uint64_t value) {
    if (!enabled || counter < 0 || counter >= M4_BYTES_COUNTERS) return;
    bytes[counter] += value;
}

void m4_phase_draw(void) {
    if (enabled) ++draws;
}

void m4_phase_present(void) {
    if (!enabled) return;
    // A frame with no draw still gives the gap between two presents, which is
    // the number the caller compares against present.tsv.
    printf("[m4 phase] present=%llu draws=%llu draw=%.1fms submit=%.1fms wait=%.1fms readback=%.1fms diff=%.1fms "
           "writeback=%.1fms deswizzle=%.1fms readback_bytes=%llu diff_bytes=%llu writeback_bytes=%llu "
           "deswizzle_bytes=%llu target_diff=%d\n",
           ++presents, draws, milliseconds(ticks[M4_PHASE_DRAW]), milliseconds(ticks[M4_PHASE_SUBMIT]),
           milliseconds(ticks[M4_PHASE_WAIT]),
           milliseconds(ticks[M4_PHASE_READBACK]), milliseconds(ticks[M4_PHASE_DIFF]),
           milliseconds(ticks[M4_PHASE_WRITEBACK]), milliseconds(ticks[M4_PHASE_DESWIZZLE]),
           bytes[M4_BYTES_READBACK], bytes[M4_BYTES_DIFF], bytes[M4_BYTES_WRITEBACK],
           bytes[M4_BYTES_DESWIZZLE], target_diff);
    for (int i = 0; i < M4_PHASE_SLOTS; ++i) ticks[i] = 0;
    for (int i = 0; i < M4_BYTES_COUNTERS; ++i) bytes[i] = 0;
    draws = 0;
}
