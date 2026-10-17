/* Host tests for EZPI_parse_http_url() -- the nine-arm sscanf ladder that turns
 * a sendHttpRequest action's `url` field into a host, a port and a page.
 *
 * WHY THIS FILE EXISTS
 *
 * Two defects, both reachable from a cloud or local-API request, and both
 * found by reading the ladder rather than by running it:
 *
 *   1. A URL carrying credentials -- http://user:pass@example.com/x -- does not
 *      take any of the arms that look like they would handle it. Arms one to
 *      four want an integer straight after the first colon and `pass@...` is
 *      not one, so it falls through to arm five, whose conversion is
 *      `%99[^/]`: the whole authority, credentials included. `host` then
 *      becomes the string "user:pass@example.com". That string is handed to
 *      EZPI_core_http_mem_malloc for `web_server` and appended verbatim as the
 *      `Host:` header, so the password ends up in the request the firmware
 *      assembles, not merely on the debug UART.
 *
 *   2. The same URL with an explicit port -- http://user:pass@example.com:8080/x
 *      -- also lands on arm five, which never looks at a port. `port` keeps the
 *      80 it was initialised with, so the explicit port is silently discarded.
 *
 * The first draft of the audit entry for this got both halves wrong: it said
 * such a URL "takes the first arm" and parsed into a "nonsense host". It takes
 * arm five, and the host is not nonsense -- it is the credentials. That draft
 * was written from a reading of the arms, and so was its correction, and so is
 * the paragraph above. This file is here so that the next claim about this
 * ladder is settled by running it instead.
 *
 * WHAT IS BEING RUN
 *
 * The real ezlopi_core_scenes_then_methods_helper_func.c, compiled for the
 * host -- not a transcription of the ladder into this file. The distinction is
 * the whole point. The audit's earlier measurements of this ladder were taken
 * against a copy pasted out of the source, which establishes what sscanf does
 * but not that the copy matched; two of those measurements turned out to be
 * about a copy that had drifted. Compiling the translation unit removes that
 * class of error. It costs seven stub headers (stubs-httpurl/) and thirteen
 * stubbed symbols (stub_httpurl.c), and the Makefile rule checks with nm that
 * the subject is really in the object before it links, for the reason
 * stubs-scenes/sdkconfig.h gives.
 *
 * WHAT IS NOT BEING RUN
 *
 * The trace output. This suite's ezlopi_util_trace.h stub makes TRACE_* a
 * no-op, so the three `%s` traces inside the subject -- `*source`, `host` and
 * `page` -- produce nothing here. Whether a credential reaches the UART is
 * therefore not decidable from this file, and the gate on the `*source` trace
 * is checked the way the other payload gates in this tree are: by probing the
 * target image's rodata for the format string.
 *
 * HOW THE ASSERTIONS ARE ADJUDICATED
 *
 * Two surfaces, deliberately. The struct the subject fills, which is what the
 * request builder downstream reads; and the list of strings the subject handed
 * to EZPI_core_http_mem_malloc, recorded by the stub. The second matters
 * because that stub is a reimplementation of a real firmware function sitting
 * in the assertion path -- see the long note in stub_httpurl.c -- and an
 * assertion about what the subject *asked for* cannot be made to pass wrongly
 * by a copy that drifted.
 *
 * The field-width case at the end is adjudicated by AddressSanitizer rather
 * than by an assert, which is why the suite's -fsanitize flags matter here:
 * before the widths were added to arm nine, a long host overflowed host[100]
 * by writing past its end, and a write into unused stack passes any assert.
 *
 * Run: make -C tests/host && ./tests/host/build/test_httpurl
 */

/* sdkconfig.h first, as every firmware translation unit does: the scenes
 * headers are guarded by CONFIG_EZPI_SERV_ENABLE_MESHBOTS and expand to
 * nothing without it. */
#include "sdkconfig.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"
#include "ezlopi_core_http.h"
#include "ezlopi_core_scenes_v2.h"
#include "ezlopi_core_scenes_then_methods_helper_func.h"

#include "stub_httpurl.h"

static int checks = 0;
#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(cond))                                              \
        {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
            exit(1);                                              \
        }                                                         \
    } while (0)

/* A string comparison that tolerates NULL on either side, because "the subject
 * left this member NULL" is one of the answers under test and strcmp(NULL, x)
 * is not a way to ask about it. */
static int streq(const char *a, const char *b)
{
    if ((NULL == a) || (NULL == b))
    {
        return a == b;
    }
    return 0 == strcmp(a, b);
}

/* A `url` field exactly as EZPI_scenes_populate_fields_get_value leaves one:
 * value_type EZLOPI_VALUE_TYPE_STRING and an owned copy of the request's
 * string. The copy is deliberate -- the subject must not need to modify the
 * caller's buffer, and ASan will say so if it reads off the end of this one. */
static l_fields_v2_t *url_field(const char *url)
{
    l_fields_v2_t *f = malloc(sizeof(l_fields_v2_t));
    assert(f);
    memset(f, 0, sizeof(l_fields_v2_t));
    snprintf(f->name, sizeof(f->name), "%s", "url");
    f->value_type = EZLOPI_VALUE_TYPE_STRING;
    f->field_value.e_type = VALUE_TYPE_STRING;
    f->field_value.u_value.value_string = strdup(url);
    assert(f->field_value.u_value.value_string);
    return f;
}

static void free_url_field(l_fields_v2_t *f)
{
    free(f->field_value.u_value.value_string);
    free(f);
}

/* One run of the subject against one URL, from a zeroed struct -- which is how
 * EZPI_core_scene_then_send_http_request starts, by memset. */
static void run(const char *url, s_ezlopi_core_http_mbedtls_t *out)
{
    l_fields_v2_t *f = url_field(url);
    memset(out, 0, sizeof(*out));
    stub_httpurl_reset();
    EZPI_parse_http_url(out, f);
    free_url_field(f);
}

/* ------------------------------------------------------------------------- */

typedef struct s_url_case
{
    const char *url;
    const char *host; /* expected web_server; NULL means "not set" */
    int port;         /* expected web_port; -1 means "not set"     */
    const char *page; /* expected target_page                      */
    const char *user; /* expected username; NULL means "not set"   */
    const char *pass;
} s_url_case_t;

/* The eight shapes the author wrote the ladder for, kept as comments at the top
 * of the function under test, plus a non-http scheme for arm nine. These are
 * here to pin the arms that already worked: a change to the ladder that fixes
 * credentials by breaking ordinary URLs is the obvious way to make this worse,
 * and these are what would catch it. */
static const s_url_case_t ordinary[] = {
    {"http://www.testhttp.com:8090/foo_page", "www.testhttp.com", 8090, "foo_page", NULL, NULL},
    {"https://www.testhttp.com:8090/foo_page", "www.testhttp.com", 8090, "foo_page", NULL, NULL},
    {"http://www.testhttp.com/foo_page", "www.testhttp.com", 80, "foo_page", NULL, NULL},
    {"https://www.testhttp.com/foo_page", "www.testhttp.com", 443, "foo_page", NULL, NULL},
    {"http://www.testhttp.com:8090", "www.testhttp.com", 8090, "", NULL, NULL},
    {"https://www.testhttp.com:8090", "www.testhttp.com", 8090, "", NULL, NULL},
    {"http://www.testhttp.com", "www.testhttp.com", 80, "", NULL, NULL},
    {"https://www.testhttp.com", "www.testhttp.com", 443, "", NULL, NULL},
    /* Arm nine, the fallback for a scheme that is neither http nor https. It is
     * the only arm that scans a port of its own outside arms one to four.
     *
     * The expected page is "/x" and not "x", which is a defect this change does
     * not fix and which running the ladder is what found. Arms one, two, five
     * and six all consume the separator themselves -- their formats spell
     * "...%i/%199[^\n]" and "...%99[^/]/%199[^\n]" -- so `page` comes out
     * without a leading slash. Arm nine's format ends "%d%199s", which stops
     * the %d at the '/' and then lets %199s take the slash along with the rest.
     * __ezlopi_http_generate_request emits "GET /%s", so a non-http scheme
     * produces a request line reading "GET //x HTTP/1.1".
     *
     * Asserted as measured rather than as it ought to be, and recorded in the
     * audit's Known gaps. The alternative -- asserting "x" and leaving the test
     * red -- would make this suite's green mean "except for the bits we have
     * decided to ignore", which is worth less than a recorded defect. */
    {"ftp://example.com:21/x", "example.com", 21, "/x", NULL, NULL},
};

/* The credentialed shapes. Every one of these reaches an arm that scans the
 * whole authority into `host`, which is how the credentials got there. */
static const s_url_case_t credentialed[] = {
    {"http://user:pass@example.com/x", "example.com", 80, "x", "user", "pass"},
    {"http://user:pass@example.com", "example.com", 80, "", "user", "pass"},
    {"https://user:pass@example.com/x", "example.com", 443, "x", "user", "pass"},
    {"https://user:pass@example.com", "example.com", 443, "", "user", "pass"},
    /* The swallowed port: arm five takes this and never looks for one. */
    {"http://user:pass@example.com:8080/x", "example.com", 8080, "x", "user", "pass"},
    {"https://user:pass@example.com:8443/x", "example.com", 8443, "x", "user", "pass"},
    {"http://user:pass@example.com:8080", "example.com", 8080, "", "user", "pass"},
    /* A userinfo with no password at all. RFC 3986 allows it, and the split has
     * to leave `password` alone rather than inventing an empty one -- the
     * request builder tests both members before emitting Authorization. */
    {"http://user@example.com/x", "example.com", 80, "x", "user", NULL},
    /* An empty password after the colon is not the same as no colon. The
     * request builder requires both members to be non-empty, so this stays out
     * of the Authorization header either way, but it must not be confused with
     * the case above. */
    {"http://user:@example.com/x", "example.com", 80, "x", "user", ""},
    /* The two rows that pin which colon and which '@' the split uses. Both
     * were added after a mutation run: changing strchr to strrchr on the
     * userinfo colon, and strrchr to strchr on the authority '@', each left all
     * the rows above passing. The choices are deliberate and documented in the
     * helper, so they are worth a row each.
     *
     * RFC 3986 section 3.2.1 allows both characters in a userinfo: ':' is the
     * separator so only the first one can be it, and '@' is not the separator
     * so only the last one can be. */
    {"http://user:pa:ss@example.com/x", "example.com", 80, "x", "user", "pa:ss"},
    {"http://user:p@ss@example.com/x", "example.com", 80, "x", "user", "p@ss"},
};

static void check_case(const s_url_case_t *c)
{
    s_ezlopi_core_http_mbedtls_t h;
    char what[320];

    run(c->url, &h);

    snprintf(what, sizeof(what), "%s -> web_server '%s' (got '%s')",
             c->url, c->host ? c->host : "(null)",
             h.web_server ? h.web_server : "(null)");
    CHECK(streq(h.web_server, c->host), what);

    snprintf(what, sizeof(what), "%s -> web_port %d (got %d)",
             c->url, c->port, h.web_port);
    CHECK(c->port == h.web_port, what);

    snprintf(what, sizeof(what), "%s -> target_page '%s' (got '%s')",
             c->url, c->page ? c->page : "(null)",
             h.target_page ? h.target_page : "(null)");
    CHECK(streq(h.target_page, c->page), what);

    snprintf(what, sizeof(what), "%s -> username '%s' (got '%s')",
             c->url, c->user ? c->user : "(null)",
             h.username ? h.username : "(null)");
    CHECK(streq(h.username, c->user), what);

    snprintf(what, sizeof(what), "%s -> password '%s' (got '%s')",
             c->url, c->pass ? c->pass : "(null)",
             h.password ? h.password : "(null)");
    CHECK(streq(h.password, c->pass), what);

    /* `url` keeps the string the request sent, credentials and all. That is
     * intentional and is asserted rather than left implicit: the member is the
     * record of what was asked for, and the request builder reads web_server
     * and target_page, not this. */
    snprintf(what, sizeof(what), "%s -> url preserved", c->url);
    CHECK(streq(h.url, c->url), what);

    EZPI_free_http_mbedtls_struct(&h);
}

/* 1 and 2. The table, in both halves. */
static void test_ordinary_urls(void)
{
    for (size_t i = 0; i < sizeof(ordinary) / sizeof(ordinary[0]); i++)
    {
        check_case(&ordinary[i]);
    }
}

static void test_credentialed_urls(void)
{
    for (size_t i = 0; i < sizeof(credentialed) / sizeof(credentialed[0]); i++)
    {
        check_case(&credentialed[i]);
    }
}

/* 3. The defect in the form it actually does damage: the `Host:` header.
 *
 * EZPI_parse_http_url does not only store the host, it appends it as a header,
 * and that header goes out on the wire. Asserting on web_server alone would
 * leave open the possibility of a fix that cleans up the member and still
 * builds the header from the unsplit string. */
static void test_host_header_carries_no_credentials(void)
{
    s_ezlopi_core_http_mbedtls_t h;
    run("http://alice:s3cr3t@example.com:8080/x", &h);

    CHECK(NULL != h.header, "a header was built");
    CHECK(NULL != strstr(h.header, "Host: example.com\r\n"),
          "Host: header names the bare host");
    CHECK(NULL == strstr(h.header, "s3cr3t"),
          "Host: header does not carry the password");
    CHECK(NULL == strstr(h.header, "alice"),
          "Host: header does not carry the username");
    CHECK(NULL == strstr(h.header, "@"),
          "Host: header has no userinfo separator left in it");

    EZPI_free_http_mbedtls_struct(&h);
}

/* 4. The same claim about what the subject asked the allocator for, which is
 * the surface that does not run through the stub's own logic. */
static void test_nothing_secret_is_handed_to_the_allocator_as_a_host(void)
{
    s_ezlopi_core_http_mbedtls_t h;
    run("http://alice:s3cr3t@example.com:8080/x", &h);

    /* The recorded strings must contain the password exactly where it belongs
     * -- as the `password` member and inside the preserved `url` -- and nowhere
     * else. Walking the log rather than naming an index keeps this from
     * depending on the order the subject happens to fill the struct in. */
    int secret_as_password = 0, secret_elsewhere = 0;
    for (int i = 0; i < g_stub_malloc_log_n; i++)
    {
        const char *s = g_stub_malloc_log[i].src;
        if (NULL == strstr(s, "s3cr3t"))
        {
            continue;
        }
        if (g_stub_malloc_log[i].dest == &h.password ||
            g_stub_malloc_log[i].dest == &h.url)
        {
            secret_as_password++;
        }
        else
        {
            secret_elsewhere++;
            printf("      secret reached an unexpected member: '%s'\n", s);
        }
    }
    CHECK(2 == secret_as_password,
          "the password was stored as the password, and kept in url");
    CHECK(0 == secret_elsewhere,
          "the password reached no other member");

    EZPI_free_http_mbedtls_struct(&h);
}

/* 5. An unparseable URL must leave the struct empty rather than half-filled.
 *
 * This is the guard that EZPI_core_http_mem_malloc's corrected failure return
 * made meaningful, and it is checked here because a userinfo split that runs
 * before the ladder could reintroduce the half-filled state. */
static void test_unparseable_url_sets_nothing(void)
{
    s_ezlopi_core_http_mbedtls_t h;
    run("not a url at all", &h);

    CHECK(NULL == h.web_server, "no web_server from an unparseable URL");
    CHECK(NULL == h.target_page, "no target_page from an unparseable URL");
    CHECK(NULL == h.header, "no Host: header from an unparseable URL");
    CHECK(NULL == h.username, "no username from an unparseable URL");

    /* Zero, not 80. The 80 in EZPI_parse_http_url is a local initialiser, and
     * the assignment to web_port is inside the `if (succ_parsing)` branch, so a
     * URL that does not parse leaves the member at whatever the caller's
     * memset left -- which is 0.
     *
     * Asserted as measured, after this check was first written expecting 80 and
     * failed. It is not a new finding: ezlopi_core_http.c:192 already names "0
     * when the URL did not parse" while explaining why
     * EZPI_core_http_mbedtls_req guards on a NULL web_server, which is what
     * stops this struct from reaching the resolver at all. Pinning it here
     * keeps the two statements from drifting apart. */
    CHECK(0 == h.web_port, "web_port left at the caller's zero, not the local default of 80");

    EZPI_free_http_mbedtls_struct(&h);
}

/* 6. The field-width property, run rather than reasoned about.
 *
 * check_scanf_field_width.py exists because arm nine used to spell its
 * conversions `%[^:]` and `%s`, writing a request-supplied host straight into
 * host[100] and a request-supplied page into page[200]. That check is static
 * and covers the whole class; this is the one input that proves the dynamic
 * half, and it is the first time it has been run against the real translation
 * unit rather than a transcription.
 *
 * Adjudicated by ASan. Pre-fix this aborts; post-fix the over-long host fails
 * the ':' that follows the bounded conversion, the arm returns 1 instead of 3,
 * and the URL is reported unparseable -- which is the right answer, because
 * truncating a host silently would send the request somewhere else. */
static void test_overlong_host_does_not_overflow(void)
{
    char url[1024];
    char host[600];

    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    snprintf(url, sizeof(url), "ftp://%s:80/x", host);
    s_ezlopi_core_http_mbedtls_t h;
    run(url, &h);
    CHECK(NULL == h.web_server, "599-character host is rejected, not truncated");
    EZPI_free_http_mbedtls_struct(&h);

    /* The page is the other half of the same arm, and it does not behave like
     * the host. A host longer than its width fails the ':' that follows the
     * conversion, so the arm returns the wrong count and the URL is rejected.
     * `%199s` is arm nine's *last* conversion, so there is no following literal
     * to fail: it takes 199 characters, succeeds, the arm returns 3, and the
     * remaining 400 characters are discarded in silence.
     *
     * Written expecting a rejection here, and corrected to what running it
     * showed. The overflow is fixed either way -- that is what ASan is
     * adjudicating, and it is why this input was chosen -- but "fixed" means
     * truncated rather than refused, and a request built from a truncated path
     * is sent to a different resource than the action named. Recorded in the
     * audit's Known gaps beside arm nine's leading-slash defect, which is the
     * same conversion and wants the same change. */
    snprintf(url, sizeof(url), "ftp://h:80/%s", host);
    run(url, &h);
    CHECK(streq(h.web_server, "h"), "599-character page: the host still parses");
    CHECK(NULL != h.target_page, "and a page is stored");
    CHECK(199 == strlen(h.target_page),
          "599-character page is silently truncated to arm nine's width of 199");
    EZPI_free_http_mbedtls_struct(&h);

    /* And the same length behind an http:// prefix, which arm seven bounds at
     * 99 -- so this one parses, and parses to a truncated host. That is
     * pre-existing behaviour of the arms that were already width-bounded, and
     * is asserted so that it is a recorded fact rather than a surprise: the
     * arm's %99 conversion stops at 99 characters and the remainder is
     * discarded with no error. */
    snprintf(url, sizeof(url), "http://%s", host);
    run(url, &h);
    CHECK(NULL != h.web_server, "arm seven accepts an over-long host");
    CHECK(99 == strlen(h.web_server), "and bounds it at 99 characters");
    EZPI_free_http_mbedtls_struct(&h);
}

/* 7. An IPv6 literal, which the ladder has never handled and must not be made
 * worse by the port split.
 *
 * `http://[::1]:8080/x` reaches arm five, so `host` becomes "[::1]:8080". A
 * port split that took the text after the last colon would be right here by
 * accident and wrong for "[::1]" with no port, where the last colon is inside
 * the address. Asserted as it behaves rather than as it ought to: getting IPv6
 * right is a separate change, and the claim being pinned is only that this one
 * does not corrupt the address. */
static void test_ipv6_literal_is_not_mangled(void)
{
    s_ezlopi_core_http_mbedtls_t h;

    run("http://[::1]:8080/x", &h);
    CHECK(streq(h.web_server, "[::1]"), "bracketed IPv6 host survives the split");
    CHECK(8080 == h.web_port, "and its port is read");
    EZPI_free_http_mbedtls_struct(&h);

    run("http://[::1]/x", &h);
    CHECK(streq(h.web_server, "[::1]"), "IPv6 host with no port is left whole");
    CHECK(80 == h.web_port, "and keeps the scheme default");
    EZPI_free_http_mbedtls_struct(&h);
}

/* 8. Which credential wins when the action supplies both.
 *
 * EZPI_core_scene_then_send_http_request walks the action's fields in whatever
 * order the request listed them and dispatches each by name, so `credential`
 * may be parsed before `url` or after it. The outcome must not depend on that.
 * The rule is that an explicit `credential` wins, implemented by having the
 * URL's userinfo fill username and password only while they are still NULL.
 *
 * Added after a mutation run: deleting that NULL test left every one of the
 * suite's then-135 checks passing, because nothing drove the two fields
 * together. The precedence is a decision, and an untested decision is a
 * coincidence. */
static l_fields_v2_t *credential_field(const char *user, const char *pass)
{
    l_fields_v2_t *f = malloc(sizeof(l_fields_v2_t));
    assert(f);
    memset(f, 0, sizeof(l_fields_v2_t));
    snprintf(f->name, sizeof(f->name), "%s", "credential");
    f->value_type = EZLOPI_VALUE_TYPE_CREDENTIAL;
    /* VALUE_TYPE_CJSON and not a VALUE_TYPE_CREDENTIAL, which does not exist:
     * e_value_type_t says how u_value is to be read, and a credential is read
     * as cj_value. The two enums are not parallel -- e_scene_value_type_v2_t
     * names the cloud's declared type, e_value_type_t the union member. */
    f->field_value.e_type = VALUE_TYPE_CJSON;

    cJSON *cj = cJSON_CreateObject(__FUNCTION__);
    assert(cj);
    assert(cJSON_AddStringToObject(__FUNCTION__, cj, "user", user));
    assert(cJSON_AddStringToObject(__FUNCTION__, cj, "password", pass));
    f->field_value.u_value.cj_value = cj;
    return f;
}

static void free_credential_field(l_fields_v2_t *f)
{
    cJSON_Delete(__FUNCTION__, f->field_value.u_value.cj_value);
    free(f);
}

static void test_explicit_credential_beats_the_url_userinfo(void)
{
    const char *url = "http://urluser:urlpass@example.com:8080/x";
    s_ezlopi_core_http_mbedtls_t h;

    /* credential first, then url -- the order where the URL's userinfo would
     * have to overwrite something to win. */
    memset(&h, 0, sizeof(h));
    stub_httpurl_reset();
    l_fields_v2_t *cf = credential_field("fielduser", "fieldpass");
    l_fields_v2_t *uf = url_field(url);
    EZPI_parse_http_creds(&h, cf);
    EZPI_parse_http_url(&h, uf);
    CHECK(streq(h.username, "fielduser"), "credential-then-url: username is the field's");
    CHECK(streq(h.password, "fieldpass"), "credential-then-url: password is the field's");
    CHECK(streq(h.web_server, "example.com"), "credential-then-url: host still split");
    CHECK(8080 == h.web_port, "credential-then-url: port still recovered");
    free_url_field(uf);
    free_credential_field(cf);
    EZPI_free_http_mbedtls_struct(&h);

    /* url first, then credential -- where EZPI_parse_http_creds overwrites,
     * which it has always done unconditionally. Same answer. */
    memset(&h, 0, sizeof(h));
    stub_httpurl_reset();
    cf = credential_field("fielduser", "fieldpass");
    uf = url_field(url);
    EZPI_parse_http_url(&h, uf);
    EZPI_parse_http_creds(&h, cf);
    CHECK(streq(h.username, "fielduser"), "url-then-credential: username is the field's");
    CHECK(streq(h.password, "fieldpass"), "url-then-credential: password is the field's");
    free_url_field(uf);
    free_credential_field(cf);
    EZPI_free_http_mbedtls_struct(&h);
}

/* 9. A guard on the stubs rather than on the subject: the two IDF enums in
 * stubs-httpurl/esp_http_client.h are copies, and EZPI_parse_http_request_type
 * assigns four of their enumerators. A copy with the wrong values would make
 * every method assignment in this translation unit wrong while every test above
 * still passed, because none of them looks at `method`. */
static void test_stubbed_http_method_enum_matches_the_idf(void)
{
    CHECK(0 == (int)HTTP_METHOD_GET, "HTTP_METHOD_GET is 0");
    CHECK(1 == (int)HTTP_METHOD_POST, "HTTP_METHOD_POST is 1");
    CHECK(2 == (int)HTTP_METHOD_PUT, "HTTP_METHOD_PUT is 2");
    CHECK(4 == (int)HTTP_METHOD_DELETE, "HTTP_METHOD_DELETE is 4");
    CHECK(200 == (int)HttpStatus_Ok, "HttpStatus_Ok is 200");
}

int main(void)
{
    test_stubbed_http_method_enum_matches_the_idf();
    test_ordinary_urls();
    test_credentialed_urls();
    test_host_header_carries_no_credentials();
    test_nothing_secret_is_handed_to_the_allocator_as_a_host();
    test_unparseable_url_sets_nothing();
    test_overlong_host_does_not_overflow();
    test_ipv6_literal_is_not_mangled();
    test_explicit_credential_beats_the_url_userinfo();

    printf("test_httpurl: %d checks passed\n", checks);
    return 0;
}
