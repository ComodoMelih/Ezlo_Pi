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
 * @file    ezlopi_service_improv_serial.h
 * @brief   Improv Wi-Fi provisioning over UART0, alongside the existing
 *          EzloPi serial protocol rather than instead of it.
 *
 * UART0 already has an owner. ezlopi-service-uart reads it, and the port is
 * also the IDF console; all three of the console, the EzloPi serial protocol
 * and ESP Web Tools run at 115200, so nothing about the port's configuration
 * has to change for Improv to work on it. What does have to change is how the
 * bytes are handled.
 *
 * The existing reader does this:
 *
 *     uart_rx_data[rxBytes] = 0;
 *     TRACE_I("%s", uart_rx_data);
 *     ezpi_service_uart_parser((const char *)uart_rx_data);
 *
 * -- it NUL-terminates the buffer and treats it as a C string, and the parser
 * walks it with `while ('\0' != *at)`. Improv frames are binary and routinely
 * contain 0x00: a bare "get current state" RPC has a data length of exactly
 * 0x00, so that single frame alone would be cut in half. Interception has to
 * happen on the byte buffer, before the string conversion, which is why this
 * is a filter the reader calls rather than another case in the parser.
 *
 * For the same reason the framing is byte-wise and never newline-based. A
 * length byte, a checksum byte or any payload byte can be 0x0a, so splitting
 * the stream on newlines would cut frames in half just as surely. The length
 * field is the only correct delimiter.
 *
 * Non-Improv bytes come back out of the filter unchanged and in order, so the
 * EzloPi protocol sees exactly what it saw before. With
 * CONFIG_EZPI_ENABLE_IMPROV_SERIAL off, the filter is not compiled and the
 * reader does not call it, so the existing path is untouched rather than
 * merely unaffected.
 */

#ifndef __EZLOPI_SERVICE_IMPROV_SERIAL_H__
#define __EZLOPI_SERVICE_IMPROV_SERIAL_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief Register the serial transport with the Improv core.
     *
     * Called once from the UART service's own init, after the port is open.
     * Does not configure or claim the port: UART0 belongs to
     * ezlopi-service-uart, and this transport only borrows it.
     */
    void EZPI_SERV_improv_serial_init(void);

    /**
     * @brief Take Improv frames out of a block of received bytes.
     *
     * Every byte is fed to the frame parser. Bytes that are not part of an
     * Improv frame are copied to @p out in the order they arrived; a complete
     * frame is dispatched to the Improv core and consumed, contributing
     * nothing to @p out.
     *
     * @p out may safely alias @p in. The filter never writes more bytes than
     * it has consumed, so an in-place compaction cannot overrun the bytes it
     * has yet to read.
     *
     * @param in      Received bytes. May contain 0x00 anywhere.
     * @param len     How many bytes in @p in.
     * @param out     Where the non-Improv bytes go.
     * @param out_cap Capacity of @p out. Must be at least @p len for no bytes
     *                to be dropped, which is what the caller passing the same
     *                buffer guarantees.
     * @return Number of bytes written to @p out.
     */
    uint32_t EZPI_SERV_improv_serial_filter(const uint8_t *in, uint32_t len,
                                            uint8_t *out, uint32_t out_cap);

    /**
     * @brief Discard a partially received frame after an idle period.
     *
     * The protocol has no escape sequence and no frame terminator that could
     * not also be data, so a truncated frame is only recoverable by timing out
     * and starting again -- which is how the reference implementation does it
     * too. Call this when the port has been quiet.
     */
    void EZPI_SERV_improv_serial_idle(void);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_SERVICE_IMPROV_SERIAL_H__

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
