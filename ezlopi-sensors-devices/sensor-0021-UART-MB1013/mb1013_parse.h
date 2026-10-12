/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    mb1013_parse.h
 * @brief   MB1013 range-frame parsing, separated from the device registration.
 *
 * The parsing used to sit inside __uart_data_upcall() in
 * sensor_0021_UART_MB1013.c, which includes ezlopi_core_cloud.h,
 * ezlopi_cloud_items.h and the device value-updated path. None of that can be
 * compiled by a host compiler, so the one part of the driver that acts on bytes
 * chosen by whatever is on the other end of the wire was the one part no test
 * could reach -- and the bounds fix in cecf5da went in unverified as a result.
 *
 * What is split out here is only the frame scan. It takes a buffer and a length
 * and hands back a value; it knows nothing about items, user_arg or the cloud,
 * which stay in the upcall. The dependency direction is deliberate: the
 * registration file includes this header, never the reverse. Adding an ESP-IDF
 * or ezlopi-core include below would put the parser back out of reach of
 * tests/host, so please do not.
 *
 * Splitting it immediately paid for itself. The original line was
 *
 *     atoi((const char *)&buffer[idx + 1]) / 10.0
 *
 * on a heap block with no NUL anywhere in it, and the comment added in cecf5da
 * argued it was bounded because "it starts at a digit and stops at the first
 * non-digit, and buffer[idx + 5] is known to be '\r'". Both halves are wrong.
 * Nothing checks that the four middle bytes are digits, and '\r' is a
 * whitespace character, which strtol *skips* rather than stopping at. So a
 * frame of the form 'R' followed by four spaces, tabs or newlines and then the
 * '\r' made atoi walk past the terminator and off the end of the allocation.
 * AddressSanitizer confirms the overflow against an exact-size block; the
 * clamp in cecf5da bounded the loop but never the atoi inside it.
 *
 * The parser here therefore reads the four payload bytes and nothing else, and
 * never calls a string function on unterminated memory at all, which removes
 * the class rather than the instance.
 */

#ifndef _MB1013_PARSE_H_
#define _MB1013_PARSE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* A reading is the six bytes 'R', four digits, '\r'. Nothing shorter can
       contain one, so nothing shorter is read. */
#define MB1013_FRAME_LEN 6

    /**
     * @brief Find the first complete range reading in a UART buffer.
     *
     * Scans for the six-byte frame 'R' dddd '\r'. When one is found entirely
     * inside output_len and its payload parses, the range in metres is written
     * to *out_value and true is returned. Otherwise false is returned and
     * *out_value is left alone.
     *
     * Accepted payloads, chosen to keep every input the old code read
     * correctly:
     *   - four digits, "0123" -> 12.3 (what the part actually emits)
     *   - leading spaces or tabs, " 123" -> 12.3
     *   - digits then junk, "12z3" -> 1.2, matching where atoi stopped
     *
     * Rejected, where the old code reported a reading of 0.0 from nothing:
     * payloads with no digit at all ("zzzz", "   -"), and a leading '-', since
     * a negative range is not a measurement. These now yield no reading and
     * the scan continues, rather than publishing a fabricated 0.0 to the cloud.
     *
     * @param buffer      Bytes as received. May contain no frame, a partial
     *                    frame, or several frames; only the first complete one
     *                    is reported, which is what the original loop did.
     * @param output_len  Bytes actually present in buffer. Every byte read is
     *                    inside this length -- the point of the exercise.
     * @param out_value   Written only on success.
     */
    bool EZPI_mb1013_parse_range(const uint8_t *buffer, uint32_t output_len, float *out_value);

#ifdef __cplusplus
}
#endif

#endif /* _MB1013_PARSE_H_ */

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
