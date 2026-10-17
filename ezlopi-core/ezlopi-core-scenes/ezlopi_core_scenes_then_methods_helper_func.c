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
 * @file    ezlopi_core_scenes_then_methods_helper_func.c
 * @brief   These are sub-functions utilized in file 'ezlopi_core_scenes_then_methods.c'
 * @author
 * @version 1.0
 * @date    October 20th, 2023 7:50 PM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "sdkconfig.h"

#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS

#include <string.h>
#include "cjext.h"
#include "ezlopi_util_trace.h"
#include "ezlopi_core_nvs.h"
#include "ezlopi_core_http.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_device_group.h"
#include "ezlopi_core_cjson_macros.h"
#include "ezlopi_core_scenes_v2.h"
#include "ezlopi_core_scenes_value.h"
#include "ezlopi_core_scenes_expressions.h"
#include "ezlopi_core_scenes_then_methods_helper_func.h"
#include "ezlopi_core_errors.h"

#include "ezlopi_cloud_constants.h"
#include "EZLOPI_USER_CONFIG.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/
#define STR_SIZE(str) ((NULL != str) ? (strlen(str)) : 0)

/**
 * @brief Frees an address => '*ptr', pointing to an occupied address in heap.
 */
#define FREE_PTR_IF_NOT_NULL(ptr)           \
    {                                       \
        if (ptr)                            \
        {                                   \
            ezlopi_free(__FUNCTION__, ptr); \
            ptr = NULL;                     \
        }                                   \
    }

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static int __ezlopi_core_scenes_then_sendhttp_relloc_header(s_ezlopi_core_http_mbedtls_t *tmp_http_data, int append_size, const char *append_str);
static int __ezlopi_core_scenes_then_create_fresh_header(s_ezlopi_core_http_mbedtls_t *tmp_http_data);
static void __ezlopi_core_scenes_then_append_to_header(s_ezlopi_core_http_mbedtls_t *tmp_http_data, const char *str1, const char *str2);
static char __ezlopi_core_scenes_then_lower_ascii(char ch);
static bool __ezlopi_core_scenes_then_split_url(const char *url, char *authority, size_t authority_size, int *port_out, const char **page_out);
static char *__ezlopi_core_scenes_then_split_authority(char *authority, char **user_out, char **pass_out, int *port_io);
/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
//------------------------------- EZPI_core_scene_then_sendhttp_request -----------------------------------------------
void EZPI_parse_http_request_type(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    const char *field_value_string = curr_field->field_value.u_value.value_string;
    if ((EZLOPI_VALUE_TYPE_STRING == curr_field->value_type) && (NULL != field_value_string))
    {
        if (0 == strncmp(curr_field->field_value.u_value.value_string, "GET", 4))
        {
            tmp_http_data->method = HTTP_METHOD_GET;
        }
        else if (0 == strncmp(curr_field->field_value.u_value.value_string, "POST", 5))
        {
            tmp_http_data->method = HTTP_METHOD_POST;
        }
        else if (0 == strncmp(curr_field->field_value.u_value.value_string, "PUT", 4))
        {
            tmp_http_data->method = HTTP_METHOD_PUT;
        }
        else if (0 == strncmp(curr_field->field_value.u_value.value_string, "DELETE", 7))
        {
            tmp_http_data->method = HTTP_METHOD_DELETE;
        }
        else
        {
            TRACE_E("The given http_req method is not implemented yet");
        }
    }
}
void EZPI_parse_http_url(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    const char *field_value_string = curr_field->field_value.u_value.value_string;
    if ((EZLOPI_VALUE_TYPE_STRING == curr_field->value_type) && (NULL != field_value_string))
    {
        // TRACE_W("Here! fresh url");
        tmp_http_data->url_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->url), field_value_string);
        if (tmp_http_data->url_maxlen > 0)
        {
            //--------------------------------------------
            // tmp_http_data->web_port = ((NULL != strstr(field_value_string, "https")) ? 443 : 80);
            // __ezlopi_core_scenes_then_sendhttp_parse_host_name(tmp_http_data, field_value_string);
            //--------------------------------------------

            /* This used to be nine sscanf arms: eight spelling out the
            ** combinations of "http"/"https" against present/absent port and
            ** present/absent path, and a ninth fallback for any other scheme.
            ** What replaced them is one pass that finds the authority and the
            ** path by position, because the arms disagreed with each other in
            ** eight measured ways and every disagreement was silent. The
            ** replaced behaviour, read off the real translation unit:
            **
            **   - Arms one to four scanned the port with %i, which is not a
            **     decimal conversion. ":0x50" was port 80 and ":010" was port
            **     8 -- the author wrote base-10 digits and got hex and octal.
            **   - ":08080" was port 0. %i read the leading "0" as octal, stopped
            **     at the '8', failed the '/' that follows, and dropped through
            **     to arm three, which asks for no path and kept the 0.
            **   - ":8090junk" was port 8090 with the junk discarded. Arm three's
            **     format ends "%i[^\n]", and that trailing [^\n] is not a
            **     conversion there -- it is five literal characters that never
            **     have to match, so nothing checks what follows the digits.
            **   - ":99999", ":65536" and ":0" were all accepted as written.
            **     Nothing range-checked the result.
            **   - ":-1" was port -1, passed to mbedtls as the string "-1".
            **   - ":99999999999999" was port 276447231. An out-of-range %i is
            **     undefined behaviour, and it is inside libc, so the sanitizers
            **     the host suite runs under cannot see it.
            **   - A scheme is case-insensitive (RFC 3986 section 3.1), but the
            **     eight literals are not, so "HTTP://host:8090/x" missed all
            **     eight and landed on the ninth arm -- and "HTTP://host/x",
            **     with no explicit port, matched nothing at all and was
            **     reported unparseable.
            **   - Arm nine ended "%d%199s", so its page kept the '/' that the
            **     other arms consume ("GET //x"), stopped at the first space,
            **     and was cut to 199 characters in silence.
            **
            ** Every one of those is a wrong request rather than a refused one,
            ** which is the reason none of them was reported.
            **
            ** The port is not parsed here at all. __ezlopi_core_scenes_then_
            ** split_authority already does it for the arms that scanned the
            ** authority whole, and it does it properly: all digits, 1 to 65535,
            ** and bracket-aware so an IPv6 literal survives. Routing every URL
            ** through that one implementation is most of this fix -- the
            ** correct code was already in the file, and four arms were going
            ** around it. */
            int port = 80;
            char authority[256] = {0};
            const char *page = "";
            char *url_user = NULL;
            char *url_pass = NULL;
            char *bare_host = NULL;

            if (__ezlopi_core_scenes_then_split_url(field_value_string, authority, sizeof(authority), &port, &page))
            {
                bare_host = __ezlopi_core_scenes_then_split_authority(authority, &url_user, &url_pass, &port);

                /* A NULL here is a port the split refused, and it has already
                ** said so. The empty-string case is separate: "http://user@/x"
                ** parses as far as having a userinfo and nothing after it, and
                ** an empty host would reach the allocator and emit a literal
                ** "Host: \r\n" -- the same header the comment below describes
                ** as the cost of an unparseable URL, so it is refused in the
                ** same way rather than sent.
                **
                ** The trace names neither the authority nor the host: by this
                ** point the authority buffer has had its terminators written
                ** in, so printing it would print the userinfo, which is where
                ** a password lives. The NULL test has to come first for the
                ** same reason it usually does. */
                if ((NULL != bare_host) && ('\0' == *bare_host))
                {
                    TRACE_E("URL has a userinfo but no host");
                    bare_host = NULL;
                }
            }

            if (NULL != bare_host)
            {
                /* Only when the 'credential' field has not already supplied
                ** them. Field order is whatever the request listed, so this is
                ** the rule that makes the outcome independent of it: an
                ** explicit credential always wins over one embedded in the
                ** URL, in either order. */
                if ((NULL != url_user) && (NULL == tmp_http_data->username))
                {
                    tmp_http_data->username_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->username), url_user);
                }
                if ((NULL != url_pass) && (NULL == tmp_http_data->password))
                {
                    tmp_http_data->password_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->password), url_pass);
                }

                /* Two of these four print request-supplied text that can carry
                ** a secret, and they are gated the same way
                ** ezlopi_core_http.c:248 gates the assembled request and the
                ** server's reply -- this log is readable over the serial
                ** console and ends up in any captured boot log.
                **
                ** '*source' is the URL verbatim, userinfo included, so before
                ** the split above it was one of three traces here that printed
                ** a password and it is now the only one left. 'page' is the
                ** second: everything after the authority goes into it, query
                ** string and all, and a URL of the form
                ** ".../v1/thing?api_key=..." is an ordinary way to authenticate
                ** an HTTP request. The generated query is already withheld at
                ** ezlopi_core_http.c:249, so printing the action's own here
                ** would have left the back door open.
                **
                ** 'host' and 'port' stay. They are the two facts a reader needs
                ** to tell a parse that worked from one that aimed somewhere
                ** unexpected, and after the split neither can carry userinfo.
                ** The withheld page's length is still printed, for the same
                ** reason the request's is: that there was a path, and how long
                ** it was, is most of what the trace was being read for. */
#ifdef CONFIG_EZPI_HTTP_LOG_PAYLOADS
                TRACE_D("*source = '%s'", field_value_string);
                TRACE_S("page = '%s'", page);
#else
                TRACE_S("page = [len: %d] (withheld; CONFIG_EZPI_HTTP_LOG_PAYLOADS is off)", (int)strlen(page));
#endif
                TRACE_S("host = '%s'", bare_host);
                TRACE_S("port = '%d'", port);

                tmp_http_data->web_server_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->web_server), bare_host);
                tmp_http_data->targe_page_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->target_page), page);
                tmp_http_data->web_port = port;

                /* Appending the Host header belongs in here, not after the
                ** branch. The host buffer is `{0}` until a successful parse
                ** fills it, so an unparseable URL used to emit a literal
                ** "Host: \r\n" into the request -- a header no server will
                ** route and the one part of the request a reader cannot see
                ** is missing. */
                int content_size = __ezlopi_core_scenes_then_create_fresh_header(tmp_http_data);
                if (content_size > 0) // if this characters exsists in the 'tmp_http_data->header'
                {
                    // 1. adding 'host' to header-buffer
                    // TRACE_W("Appending!! webserver -> header");
                    __ezlopi_core_scenes_then_append_to_header(tmp_http_data, "Host", bare_host);
                }
                else
                {
                    TRACE_E("Failed Creating header for 'Host:'");
                }
            }
            else
            {
                TRACE_E("failed to parse URL");
            }
        }
    }
}
void EZPI_parse_http_content_type(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    const char *field_value_string = curr_field->field_value.u_value.value_string;
    if ((EZLOPI_VALUE_TYPE_STRING == curr_field->value_type) && (NULL != field_value_string))
    {
        // TRACE_W("Here! content-type");
        if (STR_SIZE(field_value_string) > 0)
        {
            int content_size = __ezlopi_core_scenes_then_create_fresh_header(tmp_http_data);
            if (content_size > 0) // if this characters exsists in the 'tmp_http_data->header'
            {
                // 2. adding 'Content-Type' to header-buffer
                // TRACE_W("Appending!! content-type -> header");
                __ezlopi_core_scenes_then_append_to_header(tmp_http_data, "Content-Type", field_value_string);
            }
            else
            {
                TRACE_E("Failed Creating header for 'Content-Type:'");
            }
        }
        // else
        // {
        //     TRACE_E("ERR : Content-Type ==> STRLEN[%d]", STR_SIZE(field_value_string));
        // }
    }
}
void EZPI_parse_http_content(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    const char *field_value_string = curr_field->field_value.u_value.value_string;
    if ((EZLOPI_VALUE_TYPE_STRING == curr_field->value_type) && (NULL != field_value_string))
    {
        // TRACE_W("Here! fresh content");
        tmp_http_data->content_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->content), field_value_string);
        if (tmp_http_data->content_maxlen > 0)
        {
            int content_size = __ezlopi_core_scenes_then_create_fresh_header(tmp_http_data);
            if (content_size > 0) // if this characters exsists in the 'tmp_http_data->header'
            {
                // TRACE_W("Appending!! content_length -> header");
                uint16_t i = (int)strlen(field_value_string);
                if (i > 0)
                {
                    char str[10 + 1];
                    snprintf(str, 10, "%d", (int)i); // write length value in string
                    str[10] = '\0';
                    // 3. adding 'Content-Length' to header-buffer
                    __ezlopi_core_scenes_then_append_to_header(tmp_http_data, "Content-Length", str);
                }
            }
            else
            {
                TRACE_E("Failed Creating header for 'Content-Length:'");
            }
        }
    }
}
void EZPI_parse_http_headers(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    const cJSON *cj_value = curr_field->field_value.u_value.cj_value;
    if ((EZLOPI_VALUE_TYPE_DICTIONARY == curr_field->value_type) && cJSON_IsObject(cj_value))
    {
        // TRACE_W("Here! headers");
        int content_size = __ezlopi_core_scenes_then_create_fresh_header(tmp_http_data);
        if (content_size > 0) // if this characters exsists in the 'tmp_http_data->header'
        {
            // TRACE_W("Appending!! headers -> header");
            cJSON *header = (cj_value->child);
            while (header)
            {
                if ((NULL != header->string) && (NULL != header->valuestring))
                {
                    // 4. adding 'remaining' to header-buffer
                    __ezlopi_core_scenes_then_append_to_header(tmp_http_data, header->string, header->valuestring);
                }
                header = header->next;
            }
        }
        else
        {
            TRACE_E("Failed Creating header for 'Headers:'");
        }
    }
}
void EZPI_parse_http_skipsecurity(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    // TRACE_W("Here! skipsecurity");
    if (EZLOPI_VALUE_TYPE_BOOL == curr_field->value_type)
    {
        tmp_http_data->skip_cert_common_name_check = curr_field->field_value.u_value.value_bool;
        int content_size = __ezlopi_core_scenes_then_create_fresh_header(tmp_http_data);
        if (content_size > 0) // if this characters exsists in the 'tmp_http_data->header'
        {
            // 5. adding 'skip_security' to header-buffer
            // TRACE_W("Appending!! skipsecurity -> header");
            __ezlopi_core_scenes_then_append_to_header(tmp_http_data, "skipSecurity", ((curr_field->field_value.u_value.value_bool) ? ezlopi_true_str : ezlopi_false_str));
        }
        else
        {
            TRACE_E("Failed Creating header for 'skipSecurity:'");
        }
    }
}
void EZPI_parse_http_creds(s_ezlopi_core_http_mbedtls_t *tmp_http_data, l_fields_v2_t *curr_field)
{
    const cJSON *cj_value = curr_field->field_value.u_value.cj_value;
    if ((EZLOPI_VALUE_TYPE_CREDENTIAL == curr_field->value_type) && cJSON_IsObject(cj_value))
    {
        cJSON *userItem = cJSON_GetObjectItem(__FUNCTION__, cj_value, "user");
        cJSON *passwordItem = cJSON_GetObjectItem(__FUNCTION__, cj_value, "password");
        if ((NULL != userItem) && (NULL != passwordItem))
        {
            const char *userValue = cJSON_GetStringValue(userItem);
            const char *passValue = cJSON_GetStringValue(passwordItem);

            // TRACE_W("Here! credential");
            tmp_http_data->username_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->username), userValue);
            tmp_http_data->password_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->password), passValue);
        }
    }
}
void EZPI_free_http_mbedtls_struct(s_ezlopi_core_http_mbedtls_t *config)
{
    FREE_PTR_IF_NOT_NULL(config->url);
    FREE_PTR_IF_NOT_NULL(config->web_server);
    FREE_PTR_IF_NOT_NULL(config->target_page);
    FREE_PTR_IF_NOT_NULL(config->header);
    FREE_PTR_IF_NOT_NULL(config->content);
    FREE_PTR_IF_NOT_NULL(config->username);
    FREE_PTR_IF_NOT_NULL(config->password);
    FREE_PTR_IF_NOT_NULL(config->response);
}
//------------------------------ SetExpression / SetVariable -------------------------------------------------------
ezlopi_error_t EZPI_core_scenes_then_helper_set_expn_var(char *expression_name, const char *code_str, const char *value_type, cJSON *cj_metadata, cJSON *cj_params, l_fields_v2_t *var_value)
{
    ezlopi_error_t ret = EZPI_FAILED;
    s_ezlopi_expressions_t *curr_expr = EZPI_scenes_expressions_get_node_by_name(expression_name);
    if (curr_expr)
    {
        // 1. set new code
        if (code_str)
        {
            if (NULL != curr_expr->code)
            {
                ezlopi_free(__FUNCTION__, curr_expr->code);
                curr_expr->code = NULL;
            }
            curr_expr->code = ezlopi_malloc(__FUNCTION__, strlen(code_str) + 1);
            if (curr_expr->code)
            {
                /* (int) because "%.*s" reads its precision as an int and strlen
                ** yields a size_t; see the note in ezlopi_core_cjson_macros.c. */
                snprintf(curr_expr->code, strlen(code_str) + 1, "%.*s", (int)strlen(code_str), code_str);
            }
        }

        // 2. set new valueType
        if (value_type)
        {
            curr_expr->value_type = EZPI_core_scene_get_scene_value_type_enum(value_type);
        }

        // 3. replace metadata CJSON
        if (cj_metadata)
        {
            if (NULL != curr_expr->meta_data)
            {
                cJSON_Delete(__FUNCTION__, curr_expr->meta_data);
                curr_expr->meta_data = NULL;
            }

            curr_expr->meta_data = cJSON_Duplicate(__FUNCTION__, cj_metadata, 1);
        }

        // 4. replace Params CJSON
        if (cj_params)
        {
            cJSON *cj_new_items = NULL;
            if ((NULL != curr_expr->items) && (NULL != (cj_new_items = cJSON_GetObjectItem(__FUNCTION__, cj_params, "items"))))
            {
                if (curr_expr->items)
                {
                    EZPI_scenes_expressions_delete_items(curr_expr->items);
                    curr_expr->items->next = NULL;
                    curr_expr->items = NULL;
                }

                EZPI_scenes_expressions_populate_items(curr_expr, cj_new_items);
            }
            cJSON *cj_new_device_item_names = NULL;
            if ((NULL != curr_expr->device_item_names) && (NULL != (cj_new_device_item_names = cJSON_GetObjectItem(__FUNCTION__, cj_params, "device_item_names"))))
            {
                if (curr_expr->device_item_names)
                {
                    EZPI_scenes_expressions_delete_devitem_names(curr_expr->device_item_names);
                    curr_expr->device_item_names->next = NULL;
                    curr_expr->device_item_names = NULL;
                }
                EZPI_scenes_expressions_populate_devitem_names(curr_expr, cj_new_device_item_names);
            }
        }

        // 4. replace the variable value ( if its variable_expression )
        if (NULL != var_value)
        {
            switch (var_value->field_value.e_type)
            {
            case VALUE_TYPE_NUMBER:
            {
                if (EXPRESSION_VALUE_TYPE_NUMBER == curr_expr->exp_value.type)
                {
                    curr_expr->exp_value.u_value.number_value = var_value->field_value.u_value.value_double;
                }

                break;
            }
            case VALUE_TYPE_STRING:
            {
                if (EXPRESSION_VALUE_TYPE_STRING == curr_expr->exp_value.type)
                {
                    if (NULL != curr_expr->exp_value.u_value.str_value)
                    {
                        ezlopi_free(__FUNCTION__, curr_expr->exp_value.u_value.str_value);
                        curr_expr->exp_value.u_value.str_value = NULL;
                    }

                    const char *variable_str = var_value->field_value.u_value.value_string;

                    curr_expr->exp_value.u_value.str_value = ezlopi_malloc(__FUNCTION__, strlen(variable_str) + 1);
                    if (curr_expr->exp_value.u_value.str_value)
                    {
                        snprintf(curr_expr->exp_value.u_value.str_value, strlen(variable_str) + 1, "%.*s", (int)strlen(variable_str), variable_str);
                    }
                }
                break;
            }
            case VALUE_TYPE_BOOL:
            {
                if (EXPRESSION_VALUE_TYPE_BOOL == curr_expr->exp_value.type)
                {
                    curr_expr->exp_value.u_value.boolean_value = var_value->field_value.u_value.value_bool;
                }
                break;
            }
            case VALUE_TYPE_CJSON:
            {
                if (EXPRESSION_VALUE_TYPE_CJ == curr_expr->exp_value.type)
                {
                    if (NULL != curr_expr->exp_value.u_value.cj_value)
                    {
                        cJSON_Delete(__FUNCTION__, curr_expr->exp_value.u_value.cj_value);
                    }

                    curr_expr->exp_value.u_value.cj_value = cJSON_Duplicate(__FUNCTION__, var_value->field_value.u_value.cj_value, 1);
                }
                break;
            }
            default:
                break;
            }
        }

        // TRACE_S("-------------- Updated_exp_name : %s  ------------", expression_name);
        // EZPI_scenes_expressions_print(EZPI_scenes_expressions_get_node_by_name(expression_name));
        // TRACE_S("----------------- ll --------------------");

        // 5. Now to edit in nvs
        // A. read from  nvs
        char *exp_id_list_str = EZPI_core_nvs_read_scenes_expressions();
        if (exp_id_list_str)
        {
            TRACE_D("exp_id_list_str: %s", exp_id_list_str);
            cJSON *cj_exp_id_list = cJSON_Parse(__FUNCTION__, exp_id_list_str);
            if (cj_exp_id_list)
            {
                cJSON *cj_exp_id = NULL;
                cJSON_ArrayForEach(cj_exp_id, cj_exp_id_list)
                {
                    if (cj_exp_id && cj_exp_id->valuestring)
                    {
                        uint32_t exp_id = strtoul(cj_exp_id->valuestring, NULL, 16);
                        if (exp_id == curr_expr->exp_id)
                        {
                            TRACE_S("Found [%#x] in nvs ; req[%#x]", exp_id, curr_expr->exp_id);
                            ret = EZPI_scenes_expressions_update_nvs(cj_exp_id->valuestring, EZPI_scenes_expressions_get_cjson(curr_expr));
                            break;
                        }
                    }
                }
                cJSON_Delete(__FUNCTION__, cj_exp_id_list);
            }
            ezlopi_free(__FUNCTION__, exp_id_list_str);
        }
    }
    return ret;
}
//------------------------------ toggleValue + grouptoggleValue -------------------------------------------------------
int EZPI_core_scenes_then_helper_toggleValue(uint32_t item_id, const char *item_id_str)
{
    int ret = 0;
    if (item_id && item_id_str)
    {
        l_ezlopi_item_t *curr_item = EZPI_core_device_get_item_by_id(item_id);
        if ((curr_item) && (EZLOPI_DEVICE_INTERFACE_DIGITAL_OUTPUT == curr_item->interface_type))
        {
            cJSON *cj_tmp_value = cJSON_CreateObject(__FUNCTION__);
            if (cj_tmp_value)
            {
                if (curr_item->func(EZLOPI_ACTION_GET_EZLOPI_VALUE, curr_item, (void *)cj_tmp_value, NULL))
                {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
                    CJSON_TRACE("present_bool_gpio_value", cj_tmp_value); /*value formatted & value only*/
#endif

                    cJSON *cj_val = cJSON_GetObjectItem(__FUNCTION__, cj_tmp_value, ezlopi_value_str);
                    if (cj_val)
                    {
                        cJSON *cj_result_value = cJSON_CreateObject(__FUNCTION__);
                        if (cj_result_value)
                        {
                            cJSON_AddStringToObject(__FUNCTION__, cj_result_value, ezlopi__id_str, item_id_str);

                            if ((EZPI_STRNCMP_IF_EQUAL(curr_item->cloud_properties.value_type, value_type_bool, strlen(curr_item->cloud_properties.value_type) + 1, 5)) && cJSON_IsBool(cj_val))
                            {
                                // TRACE_S("1. getting 'item_id[%d]' ; bool_value = %s ", item_id, (cj_val->type == cJSON_True) ? ezlopi_true_str : ezlopi_false_str); // ezlopi_false_str or ezlopi_true_str
                                if (cj_val->type == cJSON_True)
                                {
                                    cJSON_AddBoolToObject(__FUNCTION__, cj_result_value, ezlopi_value_str, false);
                                }
                                else if (cj_val->type == cJSON_False)
                                {
                                    cJSON_AddBoolToObject(__FUNCTION__, cj_result_value, ezlopi_value_str, true);
                                }
                                ret = 1;
                                curr_item->func(EZLOPI_ACTION_SET_VALUE, curr_item, cj_result_value, curr_item->user_arg);
                            }
                            else if ((EZPI_STRNCMP_IF_EQUAL(curr_item->cloud_properties.value_type, value_type_int, strlen(curr_item->cloud_properties.value_type) + 1, 4)) && cJSON_IsNumber(cj_val))
                            {
                                // TRACE_S("2. getting 'item_id[%d]' ; int_value = %d ", item_id, (int)cj_val->valuedouble);
                                if (cj_val->valuedouble == 0) // either  '0' or '1'.
                                {
                                    cJSON_AddNumberToObject(__FUNCTION__, cj_result_value, ezlopi_value_str, 1);
                                }
                                else if (cj_val->valuedouble == 1)
                                {
                                    cJSON_AddNumberToObject(__FUNCTION__, cj_result_value, ezlopi_value_str, 0);
                                }
                                ret = 1;
                                curr_item->func(EZLOPI_ACTION_SET_VALUE, curr_item, cj_result_value, curr_item->user_arg);
                            }
                            else
                            {
                                ret = 0;
                                TRACE_E(" 'item_id[%d]' neither 'boolean' nor 'int' ;  Value-type mis-matched!  ", item_id);
                            }

                            cJSON_Delete(__FUNCTION__, cj_result_value);
                        }
                    }
                }

                cJSON_Delete(__FUNCTION__, cj_tmp_value);
            }
        }
    }
    return ret;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/
/**
 * @brief This funtion is called, only to reallocate a '*header' of custom_structure 's_ezlopi_core_http_mbedtls_t'
 *
 * @param tmp_http_data     [ Pointer to (s_ezlopi_core_http_mbedtls_t*) block of memory. ]
 * @param append_size       [ Size of 'string' to be appended. ]
 * @param append_str        [ 'string_literal' to be appended. ]
 * @return int [ Fail ==> returns Old-size / Success ==> returns New-size ]
 */
static int __ezlopi_core_scenes_then_sendhttp_relloc_header(s_ezlopi_core_http_mbedtls_t *tmp_http_data, int append_size, const char *append_str)
{
    int ret = (int)tmp_http_data->header_maxlen; // Assign Old-block size as default
    int new_size = append_size + (ret + 1);

    uint8_t retry = 5;
    do
    {
        if (EZPI_SUCCESS == EZPI_core_http_dyna_relloc(&(tmp_http_data->header), new_size)) // rellocate: 'tmp_http_data->header' with  'new_size'
        {
            snprintf((tmp_http_data->header) + strlen(tmp_http_data->header), append_size, "%s", append_str);
            ret = new_size; // return new memory-block size
            // TRACE_I("Append Successful: Header[size: %d , occupied: %d]", ret, STR_SIZE(tmp_http_data->header));
            break;
        }
        else
        {
            TRACE_E("Append_Failed... ; returning original header_contents [RETRY:%d]", retry);
            retry--;
        }
    } while (retry > 0);
    return ret;
}

#if 0
/**
 * @brief Function to extract "web_host" from "field_value_string".
 */
static void __ezlopi_core_scenes_then_sendhttp_parse_host_name(s_ezlopi_core_http_mbedtls_t *tmp_http_data, const char *field_value_string)
{
    if (NULL != field_value_string)
    {
        const char *start = strstr(field_value_string, "://");
        if (start != NULL)
        {
            // TRACE_W("Here! fresh webserver");
            start += 3;
            int length = 0;
            char *end = strchr(start, '/');
            if (end != NULL)
            {
                length = (end - start);
                if (length > 0)
                {
                    length++;                                         // include null character
                    char *tmp_string = ezlopi_malloc(__FUNCTION__, length); // tmp_string != NULL
                    if (tmp_string)
                    {
                        bzero(tmp_string, length);
                        snprintf(tmp_string, length, "%s", start);
                        // TRACE_I("web_host_name : %s", tmp_string);
                        tmp_http_data->web_server_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->web_server), tmp_string);
                        ezlopi_free(__FUNCTION__, tmp_string);
                    }
                }
            }
            else
            {
                const char *ptr = field_value_string;
                length = (int)strlen(field_value_string) - (int)(start - ptr);
                if (length > 0)
                {
                    length++;                                         // include null character
                    char *tmp_string = ezlopi_malloc(__FUNCTION__, length); // tmp_string != NULL
                    if (tmp_string)
                    {
                        bzero(tmp_string, length);
                        snprintf(tmp_string, length, "%s", (ptr + ((int)(start - ptr))));
                        // TRACE_I("web_host_name : %s", tmp_string);
                        tmp_http_data->web_server_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->web_server), tmp_string);
                        ezlopi_free(__FUNCTION__, tmp_string);
                    }
                }
            }
        }
    }
}
#endif

/**
 * @brief Function to Clear and Malloc the header_member (within 's_ezlopi_core_http_mbedtls_t') only.
 * @return Size of content in 's_ezlopi_core_http_mbedtls_t'->header
 */
static int __ezlopi_core_scenes_then_create_fresh_header(s_ezlopi_core_http_mbedtls_t *tmp_http_data)
{
    int ret = STR_SIZE(tmp_http_data->header);

    if ((NULL == tmp_http_data->header) && (0 == ret))
    {
        // TRACE_W("Here! fresh header init");
        tmp_http_data->header_maxlen = (uint16_t)EZPI_core_http_mem_malloc(&(tmp_http_data->header), "\r\0");
        ret = STR_SIZE(tmp_http_data->header);
        // TRACE_W("Here!! Created fresh header-> [capacity: %d] , [occupied: %d]", tmp_http_data->header_maxlen, ret);
    }
    return ret;
}
/**
 * @brief Function to append values to header_member (within 's_ezlopi_core_http_mbedtls_t') only.
 */
static void __ezlopi_core_scenes_then_append_to_header(s_ezlopi_core_http_mbedtls_t *tmp_http_data, const char *str1, const char *str2)
{
    int append_size = (STR_SIZE(str1) + 4 + STR_SIZE(str2)) + 6;
    int max_allowed = EZPI_core_http_calc_empty_bufsize(tmp_http_data->header, (tmp_http_data->header_maxlen), append_size);
    if (max_allowed > 0)
    {
        snprintf(tmp_http_data->header + STR_SIZE(tmp_http_data->header), max_allowed, "%s: %s\r\n", str1, str2);
    }
    else // We reallocate:- 'tmp_http_data->header'
    {
        char *append_str = ezlopi_malloc(__FUNCTION__, append_size); // append_str != NULL
        if (append_str)
        {
            bzero(append_str, append_size);
            snprintf(append_str, append_size, "%s: %s\r\n", str1, str2);

            //-----------------------------------------------------------------------------------
            // TRACE_D("Append_str: %s[%d] ", append_str, append_size);
            // TRACE_D("-> Before => Realloc_Header:-[capacity:%d (occupied:%d)]->[needed:%d]", tmp_http_data->header_maxlen, STR_SIZE(tmp_http_data->header), append_size);
            tmp_http_data->header_maxlen = (uint16_t)__ezlopi_core_scenes_then_sendhttp_relloc_header(tmp_http_data, append_size, append_str);
            // TRACE_D("-> After => Realloc_Header:-[capacity:%d (occupied:%d)]", tmp_http_data->header_maxlen, STR_SIZE(tmp_http_data->header));
            //-----------------------------------------------------------------------------------
            ezlopi_free(__FUNCTION__, append_str);
        }
    }
}

/**
 * @brief Lowercases one ASCII letter, for comparing a URL scheme.
 *
 * Hand-rolled rather than tolower() because a scheme is ASCII by definition
 * (RFC 3986 section 3.1 gives its grammar as ALPHA *( ALPHA / DIGIT / "+" /
 * "-" / "." )), so this needs none of what tolower() brings with it: no
 * <ctype.h>, no dependence on the C locale, and no sign-extension question
 * about passing a plain char to a function that is specified in terms of
 * unsigned char.
 */
static char __ezlopi_core_scenes_then_lower_ascii(char ch)
{
    return ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch - 'A' + 'a') : ch;
}

/**
 * @brief Finds the authority and the path in a URL whose scheme this firmware
 * can speak, and refuses every other scheme rather than guessing.
 *
 * Refusing is the answer to the question the nine-arm ladder left open. There
 * is exactly one transport on this path: EZPI_core_http_mbedtls_req always
 * calls __ezlopi_http_req_via_mbedTLS, which always performs a TLS handshake.
 * So a scheme does not select a transport here -- it only selects a default
 * port -- and a scheme this code does not implement is a request that cannot
 * be served however carefully it is parsed. A refusal says so in the log; the
 * ladder's ninth arm instead sent an HTTPS request to whatever port an ftp://
 * URL named.
 *
 * Nothing is scanned and nothing is bounded by a conversion width. The
 * authority is copied out so that the split below can write terminators into
 * it, and is refused rather than truncated when it does not fit -- truncating
 * a host aims the request at a different machine, which the replaced arms
 * seven and eight did at 99 characters without a word. The path is not copied
 * at all: '*page_out' points into the caller's URL, so there is no width to
 * truncate it at and no second buffer to size.
 *
 * @param url            [ The URL as the action supplied it. ]
 * @param authority      [ Out. The "[userinfo@]host[:port]", for
 *                         __ezlopi_core_scenes_then_split_authority to take
 *                         apart in place. ]
 * @param authority_size [ sizeof that buffer. ]
 * @param port_out       [ Out. The scheme's default port, 80 or 443. The
 *                         authority split overwrites it if the URL names one. ]
 * @param page_out       [ Out. Points into 'url', at the character after the
 *                         '/' that ends the authority, or at a '?' or '#'
 *                         because those have no separator to drop, or at "" if
 *                         the URL is bare authority. Never NULL. ]
 * @return bool [ false if the scheme is not http or https, if there is no
 *                authority, or if the authority does not fit. ]
 */
static bool __ezlopi_core_scenes_then_split_url(const char *url, char *authority, size_t authority_size, int *port_out, const char **page_out)
{
    /* Order does not matter here, and that is worth stating because a prefix
    ** table usually has to be longest-first: neither of these two is a prefix
    ** of the other. "http://" against an https URL disagrees at index 4 (':'
    ** against 's') and "https://" against an http URL disagrees at the same
    ** index the other way, so at most one can ever match. */
    static const struct
    {
        const char *prefix;
        int port;
    } k_schemes[] = {
        {"http://", 80},
        {"https://", 443},
    };

    if ((NULL == url) || (NULL == authority) || (0 == authority_size) ||
        (NULL == port_out) || (NULL == page_out))
    {
        return false;
    }

    /* No space and no control character, anywhere in the URL, before anything
    ** else is looked at.
    **
    ** This is not tidiness. __ezlopi_http_generate_request writes the page into
    ** "GET /%s HTTP/1.1\r\n" and the host into "Host: %s\r\n", so a space in
    ** either splits the request line -- "GET /a b HTTP/1.1" has the path "/a"
    ** and the version "b" -- and a CR or LF ends it early and starts whatever
    ** follows as a new header. The `url` field of a sendHttpRequest action is
    ** request-supplied and uncapped, and hub.scenes.action.block.test runs this
    ** parse on a block taken straight off the wire without a scene ever being
    ** stored, so this is reachable without storing anything.
    **
    ** Against the ladder this both fixes and does not regress, which is why the
    ** check is here rather than left as a note. Arm five's `%199[^\n]` stopped
    ** at a newline, so dropping the conversion without replacing this would
    ** have let one through for the first time -- the measured shipped answer
    ** for "http://example.com/a\nb" is the page "a", silently the wrong path
    ** rather than an injected request. But the same conversion accepted a
    ** space and a tab and a carriage return, so "http://example.com/a b" was a
    ** request line in two pieces and "http://exam ple.com/x" was a Host:
    ** header in two pieces, both shipped and both refused now.
    **
    ** 0x20 and below, and 0x7F. Cast through unsigned char because plain char's
    ** signedness is implementation-defined -- it is signed on the host this is
    ** tested on and unsigned on xtensa -- and a comparison that depends on that
    ** would pass here and do something else on the target.
    **
    ** High bytes are not refused. A byte above 0x7E is not a valid URI
    ** character either (RFC 3986 is ASCII and percent-encoding is what exists
    ** for the rest), but it cannot break the structure of a request line, and
    ** refusing a UTF-8 host would be a behaviour change rather than a fix.
    ** Recorded as a Known gap instead. */
    for (const char *scan = url; '\0' != *scan; scan++)
    {
        if (((unsigned char)*scan <= 0x20u) || (0x7Fu == (unsigned char)*scan))
        {
            TRACE_E("URL holds a space or a control character at offset %d -- refusing rather than writing it into a request line",
                    (int)(scan - url));
            return false;
        }
    }

    const char *rest = NULL;
    for (unsigned idx = 0; idx < (sizeof(k_schemes) / sizeof(k_schemes[0])); idx++)
    {
        const char *prefix = k_schemes[idx].prefix;
        unsigned at = 0;
        /* Stops at the first disagreement, and the URL's terminator is a
        ** disagreement with every character a prefix can hold, so this cannot
        ** read past the end of a URL shorter than the prefix. */
        while (('\0' != prefix[at]) &&
               (__ezlopi_core_scenes_then_lower_ascii(url[at]) == prefix[at]))
        {
            at++;
        }
        if ('\0' == prefix[at])
        {
            rest = url + at;
            *port_out = k_schemes[idx].port;
            break;
        }
    }

    if (NULL == rest)
    {
        TRACE_E("URL names a scheme this firmware does not speak -- only http and https are implemented");
        return false;
    }

    /* '?' and '#' end the authority as well as '/'. Without them a URL that
    ** carries a query and no path -- "http://example.com?q=a" -- leaves the
    ** query inside the authority, which is how it used to reach arm seven's
    ** catch-all and become a Host: header of "example.com?q=a". The authority
    ** split declines to read a port out of a string holding either character
    ** for exactly that reason; ending the authority here means it no longer
    ** has to. */
    const char *sep = strpbrk(rest, "/?#");
    size_t auth_len = (NULL != sep) ? (size_t)(sep - rest) : strlen(rest);

    if (0 == auth_len)
    {
        TRACE_E("URL has a scheme but no host");
        return false;
    }
    if (auth_len >= authority_size)
    {
        TRACE_E("URL authority is %d characters and this parser holds %d -- refusing rather than truncating a host",
                (int)auth_len, (int)(authority_size - 1));
        return false;
    }

    memcpy(authority, rest, auth_len);
    authority[auth_len] = '\0';

    /* The '/' belongs to the request line, not to the page:
    ** __ezlopi_http_generate_request writes "GET /%s", and the eight arms this
    ** replaces consumed the separator themselves. A '?' or '#' has no
    ** separator to drop, so it is kept and "GET /?q=a" comes out right. */
    if (NULL == sep)
    {
        *page_out = "";
    }
    else if ('/' == *sep)
    {
        *page_out = sep + 1;
    }
    else
    {
        *page_out = sep;
    }

    return true;
}

/**
 * @brief Splits an authority -- the "[userinfo@]host[:port]" that
 * __ezlopi_core_scenes_then_split_url hands over -- into its parts.
 *
 * In place, by writing terminators into 'authority' and returning a pointer
 * into it. Nothing is copied and nothing is moved: the returned host points
 * past the userinfo rather than having the userinfo shifted out from under it,
 * which is what keeps '*user_out' and '*pass_out' valid -- they point at the
 * front of the same buffer. All three pointers are therefore only good for as
 * long as the caller's buffer is, which is why the caller copies out of them
 * through EZPI_core_http_mem_malloc before the buffer leaves scope.
 *
 * @param authority [ The scanned authority, modified in place. ]
 * @param user_out  [ Set to the username, or left NULL if there is no
 *                    userinfo or the part before its ':' is empty. ]
 * @param pass_out  [ Set to the password, or left NULL if the userinfo has no
 *                    ':' at all. An empty string here means "a ':' with
 *                    nothing after it", which is not the same answer. ]
 * @param port_io   [ Overwritten only when an explicit port is found, so the
 *                    scheme default the caller put there -- 80 or 443 --
 *                    survives a URL that names no port. ]
 * @return char* [ The bare host, inside 'authority'; or NULL when a ':' is left
 *                 in it that is neither a port this function accepts nor part
 *                 of a bracketed IPv6 literal. May be the empty string, which
 *                 the caller refuses -- "no host" and "malformed port" are
 *                 different messages and are reported separately. ]
 */
static char *__ezlopi_core_scenes_then_split_authority(char *authority, char **user_out, char **pass_out, int *port_io)
{
    char *host = authority;
    *user_out = NULL;
    *pass_out = NULL;

    /* The last '@' and not the first. RFC 3986 section 3.2.1 lets a userinfo
    ** carry sub-delims, '@' among the characters a password is percent-encoded
    ** to avoid but not required to be, so "a:b@c@example.com" has the userinfo
    ** "a:b@c". Splitting at the first '@' would make the host "c@example.com",
    ** which is not a host at all. */
    char *at_ptr = strrchr(host, '@');
    if (NULL != at_ptr)
    {
        char *userinfo = host;
        *at_ptr = '\0';
        host = at_ptr + 1;

        /* And the first ':' here, for the mirror-image reason: a password may
        ** contain one, a username may not. */
        char *cred_sep = strchr(userinfo, ':');
        if (NULL != cred_sep)
        {
            *cred_sep = '\0';
            *pass_out = cred_sep + 1;
        }
        if ('\0' != *userinfo)
        {
            *user_out = userinfo;
        }
    }

    /* The port. This is now the only place in this file that reads one -- four
    ** of the nine sscanf arms used to scan their own with %i, and did it wrong
    ** in six measured ways, so the fix was to route every URL through here.
    **
    ** The last ':' rather than the first, because an unbracketed IPv6 address
    ** has several and the port is after all of them. The tail must then be
    ** entirely digits denoting 1..65535, which is what leaves a bracketed IPv6
    ** literal alone: "http://[::1]/x" leaves host = "[::1]", whose last ':' is
    ** inside the address and whose tail is "1]". Splitting there would hand
    ** mbedtls "[:" as a host.
    **
    ** The '<= 65535' inside the loop condition is what keeps 'value' from
    ** overflowing on a long run of digits; the test after it then fails on the
    ** digit the loop stopped at, so ":99999999999999" is refused rather than
    ** wrapped. That input was port 276447231 under %i, undefined behaviour the
    ** sanitizers could not see because it happened inside libc. With the bound
    ** in place 'value' never exceeds 655359, which is what makes this correct
    ** on a target where long is 32 bits and not merely on the 64-bit host --
    ** and that difference is why the bound has to stay even though deleting it
    ** is the one mutation tests/host/test_httpurl.c cannot be made to catch.
    ** On the host the removal is harmless (99999999999999 fits in a 64-bit
    ** long and the range test refuses it anyway); on xtensa it is signed
    ** overflow. Measured as a survivor, kept deliberately, not dead code.
    **
    ** Two guards have been removed as provably dead rather than kept for
    ** safety, each after a mutation showed deleting it changed no observable
    ** behaviour:
    **
    **   - a tail containing ']' was rejected. ']' is not a digit, so no string
    **     the all-digits test accepts can hold one.
    **   - a host containing '?' or '#' was rejected, because the catch-all arms
    **     %99[^\n] handed a query string in whole and "http://example.com?q=a:5"
    **     would otherwise have grown a port 5. __ezlopi_core_scenes_then_split_url
    **     now ends the authority at '?' and '#' as well as '/', so neither
    **     character can reach this function from its only caller. */
    char *port_sep = strrchr(host, ':');
    if (NULL != port_sep)
    {
        const char *digits = port_sep + 1;
        long value = 0;
        int idx = 0;
        while ((digits[idx] >= '0') && (digits[idx] <= '9') && (value <= 65535))
        {
            value = (value * 10) + (digits[idx] - '0');
            idx++;
        }
        if (('\0' == digits[idx]) && (idx > 0) && (value > 0) && (value <= 65535))
        {
            *port_sep = '\0';
            *port_io = (int)value;
        }
    }

    /* And then one question about whatever ':' is left, which is what turns
    ** every rejected port into a refusal instead of a strange host.
    **
    ** A successful split above wrote a terminator over its own colon, so a
    ** colon still standing here is one this function declined to read a port
    ** out of -- ":0x50", ":8090junk", ":99999", ":-1", or the bare trailing
    ** ":" of "http://example.com:/x". Returning the host with the colon still
    ** in it is what the ladder's catch-all arms did: "example.com:" went to
    ** the resolver and into the Host: header. The colon is not a character a
    ** host may contain, so the honest answer is to refuse the URL.
    **
    ** Except inside a bracketed IPv6 literal, where every colon is part of the
    ** address and therefore sits before the ']'. One comparison covers that,
    ** and covers the double-port "example.com:8080:9090" at the same time: the
    ** valid port is split off the end and the colon that is left has no bracket
    ** in front of it.
    **
    ** Both pointers are into 'host' when the comparison is reached, so the
    ** relational test is between pointers into one object. The NULL case is
    ** tested separately rather than compared, because comparing a pointer
    ** against a null pointer with '>' is not something C defines. */
    const char *stray_colon = strrchr(host, ':');
    if (NULL != stray_colon)
    {
        const char *bracket = strrchr(host, ']');
        if ((NULL == bracket) || (stray_colon > bracket))
        {
            TRACE_E("URL names a port that is not a decimal number in 1..65535, or a host holding a stray ':'");
            return NULL;
        }
    }

    return host;
}

#endif // CONFIG_EZPI_SERV_ENABLE_MESHBOTS

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
