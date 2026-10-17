/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    test_cjsongpio.c
 * @brief   Host tests for the pin number a device configuration can name.
 *
 * WHAT THIS IS FOR
 *
 * EZPI_core_cjson_get_value_gpio() is the one place a GPIO number crosses from
 * a device configuration into the firmware. 128 call sites take what it returns
 * and hand it to GPIO_IS_VALID_GPIO(), which is
 *
 *     ((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0
 *
 * -- a shift by that number. Before the bound this file tests, the function
 * returned whatever number the configuration carried, so a shift count outside
 * 0..63 was reachable from a stored JSON document. That is undefined, and the
 * way it actually failed was not a rejection: the count truncates to the
 * register width, so pin 70 passed the valid-pin test as GPIO 6 -- the SPI
 * flash clock on a standard esp32 module.
 *
 * WHY THE TEST IS HERE AND NOT ON A BOARD
 *
 * The on-target version of this experiment is "flash a configuration naming pin
 * 70 and see whether the module comes back". Here the same question is a few
 * milliseconds, and the sanitizers turn it into a diagnostic rather than a
 * module that does not boot.
 *
 * WHAT THIS FILE WAS MEASURED TO CATCH
 *
 * It was run against a subject broken on purpose, in both directions, before
 * being trusted:
 *
 *   - Range check removed, which is the aliasing defect itself: 30 of the
 *     checks below fail. The table carries the weight here, not the sanitizer,
 *     which is the right way round -- the test fails with SANITIZERS= too.
 *   - The 255 clause removed, which is the opposite failure: a configuration
 *     that works on a board today quietly loses a pin. Caught by the one table
 *     row that asserts 255 survives unchanged.
 *
 * Measuring that second direction is the point. The changelog records a
 * previous attempt in this area being withdrawn because it would have refused a
 * normal two-sensor I2C configuration; a test that only checks that bad input
 * is rejected cannot notice that kind of mistake.
 *
 * THE BOUND IS PER TARGET, AND SO IS THIS FILE
 *
 * GPIO_NUM_MAX is 40 on esp32, 49 on the esp32s3, 22 on the esp32c3. Nothing
 * below hard-codes a pin count: the expectations are written against
 * GPIO_NUM_MAX itself, and stubs/driver/gpio.h supplies 49 here. A test that
 * said "48 is accepted" would pass on this stub and be wrong for the chip most
 * of these boards are.
 *
 * WHY THERE IS A COPY OF THE OLD FUNCTION IN HERE
 *
 * test_the_test_can_fail() runs a replica of the body this function had before
 * the bound, and asserts it returns 70 for {"gpio":70}. Without it, a harness
 * that silently stopped reaching the subject would report a clean pass for the
 * same reason a fixed subject does. The replica is fed one value and only one:
 * it performs the narrowing cast unguarded, so handing it the non-finite case
 * would be undefined behaviour committed by the test itself.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"
#include "driver/gpio.h"
#include "ezlopi_core_cjson_macros.h"

/*******************************************************************************
 *                          Constants the subject links against
 *
 * ezlopi_core_cjson_macros.c uses one string from ezlopi_cloud_constants.h, in
 * EZPI_core_cjson_trace(). Defining it is cheaper than linking
 * ezlopi_cloud_keywords.c, which defines several hundred others -- the same
 * arrangement test_devcfgval.c and test_mqtt_api.c use.
 *******************************************************************************/
const char *ezlopi__str = "";

/*******************************************************************************
 *                          The flash/PSRAM pin set, chosen by this file
 *
 * ezlopi_core_cjson_macros.c calls EZPI_core_pin_is_mspi() to find out whether
 * a pin is one the chip is using for flash. On a target that resolves to
 * ezlopi_core_pin_reserved.c, which asks the chip over esp_mspi_get_io(). That
 * file is not in this rule's source list, and the definition below takes its
 * place -- the link seam that stubs/driver/gpio.h describes as the intended
 * arrangement: "any source that actually drives a pin will fail to link rather
 * than silently appear to work".
 *
 * The seam is here rather than in a stub header because it buys something a
 * header stub does not: the reserved set becomes an input to the test. The real
 * set is in efuse and differs per part, so "does the policy refuse a flash pin"
 * is only answerable on a host if the set can be chosen.
 *
 * It defaults to EMPTY, and that matters for the table below. Every expectation
 * in pin_cases[] predates this and means "what the bound does", not "what the
 * reservation does" -- including {"6", 6}, which on a real esp32 would now come
 * back -1. Defaulting to empty keeps those cases testing the one thing they
 * were written for, and test_a_flash_pin_is_refused() sets a set explicitly.
 *******************************************************************************/

#include "ezlopi_core_pin_reserved.h"

#define HOST_MSPI_MAX (8)

static int s_host_mspi_pins[HOST_MSPI_MAX];
static unsigned s_host_mspi_count;

/** @brief Choose the pin set the subject will see. NULL/0 clears it. */
static void set_mspi_pins(const int *pins, unsigned count)
{
    if (count > HOST_MSPI_MAX)
    {
        printf("  FATAL: this test file asked for %u reserved pins, max %d\n",
               count, HOST_MSPI_MAX);
        exit(2);
    }

    s_host_mspi_count = count;

    for (unsigned i = 0; i < count; i++)
    {
        s_host_mspi_pins[i] = pins[i];
    }
}

bool EZPI_core_pin_is_mspi(int gpio_num)
{
    for (unsigned i = 0; i < s_host_mspi_count; i++)
    {
        if (s_host_mspi_pins[i] == gpio_num)
        {
            return true;
        }
    }

    return false;
}

/*******************************************************************************
 *                          Test harness
 *******************************************************************************/

static int checks = 0;
static int failures = 0;

static void ok(int condition, const char *what)
{
    checks++;

    if (!condition)
    {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/**
 * @brief   Parse a literal, or give up loudly.
 *
 * A typo in this file's own JSON parses to NULL, and the subject answers -1 for
 * a NULL tree -- which is the same answer most of the cases below expect, so a
 * broken case would turn into a passing one. Exit 2 keeps "this file is wrong"
 * apart from "the subject is wrong".
 */
static cJSON *must_parse(const char *json)
{
    cJSON *root = cJSON_Parse(__FUNCTION__, json);

    if (NULL == root)
    {
        printf("  FATAL: this test file's own JSON does not parse: %s\n", json);
        exit(2);
    }

    return root;
}

/** @brief Read the "gpio" member of a JSON literal through the real function. */
static int read_gpio(const char *json)
{
    cJSON *root = must_parse(json);
    int pin = EZPI_core_cjson_get_value_gpio(root, "gpio");
    cJSON_Delete(__FUNCTION__, root);

    return pin;
}

/** @brief read_gpio() for a numeric value, written out for us. */
static int read_pin_number(const char *number_text)
{
    char json[64];

    snprintf(json, sizeof(json), "{\"gpio\":%s}", number_text);

    return read_gpio(json);
}

/** @brief The value the function returns for "there is no pin here". */
#define NO_PIN (-1)

/** @brief The second spelling of "no pin", which the drivers test by hand. */
#define UNUSED_255 (255)

/*******************************************************************************
 *                          The numbers a configuration can carry
 *
 * One table, three tests: the per-case expectation, the shift that every call
 * site performs, and the returned-value invariant. Keeping them one table is
 * what stops a value being added here and checked in only one of the three.
 *******************************************************************************/

typedef struct s_pin_case
{
    const char *text; /**< the number as it appears in the document */
    int want;         /**< what the function must return */
    const char *what; /**< for the failure message */
} s_pin_case_t;

static const s_pin_case_t pin_cases[] = {
    /* The aliasing cases, measured against the esp32 mask before the fix:
     * every one of these was accepted by GPIO_IS_VALID_GPIO and named a real
     * pin. The comment above the function records which. */
    {"70", NO_PIN, "pin 70, which aliased onto GPIO 6 -- the flash clock"},
    {"64", NO_PIN, "pin 64, which aliased onto GPIO 0"},
    {"72", NO_PIN, "pin 72, which aliased onto GPIO 8"},
    {"100", NO_PIN, "pin 100, which aliased onto GPIO 36"},
    {"128", NO_PIN, "pin 128"},
    {"4096", NO_PIN, "pin 4096"},
    {"2147483647", NO_PIN, "INT_MAX as a pin number"},

    /* Out of range downwards. -1 is a sentinel and is covered below; nothing
     * else negative is. */
    {"-2", NO_PIN, "pin -2"},
    {"-64", NO_PIN, "pin -64"},
    {"-2147483648", NO_PIN, "INT_MIN as a pin number"},

    /* Beyond what a double can hold as an int at all. cJSON parses numbers
     * with strtod, so these arrive as finite-but-huge or as inf, and the
     * narrowing cast the old body performed was undefined for both. */
    {"1e30", NO_PIN, "a pin number of 1e30"},
    {"-1e30", NO_PIN, "a pin number of -1e30"},
    {"1e999", NO_PIN, "a pin number that overflows a double to +inf"},
    {"-1e999", NO_PIN, "a pin number that overflows a double to -inf"},

    /* The two sentinels. Both are out-of-range shift counts and both are left
     * alone on purpose: a configuration using either works on a board today,
     * and the drivers test for them by hand. */
    {"-1", NO_PIN, "the -1 sentinel"},
    {"255", UNUSED_255, "the 255 sentinel, which must survive unchanged"},

    /* In range, and therefore returned as written. Whether the pin is usable is
     * still mostly the callers' question -- GPIO 24 is absent from the esp32
     * valid-pin mask and nothing here second-guesses that.
     *
     * GPIO 6 is the exception worth reading carefully. It is the esp32 flash
     * clock, and on a board it now comes back -1, because EZPI_core_pin_is_mspi()
     * says so. It is 6 here because this file's reserved set defaults to empty,
     * which is what keeps these rows testing the bound rather than the
     * reservation. The reservation has its own test, and the pairing is the
     * point: the same input gives 6 with an empty set and -1 with a set
     * containing 6, so each test is measuring only its own subject. */
    {"0", 0, "GPIO 0"},
    {"5", 5, "GPIO 5"},
    {"12", 12, "GPIO 12"},
    {"6", 6, "GPIO 6 with no pins reserved -- the bound alone lets it through"},
    {"24", 24, "GPIO 24, in range and absent from the esp32 mask"},
    {"39", 39, "GPIO 39, the last pin on an esp32"},

    /* Not numbers at all. Each of these is a shape the Ezlo app or a
     * hand-edited configuration can produce, and each answered -1 before this
     * change too -- they are here so a future rewrite of the function cannot
     * start returning something else for them unnoticed. */
    {"\"12\"", NO_PIN, "a pin number written as a string"},
    {"\"\"", NO_PIN, "an empty string, one of the four spellings of unused"},
    {"null", NO_PIN, "a null pin"},
    {"true", NO_PIN, "a boolean pin"},
    {"[12]", NO_PIN, "a pin in an array"},
    {"{\"n\":12}", NO_PIN, "a pin in an object"},
};

#define PIN_CASE_COUNT (sizeof(pin_cases) / sizeof(pin_cases[0]))

/*******************************************************************************
 *                          Tests
 *******************************************************************************/

/** @brief Every value in the table produces the documented answer. */
static void test_the_table(void)
{
    for (unsigned i = 0; i < PIN_CASE_COUNT; i++)
    {
        int got = read_pin_number(pin_cases[i].text);

        if (got != pin_cases[i].want)
        {
            printf("  (%s: wanted %d, got %d)\n",
                   pin_cases[i].text, pin_cases[i].want, got);
        }

        ok(got == pin_cases[i].want, pin_cases[i].what);
    }
}

/**
 * @brief   The point of the whole change: do what the call sites do.
 *
 * GPIO_IS_VALID_GPIO is a shift by the returned value. This runs that shift for
 * real, for every value the table can produce. With the bound removed from the
 * subject the sanitizer says "shift exponent 70 is too large for 64-bit type",
 * naming the defect in the words the firmware never gets to use on a board.
 *
 * That is a second opinion, not the verdict. The table above already fails on
 * its own when the bound goes, which is what makes this suite's result
 * independent of whether the sanitizers are compiled in -- see the file header.
 *
 * The two sentinels are excluded, and that exclusion is the honest part of this
 * test rather than a hole in it: -1 and 255 are deliberately passed through, so
 * they remain out-of-range shift counts at the call sites. Both were checked
 * against the xtensa and riscv32 compilers at -O2 and both fold to 0, which is
 * the answer the callers need; that is recorded where the decision was made and
 * cannot be re-measured from a host.
 */
static void test_the_shift_is_defined(void)
{
    unsigned long long accumulated = 0u;
    int shifted = 0;

    for (unsigned i = 0; i < PIN_CASE_COUNT; i++)
    {
        int pin = read_pin_number(pin_cases[i].text);

        if ((NO_PIN == pin) || (UNUSED_255 == pin))
        {
            continue;
        }

        /* Exactly the expression the macro expands to, with a runtime value.
         * Accumulated so the optimiser cannot discard the shift. */
        accumulated |= (1ULL << pin);
        shifted++;
    }

    ok(shifted > 0, "the shift was actually exercised, not skipped away");
    ok(0u != accumulated, "and produced a bit pattern, so it really ran");
}

/**
 * @brief   The invariant, over a wider sweep than the table.
 *
 * Every value the function can return is -1, 255, or a pin that exists on this
 * target. Swept rather than tabulated because the interesting boundary is
 * GPIO_NUM_MAX, and that moves between targets -- so the sweep is written
 * around it and crosses it wherever it happens to be.
 */
static void test_the_invariant(void)
{
    int violations = 0;
    int in_range_seen = 0;

    for (int n = -600; n <= 600; n++)
    {
        char text[16];

        snprintf(text, sizeof(text), "%d", n);

        int pin = read_pin_number(text);

        if ((NO_PIN == pin) || (UNUSED_255 == pin))
        {
            continue;
        }

        if ((pin < 0) || (pin >= (int)GPIO_NUM_MAX))
        {
            printf("  (gpio %d came back as %d, which is not a pin on this "
                   "target)\n", n, pin);
            violations++;
        }
        else
        {
            in_range_seen++;
        }
    }

    ok(0 == violations,
       "no document number comes back as a pin that does not exist");
    ok(in_range_seen == (int)GPIO_NUM_MAX,
       "and every pin that does exist comes back, so the bound is not too tight");
}

/** @brief The boundary itself, stated once in terms of GPIO_NUM_MAX. */
static void test_the_boundary(void)
{
    char text[16];

    snprintf(text, sizeof(text), "%d", (int)GPIO_NUM_MAX - 1);
    ok(((int)GPIO_NUM_MAX - 1) == read_pin_number(text),
       "the highest pin on this target is accepted");

    snprintf(text, sizeof(text), "%d", (int)GPIO_NUM_MAX);
    ok(NO_PIN == read_pin_number(text),
       "GPIO_NUM_MAX itself is one past the end and is refused");
}

/**
 * @brief   A pin the chip is using for flash comes back unused.
 *
 * The defect: GPIO_IS_VALID_GPIO is
 *
 *     ((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0
 *
 * and the mask excludes 24 and 28..31 on an esp32, 22..25 on the s2 and s3, and
 * nothing at all on the c3 -- so it never excluded a flash pin on any target.
 * A configuration naming GPIO 6 passed every test in the tree and had a driver
 * configure the SPI flash clock as a push-pull output. The masks were read out
 * of soc_caps.h with the compiler, not by eye.
 *
 * WHY THE SECOND SET IS THE IMPORTANT ONE
 *
 * The obvious fix is a table: "6..11 are flash on esp32". The second case here
 * is why that is wrong. soc/spi_pins.h for the esp32 carries a whole second set
 * of pin numbers, SPI_D2WD_PIN_NUM_* -- 17, 8, 6, 16, 7, 11 -- because parts
 * with embedded flash remap the bus, and the bootloader chooses between the two
 * by reading efuse. On such a part a table would refuse 9 and 10, which are
 * free, and allow 16 and 17, which are flash. That is wrong in both directions
 * at once, and the expensive one is refusing pins that work.
 *
 * So the subject asks the chip instead, and this test asks it two different
 * things to show the answer is not baked in.
 */
static void test_a_flash_pin_is_refused(void)
{
    /* The standard esp32 arrangement: SPI_IOMUX_PIN_NUM_* from
     * soc/esp32/include/soc/spi_pins.h. */
    static const int esp32_default_flash[] = {6, 7, 8, 9, 10, 11};

    /* The embedded-flash arrangement on the same chip: SPI_D2WD_PIN_NUM_*. */
    static const int esp32_d2wd_flash[] = {17, 8, 6, 16, 7, 11};

    set_mspi_pins(esp32_default_flash, 6);

    ok(NO_PIN == read_pin_number("6"), "GPIO 6, the flash clock, is refused");
    ok(NO_PIN == read_pin_number("7"), "GPIO 7, flash MISO, is refused");
    ok(NO_PIN == read_pin_number("8"), "GPIO 8, flash MOSI, is refused");
    ok(NO_PIN == read_pin_number("9"), "GPIO 9, flash HD, is refused");
    ok(NO_PIN == read_pin_number("10"), "GPIO 10, flash WP, is refused");
    ok(NO_PIN == read_pin_number("11"), "GPIO 11, flash CS, is refused");

    /* Immediately either side of the block. A reservation that caught these
     * would be refusing working configurations, which is the failure this area
     * already has a withdrawn attempt recorded for. */
    ok(5 == read_pin_number("5"), "GPIO 5, just below the block, still works");
    ok(12 == read_pin_number("12"), "GPIO 12, just above it, still works");
    ok(0 == read_pin_number("0"),
       "GPIO 0 still works -- a decoded 0 means 'unset', not this pin");

    /* Neither sentinel denotes a pin, so neither is asked the question. */
    ok(NO_PIN == read_pin_number("-1"), "the -1 sentinel is untouched");
    ok(UNUSED_255 == read_pin_number("255"), "the 255 sentinel is untouched");

    /* The remap. Same chip, same firmware, different efuse. */
    set_mspi_pins(esp32_d2wd_flash, 6);

    ok(NO_PIN == read_pin_number("16"),
       "on an embedded-flash part GPIO 16 is flash and is refused");
    ok(NO_PIN == read_pin_number("17"),
       "and so is GPIO 17 -- a 6..11 table would have allowed both");
    ok(9 == read_pin_number("9"),
       "while GPIO 9 is free on that part and is allowed");
    ok(10 == read_pin_number("10"),
       "and so is GPIO 10 -- a 6..11 table would have refused both");

    /* The control, and the reason this test is believable: clear the set and
     * the same input comes back. Without this, a subject that had simply
     * acquired a second bound at 6 -- or a read_pin_number() that had stopped
     * reaching the subject at all -- would pass every assertion above. */
    set_mspi_pins(NULL, 0);

    ok(6 == read_pin_number("6"),
       "with nothing reserved GPIO 6 comes back, so the refusals above are the "
       "reserved set and not a new bound");
    ok(16 == read_pin_number("16"), "and so does GPIO 16");

    /* Left empty deliberately: test_the_invariant() asserts that every pin on
     * the target comes back, which a leftover reserved set would break. */
    ok(0 == s_host_mspi_count, "and the set is left clear for the other tests");
}

/**
 * @brief   A missing key, and a tree that is not there.
 *
 * Most pin keys are absent from most device entries -- a one-wire sensor has no
 * gpio_sda -- so this is the common path, not an edge case. It must answer the
 * same thing an unusable number now answers, which is what makes the change
 * safe: a bad pin number disables one pin instead of rejecting a document.
 */
static void test_an_absent_pin(void)
{
    ok(NO_PIN == read_gpio("{\"dev_type\":1}"), "a missing gpio key is -1");
    ok(NO_PIN == read_gpio("{}"), "an empty object is -1");
    ok(NO_PIN == EZPI_core_cjson_get_value_gpio(NULL, "gpio"),
       "a NULL tree is -1 rather than a crash");
}

/**
 * @brief   Fractional numbers truncate, as they always did.
 *
 * JSON has one number type, so 12 and 12.0 are indistinguishable by the time
 * they reach here and 12.7 is legal input. Truncation is the behaviour the
 * firmware has shipped with; it is asserted so that it is a decision rather
 * than something nobody looked at.
 */
static void test_a_fractional_pin(void)
{
    ok(12 == read_pin_number("12.0"), "12.0 is GPIO 12");
    ok(12 == read_pin_number("12.7"), "12.7 truncates to GPIO 12");
    ok(0 == read_pin_number("0.5"), "0.5 truncates to GPIO 0");
    ok(NO_PIN == read_pin_number("-0.5"),
       "-0.5 truncates towards zero but is not >= 0, so it is refused");
    ok(NO_PIN == read_pin_number("-1.5"),
       "-1.5 is not the -1 sentinel and is refused");
    ok(NO_PIN == read_pin_number("255.5"),
       "255.5 is not the 255 sentinel and is refused");
}

/**
 * @brief   Case folding, because the boot parser folds case.
 *
 * cJSON_GetObjectItem is case insensitive and ezlopi_core_devices.c reads the
 * configuration through it, so "GPIO" names a pin on a board today. If this
 * function were ever given a case-sensitive lookup, a working configuration
 * would stop having pins and the devices would come up inert.
 */
static void test_the_key_lookup_folds_case(void)
{
    ok(12 == read_gpio("{\"GPIO\":12}"), "an upper-case key names the pin");
    ok(12 == read_gpio("{\"Gpio\":12}"), "a mixed-case key names the pin");
    ok(NO_PIN == read_gpio("{\"GPIO\":70}"),
       "and an upper-case key gets the bound too");
}

/**
 * @brief   The tree is left alone.
 *
 * The function is a reader. It is called while the boot parser is walking the
 * configuration, and the configuration is re-read for other keys afterwards, so
 * clamping by writing the value back into the node would be a different and
 * much worse change than the one that was made.
 */
static void test_the_tree_is_not_modified(void)
{
    cJSON *root = must_parse("{\"gpio\":70,\"gpio1\":255,\"gpio2\":12}");

    char *before = cJSON_PrintUnformatted(__FUNCTION__, root);

    (void)EZPI_core_cjson_get_value_gpio(root, "gpio");
    (void)EZPI_core_cjson_get_value_gpio(root, "gpio1");
    (void)EZPI_core_cjson_get_value_gpio(root, "gpio2");

    char *after = cJSON_PrintUnformatted(__FUNCTION__, root);

    ok((NULL != before) && (NULL != after) && (0 == strcmp(before, after)),
       "reading a pin does not change the document");

    free(before);
    free(after);
    cJSON_Delete(__FUNCTION__, root);
}

/**
 * @brief   A replica of the body this function had before the bound.
 *
 * Fed 70 and nothing else. It casts the double without checking it, which is
 * undefined for the non-finite values the table above includes -- so widening
 * what goes in here would mean the test file commits the defect it is testing
 * for.
 */
static int legacy_get_value_gpio(cJSON *cj_root, const char *item_name_str)
{
    int item_val = -1;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);

    if (o_item && (o_item->type & cJSON_Number))
    {
        item_val = (int)o_item->valuedouble;
    }

    return item_val;
}

/**
 * @brief   Show that this file can fail before believing that it passes.
 *
 * If the harness stopped reaching the subject -- a stale object file, a JSON
 * literal that quietly stopped parsing, a key that no longer matches -- every
 * case above would read -1 and the suite would be green. The old body is the
 * control: run on the same path, through the same parser, it must still give
 * back 70.
 */
static void test_the_test_can_fail(void)
{
    cJSON *root = must_parse("{\"gpio\":70}");

    ok(70 == legacy_get_value_gpio(root, "gpio"),
       "the pre-fix body still returns 70, so these cases reach a real decision");
    ok(NO_PIN == EZPI_core_cjson_get_value_gpio(root, "gpio"),
       "and the shipped body does not");

    cJSON_Delete(__FUNCTION__, root);
}

/**
 * @brief   The accessor is one function serving fifteen key names.
 *
 * The fix is at the read, not at the call sites, and the reason that is worth
 * doing is that one bound covers every key. Reading several of them here is
 * cheap and makes that claim a test rather than a sentence in a comment.
 */
static void test_every_pin_key(void)
{
    static const char *const keys[] = {
        "gpio", "gpio1", "gpio2", "gpio3", "gpio_in", "gpio_out",
        "gpio_sda", "gpio_scl", "gpio_tx", "gpio_rx"
    };

    for (unsigned i = 0; i < (sizeof(keys) / sizeof(keys[0])); i++)
    {
        char json[64];

        snprintf(json, sizeof(json), "{\"%s\":70}", keys[i]);

        cJSON *root = must_parse(json);
        int pin = EZPI_core_cjson_get_value_gpio(root, keys[i]);
        cJSON_Delete(__FUNCTION__, root);

        if (NO_PIN != pin)
        {
            printf("  (%s: 70 came back as %d)\n", keys[i], pin);
        }

        ok(NO_PIN == pin, keys[i]);
    }
}

int main(void)
{
    /* Line buffered on purpose. This rule builds with -fno-sanitize-recover,
    ** so a sanitizer finding ends the process immediately -- and `make` reads
    ** stdout through a pipe, where the default is to buffer until exit. The
    ** first run of the broken-on-purpose subject printed the sanitizer message
    ** and not one of the thirty FAIL lines that came before it, because those
    ** were still sitting in the buffer. The failure was visible; which checks
    ** failed was not. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== configuration pin numbers (GPIO_NUM_MAX = %d) ===\n",
           (int)GPIO_NUM_MAX);

    test_the_test_can_fail();
    test_the_table();
    test_the_shift_is_defined();
    test_the_invariant();
    test_the_boundary();
    test_a_flash_pin_is_refused();
    test_an_absent_pin();
    test_a_fractional_pin();
    test_the_key_lookup_folds_case();
    test_the_tree_is_not_modified();
    test_every_pin_key();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);

    if (failures)
    {
        printf("CJSON GPIO TESTS FAILED\n");
        return 1;
    }

    printf("CJSON GPIO TESTS PASSED\n");
    return 0;
}
