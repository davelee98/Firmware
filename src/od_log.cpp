#include "od_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Implemented in main.cpp: RTC-persisted wake cycle count on ESP32, always 0 on nRF52840.
uint32_t getDeepSleepCount();

// Log output destination, set once by od_log_init(). Stays NULL if
// od_log_init() is never called (e.g. DISABLE_USB_SERIAL builds), in which
// case all log calls become no-ops.
static Stream *s_port = NULL;

// Optional "is a host actually listening" predicate, set by main.cpp right after
// od_log_init(). Without it, availableForWrite() == 0 is ambiguous: it reads the
// same whether the FIFO is full (host attached but not draining) or the port is
// dark (nobody attached). Counting the second case as dropped lines would have a
// tag report "dropped 812403 line(s)" the moment a terminal finally attaches --
// a true number that says nothing. NULL means "assume ready", which keeps
// behaviour sane for any port that has no such notion.
static bool (*s_readyHook)(void) = NULL;

// Lines discarded for want of FIFO space since the last successful report.
// Written from more than one task (see od_emit), hence the atomics.
static uint32_t s_dropped = 0;
static uint32_t s_droppedTotal = 0;

// Both ports we log over have a 256-byte TX FIFO: CFG_TUD_CDC_TX_BUFSIZE on the
// nRF CDC, and HWCDC's setTxBufferSize(256) default on ESP32. That number is
// what shapes everything below.
//
// A pure "drop unless the whole line fits right now" rule is unusable at that
// size. The longest line this firmware emits is ~210 bytes (a 192-byte hex body
// plus the ~18-byte timestamp prefix), so it only ever fits a near-empty FIFO --
// and during the ~300-line burst of a single image push the FIFO is rarely near
// empty even with a perfectly healthy host. The guard would spend its time
// deleting exactly the frame dumps it exists to preserve.
//
// So: wait briefly for room, then drop. On a healthy host the FIFO drains in
// about a millisecond and the wait never fires; on a stalled one the cost is
// bounded instead of infinite, which is the entire point. Two of these back to
// back (lock, then room) cap a single line at ~40 ms against the unbounded spin
// in Adafruit_USBD_CDC::write().
static const uint32_t OD_LOG_LOCK_WAIT_MS = 20;

// How long to wait for TX room before giving up on a line. Tunable because the
// right answer differs by port, and by a lot:
//
//   USB CDC (default 20 ms) -- a stalled host never resumes on its own, so the
//   wait is pure loss. 300 lines x a long wait would itself become a multi-second
//   stall, which is the thing being fixed.
//
//   UART (250 ms, set by main.cpp) -- provably cannot stall indefinitely: uartBegin
//   hardwires flow_ctrl = UART_HW_FLOWCTRL_DISABLE, so no external signal can stop
//   the transmitter and the ring always drains at the baud rate. Waiting is
//   therefore backpressure, not a hang risk, and the -extuart envs exist precisely
//   to capture complete logs. 250 ms drains ~2.8 KB at 115200; a single ~210-byte
//   line takes ~18 ms, so the default 20 ms would have left almost no slack.
static uint32_t s_roomWaitMs = 20;

// Longest text od_emit() will hand the port, leaving headroom inside the
// 256-byte FIFO. Anything longer is truncated rather than dropped: a clipped
// hex dump still says which opcode and how many bytes, a missing line says
// nothing.
static const size_t OD_LOG_MAX_TEXT = 232;

// Serialises the check-then-write, which is otherwise a real race: this logger
// has two producers, loop() and the stack callback task (onConnectCb /
// onDisconnectCb in ble_transport_nrf.cpp, and the RX hex line in
// command_queue.cpp). Without the lock both can observe room for their own line
// and then write concurrently, overfill the FIFO, and put the loser straight
// back into the blocking write this guard exists to remove. Taken with a
// timeout, never forever -- a log mutex that can block indefinitely would just
// relocate the hang.
static SemaphoreHandle_t s_txLock = NULL;

static const char level_chars[] = "EWID";

void od_log_init(Stream *port) {
    // Created here rather than statically: both targets run the scheduler by the
    // time setup() calls this. A NULL lock (allocation failed, or a caller that
    // logs before od_log_init) degrades to unserialised writes, which is the
    // pre-existing behaviour and still better than not logging.
    if (s_txLock == NULL) {
        s_txLock = xSemaphoreCreateMutex();
    }
    s_port = port;
}

void od_log_set_ready_hook(bool (*fn)(void)) {
    s_readyHook = fn;
}

void od_log_set_room_wait_ms(uint32_t ms) {
    s_roomWaitMs = ms;
}

uint32_t od_log_dropped_total(void) {
    return __atomic_load_n(&s_droppedTotal, __ATOMIC_RELAXED);
}

static inline void od_count_drop(void) {
    __atomic_fetch_add(&s_dropped, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_droppedTotal, 1u, __ATOMIC_RELAXED);
}

// True once the port has `need` bytes of TX space, false if the deadline passed
// first. delay(1) rather than a spin so the USB task -- the thing that actually
// drains the FIFO -- gets to run; a busy-wait here would starve the very
// progress it is waiting for.
static bool od_wait_for_room(int need) {
    const uint32_t start = millis();
    for (;;) {
        const int room = s_port->availableForWrite();
        if (room >= need) {
            return true;
        }
        if ((uint32_t)(millis() - start) >= s_roomWaitMs) {
            return false;
        }
        delay(1);
    }
}

// The single write choke point. NOTHING in this file may call s_port->print()
// directly: Adafruit_USBD_CDC::write() spins forever while DTR is asserted and
// the host is not draining the IN endpoint (no timeout, no iteration cap), and
// stock HWCDC::write() has the same shape. A blocked log write blocks loop(),
// and on nRF -- which has no watchdog -- that is a permanent freeze. Dropping a
// debug line is strictly better than bricking the tag.
//
// `text` is NUL-terminated; `newline` adds the CRLF that println() would.
static void od_emit(const char *text, bool newline) {
    if (s_port == NULL) {
        return;
    }
    // Dark port: discard without counting. Nobody is listening, so there is no
    // gap for a drop report to explain.
    if (s_readyHook != NULL && !s_readyHook()) {
        return;
    }

    if (s_txLock != NULL &&
        xSemaphoreTake(s_txLock, pdMS_TO_TICKS(OD_LOG_LOCK_WAIT_MS)) != pdTRUE) {
        od_count_drop();
        return;
    }

    // Truncation buffer is static and taken under the lock rather than put on the
    // stack: callers already carry a 256-byte line buffer of their own, and this
    // path runs on the SoftDevice callback task as well as loop(), so a second
    // ~233-byte frame on every log call is stack this firmware should not spend
    // for a case that only fires on an over-long line. (Nothing currently emitted
    // reaches 232 bytes -- the longest is ~210 -- so this is a guard, not a
    // routine cost.)
    static char s_truncated[OD_LOG_MAX_TEXT + 1];
    if (strlen(text) > OD_LOG_MAX_TEXT) {
        memcpy(s_truncated, text, OD_LOG_MAX_TEXT);
        s_truncated[OD_LOG_MAX_TEXT] = '\0';
        text = s_truncated;
    }
    const int need = (int)strlen(text) + (newline ? 2 : 0);

    if (!od_wait_for_room(need)) {
        od_count_drop();
        if (s_txLock != NULL) {
            xSemaphoreGive(s_txLock);
        }
        return;
    }

    // Reported on the first line that gets through, so a gap in the log always
    // explains itself rather than looking like a freeze -- which, given what
    // this whole change is chasing, is the difference that matters.
    uint32_t dropped = __atomic_exchange_n(&s_dropped, 0u, __ATOMIC_RELAXED);
    if (dropped > 0) {
        char notice[96];
        unsigned long ms = millis();
        int n = snprintf(notice, sizeof(notice),
                         "[%04lu.%03lu|C%lu] %c: [od_log] dropped %lu line(s) - log port backpressure",
                         ms / 1000, ms % 1000,
                         (unsigned long)getDeepSleepCount(),
                         level_chars[OD_LOG_WARN],
                         (unsigned long)dropped);
        // The notice waits for its own room. If it cannot get any, hand the
        // count back rather than losing the tally -- the next successful line
        // reports it instead.
        if (n > 0 && od_wait_for_room((int)strlen(notice) + 2)) {
            s_port->println(notice);
            // Re-measure. The notice just consumed the space that was measured
            // for the line, so without this the println() below can find a full
            // FIFO and block -- reintroducing, on the recovery path of all
            // places, the exact hang this function exists to prevent.
            if (!od_wait_for_room(need)) {
                od_count_drop();
                if (s_txLock != NULL) {
                    xSemaphoreGive(s_txLock);
                }
                return;
            }
        } else {
            __atomic_fetch_add(&s_dropped, dropped, __ATOMIC_RELAXED);
        }
    }

    if (newline) {
        s_port->println(text);
    } else {
        s_port->print(text);
    }

    if (s_txLock != NULL) {
        xSemaphoreGive(s_txLock);
    }
}

void _od_log(int level, const char *fmt, ...) {
    if (s_port == NULL) {
        return;
    }

    char buf[256];
    unsigned long ms = millis();
    unsigned long cycleCount = (unsigned long)getDeepSleepCount();
    int pos = snprintf(buf, sizeof(buf), "[%04lu.%03lu|C%lu] %c: ",
                        ms / 1000, ms % 1000,
                        cycleCount,
                        level_chars[level]);
    if (pos < 0) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + pos, sizeof(buf) - pos, fmt, args);
    va_end(args);

    od_emit(buf, true);
}

void od_log_raw(const char *fmt, ...) {
    if (s_port == NULL) {
        return;
    }

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    od_emit(buf, false);
}

void od_log_hex_line(char *buf, size_t bufSize, const char *label,
                     const uint8_t *data, uint16_t len) {
    int pos = snprintf(buf, bufSize, "%s", label);
    if (pos < 0) {
        pos = 0;
        buf[0] = '\0';
    }
    int dumpLen = (len < 32) ? len : 32;
    for (int i = 0; i < dumpLen && pos < (int)bufSize; i++) {
        int n = snprintf(buf + pos, bufSize - pos, i > 0 ? " %02X" : "%02X", data[i]);
        if (n < 0) {
            break;
        }
        pos += n;
    }
    if (len > 32 && pos >= 0 && pos < (int)bufSize) {
        snprintf(buf + pos, bufSize - pos, " ...");
    }
}

void od_log_flush(void) {
    if (s_port == NULL) {
        return;
    }

    s_port->flush();
    // Settling pause after flush(), unconditional as of 2026-07-27. Stream::flush()
    // returns once the driver has accepted the bytes, which is not the same as the
    // host having seen them: on a USB CDC port the transfer still has to be polled
    // off the device, and both targets log over CDC by default (nRF always -- there
    // is no OPENDISPLAY_LOG_UART path there). This was TARGET_ESP32-only for reasons
    // never recorded; the mechanism it compensates for is not ESP32-specific, and
    // od_log_flush() is called only at boot/wake checkpoints and before a rail cut --
    // the places where losing the last line costs the most and 5 ms costs nothing.
    // 16 call sites, so at most ~80 ms across a boot.
    delay(5);
}
