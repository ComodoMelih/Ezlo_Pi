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
 * @file    ezlopi_util_mqttcfg.h
 * @brief   Validation for MQTT settings that arrive at runtime.
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 *
 * WHY THIS EXISTS
 *
 * The MQTT service used to take its broker URI, credentials and topic
 * prefixes from CONFIG_EZPI_MQTT_* -- Kconfig macros, fixed when the image was
 * built. To point an EzloPi at a broker you had to install ESP-IDF, run
 * menuconfig and reflash. That is not a configuration step, it is a
 * development step, and it put Home Assistant out of reach of anyone who was
 * not already building the firmware.
 *
 * Those settings now come from NVS and can be written at runtime, which means
 * for the first time they are *untrusted input*. A Kconfig string was checked
 * by the person who typed it; a value arriving over the network is not. So
 * every field is validated before it is stored, and validated again when it is
 * read back, and the validation lives here -- with no ESP-IDF dependency --
 * so that the awkward cases can be swept on a desktop rather than guessed at.
 *
 * WHAT IS ACTUALLY DANGEROUS HERE
 *
 * Not the broker URI. A malformed URI makes esp-mqtt fail to connect, which is
 * noisy and obvious.
 *
 * The topic prefix is the dangerous one, for a reason that is not obvious. The
 * service subscribes to
 *
 *     <prefix>/<node>/+/set
 *
 * and treats anything arriving there as a command to set an item's value -- a
 * relay, a dimmer. `+` and `#` are MQTT wildcards, and they are only wildcards
 * in a *subscription*. A prefix of "#" turns that subscription into "#/<node>/+/set",
 * and a prefix containing "+" widens it likewise: the hub starts accepting
 * commands from topic trees belonging to other devices on the same broker.
 * Meanwhile the same prefix is used to *publish*, where the MQTT specification
 * forbids wildcards outright, so the publishes silently fail and the device
 * looks dead while still taking orders. One bad character produces both halves
 * of that. Hence EZPI_util_mqttcfg_prefix_is_valid() rejects wildcards rather
 * than escaping them -- there is no legitimate use for a wildcard in a prefix,
 * so there is nothing to preserve.
 *
 * THE LENGTH LIMITS ARE DERIVED, NOT PICKED
 *
 * EZPI_MQTTCFG_PREFIX_MAX exists because the service builds topics into fixed
 * 192-byte buffers and refuses to publish when one does not fit. A prefix that
 * is legal but too long would therefore be accepted here and then cause every
 * single publish to be dropped, with nothing wrong in the configuration to
 * look at. So the limit is chosen to make the worst case fit, and the service
 * carries a compile-time assertion that it really does; see the
 * EZPI_MQTTCFG_*_MAX comments below for the arithmetic.
 */

#ifndef _EZLOPI_UTIL_MQTTCFG_H_
#define _EZLOPI_UTIL_MQTTCFG_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stddef.h>

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Definitions
     *******************************************************************************/

    /**
     * @brief Longest accepted broker URI, excluding the terminator.
     *
     * Enough for "mqtts://" plus a 119-character hostname plus ":65535". Not
     * derived from a buffer in the service -- esp-mqtt copies the URI itself --
     * so this is only a sanity bound to keep an absurd value out of NVS.
     */
#define EZPI_MQTTCFG_URI_MAX 128u

    /**
     * @brief Longest accepted topic prefix and discovery prefix, excluding the
     *        terminator.
     *
     * Derived from the service's 192-byte topic buffer. The two longest topics
     * it ever builds are
     *
     *   discovery: <prefix>/<component>/<node>/<object>/config
     *              64 + 1 + 13 + 1 + 13 + 1 + 63 + 1 + 6 + 1(NUL) = 164
     *
     *   state:     <prefix>/<node>/<object>/status
     *              64 + 1 + 13 + 1 + 63 + 1 + 6 + 1(NUL)          = 150
     *
     * where 13 is the longest component name ("binary_sensor"), 13 is the
     * MAC-derived node id ("ezlopi_" + 6 hex), 63 is the longest object id
     * (EZPI_MQTT_OBJECT_ID_LEN - 1) and 6 is the longest leaf ("status" /
     * "config"). Both fit inside 192 with room to spare, and the service
     * asserts it at compile time so that raising this constant without raising
     * the buffer fails the build instead of the device.
     */
#define EZPI_MQTTCFG_PREFIX_MAX 64u

    /**
     * @brief Longest accepted username and password, excluding the terminator.
     *
     * MQTT itself allows up to 65535 bytes. 64 covers every broker account
     * anyone sets up by hand and keeps the NVS entries small.
     */
#define EZPI_MQTTCFG_CRED_MAX 64u

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Is this a broker URI the MQTT client can be handed?
     *
     * Requires a scheme of mqtt://, mqtts://, ws:// or wss:// -- the four
     * esp-mqtt understands -- followed by a non-empty host, optionally
     * followed by ":port" with port in 1..65535, optionally followed by a
     * path. Rejects anything containing a space or a control character, and
     * anything longer than EZPI_MQTTCFG_URI_MAX.
     *
     * The scheme check is deliberately a whitelist. An http:// URI typed by
     * someone who expected a web address is accepted by esp-mqtt's parser and
     * then fails to connect for reasons that look like a network fault, so it
     * is better refused at the point it is entered.
     *
     * The scheme is matched case-insensitively, because esp-mqtt selects its
     * transport with strcasecmp() -- see the note on __scheme_len() in the
     * implementation. "MQTT://host" works on the device, so refusing it here
     * would be this check inventing a failure of its own.
     *
     * @param uri NUL-terminated candidate, may be NULL
     * @return true if it can be stored and used
     * @return false if NULL, empty, too long, or malformed
     */
    bool EZPI_util_mqttcfg_uri_is_valid(const char *uri);

    /**
     * @brief Is this a usable topic prefix or Home Assistant discovery prefix?
     *
     * Requires: non-empty; no longer than EZPI_MQTTCFG_PREFIX_MAX; no '+' or
     * '#' anywhere (see the file comment -- this is the check that matters);
     * no control characters or 0x7F; no leading or trailing '/'; and no "//",
     * since an empty topic level is legal MQTT but is never what anyone meant
     * and makes the resulting topics hard to match by eye.
     *
     * @param prefix NUL-terminated candidate, may be NULL
     * @return true if it can be stored and used
     * @return false if NULL, empty, too long, or containing a forbidden
     *         character or separator arrangement
     */
    bool EZPI_util_mqttcfg_prefix_is_valid(const char *prefix);

    /**
     * @brief Is this a storable username or password?
     *
     * An empty string is valid and meaningful: it is how "no credentials"
     * is expressed, and the service sends no credentials at all rather than
     * empty ones, because some brokers treat those differently. So this
     * only bounds the length and refuses control characters.
     *
     * No alphabet restriction. A broker password is whatever the broker
     * accepts, and this is not the place to have an opinion about it. In
     * particular a space is accepted, and so is any byte above 0x7E: MQTT
     * usernames are UTF-8 and passwords are binary, and the reason the URI and
     * prefix checks narrow themselves to printable ASCII -- byte-counted topic
     * buffers -- has nothing to do with a credential.
     *
     * That paragraph was true as documentation and false as code until the
     * host test for the API handler used "p@ss w0rd" as a password and was
     * refused. The check was sharing a helper with the URI path, which
     * reasonably rejects spaces.
     *
     * Control characters are the exception, 0x00-0x1F and 0x7F. Nobody types
     * one into a configuration form on purpose, but a copy-paste from a
     * terminal carries a trailing newline, and a newline on the end of a
     * password produces an authentication failure indistinguishable from a
     * wrong password.
     *
     * @param cred NUL-terminated candidate, may be NULL
     * @return true if it can be stored, including when empty
     * @return false if NULL, too long, or containing a control character
     */
    bool EZPI_util_mqttcfg_cred_is_valid(const char *cred);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_MQTTCFG_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
