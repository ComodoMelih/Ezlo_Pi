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
 * @file    ezlopi_util_localkey.h
 * @brief   Validation, comparison and generation of the local API key
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 *
 * The local key is the only credential the offline websocket API accepts: an
 * unauthenticated client may call `hub.offline.login.ui` and nothing else, and
 * that method compares the supplied token against this key. Everything about
 * handling it therefore belongs somewhere it can be tested exhaustively on a
 * host, which is why this component has no REQUIRES and includes nothing but
 * the C library.
 *
 * The comparison in particular is worth isolating. The version this replaces
 * was `strncmp(stored, supplied, strlen(stored))`, which is a prefix match:
 * any token beginning with the key authenticates, and a zero-length stored key
 * makes strlen() zero so strncmp() returns 0 for every input. Both cases are
 * regression-tested in tests/host/test_localkey.c against a copy of that exact
 * expression, so the test demonstrates the defect rather than asserting it.
 */

#ifndef _EZLOPI_UTIL_LOCALKEY_H_
#define _EZLOPI_UTIL_LOCALKEY_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * @brief Length, in characters, of a key this component generates.
 *
 * 32 characters drawn from a 32-symbol alphabet is 160 bits. The flash field
 * that holds it is EZLOPI_FINFO_LEN_LOCAL_KEY (0x100) bytes, and a
 * cloud-provisioned key is 64 characters, so this is comfortably inside both.
 */
#define EZPI_LOCALKEY_GEN_LEN 32

/**
 * @brief Shortest key that is allowed to authenticate anything.
 *
 * This is a floor on the stored side, not a format rule: it exists so that a
 * truncated or half-written key cannot become a weak-but-accepted credential,
 * and so that the empty-string case can never match. It is deliberately well
 * below both the 32 characters generated here and the 64 a cloud provisioning
 * run writes.
 */
#define EZPI_LOCALKEY_MIN_LEN 8

/** @brief Longest key accepted, excluding the terminator. */
#define EZPI_LOCALKEY_MAX_LEN 192

/**
 * @brief Buffer size that always holds a generated key and its terminator.
 */
#define EZPI_LOCALKEY_GEN_BUF_SIZE (EZPI_LOCALKEY_GEN_LEN + 1)

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Is this a usable local key?
     *
     * Checks length and that every character is printable and not a space.
     * It deliberately does *not* require the alphabet used by
     * EZPI_util_localkey_generate(): a key written by Ezlo cloud provisioning
     * is 64 mixed-case alphanumerics including the characters this component
     * omits when generating, and such a key must keep working unchanged.
     *
     * @param key NUL-terminated candidate, may be NULL
     * @return true if the key may be stored and used
     * @return false for NULL, too short, too long, or any non-printable byte
     */
    bool EZPI_util_localkey_is_valid(const char *key);

    /**
     * @brief Compare a supplied token against the stored key.
     *
     * Requires the two to be equal in length and equal in every byte. The
     * byte loop runs over the whole key regardless of where the first
     * difference is, so the time taken does not reveal how much of a guess was
     * correct. The length check happens first and so does leak the key length,
     * which is not secret -- it is fixed by whoever provisioned the device.
     *
     * A stored key shorter than EZPI_LOCALKEY_MIN_LEN never matches, whatever
     * is supplied. That covers the blank-flash and partially-written cases.
     *
     * @param stored   the device's key, may be NULL
     * @param supplied the token from the client, may be NULL
     * @return true only on an exact match of a valid-length key
     */
    bool EZPI_util_localkey_match(const char *stored, const char *supplied);

    /**
     * @brief How many random bytes EZPI_util_localkey_generate() consumes.
     *
     * One byte per output character. Five of the eight bits are used and three
     * are discarded, which is wasteful of entropy and exactly unbiased: 32
     * divides 256, so every symbol is equally likely without any rejection
     * loop or modulo correction.
     *
     * @param key_len number of characters wanted
     * @return bytes of entropy required
     */
    size_t EZPI_util_localkey_entropy_needed(size_t key_len);

    /**
     * @brief Render random bytes as a key.
     *
     * This component does not source its own randomness. The caller passes in
     * bytes from esp_fill_random() on a device, or from a fixed vector in a
     * test, which is what makes generation reproducible and testable at all.
     * Note that rand()/srand(time(NULL)) is *not* an acceptable source here:
     * the seed is guessable to within a second, so the key would be too.
     *
     * The alphabet is 32 symbols: lower-case letters without `l` or `o`, and
     * digits `2`-`9`. The omissions are for reading a key off a serial log or
     * a label without confusing it for another.
     *
     * @param entropy     random bytes, at least EZPI_util_localkey_entropy_needed(key_len)
     * @param entropy_len how many bytes @p entropy holds
     * @param key_len     characters of key wanted
     * @param out         destination, needs key_len + 1 bytes
     * @param out_size    size of @p out in bytes
     * @return true on success, with @p out NUL-terminated
     * @return false if any argument is NULL, the buffer is too small, there is
     *         not enough entropy, or key_len is outside the accepted range;
     *         @p out is then left with an empty string where possible
     */
    bool EZPI_util_localkey_generate(const uint8_t *entropy, size_t entropy_len,
                                     size_t key_len, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_LOCALKEY_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
