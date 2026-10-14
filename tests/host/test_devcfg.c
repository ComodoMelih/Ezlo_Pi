/* Host tests for ezlopi-util-devcfg.
 *
 * WHY THIS FILE EXISTS
 *
 * EZPI_device_prepare() parses the device configuration out of the
 * factory-info partition once per boot. When that parse failed, the code this
 * component replaces did the following, in ezlopi_core_devices.c, for every
 * possible cause of failure:
 *
 *     cJSON_AddStringToObject(__FUNCTION__, cj_config, ezlopi_chipset_str, <this chip>);
 *     char *tmp_str = cJSON_PrintUnformatted(__FUNCTION__, cj_config);
 *     if (tmp_str)
 *     {
 *         EZPI_core_factory_info_v3_set_ezlopi_config(cj_config);
 *         vTaskDelay(1000);
 *         free(tmp_str);
 *         EZPI_core_reset_reboot();
 *     }
 *
 * That repairs exactly one cause: a configuration stored without a "chipset"
 * key, which is the migration it was written for. For the others it is a
 * reboot loop, and the reason is in cjext rather than here:
 *
 *   - add_item_to_object() ends in add_item_to_array(), which appends. There
 *     is no replace and no duplicate check.
 *   - get_object_item() walks from object->child forward and returns the
 *     *first* key whose name matches.
 *
 * So if a "chipset" key is already present and is itself why the parse failed
 * -- it names a different chip, or is not a string -- the stamp appends a
 * second key that nothing will ever read. The next boot fails in the same
 * place, appends a third, and restarts again. Likewise when the chipset was
 * correct all along and a device entry is what failed: stamping is simply
 * irrelevant to the cause.
 *
 * WHY IT IS WORSE THAN A LOOP
 *
 * The restart is issued from inside EZPI_init(), which ezlopi_main.c calls at
 * line 139. EZPI_SERV_uart_init() is line 143 and EZPI_ble_service_init() is
 * line 150, so during such a loop neither ever runs, nor does the websocket
 * server. The hub is unreachable over every interface it has, which removes
 * the BLE factory reset as well -- the one documented recovery. With
 * CONFIG_FREERTOS_HZ=1000 in all four committed sdkconfig.defaults files the
 * vTaskDelay(1000) is one second, so the cycle also rewrites the configuration
 * into flash roughly once a second indefinitely.
 *
 * The configuration that triggers this is attacker- or app-supplied: it is
 * written over BLE characteristic 0xE501 or over UART. "Push a config naming
 * the wrong chip" is a plausible mistake for anyone moving a config between
 * boards, and its result is a board that needs a serial erase to recover.
 *
 * HOW THE DEFECT IS DEMONSTRATED RATHER THAN ASSERTED
 *
 * legacy_action() below is that old behaviour expressed in the same terms as
 * the new function: it restarts whenever the parse failed, regardless of
 * anything else. run_boot_cycles() then drives either decision function
 * through a model of successive boots, where the only thing a restart changes
 * is that a stamped key is now present. test_legacy_does_not_terminate()
 * fails if the old behaviour *stops* restarting -- so this file demonstrates
 * the loop rather than claiming it, and anyone who believes the concern was
 * imaginary can run it and read the output.
 *
 * THE PROPERTY WORTH THE WHOLE FILE
 *
 * test_every_input_terminates() enumerates all eight combinations of the three
 * booleans and asserts that none of them produces more than one restart. That
 * is the guarantee a user cares about, it holds for inputs no board of mine
 * will ever present, and it is checked by construction rather than by my
 * having thought about the cases.
 */

#include <stdbool.h>
#include <stdio.h>

#include "ezlopi_util_devcfg.h"

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

/* The behaviour this component replaces, in the same shape as the new one: any
 * parse failure stamps and restarts. target_name_known is accepted and ignored
 * because the old code ignored it too -- on a target with no branch in the
 * #if chain it added nothing, stored the configuration unchanged and restarted
 * anyway. */
static e_ezpi_devcfg_action_t legacy_action(bool parse_ok,
                                            bool chipset_key_present,
                                            bool target_name_known)
{
    (void)chipset_key_present;
    (void)target_name_known;

    return parse_ok ? EZPI_DEVCFG_PROCEED : EZPI_DEVCFG_STAMP_AND_REBOOT;
}

typedef e_ezpi_devcfg_action_t (*decider_t)(bool, bool, bool);

/* A model of consecutive boots from one stored configuration.
 *
 * The model is deliberately generous to the old code in the only way that
 * matters: it assumes stamping always succeeds in making the key present. The
 * real cjext appends a duplicate that get_object_item() never returns, so on
 * hardware a mismatched or non-string chipset does not even become "present"
 * in the sense the parser cares about. Modelling it as present cannot hide a
 * loop -- it can only make the old behaviour look better than it is -- so a
 * loop reported here is a loop that is at least as bad in the firmware.
 *
 * parse_ok is held constant across boots because none of the causes under test
 * are repaired by a restart; the chipset-absent case is represented by
 * parse_ok becoming true once the key has been stamped, which is what
 * actually happens.
 *
 * Returns the number of restarts, stopping at the cap.
 */
static int run_boot_cycles(decider_t decide, bool parse_ok,
                           bool chipset_key_present, bool target_name_known,
                           bool stamping_fixes_parse, int cap)
{
    int restarts = 0;
    bool present = chipset_key_present;
    bool parses = parse_ok;

    while (restarts <= cap)
    {
        e_ezpi_devcfg_action_t action = decide(parses, present, target_name_known);

        if (EZPI_DEVCFG_STAMP_AND_REBOOT != action)
        {
            /* Terminal: either the config parsed or we chose to stay up. */
            return restarts;
        }

        restarts++;
        present = true; /* the stamp put the key there */
        if (stamping_fixes_parse)
        {
            parses = true;
        }
    }

    return restarts;
}

/* The guarantee. Every combination of the three inputs, no more than one
 * restart. Enumerated rather than listed so a future input cannot be
 * overlooked by me. */
static void test_every_input_terminates(void)
{
    for (int bits = 0; bits < 8; bits++)
    {
        bool parse_ok = (bits & 1) != 0;
        bool present = (bits & 2) != 0;
        bool known = (bits & 4) != 0;

        /* The pessimistic model: stamping does not make the config parse.
         * This is the case that loops, so it is the one to bound. */
        int restarts = run_boot_cycles(EZPI_util_devcfg_recovery, parse_ok,
                                       present, known, false, 10);
        if (restarts > 1)
        {
            printf("  parse_ok=%d present=%d known=%d -> %d restarts\n",
                   parse_ok, present, known, restarts);
        }
        ok(restarts <= 1, "at most one restart for any input");
    }
}

/* The same enumeration against the old behaviour, which must loop. If this
 * ever passes, the premise of this component is wrong and it should go. */
static void test_legacy_does_not_terminate(void)
{
    int looping_inputs = 0;

    for (int bits = 0; bits < 8; bits++)
    {
        bool parse_ok = (bits & 1) != 0;
        bool present = (bits & 2) != 0;
        bool known = (bits & 4) != 0;

        int restarts = run_boot_cycles(legacy_action, parse_ok, present, known,
                                       false, 10);
        if (restarts > 1)
        {
            looping_inputs++;
        }
    }

    /* Four of the eight have parse_ok false, and every one of those restarts
     * without bound under the old behaviour. */
    ok(4 == looping_inputs,
       "old behaviour loops on all four failing inputs");
}

/* The legitimate migration must still work: a config with no chipset key is
 * stamped and the hub restarts once, after which it parses. This is the
 * behaviour being preserved, and the reason the fix is not simply "never
 * reboot". */
static void test_migration_still_reboots_once(void)
{
    ok(EZPI_DEVCFG_STAMP_AND_REBOOT ==
           EZPI_util_devcfg_recovery(false, false, true),
       "absent chipset key is stamped and restarted");

    /* With stamping modelled as repairing the parse -- which is what happens
     * for this cause -- the hub restarts exactly once and then proceeds. */
    ok(1 == run_boot_cycles(EZPI_util_devcfg_recovery, false, false, true,
                            true, 10),
       "migration costs exactly one restart");
}

/* The three causes a restart cannot fix. */
static void test_unfixable_causes_stay_up(void)
{
    ok(EZPI_DEVCFG_CONTINUE_BOOT ==
           EZPI_util_devcfg_recovery(false, true, true),
       "chipset key present and failing: stay up");

    ok(EZPI_DEVCFG_CONTINUE_BOOT ==
           EZPI_util_devcfg_recovery(false, false, false),
       "no name to stamp for this target: stay up");

    ok(EZPI_DEVCFG_CONTINUE_BOOT ==
           EZPI_util_devcfg_recovery(false, true, false),
       "key present and no name to stamp: stay up");
}

/* A parse that succeeded must never restart, whatever else is true. */
static void test_success_never_reboots(void)
{
    for (int bits = 0; bits < 4; bits++)
    {
        bool present = (bits & 1) != 0;
        bool known = (bits & 2) != 0;

        ok(EZPI_DEVCFG_PROCEED == EZPI_util_devcfg_recovery(true, present, known),
           "a parsed config proceeds regardless of the other inputs");
    }
}

int main(void)
{
    printf("ezlopi-util-devcfg host tests\n\n");

    test_every_input_terminates();
    test_legacy_does_not_terminate();
    test_migration_still_reboots_once();
    test_unfixable_causes_stay_up();
    test_success_never_reboots();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    /* Deliberately NOT tested here:
     *
     *   - That EZPI_device_prepare() actually calls this function with the
     *     right three booleans, or that it honours the answer. That is a
     *     wiring question in a file which needs NVS, cjext and FreeRTOS, so it
     *     is covered by reading the call site and by the firmware build, not
     *     here. The risk is real and worth naming: this component can be
     *     perfect while the caller passes chipset_key_present the wrong way
     *     round.
     *   - That finishing the boot after a faulty config is itself safe. The
     *     device loop tolerates an empty list -- __device_loop() re-reads the
     *     head when its static pointer is NULL and guards on
     *     device_node->items -- but nothing here proves it, and no board has
     *     run this path. Note the list is not necessarily empty: the parse
     *     instantiates devices as it goes and reports only the last entry's
     *     outcome, so a fault in a late entry leaves the earlier devices live.
     *   - Whether a mismatched chipset *should* be adopted rather than
     *     refused. That is a product decision, recorded in the header; the
     *     test only pins the refusal that was chosen.
     */

    return g_fails ? 1 : 0;
}
