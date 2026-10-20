/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    stub_issun.c
 * @brief   The symbols ezlopi_core_scenes_when_methods_helper_functions.c
 *          needs in order to link on a host, and nothing else.
 *
 * test_issun compiles that whole translation unit, not a copy of the functions
 * under test, so the object carries an undefined symbol for every function the
 * file calls -- including everything reached only from the exported functions
 * this test never touches. Measured: 52 undefined symbols, and most of them
 * are not stubs at all:
 *
 *   19  libc, including glibc's own rename of strtoul
 *   14  cJSON, supplied by compiling the real cjext.c
 *   11  `extern const char *` keywords, supplied by compiling the real
 *       ezlopi_cloud_keywords.c rather than redeclared here -- a stubbed
 *       string constant is the copy that goes wrong quietly, and a test
 *       asserting on the spelling of "before" or "at" would then be asserting
 *       on whatever this directory invented
 *    7  EZPI_core_suntime_*, supplied by compiling the real
 *       ezlopi_core_scenes_suntime.c, which test_suntime already hosts
 *
 * which leaves the 11 functions below.
 *
 * WHY EVERY ONE OF THEM abort()s
 *
 * All 11 are unreachable from the functions this test drives. They abort()
 * rather than returning NULL or 0, following stub_httpurl.c and for the same
 * reason: a stub that quietly returns something plausible lets a future test
 * wander into the device registry, the NVS layer or the expression engine and
 * then pass on the strength of a stub's invention. If a test reaches one, the
 * suite stops and names it.
 *
 * That the suite passes with all 11 aborting is itself a result, not just
 * scaffolding: it says the isSunState flag-evaluation path touches no device,
 * no item group, no NVS, no HTTP and no expression. The one that would be
 * reached is EZPI_core_nvs_read_latidtude_longitude, from
 * __issunsate_update_sunstate_tm, and this test does not go there -- that is
 * the HTTPS half of the path and it needs a server, which is exactly the half
 * still recorded under Known gaps.
 *
 * The real headers are included rather than the prototypes being retyped,
 * because four of these signatures are not what they look like from the call
 * site: EZPI_core_http_body_in_place takes three arguments, two of them out
 * parameters; EZPI_core_nvs_read_latidtude_longitude is unprototyped and
 * returns char *; and EZPI_core_scenes_get_method_v2 returns a function
 * pointer. Retyping them by hand produced four mismatches on the first
 * attempt, which is undefined behaviour the linker would not have mentioned.
 *
 * xTaskGetTickCount is here for the same unreachability reason but is not an
 * EZPI symbol: see stubs/freertos/task.h for why that stub declares it without
 * defining it.
 */

#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>

#include "cjext.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_core_http.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_device_group.h"
#include "ezlopi_core_nvs.h"
#include "ezlopi_core_scenes_v2.h"
#include "ezlopi_core_scenes_expressions.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void unreachable(const char *name)
{
    fprintf(stderr,
            "FAIL stub_issun: %s was called, but test_issun is built on the "
            "premise that nothing it exercises reaches it. Either the test "
            "grew a new path or the subject did.\n",
            name);
    abort();
}

/* --- the device registry, and device/item groups ------------------------ */

l_ezlopi_device_t *EZPI_core_device_get_by_id(uint32_t device_id)
{
    (void)device_id;
    unreachable("EZPI_core_device_get_by_id");
    return NULL;
}

l_ezlopi_item_t *EZPI_core_device_get_item_by_id(uint32_t item_id)
{
    (void)item_id;
    unreachable("EZPI_core_device_get_item_by_id");
    return NULL;
}

l_ezlopi_device_grp_t *EZPI_core_device_group_get_by_id(uint32_t _id)
{
    (void)_id;
    unreachable("EZPI_core_device_group_get_by_id");
    return NULL;
}

l_ezlopi_item_grp_t *EZPI_core_item_group_get_by_id(uint32_t _id)
{
    (void)_id;
    unreachable("EZPI_core_item_group_get_by_id");
    return NULL;
}

/* --- the HTTPS client the suntime lookup goes through -------------------- */

void EZPI_core_http_mbedtls_req(s_ezlopi_core_http_mbedtls_t *tmp_http_data)
{
    (void)tmp_http_data;
    unreachable("EZPI_core_http_mbedtls_req");
}

ezlopi_error_t EZPI_core_http_body_in_place(char *resp, char **body, uint32_t *body_len)
{
    (void)resp;
    (void)body;
    (void)body_len;
    unreachable("EZPI_core_http_body_in_place");
    return EZPI_FAILED;
}

/* --- the stored coordinates --------------------------------------------- */

char *EZPI_core_nvs_read_latidtude_longitude(void)
{
    unreachable("EZPI_core_nvs_read_latidtude_longitude");
    return NULL;
}

/* --- the scene registry and the expression engine ----------------------- */

ezlopi_error_t EZPI_core_scenes_enable_disable_scene_by_id_v2(uint32_t _id, bool enabled_flag)
{
    (void)_id;
    (void)enabled_flag;
    unreachable("EZPI_core_scenes_enable_disable_scene_by_id_v2");
    return EZPI_FAILED;
}

f_scene_method_v2_t EZPI_core_scenes_get_method_v2(e_scene_method_type_t scene_method_type)
{
    (void)scene_method_type;
    unreachable("EZPI_core_scenes_get_method_v2");
    return NULL;
}

s_ezlopi_expressions_t *EZPI_scenes_expressions_get_node_by_name(char *expression_name)
{
    (void)expression_name;
    unreachable("EZPI_scenes_expressions_get_node_by_name");
    return NULL;
}

/* --- the scheduler clock ------------------------------------------------ */

TickType_t xTaskGetTickCount(void)
{
    unreachable("xTaskGetTickCount");
    return 0;
}
