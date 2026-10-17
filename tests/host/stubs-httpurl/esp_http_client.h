/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    esp_http_client.h
 * @brief   Host-test stub: the two types ezlopi_core_http.h takes from the IDF.
 *
 * ezlopi_core_http.h names esp_http_client_method_t in
 * s_ezlopi_core_http_mbedtls_t::method and HttpStatus_Code in
 * s_ezlopi_http_data_t::status_code, so the header cannot be compiled on a host
 * without them. test_httpurl compiles the real scenes then-methods translation
 * unit, which includes that header, and EZPI_parse_http_request_type assigns
 * four of the HTTP_METHOD_* enumerators -- so the enumerator values are
 * load-bearing for that test, not decoration.
 *
 * Both enums are copied from the installed IDF v4.4.6,
 * components/esp_http_client/include/esp_http_client.h -- esp_http_client_method_t
 * from the block ending at :180 and HttpStatus_Code from :144-164 -- rather
 * than reconstructed. test_httpurl asserts HTTP_METHOD_GET == 0 and
 * HTTP_METHOD_DELETE == 4 against these, which is a weak check against this
 * file but a real one against an edit to it.
 *
 * Nothing else from esp_http_client is declared. The scenes code reaches the
 * network through EZPI_core_http_mbedtls_req, not through this client, so a
 * source that actually called esp_http_client_perform() would fail to link
 * here -- which is the behaviour wanted, for the reason stubs/driver/gpio.h
 * gives at length.
 */

#ifndef _HOST_STUB_ESP_HTTP_CLIENT_H_
#define _HOST_STUB_ESP_HTTP_CLIENT_H_

typedef enum
{
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_NOTIFY,
    HTTP_METHOD_SUBSCRIBE,
    HTTP_METHOD_UNSUBSCRIBE,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_COPY,
    HTTP_METHOD_MOVE,
    HTTP_METHOD_LOCK,
    HTTP_METHOD_UNLOCK,
    HTTP_METHOD_PROPFIND,
    HTTP_METHOD_PROPPATCH,
    HTTP_METHOD_MKCOL,
    HTTP_METHOD_MAX,
} esp_http_client_method_t;

typedef enum
{
    /* 2xx - Success */
    HttpStatus_Ok = 200,

    /* 3xx - Redirection */
    HttpStatus_MultipleChoices = 300,
    HttpStatus_MovedPermanently = 301,
    HttpStatus_Found = 302,
    HttpStatus_SeeOther = 303,
    HttpStatus_TemporaryRedirect = 307,
    HttpStatus_PermanentRedirect = 308,

    /* 4xx - Client Error */
    HttpStatus_BadRequest = 400,
    HttpStatus_Unauthorized = 401,
    HttpStatus_Forbidden = 403,
    HttpStatus_NotFound = 404,

    /* 5xx - Server Error */
    HttpStatus_InternalError = 500
} HttpStatus_Code;

#endif /* _HOST_STUB_ESP_HTTP_CLIENT_H_ */
