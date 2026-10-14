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
 * @file    ezlopi_core_offline_login.h
 * @brief   Function to perform offine login operation
 * @author
 * @version 1.0
 * @date    12th DEC 2024
 */

#ifndef _EZLOPI_CORE_OFFLINE_LOGIN_H_
#define _EZLOPI_CORE_OFFLINE_LOGIN_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "cjext.h"
#include "ezlopi_core_errors.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Function to trigger login check operation
     *
     * Authorises the connection claimed by
     * EZPI_core_offline_login_claim_connection() on this task, and no other.
     * Fails if no connection is claimed, which is the case on the cloud
     * dispatch path: there is no local socket there to grant anything to.
     *
     * @param cj_params cjson obj login info
     * @return ezlopi_error_t
     */
    ezlopi_error_t EZPI_core_offline_login_perform(cJSON *cj_params);
    /**
     * @brief Function to perform logout operation
     *
     * Ends the session of the claimed connection only. A connection that has
     * simply gone away is handled by the transport's close hook calling
     * EZPI_util_wssession_close(), not by this.
     *
     * NOTHING CURRENTLY CALLS THIS, and that is deliberate rather than an
     * oversight. It had three call sites, all of which were connection
     * teardown -- two on a websocket CLOSE frame and one on repeated send
     * failure. All three are now the close hook's job, which is both correct
     * for the connection involved and reached even when the socket dies
     * without a CLOSE frame.
     *
     * It is kept because it is public core API and is now correct if called:
     * an explicit "log me out but keep the socket open" is a reasonable method
     * for someone to add. What must not happen is it being re-wired to
     * teardown, because the version that did that was how any unauthenticated
     * peer could log every real client out -- it ended whatever session was
     * claimed, which under the old global flag was everybody's.
     *
     * @return ezlopi_error_t
     */
    ezlopi_error_t EZPI_core_offline_logout_perform();
    /**
     * @brief Function to check if user is logged in
     *
     * Answers for the claimed connection. This used to read one bool shared by
     * the whole hub, which meant any peer that could reach port 17001 was
     * admitted while anyone at all was logged in; see the note in the .c file.
     *
     * @return true
     * @return false
     */
    bool EZPI_core_offline_is_user_logged_in();
    /**
     * @brief Record which connection the request being dispatched arrived on.
     *
     * The transport must call this immediately before dispatching a request
     * into EZPI_core_api_consume*() and release it immediately afterwards, so
     * that the login, logout and query functions above act on the right
     * connection. Method handlers have no access to the transport, which is
     * why the connection is passed this way rather than as an argument.
     *
     * Only honoured on the task that called it: another task is told there is
     * no connection. Two tasks dispatch methods -- the cloud websocket client
     * and the local websocket server -- so this matters.
     *
     * @param connection The socket descriptor, or a negative value to release.
     */
    void EZPI_core_offline_login_claim_connection(int connection);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CORE_OFFLINE_LOGIN_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
