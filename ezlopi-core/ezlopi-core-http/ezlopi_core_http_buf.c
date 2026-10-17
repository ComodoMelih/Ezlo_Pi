/* ***************************************************************************
 * @file ezlopi_core_http_buf.c
 * @brief Buffer primitives for the HTTP core: growing a response buffer and
 *        appending a received chunk to it.
 *
 * WHY THIS IS A SEPARATE TRANSLATION UNIT
 *
 * ezlopi_core_http.c cannot be compiled on a host. It needs ten headers this
 * repository does not ship -- nine of them under mbedtls/ and esp_tls/ -- and
 * even with those it leaves 44 further diagnostics, mostly designated
 * initializers against esp_http_client_config_t. That was measured, not
 * assumed, and it is why tests/host/stub_httpurl.c reimplements two of that
 * file's buffer helpers instead of linking them.
 *
 * The response accumulator below cannot be left in the same position. It is
 * pure buffer arithmetic with no ESP-IDF surface at all, and the defect it
 * exists to fix -- see EZPI_core_http_resp_append -- was an off-by-one that no
 * compiler warning, no sanitizer and no target build could report, because it
 * silently dropped a byte rather than touching memory it should not. The only
 * instrument that can hold it is a test that runs the real code, so the real
 * code has to live somewhere a test can reach. This file is that place: it
 * includes ezlopi_core_http.h and nothing else of consequence, and the host
 * suite already stubs everything that header needs.
 *
 * EZPI_core_http_dyna_relloc moved here from ezlopi_core_http.c for the same
 * reason and unchanged otherwise -- the accumulator grows through it, and a
 * test of the accumulator that stubbed its growth would be testing the half
 * that was never in doubt. Its other caller, in the scenes then-methods
 * helper, is unaffected: same component, same declaration in the same header.
 *
 * The move had a second effect worth recording here, since it is the reason the
 * count above is two and not three. stub_httpurl.c carried a copy of
 * dyna_relloc only because the single definition sat in a file no host can
 * compile; now that it is here, test_httpurl links this file and deletes its
 * copy. All 153 of that test's assertions still pass against the real
 * function, which is the only evidence available that the copy had been
 * faithful.
 * ***************************************************************************/

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "ezlopi_util_trace.h"

#include "ezlopi_core_http.h"
#include "EZLOPI_USER_CONFIG.h"

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

ezlopi_error_t EZPI_core_http_dyna_relloc(char **Buf, int reqSize)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if ((NULL != *Buf) && (reqSize > 0)) // strictly:  (new-size != 0)
    {
        void *NewBuf = ezlopi_realloc(__FUNCTION__, *Buf, reqSize); // reqSize ≤ 1.6 * n
        if (NULL == NewBuf)
        {
            TRACE_E("Reallocation - failed");
        }
        else
        {
            *Buf = NewBuf;
            ret = EZPI_SUCCESS; // return success
        }
    }
    return ret;
}

/**
 * Append one received chunk to a response buffer, growing it if needed.
 *
 * This replaces the two snprintf calls that used to accumulate an HTTPS reply
 * inside __ezlopi_http_req_via_mbedTLS, each of which lost a byte:
 *
 *     snprintf(buf, len, "%s", chunk);                  // first chunk
 *     snprintf(buf + strlen(buf), len, "%s", chunk);    // every later chunk
 *
 * snprintf's size argument counts the terminator, so asking it to write `len`
 * bytes of payload with a limit of `len` stores len-1 of them. mbedtls_ssl_read
 * returns at most 255 bytes at a time, so every 255 bytes of any reply lost its
 * last byte -- and a reply that fits in one read lost its final character,
 * which for a JSON body is the closing brace.
 *
 * It reached only this path, not every HTTPS reply the firmware receives: the
 * esp_http_client path in ezlopi_core_http.c memcpy's each chunk exactly into a
 * list and loses nothing, which is the path provisioning uses and parses. Of
 * this path's three callers, two never read the body and the third reads only
 * the status line at the front of the first chunk. So no caller was visibly
 * broken -- and no caller could have a body either, which is what blocked the
 * sunstate feature.
 *
 * Nothing about the loss was visible: it wrote strictly less than it had room
 * for, so there was no overflow for a sanitizer to catch, no truncation a
 * compiler could warn about, and the trace that printed the body printed the
 * damaged copy without any mark where the bytes went missing.
 *
 * Three things are deliberate here.
 *
 * `used` is carried by the caller rather than recovered with strlen(). The old
 * append offset was strlen(buf), which is the body's length only while the body
 * contains no NUL -- a reply that does would have had every later chunk written
 * over the earlier ones. Nothing requires an HTTP body to be text.
 *
 * memcpy, not snprintf: the chunk is `chunk_len` bytes of data, not a C string,
 * and copying it as one costs a scan and re-derives a length the caller already
 * knows.
 *
 * The buffer is always terminated at `*used`, so a caller that treats a text
 * body as a string sees a well-formed one, while a caller that wants the exact
 * length reads it from `*used` instead of measuring.
 *
 * @param buf        address of the buffer pointer; may be moved by growth
 * @param cap        address of the buffer's capacity in bytes, updated on growth
 * @param used       address of the payload length so far, excluding the
 *                   terminator; updated on success
 * @param chunk      bytes to append; not required to be NUL-terminated
 * @param chunk_len  how many bytes of `chunk` to append
 *
 * @return EZPI_SUCCESS when all `chunk_len` bytes were appended. On
 *         EZPI_FAILED nothing is appended and `*used`, `*cap` and `*buf` are
 *         left as they were, so the caller's accounting stays consistent with
 *         the buffer and a failed chunk cannot half-apply.
 */
ezlopi_error_t EZPI_core_http_resp_append(char **buf, uint32_t *cap, uint32_t *used, const char *chunk, uint32_t chunk_len)
{
    if ((NULL == buf) || (NULL == *buf) || (NULL == cap) || (NULL == used) || (NULL == chunk))
    {
        return EZPI_FAILED;
    }

    if (0 == chunk_len)
    {
        return EZPI_SUCCESS; /* nothing to do, and not a failure */
    }

    /* The terminator is counted here, which is the whole point of the
    ** function: `need` is what the buffer must hold, not what the payload
    ** measures.
    **
    ** Both sums are computed in 64 bits and bounded before being narrowed. The
    ** first draft of this guard was `need < chunk_len` on the 32-bit sum, which
    ** is the idiom for catching a wrap and is wrong at exactly one input: with
    ** *used == UINT32_MAX the sum wraps to chunk_len, the comparison is false,
    ** and the function goes on to memcpy at an offset of 4GB. No reply this
    ** firmware can receive comes near that -- the heap is a few hundred
    ** kilobytes -- so the hole is unreachable here. It is still rewritten
    ** rather than documented, because "nearly right, and the shortfall is
    ** unreachable" is the shape of the defect this whole function exists to
    ** remove, and a bound that is correct for every input costs one cast.
    **
    ** The ceiling is INT_MAX, not UINT32_MAX: EZPI_core_http_dyna_relloc takes
    ** the size as an int and rejects anything <= 0, so a `want` above INT_MAX
    ** would otherwise be narrowed to a negative and refused as if the heap had
    ** failed. Refusing it here says what actually happened. */
    uint64_t need64 = (uint64_t)(*used) + (uint64_t)chunk_len + 1u;

    if (need64 > (uint64_t)INT_MAX)
    {
        TRACE_E("response would exceed the largest size the buffer can hold");
        return EZPI_FAILED;
    }

    uint32_t need = (uint32_t)need64;

    if (need > *cap)
    {
        /* Grow geometrically rather than to exactly `need`: the caller is in a
        ** read loop, so growing to a tight fit on every chunk makes the number
        ** of reallocations linear in the reply size. For a 1299-byte reply read
        ** 255 bytes at a time that is the difference between 3 reallocations
        ** and 5; test_httpresp measures it. */
        uint64_t want64 = (uint64_t)(*cap) + ((uint64_t)(*cap) / 2u) + (uint64_t)chunk_len + 1u;
        if (want64 > (uint64_t)INT_MAX)
        {
            /* The geometric overshoot is a convenience, not a requirement, so
            ** when it does not fit fall back to the tight size -- which `need64`
            ** has already been shown to be within bounds. */
            want64 = (uint64_t)need;
        }
        if (want64 < (uint64_t)need)
        {
            want64 = (uint64_t)need;
        }

        uint32_t want = (uint32_t)want64;
        if (EZPI_SUCCESS != EZPI_core_http_dyna_relloc(buf, (int)want))
        {
            /* dyna_relloc has already traced the failure. Reported rather than
            ** swallowed: the old loop dropped the chunk and carried on, which
            ** produced a truncated body indistinguishable from a complete one.
            ** Nothing is written and no output is touched, so the caller's
            ** `used` still describes the buffer exactly and a later chunk can
            ** still be appended if the heap recovers. */
            return EZPI_FAILED;
        }
        *cap = want;
    }

    memcpy((*buf) + (*used), chunk, chunk_len);
    *used += chunk_len;
    (*buf)[*used] = '\0';

    return EZPI_SUCCESS;
}
