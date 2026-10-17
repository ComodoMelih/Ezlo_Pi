/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    stub_httpurl.c
 * @brief   The symbols ezlopi_core_scenes_then_methods_helper_func.c needs in
 *          order to link on a host, and nothing else.
 *
 * test_httpurl compiles that whole translation unit, not a copy of the function
 * under test, so the object carries an undefined symbol for every function the
 * file calls -- including the ones reached only from the other eight exported
 * functions the test never touches. Measured: 22 undefined symbols beyond the
 * sanitizer runtime, of which three are glibc's own renames of sscanf, strtoul
 * and memset, six are string constants that the real
 * ezlopi-cloud-constants sources supply, and thirteen are functions. Those
 * thirteen are here.
 *
 * THE TWO KINDS OF STUB HERE, AND WHY THE DIFFERENCE MATTERS
 *
 * Ten of the thirteen are unreachable from EZPI_parse_http_url. They abort()
 * rather than return a plausible value. That is deliberate: a stub that
 * returns NULL quietly would let a future test wander into the expression
 * engine or the NVS layer and pass on the strength of a stub's invention. If
 * the test ever reaches one, the suite stops and says which.
 *
 * The other three are on the path the test exercises, and are reimplementations
 * rather than stubs:
 *
 *   EZPI_core_http_mem_malloc          ezlopi_core_http.c:116-157
 *   EZPI_core_http_calc_empty_bufsize  ezlopi_core_http.c:105-114
 *   EZPI_core_http_dyna_relloc         ezlopi_core_http.c:159-176
 *
 * EZPI_parse_http_url reaches all three: mem_malloc fills web_server,
 * target_page, username and password, while calc_empty_bufsize and dyna_relloc
 * between them decide how the `Host:` header is grown. So the header contents
 * the test asserts on are produced jointly by the subject and these three
 * copies, which is the one shape of copy this suite's Makefile argues against
 * at length.
 *
 * Linking the real ezlopi_core_http.c instead was measured before settling for
 * copies, and it is not available: that file needs ten absent headers, nine of
 * them mbedtls and esp_tls, and 44 further diagnostics -- most of them
 * designated initializers against esp_http_client_config_t, whose full struct
 * layout a stub would then have to reproduce field for field. A stub carrying
 * the IDF's HTTP client config layout is a far worse copy than three leaf
 * functions, so three leaf functions it is. (The number 10 is also a
 * correction: an earlier pass of this audit recorded 12.)
 *
 * What makes the copies tolerable is that they are leaves -- no branching on
 * anything but allocation success -- and that each carries the line range it
 * came from, so a reader can diff them by eye. What makes them safe enough is
 * on the test's side rather than here: test_httpurl asserts on the strings the
 * subject passes *in*, recorded below, as well as on the struct the subject
 * leaves behind. The first of those does not depend on this file's behaviour
 * at all, so a copy that drifts cannot make a parsing assertion pass wrongly.
 */

/* sdkconfig.h first, before any scenes header. Without
 * CONFIG_EZPI_SERV_ENABLE_MESHBOTS the scenes headers preprocess away entirely
 * and every type below is undeclared -- which is how this file failed on its
 * first build, with eight "unknown type name" errors that looked like missing
 * includes and were a missing macro. */
#include "sdkconfig.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_core_http.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_nvs.h"
#include "ezlopi_core_scenes_v2.h"
#include "ezlopi_core_scenes_value.h"
#include "ezlopi_core_scenes_expressions.h"

#include "stub_httpurl.h"

/* -------------------------------------------------------------------------
 * The recorder. Every mem_malloc destination the subject writes to, with the
 * string it asked to store. This is the assertion surface that does not pass
 * through the copies below.
 * ------------------------------------------------------------------------- */
s_stub_malloc_log_t g_stub_malloc_log[STUB_MALLOC_LOG_MAX];
int g_stub_malloc_log_n;

void stub_httpurl_reset(void)
{
    memset(g_stub_malloc_log, 0, sizeof(g_stub_malloc_log));
    g_stub_malloc_log_n = 0;
}

const char *stub_malloc_arg(int nth)
{
    int seen = 0;
    for (int i = 0; i < g_stub_malloc_log_n; i++)
    {
        if (seen++ == nth)
        {
            return g_stub_malloc_log[i].src;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Reimplementations. Each mirrors the cited range of ezlopi_core_http.c.
 * ------------------------------------------------------------------------- */

/* ezlopi_core_http.c:105-114. The real one spells strlen through the
** GET_STRING_SIZE macro at :85, which is NULL-tolerant; written out here. */
int EZPI_core_http_calc_empty_bufsize(char *dest_buff, int dest_size, int reqd_size)
{
    int limit = dest_size - (int)((NULL != dest_buff) ? strlen(dest_buff) : 0);
    limit = (limit > 0) ? limit : 0;
    if (limit > reqd_size)
    {
        return limit;
    }
    return 0;
}

/* ezlopi_core_http.c:116-157, plus the recording. The `+ 5` and the
** free-the-old-first order are both load-bearing and both copied; the `ret = 0`
** on allocation failure is the contract this audit corrected upstream, and it
** is reproduced so that a test for the failure path would see the same answer
** here as on target.
**
** The one difference from the original is the record kept of (dest, src). It
** is taken before the allocation so that a failure injected by the test is
** still visible as an attempt. */
int EZPI_core_http_mem_malloc(char **__dest_ptr, const char *src_ptr)
{
    int ret = (int)((NULL != src_ptr) ? strlen(src_ptr) : 0);
    if (NULL != src_ptr)
    {
        if (g_stub_malloc_log_n < STUB_MALLOC_LOG_MAX)
        {
            g_stub_malloc_log[g_stub_malloc_log_n].dest = __dest_ptr;
            snprintf(g_stub_malloc_log[g_stub_malloc_log_n].src,
                     sizeof(g_stub_malloc_log[0].src), "%s", src_ptr);
            g_stub_malloc_log_n++;
        }
        else
        {
            fprintf(stderr, "stub_httpurl: mem_malloc log overflowed at %d; "
                            "raise STUB_MALLOC_LOG_MAX\n",
                    STUB_MALLOC_LOG_MAX);
            abort();
        }

        if (NULL != *__dest_ptr)
        {
            free(*__dest_ptr);
            *__dest_ptr = NULL;
        }
        ret += 5;
        char *tmp_ptr = malloc(ret);
        if (tmp_ptr)
        {
            memset(tmp_ptr, 0, ret);
            snprintf(tmp_ptr, ret, "%s", src_ptr);
            *__dest_ptr = tmp_ptr;
        }
        else
        {
            ret = 0;
        }
    }
    return ret;
}

/* ezlopi_core_http.c:159-176. ezlopi_realloc is a macro for plain realloc in
** this suite -- see stubs/EZLOPI_USER_CONFIG.h -- so the body is the original
** with the macro expanded. */
ezlopi_error_t EZPI_core_http_dyna_relloc(char **Buf, int reqSize)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if ((NULL != *Buf) && (reqSize > 0))
    {
        void *NewBuf = realloc(*Buf, reqSize);
        if (NULL != NewBuf)
        {
            *Buf = NewBuf;
            ret = EZPI_SUCCESS;
        }
    }
    return ret;
}

/* -------------------------------------------------------------------------
 * Unreachable from the subject under test. Loud rather than plausible.
 * ------------------------------------------------------------------------- */
static void unreachable(const char *who)
{
    fprintf(stderr,
            "\nstub_httpurl: %s() was called.\n"
            "  Nothing EZPI_parse_http_url does should reach it, so either the\n"
            "  test has grown into territory this stub does not model, or the\n"
            "  subject has. Decide which before giving it a return value --\n"
            "  a stub that invents one here would make the suite agree with\n"
            "  itself instead of with the firmware.\n",
            who);
    abort();
}

l_ezlopi_item_t *EZPI_core_device_get_item_by_id(uint32_t item_id)
{
    (void)item_id;
    unreachable("EZPI_core_device_get_item_by_id");
    return NULL;
}

char *EZPI_core_nvs_read_scenes_expressions(void)
{
    unreachable("EZPI_core_nvs_read_scenes_expressions");
    return NULL;
}

e_scene_value_type_v2_t EZPI_core_scene_get_scene_value_type_enum(const char *value_type_str)
{
    (void)value_type_str;
    unreachable("EZPI_core_scene_get_scene_value_type_enum");
    return (e_scene_value_type_v2_t)0;
}

void EZPI_scenes_expressions_delete_devitem_names(s_exp_device_item_names_t *exp_device_item_names)
{
    (void)exp_device_item_names;
    unreachable("EZPI_scenes_expressions_delete_devitem_names");
}

void EZPI_scenes_expressions_delete_items(s_exp_items_t *exp_items)
{
    (void)exp_items;
    unreachable("EZPI_scenes_expressions_delete_items");
}

cJSON *EZPI_scenes_expressions_get_cjson(s_ezlopi_expressions_t *exp_node)
{
    (void)exp_node;
    unreachable("EZPI_scenes_expressions_get_cjson");
    return NULL;
}

s_ezlopi_expressions_t *EZPI_scenes_expressions_get_node_by_name(char *expression_name)
{
    (void)expression_name;
    unreachable("EZPI_scenes_expressions_get_node_by_name");
    return NULL;
}

void EZPI_scenes_expressions_populate_devitem_names(s_ezlopi_expressions_t *exp_node, cJSON *cj_device_item_names)
{
    (void)exp_node;
    (void)cj_device_item_names;
    unreachable("EZPI_scenes_expressions_populate_devitem_names");
}

void EZPI_scenes_expressions_populate_items(s_ezlopi_expressions_t *exp_node, cJSON *cj_items)
{
    (void)exp_node;
    (void)cj_items;
    unreachable("EZPI_scenes_expressions_populate_items");
}

ezlopi_error_t EZPI_scenes_expressions_update_nvs(char *nvs_exp_id_key, cJSON *cj_updated_exp)
{
    (void)nvs_exp_id_key;
    (void)cj_updated_exp;
    unreachable("EZPI_scenes_expressions_update_nvs");
    return EZPI_FAILED;
}
