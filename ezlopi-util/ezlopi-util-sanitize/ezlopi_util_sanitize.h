/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/

/**
 * @file    ezlopi_util_sanitize.h
 * @brief   Small helpers for data that arrived from an untrusted client.
 *
 * Everything here operates on bytes that came in over BLE, UART or the cloud
 * websocket, which means lengths and delimiters in the payload are chosen by
 * whoever is talking to the hub and cannot be trusted.
 *
 * These live in their own component, with no dependency beyond the C library,
 * for one reason: so the host-side test suite under tests/host can compile and
 * exercise the real implementations rather than a copy of them. Each function
 * here replaced an open-coded version that had a bug in it; the tests encode
 * those bugs as regression cases.
 */

#ifndef __EZLOPI_UTIL_SANITIZE_H__
#define __EZLOPI_UTIL_SANITIZE_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Public Function Prototypes
     *******************************************************************************/

    /**
     * @brief Find the end of the JSON value that starts at @p start.
     *
     * Scans for the brace or bracket that closes the container opened at
     * @p start, tracking string state so that a delimiter inside a string
     * literal -- including one preceded by a backslash escape -- does not affect
     * the nesting depth.
     *
     * Used to split a transport buffer that holds several concatenated JSON
     * objects, e.g. `{"cmd":4}{"cmd":1}`. cJSON_Parse stops after the first
     * value and still reports success, so a caller that parses the buffer whole
     * silently discards everything after the first object.
     *
     * @param start First character of the value. May be NULL.
     * @return Pointer one past the closing delimiter, or NULL if @p start is
     *         NULL, does not open a container, or the container never closes.
     */
    const char *EZPI_util_json_value_end(const char *start);

    /**
     * @brief Overwrite the values of secret-bearing JSON members with '*'.
     *
     * Rewrites @p json in place and preserves its length. Recognises the member
     * names that carry a credential in this firmware's payloads: `key` (the
     * Wi-Fi PSK, as reported by hub.network.get), `pass`, `password` and
     * `wifi_password`.
     *
     * Intended for making a payload safe to trace. Call it only *after* the
     * payload has gone out on the wire, so the peer still receives the real
     * value and the API is unchanged.
     *
     * @param json NUL-terminated, mutable JSON text. NULL is a no-op.
     */
    void EZPI_util_redact_secrets_in_place(char *json);

    /**
     * @brief Clamp a client-supplied copy length to what is actually available.
     *
     * Returns the largest length that is safe to copy: no more than
     * @p requested, no more than the @p src_len bytes the source actually
     * holds, and no more than the @p dst_remaining bytes left in the
     * destination.
     *
     * Exists because a length taken straight off the wire was previously passed
     * to memcpy unchecked, so a client that claimed more bytes than it sent --
     * or more than the destination was sized for -- walked off the end of a
     * heap block.
     *
     * @param requested     Length the client asked for.
     * @param src_len       Bytes actually present at the source.
     * @param dst_remaining Bytes still free in the destination.
     * @return Byte count that is safe to copy. Zero if anything is exhausted.
     */
    uint32_t EZPI_util_clamp_copy_len(uint32_t requested, uint32_t src_len, uint32_t dst_remaining);

    /**
     * @brief Copy a client-supplied string into a fixed buffer and terminate it.
     *
     * Copies at most @p dst_size - 1 bytes and always writes a NUL, so the
     * result is safe to hand to strcmp/strncmp.
     *
     * Exists because a config string was previously copied into a 10-byte stack
     * buffer using a length taken from the payload: a longer string overflowed
     * the buffer, and an exactly-10-byte one left it unterminated.
     *
     * @param dst      Destination buffer. NULL is a no-op returning 0.
     * @param dst_size Size of @p dst in bytes, including room for the NUL.
     * @param src      Source bytes. NULL yields an empty string.
     * @param src_len  Length of @p src as claimed by the payload.
     * @return Number of bytes written to @p dst, excluding the NUL.
     */
    uint32_t EZPI_util_copy_str_bounded(char *dst, uint32_t dst_size, const char *src, uint32_t src_len);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_UTIL_SANITIZE_H__
