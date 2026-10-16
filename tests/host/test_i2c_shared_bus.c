/* Host tests for ezlopi-hal-i2c's channel sharing.
 *
 * WHY THIS FILE EXISTS
 *
 * EZPI_hal_i2c_master_init() installs the IDF driver only the first time a
 * channel is used:
 *
 *     if (NULL == i2c_master_conf_ptr[i2c_master_conf->channel])
 *     {
 *         ... i2c_param_config(); i2c_driver_install(); ...
 *         ret = EZPI_SUCCESS;
 *     }
 *
 * That guard is correct and deliberate -- I2C is a bus, every sensor on it
 * shares SDA and SCL, and the device is selected by address at transfer time.
 * The defect was that `ret = EZPI_SUCCESS` sat *inside* the guard, so the
 * second device on a channel fell through with the initial
 * EZPI_ERR_HAL_INIT_FAILED. Eight of the nine callers are shaped
 *
 *     if (EZPI_SUCCESS == EZPI_hal_i2c_master_init(&item->interface.i2c_master))
 *
 * and give up on the sensor otherwise. Every I2C driver in the tree hardcodes
 * channel I2C_NUM_0 rather than reading a channel from the configuration, so
 * "the second device on a channel" means *any* second I2C sensor, whatever
 * pins it names. Two sensors on one bus is the ordinary way to use I2C, and the
 * second one never initialised.
 *
 * The user-visible shape of that: configure a BME280 and a TSL2561 on GPIO
 * 21/22 -- which is the in-tree example configuration at
 * ezlopi_core_factory_info.h:225 -- and one of them silently reports nothing,
 * with "I2C init failed" on a serial console nobody is watching.
 *
 * test_second_device_on_the_same_pins_succeeds() is the regression. It asserts
 * on the install count as well as the return, because returning SUCCESS while
 * installing the driver twice would be a different bug with the same green
 * test: i2c_driver_install() on an installed port fails, and ESP_ERROR_CHECK
 * turns that into an abort.
 *
 * WHAT IS DELIBERATELY NOT TESTED HERE
 *
 * Mutual exclusion. The HAL serialises transfers on a binary semaphore, and
 * stubs/freertos/semphr.h emulates that single-threaded, where "take" cannot
 * contend. Whether two FreeRTOS tasks sharing one bus actually serialise needs
 * the scheduler and is left to the target.
 *
 * EZPI_hal_i2c_master_deinit() is also untested, on purpose: it is declared in
 * the header and called from nowhere in the tree. Its guard reads
 * `if (1 == i2c_master_conf_ptr[channel]->channel)`, which compares a channel
 * *number* against 1 and dereferences the table without a NULL check. Writing
 * a test for dead code would freeze that behaviour in place; it is recorded
 * under Known gaps in CHANGELOG.md instead.
 *
 * NOTE ON ORDER
 *
 * i2c_master_conf_ptr is file-static in the HAL and there is no working teardown
 * to reset it, so these cases run in sequence against accumulating state and
 * are not independent. That is a property of the code under test, not an
 * oversight -- the sequence below is built to exploit it, using channel 1 at the
 * end to show the guard is per-channel rather than global.
 */
#include <stdio.h>
#include <string.h>

#include "driver/i2c.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_hal_i2c_master.h"

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

/* Recording fakes for the IDF calls. stubs/driver/i2c.h declares these without
** defining them precisely so that the counts live here, next to the assertions
** that read them. */
static int g_param_config_calls;
static int g_driver_install_calls;
static int g_driver_delete_calls;
static i2c_config_t g_last_config;
static i2c_port_t g_last_port = -1;

esp_err_t i2c_param_config(i2c_port_t port, const i2c_config_t *conf)
{
    g_param_config_calls++;
    g_last_port = port;
    g_last_config = *conf;
    return ESP_OK;
}

esp_err_t i2c_driver_install(i2c_port_t port, i2c_mode_t mode,
                             size_t slv_rx_buf_len, size_t slv_tx_buf_len,
                             int intr_alloc_flags)
{
    (void)mode;
    (void)slv_rx_buf_len;
    (void)slv_tx_buf_len;
    (void)intr_alloc_flags;
    (void)port;
    g_driver_install_calls++;
    return ESP_OK;
}

esp_err_t i2c_driver_delete(i2c_port_t port)
{
    (void)port;
    g_driver_delete_calls++;
    return ESP_OK;
}

esp_err_t i2c_master_read_from_device(i2c_port_t port, uint8_t device_address,
                                      uint8_t *read_buffer, size_t read_size,
                                      int ticks_to_wait)
{
    (void)port;
    (void)device_address;
    (void)read_buffer;
    (void)read_size;
    (void)ticks_to_wait;
    return ESP_OK;
}

esp_err_t i2c_master_write_to_device(i2c_port_t port, uint8_t device_address,
                                     const uint8_t *write_buffer,
                                     size_t write_size, int ticks_to_wait)
{
    (void)port;
    (void)device_address;
    (void)write_buffer;
    (void)write_size;
    (void)ticks_to_wait;
    return ESP_OK;
}

/* The HAL stores the pointer it is handed in its channel table, so these have
** to outlive each call. File scope rather than locals for that reason. */
static s_ezlopi_i2c_master_t g_bme280;  /* first on the bus */
static s_ezlopi_i2c_master_t g_tsl2561; /* second, same pins */
static s_ezlopi_i2c_master_t g_mismatch;
static s_ezlopi_i2c_master_t g_other_channel;
static s_ezlopi_i2c_master_t g_disabled;

static void setup_conf(s_ezlopi_i2c_master_t *c, int sda, int scl,
                       uint32_t channel, uint32_t address, bool enable)
{
    memset(c, 0, sizeof(*c));
    c->enable = enable;
    c->sda = sda;
    c->scl = scl;
    c->clock_speed = 100000;
    c->channel = channel;
    c->address = address;
}

static void test_the_first_device_brings_the_bus_up(void)
{
    printf("-- the first device brings the bus up --\n");

    /* GPIO 21/22 and addresses 118 and 57 are the in-tree example at
    ** ezlopi_core_factory_info.h:225, so this is a configuration the project
    ** itself documents rather than one invented for the test. */
    setup_conf(&g_bme280, 21, 22, I2C_NUM_0, 118, true);

    ok(EZPI_SUCCESS == EZPI_hal_i2c_master_init(&g_bme280),
       "first device on a fresh channel succeeds");
    ok(1 == g_driver_install_calls, "the driver is installed once");
    ok(1 == g_param_config_calls, "the channel is configured once");
    ok(I2C_NUM_0 == g_last_port, "it configured channel 0");

    /* The pins reaching the IDF are the ones from the configuration, not a
    ** default. Worth asserting because everything below is about a *second*
    ** device inheriting these. */
    ok(21 == g_last_config.sda_io_num, "SDA 21 reached the driver");
    ok(22 == g_last_config.scl_io_num, "SCL 22 reached the driver");
    ok(100000 == g_last_config.master.clk_speed, "100 kHz reached the driver");
    ok(I2C_MODE_MASTER == g_last_config.mode, "configured as master");
}

static void test_second_device_on_the_same_pins_succeeds(void)
{
    printf("-- a second device on the same pins succeeds --\n");

    /* THE REGRESSION. Before the fix this returned EZPI_ERR_HAL_INIT_FAILED,
    ** and the eight callers shaped `if (EZPI_SUCCESS == ...)` abandoned the
    ** sensor. */
    setup_conf(&g_tsl2561, 21, 22, I2C_NUM_0, 57, true);

    ok(EZPI_SUCCESS == EZPI_hal_i2c_master_init(&g_tsl2561),
       "second device sharing SDA/SCL succeeds");

    /* And it succeeds by *reusing* the bus. If this count moved, the fix would
    ** be installing a driver on an installed port, which the real
    ** i2c_driver_install() rejects and ESP_ERROR_CHECK turns into an abort. */
    ok(1 == g_driver_install_calls, "the driver is still installed only once");
    ok(1 == g_param_config_calls, "the channel is not reconfigured");

    /* A third device on the bus is no different from the second: the guard is
    ** not an off-by-one that happens to let exactly two through. */
    static s_ezlopi_i2c_master_t third;
    setup_conf(&third, 21, 22, I2C_NUM_0, 0x40, true);
    ok(EZPI_SUCCESS == EZPI_hal_i2c_master_init(&third),
       "a third device on the bus also succeeds");
    ok(1 == g_driver_install_calls, "still one install after three devices");
}

static void test_a_second_device_on_different_pins_is_refused(void)
{
    printf("-- different pins on one channel are refused --\n");

    /* A check that said SUCCESS unconditionally once the channel was up would
    ** pass every assertion above and be wrong here: one channel cannot drive
    ** two pin pairs, so this device would be handed GPIO 21/22 and sit in the
    ** device list never responding. Since every driver hardcodes I2C_NUM_0,
    ** that is reachable purely by configuring two sensors on different pins.
    **
    ** These refusal assertions are only meaningful *together with* the sharing
    ** assertions above, and that is worth being explicit about: against the
    ** pre-fix HAL, which refused every second device, this whole case passed
    ** vacuously. Run on its own it would be a test that cannot fail. The pair
    ** of cases pins the behaviour from both sides -- share when the pins match,
    ** refuse when they do not -- and neither half is load-bearing alone. */
    setup_conf(&g_mismatch, 18, 19, I2C_NUM_0, 0x76, true);

    ok(EZPI_SUCCESS != EZPI_hal_i2c_master_init(&g_mismatch),
       "SDA/SCL that disagree with the running bus are refused");
    ok(1 == g_driver_install_calls, "a refusal installs nothing");
    ok(1 == g_param_config_calls, "a refusal reconfigures nothing");

    /* Refusing must not have disturbed the bus the first two are using. */
    ok(EZPI_SUCCESS == EZPI_hal_i2c_master_init(&g_bme280),
       "the original device still initialises after a refusal");
    ok(1 == g_driver_install_calls, "and still without reinstalling");

    /* Only one pin differing is still a conflict -- a sensor wired to the right
    ** SDA and the wrong SCL does not work any better than one wired wrong
    ** twice. */
    static s_ezlopi_i2c_master_t half;
    setup_conf(&half, 21, 19, I2C_NUM_0, 0x77, true);
    ok(EZPI_SUCCESS != EZPI_hal_i2c_master_init(&half),
       "matching SDA with a different SCL is still refused");

    setup_conf(&half, 18, 22, I2C_NUM_0, 0x77, true);
    ok(EZPI_SUCCESS != EZPI_hal_i2c_master_init(&half),
       "matching SCL with a different SDA is still refused");
}

static void test_the_guard_is_per_channel(void)
{
    printf("-- the guard is per channel, not global --\n");

    /* Channel 1 is untouched, so this must install rather than be told the bus
    ** is already up. No driver in the tree uses channel 1 today -- they all
    ** hardcode I2C_NUM_0 -- so this tests the table indexing rather than a
    ** configuration anyone currently ships. */
    setup_conf(&g_other_channel, 18, 19, I2C_NUM_1, 0x76, true);

    ok(EZPI_SUCCESS == EZPI_hal_i2c_master_init(&g_other_channel),
       "a device on a second channel brings that channel up");
    ok(2 == g_driver_install_calls, "which is a second install");
    ok(I2C_NUM_1 == g_last_port, "on channel 1");
    ok(18 == g_last_config.sda_io_num, "with its own SDA");
    ok(19 == g_last_config.scl_io_num, "with its own SCL");

    /* 18/19 is now legitimate on channel 1 while still being refused on
    ** channel 0, which is the point of keeping the comparison per channel. */
    static s_ezlopi_i2c_master_t sharer;
    setup_conf(&sharer, 18, 19, I2C_NUM_1, 0x77, true);
    ok(EZPI_SUCCESS == EZPI_hal_i2c_master_init(&sharer),
       "and a second device shares channel 1");
    ok(2 == g_driver_install_calls, "without a third install");
}

static void test_the_edges(void)
{
    printf("-- the edges --\n");

    ok(EZPI_SUCCESS != EZPI_hal_i2c_master_init(NULL),
       "NULL is refused rather than dereferenced");

    /* enable:false means the item is not an I2C device. It must not report
    ** success and must not touch the bus -- unchanged by the fix, asserted so
    ** that widening the success path later cannot quietly swallow it. */
    setup_conf(&g_disabled, 21, 22, I2C_NUM_0, 118, false);
    ok(EZPI_SUCCESS != EZPI_hal_i2c_master_init(&g_disabled),
       "a disabled interface is not initialised");
    ok(2 == g_driver_install_calls, "and installs nothing");
}

int main(void)
{
    printf("ezlopi-hal-i2c shared-bus host tests\n\n");

    test_the_first_device_brings_the_bus_up();
    test_second_device_on_the_same_pins_succeeds();
    test_a_second_device_on_different_pins_is_refused();
    test_the_guard_is_per_channel();
    test_the_edges();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    if (g_fails)
    {
        printf("I2C SHARED BUS TESTS FAILED\n");
        return 1;
    }
    printf("I2C SHARED BUS TESTS PASSED\n");
    return 0;
}
