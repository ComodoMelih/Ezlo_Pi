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
 * @file    ezlopi_service_ws_server_clients.c
 * @brief   perform some function on data
 * @authors Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    February 25th, 2024 12:37 AM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdio.h>
#include <string.h>

#include "ezlopi_util_trace.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_service_ws_server.h"
#include "ezlopi_service_ws_server_clients.h"
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
/**
 * @brief Function to create new clinet
 *
 * @param[in] http_handle Pointer to the client http handle
 * @param[in] http_descriptor HTTP descriptro for the clinent
 * @return l_ws_server_client_conn_t*
 * @retval Pointer to the newly created client
 */
static l_ws_server_client_conn_t *ezpi_create_new_client(void *http_handle, int http_descriptor);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static l_ws_server_client_conn_t *l_client_conn_head = NULL;
static uint32_t __number_of_clients = 0;
/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

/**
 * @brief Global/extern function template example
 * Convention : Use capital letter for initial word on extern function
 * @param arg
 */
l_ws_server_client_conn_t *EZPI_service_ws_server_clients_get_head(void)
{
    return l_client_conn_head;
}

/* WHY EVERY LOOKUP BELOW CHANGED FROM http_handle TO http_descriptor
**
** These functions used to match on http_handle, and the caller passed
** req->handle for it. ESP-IDF documents that field as "Handle to server
** instance": it is one value for the whole server, identical for every
** connection. The struct's own comment called it an "HTTP connection handle",
** which is how the mistake survived.
**
** Matching on a constant had three consequences:
**
**   - add() found the "duplicate" immediately for every client after the
**     first, so only ONE client was ever registered. A second local client --
**     a second phone, or a local UI alongside the app -- received no
**     broadcasts at all, while appearing to connect fine.
**   - remove_by_handle() removed whichever entry it found first, so a close
**     from any connection deregistered the one client that was registered.
**   - the fail-count eviction in ezpi_ws_server_send() could evict a healthy
**     client on behalf of a failing one.
**
** The descriptor identifies the connection, so that is what these key on now.
** The server handle is still stored, because httpd_ws_send_data() needs it
** alongside the descriptor, but it is no longer used to tell clients apart. */
l_ws_server_client_conn_t *EZPI_service_ws_server_clients_get_by_fd(int http_descriptor)
{
    l_ws_server_client_conn_t *curr_client = l_client_conn_head;
    while (curr_client)
    {
        if (curr_client->http_descriptor == http_descriptor)
        {
            break;
        }
        curr_client = curr_client->next;
    }

    return curr_client;
}

l_ws_server_client_conn_t *EZPI_service_ws_server_clients_add(void *http_handle, int http_descriptor)
{
    /* A descriptor already registered is a genuine duplicate -- the same
    ** connection announced twice -- and is refreshed rather than added again,
    ** so the list cannot grow an entry per request. Descriptors are reused by
    ** the operating system, so refreshing also clears a stale failure count
    ** that belonged to the previous owner of the number. */
    l_ws_server_client_conn_t *existing = EZPI_service_ws_server_clients_get_by_fd(http_descriptor);

    if (existing)
    {
        existing->http_handle = http_handle;
        existing->fail_count = 0;

        TRACE_I("numbers of clients: %u", __number_of_clients);

        return existing;
    }

    l_ws_server_client_conn_t *ws_client_conn = ezpi_create_new_client(http_handle, http_descriptor);

    if (ws_client_conn)
    {
        /* Prepended rather than appended: it costs no traversal and the order
        ** broadcasts go out in is not meaningful. */
        ws_client_conn->next = l_client_conn_head;
        l_client_conn_head = ws_client_conn;
        __number_of_clients++;
    }
    else
    {
        TRACE_E("WSL: could not allocate a client record for fd %d", http_descriptor);
    }

    TRACE_I("numbers of clients: %u", __number_of_clients);

    return ws_client_conn;
}

int EZPI_service_ws_server_clients_remove_by_fd(int http_descriptor)
{
    int ret = 0;

    l_ws_server_client_conn_t *ws_popped_con = EZPI_service_ws_server_clients_pop_by_fd(http_descriptor);

    if (ws_popped_con)
    {
        __number_of_clients--;
        ezlopi_free(__FUNCTION__, ws_popped_con);
        ret = 1;
    }

    TRACE_I("numbers of clients: %u", __number_of_clients);

    return ret;
}

l_ws_server_client_conn_t *EZPI_service_ws_server_clients_pop_by_fd(int http_descriptor)
{
    l_ws_server_client_conn_t *pop_con = NULL;

    if (l_client_conn_head)
    {
        if (http_descriptor == l_client_conn_head->http_descriptor)
        {
            pop_con = l_client_conn_head;
            l_client_conn_head = l_client_conn_head->next;
            pop_con->next = NULL;
        }
        else
        {
            l_ws_server_client_conn_t *curr_conn = l_client_conn_head;
            while (curr_conn->next)
            {
                if (curr_conn->next->http_descriptor == http_descriptor)
                {
                    pop_con = curr_conn->next;
                    curr_conn->next = curr_conn->next->next;
                    pop_con->next = NULL;
                    break;
                }

                curr_conn = curr_conn->next;
            }
        }
    }

    return pop_con;
}
/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
static l_ws_server_client_conn_t *ezpi_create_new_client(void *http_handle, int http_descriptor)
{
    l_ws_server_client_conn_t *ws_client_conn = ezlopi_malloc(__FUNCTION__, sizeof(l_ws_server_client_conn_t));
    if (ws_client_conn)
    {
        memset(ws_client_conn, 0, sizeof(l_ws_server_client_conn_t));

        ws_client_conn->http_handle = http_handle;
        ws_client_conn->http_descriptor = http_descriptor;
    }

    return ws_client_conn;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
