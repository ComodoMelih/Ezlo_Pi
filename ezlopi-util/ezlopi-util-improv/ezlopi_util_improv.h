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
 * @file    ezlopi_util_improv.h
 * @brief   Improv Wi-Fi protocol codec, independent of any transport.
 *
 * Improv Wi-Fi (https://www.improv-wifi.com/) is carried over two transports
 * that share one payload format: BLE, where each field is a GATT
 * characteristic, and Serial, where the same RPC payload is wrapped in a
 * framed packet on a UART. This component holds the parts that are identical
 * between them -- the checksum, the RPC payload grammar, the result builder
 * and the serial frame codec -- so that the two transports cannot disagree
 * about the wire format.
 *
 * Like ezlopi-util-sanitize, it has no dependency beyond the C library and no
 * REQUIRES in its CMakeLists. That is deliberate and load-bearing: it lets
 * tests/host compile this file with a plain host compiler under ASan/UBSan and
 * actually exercise the parser against truncated, oversized and hostile input.
 * Nothing here touches ESP-IDF, FreeRTOS, the Wi-Fi stack or the heap; keep it
 * that way or the tests stop building.
 *
 * ---------------------------------------------------------------------------
 * Where these constants come from
 * ---------------------------------------------------------------------------
 * The published specification at improv-wifi.com documents the packet layout
 * but *not* the checksum algorithm, and it is actively wrong about framing: it
 * states the protocol "does not use newline terminators", while the reference
 * implementation appends one to every transmitted frame. Both of those were
 * taken from the reference sources rather than the prose:
 *
 *   improv-wifi/sdk-cpp  src/improv.cpp, src/improv.h
 *   esphome              components/improv_serial/improv_serial_component.{h,cpp}
 *
 * What those establish, and what this file implements:
 *
 *   - Checksum is an 8-bit wrapping sum of every byte that precedes the
 *     checksum byte. On a serial frame that includes the six magic bytes, the
 *     version, the type and the length -- not just the payload.
 *       improv.cpp parse_improv_serial_byte():
 *           for (i = 0; i < position; i++) checksum += buffer[i];
 *   - A transmitted frame ends with '\n' *after* the checksum, and that
 *     newline is not part of the checksum.
 *       improv_serial_component.h: TX_CHECKSUM_IDX = 10, TX_NEWLINE_IDX = 11.
 *   - A serial frame's RPC payload carries no checksum of its own; the frame
 *     checksum covers it. BLE has no frame, so there the payload carries the
 *     checksum itself. Hence the has_checksum argument below, which mirrors
 *     the reference's check_checksum parameter.
 *       improv.cpp: parse_improv_data(&buffer[9], data_len, false)
 *   - There is no "identify" RPC. The sdk-cpp enum defines IDENTIFY = 0x02 and
 *     GET_CURRENT_STATE = 0x02 -- the same value -- and the reference serial
 *     dispatch only ever treats 0x02 as a current-state request.
 */

#ifndef _EZLOPI_UTIL_IMPROV_H_
#define _EZLOPI_UTIL_IMPROV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*-------------------------------------------------------------------------
     * Protocol constants
     *-----------------------------------------------------------------------*/

    /** @brief Serial frame magic, bytes 0..5. Not NUL terminated on the wire. */
#define EZPI_IMPROV_MAGIC "IMPROV"

    /** @brief Length of the magic, in bytes. */
#define EZPI_IMPROV_MAGIC_LEN 6u

    /** @brief The only protocol version that exists. */
#define EZPI_IMPROV_VERSION 1u

    /**
     * @brief Bytes in a serial frame other than its data.
     *
     * magic (6) + version (1) + type (1) + length (1) + checksum (1). The
     * trailing newline is deliberately excluded: it is appended on transmit
     * and tolerated on receive, but it is not part of the framing that the
     * length field describes.
     */
#define EZPI_IMPROV_FRAME_OVERHEAD 10u

    /** @brief The length field is one byte, so this is the hard maximum. */
#define EZPI_IMPROV_DATA_MAX 255u

    /**
     * @brief Largest serial frame this codec will accept or produce, excluding
     *        the trailing newline.
     *
     * Sized to the protocol maximum rather than to the largest payload EzloPi
     * happens to understand. A client is entitled to send a longer frame than
     * we support; buffering it in full and rejecting it at the parse step
     * keeps the byte stream in sync, whereas bailing out mid-frame would leave
     * the remaining bytes to be misread as a new frame or as JSON.
     */
#define EZPI_IMPROV_FRAME_MAX (EZPI_IMPROV_FRAME_OVERHEAD + EZPI_IMPROV_DATA_MAX)

    /** @brief Frame buffer size including room for the trailing newline. */
#define EZPI_IMPROV_FRAME_BUF_MAX (EZPI_IMPROV_FRAME_MAX + 1u)

    /** @brief Wi-Fi limits, enforced here so both transports agree. */
#define EZPI_IMPROV_SSID_MAX 32u
#define EZPI_IMPROV_PASSPHRASE_MAX 64u

    /** @brief Serial packet types (frame byte 7). */
    typedef enum
    {
        EZPI_IMPROV_TYPE_CURRENT_STATE = 0x01,
        EZPI_IMPROV_TYPE_ERROR_STATE = 0x02,
        EZPI_IMPROV_TYPE_RPC = 0x03,
        EZPI_IMPROV_TYPE_RPC_RESULT = 0x04,
    } e_ezpi_improv_type_t;

    /** @brief Provisioning state, reported on both transports. */
    typedef enum
    {
        EZPI_IMPROV_STATE_STOPPED = 0x00,
        EZPI_IMPROV_STATE_AWAITING_AUTHORIZATION = 0x01,
        EZPI_IMPROV_STATE_AUTHORIZED = 0x02,
        EZPI_IMPROV_STATE_PROVISIONING = 0x03,
        EZPI_IMPROV_STATE_PROVISIONED = 0x04,
    } e_ezpi_improv_state_t;

    /** @brief Error codes, reported on both transports. */
    typedef enum
    {
        EZPI_IMPROV_ERROR_NONE = 0x00,
        EZPI_IMPROV_ERROR_INVALID_RPC = 0x01,
        EZPI_IMPROV_ERROR_UNKNOWN_RPC = 0x02,
        EZPI_IMPROV_ERROR_UNABLE_TO_CONNECT = 0x03,
        EZPI_IMPROV_ERROR_NOT_AUTHORIZED = 0x04,
        EZPI_IMPROV_ERROR_BAD_HOSTNAME = 0x05,
        EZPI_IMPROV_ERROR_UNKNOWN = 0xFF,
    } e_ezpi_improv_error_t;

    /**
     * @brief RPC command identifiers.
     *
     * 0x02 is "request current state". The reference enum also spells it
     * IDENTIFY with the same numeric value; there is no distinct identify
     * command, and claiming one would make a client draw a button that cannot
     * do anything. See the file comment.
     */
    typedef enum
    {
        EZPI_IMPROV_RPC_SEND_WIFI = 0x01,
        EZPI_IMPROV_RPC_GET_CURRENT_STATE = 0x02,
        EZPI_IMPROV_RPC_GET_DEVICE_INFO = 0x03,
        EZPI_IMPROV_RPC_GET_WIFI_NETWORKS = 0x04,
    } e_ezpi_improv_rpc_t;

    /** @brief Capability bits for the BLE capabilities characteristic. */
#define EZPI_IMPROV_CAP_NONE 0x00u
#define EZPI_IMPROV_CAP_IDENTIFY 0x01u

    /*-------------------------------------------------------------------------
     * Checksum
     *-----------------------------------------------------------------------*/

    /**
     * @brief 8-bit wrapping sum of @p len bytes starting at @p data.
     *
     * The accumulator is deliberately wider than the result and truncated at
     * the end, matching the reference implementation exactly. Summing into a
     * uint8_t would give the same answer, but the reference's intent is
     * clearer this way and it leaves no doubt that the wrap is intentional
     * rather than an overlooked overflow.
     *
     * @param data  Bytes to sum. May be NULL only if @p len is 0.
     * @param len   Number of bytes.
     * @return The low byte of the sum.
     */
    uint8_t EZPI_improv_checksum(const uint8_t *data, uint32_t len);

    /*-------------------------------------------------------------------------
     * RPC payload parsing
     *-----------------------------------------------------------------------*/

    /**
     * @brief One decoded RPC command.
     *
     * For EZPI_IMPROV_RPC_SEND_WIFI, @c ssid and @c passphrase point into the
     * caller's buffer rather than to copies, so they are valid only as long as
     * that buffer is. Neither is NUL terminated: an SSID may legitimately
     * contain any byte, including 0x00, so the length is the only safe
     * delimiter. Callers that need C strings must copy and terminate.
     */
    typedef struct
    {
        uint8_t command;
        const uint8_t *ssid;
        uint8_t ssid_len;
        const uint8_t *passphrase;
        uint8_t passphrase_len;
    } s_ezpi_improv_rpc_t;

    /**
     * @brief Validate and decode an RPC payload.
     *
     * The payload grammar is [command][data length][data ...] optionally
     * followed by [checksum]. Every length in it is attacker-chosen, so the
     * declared length is checked against the length that actually arrived
     * before any byte of the data is indexed.
     *
     * @param payload       Payload bytes.
     * @param len           Number of bytes available at @p payload.
     * @param has_checksum  True when the payload carries its own trailing
     *                      checksum byte (BLE). False when an enclosing frame
     *                      already checksums it (Serial).
     * @param out           Filled in on success. Untouched on failure.
     * @return EZPI_IMPROV_ERROR_NONE on success, otherwise the error that
     *         should be reported to the client.
     */
    e_ezpi_improv_error_t EZPI_improv_rpc_parse(const uint8_t *payload, uint32_t len,
                                                bool has_checksum, s_ezpi_improv_rpc_t *out);

    /*-------------------------------------------------------------------------
     * RPC result building
     *-----------------------------------------------------------------------*/

    /**
     * @brief Cursor for building an RPC result into caller-owned memory.
     *
     * No allocation anywhere: the caller supplies the buffer. Overflow is
     * recorded in @c overflow and is sticky, so a caller may append several
     * entries and check once at the end instead of after each one.
     */
    typedef struct
    {
        uint8_t *buf;
        uint16_t cap;
        uint16_t pos;
        bool overflow;
    } s_ezpi_improv_result_t;

    /**
     * @brief Begin an RPC result for @p command.
     *
     * @param r        Cursor to initialise.
     * @param buf      Destination buffer.
     * @param cap      Capacity of @p buf in bytes.
     * @param command  The command being responded to.
     */
    void EZPI_improv_result_init(s_ezpi_improv_result_t *r, uint8_t *buf,
                                 uint16_t cap, uint8_t command);

    /**
     * @brief Append one length-prefixed entry.
     *
     * @param r    Cursor.
     * @param data Entry bytes. May be NULL only if @p len is 0.
     * @param len  Entry length. Must be <= EZPI_IMPROV_DATA_MAX.
     * @return True if appended. False if it would not fit, or if the cursor
     *         has already overflowed, in which case nothing is written.
     */
    bool EZPI_improv_result_add(s_ezpi_improv_result_t *r, const uint8_t *data, uint8_t len);

    /**
     * @brief Append one NUL-terminated string as an entry.
     *
     * @param r   Cursor.
     * @param str String to append. NULL is treated as an empty entry, which is
     *            what the protocol uses for "no redirect URL".
     * @return As EZPI_improv_result_add().
     */
    bool EZPI_improv_result_add_str(s_ezpi_improv_result_t *r, const char *str);

    /**
     * @brief Finalise the result and return its length.
     *
     * Writes the data-length field and the trailing byte. The trailing byte is
     * the payload checksum when @p add_checksum is true and 0x00 otherwise --
     * the slot exists either way, which is what the reference does.
     *
     * @param r             Cursor.
     * @param add_checksum  True for BLE, false for Serial.
     * @return Total payload length, or 0 if the build overflowed.
     */
    uint16_t EZPI_improv_result_finish(s_ezpi_improv_result_t *r, bool add_checksum);

    /*-------------------------------------------------------------------------
     * Serial frame building
     *-----------------------------------------------------------------------*/

    /**
     * @brief Build a complete serial frame, newline included.
     *
     * The whole frame is produced in one buffer so that the caller can hand it
     * to a single write. That matters on a shared UART: the console logger
     * writes to the same port from other tasks, and a frame emitted as several
     * writes can be interrupted part-way and corrupted. The reference
     * implementation uses three writes; one is strictly safer.
     *
     * @param out       Destination, at least EZPI_IMPROV_FRAME_BUF_MAX bytes
     *                  for an arbitrary payload.
     * @param cap       Capacity of @p out.
     * @param type      Packet type.
     * @param data      Payload. May be NULL only if @p data_len is 0.
     * @param data_len  Payload length, at most EZPI_IMPROV_DATA_MAX.
     * @return Total bytes written, or 0 if the frame would not fit.
     */
    uint16_t EZPI_improv_frame_build(uint8_t *out, uint16_t cap, e_ezpi_improv_type_t type,
                                     const uint8_t *data, uint16_t data_len);

    /*-------------------------------------------------------------------------
     * Serial frame receiving
     *-----------------------------------------------------------------------*/

    /** @brief Outcome of feeding one received byte. */
    typedef enum
    {
        /** Byte consumed as part of a frame that is not yet complete. */
        EZPI_IMPROV_RX_NEED_MORE = 0,

        /**
         * The candidate was not an Improv frame after all. Bytes handed back
         * through the passthrough arguments belong to whatever else shares the
         * port and must be forwarded there.
         */
        EZPI_IMPROV_RX_NOT_IMPROV,

        /** A complete frame with a valid checksum is available. */
        EZPI_IMPROV_RX_FRAME,

        /** A complete frame arrived but its checksum was wrong. */
        EZPI_IMPROV_RX_BAD_CHECKSUM,
    } e_ezpi_improv_rx_result_t;

    /**
     * @brief Receive state for one serial stream.
     *
     * Zero-initialise before first use; there is no init function and no
     * allocation. Roughly 270 bytes, sized by the protocol maximum.
     */
    typedef struct
    {
        uint8_t buf[EZPI_IMPROV_FRAME_MAX];
        uint16_t len;
        /**
         * Set when a mismatching byte was itself the start of a possible new
         * frame. It cannot be stored into buf immediately, because buf still
         * holds the bytes being handed back to the caller for passthrough and
         * writing to buf[0] would corrupt them. It is consumed at the top of
         * the next call instead.
         */
        bool restart;
        /**
         * Set once a frame is complete, so that buf survives for the caller to
         * read the payload out of, and is cleared on the next byte.
         *
         * This is not merely tidiness. Without it, feeding a further byte
         * while len still described a finished frame would index one past the
         * end of buf for a maximum-length payload, and the contract "the
         * caller must remember to reset" is the kind that eventually is not
         * honoured. The state machine recovers by itself instead.
         */
        bool complete;
    } s_ezpi_improv_rx_t;

    /**
     * @brief Feed one received byte to the frame scanner.
     *
     * Written as a byte-wise state machine rather than a line splitter on
     * purpose. Improv frames are binary: a length, a checksum or a passphrase
     * byte can be 0x0a, so splitting the stream on newlines would cut frames
     * in half. The length field is the only correct delimiter.
     *
     * Bytes that turn out not to belong to a frame are handed back rather than
     * dropped, so this can sit in front of another protocol on the same UART
     * without eating its traffic. A lone 'I' inside a JSON string is handed
     * back on the following byte.
     *
     * @param rx               Receive state.
     * @param byte             The byte just read.
     * @param passthrough      On EZPI_IMPROV_RX_NOT_IMPROV, set to the bytes
     *                         to forward elsewhere. Valid only until the next
     *                         call. Set to NULL otherwise.
     * @param passthrough_len  On EZPI_IMPROV_RX_NOT_IMPROV, the number of
     *                         bytes at @p passthrough. Zero otherwise.
     * @return What happened to the byte.
     */
    e_ezpi_improv_rx_result_t EZPI_improv_rx_feed(s_ezpi_improv_rx_t *rx, uint8_t byte,
                                                 const uint8_t **passthrough,
                                                 uint16_t *passthrough_len);

    /**
     * @brief Discard a partially received frame.
     *
     * The transport calls this when the stream has been quiet long enough that
     * a half-arrived frame should be abandoned. The reference implementation
     * uses a 100 ms idle timeout for this, and it is the only resynchronisation
     * the protocol has.
     *
     * @param rx Receive state.
     */
    void EZPI_improv_rx_reset(s_ezpi_improv_rx_t *rx);

    /**
     * @brief Packet type of a completed frame.
     * @param rx Receive state, after EZPI_IMPROV_RX_FRAME.
     * @return The type byte.
     */
    uint8_t EZPI_improv_rx_type(const s_ezpi_improv_rx_t *rx);

    /**
     * @brief Payload of a completed frame.
     *
     * @param rx   Receive state, after EZPI_IMPROV_RX_FRAME.
     * @param len  Set to the payload length.
     * @return Pointer to the payload, or NULL if no frame is complete.
     */
    const uint8_t *EZPI_improv_rx_payload(const s_ezpi_improv_rx_t *rx, uint16_t *len);

#ifdef __cplusplus
}
#endif

#endif /* _EZLOPI_UTIL_IMPROV_H_ */
