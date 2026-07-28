#include "diagnostics.h"

#include <Arduino.h>

#include "command_queue.h"
#include "od_log.h"

#ifndef TARGET_ESP32
#include <malloc.h>

extern "C" {
// Defined in the nRF5 linker script; the core's _sbrk() bounds the heap with the
// same two symbols. Taken directly rather than through the core's
// utility/debug.h (dbgHeapTotal/dbgHeapUsed) so this file does not depend on the
// internals of a vendor fork that has already diverged from upstream elsewhere.
extern unsigned char __HeapBase[];
extern unsigned char __HeapLimit[];
}
#endif

// Low-water mark of free heap, nRF only. ESP32 keeps a true cross-task minimum
// in the allocator itself (ESP.getMinFreeHeap()), which is strictly better than
// anything sampled from loop() -- it catches a trough between two samples, this
// does not. On nRF there is no such counter, so a sampled approximation is the
// honest best available: it under-reports transient dips, and the line below
// says so by construction rather than pretending otherwise.
#ifndef TARGET_ESP32
static uint32_t s_minFreeEver = UINT32_MAX;
#endif

// Heartbeat cadence. Long enough to be quiet next to the ~300 progress lines a
// full push already emits, short enough that the gap it leaves on a hang
// localises the stall to a 5 s window.
static const uint32_t DIAG_HEARTBEAT_MS = 5000;

static uint32_t s_lastHeartbeatMs = 0;
static bool     s_wasConnected = false;
static uint32_t s_loopCount = 0;
static uint32_t s_loopCountAtLastBeat = 0;

static uint32_t diagHeapFree(void) {
#ifdef TARGET_ESP32
    return (uint32_t)ESP.getFreeHeap();
#else
    const uint32_t total = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__HeapBase);
    const uint32_t used  = (uint32_t)mallinfo().uordblks;
    return (used > total) ? 0u : (total - used);
#endif
}

void diagHeapSample(void) {
#ifndef TARGET_ESP32
    const uint32_t freeNow = diagHeapFree();
    if (freeNow < s_minFreeEver) {
        s_minFreeEver = freeNow;
    }
#endif
}

void diagLogHeap(const char *tag) {
    diagHeapSample();
#ifdef TARGET_ESP32
    // largest= is the biggest single block still allocatable. Free-but-fragmented
    // heap reads healthy on free= and fails an allocation anyway, so the gap
    // between the two is the number that predicts a fragmentation failure.
    od_log_info("[mem] %s total=%lu free=%lu minfree=%lu largest=%lu",
                tag,
                (unsigned long)ESP.getHeapSize(),
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());
#else
    // No largest-block query exists for newlib malloc here, and finding one by
    // probing would mean real allocations on a path that runs immediately before
    // a panel refresh -- the worst possible moment to perturb the heap. Reported
    // as n/a rather than guessed.
    od_log_info("[mem] %s total=%lu free=%lu minfree=%lu largest=n/a",
                tag,
                (unsigned long)((uintptr_t)__HeapLimit - (uintptr_t)__HeapBase),
                (unsigned long)diagHeapFree(),
                (unsigned long)(s_minFreeEver == UINT32_MAX ? 0u : s_minFreeEver));
#endif
}

void diagHeartbeat(bool connected) {
    s_loopCount++;

    if (!connected) {
        // Re-arm on the edge so the first beat of a session lands a full interval
        // after connect rather than immediately, and a short session that never
        // reaches 5 s stays silent instead of emitting a beat per connect.
        s_wasConnected = false;
        return;
    }

    const uint32_t now = millis();
    if (!s_wasConnected) {
        s_wasConnected = true;
        s_lastHeartbeatMs = now;
        s_loopCountAtLastBeat = s_loopCount;
        return;
    }
    if ((uint32_t)(now - s_lastHeartbeatMs) < DIAG_HEARTBEAT_MS) {
        return;
    }
    s_lastHeartbeatMs = now;

    const uint32_t loops = s_loopCount - s_loopCountAtLastBeat;
    s_loopCountAtLastBeat = s_loopCount;

    diagHeapSample();
    // loops= separates the two ways a tag can go quiet: a beat that arrives with
    // loops in the thousands is a healthy idle link, a beat that arrives with
    // loops near zero is a starved loop() (something else hogging the CPU), and
    // no beat at all is a blocked one. Those need different fixes, and until now
    // the log could not tell them apart.
    od_log_debug("[hb] up=%lus loops=%lu rxQ=%u txQ=%u free=%lu drops=%lu",
                 (unsigned long)(now / 1000),
                 (unsigned long)loops,
                 (unsigned)bleRxQueueDepth(),
                 (unsigned)bleTxQueueDepth(),
                 (unsigned long)diagHeapFree(),
                 (unsigned long)od_log_dropped_total());
}
