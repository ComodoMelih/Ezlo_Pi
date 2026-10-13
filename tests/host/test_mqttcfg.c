/**
 * @file  test_mqttcfg.c
 * @brief Host tests for ezlopi-util-mqttcfg.
 *
 * WHAT THIS IS GUARDING
 *
 * MQTT settings used to be Kconfig macros, fixed at build time and therefore
 * typed by whoever built the firmware. They are now stored in NVS and can be
 * written at runtime, which makes them the first configuration this project
 * accepts from a caller rather than from a developer. Validation that used to
 * be unnecessary is now the only thing between an arbitrary string and the
 * MQTT client.
 *
 * THE CHECK THAT MATTERS
 *
 * test_prefix_wildcards_are_refused(). The service subscribes to
 * <prefix>/<node>/+/set and acts on anything that arrives there by setting an
 * item's value -- switching a relay. '+' and '#' are MQTT wildcards *in a
 * subscription*, so a prefix containing either widens that subscription beyond
 * this device's own topic tree, while simultaneously making every publish
 * invalid per the MQTT specification, because wildcards are forbidden in a
 * topic being published to. The device goes quiet and starts taking orders from
 * other devices' topics, from one character. Nothing about that is visible on
 * the device, which is why it is checked here.
 *
 * THE CHECK THAT ALMOST WENT THE WRONG WAY
 *
 * test_scheme_case_is_accepted(). The first version of the validator compared
 * schemes with strncmp() and so refused "MQTT://192.168.1.10:1883". That looked
 * like strictness. It was a bug: esp-mqtt parses the scheme and then picks a
 * transport with esp_transport_list_get_transport(), which compares using
 * strcasecmp() (esp-idf components/tcp_transport/transport.c:65), so an
 * uppercase scheme genuinely works on the device. The validator would have been
 * inventing a failure of its own and reporting it as a bad URI, with the
 * capitalisation being the only thing wrong and no way for anyone to tell. The
 * checks below pin the accepting behaviour so it cannot be "tightened" back.
 *
 * THE LENGTH LIMITS ARE A REAL FAILURE MODE, NOT TIDINESS
 *
 * test_prefix_length_bound() exists because the service builds topics into
 * fixed 192-byte buffers and *refuses to publish* when one does not fit. A
 * prefix that is legal but one byte too long would be stored happily and then
 * cause every publish and every discovery message to be silently dropped,
 * leaving a correct-looking configuration and a device that does nothing. The
 * bound is checked here, and the service separately asserts at compile time
 * that the bound actually makes the worst-case topic fit.
 */
#include <stdio.h>
#include <string.h>

#include "ezlopi_util_mqttcfg.h"

static int g_checks;
static int g_fails;

static void ok(int cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_fails++;
        printf("  FAIL: %s\n", what);
    }
}

/* Builds a string of n copies of ch. n must be < sizeof(buf). */
static void fill(char *buf, size_t n, char ch)
{
    memset(buf, ch, n);
    buf[n] = '\0';
}

/*******************************************************************************
 *                              Broker URI
 *******************************************************************************/

static void test_uri_accepts_what_people_actually_type(void)
{
    printf("-- broker URIs that must work --\n");

    /* The two forms the Kconfig help text itself gives as examples. If either
    ** of these were refused, the documented configuration would be
    ** unreachable. */
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://192.168.1.10:1883"),
       "the documented IP:port example");
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://homeassistant.local:1883"),
       "the documented mDNS-name example");

    /* No port. esp-mqtt defaults to 1883 for mqtt:// and this is the shortest
    ** thing anyone will type. */
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://broker"), "host with no port");
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://192.168.1.10"), "IP with no port");

    /* All four schemes esp-mqtt has a transport for. mqtts and wss need
    ** CONFIG_MQTT_TRANSPORT_SSL, which the committed defaults currently turn
    ** off -- but that is a build-configuration question, not a reason to
    ** reject the URI as malformed, and the distinction matters because the two
    ** failures need completely different fixes. */
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://h"), "mqtt scheme");
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtts://h"), "mqtts scheme");
    ok(EZPI_util_mqttcfg_uri_is_valid("ws://h"), "ws scheme");
    ok(EZPI_util_mqttcfg_uri_is_valid("wss://h"), "wss scheme");

    /* Websocket brokers need a path; HA's own add-on serves ws at /mqtt. */
    ok(EZPI_util_mqttcfg_uri_is_valid("ws://h:1884/mqtt"), "ws with a path");
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://h/path"), "path with no port");

    /* Port boundaries. 1 and 65535 are both legal and both sit exactly on the
    ** edge of the accepted range. */
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://h:1"), "port 1");
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://h:65535"), "port 65535");

    /* A hostname containing the characters that show up in real ones. */
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://my-broker_01.lan:1883"),
       "hyphen, underscore and dots in the host");
}

static void test_scheme_case_is_accepted(void)
{
    printf("-- scheme case (esp-mqtt uses strcasecmp) --\n");

    /* See the file comment. These are not cosmetic: refusing them was a real
    ** bug in the first draft of this validator. */
    ok(EZPI_util_mqttcfg_uri_is_valid("MQTT://192.168.1.10:1883"),
       "uppercase scheme is accepted, because the device accepts it");
    ok(EZPI_util_mqttcfg_uri_is_valid("Mqtt://h"), "mixed case scheme");
    ok(EZPI_util_mqttcfg_uri_is_valid("MQTTS://h"), "uppercase mqtts");
    ok(EZPI_util_mqttcfg_uri_is_valid("WS://h"), "uppercase ws");
    ok(EZPI_util_mqttcfg_uri_is_valid("WSS://h"), "uppercase wss");

    /* The scheme is folded; the host is not, and must not be, because it is
    ** passed through to the resolver unchanged. This only checks that folding
    ** the scheme did not accidentally fold the whole string into something
    ** that still validates for the wrong reason. */
    ok(EZPI_util_mqttcfg_uri_is_valid("mqtt://CAPS-HOST.lan"),
       "an uppercase host is still fine");
}

static void test_uri_rejects_malformed(void)
{
    printf("-- broker URIs that must be refused --\n");

    ok(false == EZPI_util_mqttcfg_uri_is_valid(NULL), "NULL");
    ok(false == EZPI_util_mqttcfg_uri_is_valid(""), "empty");

    /* No scheme at all. This is the single most likely thing a user types:
    ** they know their broker's address and not that it needs a scheme. It
    ** must be refused clearly rather than handed to esp-mqtt, which treats a
    ** scheme-less URI as having no transport and fails later and less
    ** legibly. */
    ok(false == EZPI_util_mqttcfg_uri_is_valid("192.168.1.10"),
       "bare IP with no scheme");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("192.168.1.10:1883"),
       "bare IP:port with no scheme");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("homeassistant.local"),
       "bare hostname");

    /* The wrong scheme. http:// is what someone pastes after finding their
    ** Home Assistant web address. */
    ok(false == EZPI_util_mqttcfg_uri_is_valid("http://h:1883"), "http");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("https://h"), "https");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("tcp://h"), "tcp");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt:/h"), "one slash");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt:h"), "no slashes");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("://h"), "slashes with no scheme");

    /* A scheme that is a prefix of a real one, and one that extends a real
    ** one. Neither has a transport. */
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqt://h"), "mqt");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqttx://h"), "mqttx");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("w://h"), "w");

    /* Scheme but no host. */
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://"), "scheme only");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtts://"), "mqtts scheme only");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://:1883"),
       "port with no host");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt:///path"),
       "path with no host");

    /* Ports that are not ports. ":0" and ":65536" both parse as integers and
    ** neither is usable; strtol() inside esp-mqtt would take them without
    ** complaint. */
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:0"), "port 0");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:65536"),
       "port 65536, one past the top");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:99999"), "port 99999");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:123456"),
       "six-digit port");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:"), "colon, no port");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:abc"),
       "non-numeric port");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:1883x"),
       "trailing junk after the port");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h:18 83"),
       "space inside the port");

    /* Whitespace. A URI copied out of a chat message or a web page arrives
    ** with a leading or trailing space more often than anything else on this
    ** list, and a trailing space in a hostname produces a DNS failure that
    ** looks exactly like a network problem. */
    ok(false == EZPI_util_mqttcfg_uri_is_valid(" mqtt://h"), "leading space");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h "), "trailing space");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://ho st"),
       "space inside the host");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h\t"), "tab");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h\n"),
       "trailing newline, as a file or a form field supplies it");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h\r\n"), "CRLF");
    ok(false == EZPI_util_mqttcfg_uri_is_valid("mqtt://h/pa th"),
       "space inside the path");
}

static void test_uri_length_bound(void)
{
    printf("-- broker URI length --\n");

    char buf[EZPI_MQTTCFG_URI_MAX + 64u];
    const char *const scheme = "mqtt://";
    const size_t scheme_len = strlen(scheme);

    /* Exactly at the limit must be accepted: an off-by-one here would refuse
    ** a value the documentation says is allowed. */
    memcpy(buf, scheme, scheme_len);
    fill(buf + scheme_len, EZPI_MQTTCFG_URI_MAX - scheme_len, 'h');
    ok(strlen(buf) == EZPI_MQTTCFG_URI_MAX, "test built a URI of exactly the max");
    ok(EZPI_util_mqttcfg_uri_is_valid(buf), "a URI of exactly the max length");

    /* One over must be refused. */
    memcpy(buf, scheme, scheme_len);
    fill(buf + scheme_len, (EZPI_MQTTCFG_URI_MAX - scheme_len) + 1u, 'h');
    ok(strlen(buf) == EZPI_MQTTCFG_URI_MAX + 1u, "test built a URI one over");
    ok(false == EZPI_util_mqttcfg_uri_is_valid(buf), "one byte over the max");
}

/*******************************************************************************
 *                              Topic prefixes
 *******************************************************************************/

static void test_prefix_accepts_sensible_values(void)
{
    printf("-- topic prefixes that must work --\n");

    /* The two shipped defaults. If either of these were refused, a device
    ** with no stored configuration would fall back to a prefix it then
    ** rejected, and MQTT would never start. */
    ok(EZPI_util_mqttcfg_prefix_is_valid("ezlopi"),
       "the default topic prefix");
    ok(EZPI_util_mqttcfg_prefix_is_valid("homeassistant"),
       "the default discovery prefix");

    ok(EZPI_util_mqttcfg_prefix_is_valid("a"), "a single character");
    ok(EZPI_util_mqttcfg_prefix_is_valid("home/ezlopi"),
       "a nested prefix, which is a normal thing to want");
    ok(EZPI_util_mqttcfg_prefix_is_valid("home/floor1/ezlopi"),
       "several levels");
    ok(EZPI_util_mqttcfg_prefix_is_valid("ezlopi-hub_2"),
       "hyphen, underscore and digits");
    ok(EZPI_util_mqttcfg_prefix_is_valid("EzloPi"), "mixed case is untouched");
}

static void test_prefix_wildcards_are_refused(void)
{
    printf("-- topic prefix wildcards (the check that matters) --\n");

    /* A '#' prefix turns the command subscription into "#/<node>/+/set".
    ** A '+' prefix widens it a level at a time. Either way the hub starts
    ** accepting item-set commands published under topics that are not its
    ** own, and stops being able to publish at all, because the specification
    ** forbids wildcards in a topic name being published to. */
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("#"),
       "'#' alone");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("+"),
       "'+' alone");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi/#"),
       "a trailing '#' level");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi/+"),
       "a trailing '+' level");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("#/ezlopi"),
       "a leading '#' level");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("+/ezlopi"),
       "a leading '+' level");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ez#lopi"),
       "'#' in the middle of a level, which MQTT also forbids");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ez+lopi"),
       "'+' in the middle of a level");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi#"),
       "'#' appended with no separator");
}

static void test_prefix_separators(void)
{
    printf("-- topic prefix separators --\n");

    /* A leading '/' makes the first topic level empty, so the published topic
    ** becomes "/ezlopi/..." -- legal MQTT, and a different topic from the one
    ** the user believes they configured. */
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("/ezlopi"), "leading slash");

    /* A trailing '/' is worse: the service appends "/<node>", so the result
    ** contains "//" and an empty level in the middle. */
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi/"), "trailing slash");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("/"), "slash alone");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("//"), "two slashes");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("a//b"), "an empty level");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("a///b"), "two empty levels");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("/ezlopi/"), "both ends");
}

static void test_prefix_rejects_junk(void)
{
    printf("-- topic prefixes that must be refused --\n");

    ok(false == EZPI_util_mqttcfg_prefix_is_valid(NULL), "NULL");

    /* Empty would produce a topic starting with '/'. It is also what an
    ** absent or truncated NVS entry looks like, so it has to be refused for
    ** the fallback to Kconfig to happen. */
    ok(false == EZPI_util_mqttcfg_prefix_is_valid(""), "empty");

    ok(false == EZPI_util_mqttcfg_prefix_is_valid(" ezlopi"), "leading space");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi "), "trailing space");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlo pi"), "embedded space");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi\n"), "newline");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi\t"), "tab");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezlopi\x7f"), "DEL");
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezl\x01opi"),
       "an embedded control character");

    /* Above 0x7E. Deliberately narrowed: MQTT topics are UTF-8 so this could
    ** in principle be valid, but the topic buffers are counted in bytes while
    ** anyone choosing such a prefix counts characters. */
    ok(false == EZPI_util_mqttcfg_prefix_is_valid("ezl\xc3\xa9pi"),
       "non-ASCII is refused on purpose, not by accident");
}

static void test_prefix_length_bound(void)
{
    printf("-- topic prefix length --\n");

    char buf[EZPI_MQTTCFG_PREFIX_MAX + 64u];

    fill(buf, EZPI_MQTTCFG_PREFIX_MAX, 'p');
    ok(strlen(buf) == EZPI_MQTTCFG_PREFIX_MAX,
       "test built a prefix of exactly the max");
    ok(EZPI_util_mqttcfg_prefix_is_valid(buf),
       "a prefix of exactly the max length");

    fill(buf, EZPI_MQTTCFG_PREFIX_MAX + 1u, 'p');
    ok(false == EZPI_util_mqttcfg_prefix_is_valid(buf),
       "one byte over the max");

    /* The reason the bound exists. A prefix at the limit still has to leave
    ** room for "/<component>/<node>/<object>/config" inside the service's
    ** 192-byte topic buffer. This reproduces that arithmetic here so that
    ** raising EZPI_MQTTCFG_PREFIX_MAX without raising the buffer fails a test
    ** as well as the service's static assertion. */
    {
        const size_t topic_buf = 192u;   /* EZPI_MQTT_TOPIC_LEN */
        const size_t component = 13u;    /* "binary_sensor" */
        const size_t node = 13u;         /* "ezlopi_" + 6 hex */
        const size_t object = 63u;       /* EZPI_MQTT_OBJECT_ID_LEN - 1 */
        const size_t leaf = 6u;          /* "config" / "status" */
        const size_t separators = 4u;
        const size_t worst = EZPI_MQTTCFG_PREFIX_MAX + separators + component +
                             node + object + leaf + 1u /* NUL */;

        ok(worst <= topic_buf,
           "the worst-case discovery topic fits in the service's buffer");
    }
}

/*******************************************************************************
 *                              Credentials
 *******************************************************************************/

static void test_cred_validation(void)
{
    printf("-- usernames and passwords --\n");

    /* Empty is valid and load-bearing: it is how "anonymous broker" is
    ** expressed, and the service sends no credentials at all rather than
    ** empty ones because brokers treat those differently. If this were
    ** rejected, an anonymous broker would be unconfigurable. */
    ok(EZPI_util_mqttcfg_cred_is_valid(""),
       "empty is valid and means 'no credentials'");

    ok(EZPI_util_mqttcfg_cred_is_valid("homeassistant"), "a plain username");
    ok(EZPI_util_mqttcfg_cred_is_valid("p@ssw0rd!#+/%&*"),
       "punctuation, including MQTT wildcards, is fine in a password -- it is "
       "never used to build a topic");

    /* This check used to assert the opposite -- that an embedded space is
    ** refused -- because that is what the code did, and the test was written
    ** from the code rather than from the contract. The header has always said
    ** credentials have "no alphabet restriction" and are only refused for
    ** length and control characters; the implementation was sharing a helper
    ** with the URI path, which rejects spaces for good reasons of its own.
    **
    ** A space in a password is ordinary, nothing downstream of the credential
    ** can be confused by one, and a user whose broker password contains a
    ** space would have had no way to configure this device and no explanation
    ** beyond "Wrong params". */
    ok(EZPI_util_mqttcfg_cred_is_valid("pass word"),
       "an embedded space is accepted -- a password is not a topic");
    ok(EZPI_util_mqttcfg_cred_is_valid(" leading and trailing "),
       "including at the ends, where it is invisible but still the password");

    /* Non-ASCII, for the same reason: MQTT usernames are UTF-8 and passwords
    ** are binary. This is a two-byte UTF-8 'e-acute'. */
    ok(EZPI_util_mqttcfg_cred_is_valid("caf\xc3\xa9"),
       "a non-ASCII password is accepted");

    ok(false == EZPI_util_mqttcfg_cred_is_valid(NULL), "NULL");
    ok(false == EZPI_util_mqttcfg_cred_is_valid("pass\n"),
       "trailing newline, which is how a password pasted from a file arrives");
    ok(false == EZPI_util_mqttcfg_cred_is_valid("pass\t"), "tab");
    ok(false == EZPI_util_mqttcfg_cred_is_valid("pa\x01ss"),
       "control character");
    ok(false == EZPI_util_mqttcfg_cred_is_valid("pa\x7fss"),
       "0x7F, which is a control character everywhere except isprint()");

    {
        char buf[EZPI_MQTTCFG_CRED_MAX + 64u];

        fill(buf, EZPI_MQTTCFG_CRED_MAX, 'c');
        ok(EZPI_util_mqttcfg_cred_is_valid(buf), "exactly the max length");

        fill(buf, EZPI_MQTTCFG_CRED_MAX + 1u, 'c');
        ok(false == EZPI_util_mqttcfg_cred_is_valid(buf), "one over the max");
    }
}

/*******************************************************************************
 *                                  Main
 *******************************************************************************/

int main(void)
{
    printf("=== ezlopi-util-mqttcfg ===\n");

    test_uri_accepts_what_people_actually_type();
    test_scheme_case_is_accepted();
    test_uri_rejects_malformed();
    test_uri_length_bound();

    test_prefix_accepts_sensible_values();
    test_prefix_wildcards_are_refused();
    test_prefix_separators();
    test_prefix_rejects_junk();
    test_prefix_length_bound();

    test_cred_validation();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    if (0 != g_fails)
    {
        printf("MQTTCFG TESTS FAILED\n");
        return 1;
    }

    printf("MQTTCFG TESTS PASSED\n");
    return 0;
}
