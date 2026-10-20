/* test_aledonboard.c -- the documented onboarding example, checked.
 *
 * WHAT THIS IS FOR
 *
 * docs/addressable-led-onboarding.md tells a user how to onboard an
 * addressable strip, and shows a worked device-configuration document. A
 * documentation example that has drifted away from the code is worse than no
 * example, because it is read as authoritative and it is the first thing
 * anybody tries. This test extracts that example from the markdown and runs it
 * through the two real functions that decide its fate:
 *
 *   EZPI_util_devcfgval_check()  -- would this firmware store it?
 *   EZPI_aledcfg_from_json()     -- what strip shape does it resolve to?
 *
 * So the doc is the fixture. There is deliberately no copy of the JSON in this
 * file: a copy is the thing that drifts, and two copies that agree with each
 * other and not with the firmware would pass.
 *
 * WHY TWO BLOCKS
 *
 * The doc carries the configuration *and* the shape it claims to resolve to,
 * each behind its own marker. This test compares the second against the actual
 * resolution of the first, which means this file holds no expected numbers
 * either. Change 60 to 120 in one block and leave the other, and this goes
 * red -- which is the point.
 *
 * WHY THE CHIPSET STRING IS LINKED AND NOT TYPED OUT
 *
 * The expected chipset comes from ezlopi_ESP32_str in ezlopi_cloud_keywords.c,
 * the same definition EZPI_core_device_expected_chipset_name() returns for an
 * esp32 build. If this test spelled "ESP32" itself, a renamed constant would
 * leave the test green and every real configuration refused. Same argument the
 * Makefile makes for the three strip key names.
 *
 * WHAT A FAILURE HERE MEANS
 *
 * Any of: the doc's example would not be stored by this firmware; it does not
 * resolve to what the doc says; a marker or a fenced block is missing; the file
 * itself cannot be read. All four are reported distinctly, because "the doc is
 * wrong" and "this test cannot find the doc" need different fixes and a silent
 * skip would look like a pass in both cases.
 *
 * HOW IT WAS CONFIRMED TO BE ABLE TO FAIL
 *
 * Eight mutated copies of the doc were made under /root and DOC_PATH repointed
 * at each, so the worktree file was never touched: pixel_count 60 -> 61 in the
 * config block; rgbw flipped in the claimed block; chipset lowercased to
 * "esp32"; id_item 9 -> 11; the config marker deleted; the marker and its
 * fenced block deleted; DOC_PATH aimed at a file that does not exist; and
 * color_order "grb" -> "rbg". The real doc passed and all eight went red, each
 * at the check that should catch it.
 *
 * One of those eight was green on the first attempt, and the fault was in the
 * mutation, not here: the edit anchored on `"rgbw": true` at end of line and
 * both occurrences carry a trailing comma, so it produced a byte-identical
 * copy of the doc. A no-op mutant reads exactly like a test that failed to
 * notice. Anything re-running this should compare each mutant against the
 * original before trusting a green.
 *
 * A test that can only pass is not a test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"
#include "ezlopi_util_aledcfg.h"
#include "ezlopi_util_devcfgval.h"
#include "ezlopi_cloud_keywords.h"

/* Relative to tests/host, which is where the suite runs its binaries from. */
#ifndef DOC_PATH
#define DOC_PATH "../../docs/addressable-led-onboarding.md"
#endif

#define MARKER_CONFIG "<!-- machine-checked: strip-config -->"
#define MARKER_SHAPE "<!-- machine-checked: strip-config-resolves-to -->"

/* The region the config is stored in: 0x1000, as EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON.
 * Passed so the example is checked against the real size bound too, not just
 * its shape -- an example too large to store is an example that does not work. */
#define CONFIG_REGION_SIZE 4096u

static int checks = 0;
static int failures = 0;

#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(cond))                                              \
        {                                                         \
            failures++;                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                         \
    } while (0)

/* Read the whole doc. Returns NULL and counts a failure on any problem, so a
 * missing or unreadable file is a red test rather than zero checks run. */
static char *slurp_doc(void)
{
    FILE *const f = fopen(DOC_PATH, "rb");
    long size;
    char *buf;
    size_t got;

    if (NULL == f)
    {
        checks++;
        failures++;
        printf("FAIL  cannot open %s -- this test cannot find the document it checks\n",
               DOC_PATH);
        return NULL;
    }

    if ((0 != fseek(f, 0, SEEK_END)) || ((size = ftell(f)) < 0))
    {
        checks++;
        failures++;
        printf("FAIL  cannot size %s\n", DOC_PATH);
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (char *)malloc((size_t)size + 1u);
    if (NULL == buf)
    {
        checks++;
        failures++;
        printf("FAIL  out of memory reading %s\n", DOC_PATH);
        fclose(f);
        return NULL;
    }

    got = fread(buf, 1u, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Pull the fenced JSON block that follows `marker`.
 *
 * Returns a malloc'd NUL-terminated copy of the block's contents, or NULL with
 * a failure counted. Looks for the marker, then the next ```json fence, then
 * reads to the closing fence. Deliberately strict: a marker with no block after
 * it is a broken doc, not an absent check. */
static char *extract_block(const char *doc, const char *marker)
{
    const char *at = strstr(doc, marker);
    const char *open;
    const char *body;
    const char *close;
    char *out;
    size_t len;

    if (NULL == at)
    {
        checks++;
        failures++;
        printf("FAIL  marker not found in %s: %s\n", DOC_PATH, marker);
        return NULL;
    }

    /* The marker must be followed by the fence, not by prose that happens to
     * contain one later in the file -- so search from the marker and require
     * the fence to be close behind it. */
    open = strstr(at, "```json");
    if ((NULL == open) || ((size_t)(open - at) > strlen(marker) + 4u))
    {
        checks++;
        failures++;
        printf("FAIL  no ```json fence directly after marker %s\n", marker);
        return NULL;
    }

    body = open + strlen("```json");
    close = strstr(body, "```");
    if (NULL == close)
    {
        checks++;
        failures++;
        printf("FAIL  unterminated fenced block after marker %s\n", marker);
        return NULL;
    }

    len = (size_t)(close - body);
    out = (char *)malloc(len + 1u);
    if (NULL == out)
    {
        checks++;
        failures++;
        printf("FAIL  out of memory extracting block after %s\n", marker);
        return NULL;
    }
    memcpy(out, body, len);
    out[len] = '\0';
    return out;
}

/* Parse a block, counting a failure if it is not JSON. A block this test
 * cannot parse would otherwise be indistinguishable from one the firmware
 * rejected. */
static cJSON *parse_block(const char *json, const char *what)
{
    cJSON *const root = cJSON_Parse(__FUNCTION__, json);
    if (NULL == root)
    {
        checks++;
        failures++;
        printf("FAIL  the %s block in %s is not valid JSON\n", what, DOC_PATH);
    }
    return root;
}

/* ------------------------------------------------------------------------- */

/* The documented example must be a configuration this firmware would store.
 * Checked with the real region size, so "too large to store" counts as a
 * broken example rather than passing on shape alone. */
static void test_the_example_would_be_stored(cJSON *config)
{
    int bad_index = -2;
    char *serialised = cJSON_PrintUnformatted(__FUNCTION__, config);
    uint32_t len = 0u;
    e_ezpi_devcfgval_t reason;

    if (NULL != serialised)
    {
        len = (uint32_t)strlen(serialised);
    }

    reason = EZPI_util_devcfgval_check(config, ezlopi_ESP32_str, len,
                                       CONFIG_REGION_SIZE, &bad_index);

    if (EZPI_DEVCFGVAL_OK != reason)
    {
        printf("      refused as %s, entry %d\n",
               EZPI_util_devcfgval_token(reason), bad_index);
    }
    CHECK(EZPI_DEVCFGVAL_OK == reason,
          "the documented example is a configuration this firmware would store");

    /* Not a tautology: it is the claim the doc makes about the 4 KB field, and
     * an example that outgrew it would still have passed the shape checks. */
    CHECK((0u < len) && (len < CONFIG_REGION_SIZE),
          "the documented example fits the 4 KB config field");

    if (NULL != serialised)
    {
        free(serialised);
    }
}

/* The chipset in the example must be the one an esp32 build actually accepts.
 * devcfgval is handed that name by this test, so a doc saying "esp32" in
 * lowercase would pass the check above only because the test told it to --
 * this asserts the document's own string. */
static void test_the_example_names_a_chipset_this_build_accepts(cJSON *config)
{
    cJSON *const chip = cJSON_GetObjectItem(__FUNCTION__, config, ezlopi_chipset_str);

    CHECK((NULL != chip) && (cJSON_IsString(chip)) && (NULL != chip->valuestring) &&
              (0 == strcmp(chip->valuestring, ezlopi_ESP32_str)),
          "the example's chipset string is the one an esp32 build expects");
}

/* The strip entry must be the one this driver answers to. 9 is not a magic
 * number here: it is what the device table registers
 * device-rgp_strip-RMT_SK6812 under, and an example with any other id_item
 * would be stored and then skipped in silence at boot. */
static cJSON *find_the_strip_entry(cJSON *config)
{
    cJSON *const list = cJSON_GetObjectItem(__FUNCTION__, config, ezlopi_dev_detail_str);
    cJSON *entry = NULL;
    cJSON *found = NULL;

    CHECK((NULL != list) && cJSON_IsArray(list),
          "the example has a dev_detail array");

    if ((NULL == list) || !cJSON_IsArray(list))
    {
        return NULL;
    }

    cJSON_ArrayForEach(entry, list)
    {
        cJSON *const id_item = cJSON_GetObjectItem(__FUNCTION__, entry, ezlopi_id_item_str);
        if ((NULL != id_item) && cJSON_IsNumber(id_item) && (9 == id_item->valueint))
        {
            found = entry;
            break;
        }
    }

    CHECK(NULL != found,
          "the example contains an entry with id_item 9, the SK6812 strip driver");
    return found;
}

/* The heart of it: resolve the documented entry and compare against the shape
 * the doc claims, field by field, with no expected values held here. */
static void test_it_resolves_to_what_the_doc_claims(cJSON *entry, cJSON *claimed)
{
    s_aledcfg_t got;
    bool verbatim;
    cJSON *cj_count;
    cJSON *cj_rgbw;
    cJSON *cj_order;
    e_aled_order_t claimed_order = ALED_ORDER_GRB;

    memset(&got, 0, sizeof(got));
    verbatim = EZPI_aledcfg_from_json(entry, &got);

    /* A documented example that the resolver has to substitute defaults into
     * is a broken example, even though the resolver still yields a usable
     * config. The driver logs a TRACE_W in exactly this case, so a user
     * following the doc would see a warning for a config the doc handed them. */
    CHECK(verbatim,
          "the documented example is taken verbatim -- no key needed substituting");

    cj_count = cJSON_GetObjectItem(__FUNCTION__, claimed, ezlopi_pixel_count_str);
    cj_rgbw = cJSON_GetObjectItem(__FUNCTION__, claimed, ezlopi_rgbw_str);
    cj_order = cJSON_GetObjectItem(__FUNCTION__, claimed, ezlopi_color_order_str);

    CHECK((NULL != cj_count) && cJSON_IsNumber(cj_count),
          "the claimed-shape block states a pixel_count");
    CHECK((NULL != cj_rgbw) && cJSON_IsBool(cj_rgbw),
          "the claimed-shape block states rgbw as a boolean");
    CHECK((NULL != cj_order) && cJSON_IsString(cj_order) && (NULL != cj_order->valuestring),
          "the claimed-shape block states a color_order string");

    if ((NULL != cj_count) && cJSON_IsNumber(cj_count))
    {
        CHECK(got.pixel_count == (size_t)cj_count->valueint,
              "resolved pixel_count equals the count the doc claims");
    }
    if ((NULL != cj_rgbw) && cJSON_IsBool(cj_rgbw))
    {
        CHECK(got.has_white == (bool)cJSON_IsTrue(cj_rgbw),
              "resolved white-channel flag equals the one the doc claims");
    }
    if ((NULL != cj_order) && cJSON_IsString(cj_order) && (NULL != cj_order->valuestring))
    {
        CHECK(EZPI_aledcfg_order_from_str(cj_order->valuestring, &claimed_order),
              "the color_order the doc claims is one the parser understands");
        CHECK(got.order == claimed_order,
              "resolved colour order equals the one the doc claims");
    }

    /* The doc states that only grb is honoured and that anything else is driven
     * as grb with a warning. An example that asked for an unhonourable order
     * would be telling the user to expect a TRACE_W, so the doc's own example
     * must be the honourable one. */
    CHECK(ALED_ORDER_GRB == got.order,
          "the documented example asks for the one order this driver can honour");
}

int main(void)
{
    char *doc = slurp_doc();
    char *config_block = NULL;
    char *shape_block = NULL;
    cJSON *config = NULL;
    cJSON *shape = NULL;

    if (NULL != doc)
    {
        config_block = extract_block(doc, MARKER_CONFIG);
        shape_block = extract_block(doc, MARKER_SHAPE);
    }

    if (NULL != config_block)
    {
        config = parse_block(config_block, "strip-config");
    }
    if (NULL != shape_block)
    {
        shape = parse_block(shape_block, "strip-config-resolves-to");
    }

    if (NULL != config)
    {
        cJSON *entry;

        test_the_example_would_be_stored(config);
        test_the_example_names_a_chipset_this_build_accepts(config);

        entry = find_the_strip_entry(config);
        if ((NULL != entry) && (NULL != shape))
        {
            test_it_resolves_to_what_the_doc_claims(entry, shape);
        }
    }

    if (NULL != config)
    {
        cJSON_Delete(__FUNCTION__, config);
    }
    if (NULL != shape)
    {
        cJSON_Delete(__FUNCTION__, shape);
    }
    free(config_block);
    free(shape_block);
    free(doc);

    /* Zero checks is a failure in its own right: it means the extraction never
     * got far enough to assert anything, which a bare "0 check(s), 0
     * failure(s)" would otherwise report as a clean pass. */
    if (0 == checks)
    {
        failures++;
        printf("FAIL  no checks ran -- the document was never reached\n");
    }

    /* Bare, with no prefix: "N check(s), M failure(s)" is the shape the
     * suite's aggregate tally reads. */
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
