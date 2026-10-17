/* ***************************************************************************
 * @file ezlopi_core_http_buf.c
 * @brief Buffer primitives for the HTTP core: growing a response buffer,
 *        appending a received chunk to it, and finding the body in the raw
 *        reply that results.
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
 * EZPI_core_http_body_in_place is here by the same argument and was written
 * here from the start. It is a parser for data chosen by whoever is on the
 * other end of the socket -- hex chunk lengths, a declared Content-Length, an
 * arbitrary number of header lines -- which is the category of code where a
 * host test under ASan is worth more than any amount of reading, and the
 * category this firmware had no way to test until a file in this component
 * could be compiled on a host.
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
 *                          Static Function Prototypes
 *******************************************************************************/
static char __lc(char c);
static int __ci_starts_with(const char *s, uint32_t s_len, const char *lower);
static const char *__header_value(const char *hdrs, uint32_t hdrs_len, const char *name_lower);
static int __value_has_token(const char *v, const char *token_lower);
static int __status_code(const char *resp);
static int __hex_digit(char c);
static ezlopi_error_t __dechunk_in_place(char *body, uint32_t len, uint32_t *out_len);

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

/**
 * Find the entity body in a raw HTTP reply, undoing chunked framing in place.
 *
 * WHY THIS IS NEEDED AT ALL
 *
 * EZPI_core_http_mbedtls_req hands the caller everything the socket produced.
 * For most of this function's life that did not matter, because no caller read
 * the body: two ignore it and the third reads the status line. The isSunState
 * suntime lookup is the first caller that has to parse it, and
 * cJSON_Parse("HTTP/1.1 200 OK\r\n...") fails -- so without this the feature
 * cannot work no matter how good the JSON parsing downstream is.
 *
 * Chunked framing is handled rather than rejected because it is the common
 * case, not the exotic one: an HTTP/1.1 server that does not know the body
 * length in advance is required to chunk, and a chunked body begins with a hex
 * length on its own line, which no JSON parser will accept either.
 *
 * In place, because the reassembled body is strictly shorter than the chunked
 * form -- every chunk costs a header and a CRLF -- so no allocation is needed,
 * and because the buffer belongs to the caller and is freed straight after.
 *
 * STRICT ON PURPOSE, IN ONE DIRECTION
 *
 * Every rejection here is a reply this cannot account for in full: no blank
 * line, a status other than 200, a Content-Length larger than what arrived, a
 * chunk sequence with no terminating zero. In each of those cases some bytes
 * are present and could be handed back, and that is the thing not to do. A
 * body missing its middle parses exactly as well as a complete one, and the
 * caller's only alternative to a correct answer -- not firing the scene, and
 * saying why -- is far better than a wrong one.
 *
 * The one place it is lenient is line endings: a bare LF is accepted as the
 * header/body separator, because proxies produce them and refusing a reply
 * over its line endings costs a feature and buys nothing. Chunk framing itself
 * still requires CRLF, which is what servers send.
 */
ezlopi_error_t EZPI_core_http_body_in_place(char *resp, char **body, uint32_t *body_len)
{
    if ((NULL == resp) || (NULL == body) || (NULL == body_len))
    {
        return EZPI_FAILED;
    }

    const int code = __status_code(resp);
    if (200 != code)
    {
        /* A 301 or a 404 has a body too, and it is not the data that was
        ** asked for. Reading it would turn a redirect into a parse failure
        ** several layers away from the thing that actually went wrong. */
        TRACE_E("HTTP reply status is %d, not 200", code);
        return EZPI_FAILED;
    }

    const char *crlf = strstr(resp, "\r\n\r\n");
    const char *lf = strstr(resp, "\n\n");

    uint32_t sep = 0;
    uint32_t sep_len = 0;

    /* "\r\n\r\n" contains "\n\n" one byte in, so comparing positions -- not
    ** testing crlf first -- is what picks the right separator when both are
    ** present but the LF pair comes earlier. */
    if ((NULL != crlf) && ((NULL == lf) || (crlf < lf)))
    {
        sep = (uint32_t)(crlf - resp);
        sep_len = 4;
    }
    else if (NULL != lf)
    {
        sep = (uint32_t)(lf - resp);
        sep_len = 2;
    }
    else
    {
        TRACE_E("HTTP reply has no blank line; the headers did not arrive whole");
        return EZPI_FAILED;
    }

    char *body_start = resp + sep + sep_len;
    const uint32_t arrived = (uint32_t)strlen(body_start);
    uint32_t len = 0;

    const char *te = __header_value(resp, sep, "transfer-encoding");

    if ((NULL != te) && (0 != __value_has_token(te, "chunked")))
    {
        if (EZPI_SUCCESS != __dechunk_in_place(body_start, arrived, &len))
        {
            return EZPI_FAILED;
        }
    }
    else
    {
        const char *cl = __header_value(resp, sep, "content-length");

        /* No Content-Length and no chunking means the body is delimited by the
        ** connection closing, which is exactly what the read loop upstream
        ** observes, so everything after the blank line is the body. */
        len = arrived;

        if (NULL != cl)
        {
            uint64_t declared = 0;
            uint32_t digits = 0;

            while ((cl[digits] >= '0') && (cl[digits] <= '9'))
            {
                declared = (declared * 10u) + (uint64_t)(cl[digits] - '0');
                if (declared > (uint64_t)INT_MAX)
                {
                    TRACE_E("Content-Length is larger than any reply this can hold");
                    return EZPI_FAILED;
                }
                digits++;
            }

            if (0 == digits)
            {
                TRACE_E("Content-Length has no number after it");
                return EZPI_FAILED;
            }
            if (declared > (uint64_t)arrived)
            {
                /* The reply said how long it was and it is shorter than that.
                ** Said rather than trimmed: this is the one case where the
                ** server itself tells us the body is incomplete. */
                TRACE_E("reply declares %u body bytes, %u arrived", (unsigned)declared, (unsigned)arrived);
                return EZPI_FAILED;
            }

            /* Shorter than what arrived is normal and not an error: with
            ** keep-alive the next reply's bytes can already be in the buffer,
            ** and Content-Length is what says where this one stops. */
            len = (uint32_t)declared;
            body_start[len] = '\0';
        }
    }

    if (0 == len)
    {
        TRACE_E("HTTP reply has an empty body");
        return EZPI_FAILED;
    }

    *body = body_start;
    *body_len = len;

    return EZPI_SUCCESS;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static char __lc(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (char)(c - 'A' + 'a') : c;
}

/**
 * Case-insensitive prefix test, bounded by `s_len` so it cannot read past the
 * line it was given. Written here rather than reaching for strncasecmp, which
 * glibc hides under -std=c99 and which would need a feature macro on the host
 * for no benefit over six lines.
 */
static int __ci_starts_with(const char *s, uint32_t s_len, const char *lower)
{
    uint32_t i = 0;

    while ('\0' != lower[i])
    {
        if ((i >= s_len) || (__lc(s[i]) != lower[i]))
        {
            return 0;
        }
        i++;
    }

    return 1;
}

/**
 * The value of one header, or NULL. `name_lower` must be lowercase; header
 * names are case-insensitive on the wire and servers do not agree on a
 * spelling -- "Content-Length", "content-length" and "CONTENT-LENGTH" are all
 * the same header.
 *
 * The match is anchored at a line start and requires the ':' immediately
 * after the name, so a header named "X-Content-Length" cannot answer for
 * "Content-Length", and a value that happens to contain the word cannot
 * either. Continuation lines -- a header folded onto the next line with
 * leading whitespace -- are not joined; nothing sends them and reading one as
 * a header of its own cannot match any name asked for here.
 */
static const char *__header_value(const char *hdrs, uint32_t hdrs_len, const char *name_lower)
{
    uint32_t name_len = 0;
    uint32_t i = 0;

    while ('\0' != name_lower[name_len])
    {
        name_len++;
    }

    /* Step over the status line: it is not a header, and anchoring at line
    ** starts would otherwise test it.
    **
    ** This is belt-and-braces and is recorded as such rather than defended. A
    ** mutation that removed this skip entirely changed no observable
    ** behaviour, because the anchor below requires the name at a line start
    ** and a status line this function accepts always begins "HTTP/1." -- so
    ** the status line cannot match any header name regardless. Kept because it
    ** costs two comparisons once per lookup and because the day something
    ** else is passed in here, the skip is what makes it still correct. */
    while ((i < hdrs_len) && ('\n' != hdrs[i]))
    {
        i++;
    }
    if (i < hdrs_len)
    {
        i++;
    }

    while (i < hdrs_len)
    {
        uint32_t eol = i;

        while ((eol < hdrs_len) && ('\n' != hdrs[eol]))
        {
            eol++;
        }

        if ((0 != __ci_starts_with(hdrs + i, eol - i, name_lower)) &&
            ((i + name_len) < hdrs_len) && (':' == hdrs[i + name_len]))
        {
            uint32_t v = i + name_len + 1;

            while ((v < eol) && ((' ' == hdrs[v]) || ('\t' == hdrs[v])))
            {
                v++;
            }

            return hdrs + v;
        }

        i = (eol < hdrs_len) ? (eol + 1) : hdrs_len;
    }

    return NULL;
}

/**
 * Whether a header value contains `token_lower`, searching only as far as the
 * end of its line. A list is allowed there -- "Transfer-Encoding: gzip,
 * chunked" is legal and means chunked framing is the outer layer -- so a whole
 * value comparison against "chunked" would miss it.
 */
static int __value_has_token(const char *v, const char *token_lower)
{
    uint32_t end = 0;
    uint32_t i = 0;

    while (('\0' != v[end]) && ('\r' != v[end]) && ('\n' != v[end]))
    {
        end++;
    }

    for (i = 0; i < end; i++)
    {
        if (0 != __ci_starts_with(v + i, end - i, token_lower))
        {
            return 1;
        }
    }

    return 0;
}

/**
 * The three-digit status, or -1 when the first line is not an HTTP/1.x status
 * line at all. The "HTTP/1." prefix is required: without it any text whose
 * first line happens to contain a space followed by three digits would be read
 * as a reply.
 */
static int __status_code(const char *resp)
{
    uint32_t i = 0;
    int code = 0;
    int digits = 0;

    if (0 != strncmp(resp, "HTTP/1.", 7))
    {
        return -1;
    }

    while (('\0' != resp[i]) && (' ' != resp[i]) && ('\r' != resp[i]) && ('\n' != resp[i]))
    {
        i++;
    }
    if (' ' != resp[i])
    {
        return -1;
    }
    i++;

    while ((digits < 3) && (resp[i + digits] >= '0') && (resp[i + digits] <= '9'))
    {
        code = (code * 10) + (resp[i + digits] - '0');
        digits++;
    }

    return (3 == digits) ? code : -1;
}

static int __hex_digit(char c)
{
    int d = -1;

    if ((c >= '0') && (c <= '9'))
    {
        d = c - '0';
    }
    else if ((c >= 'a') && (c <= 'f'))
    {
        d = (c - 'a') + 10;
    }
    else if ((c >= 'A') && (c <= 'F'))
    {
        d = (c - 'A') + 10;
    }

    return d;
}

/**
 * Reassemble a chunked body over itself.
 *
 *     1a\r\n{"status":"OK","res\r\n10\r\nults":{...}}\r\n0\r\n\r\n
 *
 * becomes the concatenation of the two payloads. `dst` never overtakes `src`
 * because each chunk is preceded by at least three bytes of framing it does
 * not keep, so the memmove is always a copy downwards -- and memmove rather
 * than memcpy because for a single chunk the two regions are adjacent and
 * overlap is the thing memcpy is not allowed to be given.
 *
 * Running out of input is a failure in every position: a chunked reply whose
 * terminating "0\r\n" never arrived is a reply that was cut off mid-transfer,
 * which is precisely the case chunked framing exists to make detectable. The
 * old read loop's habit of treating a short reply as a complete one is what
 * this is written against.
 */
static ezlopi_error_t __dechunk_in_place(char *body, uint32_t len, uint32_t *out_len)
{
    uint32_t src = 0;
    uint32_t dst = 0;

    while (1)
    {
        uint64_t size = 0;
        uint32_t digits = 0;
        int d = 0;

        while ((src < len) && ((d = __hex_digit(body[src])) >= 0))
        {
            size = (size * 16u) + (uint64_t)d;
            if (size > (uint64_t)INT_MAX)
            {
                TRACE_E("chunk size is larger than any reply this can hold");
                return EZPI_FAILED;
            }
            src++;
            digits++;
        }

        if (0 == digits)
        {
            TRACE_E("chunked reply has a chunk header with no size in it");
            return EZPI_FAILED;
        }

        /* A chunk header may carry ";name=value" extensions. Nothing reads
        ** them, but they have to be stepped over to reach the CRLF. */
        while ((src < len) && ('\r' != body[src]))
        {
            src++;
        }
        if (((src + 2) > len) || ('\r' != body[src]) || ('\n' != body[src + 1]))
        {
            TRACE_E("chunked reply ended inside a chunk header");
            return EZPI_FAILED;
        }
        src += 2;

        if (0 == size)
        {
            /* The terminating chunk. Anything after it is a trailer, which is
            ** not part of the body. */
            break;
        }

        if (((uint64_t)src + size) > (uint64_t)len)
        {
            TRACE_E("chunked reply announced %u bytes and %u are left", (unsigned)size, (unsigned)(len - src));
            return EZPI_FAILED;
        }

        memmove(body + dst, body + src, (size_t)size);
        dst += (uint32_t)size;
        src += (uint32_t)size;

        if (((src + 2) > len) || ('\r' != body[src]) || ('\n' != body[src + 1]))
        {
            TRACE_E("chunked reply has no CRLF after a chunk");
            return EZPI_FAILED;
        }
        src += 2;
    }

    body[dst] = '\0';
    *out_len = dst;

    return EZPI_SUCCESS;
}
