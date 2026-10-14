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
 * @file    ezlopi_util_wssession.c
 * @brief   Per-connection authorisation for the local websocket API
 * @author
 * @version 1.0
 * @date    13th OCT 2025
 *
 * See the header for why this replaces a single global bool.
 *
 * A fixed array rather than a list. There is no allocation to fail and nothing
 * to leak, the bound is the server's own socket limit, and a linear scan of
 * eight entries is not worth a hash table. It also means the whole thing
 * compiles on a host with no dependency beyond stdbool, which is what lets the
 * properties in tests/host/test_wssession.c be checked against this exact file
 * rather than against a copy of it that could drift.
 *
 * THREADING. The caller is ESP-IDF's single httpd task: the accept hook, the
 * close hook and every request handler for this server run on it, one at a
 * time. So these functions are not locked, and that is a statement about the
 * caller rather than about this file. If a second task is ever given a
 * reference -- a broadcast path that wants to know who is authorised, say --
 * it needs a lock added here, because an int write is atomic but the
 * find-then-modify pairs below are not.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
/* For NULL. Included explicitly rather than inherited: on target the ESP-IDF
** headers drag it in anyway, so a missing include here compiles on the board
** and fails only on a host -- which is the wrong way round for a file whose
** whole purpose is to be host-testable. */
#include <stddef.h>

#include "ezlopi_util_wssession.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/** A descriptor value that cannot name a real socket, so it marks a free slot.
 *  Negative descriptors are rejected on the way in, which is what makes -1
 *  usable as the sentinel. */
#define WSSESSION_FREE (-1)

typedef struct s_wssession
{
    int fd;          /**< The socket this session belongs to, or WSSESSION_FREE. */
    bool authorised; /**< Whether this connection presented the local key. */
} s_wssession_t;

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/* Zero-initialised at load, which would mean fd == 0 -- a descriptor that can
 * legitimately exist. The table is therefore initialised lazily on first use
 * rather than relying on static zeroing, because "every slot claims to own
 * socket 0" is the one starting state that would hand out a session nobody
 * opened. */
static s_wssession_t __sessions[EZPI_WSSESSION_MAX];
static bool __initialised;

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void __ensure_initialised(void);
static s_wssession_t *__find(int fd);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

bool EZPI_util_wssession_open(int fd)
{
    __ensure_initialised();

    if (WSSESSION_FREE >= fd)
    {
        /* Not a socket this component will track. The caller's connection then
         * answers false to every authorisation query, which is the safe
         * direction. */
        return false;
    }

    /* Reuse the existing entry if this descriptor is somehow still in the
     * table. That happens when a close was never delivered and the number has
     * been handed to a new connection, and the important part is the reset
     * below: the new connection must not inherit the old one's authority. */
    s_wssession_t *session = __find(fd);

    if (NULL == session)
    {
        for (int i = 0; i < EZPI_WSSESSION_MAX; i++)
        {
            if (WSSESSION_FREE == __sessions[i].fd)
            {
                session = &__sessions[i];
                break;
            }
        }
    }

    if (NULL == session)
    {
        /* Full. Nothing is granted; see the header for why failing closed is
         * the only acceptable direction. */
        return false;
    }

    session->fd = fd;
    session->authorised = false;

    return true;
}

void EZPI_util_wssession_close(int fd)
{
    __ensure_initialised();

    s_wssession_t *session = __find(fd);

    if (session)
    {
        session->authorised = false;
        session->fd = WSSESSION_FREE;
    }
}

bool EZPI_util_wssession_authorise(int fd)
{
    __ensure_initialised();

    s_wssession_t *session = __find(fd);

    if (NULL == session)
    {
        /* No session to grant anything to. Deliberately does not create one:
         * a login arriving for a descriptor the server never announced means
         * this component's view and the server's have diverged, and inventing
         * an authorised session would be the worst possible response to that. */
        return false;
    }

    session->authorised = true;

    return true;
}

bool EZPI_util_wssession_is_authorised(int fd)
{
    __ensure_initialised();

    const s_wssession_t *session = __find(fd);

    /* The whole point of the component is in this line: the answer depends on
     * this descriptor's own session and on nothing else. No other connection's
     * state is consulted, so no other connection's login can admit this one. */
    return (session && session->authorised);
}

void EZPI_util_wssession_reset_all(void)
{
    for (int i = 0; i < EZPI_WSSESSION_MAX; i++)
    {
        __sessions[i].fd = WSSESSION_FREE;
        __sessions[i].authorised = false;
    }

    __initialised = true;
}

int EZPI_util_wssession_count(void)
{
    __ensure_initialised();

    int count = 0;

    for (int i = 0; i < EZPI_WSSESSION_MAX; i++)
    {
        if (WSSESSION_FREE != __sessions[i].fd)
        {
            count++;
        }
    }

    return count;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

static void __ensure_initialised(void)
{
    if (!__initialised)
    {
        EZPI_util_wssession_reset_all();
    }
}

static s_wssession_t *__find(int fd)
{
    if (WSSESSION_FREE >= fd)
    {
        return NULL;
    }

    for (int i = 0; i < EZPI_WSSESSION_MAX; i++)
    {
        if (fd == __sessions[i].fd)
        {
            return &__sessions[i];
        }
    }

    return NULL;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
