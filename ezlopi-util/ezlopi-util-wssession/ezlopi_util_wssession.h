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
 * @file    ezlopi_util_wssession.h
 * @brief   Who is logged in on the local API, per connection rather than once
 *          for the whole hub
 * @author
 * @version 1.0
 * @date    13th OCT 2025
 *
 * WHY THIS EXISTS
 *
 * The local websocket API on port 17001 gates every method behind
 * EZPI_core_offline_is_user_logged_in(). That check used to read one
 * file-static bool:
 *
 *     static bool logged_in = false;          // ezlopi_core_offline_login.c
 *
 * One bool for the whole hub, with nothing tying it to the connection that
 * authenticated. Three consequences, all of them reachable from the local
 * network -- the server binds INADDR_ANY, not loopback:
 *
 *   1. ANY PEER INHERITS A SESSION. While a legitimate client is logged in,
 *      the flag is true, so a second connection from anywhere on the LAN
 *      passes the gate in __message_upcall() and may call any API method
 *      without ever presenting the local key. That is an authentication
 *      bypass, not merely an oversight in bookkeeping.
 *   2. ANY PEER CAN LOG EVERYONE OUT. A websocket CLOSE frame reaches
 *      EZPI_core_offline_logout_perform() with no login check at all, which
 *      flips the one flag false and ends the legitimate client's session.
 *      Repeatable, and it needs no credential.
 *   3. A SECOND LEGITIMATE CLIENT CANNOT LOG IN. login_perform() refuses when
 *      the flag is already set, so the honest second client is turned away
 *      while the dishonest one is waved through -- precisely backwards.
 *
 * THE RULE. Authorisation belongs to a connection. A session is created when a
 * socket is accepted, carries its own authorised flag, and is destroyed when
 * that socket is closed. Nothing another connection does can set or clear it.
 *
 * WHY KEYING ON THE SOCKET DESCRIPTOR IS SAFE, AND WHERE IT WOULD NOT BE
 *
 * A descriptor identifies a connection only for as long as that connection is
 * open; the operating system is free to hand the same number to the next
 * accepted socket. So a table keyed on the descriptor is only correct if an
 * entry cannot outlive its connection. Two things make that hold here:
 *
 *   - EZPI_util_wssession_open() ALWAYS resets the entry for its descriptor,
 *     whether or not one was already there. A new connection therefore starts
 *     unauthorised even if the close for the previous owner of that number was
 *     never delivered. Correctness does not depend on the close arriving,
 *     which is the property worth having, because a missed close is exactly
 *     the case that would otherwise hand a stranger a live session.
 *   - The caller drives open from the server's accept hook and close from its
 *     own close hook, and ESP-IDF documents the latter as running "for all
 *     terminated sessions, including sessions where the socket was closed by
 *     the network stack". It is not conditional on a well-behaved client
 *     sending a CLOSE frame, which is what the old code relied on.
 *
 * This is deliberately NOT a bearer token. There is no secret here to steal,
 * because nothing is handed to the client: the key is the kernel's own idea of
 * which connection a byte arrived on. A peer cannot forge it without already
 * owning the socket.
 *
 * WHAT IT DOES NOT DEFEND AGAINST, SAID PLAINLY. The transport is plain ws://,
 * so anyone able to read or inject on the path can still take over a connection
 * at the TCP level; per-connection sessions do not change that, and only TLS
 * would. The point of this component is that an ordinary second peer -- one
 * that can merely connect -- no longer inherits anyone's authority.
 *
 * TESTING. tests/host/test_wssession.c compiles this file itself and
 * enumerates the properties, including the two the old model fails. It keeps a
 * model of that old global-bool behaviour and asserts it fails them, so the
 * defect is demonstrated rather than claimed.
 */

#ifndef _EZLOPI_UTIL_WSSESSION_H_
#define _EZLOPI_UTIL_WSSESSION_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /**
     * @brief How many concurrent local connections can hold a session.
     *
     * ESP-IDF's HTTPD_DEFAULT_CONFIG sets max_open_sockets to 7 and the
     * websocket server does not override it, so the server cannot present more
     * than seven live sockets at once and this table cannot be filled by
     * ordinary use. The spare entry exists so that a change to
     * max_open_sockets shows up as a failed session rather than as silent
     * reuse of someone else's.
     *
     * If it ever does fill, EZPI_util_wssession_open() fails and every query
     * for that descriptor answers "not authorised". Failing closed is the only
     * acceptable direction here: the alternative is admitting an
     * unauthenticated caller.
     */
#define EZPI_WSSESSION_MAX 8

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief   Begin an unauthenticated session for a freshly accepted socket.
     *
     * Always resets the state for this descriptor. Calling it for a descriptor
     * that still has an entry -- which happens when a close was never
     * delivered and the number has been reused -- discards the old entry
     * rather than adopting it.
     *
     * @param   fd  The socket descriptor the server just accepted.
     * @return  true if a session slot is held for fd; false if the table is
     *          full or fd is negative, in which case that connection can never
     *          become authorised.
     */
    bool EZPI_util_wssession_open(int fd);

    /**
     * @brief   Forget the session for a closed socket.
     *
     * Safe to call for a descriptor with no session, which happens when the
     * server reports a close for a socket that was rejected at accept time.
     *
     * @param   fd  The socket descriptor that has been closed.
     */
    void EZPI_util_wssession_close(int fd);

    /**
     * @brief   Record that this connection presented the correct local key.
     *
     * @param   fd  The connection that authenticated.
     * @return  true if the session was marked authorised; false if fd has no
     *          open session, in which case nothing is granted.
     */
    bool EZPI_util_wssession_authorise(int fd);

    /**
     * @brief   Whether this connection may call the API.
     *
     * @param   fd  The connection a request arrived on.
     * @return  true only if fd has an open session that authenticated. A
     *          descriptor with no session, or one that has not logged in,
     *          answers false -- including when some other connection is
     *          logged in.
     */
    bool EZPI_util_wssession_is_authorised(int fd);

    /**
     * @brief   Drop every session.
     *
     * For server shutdown, where all sockets go away at once, and for tests.
     */
    void EZPI_util_wssession_reset_all(void);

    /**
     * @brief   How many sessions are currently held.
     *
     * Exposed so that a leak -- sessions accumulating because closes are not
     * being delivered -- is observable rather than invisible.
     *
     * @return  The number of open sessions, authorised or not.
     */
    int EZPI_util_wssession_count(void);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_WSSESSION_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
