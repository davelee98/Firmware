/*
 * od_inflate_tinfl — ESP32-WiFi inflate adapter backed by the ROM miniz `tinfl`.
 *
 * Drop-in replacement for the uzlib streaming inflater (lib/uzlib,
 * od_zlib_stream_*) on ESP32 builds that carry the WiFi/LAN transport. It exposes
 * the SAME streaming contract (reset / push / poll / error / output_count) and the
 * SAME status type (od_zlib_status_t, reused from uzlib.h) so display_service.cpp
 * can bind its existing od_zlib_stream_* call sites to this engine via a compile-time
 * #define remap, with no changes to lib/uzlib and no changes to the call sites.
 *
 * WHY: the uzlib engine is a bit-serial, byte-at-a-time resumable state machine —
 * fine for BLE (wire << inflate), but the LAN wire is ~10-100x faster so software
 * inflate becomes the bottleneck and compression turns into a net loss. `tinfl` is
 * word-at-a-time + table-driven and lives in mask ROM on S3/C3/C6 (fixed addresses
 * in <chip>.rom.ld), so it is faster at zero flash cost.
 *
 * The status enum (od_zlib_status_t, OD_ZLIB_STATUS_*) is defined by uzlib.h; we
 * only include it, never modify it.
 */

#ifndef OD_INFLATE_TINFL_H
#define OD_INFLATE_TINFL_H

#include "uzlib.h"   /* od_zlib_status_t + OD_ZLIB_STATUS_* (include only, not modified) */

/* Gate: ROM tinfl exists only on the WiFi-capable ESP32 builds. Mirror the exact
 * condition that defines OPENDISPLAY_HAS_WIFI (src/wifi_service.h). Overridable via
 * a build flag if a target ever needs to force one engine or the other. */
#if !defined(OPENDISPLAY_USE_TINFL)
#  if defined(TARGET_ESP32) && defined(OPENDISPLAY_ENABLE_WIFI)
#    define OPENDISPLAY_USE_TINFL 1
#  else
#    define OPENDISPLAY_USE_TINFL 0
#  endif
#endif

#if OPENDISPLAY_USE_TINFL

#ifdef __cplusplus
extern "C" {
#endif

/* Same semantics/signatures as od_zlib_stream_* in uzlib.h. */
void od_inflate_tinfl_reset(uint32_t expected_output_size);
od_zlib_status_t od_inflate_tinfl_push(const uint8_t *input, size_t len, bool final);
od_zlib_status_t od_inflate_tinfl_poll(uint8_t *output, size_t capacity, size_t *produced);
const char *od_inflate_tinfl_error(void);
uint32_t od_inflate_tinfl_output_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDISPLAY_USE_TINFL */

#endif /* OD_INFLATE_TINFL_H */
