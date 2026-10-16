/* Host tests for ezlopi-util-localkey.
 *
 * WHY THIS FILE EXISTS
 *
 * The local key is the whole of the offline API's access control. An
 * unauthenticated websocket client gets exactly one method --
 * `hub.offline.login.ui`, gated at ezlopi_service_ws_server.c:292 -- and that
 * method's only check is a comparison of the supplied token against this key.
 * There is no second factor, no rate limit, and no lockout. So the comparison
 * is the thing, and before this component existed it was written like this, in
 * ezlopi_core_offline_login.c:
 *
 *     if (0 == strncmp(password_saved, cj_token->valuestring, strlen(password_saved)))
 *
 * That is a prefix match. Two consequences, and neither is visible to anyone
 * testing the device by logging into it, because the correct token still
 * works:
 *
 *   1. Any token that *begins* with the key authenticates. Appending junk is
 *      accepted.
 *   2. strlen("") is 0, and strncmp(a, b, 0) returns 0. An empty stored key
 *      therefore matches every token, including the empty one.
 *
 * Case 2 is not hypothetical on this hardware. The stored key lives in the
 * factory-info partition and a self-flashed device has never been provisioned,
 * so that region reads back as 0xFF. Today EZPI_core_factory_info_v3_get_local_key()
 * happens to return NULL rather than "" for that case -- it checks
 * isprint(local_key[0]), and isprint(0xFF) is false -- and the NULL is caught.
 * That is a lucky escape rather than a check: the protection against "empty
 * key accepts everything" is a printability test on one byte in a different
 * component, written for a different purpose. Anything that makes the key
 * readable-but-empty (a zero-length NVS entry, a write that stores only the
 * terminator, a future provisioning path) reinstates case 2 in full.
 *
 * HOW THE DEFECT IS DEMONSTRATED RATHER THAN ASSERTED
 *
 * legacy_match() below is that strncmp expression copied verbatim. The checks
 * do not merely confirm the new function rejects a prefix; they confirm the
 * old one *accepted* it, and fail if it did not. If someone decides the
 * concern was imaginary, this file is the argument: run it and read the
 * "legacy accepted" lines.
 *
 * The other half of the file is the part that matters for not breaking
 * anything. A key written by Ezlo cloud provisioning is 64 mixed-case
 * alphanumerics, and it contains the four characters this component omits when
 * generating its own (`l`, `o`, `0`, `1`). EZPI_util_localkey_is_valid() must
 * accept it unchanged, and the real 64-character key from the commented-out
 * line at ezlopi_core_factory_info.c:463 is used as the test vector so the
 * question is settled against an actual provisioned value and not a made-up
 * one.
 */
#include <stdio.h>
#include <string.h>

#include "ezlopi_util_localkey.h"

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

/* The comparison this component replaces, character for character. Kept so the
** tests can show the difference instead of claiming it. */
static int legacy_match(const char *stored, const char *supplied)
{
    return (0 == strncmp(stored, supplied, strlen(stored)));
}

/* A real cloud-provisioned key: the value left in a commented-out line in
** ezlopi_core_factory_info.c, which is what a device configured through the
** Ezlo app actually holds. 64 characters, mixed case, and containing `0`, `O`,
** `1` and `l`-class characters that this component never generates. */
static const char *const provisioned_key =
    "opAUdOPv04SyaVbBzm8VNUg7BrFfaGXC7pxEBAv6evZnmiijcCaUXNLqgYrQASDk";

static void test_prefix_match_is_closed(void)
{
    printf("-- the prefix match --\n");

    const char *stored = provisioned_key;
    char longer[160];

    /* The key with junk appended. */
    snprintf(longer, sizeof(longer), "%s%s", stored, "and-anything-at-all");

    ok(1 == legacy_match(stored, longer),
       "legacy accepted key+suffix (this is the defect; if this fails the "
       "premise is wrong)");
    ok(false == EZPI_util_localkey_match(stored, longer),
       "key+suffix is rejected");

    /* One character appended is enough. */
    snprintf(longer, sizeof(longer), "%sx", stored);
    ok(1 == legacy_match(stored, longer), "legacy accepted key+1 char");
    ok(false == EZPI_util_localkey_match(stored, longer),
       "key+1 char is rejected");

    /* And the correct token still works, which is the point of the whole
    ** exercise -- a fix that locked out provisioned devices would be worse
    ** than the defect. */
    ok(true == EZPI_util_localkey_match(stored, stored),
       "the exact provisioned key still authenticates");

    /* A truncation must not authenticate, and did not before either: strncmp
    ** over strlen(stored) bytes reads past the end of the shorter supplied
    ** string but stops at its terminator, so it differs. Checked so that the
    ** new length test is not silently the only thing rejecting it. */
    char shorter[80];
    snprintf(shorter, sizeof(shorter), "%s", stored);
    shorter[strlen(stored) - 1] = '\0';
    ok(false == EZPI_util_localkey_match(stored, shorter),
       "key minus its last character is rejected");
}

static void test_empty_stored_key_matches_nothing(void)
{
    printf("-- the empty stored key --\n");

    ok(1 == legacy_match("", ""),
       "legacy accepted empty token against empty key");
    ok(1 == legacy_match("", "whatever-i-like"),
       "legacy accepted ANY token against an empty key (this is the defect)");
    ok(1 == legacy_match("", provisioned_key),
       "legacy accepted a valid-looking token against an empty key");

    ok(false == EZPI_util_localkey_match("", ""),
       "empty key rejects empty token");
    ok(false == EZPI_util_localkey_match("", "whatever-i-like"),
       "empty key rejects arbitrary token");
    ok(false == EZPI_util_localkey_match("", provisioned_key),
       "empty key rejects a valid-looking token");

    /* Short-but-nonempty too. A 1-byte stored key would otherwise be a
    ** credential with 94 possibilities. */
    ok(false == EZPI_util_localkey_match("a", "a"),
       "a 1-character key does not authenticate even when matched exactly");
    ok(false == EZPI_util_localkey_match("abcdefg", "abcdefg"),
       "a 7-character key is below the floor and does not authenticate");
    ok(true == EZPI_util_localkey_match("abcdefgh", "abcdefgh"),
       "an 8-character key is at the floor and does authenticate");
}

static void test_match_rejects_null(void)
{
    printf("-- NULL handling --\n");

    /* legacy_match is not called with NULL here: strlen(NULL) is a crash, not
    ** a wrong answer, which is itself worth noting -- the old code was only
    ** safe because its caller checked for NULL first. The new function does
    ** not rely on that. */
    ok(false == EZPI_util_localkey_match(NULL, provisioned_key),
       "NULL stored key rejects");
    ok(false == EZPI_util_localkey_match(provisioned_key, NULL),
       "NULL supplied token rejects");
    ok(false == EZPI_util_localkey_match(NULL, NULL),
       "both NULL rejects");
}

static void test_validation(void)
{
    printf("-- validation --\n");

    ok(true == EZPI_util_localkey_is_valid(provisioned_key),
       "a real 64-char cloud-provisioned key is valid");
    ok(64 == strlen(provisioned_key),
       "that key is the 64 characters the cloud writes");

    ok(false == EZPI_util_localkey_is_valid(NULL), "NULL is not valid");
    ok(false == EZPI_util_localkey_is_valid(""), "empty is not valid");
    ok(false == EZPI_util_localkey_is_valid("short"), "5 chars is not valid");
    ok(false == EZPI_util_localkey_is_valid("abcdefg"), "7 chars is not valid");
    ok(true == EZPI_util_localkey_is_valid("abcdefgh"), "8 chars is valid");

    /* Characters the generator omits must still validate, or provisioned
    ** devices break. This is the check that stops someone "tidying up"
    ** is_valid() into an alphabet test. */
    ok(true == EZPI_util_localkey_is_valid("l0O1lOo0abcdefgh"),
       "l/0/O/1/o are accepted even though generation never emits them");
    ok(true == EZPI_util_localkey_is_valid("!@#$%^&*()_+-=[]{}"),
       "punctuation is accepted: this is a length-and-printability rule");

    ok(false == EZPI_util_localkey_is_valid("has a space here"),
       "a space is rejected");
    ok(false == EZPI_util_localkey_is_valid("has\ttab\there"),
       "a tab is rejected");
    ok(false == EZPI_util_localkey_is_valid("has\nnewline\there"),
       "a newline is rejected");

    /* 0xFF is what an erased flash byte reads as. A key that is all-0xFF must
    ** not validate; that is the blank-factory-info case. */
    char erased[33];
    memset(erased, (char)0xFF, sizeof(erased) - 1);
    erased[sizeof(erased) - 1] = '\0';
    ok(false == EZPI_util_localkey_is_valid(erased),
       "a key of erased 0xFF bytes is not valid");

    /* Too long. */
    char overlong[EZPI_LOCALKEY_MAX_LEN + 10];
    memset(overlong, 'a', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';
    ok(false == EZPI_util_localkey_is_valid(overlong),
       "longer than EZPI_LOCALKEY_MAX_LEN is not valid");

    char at_max[EZPI_LOCALKEY_MAX_LEN + 1];
    memset(at_max, 'a', sizeof(at_max) - 1);
    at_max[sizeof(at_max) - 1] = '\0';
    ok(true == EZPI_util_localkey_is_valid(at_max),
       "exactly EZPI_LOCALKEY_MAX_LEN is valid");
}

static void test_generation(void)
{
    printf("-- generation --\n");

    uint8_t entropy[EZPI_LOCALKEY_MAX_LEN];
    char key[EZPI_LOCALKEY_MAX_LEN + 1];

    ok(EZPI_LOCALKEY_GEN_LEN == EZPI_util_localkey_entropy_needed(EZPI_LOCALKEY_GEN_LEN),
       "one entropy byte per character");

    /* All-zero entropy: every character is alphabet[0]. Not a key anyone would
    ** want, and exactly what should come out -- generation is a rendering of
    ** the bytes handed in, with no hidden state. */
    memset(entropy, 0x00, sizeof(entropy));
    ok(true == EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                           EZPI_LOCALKEY_GEN_LEN,
                                           key, sizeof(key)),
       "generate succeeds with zero entropy bytes");
    ok(EZPI_LOCALKEY_GEN_LEN == strlen(key), "generated key is 32 characters");
    ok(0 == strcmp(key, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
       "zero bytes map to the first alphabet symbol throughout");
    ok(true == EZPI_util_localkey_is_valid(key),
       "a generated key passes validation");

    /* Known vector. 0x00..0x1f selects each alphabet symbol exactly once, in
    ** order, which pins the alphabet itself: if a character is ever changed,
    ** added or reordered this string stops matching. */
    for (size_t i = 0; i < 32; i++)
    {
        entropy[i] = (uint8_t)i;
    }
    ok(true == EZPI_util_localkey_generate(entropy, 32, 32, key, sizeof(key)),
       "generate succeeds for the alphabet-sweep vector");
    ok(0 == strcmp(key, "abcdefghijkmnpqrstuvwxyz23456789"),
       "0x00..0x1f renders the alphabet in order (pins the 32 symbols)");

    /* Only the low five bits are used, so the top three must not change the
    ** output. This is the property that makes the mapping unbiased, and it is
    ** the one a reader cannot check by eye. */
    uint8_t high[32];
    char key_high[64];
    for (size_t i = 0; i < 32; i++)
    {
        high[i] = (uint8_t)(i | 0xE0u);
    }
    ok(true == EZPI_util_localkey_generate(high, 32, 32, key_high, sizeof(key_high)),
       "generate succeeds with the high bits set");
    ok(0 == strcmp(key, key_high),
       "the top three bits of each entropy byte do not affect the output");

    /* Every byte value must land on a symbol, and the distribution over all
    ** 256 of them must be exactly 8 per symbol -- that is what "unbiased"
    ** means here, and it is cheap to check exhaustively. */
    uint8_t all[256];
    char sweep[257];
    for (size_t i = 0; i < 256; i++)
    {
        all[i] = (uint8_t)i;
    }
    int counts[32];
    memset(counts, 0, sizeof(counts));
    /* 256 is above EZPI_LOCALKEY_MAX_LEN, so this is done in two halves. */
    ok(true == EZPI_util_localkey_generate(all, 128, 128, sweep, sizeof(sweep)),
       "generate succeeds for entropy bytes 0x00-0x7f");
    for (size_t i = 0; i < 128; i++)
    {
        const char *at = strchr("abcdefghijkmnpqrstuvwxyz23456789", sweep[i]);
        if (at)
        {
            counts[at - "abcdefghijkmnpqrstuvwxyz23456789"]++;
        }
        else
        {
            ok(0, "generated character is outside the alphabet");
        }
    }
    ok(true == EZPI_util_localkey_generate(all + 128, 128, 128, sweep, sizeof(sweep)),
       "generate succeeds for entropy bytes 0x80-0xff");
    for (size_t i = 0; i < 128; i++)
    {
        const char *at = strchr("abcdefghijkmnpqrstuvwxyz23456789", sweep[i]);
        if (at)
        {
            counts[at - "abcdefghijkmnpqrstuvwxyz23456789"]++;
        }
        else
        {
            ok(0, "generated character is outside the alphabet");
        }
    }
    int uneven = 0;
    for (size_t i = 0; i < 32; i++)
    {
        if (8 != counts[i])
        {
            uneven++;
        }
    }
    ok(0 == uneven,
       "all 256 byte values spread exactly 8-per-symbol over the 32 symbols");

    /* Two different entropy inputs give two different keys. Guards against a
    ** generate() that ignores its input, which would pass every check above
    ** except the alphabet sweep. */
    char key_a[EZPI_LOCALKEY_GEN_BUF_SIZE];
    char key_b[EZPI_LOCALKEY_GEN_BUF_SIZE];
    memset(entropy, 0x11, sizeof(entropy));
    (void)EZPI_util_localkey_generate(entropy, 32, EZPI_LOCALKEY_GEN_LEN,
                                      key_a, sizeof(key_a));
    memset(entropy, 0x22, sizeof(entropy));
    (void)EZPI_util_localkey_generate(entropy, 32, EZPI_LOCALKEY_GEN_LEN,
                                      key_b, sizeof(key_b));
    ok(0 != strcmp(key_a, key_b), "different entropy gives a different key");
    ok(false == EZPI_util_localkey_match(key_a, key_b),
       "and the two do not match each other");
    ok(true == EZPI_util_localkey_match(key_a, key_a),
       "a generated key matches itself");
}

static void test_generation_refuses_bad_arguments(void)
{
    printf("-- generation argument checks --\n");

    uint8_t entropy[64];
    char key[EZPI_LOCALKEY_GEN_BUF_SIZE];
    memset(entropy, 0x5A, sizeof(entropy));

    /* Buffer exactly one byte too small for the terminator. This is the
    ** off-by-one that would otherwise write the NUL past the end, which ASan
    ** catches here and nothing catches on a device. */
    ok(false == EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                            EZPI_LOCALKEY_GEN_LEN,
                                            key, EZPI_LOCALKEY_GEN_LEN),
       "a buffer with no room for the terminator is refused");
    ok('\0' == key[0], "and the buffer is left empty rather than half-written");

    ok(true == EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                           EZPI_LOCALKEY_GEN_LEN,
                                           key, EZPI_LOCALKEY_GEN_LEN + 1),
       "one more byte is enough");

    /* Not enough entropy for the length asked for. Silently generating a
    ** shorter key, or reusing bytes, would both be worse than refusing. */
    ok(false == EZPI_util_localkey_generate(entropy, 10, EZPI_LOCALKEY_GEN_LEN,
                                            key, sizeof(key)),
       "too few entropy bytes is refused");
    ok('\0' == key[0], "and again the buffer is left empty");

    ok(false == EZPI_util_localkey_generate(NULL, 64, EZPI_LOCALKEY_GEN_LEN,
                                            key, sizeof(key)),
       "NULL entropy is refused");
    ok(false == EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                            EZPI_LOCALKEY_GEN_LEN, NULL, 64),
       "NULL output is refused");
    ok(false == EZPI_util_localkey_generate(entropy, sizeof(entropy), 0,
                                            key, sizeof(key)),
       "a zero-length key is refused");
    ok(false == EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                            EZPI_LOCALKEY_MIN_LEN - 1,
                                            key, sizeof(key)),
       "a key below the minimum length is refused");
    ok(false == EZPI_util_localkey_generate(entropy, sizeof(entropy),
                                            EZPI_LOCALKEY_MAX_LEN + 1,
                                            key, sizeof(key)),
       "a key above the maximum length is refused");
}

int main(void)
{
    printf("ezlopi-util-localkey host tests\n\n");

    test_prefix_match_is_closed();
    test_empty_stored_key_matches_nothing();
    test_match_rejects_null();
    test_validation();
    test_generation();
    test_generation_refuses_bad_arguments();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    /* Deliberately NOT tested here:
     *
     *   - That the comparison is constant-time. The loop has no early exit and
     *     reads every byte, which is the structural property worth having, but
     *     timing it on a host under ASan would measure the host's scheduler
     *     rather than the code. Compilers are also permitted to vectorise the
     *     loop; the guarantee here is "no data-dependent branch in the
     *     comparison", not a measured constant duration. The length check
     *     ahead of the loop leaks the key length by design -- see the header.
     *
     *   - Where the entropy comes from. On a device it is esp_fill_random(),
     *     which cannot be reached from a host, and that is the point of taking
     *     the bytes as an argument. What would NOT be acceptable is
     *     srand(time(NULL))/rand(): a key from that source is guessable to
     *     within whichever second the device first booted. That used to be how
     *     ezlopi_util_uuid_generate_random() worked and no longer is, so there
     *     is now no rand()-based source in the tree to confuse this with. The
     *     wiring in ezlopi_core_factory_info.c is where the choice lives and
     *     where it has to be read.
     *
     *   - Whether the key is actually persisted and reloaded. That needs NVS
     *     and the factory-info partition, neither of which exists on a host.
     *     Covered on hardware only, and recorded as validation debt.
     *
     *   - Rate limiting on login attempts. There is none. A 160-bit key makes
     *     online guessing irrelevant, but the absence is a real property of
     *     the local API and is not addressed by this component.
     */
    if (g_fails)
    {
        printf("LOCALKEY TESTS FAILED\n");
        return 1;
    }
    printf("LOCALKEY TESTS PASSED\n");
    return 0;
}
