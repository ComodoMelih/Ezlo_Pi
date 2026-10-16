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
 * @file    ezlopi_core_local_key.c
 * @brief   Where the local API key comes from on a device nobody provisioned
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdio.h>
#include <string.h>

#include <esp_random.h>

#include "EZLOPI_USER_CONFIG.h"
#include "ezlopi_core_factory_info.h"
#include "ezlopi_core_local_key.h"
#include "ezlopi_core_nvs.h"
#include "ezlopi_util_localkey.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief NVS entry holding a locally generated key.
 *
 * Deliberately not the same storage as the cloud-provisioned key, which lives
 * in the factory-info partition. Keeping them apart is what makes the
 * precedence in __resolve() a read-only decision: nothing here can overwrite
 * or shadow what Ezlo provisioning wrote, so a device that is later
 * provisioned through the app goes straight back to the cloud key with no
 * migration step and nothing to clean up.
 *
 * NVS limits a key name to 15 characters; this is 9.
 */
static const char *local_key_nvs_name = "local_key";

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static char *__resolve(e_ezpi_local_key_source_t *source);
static char *__generate_and_store(void);
static void __announce(const char *key);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/** @brief So a repeated init() does not print the key again. */
static bool announced = false;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
ezlopi_error_t EZPI_core_local_key_init(void)
{
    ezlopi_error_t error = EZPI_FAILED;

    e_ezpi_local_key_source_t source = E_EZPI_LOCAL_KEY_SRC_NONE;
    char *key = __resolve(&source);

    if (NULL == key)
    {
        /* No factory key and nothing in NVS: this is a self-flashed device on
        ** its first boot, and the whole reason this component exists. */
        key = __generate_and_store();
        source = (NULL != key) ? E_EZPI_LOCAL_KEY_SRC_GENERATED
                               : E_EZPI_LOCAL_KEY_SRC_NONE;
    }

    if (NULL != key)
    {
        error = EZPI_SUCCESS;

        switch (source)
        {
        case E_EZPI_LOCAL_KEY_SRC_FACTORY:
            /* Never printed. The Ezlo app already holds this key, so there is
            ** nothing to be gained by putting it on the wire, and it is tied
            ** to a cloud account rather than to this device alone. */
            TRACE_I("local-api key: using the cloud-provisioned key");
            break;

        case E_EZPI_LOCAL_KEY_SRC_NVS:
        case E_EZPI_LOCAL_KEY_SRC_GENERATED:
            __announce(key);
            break;

        default:
            break;
        }
    }
    else
    {
        /* The local API is now unusable: every method except
        ** hub.offline.login.ui is gated, and login has no credential to check
        ** against. Worth saying loudly, because the symptom a user sees is a
        ** websocket that connects and then rejects everything. */
        TRACE_E("local-api key: NONE. The local API cannot be logged into.");
    }

    return error;
}

char *EZPI_core_local_key_get(e_ezpi_local_key_source_t *source)
{
    e_ezpi_local_key_source_t src = E_EZPI_LOCAL_KEY_SRC_NONE;
    char *key = __resolve(&src);

    if (NULL != source)
    {
        *source = src;
    }

    return key;
}

void EZPI_core_local_key_free(char *key)
{
    if (NULL != key)
    {
        /* Scrub before releasing. memset on a buffer about to be freed is the
        ** classic candidate for being optimised away; this one survives
        ** because the length is read back from the same buffer afterwards and
        ** the compiler cannot prove the store is unobservable. Not a security
        ** boundary -- the key is in NVS and in RAM elsewhere -- just cheap. */
        const size_t len = strlen(key);
        memset(key, 0, len);
        if ('\0' != key[0])
        {
            TRACE_E("local-api key: scrub failed");
        }
        ezlopi_free(__FUNCTION__, key);
    }
}

ezlopi_error_t EZPI_core_local_key_set(const char *key)
{
    ezlopi_error_t error = EZPI_ERR_WRONG_PARAM;

    if (EZPI_util_localkey_is_valid(key))
    {
        error = EZPI_core_nvs_write_str(key, (uint32_t)(strlen(key) + 1),
                                        local_key_nvs_name);
        if (EZPI_SUCCESS == error)
        {
            TRACE_I("local-api key: replaced with a user-supplied key (%d chars)",
                    (int)strlen(key));
        }
        else
        {
            TRACE_E("local-api key: could not store the user-supplied key");
        }
    }
    else
    {
        /* The length is reported and the value is not. A rejected key is still
        ** a credential somebody intended to use. */
        TRACE_E("local-api key: rejected, must be %d-%d printable characters "
                "with no spaces",
                EZPI_LOCALKEY_MIN_LEN, EZPI_LOCALKEY_MAX_LEN);
    }

    return error;
}

ezlopi_error_t EZPI_core_local_key_clear(void)
{
    EZPI_core_nvs_delete_stored_data_by_name((char *)local_key_nvs_name);
    TRACE_I("local-api key: NVS key cleared, a new one is generated next boot");
    return EZPI_SUCCESS;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief The precedence: factory partition, then NVS, then nothing.
 *
 * Factory first is the compatibility guarantee. A device provisioned through
 * the Ezlo app keeps using exactly the key it used before this component
 * existed, whatever else is in NVS, so nothing about the cloud path changes.
 *
 * @param source set to where the returned key came from
 * @return a key the caller must release, or NULL
 */
static char *__resolve(e_ezpi_local_key_source_t *source)
{
    char *key = NULL;

    *source = E_EZPI_LOCAL_KEY_SRC_NONE;

    key = EZPI_core_factory_info_v3_get_local_key();
    if (NULL != key)
    {
        /* Taken as-is, deliberately not put through
        ** EZPI_util_localkey_is_valid() the way the NVS key below is. A
        ** validation rule applied here could only ever reject a key that a
        ** provisioned device is currently using, and there is no upside: the
        ** gate that matters is EZPI_util_localkey_match(), which already
        ** refuses anything shorter than 8 characters and requires an exact
        ** match.
        **
        ** It is also not needed. Both writers of this region --
        ** ezlopi_service_ble_provisioning.c:386 and
        ** ezlopi_service_uart.c:598 -- declare a 256-byte buffer, memset it to
        ** zero and copy the JSON string into it, so the region is always the
        ** key followed by zeros and __EZPI_core_factory_info_v3_read_string()
        ** returns exactly the key. An unprovisioned region is 0xFF throughout,
        ** which get_local_key() already turns into NULL. */
        *source = E_EZPI_LOCAL_KEY_SRC_FACTORY;
    }
    else
    {
        key = EZPI_core_nvs_read_str(local_key_nvs_name);
        if (NULL != key)
        {
            if (EZPI_util_localkey_is_valid(key))
            {
                *source = E_EZPI_LOCAL_KEY_SRC_NVS;
            }
            else
            {
                /* A stored key that no longer validates is discarded rather
                ** than used. This is the case that used to be dangerous: a
                ** zero-length or truncated key reaching the old
                ** strncmp(stored, supplied, strlen(stored)) authenticated
                ** every token presented to it. */
                TRACE_W("local-api key: stored key is unusable, discarding it");
                ezlopi_free(__FUNCTION__, key);
                key = NULL;
            }
        }
    }

    return key;
}

/**
 * @brief Make a key from hardware entropy and persist it.
 *
 * esp_fill_random() and not rand(), and that distinction is the entire
 * security of this function: a credential seeded from srand(time(NULL)) would
 * carry perhaps 20 bits of real entropy rather than 160, because the seed is
 * guessable to within the second the device first booted. The repository's
 * other random source, ezlopi_util_uuid_generate_random(), used to be exactly
 * that and now calls esp_fill_random() as well.
 *
 * @return the new key, which the caller must release, or NULL
 */
static char *__generate_and_store(void)
{
    char *stored = NULL;

    uint8_t entropy[EZPI_LOCALKEY_GEN_LEN];
    char key[EZPI_LOCALKEY_GEN_BUF_SIZE];

    esp_fill_random(entropy, sizeof(entropy));

    if (EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                    EZPI_LOCALKEY_GEN_LEN,
                                    key, sizeof(key)))
    {
        if (EZPI_SUCCESS == EZPI_core_nvs_write_str(key,
                                                    (uint32_t)(strlen(key) + 1),
                                                    local_key_nvs_name))
        {
            /* Read it back rather than returning the stack buffer. If the
            ** write silently did not take, the next boot would present a
            ** different key from the one just announced, and the user would be
            ** holding a credential that does not work -- a failure that looks
            ** like "the login is broken" rather than like "the write failed".
            ** Reading back makes that show up here instead. */
            stored = EZPI_core_nvs_read_str(local_key_nvs_name);

            if ((NULL == stored) || (0 != strcmp(stored, key)))
            {
                TRACE_E("local-api key: stored key did not read back, "
                        "not using it");
                if (NULL != stored)
                {
                    ezlopi_free(__FUNCTION__, stored);
                    stored = NULL;
                }
            }
        }
        else
        {
            TRACE_E("local-api key: could not be stored");
        }
    }
    else
    {
        TRACE_E("local-api key: could not be generated");
    }

    memset(entropy, 0, sizeof(entropy));
    memset(key, 0, sizeof(key));

    return stored;
}

/**
 * @brief Print a locally generated key, once.
 *
 * printf() rather than TRACE_*, for two reasons that are both about this being
 * a value the user needs rather than a log line:
 *
 *   - Every TRACE_* macro expands to nothing when CONFIG_EZPI_UTIL_TRACE_EN is
 *     off (ezlopi_util_trace.h:203-207). A build with tracing disabled would
 *     then have a generated credential and no way at all to learn it.
 *   - TRACE_* output is subject to serial_log_severity, which is runtime
 *     settable and persisted in NVS. A user who has turned the log level down
 *     would get a device with an unknowable key.
 *
 * Gated at compile time by CONFIG_EZPI_LOCAL_KEY_ANNOUNCE so that anyone
 * shipping a product on this firmware can switch it off and use
 * EZPI_core_local_key_set() to install a key of their own choosing.
 *
 * @param key the key to show
 */
static void __announce(const char *key)
{
    if (announced)
    {
        return;
    }
    announced = true;

#ifdef CONFIG_EZPI_LOCAL_KEY_ANNOUNCE
    printf("\n"
           "+--------------------------------------------------------------+\n"
           "|  EzloPi local API key                                        |\n"
           "|                                                              |\n"
           "|  %-60s|\n"
           "|                                                              |\n"
           "|  Use it as the \"token\" of hub.offline.login.ui on the local   |\n"
           "|  websocket API. It was generated on this device and is not    |\n"
           "|  known to anyone else.                                        |\n"
           "|                                                              |\n"
           "|  Printed because CONFIG_EZPI_LOCAL_KEY_ANNOUNCE is set.       |\n"
           "+--------------------------------------------------------------+\n"
           "\n",
           key);
#else
    /* Still say that a local key exists, so the absence of the banner is not
    ** mistaken for the absence of a key. The value is withheld, not the fact. */
    TRACE_I("local-api key: present (%d chars), not printed "
            "(CONFIG_EZPI_LOCAL_KEY_ANNOUNCE is off)",
            (int)strlen(key));
#endif
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
