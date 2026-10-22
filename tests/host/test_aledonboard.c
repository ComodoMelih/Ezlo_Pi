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
 * WHY FOUR BLOCKS
 *
 * The doc carries the configuration, the shape it claims to resolve to, and
 * the same configuration again with a `cmd` member in front of it for the
 * serial route -- each behind its own marker. This test compares the second
 * against the actual resolution of the first and the third against the first
 * itself, which means this file holds no expected numbers either. Change 60 to
 * 120 in one block and leave the others, and this goes red -- which is the
 * point.
 *
 * The fourth block is different in kind: it is the WiFi line (`cmd:2`) the doc
 * tells a reader to send before the websocket route, because that route cannot
 * be reached on a factory-fresh hub -- it has no network. That block is not a
 * copy of anything in the document, so there is nothing to compare it against;
 * it is checked against the firmware directly, member by member. See the
 * comment on test_the_wifi_block_is_what_the_firmware_accepts() for what and
 * why, including the one check whose value is read out of
 * ezlopi-user-config/EZLOPI_USER_CONFIG.h at runtime rather than included.
 *
 * The third block is checked for two things beyond being the same document,
 * and both of them were wrong in this doc before the block existed: it named
 * cmd 7, which is set-*provisioning*, and it did not say the configuration
 * goes at the top level while the provisioning payload it pointed at nests
 * under `data`. A user following it would have sent a device configuration to
 * the certificate handler, or stored a document that yields a hub with no
 * devices and acknowledges the write.
 *
 * WHY THE COMMAND NUMBER IS LINKED AND NOT TYPED OUT
 *
 * Same argument as the chipset string below: EZPI_UART_CMD_SET_CONFIG comes
 * from the real ezlopi_service_uart.h, so inserting a command above it in that
 * enum makes this test red rather than leaving the doc quietly naming a
 * different command. There is deliberately no 3 in this file.
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
 * resolve to what the doc says; the serial route's block names a command other
 * than set-config, nests the configuration under `data`, or has stopped being
 * a copy of the worked example; the WiFi block names a command other than
 * EZPI_UART_CMD_WIFI, nests under `data`, loses either credential key, shows a
 * password this firmware would refuse on length, or carries a member the
 * handler does not read; a marker or a fenced block is missing; either file
 * itself cannot be read. All of those are reported distinctly, because "the doc
 * is wrong" and "this test cannot find the doc" need different fixes and a
 * silent skip would look like a pass in both cases.
 *
 * HOW IT WAS CONFIRMED TO BE ABLE TO FAIL
 *
 * Thirteen mutated copies of the doc were made under /root and DOC_PATH
 * repointed at each, so the worktree file was never touched: pixel_count
 * 60 -> 61 in the config block; rgbw flipped in the claimed block; chipset
 * lowercased to "esp32"; id_item 9 -> 11; the config marker deleted; the
 * marker and its fenced block deleted; DOC_PATH aimed at a file that does not
 * exist; color_order "grb" -> "rbg" in the claimed block only; the serial
 * block's cmd 3 -> 7; the serial block nested under `data`; the serial copy
 * drifted to 120 pixels while the worked example stayed at 60; and the serial
 * marker deleted. The real doc passed and all twelve went red, each at the
 * check that should catch it.
 *
 * The thirteenth does not mutate the doc at all: it compiles this file against
 * a copy of ezlopi_service_uart.h with one command inserted above SET_CONFIG,
 * so the enum says 4 while the real, unmutated doc says 3. That one is red
 * only because the number is linked; a 3 typed into this file would have
 * stayed green with the document naming the wrong command. It cannot be done
 * with -D, because defining an enumerator as a macro makes the header a syntax
 * error.
 *
 * Five of those mutate one block and leave the others, which is the shape a
 * real drift takes. A further mutant exists to be *green*: all three blocks
 * changed to say 120 pixels instead of 60. That is a doc describing a
 * different strip, not a doc disagreeing with the firmware, and it passes --
 * which is what proves this file compares the blocks rather than pinning the
 * literal 60. Without that one, every red mutant could be red for the wrong
 * reason.
 *
 * One of the first eight was green on the first attempt, and the fault was in
 * the mutation, not here: the edit anchored on `"rgbw": true` at end of line
 * and every occurrence carries a trailing comma, so it produced a
 * byte-identical copy of the doc. A no-op mutant reads exactly like a test
 * that failed to notice. Anything re-running this should compare each mutant
 * against the original before trusting a green, which the sweep now does for
 * every one of them.
 *
 * Deleting the serial marker is worth singling out, because it is the mutation
 * that reports 21 checks where the doc reports 24: four checks stop running
 * and the extraction counts one failure in their place. A test that treated a
 * missing marker as "nothing to check" would have reported a smaller green,
 * and a smaller green is indistinguishable from a pass in a suite tally.
 *
 * THE WIFI BLOCK'S OWN MUTANTS
 *
 * Added later, and swept the same way -- copies under /tmp with DOC_PATH and
 * USERCONFIG_PATH repointed, so the worktree is never edited. The document
 * reports 24 checks and 0 failures. Nine mutants, all killed: cmd 2 -> 3 (the
 * transposition, since 2 and 3 are adjacent in the enum); the credentials
 * nested under `data`; the password cut to 3 characters; an empty ssid; `ssid`
 * renamed to `network`; a `dev_detail` pasted in beside the credentials; the
 * WiFi marker deleted (18 checks, not a smaller green); the serial marker
 * deleted (21); and -- the one that matters most -- the real, unmutated
 * document compiled against a header copy whose
 * EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN reads 32 instead of 8. That last one went
 * red, reporting an 18-character example against a 32-character minimum, which
 * is the evidence that the minimum is genuinely read out of the firmware's
 * header: had an 8 been typed into this file, that mutant would have been
 * green. A tenth, a header with the constant deleted entirely, is also red --
 * a renamed constant must not become a skipped check.
 *
 * Two green controls, and one of them corrected an expectation of mine rather
 * than finding a defect. A different but still-valid placeholder password stays
 * green, which is what shows the check tests the length and not the literal
 * string. And capitalising the key to "SSID" stays green: it was written
 * expecting a kill, and cJSON_GetObjectItem passes case_sensitive = false
 * (cjext.c:2444), so the firmware accepts it too. The check was right and the
 * mutant's premise was wrong. A check stricter than the firmware would be a
 * worse defect than the one it pretended to catch, so it is kept as a green
 * control instead of being "fixed".
 *
 * A test that can only pass is not a test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ezlopi_service_uart.h declares EZPI_SERV_uart_tx_data() with a uint8_t
** parameter and includes only "sdkconfig.h", so it cannot be the first header
** in a translation unit. Including <stdint.h> ahead of it is what every real
** build does by accident, through its other includes; doing it on purpose here
** keeps this test from depending on cjext.h continuing to pull it in. The
** header's missing <stdint.h> is a pre-existing defect, left alone because
** this commit is about the document. */
#include <stdint.h>

#include "cjext.h"
#include "ezlopi_util_aledcfg.h"
#include "ezlopi_util_devcfgval.h"
#include "ezlopi_cloud_keywords.h"
#include "ezlopi_service_uart.h"

/* Relative to tests/host, which is where the suite runs its binaries from. */
#ifndef DOC_PATH
#define DOC_PATH "../../docs/addressable-led-onboarding.md"
#endif

/* Also relative to tests/host. Read as text rather than included, for the
 * reason set out above read_define_u(). */
#ifndef USERCONFIG_PATH
#define USERCONFIG_PATH "../../ezlopi-user-config/EZLOPI_USER_CONFIG.h"
#endif

#define MARKER_CONFIG "<!-- machine-checked: strip-config -->"
#define MARKER_SHAPE "<!-- machine-checked: strip-config-resolves-to -->"
#define MARKER_SERIAL "<!-- machine-checked: strip-config-over-serial -->"
#define MARKER_WIFI "<!-- machine-checked: wifi-over-serial -->"

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

/* Read a whole file. Returns NULL and counts a failure on any problem, so a
 * missing or unreadable file is a red test rather than zero checks run.
 *
 * Takes a path because this test now reads two files out of the tree: the
 * document it checks, and the firmware header the minimum password length is
 * read from. Both are fixtures, and neither may be unreadable silently. */
static char *slurp(const char *path)
{
    FILE *const f = fopen(path, "rb");
    long size;
    char *buf;
    size_t got;

    if (NULL == f)
    {
        checks++;
        failures++;
        printf("FAIL  cannot open %s -- this test cannot find a file it checks against\n",
               path);
        return NULL;
    }

    if ((0 != fseek(f, 0, SEEK_END)) || ((size = ftell(f)) < 0))
    {
        checks++;
        failures++;
        printf("FAIL  cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = (char *)malloc((size_t)size + 1u);
    if (NULL == buf)
    {
        checks++;
        failures++;
        printf("FAIL  out of memory reading %s\n", path);
        fclose(f);
        return NULL;
    }

    got = fread(buf, 1u, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Read `#define <name> <digits>` out of an already-slurped C header.
 *
 * WHY THIS IS NOT A #include, AND NOT A RETYPED 8
 *
 * The wanted constant is EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN, which lives in
 * ezlopi-user-config/EZLOPI_USER_CONFIG.h. That header cannot go on a host
 * include path: it includes "driver/uart.h" and "ezlopi_core_heap.h", neither
 * of which exists off an ESP-IDF build. The host suite has a stand-in at
 * stubs/EZLOPI_USER_CONFIG.h, and -I$(STUBS_DIR) is on this test's include
 * path -- so a plain #include here would silently resolve to the stub, and the
 * check would be comparing the document against a number typed into the test
 * suite rather than against the firmware's. That is the exact failure this
 * whole file exists to prevent, one level up.
 *
 * Typing 8 in here has the same defect: the check would then agree with my
 * typo instead of with the firmware. So the real header is read as text, the
 * same way the document is. Raising the minimum in the firmware makes this test
 * re-read it; a document whose example password no longer clears it goes red.
 *
 * Returns false and counts a failure if the define is absent or unparseable --
 * a renamed constant must be a red test, not a skipped check. */
static bool read_define_u(const char *header, const char *name, unsigned long *out)
{
    const char *at = (NULL != header) ? strstr(header, name) : NULL;

    while (NULL != at)
    {
        const char *p = at + strlen(name);

        /* Require the match to be the whole identifier and to be a #define of
         * it, not a mention inside a comment or a longer name. */
        if ((at == header) || ('\n' == at[-1]) || (' ' == at[-1]) || ('\t' == at[-1]))
        {
            if ((' ' == *p) || ('\t' == *p))
            {
                while ((' ' == *p) || ('\t' == *p))
                {
                    p++;
                }
                if (('0' <= *p) && ('9' >= *p))
                {
                    *out = strtoul(p, NULL, 10);
                    return true;
                }
            }
        }
        at = strstr(at + 1, name);
    }

    checks++;
    failures++;
    printf("FAIL  no numeric '#define %s' in %s -- the constant this test reads"
           " has been renamed or moved, so the document's example password is"
           " no longer being checked against the firmware\n",
           name, USERCONFIG_PATH);
    return false;
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

    /* There was a check here asserting the example asked for grb specifically,
     * on the grounds that the driver honoured no other order and so any other
     * example would be telling the user to expect a TRACE_W. The driver now
     * honours all six, so that reason is gone and the check went with it rather
     * than being kept for the shape of it: a check whose stated justification
     * is no longer true is worse than no check, because the next reader takes
     * the justification at face value. The order is still compared against the
     * doc's own claimed-shape block above, which is the part that can drift. */
}

/* The serial route's block is the worked example again, with a `cmd` member in
 * front of it. Both halves of that sentence are checked here, because both
 * were wrong in this document before this test existed: it named cmd 7, which
 * is set-*provisioning*, and it did not say the configuration goes at the top
 * level while the provisioning payload it pointed at nests under `data`.
 *
 * The command number is taken from EZPI_UART_CMD_SET_CONFIG rather than typed,
 * for the same reason the chipset string is linked: a reordered enum must make
 * this red, and a `3` written here would agree with the doc and with nothing
 * else. Three is not an expected value in this file.
 *
 * The equality check is what keeps the second copy of the example honest. A
 * doc that shows one configuration in the worked example and a different one
 * over serial has drifted even if both would be stored, and the drift is
 * invisible to a check that only validates each block on its own. */
static void test_the_serial_example_is_the_same_document(cJSON *serial, cJSON *config)
{
    cJSON *const cj_cmd = cJSON_GetObjectItem(__FUNCTION__, serial, ezlopi_cmd_str);
    cJSON *const cj_data = cJSON_GetObjectItem(__FUNCTION__, serial, ezlopi_data_str);
    cJSON *stripped;

    CHECK((NULL != cj_cmd) && cJSON_IsNumber(cj_cmd),
          "the serial block carries a numeric cmd");

    if ((NULL != cj_cmd) && cJSON_IsNumber(cj_cmd))
    {
        if (EZPI_UART_CMD_SET_CONFIG != cj_cmd->valueint)
        {
            printf("      doc says cmd %d, EZPI_UART_CMD_SET_CONFIG is %d"
                   " (EZPI_UART_CMD_SET_PROV is %d)\n",
                   cj_cmd->valueint, (int)EZPI_UART_CMD_SET_CONFIG,
                   (int)EZPI_UART_CMD_SET_PROV);
        }
        CHECK(EZPI_UART_CMD_SET_CONFIG == cj_cmd->valueint,
              "the serial block's cmd is EZPI_UART_CMD_SET_CONFIG, from the firmware's enum");
    }

    /* Not a style point. ezpi_service_uart_set_config() parses the whole
     * received line and hands that root to the storer -- it reads no `data`
     * member -- so a document nested the way the provisioning route nests its
     * own is stored intact and yields a hub with no devices. The get-config
     * handler deleting `cmd` from what it read back out of flash
     * (ezlopi_service_uart.c:1463) is the firmware's own confirmation that the
     * stored document is the received line, `cmd` included. */
    CHECK(NULL == cj_data,
          "the serial block has no data wrapper -- the config sits at the top level");

    stripped = cJSON_Duplicate(__FUNCTION__, serial, true);
    if (NULL == stripped)
    {
        checks++;
        failures++;
        printf("FAIL  out of memory duplicating the serial block\n");
        return;
    }
    cJSON_DeleteItemFromObject(__FUNCTION__, stripped, ezlopi_cmd_str);

    /* Case-sensitive: the key spellings the firmware uses are case-sensitive
     * everywhere else, and a doc whose serial block said "Chipset" would be a
     * doc that does not work. */
    CHECK(cJSON_Compare(stripped, config, true),
          "the serial block, less its cmd, is the same document as the worked example");

    cJSON_Delete(__FUNCTION__, stripped);
}

/* The WiFi line the doc tells a user to send before the websocket route.
 *
 * This block is NOT a copy of the worked example and is not compared against
 * it -- it is a different command with a different payload, and the only
 * reason it is in this document at all is that the `hub.ezlopi.config.set`
 * route cannot be reached until the hub is on a network.
 *
 * Checked against the firmware rather than against itself:
 *
 *   cmd        EZPI_UART_CMD_WIFI, from the same enum as SET_CONFIG. 2 and 3
 *              are adjacent in it, which is exactly the pair a reader can
 *              transpose, and sending a configuration to the WiFi handler or
 *              credentials to the config handler both look like a hub that
 *              ignored you.
 *   ssid/pass  the key spellings from ezlopi_cloud_keywords.c, which is what
 *              ezpi_service_uart_set_wifi() looks up. Not retyped here.
 *
 *              Looked up with cJSON_GetObjectItem, the same call the handler
 *              makes -- and that one is case-INsensitive in cjext
 *              (cjext.c:2444 passes case_sensitive = false; the case-sensitive
 *              behaviour is the separate cJSON_GetObjectItemCaseSensitive).
 *              So a document spelling the key "SSID" is accepted here, and a
 *              mutant that capitalised it stayed green. That is correct rather
 *              than a gap: the firmware would accept it too, and a check
 *              stricter than the firmware would be asserting a rule the
 *              product does not have. What it does catch is a key the lookup
 *              cannot find at all, which the `network` mutant confirms.
 *   top level  same as cmd:3 and unlike cmd:7 -- the handler reads ssid and
 *              pass off the received root and never looks for `data`.
 *   length     the example password must be one this firmware would accept.
 *              ezpi_service_uart_set_wifi() refuses a password shorter than
 *              EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN *before* trying to associate,
 *              so a doc showing a 5-character placeholder would be a doc whose
 *              example is rejected on length with no attempt made. The minimum
 *              is read out of the firmware's own header at runtime.
 *
 * The example is also required to carry nothing but those three members. A
 * reader who pastes `dev_detail` in beside the credentials gets a document
 * that is accepted, stores WiFi, and silently configures no devices -- the
 * handler simply does not look at anything else. */
static void test_the_wifi_block_is_what_the_firmware_accepts(cJSON *wifi,
                                                             const char *userconfig)
{
    cJSON *const cj_cmd = cJSON_GetObjectItem(__FUNCTION__, wifi, ezlopi_cmd_str);
    cJSON *const cj_data = cJSON_GetObjectItem(__FUNCTION__, wifi, ezlopi_data_str);
    cJSON *const cj_ssid = cJSON_GetObjectItem(__FUNCTION__, wifi, ezlopi_ssid_str);
    cJSON *const cj_pass = cJSON_GetObjectItem(__FUNCTION__, wifi, ezlopi_pass_str);
    unsigned long min_len = 0ul;
    int members = 0;
    cJSON *it;

    CHECK((NULL != cj_cmd) && cJSON_IsNumber(cj_cmd),
          "the wifi block carries a numeric cmd");

    if ((NULL != cj_cmd) && cJSON_IsNumber(cj_cmd))
    {
        if (EZPI_UART_CMD_WIFI != cj_cmd->valueint)
        {
            printf("      doc says cmd %d, EZPI_UART_CMD_WIFI is %d"
                   " (EZPI_UART_CMD_SET_CONFIG is %d)\n",
                   cj_cmd->valueint, (int)EZPI_UART_CMD_WIFI,
                   (int)EZPI_UART_CMD_SET_CONFIG);
        }
        CHECK(EZPI_UART_CMD_WIFI == cj_cmd->valueint,
              "the wifi block's cmd is EZPI_UART_CMD_WIFI, from the firmware's enum");
    }

    CHECK(NULL == cj_data,
          "the wifi block has no data wrapper -- ssid and pass sit at the top level");

    CHECK((NULL != cj_ssid) && cJSON_IsString(cj_ssid) &&
              (NULL != cj_ssid->valuestring) && ('\0' != cj_ssid->valuestring[0]),
          "the wifi block carries a non-empty string ssid, under the firmware's own key");

    CHECK((NULL != cj_pass) && cJSON_IsString(cj_pass) &&
              (NULL != cj_pass->valuestring),
          "the wifi block carries a string pass, under the firmware's own key");

    if (read_define_u(userconfig, "EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN", &min_len) &&
        (NULL != cj_pass) && cJSON_IsString(cj_pass) && (NULL != cj_pass->valuestring))
    {
        const size_t len = strlen(cj_pass->valuestring);

        if (len < (size_t)min_len)
        {
            printf("      the example password is %lu character(s); this firmware"
                   " refuses anything under %lu without trying to associate\n",
                   (unsigned long)len, min_len);
        }
        CHECK(len >= (size_t)min_len,
              "the example password clears EZPI_CORE_WIFI_PASS_CHAR_MIN_LEN,"
              " read from the firmware's header");
    }

    for (it = wifi->child; NULL != it; it = it->next)
    {
        members++;
    }
    if (3 != members)
    {
        printf("      the wifi block has %d member(s); it must carry cmd, ssid and"
               " pass and nothing else\n",
               members);
    }
    CHECK(3 == members,
          "the wifi block carries only cmd, ssid and pass -- the handler reads nothing else");
}

int main(void)
{
    char *doc = slurp(DOC_PATH);
    char *userconfig = slurp(USERCONFIG_PATH);
    char *config_block = NULL;
    char *shape_block = NULL;
    char *serial_block = NULL;
    char *wifi_block = NULL;
    cJSON *config = NULL;
    cJSON *shape = NULL;
    cJSON *serial = NULL;
    cJSON *wifi = NULL;

    if (NULL != doc)
    {
        config_block = extract_block(doc, MARKER_CONFIG);
        shape_block = extract_block(doc, MARKER_SHAPE);
        serial_block = extract_block(doc, MARKER_SERIAL);
        wifi_block = extract_block(doc, MARKER_WIFI);
    }

    if (NULL != config_block)
    {
        config = parse_block(config_block, "strip-config");
    }
    if (NULL != shape_block)
    {
        shape = parse_block(shape_block, "strip-config-resolves-to");
    }
    if (NULL != serial_block)
    {
        serial = parse_block(serial_block, "strip-config-over-serial");
    }
    if (NULL != wifi_block)
    {
        wifi = parse_block(wifi_block, "wifi-over-serial");
    }

    /* Outside the `if (NULL != config)` below on purpose: the WiFi block is not
     * derived from the worked example, so a broken config block must not take
     * these checks silently out of the run. */
    if (NULL != wifi)
    {
        test_the_wifi_block_is_what_the_firmware_accepts(wifi, userconfig);
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
        if (NULL != serial)
        {
            test_the_serial_example_is_the_same_document(serial, config);
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
    if (NULL != serial)
    {
        cJSON_Delete(__FUNCTION__, serial);
    }
    if (NULL != wifi)
    {
        cJSON_Delete(__FUNCTION__, wifi);
    }
    free(config_block);
    free(shape_block);
    free(serial_block);
    free(wifi_block);
    free(userconfig);
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
