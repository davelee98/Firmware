#pragma once

#include <Arduino.h>

// Timestamped, allocation-free logging.
//
// Call od_log_init() once in setup() right after the serial port's begin().
// Levels are filtered at compile time via OD_LOG_LEVEL, so disabled levels
// compile to nothing.

#define OD_LOG_ERROR 0
#define OD_LOG_WARN  1
#define OD_LOG_INFO  2
#define OD_LOG_DEBUG 3

#ifndef OD_LOG_LEVEL
#define OD_LOG_LEVEL OD_LOG_INFO
#endif

#define od_log_error(fmt, ...) do { if (OD_LOG_LEVEL >= OD_LOG_ERROR) \
    _od_log(OD_LOG_ERROR, fmt, ##__VA_ARGS__); } while (0)
#define od_log_warn(fmt, ...)  do { if (OD_LOG_LEVEL >= OD_LOG_WARN) \
    _od_log(OD_LOG_WARN, fmt, ##__VA_ARGS__); } while (0)
#define od_log_info(fmt, ...)  do { if (OD_LOG_LEVEL >= OD_LOG_INFO) \
    _od_log(OD_LOG_INFO, fmt, ##__VA_ARGS__); } while (0)
#define od_log_debug(fmt, ...) do { if (OD_LOG_LEVEL >= OD_LOG_DEBUG) \
    _od_log(OD_LOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)

void od_log_init(Stream *port);
void _od_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void od_log_raw(const char *fmt, ...)         __attribute__((format(printf, 1, 2)));
void od_log_flush(void);

// Writes are non-blocking: a line that does not fit the port's TX FIFO is
// dropped and counted, never waited on. Both USB CDC implementations we log
// over (Adafruit_USBD_CDC on nRF, HWCDC on ESP32) spin without a timeout when
// the host stops draining, and a blocked log write blocks loop() -- fatal on
// nRF, which has no watchdog. The count is reported on the next write that has
// room, so a gap in the log always announces itself.

// Tells the logger whether a host is listening, so a dark port is not counted as
// dropped lines. Optional; NULL (the default) means "assume ready".
void od_log_set_ready_hook(bool (*fn)(void));

// Lines dropped since boot. For the connected-state heartbeat.
uint32_t od_log_dropped_total(void);

// Builds "<label><space-separated %02X bytes, up to 32><' ...' if truncated>" into
// buf. Lives here rather than in one caller's translation unit because the RX line
// (command_queue.cpp, on the stack callback task) and the TX line
// (communication.cpp, on loop()) must render frames identically -- two copies of
// this loop is how the two directions drift apart.
void od_log_hex_line(char *buf, size_t bufSize, const char *label,
                     const uint8_t *data, uint16_t len);
