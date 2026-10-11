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
 * @file    ezlopi_util_improv.c
 * @brief   Improv Wi-Fi protocol codec, independent of any transport.
 *
 * See ezlopi_util_improv.h for where every constant in here was established
 * from, and why the published specification was not treated as sufficient.
 */

#include <string.h>

#include "ezlopi_util_improv.h"

/*-----------------------------------------------------------------------------
 * Frame byte offsets
 *
 * Named rather than written inline, because the receive state machine indexes
 * them by position and an off-by-one in that arithmetic is the kind of defect
 * that produces a parser which works on every frame the author happened to try.
 *---------------------------------------------------------------------------*/
#define FRAME_IDX_VERSION 6u
#define FRAME_IDX_TYPE 7u
#define FRAME_IDX_LENGTH 8u
#define FRAME_IDX_DATA 9u

/** @brief Payload offsets within an RPC. */
#define RPC_IDX_COMMAND 0u
#define RPC_IDX_LENGTH 1u
#define RPC_IDX_DATA 2u

/** @brief Bytes an RPC payload has besides its data, excluding the checksum. */
#define RPC_OVERHEAD 2u

uint8_t EZPI_improv_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;

    if (NULL != data)
    {
        for (uint32_t i = 0; i < len; i++)
        {
            sum += data[i];
        }
    }

    return (uint8_t)(sum & 0xFFu);
}

e_ezpi_improv_error_t EZPI_improv_rpc_parse(const uint8_t *payload, uint32_t len,
                                            bool has_checksum, s_ezpi_improv_rpc_t *out)
{
    const uint32_t trailer = has_checksum ? 1u : 0u;

    if ((NULL == payload) || (NULL == out))
    {
        return EZPI_IMPROV_ERROR_INVALID_RPC;
    }

    /* Smallest legal payload is a command and a zero length, plus a checksum
     * if this transport carries one. */
    if (len < (RPC_OVERHEAD + trailer))
    {
        return EZPI_IMPROV_ERROR_INVALID_RPC;
    }

    const uint8_t command = payload[RPC_IDX_COMMAND];
    const uint8_t data_len = payload[RPC_IDX_LENGTH];

    /* The declared length has to agree with what actually arrived before any
     * of the data is indexed. Trusting payload[1] and reading payload[2 + i]
     * would be an out-of-bounds read on a truncated or hostile payload. */
    if (((uint32_t)data_len + RPC_OVERHEAD + trailer) != len)
    {
        return EZPI_IMPROV_ERROR_INVALID_RPC;
    }

    if (has_checksum)
    {
        if (EZPI_improv_checksum(payload, len - 1u) != payload[len - 1u])
        {
            return EZPI_IMPROV_ERROR_INVALID_RPC;
        }
    }

    s_ezpi_improv_rpc_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.command = command;

    if (EZPI_IMPROV_RPC_SEND_WIFI == command)
    {
        const uint8_t *data = &payload[RPC_IDX_DATA];

        /* [ssid length][ssid ...][passphrase length][passphrase ...]. Both
         * lengths come from the client, so each is bounds-checked against
         * data_len before it is used to compute the next offset. */
        if (data_len < 2u)
        {
            return EZPI_IMPROV_ERROR_INVALID_RPC;
        }

        const uint8_t ssid_len = data[0];

        /* The passphrase length byte must itself be inside the payload. */
        if (((uint32_t)ssid_len + 1u) >= (uint32_t)data_len)
        {
            return EZPI_IMPROV_ERROR_INVALID_RPC;
        }

        const uint8_t pass_len = data[1u + ssid_len];

        if (((uint32_t)ssid_len + (uint32_t)pass_len + 2u) != (uint32_t)data_len)
        {
            return EZPI_IMPROV_ERROR_INVALID_RPC;
        }

        /* An empty SSID cannot be joined, and anything over the 802.11 limits
         * would be truncated silently by the Wi-Fi stack -- a client that sent
         * it would be told provisioning succeeded for a network it did not
         * ask for. */
        if ((0u == ssid_len) || (ssid_len > EZPI_IMPROV_SSID_MAX) ||
            (pass_len > EZPI_IMPROV_PASSPHRASE_MAX))
        {
            return EZPI_IMPROV_ERROR_INVALID_RPC;
        }

        parsed.ssid = &data[1];
        parsed.ssid_len = ssid_len;
        parsed.passphrase = &data[2u + ssid_len];
        parsed.passphrase_len = pass_len;
    }

    *out = parsed;
    return EZPI_IMPROV_ERROR_NONE;
}

void EZPI_improv_result_init(s_ezpi_improv_result_t *r, uint8_t *buf,
                             uint16_t cap, uint8_t command)
{
    if (NULL == r)
    {
        return;
    }

    r->buf = buf;
    r->cap = cap;
    r->pos = RPC_IDX_DATA;
    r->overflow = false;

    /* The command and length fields plus the trailing byte must fit before
     * anything is written, so that add() never has to re-check them. */
    if ((NULL == buf) || (cap < (RPC_OVERHEAD + 1u)))
    {
        r->overflow = true;
        return;
    }

    buf[RPC_IDX_COMMAND] = command;
    buf[RPC_IDX_LENGTH] = 0;
}

bool EZPI_improv_result_add(s_ezpi_improv_result_t *r, const uint8_t *data, uint8_t len)
{
    if ((NULL == r) || r->overflow)
    {
        return false;
    }

    if ((NULL == data) && (0u != len))
    {
        r->overflow = true;
        return false;
    }

    /* One length byte plus the data, and the trailing checksum slot still has
     * to fit afterwards. */
    const uint32_t needed = (uint32_t)r->pos + 1u + (uint32_t)len + 1u;

    if (needed > (uint32_t)r->cap)
    {
        r->overflow = true;
        return false;
    }

    /* The data-length field is a single byte, so the whole payload cannot
     * exceed 255 however much room the buffer has. */
    if (((uint32_t)r->pos + 1u + (uint32_t)len - RPC_OVERHEAD) > EZPI_IMPROV_DATA_MAX)
    {
        r->overflow = true;
        return false;
    }

    r->buf[r->pos] = len;
    r->pos++;

    if (0u != len)
    {
        memcpy(&r->buf[r->pos], data, len);
        r->pos = (uint16_t)(r->pos + len);
    }

    return true;
}

bool EZPI_improv_result_add_str(s_ezpi_improv_result_t *r, const char *str)
{
    if (NULL == str)
    {
        /* An empty entry, which is how the protocol spells "no redirect URL". */
        return EZPI_improv_result_add(r, NULL, 0);
    }

    const size_t len = strlen(str);

    if (len > EZPI_IMPROV_DATA_MAX)
    {
        if (NULL != r)
        {
            r->overflow = true;
        }
        return false;
    }

    return EZPI_improv_result_add(r, (const uint8_t *)str, (uint8_t)len);
}

uint16_t EZPI_improv_result_finish(s_ezpi_improv_result_t *r, bool add_checksum)
{
    if ((NULL == r) || r->overflow)
    {
        return 0;
    }

    r->buf[RPC_IDX_LENGTH] = (uint8_t)(r->pos - RPC_OVERHEAD);

    /* The trailing byte exists either way; it is zero when the enclosing
     * transport checksums the payload for us. */
    r->buf[r->pos] = add_checksum ? EZPI_improv_checksum(r->buf, r->pos) : 0u;
    r->pos++;

    return r->pos;
}

uint16_t EZPI_improv_frame_build(uint8_t *out, uint16_t cap, e_ezpi_improv_type_t type,
                                 const uint8_t *data, uint16_t data_len)
{
    if (NULL == out)
    {
        return 0;
    }

    if ((NULL == data) && (0u != data_len))
    {
        return 0;
    }

    if (data_len > EZPI_IMPROV_DATA_MAX)
    {
        return 0;
    }

    /* Overhead plus payload plus the trailing newline. */
    const uint32_t total = EZPI_IMPROV_FRAME_OVERHEAD + (uint32_t)data_len + 1u;

    if (total > (uint32_t)cap)
    {
        return 0;
    }

    memcpy(out, EZPI_IMPROV_MAGIC, EZPI_IMPROV_MAGIC_LEN);
    out[FRAME_IDX_VERSION] = EZPI_IMPROV_VERSION;
    out[FRAME_IDX_TYPE] = (uint8_t)type;
    out[FRAME_IDX_LENGTH] = (uint8_t)data_len;

    if (0u != data_len)
    {
        memcpy(&out[FRAME_IDX_DATA], data, data_len);
    }

    /* Everything before the checksum byte, which is the magic and the header
     * as well as the payload -- not the payload alone. */
    const uint16_t checksum_idx = (uint16_t)(FRAME_IDX_DATA + data_len);
    out[checksum_idx] = EZPI_improv_checksum(out, checksum_idx);

    /* Not in the published specification, which says the protocol has no
     * newline terminator, but present in every frame the reference
     * implementation emits. See the header. */
    out[checksum_idx + 1u] = (uint8_t)'\n';

    return (uint16_t)total;
}

void EZPI_improv_rx_reset(s_ezpi_improv_rx_t *rx)
{
    if (NULL != rx)
    {
        rx->len = 0;
        rx->restart = false;
        rx->complete = false;
    }
}

uint8_t EZPI_improv_rx_type(const s_ezpi_improv_rx_t *rx)
{
    if ((NULL == rx) || (rx->len <= FRAME_IDX_TYPE))
    {
        return 0;
    }

    return rx->buf[FRAME_IDX_TYPE];
}

const uint8_t *EZPI_improv_rx_payload(const s_ezpi_improv_rx_t *rx, uint16_t *len)
{
    if (NULL != len)
    {
        *len = 0;
    }

    if ((NULL == rx) || (rx->len < EZPI_IMPROV_FRAME_OVERHEAD))
    {
        return NULL;
    }

    if (NULL != len)
    {
        *len = rx->buf[FRAME_IDX_LENGTH];
    }

    return &rx->buf[FRAME_IDX_DATA];
}

/**
 * @brief Hand back the buffered candidate because it was not a frame.
 *
 * @param rx               Receive state.
 * @param byte             The byte that broke the match.
 * @param passthrough      Set to the bytes the caller must forward.
 * @param passthrough_len  Set to how many.
 */
static e_ezpi_improv_rx_result_t ezpi_improv_rx_bail(s_ezpi_improv_rx_t *rx, uint8_t byte,
                                                     const uint8_t **passthrough,
                                                     uint16_t *passthrough_len)
{
    const uint16_t prefix = rx->len;

    if ((uint8_t)EZPI_IMPROV_MAGIC[0] == byte)
    {
        /* The byte that broke this match may begin the next one, as in a
         * stream of "II M P R O V". It cannot be written to buf[0] here: buf
         * still holds the prefix being handed back, and overwriting byte 0
         * would corrupt what the caller is about to read. Defer it. */
        *passthrough = rx->buf;
        *passthrough_len = prefix;
        rx->restart = true;
    }
    else
    {
        /* Appending is safe: a mismatch is only ever detected in the magic or
         * version bytes, so prefix is at most FRAME_IDX_VERSION. Every later
         * byte is accepted on sight -- the length field is what ends the frame,
         * and an unsupported type or payload is rejected after the frame has
         * been consumed, not part-way through it. */
        rx->buf[prefix] = byte;
        *passthrough = rx->buf;
        *passthrough_len = (uint16_t)(prefix + 1u);
        rx->len = 0;
        rx->restart = false;
    }

    return EZPI_IMPROV_RX_NOT_IMPROV;
}

e_ezpi_improv_rx_result_t EZPI_improv_rx_feed(s_ezpi_improv_rx_t *rx, uint8_t byte,
                                              const uint8_t **passthrough,
                                              uint16_t *passthrough_len)
{
    const uint8_t *sink = NULL;
    uint16_t sink_len = 0;

    /* Let callers that do not share the port pass NULL for both. */
    if (NULL == passthrough)
    {
        passthrough = &sink;
    }
    if (NULL == passthrough_len)
    {
        passthrough_len = &sink_len;
    }

    *passthrough = NULL;
    *passthrough_len = 0;

    if (NULL == rx)
    {
        return EZPI_IMPROV_RX_NOT_IMPROV;
    }

    /* The previous call completed a frame and the caller has now had its
     * chance to read the payload, so the buffer is free to reuse. Doing this
     * here rather than demanding the caller reset is what keeps a maximal
     * frame followed by another byte from indexing past the end of buf. */
    if (rx->complete)
    {
        rx->len = 0;
        rx->complete = false;
        rx->restart = false;
    }

    /* A deferred restart from the previous call, now that the caller has had
     * its passthrough bytes. */
    if (rx->restart)
    {
        rx->buf[0] = (uint8_t)EZPI_IMPROV_MAGIC[0];
        rx->len = 1;
        rx->restart = false;
    }

    const uint16_t at = rx->len;

    /* Magic, one byte at a time. */
    if (at < EZPI_IMPROV_MAGIC_LEN)
    {
        if ((uint8_t)EZPI_IMPROV_MAGIC[at] != byte)
        {
            return ezpi_improv_rx_bail(rx, byte, passthrough, passthrough_len);
        }

        rx->buf[at] = byte;
        rx->len = (uint16_t)(at + 1u);
        return EZPI_IMPROV_RX_NEED_MORE;
    }

    /* Only version 1 exists. An unknown version is not an Improv frame as far
     * as this codec is concerned, and guessing at a future layout would be
     * worse than handing the bytes back. */
    if (FRAME_IDX_VERSION == at)
    {
        if (EZPI_IMPROV_VERSION != byte)
        {
            return ezpi_improv_rx_bail(rx, byte, passthrough, passthrough_len);
        }

        rx->buf[at] = byte;
        rx->len = (uint16_t)(at + 1u);
        return EZPI_IMPROV_RX_NEED_MORE;
    }

    /* Type and length are accepted as they come; the length is what tells us
     * where the frame ends, and an unsupported type is rejected by the
     * transport after the frame has been consumed in full. Consuming it is the
     * point: bailing out here would leave the payload to be misread. */
    if ((FRAME_IDX_TYPE == at) || (FRAME_IDX_LENGTH == at))
    {
        rx->buf[at] = byte;
        rx->len = (uint16_t)(at + 1u);
        return EZPI_IMPROV_RX_NEED_MORE;
    }

    const uint16_t data_len = rx->buf[FRAME_IDX_LENGTH];
    const uint16_t checksum_at = (uint16_t)(FRAME_IDX_DATA + data_len);

    if (at < checksum_at)
    {
        rx->buf[at] = byte;
        rx->len = (uint16_t)(at + 1u);
        return EZPI_IMPROV_RX_NEED_MORE;
    }

    /* The checksum byte completes the frame either way: a wrong checksum is
     * reported as such and the buffer is cleared, so the stream stays in sync
     * instead of the remaining bytes being rescanned as a new frame. */
    rx->buf[at] = byte;
    rx->len = (uint16_t)(at + 1u);

    const uint8_t expected = EZPI_improv_checksum(rx->buf, checksum_at);

    if (expected != byte)
    {
        rx->len = 0;
        rx->restart = false;
        rx->complete = false;
        return EZPI_IMPROV_RX_BAD_CHECKSUM;
    }

    rx->complete = true;
    return EZPI_IMPROV_RX_FRAME;
}
