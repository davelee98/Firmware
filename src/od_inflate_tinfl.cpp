/*
 * od_inflate_tinfl — ESP32-WiFi inflate adapter backed by the ROM miniz `tinfl`.
 * See od_inflate_tinfl.h for the rationale. Compiled to an empty TU when the gate
 * is off (nRF52840 / classic ESP32), so miniz.h is only pulled in on ROM-tinfl builds.
 */

#include "od_inflate_tinfl.h"

#if OPENDISPLAY_USE_TINFL

#include <string.h>
#include "miniz.h"   /* ROM tinfl on S3/C3/C6 (symbol via <chip>.rom.ld, header via esp_rom) */

/* ------------------------------------------------------------------ state ---
 * All static: plain arrays land in internal .bss/DRAM automatically, which is
 * exactly what the history/output ring needs (fast match reads). The framebuffer
 * is in PSRAM, so we decode into this SRAM ring and let the caller flush each
 * delivered burst sequentially to PSRAM — never decode directly into PSRAM.
 */
static tinfl_decompressor s_decomp;                 /* ~11 KB Huffman tables + bit buffer */
static uint8_t            s_dict[TINFL_LZ_DICT_SIZE]; /* 32768: LZ77 history AND output ring */

static size_t   s_dict_ofs;      /* tinfl next-write position in the ring */
static size_t   s_deliver_ofs;   /* start of the undelivered (pending) region */
static size_t   s_pending;       /* bytes decoded but not yet copied to the caller (contiguous) */

static const uint8_t *s_in;      /* staged input (current frame) */
static size_t   s_in_remaining;
static bool     s_more_input;    /* = !final of the current push (controls HAS_MORE_INPUT) */

static uint32_t s_expected;      /* expected decompressed size (parity check with uzlib) */
static uint32_t s_produced;      /* total bytes decoded so far */
static bool     s_done;
static bool     s_initialized;
static const char *s_error;

extern "C" {

void od_inflate_tinfl_reset(uint32_t expected_output_size) {
    tinfl_init(&s_decomp);
    s_dict_ofs = 0;
    s_deliver_ofs = 0;
    s_pending = 0;
    s_in = NULL;
    s_in_remaining = 0;
    s_more_input = true;
    s_expected = expected_output_size;
    s_produced = 0;
    s_done = false;
    s_error = NULL;
    s_initialized = true;
    /* s_dict is not cleared: tinfl only reads history bytes it has written. */
}

od_zlib_status_t od_inflate_tinfl_push(const uint8_t *input, size_t len, bool final) {
    if (!s_initialized) { s_error = "tinfl stream not initialized"; return OD_ZLIB_STATUS_ERROR; }
    if (s_error) return OD_ZLIB_STATUS_ERROR;
    if (len > 0 && input == NULL) { s_error = "tinfl stream input is null"; return OD_ZLIB_STATUS_ERROR; }
    if (s_in_remaining > 0) { s_error = "previous input not fully consumed"; return OD_ZLIB_STATUS_ERROR; }
    if (s_done) {
        if (len != 0) { s_error = "input after end of zlib stream"; return OD_ZLIB_STATUS_ERROR; }
        return OD_ZLIB_STATUS_DONE;
    }
    s_in = input;
    s_in_remaining = len;
    s_more_input = !final;
    return OD_ZLIB_STATUS_NEEDS_INPUT;
}

od_zlib_status_t od_inflate_tinfl_poll(uint8_t *output, size_t capacity, size_t *produced) {
    *produced = 0;
    if (!s_initialized) { s_error = "tinfl stream not initialized"; return OD_ZLIB_STATUS_ERROR; }
    if (s_error) return OD_ZLIB_STATUS_ERROR;

    for (;;) {
        /* 1) Deliver already-decoded bytes to the caller first. The pending region is
         *    always contiguous (each decode is bounded to the ring end), so this is a
         *    plain memcpy — no ring wrap to handle here. */
        if (s_pending > 0) {
            size_t room = capacity - *produced;
            if (room == 0) return OD_ZLIB_STATUS_OUTPUT_READY;
            size_t n = (s_pending < room) ? s_pending : room;
            memcpy(output + *produced, s_dict + s_deliver_ofs, n);
            *produced += n;
            s_deliver_ofs += n;
            s_pending -= n;
            if (s_pending > 0) return OD_ZLIB_STATUS_OUTPUT_READY;  /* caller's buffer is full */
        }

        /* pending == 0 here */
        if (s_done) {
            if (s_expected != 0 && s_produced != s_expected) {
                s_error = "tinfl decompressed output size mismatch";
                return OD_ZLIB_STATUS_ERROR;
            }
            return OD_ZLIB_STATUS_DONE;
        }
        if (*produced == capacity) return OD_ZLIB_STATUS_OUTPUT_READY;

        /* 2) Decode the next contiguous burst into the ring (only when drained, so tinfl
         *    can never overwrite bytes we have not delivered yet). Bound output to the
         *    room up to the ring end; the wrap is handled by re-entering the loop. */
        size_t in_bytes = s_in_remaining;
        size_t out_bytes = TINFL_LZ_DICT_SIZE - s_dict_ofs;
        const mz_uint32 flags = (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER |
                                            (s_more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        s_deliver_ofs = s_dict_ofs;  /* the pending region will start where tinfl writes */

        tinfl_status st = tinfl_decompress(&s_decomp,
                                           (const mz_uint8 *)s_in, &in_bytes,
                                           (mz_uint8 *)s_dict, (mz_uint8 *)(s_dict + s_dict_ofs),
                                           &out_bytes, flags);

        s_in += in_bytes;
        s_in_remaining -= in_bytes;
        s_produced += (uint32_t)out_bytes;
        s_pending = out_bytes;
        s_dict_ofs = (s_dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1u);

        if (st < TINFL_STATUS_DONE) {  /* negative status codes are fatal */
            s_error = (st == TINFL_STATUS_ADLER32_MISMATCH) ? "tinfl adler32 mismatch"
                    : (st == TINFL_STATUS_BAD_PARAM)        ? "tinfl bad param"
                    :                                         "tinfl inflate failed";
            return OD_ZLIB_STATUS_ERROR;
        }
        if (st == TINFL_STATUS_DONE) {
            s_done = true;
            continue;  /* re-enter loop to deliver the final burst, then return DONE */
        }
        /* NEEDS_MORE_INPUT: staged frame consumed. If nothing was produced to deliver,
         * ask the caller for the next frame; otherwise loop to deliver first. */
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && s_pending == 0) {
            return OD_ZLIB_STATUS_NEEDS_INPUT;
        }
        /* HAS_MORE_OUTPUT (ring end reached) or NEEDS_MORE_INPUT with pending>0:
         * loop to deliver, then continue decoding / request input. */
    }
}

const char *od_inflate_tinfl_error(void) { return s_error ? s_error : ""; }

uint32_t od_inflate_tinfl_output_count(void) { return s_produced; }

}  /* extern "C" */

#endif /* OPENDISPLAY_USE_TINFL */
