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
 * @file    ezlopi_util_mqttcfg.c
 * @brief   Validation for MQTT settings that arrive at runtime
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <string.h>

#include "ezlopi_util_mqttcfg.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief The four URI schemes esp-mqtt implements a transport for.
 *
 * Kept as a table rather than a chain of strncmp() calls so that the test can
 * assert the accepted set is exactly this and no larger. "mqtt" has to be
 * tested after "mqtts" would match, which is why the comparison below includes
 * the "://" -- otherwise "mqtts://host" matches the "mqtt" entry on its first
 * four characters and the scheme check passes for the wrong reason.
 */
static const char *const mqttcfg_schemes[] = {
    "mqtt://",
    "mqtts://",
    "ws://",
    "wss://",
};

#define MQTTCFG_SCHEME_COUNT (sizeof(mqttcfg_schemes) / sizeof(mqttcfg_schemes[0]))

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static bool __is_printable_no_space(unsigned char ch);
static bool __all_printable_no_space(const char *s);
static bool __no_control_chars(const char *s);
static bool __scheme_matches(const char *uri, const char *scheme, size_t n);
static size_t __scheme_len(const char *uri);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

bool EZPI_util_mqttcfg_uri_is_valid(const char *uri)
{
    bool valid = false;

    if (NULL != uri)
    {
        const size_t len = strlen(uri);

        if ((len > 0u) && (len <= EZPI_MQTTCFG_URI_MAX))
        {
            const size_t scheme_len = __scheme_len(uri);

            /* scheme_len of 0 means no recognised scheme. A URI that is
            ** nothing but a scheme ("mqtt://") has no host and is refused by
            ** the same condition. */
            if ((scheme_len > 0u) && (len > scheme_len))
            {
                const char *host = uri + scheme_len;

                /* The host must not start with ':' (no host, just a port) or
                ** '/' (no host at all, as in "mqtt:///path"). */
                if ((':' != host[0]) && ('/' != host[0]) &&
                    __is_printable_no_space((unsigned char)host[0]))
                {
                    /* Walk to the end of the host: ':' starts a port, '/'
                    ** starts a path, either ends the host. Everything in the
                    ** host must be a printable non-space; a space in a
                    ** hostname is the single most common way a copied-and-
                    ** pasted URI goes wrong. */
                    const char *p = host;
                    bool chars_ok = true;

                    while (('\0' != *p) && (':' != *p) && ('/' != *p))
                    {
                        if (false == __is_printable_no_space((unsigned char)*p))
                        {
                            chars_ok = false;
                            break;
                        }
                        p++;
                    }

                    if (chars_ok)
                    {
                        valid = true;

                        if (':' == *p)
                        {
                            /* A port was given, so it has to be a number in
                            ** range rather than merely present. ":0" and
                            ** ":99999" both parse as integers and neither is
                            ** a port. */
                            unsigned long port = 0u;
                            size_t digits = 0u;

                            p++;

                            while ((*p >= '0') && (*p <= '9'))
                            {
                                port = (port * 10u) + (unsigned long)(*p - '0');
                                digits++;

                                /* Stop accumulating well before overflow;
                                ** five digits is already past the maximum
                                ** port, so anything longer is wrong whatever
                                ** it adds up to. */
                                if (digits > 5u)
                                {
                                    break;
                                }
                                p++;
                            }

                            if ((0u == digits) || (digits > 5u) ||
                                (0u == port) || (port > 65535u))
                            {
                                valid = false;
                            }
                            else if (('\0' != *p) && ('/' != *p))
                            {
                                /* Something that is not a digit and does not
                                ** start a path, as in "mqtt://h:1883x". */
                                valid = false;
                            }
                            else
                            {
                                /* Fall through: whatever follows is a path. */
                            }
                        }

                        /* Whatever remains is a path. It is handed to
                        ** esp-mqtt untouched, so it only has to be free of
                        ** spaces and control characters. */
                        if (valid && (false == __all_printable_no_space(p)))
                        {
                            valid = false;
                        }
                    }
                }
            }
        }
    }

    return valid;
}

bool EZPI_util_mqttcfg_prefix_is_valid(const char *prefix)
{
    bool valid = false;

    if (NULL != prefix)
    {
        const size_t len = strlen(prefix);

        if ((len > 0u) && (len <= EZPI_MQTTCFG_PREFIX_MAX) &&
            ('/' != prefix[0]) && ('/' != prefix[len - 1u]))
        {
            size_t i;

            valid = true;

            for (i = 0u; i < len; i++)
            {
                const unsigned char ch = (unsigned char)prefix[i];

                /* '+' and '#' are the whole point of this function. See the
                ** header: in a subscription they are wildcards, and this
                ** prefix is used to build both subscriptions and publishes. */
                if (('+' == ch) || ('#' == ch))
                {
                    valid = false;
                    break;
                }

                if (false == __is_printable_no_space(ch))
                {
                    valid = false;
                    break;
                }

                /* An empty topic level. Legal per the specification, never
                ** intended by anyone typing it. */
                if (('/' == ch) && (i > 0u) && ('/' == prefix[i - 1u]))
                {
                    valid = false;
                    break;
                }
            }
        }
    }

    return valid;
}

bool EZPI_util_mqttcfg_cred_is_valid(const char *cred)
{
    bool valid = false;

    if (NULL != cred)
    {
        /* Empty is valid and is how "no credentials" is expressed. */
        if (strlen(cred) <= EZPI_MQTTCFG_CRED_MAX)
        {
            /* Control characters only. Not the printable-no-space rule the
            ** URI and prefix use: see __no_control_chars() for why a space
            ** has to be allowed here and nowhere else. */
            valid = __no_control_chars(cred);
        }
    }

    return valid;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

/**
 * @brief Printable ASCII excluding space.
 *
 * Written out rather than calling isprint(), for the same two reasons as in
 * ezlopi_util_localkey.c: isprint() accepts space, and it is undefined
 * behaviour when handed a negative char, which is what every byte above 0x7F
 * becomes on a platform with a signed char. Taking an unsigned char and
 * comparing the bounds directly has neither problem.
 *
 * Bytes above 0x7E are rejected, which means a non-ASCII hostname or a
 * non-ASCII topic prefix is refused. MQTT topics are UTF-8 and so in principle
 * one could be valid; refusing them is a deliberate narrowing, because the
 * service's fixed-size topic buffers are counted in bytes while anyone
 * choosing such a prefix would be counting characters, and because Home
 * Assistant's own discovery documentation uses ASCII throughout.
 */
static bool __is_printable_no_space(unsigned char ch)
{
    return ((ch > 0x20u) && (ch < 0x7Fu));
}

/**
 * @brief Every byte is printable ASCII and none is a space.
 *
 * The rule for the URI and the topic prefixes, where a space really is
 * malformed input.
 *
 * This function used to be called __no_control_chars() and was used for
 * credentials as well, which is how a broker password containing a space came
 * to be refused -- a restriction nothing documented and nothing wanted. It is
 * now named after what it actually enforces, and the credential path has its
 * own check below.
 */
static bool __all_printable_no_space(const char *s)
{
    bool ok = true;
    size_t i;

    for (i = 0u; '\0' != s[i]; i++)
    {
        if (false == __is_printable_no_space((unsigned char)s[i]))
        {
            ok = false;
            break;
        }
    }

    return ok;
}

/**
 * @brief No control characters anywhere in the string. Space is allowed.
 *
 * The rule for usernames and passwords, and the only restriction the header
 * has ever claimed for them ("No alphabet restriction. A broker password is
 * whatever the broker accepts, and this is not the place to have an opinion
 * about it.").
 *
 * A space is allowed because a space in a password is ordinary -- a
 * passphrase of several words is a password manager's default suggestion in
 * some tools -- and because nothing downstream cares: the credential is handed
 * to esp-mqtt as a C string in esp_mqtt_client_config_t and never appears in a
 * topic, a filename or a command line. The thing that would break on a space
 * is the topic prefix, and that still refuses one.
 *
 * Bytes above 0x7E are allowed, unlike everywhere else in this file. MQTT
 * usernames are UTF-8 by specification and passwords are binary, so a
 * non-ASCII password is legitimate; the reason the prefixes refuse one (the
 * service's byte-counted topic buffers) does not apply to a credential, which
 * is only ever bounded against EZPI_MQTTCFG_CRED_MAX as bytes.
 *
 * 0x7F (DEL) is refused along with 0x00-0x1F. It is a control character in
 * every sense but the one C's isprint() uses.
 */
static bool __no_control_chars(const char *s)
{
    bool ok = true;
    size_t i;

    for (i = 0u; '\0' != s[i]; i++)
    {
        const unsigned char ch = (unsigned char)s[i];

        if ((ch < 0x20u) || (0x7Fu == ch))
        {
            ok = false;
            break;
        }
    }

    return ok;
}

/**
 * @brief Case-insensitive comparison of the first n bytes, ASCII only.
 *
 * Hand-rolled rather than strncasecmp() because that is POSIX
 * (<strings.h>), not C99, and this component is deliberately free of
 * everything but <string.h> so it compiles unchanged on a desktop. Only the
 * A-Z range is folded, which is all a URI scheme can contain.
 */
static bool __scheme_matches(const char *uri, const char *scheme, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++)
    {
        char a = uri[i];
        const char b = scheme[i];

        if ((a >= 'A') && (a <= 'Z'))
        {
            a = (char)(a + ('a' - 'A'));
        }

        /* The table is lowercase, so only the input needs folding. A NUL in
        ** the input fails this comparison rather than reading past it,
        ** because the table entry at that position is never NUL for i < n. */
        if (a != b)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Length of the recognised scheme at the start of uri, or 0.
 *
 * Matches against the full "scheme://" rather than the bare scheme, so that
 * "mqtts://" cannot be accepted on the strength of the "mqtt" entry and then
 * have its host read starting from the wrong offset -- which would leave the
 * host as "s://host" and still pass every subsequent check.
 *
 * The comparison is case-insensitive, and that is not a guess. esp-mqtt parses
 * the scheme out of the URI and then selects a transport with
 * esp_transport_list_get_transport(), which compares with strcasecmp()
 * (components/tcp_transport/transport.c:65). So "MQTT://192.168.1.10:1883"
 * genuinely works on the device, as RFC 3986 says it should -- schemes are
 * case-insensitive. Rejecting it here would have refused a URI the stack
 * handles perfectly well, which is a worse failure than accepting a strange
 * one: the user would have no way to tell that the only thing wrong was the
 * capitalisation.
 */
static size_t __scheme_len(const char *uri)
{
    size_t found = 0u;
    size_t i;

    for (i = 0u; i < MQTTCFG_SCHEME_COUNT; i++)
    {
        const size_t n = strlen(mqttcfg_schemes[i]);

        if (__scheme_matches(uri, mqttcfg_schemes[i], n))
        {
            /* Longest match wins, so that "mqtts://" is reported as 8 and not
            ** as whatever a shorter prefix entry would have said. The table
            ** has no such overlap today, but it is the kind of table that
            ** gets a new row added to it. */
            if (n > found)
            {
                found = n;
            }
        }
    }

    return found;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
