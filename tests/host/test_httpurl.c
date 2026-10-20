/* Host tests for EZPI_parse_http_url() -- which turns a sendHttpRequest
 * action's `url` field into a host, a port and a page.
 *
 * WHY THIS FILE EXISTS
 *
 * It was written against the nine-arm sscanf ladder that used to do that job:
 * eight arms spelling out "http"/"https" against present/absent port and
 * present/absent path, and a ninth fallback for any other scheme. The two
 * defects it was written for were a URL carrying credentials -- which reached
 * an arm that scanned the whole authority, so `host` became the string
 * "user:pass@example.com" and the password went out as a `Host:` header -- and
 * the same URL with an explicit port, which that arm never looked for.
 *
 * The first draft of the audit entry for those got both halves wrong: it said
 * such a URL "takes the first arm" and parsed into a "nonsense host". It took
 * arm five, and the host was not nonsense -- it was the credentials. That draft
 * was written from a reading of the arms, and so was its correction. This file
 * exists so that claims about this function are settled by running it.
 *
 * Which is what then happened. Driving the real translation unit from here
 * found twenty-three more defects in the ladder, all of them silent, and the
 * ladder is now gone: one pass finds the authority and the path by position,
 * the scheme is matched case-insensitively against http and https and every
 * other scheme is refused, and the port is parsed in exactly one place -- the
 * authority split, which was already in the file and already correct, and which
 * four of the nine arms were going around.
 *
 * "Twenty-three" is a count of an enumeration, not an impression: the CHANGELOG
 * entry tabulates them, each row tied to a check below that is named in the
 * failing output of the control run. Three earlier drafts of this header had it
 * wrong -- twelve, the count when it was first written and never revised as the
 * file grew; twenty-two, a hand count taken before the table existed; and
 * twenty-one, the table's own count before every row was checked against the
 * control run's list of failing checks. That last pass is what found two
 * defects with no row at all: a leading zero in the port (":08080" was port 0)
 * and an unclosed bracket ("http://[::1/x" was host "[:" on port 1). Both were
 * already pinned by checks in this file. Neither was in any prose. The table is
 * the enumeration; this number is recounted from it rather than carried
 * forward.
 *
 * Many of the checks below assert what the ladder actually did rather than what
 * it should have done, each number read off a measurement rather than off the
 * new implementation. They are marked WAS at the site. The count is not quoted
 * from a reading of them, because that is the kind of number that goes stale:
 * it is measured by swapping the subject for `git show HEAD:` of itself and
 * running this file unchanged. 150 of the 478 checks fail that way, against 0
 * on the fixed subject -- so the suite distinguishes the two in both
 * directions, which is the only thing that makes "0 failures" mean anything.
 * probe/hurestore.sh is that run -- an uncommitted throwaway in the audit
 * working directory beside this checkout, not a path in this tree.
 *
 * The last ten checks added moved that total from 468 to 478 and left the
 * failure count at 150, which is itself their answer: a leading space and a
 * high byte in the host were both handled by the ladder the same way the
 * rewrite handles them -- the first refused by accident, the second accepted.
 * They pin behaviour the rewrite had to preserve, and add nothing to the count
 * of what it fixed. Both figures are re-measured whenever a check is added,
 * because a new check that fails on neither subject proves nothing about the
 * change and the failure count is the only place that shows up.
 *
 * Counting rather than exiting on the first failure is what makes that
 * possible, and is why this file's CHECK was changed to do so.
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
 * The over-long-input case is adjudicated by AddressSanitizer rather than by an
 * assert, which is why the suite's -fsanitize flags matter here: before the
 * widths were added to arm nine, a long host overflowed host[100] by writing
 * past its end, and a write into unused stack passes any assert.
 *
 * One defect is adjudicated by neither, and it is worth naming because it is
 * the limit of what this file can do. ":99999999999999" under arm one's %i was
 * an out-of-range conversion -- undefined behaviour -- and it produced port
 * 276447231 while exiting 0 with no sanitizer report, because the conversion
 * happened inside libc and ASan instruments only what it compiles. The same
 * blind spot was measured for strtoul in test_suntime.c. All this suite can
 * assert is the wrong number that came out; it cannot assert that the
 * replacement is free of the undefined behaviour, only that no conversion
 * function is called any more.
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

/* Counting rather than exiting on the first failure, which this harness used to
 * do. Two reasons, and the second is the one that matters:
 *
 *   - "N check(s), M failure(s)" is the line the suite's tally reads. This file
 *     printed "test_httpurl: N checks passed", which matches nothing, so its
 *     checks were never in the aggregate and -- worse -- a failure here
 *     contributed 0 to the aggregate failure count. The verdict line is still
 *     sound, because make's `./$$t || fail=1` reads the exit status, but the
 *     number beside it was not.
 *   - exit-on-first-failure makes a restore run useless. Reinstating the
 *     shipped ladder and seeing one FAIL tells you nothing about the other
 *     twenty; the whole point of asserting a defect's measured answer is that
 *     the restore run enumerates them. */
static int checks = 0;
static int fails = 0;
#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(cond))                                              \
        {                                                         \
            fails++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
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

/* And a substring test that tolerates a NULL haystack, for the same reason:
 * "the subject built no header at all" is one of the answers under test, and
 * now that CHECK counts instead of exiting, a run that gets that answer goes
 * on to the next line rather than stopping. strstr(NULL, x) would make a
 * restore run crash instead of enumerating. */
static int contains(const char *haystack, const char *needle)
{
    return (NULL != haystack) && (NULL != strstr(haystack, needle));
}

/* Likewise strlen: asserting a length is asserting that there is a string. */
static size_t len_or_max(const char *s)
{
    return (NULL != s) ? strlen(s) : (size_t)-1;
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
 * of the function under test. These are here to pin what already worked: a
 * change that fixes credentials or ports by breaking ordinary URLs is the
 * obvious way to make this worse, and these are what would catch it. */
static const s_url_case_t ordinary[] = {
    {"http://www.testhttp.com:8090/foo_page", "www.testhttp.com", 8090, "foo_page", NULL, NULL},
    {"https://www.testhttp.com:8090/foo_page", "www.testhttp.com", 8090, "foo_page", NULL, NULL},
    {"http://www.testhttp.com/foo_page", "www.testhttp.com", 80, "foo_page", NULL, NULL},
    {"https://www.testhttp.com/foo_page", "www.testhttp.com", 443, "foo_page", NULL, NULL},
    {"http://www.testhttp.com:8090", "www.testhttp.com", 8090, "", NULL, NULL},
    {"https://www.testhttp.com:8090", "www.testhttp.com", 8090, "", NULL, NULL},
    {"http://www.testhttp.com", "www.testhttp.com", 80, "", NULL, NULL},
    {"https://www.testhttp.com", "www.testhttp.com", 443, "", NULL, NULL},
    /* A scheme is case-insensitive -- RFC 3986 section 3.1 says so in as many
     * words -- and the ladder's eight literals were not.
     *
     * WAS: the first of these missed all eight arms and landed on the ninth,
     * which got the host and the port right and gave the page a stray leading
     * slash -- "GET //foo_page". The other three have no explicit port, so
     * there was nothing for the ninth arm's %d to take either: they matched
     * nothing at all and were reported unparseable. An uppercased scheme turned
     * a working scene into a dead one, and that is three of the four rows.
     *
     * Both halves of the case fold are exercised, not just the first character,
     * because lowercasing only url[0] would pass the first of these. */
    {"HTTP://www.testhttp.com:8090/foo_page", "www.testhttp.com", 8090, "foo_page", NULL, NULL},
    {"HTTP://www.testhttp.com/foo_page", "www.testhttp.com", 80, "foo_page", NULL, NULL},
    {"HtTpS://www.testhttp.com/foo_page", "www.testhttp.com", 443, "foo_page", NULL, NULL},
    {"HTTPS://www.testhttp.com", "www.testhttp.com", 443, "", NULL, NULL},
    /* A query or a fragment with no path ends the authority too. Before this
     * change these reached the catch-all arms, which scan %99[^\n], so the
     * query went into the host.
     *
     * WAS: web_server "example.com?q=a", emitted verbatim as
     * "Host: example.com?q=a", with the page left empty -- so the query was
     * both in the wrong place and absent from the right one. The header half is
     * asserted again in test 12, because that is the surface that did the
     * damage.
     *
     * The explicit-port row is worth having separately: that one reached arm
     * three, which asks for no path at all, so the host and the port came out
     * right and the query was simply discarded. A scene requesting
     * ".../thing?api_key=..." on a non-default port sent the request with no
     * query at all. */
    {"http://example.com?q=a", "example.com", 80, "?q=a", NULL, NULL},
    {"http://example.com#frag", "example.com", 80, "#frag", NULL, NULL},
    {"http://example.com:8080?q=a", "example.com", 8080, "?q=a", NULL, NULL},
    /* Already right: arm six consumed the '/' and took the rest with
     * %199[^\n]. Here to pin that the new code does not break it. */
    {"https://example.com/p?q=a#f", "example.com", 443, "p?q=a#f", NULL, NULL},
};

/* Schemes this firmware does not speak, which are now refused rather than
 * parsed. That is a behaviour decision and it is the one the audit's Known gaps
 * entry left open, so it is worth stating why it went this way: there is
 * exactly one transport on this path. EZPI_core_http_mbedtls_req always calls
 * __ezlopi_http_req_via_mbedTLS, which always performs a TLS handshake, and
 * `web_port_num` only builds the connect string. A scheme therefore never
 * selected a transport here -- it only ever selected a default port -- so the
 * ladder's ninth arm was sending a TLS handshake to whatever port an ftp:// URL
 * named, and parsing it more carefully would not have helped.
 *
 * WAS: every one of the first four parsed, and sent a TLS handshake to the port
 * the URL named. "ftp://example.com:21/x" gave host "example.com", port 21 and
 * page "/x"; "file://etc/passwd:1/x" gave host "etc/passwd" and port 1. */
static const char *const rejected_schemes[] = {
    "ftp://example.com:21/x",
    "file://etc/passwd:1/x",
    "ws://example.com:8080/x",
    "gopher://example.com:70/x",
    /* The last four never parsed and must still not -- arm nine needed both a
     * port and a path, so a non-http scheme without an explicit port fell off
     * the end of the ladder. They are here because the scheme match is a
     * character loop now, and an off-by-one in it is exactly the kind of
     * mistake that would start accepting them: "http:/" has one slash,
     * "httpx://" shares a prefix with neither scheme, and the last has no
     * scheme at all. */
    "ftp://example.com/x",
    "http:/example.com/x",
    "httpx://example.com/x",
    "//example.com/x",
};

/* The credentialed shapes -- the defect this file was opened for. Every one of
 * these used to reach an arm that scanned the whole authority into `host`,
 * which is how the credentials got into the Host: header. */
static const s_url_case_t credentialed[] = {
    {"http://user:pass@example.com/x", "example.com", 80, "x", "user", "pass"},
    {"http://user:pass@example.com", "example.com", 80, "", "user", "pass"},
    {"https://user:pass@example.com/x", "example.com", 443, "x", "user", "pass"},
    {"https://user:pass@example.com", "example.com", 443, "", "user", "pass"},
    /* The swallowed port: arm five took this and never looked for one. */
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

/* A URL that must leave the struct untouched. Four members and the port,
 * because a parser that refuses halfway through is worse than one that refuses
 * at the start: EZPI_core_http_mbedtls_req guards on web_server, so a NULL
 * there stops the request, but a header appended before the refusal would
 * still be in the buffer the next field appends to. */
static void check_rejected(const char *url, const char *why)
{
    s_ezlopi_core_http_mbedtls_t h;
    char what[400];

    run(url, &h);

    snprintf(what, sizeof(what), "%s: %s -> no web_server (got '%s')",
             why, url, h.web_server ? h.web_server : "(null)");
    CHECK(NULL == h.web_server, what);

    snprintf(what, sizeof(what), "%s: %s -> no target_page (got '%s')",
             why, url, h.target_page ? h.target_page : "(null)");
    CHECK(NULL == h.target_page, what);

    snprintf(what, sizeof(what), "%s: %s -> no Host: header (got '%s')",
             why, url, h.header ? h.header : "(null)");
    CHECK(NULL == h.header, what);

    snprintf(what, sizeof(what), "%s: %s -> web_port left at zero (got %d)",
             why, url, h.web_port);
    CHECK(0 == h.web_port, what);

    /* The userinfo split runs before the refusal is decided, so this is not
     * implied by the above: it is the assertion that the refusal happens before
     * anything is stored, not after. */
    snprintf(what, sizeof(what), "%s: %s -> no username (got '%s')",
             why, url, h.username ? h.username : "(null)");
    CHECK(NULL == h.username, what);

    snprintf(what, sizeof(what), "%s: %s -> no password (got '%s')",
             why, url, h.password ? h.password : "(null)");
    CHECK(NULL == h.password, what);

    /* `url` is still stored. EZPI_parse_http_url copies it before parsing
     * anything, which is deliberate -- the member is the record of what the
     * action asked for -- and a refusal must not change that. */
    snprintf(what, sizeof(what), "%s: %s -> url still preserved", why, url);
    CHECK(streq(h.url, url), what);

    EZPI_free_http_mbedtls_struct(&h);
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
    CHECK(contains(h.header, "Host: example.com\r\n"),
          "Host: header names the bare host");
    CHECK(!contains(h.header, "s3cr3t"),
          "Host: header does not carry the password");
    CHECK(!contains(h.header, "alice"),
          "Host: header does not carry the username");
    CHECK(!contains(h.header, "@"),
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

/* 6. The over-long-input property, run rather than reasoned about.
 *
 * check_scanf_field_width.py exists because arm nine used to spell its
 * conversions `%[^:]` and `%s`, writing a request-supplied host straight into
 * host[100] and a request-supplied page into page[200]. That check is static
 * and covers the whole class; this is the one input that proves the dynamic
 * half. There is no sscanf in this function any more, so what the check guards
 * is the rest of the tree and the possibility of one coming back.
 *
 * Still adjudicated by ASan, because the authority is still copied into a
 * fixed buffer -- a memcpy of an unchecked length is the same defect with a
 * different spelling, and this is the input that would find it. */
static void test_overlong_input_does_not_overflow(void)
{
    char url[1024];
    char host[600];

    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    s_ezlopi_core_http_mbedtls_t h;

    /* WAS: arms seven and eight bounded this at %99 and accepted it, so a
     * 599-character host became a 99-character one and the request went to a
     * different machine with nothing said. Refused now -- that is the whole
     * argument for refusing rather than truncating a host. */
    snprintf(url, sizeof(url), "http://%s", host);
    check_rejected(url, "599-character host, WAS truncated to 99 and accepted");

    /* The two boundary rows, which are what makes the refusal a width rather
     * than a guess. authority[256] holds 255 characters and a terminator. */
    snprintf(url, sizeof(url), "http://%.255s/x", host);
    run(url, &h);
    CHECK(NULL != h.web_server, "a 255-character authority fits");
    CHECK(255 == len_or_max(h.web_server), "and is stored whole, not shortened");
    EZPI_free_http_mbedtls_struct(&h);

    snprintf(url, sizeof(url), "http://%.256s/x", host);
    check_rejected(url, "a 256-character authority is one over and is refused");

    /* The page is the other half, and it is no longer bounded at all: it is a
     * pointer into the caller's URL rather than a copy into page[200], so
     * there is no width to truncate it at.
     *
     * WAS: `%199[^\n]` was arm five's last conversion, so there was no
     * following literal to fail -- it took 199 characters, the arm returned 2,
     * and the remaining 400 were discarded in silence. A request built from a
     * truncated path is sent to a different resource than the action named.
     *
     * Arm nine spelled the same width `%199s` and the audit's Known gaps entry
     * recorded it there, which was the narrower reading: the truncation was
     * never specific to the non-http arm. This input goes through arm five. */
    snprintf(url, sizeof(url), "http://h/%s", host);
    run(url, &h);
    CHECK(streq(h.web_server, "h"), "599-character page: the host still parses");
    CHECK(NULL != h.target_page, "and a page is stored");
    CHECK(599 == len_or_max(h.target_page),
          "599-character page is stored whole, WAS cut to arm nine's width of 199");

    /* Unbounding the page is only safe because the request buffer is sized
     * from the URL. __ezlopi_http_generate_request computes request_len from
     * url_maxlen among other things, and the page is a substring of the URL, so
     * a page that grew cannot under-size the buffer it is written into.
     *
     * Asserted as the inequality between the two maxlen members rather than as
     * a number, because that is the claim the sizing rests on and it is the one
     * that survives a change to either buffer. */
    CHECK(h.url_maxlen > h.targe_page_maxlen,
          "and url_maxlen still exceeds targe_page_maxlen, which is what sizes the request");
    EZPI_free_http_mbedtls_struct(&h);
}

/* 7. An IPv6 literal, which must not be made worse by the port split.
 *
 * A port split that took the text after the last colon would be right for
 * "[::1]:8080" by accident and wrong for "[::1]" with no port, where the last
 * colon is inside the address. The stray-colon refusal added for the port
 * defects is the other half of the same question: every colon in a bracketed
 * literal sits before the ']', which is exactly what distinguishes it from the
 * ":0x50" the refusal is for. */
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

    run("https://[2001:db8::1]/x", &h);
    CHECK(streq(h.web_server, "[2001:db8::1]"), "a longer literal is left whole too");
    CHECK(443 == h.web_port, "and takes the https default");
    EZPI_free_http_mbedtls_struct(&h);

    /* A colon after the ']' that is not a port is still a refusal, so the
     * bracket exemption is bounded rather than a blanket one. */
    check_rejected("http://[::1]:0x50/x", "bracketed host, malformed port");
    /* And an unclosed bracket has no ']' to be before. */
    check_rejected("http://[::1/x", "unclosed bracket");
}

/* 10. The schemes this firmware cannot serve, refused rather than parsed. */
static void test_unspeakable_schemes_are_refused(void)
{
    for (size_t i = 0; i < sizeof(rejected_schemes) / sizeof(rejected_schemes[0]); i++)
    {
        check_rejected(rejected_schemes[i], "scheme not implemented");
    }
}

/* 11. The port, which is the largest group of defects the ladder carried and
 * the reason it is gone. Arms one to four each scanned their own with `%i`,
 * which is not a decimal conversion, and nothing after it was checked.
 *
 * All of it is now one implementation -- the authority split, which was already
 * in the file, already decimal-only and already range-checked, and which four
 * arms were going around. */
static void test_port_is_decimal_and_range_checked(void)
{
    s_ezlopi_core_http_mbedtls_t h;

    /* WAS: ":0x50" was port 80 and ":010" was port 8. The author wrote base-ten
     * digits in a format that accepts hex and octal. */
    check_rejected("http://h:0x50/x", "hex port, WAS 80");

    run("http://h:010/x", &h);
    CHECK(10 == h.web_port, "':010' is port 10 in decimal, WAS 8 in octal");
    CHECK(streq(h.web_server, "h"), "and the host is unaffected");
    EZPI_free_http_mbedtls_struct(&h);

    /* WAS: port 0. %i read the leading '0' as an octal prefix, stopped at the
     * '8', failed the '/' that follows, and dropped through to arm three --
     * which asks for no path at all and kept the 0. A leading zero is not a
     * sensible thing to write, but silently rewriting 8080 as 0 is not a
     * sensible thing to do about it. */
    run("http://h:08080/x", &h);
    CHECK(8080 == h.web_port, "':08080' is port 8080, WAS 0");
    EZPI_free_http_mbedtls_struct(&h);

    /* WAS: port 8090, junk discarded. Arm three's format ends "%i[^\n]" and
     * that trailing [^\n] is five literal characters there, not a conversion,
     * so nothing ever checked what followed the digits. */
    check_rejected("http://h:8090junk/x", "trailing junk after the port, WAS accepted as 8090");

    /* WAS: all three accepted as written and handed to mbedtls as a string. */
    check_rejected("http://h:99999/x", "port above 65535, WAS accepted");
    check_rejected("http://h:65536/x", "port one above the maximum, WAS accepted");
    check_rejected("http://h:0/x", "port zero, WAS accepted");

    /* The boundary on the other side, so the range check is a range and not a
     * rejection of everything large. */
    run("http://h:65535/x", &h);
    CHECK(65535 == h.web_port, "65535 is the largest port and is accepted");
    EZPI_free_http_mbedtls_struct(&h);
    run("http://h:1/x", &h);
    CHECK(1 == h.web_port, "and 1 is the smallest");
    EZPI_free_http_mbedtls_struct(&h);

    /* WAS: port -1, passed to mbedtls as the string "-1". %i accepts a sign. */
    check_rejected("http://h:-1/x", "negative port, WAS -1");

    /* WAS: port 276447231. An out-of-range %i is undefined behaviour and it
     * happened inside libc, so neither ASan nor UBSan reported it -- the same
     * blind spot measured for strtoul in test_suntime.c. The replacement stops
     * accumulating the moment the value passes 65535, so there is no overflow
     * to be undefined about; the digit it stopped at then fails the
     * end-of-string test and the URL is refused. */
    check_rejected("http://h:99999999999999/x", "14-digit port, WAS 276447231");
    check_rejected("http://h:4294967296/x", "a port one past 32 bits");

    /* WAS: web_server "example.com:" -- a trailing colon with no digits after
     * it went to the resolver and into the Host: header. Recorded in the
     * audit's Known gaps and closed by the same check as the rest. */
    check_rejected("http://example.com:/x", "empty port, WAS host 'example.com:'");

    /* Two colons. The valid port is split off the end and the one left behind
     * has no bracket in front of it, so this is refused by the same test. */
    check_rejected("http://example.com:8080:9090/x", "two ports");

    /* And the port on a credentialed URL, because the userinfo colon must not
     * be mistaken for this one in either direction. */
    run("http://user:pass@h:8080/x", &h);
    CHECK(8080 == h.web_port, "the port after a userinfo is still the port");
    CHECK(streq(h.password, "pass"), "and the userinfo colon is still the userinfo's");
    EZPI_free_http_mbedtls_struct(&h);
    check_rejected("http://user:pass@h:0x50/x", "a malformed port behind a userinfo is still refused");
}

/* 12. A query or a fragment with no path. Recorded in the audit's Known gaps
 * and closed here almost for free, because ending the authority at '?' and '#'
 * is one more character in the strpbrk set that finds it.
 *
 * WAS: the catch-all arms scan %99[^\n], so "http://example.com?q=a" left the
 * query inside the host and emitted "Host: example.com?q=a" -- a header no
 * server will route, for a URL a browser handles without comment. The
 * surviving rows in the ordinary[] table assert the new host and page; this
 * asserts the header, which is the surface that did the damage. */
static void test_query_with_no_path_does_not_reach_the_host_header(void)
{
    s_ezlopi_core_http_mbedtls_t h;

    run("http://example.com?q=a", &h);
    CHECK(NULL != h.header, "a header was built");
    CHECK(contains(h.header, "Host: example.com\r\n"),
          "Host: names the bare host, WAS 'example.com?q=a'");
    CHECK(!contains(h.header, "q=a"), "and the query is not in it");
    EZPI_free_http_mbedtls_struct(&h);

    /* The query survives as the page, which is where the request line needs
     * it: __ezlopi_http_generate_request emits "GET /%s", so a page of "?q=a"
     * produces "GET /?q=a" and a page of "p?q=a" produces "GET /p?q=a". The
     * '?' is kept because, unlike the '/', it has no separator to drop. */
    run("http://example.com?q=a", &h);
    CHECK(streq(h.target_page, "?q=a"), "the query becomes the page, '?' and all");
    EZPI_free_http_mbedtls_struct(&h);

    /* A query that carries an API key is the reason the page trace is behind
     * CONFIG_EZPI_HTTP_LOG_PAYLOADS. That gate is not observable from here --
     * TRACE_* is a no-op in this suite -- so what is asserted instead is that
     * the key does not reach the one place a reader could not see it coming
     * from: the header. */
    run("https://example.com/v1/thing?api_key=s3cr3t", &h);
    CHECK(NULL != h.header, "a header was built for the api-key URL");
    CHECK(!contains(h.header, "s3cr3t"),
          "an api_key in the query reaches no header");
    CHECK(streq(h.target_page, "v1/thing?api_key=s3cr3t"),
          "but does reach the page, which is where the request line needs it");
    EZPI_free_http_mbedtls_struct(&h);
}

/* 13. The page. Two of the ladder's defects were here, and this is the one
 * place in this file where the fix cannot be demonstrated against the new code,
 * which is worth saying plainly rather than implying otherwise with a row that
 * looks like the others.
 *
 * Both defects were arm nine's alone, and arm nine's conversions were
 * "%d%199s". The %d stopped at the '/', so %199s took the separator along with
 * the rest and the request line read "GET //x"; and %199s stops at whitespace,
 * where the other arms spell `%199[^\n]`, so everything from the first space
 * onward was dropped. Arm nine was reached only by a scheme that is neither
 * http nor https -- and every such scheme is now refused outright, by
 * test_unspeakable_schemes_are_refused above.
 *
 * So there is no input that both reaches the new code and exhibits either
 * defect. They are fixed by the removal of the arm, not by a replacement that
 * handles them, and nothing below asserts a shipped answer. What is asserted is
 * the rule those two were exceptions to, now that it is the only rule: the
 * separator belongs to the request line, and nothing truncates the page. */
static void test_page_keeps_no_separator_and_stops_at_nothing(void)
{
    s_ezlopi_core_http_mbedtls_t h;

    run("http://example.com/x", &h);
    CHECK(streq(h.target_page, "x"), "the separator is the request line's, not the page's");
    EZPI_free_http_mbedtls_struct(&h);

    run("http://example.com/", &h);
    CHECK(streq(h.target_page, ""), "a bare trailing slash leaves an empty page");
    CHECK(streq(h.web_server, "example.com"), "and a host");
    EZPI_free_http_mbedtls_struct(&h);

    /* Nothing is truncated, which is only safe because nothing that would
     * break a request line gets this far -- see test 15. */
    run("http://example.com/a.b-c_d~e%20f", &h);
    CHECK(streq(h.target_page, "a.b-c_d~e%20f"),
          "an already-encoded page is passed through untouched");
    EZPI_free_http_mbedtls_struct(&h);
}

/* 15. A space or a control character anywhere in the URL.
 *
 * This is the one check in this file that exists because of the fix rather than
 * because of the ladder, and it is here because removing arm five's `%199[^\n]`
 * would otherwise have been a regression: that conversion stopped at a newline,
 * so dropping it without replacement would have let one reach the request line
 * for the first time.
 *
 * __ezlopi_http_generate_request writes the page into "GET /%s HTTP/1.1\r\n"
 * and the host into "Host: %s\r\n". A space in either splits the line; a CR or
 * an LF ends it and starts what follows as a new header. */
static void test_space_and_control_characters_are_refused(void)
{
    /* WAS: page "a b", so the request line read "GET /a b HTTP/1.1" -- path
     * "/a", version "b". Arm five's [^\n] accepted a space. */
    check_rejected("http://example.com/a b", "space in the page, WAS 'GET /a b HTTP/1.1'");
    /* WAS: host "exam ple.com", so "Host: exam ple.com\r\n". */
    check_rejected("http://exam ple.com/x", "space in the host, WAS a two-piece Host: header");
    /* WAS: page "a\tb" -- a tab is not '\n', so it went through. */
    check_rejected("http://example.com/a\tb", "tab in the page, WAS accepted");
    /* WAS: page "x\r". The LF stopped arm five's conversion but the CR before
     * it did not, so a carriage return reached the request line and the header
     * after it was dropped. Half an injection, shipped. */
    check_rejected("http://example.com/x\r\nX-Injected: 1",
                   "CRLF in the page, WAS page 'x\\r'");
    /* WAS: page "a" -- arm five stopped at the newline and the request went to
     * a path the action did not name. The only one of these five that the
     * ladder refused to put in the request, and the only one this check is
     * preventing a regression for rather than fixing. */
    check_rejected("http://example.com/a\nb", "newline in the page, WAS silently truncated to 'a'");
    /* A leading space, which is the shape a copy-pasted URL actually arrives
     * in. WAS: no arm matched " http://..." either -- the ladder refused this
     * one too, by accident rather than on purpose.
     *
     * This does NOT pin down where the guard's scan starts, and it is worth
     * saying so rather than leaving the impression that it does. A mutation
     * that starts the loop at url[1] survives the whole suite, because byte 0
     * is already forced by the prefix match: lower_ascii(url[0]) has to equal
     * 'h' for any scheme to match, so the only first bytes that reach an
     * authority are 'h' and 'H'. The URL below is refused either way, by the
     * scheme match instead of the guard. No input can distinguish the two, so
     * no check here can -- it is asserted for the behaviour, not for coverage
     * of the first iteration. */
    check_rejected(" http://example.com/x", "a leading space");
    /* And the boundary: 0x21 is the first byte that is not refused. */
    s_ezlopi_core_http_mbedtls_t h;
    run("http://example.com/!", &h);
    CHECK(streq(h.target_page, "!"), "0x21 is the first byte that is allowed through");
    EZPI_free_http_mbedtls_struct(&h);
    check_rejected("http://example.com/\x7f", "0x7F is refused");
    check_rejected("http://example.com/\x1f", "and so is 0x1F");

    /* The other boundary, and this one asserts a gap rather than a fix: a byte
     * above 0x7E goes through. It is not a valid URI character -- RFC 3986 is
     * ASCII and percent-encoding is what exists for everything else -- but it
     * cannot split a request line, and refusing a UTF-8 host would be a
     * behaviour change rather than a fix. Recorded as a Known gap.
     *
     * It is asserted rather than left unstated because it is what makes the
     * `(unsigned char)` cast in the guard testable. Plain char is signed on
     * this host and unsigned on xtensa, so without the cast 0xC3 compares as
     * -61, lands under 0x20, and gets refused here while sailing through on
     * the target -- a divergence that only shows up as a test failure if
     * something asserts the accepting answer. */
    run("http://ex\xc3\xa9mple.com/caf\xc3\xa9", &h);
    CHECK(streq(h.web_server, "ex\xc3\xa9mple.com"), "a high byte in the host is not refused (gap, not fix)");
    CHECK(streq(h.target_page, "caf\xc3\xa9"), "a high byte in the page is not refused either");
    CHECK(80 == h.web_port, "and the high-byte URL parses as an ordinary one");
    EZPI_free_http_mbedtls_struct(&h);
}

/* 14. The empty host, which is the one refusal the authority split cannot make
 * on its own: it returns a pointer into the buffer and "" is a valid pointer.
 *
 * WAS: "http://user@/x" reached arm five, whose %99[^/] took "user@" as the
 * whole authority. The authority split then moved the username out and left
 * the host empty, which went to the allocator and emitted a literal
 * "Host: \r\n" -- a header no server will route, and the one part of a request
 * a reader cannot see is missing. So this is half-fixed code being finished:
 * the split that fixed the credentialed URLs is what created the empty host.
 *
 * "http:///x" is the same answer by a different route: arm five's %99[^/]
 * matched zero characters and failed, and arm seven's %99[^\n] took "/x" as
 * the host. */
static void test_empty_host_is_refused(void)
{
    check_rejected("http://user@/x", "userinfo and no host");
    check_rejected("http://user:pass@/x", "credentials and no host");
    check_rejected("http:///x", "scheme, separator and no authority at all");
    /* The only row here the ladder already refused: arm seven's conversion
     * needs at least one character and there is none. */
    check_rejected("http://", "nothing after the scheme");
    /* WAS: host "?q=a", because arm seven is a catch-all. */
    check_rejected("http://?q=a", "a query and no host");
    check_rejected("http://@/x", "an empty userinfo and no host");
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
    test_overlong_input_does_not_overflow();
    test_ipv6_literal_is_not_mangled();
    test_explicit_credential_beats_the_url_userinfo();
    test_unspeakable_schemes_are_refused();
    test_port_is_decimal_and_range_checked();
    test_query_with_no_path_does_not_reach_the_host_header();
    test_page_keeps_no_separator_and_stops_at_nothing();
    test_empty_host_is_refused();
    test_space_and_control_characters_are_refused();

    printf("\n%d check(s), %d failure(s)\n", checks, fails);
    return (0 == fails) ? 0 : 1;
}
