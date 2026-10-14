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
 * @file    ezlopi_service_ws_server_clients.h
 * @brief   perform some function on data
 * @authors Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    February 25th, 2024 12:37 AM
 */

#ifndef __EZLOPI_SERVICE_WS_SERVER_CLIENTS_H__
#define __EZLOPI_SERVICE_WS_SERVER_CLIENTS_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

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

    /**
     * @brief Linked list for websoket clinet connection handler
     *
     */
    typedef struct l_ws_server_client_conn
    {
        /**
         * The httpd SERVER-instance handle, as ESP-IDF's httpd_req_t.handle
         * documents itself. NOT a per-connection value: it is the same for
         * every client of this server. It is kept only because
         * httpd_ws_send_data() wants it alongside the descriptor, and it must
         * never be used to tell one client from another -- the comment here
         * used to read "HTTP connection handle", and the lookups that believed
         * it meant only one client was ever registered.
         */
        void *http_handle;
        int http_descriptor; /**< Socket descriptor. This identifies the connection. */
        uint32_t fail_count; /**< Connection failed count */

        struct l_ws_server_client_conn *next; /**< Points to the next clinet handle */
    } l_ws_server_client_conn_t;

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/
    /**
     * @brief Remove the client registered for a socket descriptor.
     *
     * Keyed on the descriptor, which identifies one connection. The previous
     * version keyed on the server-instance handle and so removed an arbitrary
     * client whenever any connection closed.
     *
     * @param[in] http_descriptor Socket descriptor of the client to remove
     * @return int 1 if a client was removed, 0 if none was registered
     */
    int EZPI_service_ws_server_clients_remove_by_fd(int http_descriptor);
    /**
     * @brief Function to get clinet list head
     *
     * @return l_ws_server_client_conn_t*
     * @retval Head of the client handles list
     */
    l_ws_server_client_conn_t *EZPI_service_ws_server_clients_get_head(void);
    /**
     * @brief Unlink the client for a socket descriptor without freeing it.
     *
     * @param[in] http_descriptor Socket descriptor of the client to pop
     * @return l_ws_server_client_conn_t*
     * @retval Pointer to the client popped, or NULL if none matched
     */
    l_ws_server_client_conn_t *EZPI_service_ws_server_clients_pop_by_fd(int http_descriptor);
    /**
     * @brief Find the client registered for a socket descriptor.
     *
     * @param[in] http_descriptor Socket descriptor to look for
     * @return l_ws_server_client_conn_t*
     * @retval Pointer to the client, or NULL if none matched
     */
    l_ws_server_client_conn_t *EZPI_service_ws_server_clients_get_by_fd(int http_descriptor);
    /**
     * @brief Register a client, or refresh one already registered.
     *
     * More than one client can be registered at a time; that is the point of
     * the change. The old version compared the server-instance handle, found a
     * match for every client after the first, and so silently kept the list at
     * one entry -- a second local client got no broadcasts.
     *
     * @param[in] http_handle The httpd server-instance handle, stored for
     *                        httpd_ws_send_data() and not used as an identity
     * @param[in] http_desc   Socket descriptor, which identifies the client
     * @return l_ws_server_client_conn_t*
     * @retval Pointer to the client record, or NULL if allocation failed
     */
    l_ws_server_client_conn_t *EZPI_service_ws_server_clients_add(void *http_handle, int http_desc);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_SERVICE_WS_SERVER_CLIENTS_H__

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
