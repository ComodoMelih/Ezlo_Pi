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
 * @file    ezlopi_core_offline_login.c
 * @brief   Function to perform offine login operation
 * @author
 * @version 1.0
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cjext.h"
#include "ezlopi_core_nvs.h"
#include "EZLOPI_USER_CONFIG.h"
#include "ezlopi_core_factory_info.h"
#include "ezlopi_core_local_key.h"
#include "ezlopi_core_offline_login.h"
#include "ezlopi_util_localkey.h"
#include "ezlopi_util_wssession.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static bool __user_id_accepted(const char *supplied_user);
static int __current_connection(void);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/* WHAT USED TO BE HERE
**
**     static bool logged_in = false;
**
** One flag for the whole hub, with nothing tying it to the connection a
** request arrived on. The local websocket server binds INADDR_ANY, so while
** one client was logged in, any other peer on the network passed the gate in
** __message_upcall() and could call any API method without presenting the
** local key; and a websocket CLOSE frame from any peer reached
** EZPI_core_offline_logout_perform() with no check of who sent it, ending the
** legitimate client's session. Authorisation now lives in
** ezlopi-util-wssession, keyed on the connection, and is host-tested.
**
** THE CONNECTION A REQUEST ARRIVED ON
**
** The API dispatcher does not carry the connection's identity -- every method
** handler has the signature (cj_request, cj_response) -- so the transport
** records it here for the duration of one dispatch, and the handler for
** "hub.offline.login.ui" reads it back.
**
** The task handle is stored alongside it, and the value is honoured only when
** read from the task that claimed it. That is not belt-and-braces: two
** different tasks dispatch methods through EZPI_core_api_consume*(), the cloud
** websocket client in ezlopi_service_webprov.c and the local server in
** ezlopi_service_ws_server.c, which is the same hazard ezlopi_core_ota.c
** documents at its __ota_guard_spinlock. Without the ownership check, a login
** arriving over the cloud while the local server happened to have a
** connection claimed would authorise that local connection instead. With it,
** a read from any other task answers "no connection", so the login simply
** fails -- the safe direction.
**
** No lock is needed for the pair, because only the claiming task ever writes
** them and only that task can satisfy the ownership test. A reader on another
** task sees a mismatched owner whatever ordering it observes, which is why
** the owner is set first on claim and cleared last on release. */
static volatile TaskHandle_t __dispatch_owner = NULL;
static volatile int __dispatch_connection = -1;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
ezlopi_error_t EZPI_core_offline_login_perform(cJSON *cj_params)
{
    ezlopi_error_t error = EZPI_SUCCESS;

    const int connection = __current_connection();

    if (EZPI_util_wssession_is_authorised(connection))
    {
        /* This connection has already logged in. Refusing a second login on
        ** the SAME connection is the old behaviour preserved; what changed is
        ** that it no longer refuses a different client. The old check read the
        ** one global flag, so once anybody was logged in the next honest
        ** client was turned away -- while an unauthenticated one was admitted
        ** without asking. A local UI and the Ezlo app being open at the same
        ** time is ordinary use and now works. */
        error = EZPI_FAILED;
    }
    else
    {
        cJSON *cj_user = cJSON_GetObjectItem(__FUNCTION__, cj_params, "user");
        cJSON *cj_token = cJSON_GetObjectItem(__FUNCTION__, cj_params, "token");
        if (cj_user && cj_token && (cJSON_IsString(cj_user)) && (cJSON_IsString(cj_token)))
        {
            if (__user_id_accepted(cj_user->valuestring))
            {
                char *password_saved = EZPI_core_local_key_get(NULL);

                /* Note that the token is NOT logged, and neither is the stored
                ** key. The line removed from here was
                **
                **     TRACE_D("password: %s", password_saved);
                **
                ** which printed the device's only local credential on every
                ** login attempt. It is tempting to assume a TRACE_D is
                ** compiled out or filtered in a shipped image; it was not.
                ** These are EzloPi's own trace macros rather than ESP_LOG, so
                ** CONFIG_LOG_DEFAULT_LEVEL does not apply to them, and
                ** serial_log_severity defaults to
                ** ENUM_EZLOPI_TRACE_SEVERITY_MAX (ezlopi_core_log.c:86),
                ** which passes severity 4. The default build printed it. */
                if (EZPI_util_localkey_match(password_saved,
                                             cj_token->valuestring))
                {
                    /* Authorise THIS connection and nothing else. The call
                    ** fails if the transport never announced the connection --
                    ** which is the case for a login arriving over the cloud
                    ** path, where there is no local socket to authorise -- and
                    ** the error is reported rather than swallowed, because
                    ** silently answering "logged in" to a caller that holds no
                    ** session is how the original defect behaved. */
                    if (!EZPI_util_wssession_authorise(connection))
                    {
                        /* NOT_AVAILABLE rather than FAILED, because FAILED is
                        ** reported to the client as "Already logged in" and
                        ** that would be untrue. There is no local connection
                        ** to authorise: either this request came in over the
                        ** cloud, which has no socket on the local server, or
                        ** the session table was full. */
                        error = EZPI_NOT_AVAILABLE;
                    }
                }
                else
                {
                    /* One error for "no key on the device" and "wrong token"
                    ** alike. Distinguishing them would tell an unauthenticated
                    ** caller whether the device has been provisioned, and the
                    ** answer is of no use to a legitimate client: either way
                    ** it cannot log in. The device-side diagnosis is in
                    ** EZPI_core_local_key_init()'s startup output. */
                    error = EZPI_ERR_INVALID_CREDENTIALS;
                }

                EZPI_core_local_key_free(password_saved);
            }
            else
            {
                error = EZPI_ERR_INVALID_CREDENTIALS;
            }
        }
        else
        {
            error = EZPI_ERR_WRONG_PARAM;
        }
    }

    return error;
}

ezlopi_error_t EZPI_core_offline_logout_perform()
{
    ezlopi_error_t error = EZPI_FAILED;

    const int connection = __current_connection();

    if (EZPI_util_wssession_is_authorised(connection))
    {
        /* Ends the session of the connection the request came in on, and of no
        ** other. The old version cleared the single global flag, so this was
        ** reachable -- from an unauthenticated peer, via a CLOSE frame -- as a
        ** way of logging out somebody else.
        **
        ** The whole session is dropped rather than just its authorised flag.
        ** If the connection is in fact still open it must log in again, which
        ** fails closed. Connections going away are handled by the transport's
        ** close hook calling EZPI_util_wssession_close() directly, not by this
        ** function. */
        EZPI_util_wssession_close(connection);
        error = EZPI_SUCCESS;
    }

    return error;
}

bool EZPI_core_offline_is_user_logged_in()
{
    /* Answers for the connection currently being dispatched. A caller with no
    ** claimed connection -- any task other than the one that claimed it, or
    ** the cloud path, which has no local socket -- gets false. */
    return EZPI_util_wssession_is_authorised(__current_connection());
}

void EZPI_core_offline_login_claim_connection(int connection)
{
    if (0 > connection)
    {
        /* Release. Clear the value first and the owner last, so no ordering a
        ** reader on another task can observe leaves it matching the owner
        ** while a stale descriptor is still readable. */
        __dispatch_connection = -1;
        __dispatch_owner = NULL;
    }
    else
    {
        __dispatch_owner = xTaskGetCurrentTaskHandle();
        __dispatch_connection = connection;
    }
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

/* The connection being dispatched on this task, or -1 if there is none.
**
** The ownership test is the load-bearing line: a task that did not claim the
** connection is told there isn't one, so it can neither authorise nor consult
** somebody else's socket. See the note by __dispatch_owner for why two tasks
** reach this code. */
static int __current_connection(void)
{
    if (__dispatch_owner && (xTaskGetCurrentTaskHandle() == __dispatch_owner))
    {
        return __dispatch_connection;
    }

    return -1;
}

/**
 * @brief Check the "user" field, if this build is configured to.
 *
 * WHAT THIS REPLACES
 *
 * Two copies of
 *
 *     #warning "Lomas: user-id verification disabled!"
 *
 * and about fifteen lines of commented-out code that read the user id from
 * NVS, required it to be non-NULL, and compared it with
 * strncmp(stored, supplied, strlen(stored)).
 *
 * WHY IT IS NOT SIMPLY UNCOMMENTED, AND WHY IT DEFAULTS TO OFF
 *
 * Three separate reasons, and all three had to be checked rather than assumed:
 *
 *   1. The commented-out code refuses the login when no user id is stored,
 *      because of its `if (NULL != stored_user_id)` guard.
 *      EZPI_core_nvs_read_user_id_str() returns the id of the Ezlo account
 *      that claimed the device, written only by the BLE auth path
 *      (ezlopi_service_ble_ble_auth.c:109 and :172). A device somebody flashed
 *      themselves has no stored id, so uncommenting it would lock a
 *      self-flashed device out of its own local API even once it has a key --
 *      defeating the whole point of generating one.
 *
 *   2. This check has been disabled since the file was written, so no shipped
 *      device has ever had it enforced on this path. Turning it on by default
 *      changes the behaviour of existing claimed devices against a cloud and
 *      an app whose exact "user" field this repository cannot test.
 *
 *   3. The repository has already faced this question once and answered it the
 *      same way. ezlopi_service_ble_ble_auth.c:158 keeps a *prefix* comparison
 *      for the BLE user id, with a note saying so explicitly: it is the
 *      comparison the shipped Ezlo app and cloud already authenticate against,
 *      and tightening it would change the behaviour of claimed devices. It
 *      would be incoherent to enforce strict equality here while that path
 *      deliberately does not.
 *
 * So the default is off, which preserves today's behaviour exactly -- the
 * local key is the credential on this path, as it has always been -- and the
 * check is available to anyone who wants it, as working code rather than as a
 * comment. The #warning is gone because the question is now answered and
 * recorded, not because it was quietly dropped.
 *
 * When CONFIG_EZPI_OFFLINE_LOGIN_VERIFY_USER_ID is set, the check is applied
 * where there is something to apply it to, and skipped where there is not:
 *
 *   - Claimed device: the id is present and must match exactly.
 *   - Self-flashed device: no id is stored, nothing to compare against, so the
 *     user field is not treated as a credential. It never was. The local key
 *     is checked either way.
 *
 * The comparison is strcmp() and is not constant-time, deliberately. A user id
 * is an identifier, not a secret; making it look like a secret here would
 * imply it carries security weight that it does not.
 *
 * @param supplied_user the "user" field from the request, non-NULL
 * @return true if the login may proceed to the key check
 * @return false if verification is enabled, an id is stored, and it does not
 *         match
 */
static bool __user_id_accepted(const char *supplied_user)
{
#ifndef CONFIG_EZPI_OFFLINE_LOGIN_VERIFY_USER_ID

    /* Default. The user field is accepted without being checked, exactly as
    ** every shipped build has done. The local key is the credential. */
    (void)supplied_user;
    return true;

#else

    bool accepted = false;

    char *stored_user_id = EZPI_core_nvs_read_user_id_str();

    if (NULL == stored_user_id)
    {
        /* Nothing has claimed this device, so there is no account to belong
        ** to. Not an error and not a weakening -- the local key still has to
        ** match. */
        accepted = true;
    }
    else
    {
        if ((0 != stored_user_id[0]) &&
            (0 == strcmp(stored_user_id, supplied_user)))
        {
            accepted = true;
        }
        else
        {
            /* The stored id is not printed. It is not secret, but it is the
            ** answer to the question the caller just got wrong, and there is
            ** no reason to supply it. The token is never printed either. */
            TRACE_W("offline login: user id does not match the claimed one");
        }

        ezlopi_free(__FUNCTION__, stored_user_id);
    }

    return accepted;

#endif // CONFIG_EZPI_OFFLINE_LOGIN_VERIFY_USER_ID
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
