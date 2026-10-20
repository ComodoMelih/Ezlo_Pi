# Host-side tests. These compile the real firmware sources with the system C
# compiler -- no ESP-IDF, no toolchain, no board. Run them before sending a
# pull request:
#
#     make -C tests/host
#
# Only code that is free of ESP-IDF dependencies can be tested this way, which
# is why ezlopi-util-sanitize deliberately has no REQUIRES. Compiling the real
# implementation rather than a copy is the whole point: a copy drifts.

CC      ?= cc
SANITIZE_DIR := ../../ezlopi-util/ezlopi-util-sanitize
IMPROV_DIR   := ../../ezlopi-util/ezlopi-util-improv
HA_DIR       := ../../ezlopi-util/ezlopi-util-ha
CJEXT_DIR    := ../../ezlopi-components/cjext
UART_HAL_DIR := ../../ezlopi-hal/ezlopi-hal-uart
ZE08_DIR     := ../../ezlopi-sensors-devices/sensor-0069-UART-ze08-ch02-gas-sensor
PMS_DIR      := ../../ezlopi-sensors-devices/sensor-0037-pms5003-sensor
MB1013_DIR   := ../../ezlopi-sensors-devices/sensor-0021-UART-MB1013
GPS_DIR      := ../../ezlopi-sensors-devices/sensor-0053-UART-GYGPS6MV2
UUID_DIR     := ../../ezlopi-util/ezlopi-util-uuid
ACTIONS_DIR  := ../../ezlopi-core/ezlopi-core-actions
LOCALKEY_DIR := ../../ezlopi-util/ezlopi-util-localkey
DEVCFG_DIR   := ../../ezlopi-util/ezlopi-util-devcfg
DEVCFGVAL_DIR := ../../ezlopi-util/ezlopi-util-devcfgval
DEVCFGAPI_DIR := ../../ezlopi-cloud/ezlopi-cloud-devcfg
WSSESSION_DIR := ../../ezlopi-util/ezlopi-util-wssession
SCENES_DIR   := ../../ezlopi-core/ezlopi-core-scenes
MQTTCFG_DIR  := ../../ezlopi-util/ezlopi-util-mqttcfg
MQTTAPI_DIR  := ../../ezlopi-cloud/ezlopi-cloud-mqtt-config
MQTTCORE_DIR := ../../ezlopi-core/ezlopi-core-mqtt-config
ERRORS_DIR   := ../../ezlopi-core/ezlopi-core-errors
COREAPI_DIR  := ../../ezlopi-core/ezlopi-core-api
COREEZLOPI_DIR := ../../ezlopi-core/ezlopi-core-ezlopi
SNTP_DIR     := ../../ezlopi-core/ezlopi-core-sntp
EVENTGRP_DIR := ../../ezlopi-core/ezlopi-core-event-group
CONSTANTS_DIR := ../../ezlopi-cloud/ezlopi-cloud-constants
BLEBUF_DIR   := ../../ezlopi-core/ezlopi-core-ble
I2C_HAL_DIR  := ../../ezlopi-hal/ezlopi-hal-i2c
HTTP_DIR     := ../../ezlopi-core/ezlopi-core-http
COORDS_DIR   := ../../ezlopi-cloud/ezlopi-cloud-coordinates
NVS_DIR      := ../../ezlopi-core/ezlopi-core-nvs
TZDB_DIR     := ../../ezlopi-components/micro_tz_db
VERSION_DIR  := ../../ezlopi-util/ezlopi-util-version

# cjext needs two headers that do not exist on a host: EZLOPI_USER_CONFIG.h
# (whose real version includes driver/uart.h and the generated sdkconfig.h) and
# ezlopi_util_trace.h (esp_log.h, FreeRTOS). stubs/ supplies both. cjext
# references no CONFIG_* symbol and no TRACE_* macro, which is the only reason
# a shim this thin is enough -- see stubs/EZLOPI_USER_CONFIG.h.
#
# stubs/ also carries driver/gpio.h, driver/uart.h, freertos/FreeRTOS.h,
# freertos/queue.h and esp_err.h. Those exist so that the *real*
# ezlopi_hal_uart.h can be compiled on a host rather than copied into stubs/.
# That distinction matters: a host copy of the HAL header would let the UART
# callback signature and struct s_ezlopi_uart_object drift away from what the
# tests assume, and the tests would keep passing while doing so.
STUBS_DIR    := stubs

# -fsanitize is where the value is for this particular suite: the defects being
# regression-tested were buffer overruns, and an overrun that happens to land
# in unused stack passes a plain assert. Set SANITIZERS= to disable (e.g. for a
# toolchain that lacks them).
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

CFLAGS  += -std=c99 -O1 -g \
           -Wall -Wextra -Werror \
           -I$(SANITIZE_DIR) \
           -I$(IMPROV_DIR) \
           -I$(HA_DIR) \
           $(SANITIZERS)
LDFLAGS += $(SANITIZERS)

BUILD   := build
TESTS   := test_sanitize test_cjext test_improv test_ha test_ze08 test_pms5003 \
           test_mb1013 test_gps6mv2 test_uuid test_actions test_localkey \
           test_mqttcfg test_mqtt_api test_devcfg test_devcfgval \
           test_devcfg_api test_wssession test_scenefields test_ble_buffer \
           test_i2c_shared_bus test_api_envelope test_cjsongpio test_httpurl \
           test_httpresp test_suntime test_issun test_coords test_tz

BINARIES := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test clean
all: test

test: $(BINARIES)
	@fail=0; \
	for t in $(BINARIES); do \
	    echo "=== $$t ==="; \
	    ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "HOST TESTS FAILED"; exit 1; fi; \
	echo "HOST TESTS PASSED"

$(BUILD)/test_sanitize: test_sanitize.c $(SANITIZE_DIR)/ezlopi_util_sanitize.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# The Improv codec is the one part of the Improv work that can be tested rather
# than only linked, because it deliberately depends on nothing but the C
# library. The frame parser acts on lengths chosen by whoever is on the other
# end of the wire, so running it under ASan here is worth more than any amount
# of reading it.
$(BUILD)/test_improv: test_improv.c $(IMPROV_DIR)/ezlopi_util_improv.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# The Home Assistant mapping is pure lookup, so none of it can be observed from
# the firmware: a device class Home Assistant rejects produces no error on the
# device, just an entity that never appears. Here every item name and value
# type the drivers in ezlopi-sensors-devices actually assign can be swept in a
# few milliseconds.
$(BUILD)/test_ha: test_ha.c $(HA_DIR)/ezlopi_util_ha.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# cjext.c is vendored third-party code, so it is compiled without -Werror and
# with the two warnings it legitimately trips disabled. The test file itself is
# still built under the strict flags above. What is being regression-tested
# here is cjext's documented behaviour, not its warning cleanliness; turning
# the vendored file's style warnings into build failures would only invite
# someone to patch upstream code and make the next cjext update harder.
$(BUILD)/test_cjext: CJEXT_CFLAGS := -std=c99 -O1 -g -I$(CJEXT_DIR) -I$(STUBS_DIR) $(SANITIZERS)
$(BUILD)/test_cjext: test_cjext.c $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(CJEXT_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_cjext_main.o test_cjext.c
	$(CC) $(CJEXT_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_cjext_main.o $(BUILD)/cjext.o $(LDFLAGS) -lm

# The ZE08-CH2O driver is the first sensor driver in this suite, and it is here
# because it is the only UART one whose dependency on ESP-IDF is confined to the
# type declarations in ezlopi_hal_uart.h. stubs/ supplies the five ESP-IDF
# headers underneath that one, so the real HAL header gets compiled; the test
# then supplies EZPI_hal_uart_init() itself and keeps the upcall pointer the
# driver registers, which is how it reaches a parser that is entirely static.
#
# -Wno-error=cpp is needed for one reason only: ze08_ch2o.c contains a
# "#warning "DO NOT user printf"" left behind by its author, and -Werror
# promotes #warning to an error. The note is deliberately left in place rather
# than deleted as a drive-by, so the exemption is scoped to this one file and
# the warning still prints.
#
# The other four UART drivers cannot join yet -- they pull in cJSON, the device
# and item core, FreeRTOS tasks and GPIO -- which is the concrete argument for
# the HAL/parser seam those drivers still need.
$(BUILD)/test_ze08: ZE08_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                   -I$(ZE08_DIR) -I$(UART_HAL_DIR) -I$(STUBS_DIR) \
                                   $(SANITIZERS)
$(BUILD)/test_ze08: test_ze08.c $(ZE08_DIR)/ze08_ch2o.c | $(BUILD)
	$(CC) $(ZE08_CFLAGS) -c -o $(BUILD)/test_ze08_main.o test_ze08.c
	$(CC) $(ZE08_CFLAGS) -Wno-error=cpp -c -o $(BUILD)/ze08_ch2o.o $(ZE08_DIR)/ze08_ch2o.c
	$(CC) -o $@ $(BUILD)/test_ze08_main.o $(BUILD)/ze08_ch2o.o $(LDFLAGS)

# The PMS5003 parser is the first driver brought in by splitting it rather than
# by being separable already: pms5003.c configures GPIOs and creates an
# esp_timer, so only pms5003_parse.c is compiled here. It needs no stand-in for
# the UART HAL -- unlike ze08, whose upcall is static and has to be captured
# through EZPI_hal_uart_init() -- because the split made the upcall an extern
# symbol the test can call directly. stubs/ still supplies the headers beneath
# the real ezlopi_hal_uart.h so the handle struct cannot drift.
$(BUILD)/test_pms5003: PMS_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                     -I$(PMS_DIR) -I$(UART_HAL_DIR) -I$(STUBS_DIR) \
                                     $(SANITIZERS)
$(BUILD)/test_pms5003: test_pms5003.c $(PMS_DIR)/pms5003_parse.c | $(BUILD)
	$(CC) $(PMS_CFLAGS) -o $@ $^ $(LDFLAGS)

# The MB1013 parser needs no stubs at all -- not even the UART HAL headers that
# ze08 and pms5003 pull in -- because the split left it taking a buffer and a
# length and returning a value, with the item and user_arg handling kept in the
# driver. It is the cheapest of the three rules here, which is the argument for
# the seam: the ESP-IDF surface a parser needs is usually zero once the
# hardware plumbing is on the other side of a function call.
$(BUILD)/test_mb1013: MB1013_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                       -I$(MB1013_DIR) \
                                       $(SANITIZERS)
$(BUILD)/test_mb1013: test_mb1013.c $(MB1013_DIR)/mb1013_parse.c | $(BUILD)
	$(CC) $(MB1013_CFLAGS) -o $@ $^ $(LDFLAGS)

# The GY-GPS6MV2 parser was the cheapest seam of the four, and not because it
# was split: GYGPS6MV2_parse_gpgga_mesg() was already a standalone function in a
# file whose only other includes are <string.h> and the trace macros. The one
# thing keeping it off a host compiler was its header, which declared the NMEA
# layout and a cloud-action prototype together, so including it dragged in three
# ezlopi_core_* headers for a prototype this file never uses. gps6mv2_parse.h is
# that layout on its own; sensor_0053_UART_GYGPS6MV2.h still includes it, so
# nothing downstream changed.
#
# Unlike MB1013 this one does need -Istubs, for the two headers the driver file
# includes directly: ezlopi_util_trace.h and EZLOPI_USER_CONFIG.h. The latter is
# load-bearing here rather than incidental -- it is where ezlopi_malloc() comes
# from, and the parser allocates a working copy of the sentence.
$(BUILD)/test_gps6mv2: GPS_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                     -I$(GPS_DIR) -I$(STUBS_DIR) \
                                     $(SANITIZERS)
$(BUILD)/test_gps6mv2: test_gps6mv2.c $(GPS_DIR)/gyGPS6MV2.c | $(BUILD)
	$(CC) $(GPS_CFLAGS) -o $@ $^ $(LDFLAGS)

# ezlopi-util-uuid used to tie MB1013 for the shortest command line here --
# one -I, its own directory, and no stubs -- and it got there differently, which
# was the point. MB1013 needed a split: mb1013_parse.c was carved out of a
# driver that configures GPIOs. This file needed nothing at all; it included
# <stdlib.h>, <string.h> and <time.h>, and the whole translation unit compiled
# against the system compiler unmodified.
#
# Which was the uncomfortable part: this component could have been tested from
# the day it was written, and was not. ezlopi_util_uuid_parse() turned out to
# have four defects, including a read past the end of its input buffer that
# ASan reports on any string shorter than 32 hex digits. None of them was
# reachable, because nothing in the repository calls this component. It is not
# "dead code that compiles into the image", as this comment used to say: `nm`
# on the linked esp32c3 ELF finds no ezlopi_util_uuid_* symbol, and the four
# images came out byte-identical in size across a change to this component.
# The linker discards all of it.
#
# It now takes one stub directory, and it is worth being clear that this is a
# cost. <time.h> was replaced by <esp_random.h> when generate_random() stopped
# using srand(time(NULL)), so the component is no longer portable by accident.
# stubs-uuid/esp_random.h redeclares the real header's two functions verbatim
# and stub_random.c implements esp_fill_random() as a counter. The reason to
# pay this rather than put an #ifdef ESP_PLATFORM in the component is that the
# #ifdef would give the firmware and the test two different functions, and the
# test is only evidence while they are one. The counter is also what makes the
# new assertion possible: "all sixteen bytes came from the RNG" is decidable
# against a predictable source and not against rand().
$(BUILD)/test_uuid: UUID_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                   -I$(UUID_DIR) -Istubs-uuid \
                                   $(SANITIZERS)
$(BUILD)/test_uuid: test_uuid.c $(UUID_DIR)/ezlopi_util_uuid.c \
                    stubs-uuid/stub_random.c | $(BUILD)
	$(CC) $(UUID_CFLAGS) -o $@ $^ $(LDFLAGS)

# ezlopi-core-actions was found the same way as uuid, by a sweep rather than by
# reading, and it is the end of that particular seam. host_reachable.py asks
# `gcc -fsyntax-only` which first-party .c files compile with nothing but their
# own directory on the include path; 27 did so while having no tests, and this
# is the only one of the 27 with any executable logic in it. The others are
# string tables, four files that are a licence header and nothing else, three
# that are a single #include, and three -- ezlopi_cloud_data.c,
# ezlopi_core_event_queue.c, ezlopi_core_ethernet.c -- wrapped in `#if 0` end
# to end. "Compiles with no stubs" selects for emptiness far more than for
# portability, so there is no third uuid waiting in that list.
#
# A stub sdkconfig.h used to be named here as the next lever, on the strength
# of being the first missing header for 69 of the files that fail. It is not
# one, and tests/ci/host_reachable.py is now in the tree so that can be
# re-checked rather than taken on trust: an empty sdkconfig.h lets 52 more
# files compile and all 52 define no symbols whatsoever, because every file it
# blocks is wrapped end to end in #ifdef CONFIG_SOMETHING and an empty stub
# leaves the body switched off. Supply a real generated one and the number
# falls to 9, all nine still empty, because switching the bodies on makes them
# need ESP-IDF. The actual levers are stubs for ezlopi_core_cloud.h (41 files),
# ezlopi_util_trace.h (31), cjext.h (22) and freertos/FreeRTOS.h (22) -- which
# is what stubs/ holds today, one judgement call at a time. See CONTRIBUTING.md.
#
# What it found: EZLOPI_ACTION(HUB_UPDATE, "EZLOPI_ACTION_HUB_GET_ITEM") in
# ezlopi_core_actions_list.h, so two distinct enum values stringified to one
# name. Not reachable -- all three call sites are commented out and the linker
# discards both symbols -- but wrong, and the sort of wrong that reads as right
# in a log. The test derives each expected string from the macro's token with
# the stringify operator instead of reading the table's own string column,
# because a test that read `name` would have agreed with the bug.
$(BUILD)/test_actions: ACTIONS_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                         -I$(ACTIONS_DIR) \
                                         $(SANITIZERS)
$(BUILD)/test_actions: test_actions.c $(ACTIONS_DIR)/ezlopi_core_actions.c | $(BUILD)
	$(CC) $(ACTIONS_CFLAGS) -o $@ $^ $(LDFLAGS)

# ezlopi-util-localkey is the first component in this suite that was created in
# order to be testable rather than found to be testable. The local key is the
# entire access control of the offline websocket API -- an unauthenticated
# client is allowed `hub.offline.login.ui` and nothing else -- and the check it
# performs used to be `strncmp(stored, supplied, strlen(stored))` inline in
# ezlopi_core_offline_login.c, a file that cannot be host-compiled because it
# pulls in cjext, NVS and the factory-info partition.
#
# So the comparison, the validation and the generation were moved somewhere
# with no dependencies at all, and the entropy is passed in as an argument
# instead of being read from esp_fill_random() inside. Both choices exist only
# to make the checks in test_localkey.c possible: a prefix match, an empty key
# that matches every token, and a generation alphabet whose size does not
# divide 256 are all defects that leave a device behaving correctly in every
# manual test.
#
# The test keeps a verbatim copy of the old strncmp expression and asserts that
# it *accepted* the prefix and the empty key, so the file demonstrates the
# defect rather than asserting it. Needs no stubs and no -I but its own
# directory.
$(BUILD)/test_localkey: LOCALKEY_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                           -I$(LOCALKEY_DIR) \
                                           $(SANITIZERS)
$(BUILD)/test_localkey: test_localkey.c $(LOCALKEY_DIR)/ezlopi_util_localkey.c | $(BUILD)
	$(CC) $(LOCALKEY_CFLAGS) -o $@ $^ $(LDFLAGS)

# ezlopi-util-mqttcfg validates the MQTT settings that, as of this commit,
# arrive at runtime from NVS instead of from CONFIG_EZPI_MQTT_* at build time.
# That change is what makes the validation necessary: a Kconfig string was
# vetted by whoever typed it into menuconfig, a value written over the network
# is not.
#
# The check worth having a test for is the topic prefix. The service subscribes
# to <prefix>/<node>/+/set and switches relays in response, so a prefix
# containing '+' or '#' widens that subscription past this device's own topics
# while making every publish invalid -- wildcards are forbidden in a topic
# published to. The device goes silent and starts taking orders from elsewhere,
# and neither half is visible on the device.
#
# One check here records a bug this validator already had: the first draft
# compared URI schemes with strncmp() and refused "MQTT://host". esp-mqtt
# resolves the scheme through esp_transport_list_get_transport(), which uses
# strcasecmp(), so that URI works on hardware and refusing it was the validator
# inventing a failure. test_scheme_case_is_accepted() pins the fix.
$(BUILD)/test_mqttcfg: MQTTCFG_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                         -I$(MQTTCFG_DIR) \
                                         $(SANITIZERS)
$(BUILD)/test_mqttcfg: test_mqttcfg.c $(MQTTCFG_DIR)/ezlopi_util_mqttcfg.c | $(BUILD)
	$(CC) $(MQTTCFG_CFLAGS) -o $@ $^ $(LDFLAGS)

# The hub.mqtt.config.* API methods: the layer that decides which settings a
# request writes, as opposed to test_mqttcfg above, which decides whether a
# value is well formed. Both are needed and neither implies the other -- the
# validators can be perfect while the handler stores a field it was told to
# reject.
#
# This is the first test here to compile a file from ezlopi-cloud, and the only
# reason it can is that ezlopi_cloud_mqtt_config.c was written to need nothing
# from ESP-IDF: cjext, the two MQTT components and the keyword strings. The
# seven EZPI_core_mqtt_config_* functions are supplied by the test as recording
# fakes, because NVS cannot run here and because what is being tested is which
# calls the handler *chooses* to make -- including the case where it correctly
# makes none.
#
# The property worth the whole rule: a request carrying one bad field must
# store none of the good ones. Walking params and storing as it goes is the
# natural implementation and it leaves a half-applied configuration behind on
# every typo. That is invisible in manual testing, because the error reply
# looks the same either way.
#
# Two include paths are unusual here. stubs-mqtt-api/ holds a one-line
# sdkconfig.h defining CONFIG_EZPI_ENABLE_MQTT, without which the whole subject
# file preprocesses away to nothing and the suite would link and pass while
# testing an empty object file -- test_the_subject_exists() exists to catch
# precisely that. It is deliberately NOT in the shared stubs/ directory, which
# six other binaries include -- test_cjext, test_ze08, test_pms5003,
# test_gps6mv2, test_devcfgval and test_devcfg_api, which is what
# -I$(STUBS_DIR) above greps to; the comment said "ten" once, which was never
# true of any revision of this file, and "five" until test_devcfg_api was
# added. See the comment
# in stubs-mqtt-api/sdkconfig.h for why that matters. The ordinary stubs/
# comes second, for cjext's ezlopi_util_trace.h and EZLOPI_USER_CONFIG.h.
#
# cjext.c is compiled with its own relaxed flags for the same reason as in the
# test_cjext rule above: it is vendored, and -Werror on third-party code only
# makes the next update harder.
$(BUILD)/test_mqtt_api: MQTTAPI_CFLAGS := -std=c99 -O1 -g \
                                          -I$(MQTTAPI_DIR) -I$(MQTTCORE_DIR) \
                                          -I$(MQTTCFG_DIR) -I$(ERRORS_DIR) \
                                          -I$(CONSTANTS_DIR) -I$(CJEXT_DIR) \
                                          -I stubs-mqtt-api -I$(STUBS_DIR) \
                                          $(SANITIZERS)
$(BUILD)/test_mqtt_api: test_mqtt_api.c $(MQTTAPI_DIR)/ezlopi_cloud_mqtt_config.c \
                        $(MQTTCFG_DIR)/ezlopi_util_mqttcfg.c $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(MQTTAPI_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_mqtt_api_main.o test_mqtt_api.c
	$(CC) $(MQTTAPI_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/mqtt_api_subject.o $(MQTTAPI_DIR)/ezlopi_cloud_mqtt_config.c
	$(CC) $(MQTTAPI_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/mqtt_api_mqttcfg.o $(MQTTCFG_DIR)/ezlopi_util_mqttcfg.c
	$(CC) $(MQTTAPI_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/mqtt_api_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_mqtt_api_main.o $(BUILD)/mqtt_api_subject.o \
	      $(BUILD)/mqtt_api_mqttcfg.o $(BUILD)/mqtt_api_cjext.o $(LDFLAGS) -lm

# ezlopi-util-devcfg decides whether the hub may restart itself during boot
# because the stored device configuration would not parse. It was extracted for
# the same reason as localkey above -- to be testable -- but the thing being
# bounded is different and worth stating: the old code restarted on every parse
# failure while being able to repair only one of them, and the restart happens
# inside EZPI_init(), before UART, BLE and the websocket start. A config naming
# the wrong chip therefore produced a board that rebooted about once a second,
# rewrote flash each time, and offered no interface to fix it through.
#
# Three booleans and no dependencies, so test_devcfg.c can enumerate all eight
# inputs and bound the restart count by construction rather than by my having
# reasoned about the cases. It also keeps the old rule as legacy_action() and
# fails if that one *stops* looping, so the file is the evidence for the change.
$(BUILD)/test_devcfg: DEVCFG_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                       -I$(DEVCFG_DIR) \
                                       $(SANITIZERS)
$(BUILD)/test_devcfg: test_devcfg.c $(DEVCFG_DIR)/ezlopi_util_devcfg.c | $(BUILD)
	$(CC) $(DEVCFG_CFLAGS) -o $@ $^ $(LDFLAGS)

# ezlopi-util-devcfgval: whether a device configuration arriving from outside is
# worth writing to flash. Next door to test_devcfg above and a different
# question -- that one decides what to do when the *stored* configuration will
# not parse, this one decides whether to store the configuration at all.
#
# It exists because the two paths that write a configuration today, the BLE
# dynamic-config characteristic and the UART command channel, check only that
# cJSON_Parse returned non-NULL. So {"hello":"world"} is written, and the consequence
# arrives on the next boot as a hub with no devices and nothing linking that to
# the configuration sent earlier.
#
# A host test rather than a board test for a reason that is not convenience:
# there is no dry run. ezlopi_device_parse_json_v3() is static and instantiates
# devices as it walks, so the only way to ask hardware whether a configuration
# is acceptable is to store it and reboot, and the answer comes back as an
# absence. The checks that would be hardest to get from a board are the ones
# that matter most here -- the first-bad-entry index, the 4 KB boundary, and
# the long-list walk under the sanitizers.
#
# cjext is linked rather than stubbed because the subject's whole job is
# reading a cJSON tree: a fake tree would let this file agree with itself about
# what cJSON_IsArray means. The four keyword strings are defined in the test
# instead of linking ezlopi_cloud_keywords.c, as in test_mqtt_api above, and
# cjext.c gets the same relaxed flags it does everywhere here, being vendored.
$(BUILD)/test_devcfgval: DEVCFGVAL_CFLAGS := -std=c99 -O1 -g \
                                             -I$(DEVCFGVAL_DIR) \
                                             -I$(CONSTANTS_DIR) \
                                             -I$(CJEXT_DIR) -I$(STUBS_DIR) \
                                             $(SANITIZERS)
$(BUILD)/test_devcfgval: test_devcfgval.c \
                         $(DEVCFGVAL_DIR)/ezlopi_util_devcfgval.c \
                         $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(DEVCFGVAL_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_devcfgval_main.o test_devcfgval.c
	$(CC) $(DEVCFGVAL_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/devcfgval_subject.o $(DEVCFGVAL_DIR)/ezlopi_util_devcfgval.c
	$(CC) $(DEVCFGVAL_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/devcfgval_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_devcfgval_main.o $(BUILD)/devcfgval_subject.o \
	      $(BUILD)/devcfgval_cjext.o $(LDFLAGS) -lm

# ezlopi-cloud-devcfg: hub.ezlopi.config.get / .set -- the first way to read
# and replace the device configuration over the network rather than over BLE or
# a cable.
#
# The property worth the rule is the negative one: a request that is refused
# must leave the stored configuration alone. Writing as you walk is the natural
# way to write this handler, the error reply looks identical either way, and the
# damage only shows up on the next boot as devices that are quietly missing. So
# every refusal case asserts the fake's write count as well as the error, which
# is the only way that distinction can fail a test rather than fail a user.
#
# Two sources, and only one of them is here. ezlopi_cloud_devcfg.c holds the
# decisions; ezlopi_cloud_devcfg_port.c -- which is NOT compiled by this rule --
# is the six-function adapter that reaches ezlopi-core-factory-info and the
# compiled-in device table, neither of which exists on a host. test_devcfg_api.c
# defines those six itself as recording fakes, the same arrangement as
# test_mqtt_api above. That is the whole reason the port was separated: it moves
# the untestable part into a file short enough to read in full, instead of
# making the handler untestable along with it.
#
# ezlopi-util-devcfgval is linked in real, not faked. It is the thing that
# decides which shapes are acceptable, it is already host-tested on its own, and
# faking it would let this file assert that the handler forwards reasons that
# the firmware does not actually produce.
#
# No stubs-devcfg-api/ directory: unlike the MQTT methods, both handlers live
# outside any #ifdef, so there is no configuration in which this subject
# preprocesses away to nothing. test_the_subject_exists() still checks that
# something ran, because linking against a stale object file is a different
# failure and looks the same from here.
$(BUILD)/test_devcfg_api: DEVCFGAPI_CFLAGS := -std=c99 -O1 -g \
                                              -I$(DEVCFGAPI_DIR) \
                                              -I$(DEVCFGVAL_DIR) \
                                              -I$(CONSTANTS_DIR) \
                                              -I$(CJEXT_DIR) -I$(STUBS_DIR) \
                                              $(SANITIZERS)
$(BUILD)/test_devcfg_api: test_devcfg_api.c \
                          $(DEVCFGAPI_DIR)/ezlopi_cloud_devcfg.c \
                          $(DEVCFGVAL_DIR)/ezlopi_util_devcfgval.c \
                          $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(DEVCFGAPI_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_devcfg_api_main.o test_devcfg_api.c
	$(CC) $(DEVCFGAPI_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/devcfg_api_subject.o $(DEVCFGAPI_DIR)/ezlopi_cloud_devcfg.c
	$(CC) $(DEVCFGAPI_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/devcfg_api_val.o $(DEVCFGVAL_DIR)/ezlopi_util_devcfgval.c
	$(CC) $(DEVCFGAPI_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/devcfg_api_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_devcfg_api_main.o $(BUILD)/devcfg_api_subject.o \
	      $(BUILD)/devcfg_api_val.o $(BUILD)/devcfg_api_cjext.o $(LDFLAGS) -lm

# ezlopi-util-wssession: who is allowed to call the local API.
#
# The behaviour this replaces was one file-static bool in
# ezlopi_core_offline_login.c, shared by every connection to port 17001 -- a
# port bound on all interfaces. So while one client was logged in, any other
# peer on the network passed the gate without presenting the local key, and any
# peer could end the legitimate session by sending a websocket CLOSE frame,
# which reached the logout path with no check of who sent it.
#
# Both defects are invisible to a single-client test: they need two connections
# driven independently, which is awkward on a board and trivial here. The
# descriptor-reuse case is the one that cannot be tested on hardware at all
# with any confidence, because it depends on the kernel handing out a number
# again at a moment of its choosing.
$(BUILD)/test_wssession: WSSESSION_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                             -I$(WSSESSION_DIR) \
                                             $(SANITIZERS)
$(BUILD)/test_wssession: test_wssession.c $(WSSESSION_DIR)/ezlopi_util_wssession.c | $(BUILD)
	$(CC) $(WSSESSION_CFLAGS) -o $@ $^ $(LDFLAGS)

# The scene field destructor. `l_fields_v2_t::user_arg` is a void* shared by
# several when-methods that store different kinds of thing in it, and it used
# to be released with an unconditional free() -- which, for the when-method
# that stores a FreeRTOS tick count directly in the pointer, meant free() on a
# value that was never an address.
#
# Two things about this rule are deliberate:
#
# The include path puts stubs-scenes/ before stubs/ so that
# CONFIG_EZPI_SERV_ENABLE_MESHBOTS is defined for this rule only -- see the
# note in stubs-scenes/sdkconfig.h.
#
# The nm check is not decoration. ezlopi_core_scenes_delete.c is wrapped
# end to end in that same #ifdef, so without the macro the object file is empty
# and the link still succeeds as long as the test does not reference anything
# from it -- which is exactly how the first attempt at this rule "passed".
# Requiring the symbol to be present turns that silent pass into a build
# failure.
#
# cjext.c gets the relaxed flags for the same reason as everywhere else here.
$(BUILD)/test_scenefields: SCENEFIELDS_CFLAGS := -std=c99 -O1 -g \
                                                 -I$(SCENES_DIR) -I$(ERRORS_DIR) \
                                                 -I$(CJEXT_DIR) \
                                                 -I stubs-scenes -I$(STUBS_DIR) \
                                                 $(SANITIZERS)
$(BUILD)/test_scenefields: test_scenefields.c $(SCENES_DIR)/ezlopi_core_scenes_delete.c \
                           $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(SCENEFIELDS_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_scenefields_main.o test_scenefields.c
	$(CC) $(SCENEFIELDS_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/scenefields_subject.o $(SCENES_DIR)/ezlopi_core_scenes_delete.c
	@nm $(BUILD)/scenefields_subject.o | grep -q ' T EZPI_scenes_delete_fields' \
	  || { echo "ezlopi_core_scenes_delete.c compiled to an empty object -- CONFIG_EZPI_SERV_ENABLE_MESHBOTS is not reaching it"; exit 1; }
	$(CC) $(SCENEFIELDS_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/scenefields_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_scenefields_main.o $(BUILD)/scenefields_subject.o \
	      $(BUILD)/scenefields_cjext.o $(LDFLAGS) -lm

# The nine-arm sscanf ladder in EZPI_parse_http_url(), which turns a
# sendHttpRequest action's `url` field into a host, a port and a page. A URL
# carrying credentials falls through to the arm whose conversion is the whole
# authority, so `host` -- and from there web_server and the `Host:` header --
# used to contain "user:pass@example.com".
#
# WHY THIS RULE IS LARGER THAN THE OTHERS
#
# It compiles a whole firmware translation unit that was previously assumed
# unreachable from a host. check_scanf_field_width.py's docstring says as much:
# "the host test suite cannot compile this translation unit without stubbing
# seven IDF headers and four enum families". The seven is right -- measured, and
# they are in stubs-httpurl/. The second half is not: what is actually missing
# after those seven is six type names, two of them from esp_http_client and
# three gpio enums that now live in stubs/driver/gpio.h beside gpio_num_t.
#
# The rest of the cost is at the link. The object carries an undefined symbol
# for every function the file calls, including the ones reached only from the
# eight exported functions this test never touches: 22 of them, of which three
# are glibc's own renames, six are string constants, and thirteen are functions
# that stub_httpurl.c supplies. Ten of those thirteen abort() instead of
# returning something plausible -- see the note at the top of that file.
#
# THE SIX CONSTANTS ARE REAL SOURCES, NOT STUBS
#
# ezlopi__id_str and five others are `extern const char *` in
# ezlopi_cloud_constants.h, and they are supplied by compiling the real
# ezlopi_cloud_keywords.c and ezlopi_cloud_value_type_str.c rather than by
# redeclaring them here. Both are self-contained -- keywords.c includes only
# stdio.h and value_type_str.c includes nothing -- so there is no reason to
# accept a copy. A stubbed string constant is the kind of copy that goes wrong
# quietly: a test asserting on a JSON key would then be asserting on the key
# this directory invented.
#
# -D_GNU_SOURCE is needed because the subject calls bzero(), which glibc hides
# under -std=c99 alone.
#
# stubs-httpurl/ comes before stubs/ so the private headers win, and
# stubs-scenes/ supplies CONFIG_EZPI_SERV_ENABLE_MESHBOTS -- the same macro
# test_scenefields needs, for the same reason, which is why that stub is shared
# rather than copied.
#
# The nm check is the same guard as test_scenefields', and for a sharper reason
# here: this translation unit is wrapped in the same #ifdef, so without the
# macro it preprocesses to nothing, the link still succeeds, and every
# assertion below would be asserting about a function that was never compiled.
HTTPURL_INC := -I$(SCENES_DIR) -I$(ERRORS_DIR) -I$(CJEXT_DIR) \
               -I$(CONSTANTS_DIR) -I$(ACTIONS_DIR) -I$(COREEZLOPI_DIR) \
               -I$(I2C_HAL_DIR) -I$(UART_HAL_DIR) \
               -I../../ezlopi-core/ezlopi-core-http \
               -I../../ezlopi-core/ezlopi-core-nvs \
               -I../../ezlopi-core/ezlopi-core-devices \
               -I../../ezlopi-core/ezlopi-core-cloud \
               -I../../ezlopi-core/ezlopi-core-settings \
               -I../../ezlopi-core/ezlopi-core-protect \
               -I../../ezlopi-hal/ezlopi-hal-adc \
               -I../../ezlopi-hal/ezlopi-hal-pwm \
               -I../../ezlopi-hal/ezlopi-hal-gpio \
               -I../../ezlopi-hal/ezlopi-hal-onewire \
               -I../../ezlopi-hal/ezlopi-hal-spi \
               -I stubs-httpurl -I stubs-scenes -I$(STUBS_DIR)
$(BUILD)/test_httpurl: HTTPURL_CFLAGS := -std=c99 -O1 -g -D_GNU_SOURCE \
                                         $(HTTPURL_INC) $(SANITIZERS)
#
# ezlopi_core_http_buf.c is linked rather than stubbed. stub_httpurl.c used to
# carry a third reimplementation, EZPI_core_http_dyna_relloc, because the only
# copy of it was in the un-compilable ezlopi_core_http.c. It moved to
# ezlopi_core_http_buf.c when the response accumulator needed a host test, and
# that file does compile here -- so the copy is deleted and the real function
# grows the `Host:` header this test asserts on. Two reimplementations remain
# (mem_malloc, calc_empty_bufsize); both are still stuck in ezlopi_core_http.c.
$(BUILD)/test_httpurl: test_httpurl.c stub_httpurl.c \
                       $(HTTP_DIR)/ezlopi_core_http_buf.c \
                       $(SCENES_DIR)/ezlopi_core_scenes_then_methods_helper_func.c \
                       $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c \
                       $(CONSTANTS_DIR)/ezlopi_cloud_value_type_str.c \
                       $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(HTTPURL_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_httpurl_main.o test_httpurl.c
	$(CC) $(HTTPURL_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/httpurl_stub.o stub_httpurl.c
	$(CC) $(HTTPURL_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/httpurl_httpbuf.o $(HTTP_DIR)/ezlopi_core_http_buf.c
	$(CC) $(HTTPURL_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/httpurl_subject.o $(SCENES_DIR)/ezlopi_core_scenes_then_methods_helper_func.c
	@nm $(BUILD)/httpurl_subject.o | grep -q ' T EZPI_parse_http_url' \
	  || { echo "ezlopi_core_scenes_then_methods_helper_func.c compiled to an object without EZPI_parse_http_url in it -- CONFIG_EZPI_SERV_ENABLE_MESHBOTS is not reaching it"; exit 1; }
	$(CC) $(HTTPURL_CFLAGS) -c -o $(BUILD)/httpurl_keywords.o $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c
	$(CC) $(HTTPURL_CFLAGS) -c -o $(BUILD)/httpurl_valuetypes.o $(CONSTANTS_DIR)/ezlopi_cloud_value_type_str.c
	$(CC) $(HTTPURL_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/httpurl_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_httpurl_main.o $(BUILD)/httpurl_stub.o \
	      $(BUILD)/httpurl_httpbuf.o \
	      $(BUILD)/httpurl_subject.o $(BUILD)/httpurl_keywords.o \
	      $(BUILD)/httpurl_valuetypes.o $(BUILD)/httpurl_cjext.o $(LDFLAGS) -lm

# The HTTPS response accumulator. ezlopi_core_http_buf.c is the one file in
# ezlopi-core-http that can be compiled here, and it is that file because the
# accumulator was moved into it for this test: ezlopi_core_http.c needs ten
# headers this repository does not ship, nine of them under mbedtls/ and
# esp_tls/, which is also why stub_httpurl.c still reimplements two of its
# buffer helpers rather than linking them. It carried a third until this file
# existed; see the test_httpurl rule above.
#
# The move was worth making because the defect being regressed -- an snprintf
# size argument that counted the terminator, so every 255-byte read lost its
# last byte -- is invisible to everything else in this repository. It writes
# less than it has room for, so ASan sees no overflow; it is not a crash, so no
# target build reports it. A test that compares the reassembled body against
# what was sent is the only instrument that can see it, and that test has to run
# the real code to be worth anything.
#
# stubs-httpresp/ comes first on the include path and shadows exactly one macro
# from stubs/EZLOPI_USER_CONFIG.h: ezlopi_realloc, routed through a function the
# test can make fail. The contract on a failed growth is that nothing is
# appended and none of *buf, *cap or *used moves, and the only way to check that
# is to make growth fail. See that header for why --wrap and giant sizes were
# rejected.
#
# No nm guard here, and that is a deliberate difference from test_httpurl and
# test_ble_buffer rather than an omission: ezlopi_core_http_buf.c is not wrapped
# in any #ifdef, so there is no macro whose absence could silently empty the
# object. If one is ever added, add the guard with it.
HTTPRESP_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                   -I$(HTTP_DIR) -I$(ERRORS_DIR) -I$(CJEXT_DIR) \
                   -I stubs-httpresp -I stubs-httpurl -I$(STUBS_DIR) \
                   $(SANITIZERS)
$(BUILD)/test_httpresp: test_httpresp.c $(HTTP_DIR)/ezlopi_core_http_buf.c | $(BUILD)
	$(CC) $(HTTPRESP_CFLAGS) -c -o $(BUILD)/test_httpresp_main.o test_httpresp.c
	$(CC) $(HTTPRESP_CFLAGS) -c -o $(BUILD)/httpresp_subject.o $(HTTP_DIR)/ezlopi_core_http_buf.c
	$(CC) -o $@ $(BUILD)/test_httpresp_main.o $(BUILD)/httpresp_subject.o $(LDFLAGS)

# The isSunState suntime helpers: the 12-hour clock parser, the query built
# from the stored coordinates, and the extraction of one time from a reply
# body. These were split out of __issunsate_update_sunstate_tm, which is in
# the 4000-line when-methods helper file and needs a radio, a clock and an NVS
# partition; the three functions here need none of those and carry all of the
# arithmetic that can be got wrong.
#
# The clock parser is the reason this rule exists. "12:00:00 AM" is hour 0 and
# "12:00:00 PM" is hour 12, and a +12 on every PM reading is both the obvious
# implementation and wrong twice a day. test_suntime walks all 1440 minutes
# through both renderings, which is a check no amount of reading substitutes
# for.
#
# ezlopi_cloud_keywords.c is compiled rather than stubbed, for the reason given
# at length in the test_httpurl rule: ezlopi_latitude_str and
# ezlopi_longitude_str are the same constants that named the fields when the
# coordinates were stored, and a stubbed copy of them would let this test pass
# against key names the firmware does not use.
#
# stubs-scenes/ comes first on the include path for CONFIG_EZPI_SERV_ENABLE_MESHBOTS,
# shared with test_scenefields and test_httpurl rather than copied. The nm
# guard is load-bearing for exactly that reason: the subject is wrapped end to
# end in that #ifdef, so without the macro it preprocesses to nothing, the link
# still succeeds for as long as the test references no symbol from it, and the
# suite would report a pass having compiled an empty object.
#
# -fno-sanitize-recover=undefined, which most rules here do not carry. It is
# here for one operation: EZPI_core_suntime_backoff_delay's
# `EZPI_SUNTIME_BACKOFF_BASE_SEC << (step - 1)`. The two guards in front of it
# -- step <= 0 rejected, step above the maximum saturated down -- exist only to
# keep that shift count inside 0..5, so the thing this rule has to be able to
# detect is a shift count outside it.
#
# It was added because the mutation sweep needed it, and that is the honest
# reason. Loosening `step <= 0` to `step < 0` makes delay(0) evaluate 60 << -1.
# gcc's default UBSan handler prints "runtime error: shift exponent -1 is
# negative" and carries on, and on this host the shift then yields 0 -- which is
# the answer the test asserts for step 0, so every check passed and the suite
# exited 0 with a sanitizer finding in its own output. The mutant survived a
# green run. That is the failure mode the flag removes: see probe/bomut.py,
# mutant D1.
#
# Scoped to the subject's own object file, not to SUNTIME_CFLAGS. Following the
# precedent set by test_cjsongpio further down, and for the same stated reason
# -- turning it on for all 27 binaries is probably right and is not something
# to do as a side effect of one change. Narrower than that rule, though, because
# cjext.c is compiled with these flags too: -fno-sanitize-recover changes the
# instrumentation in whichever translation unit carries it, so putting it on
# cjext.c would make any pre-existing recoverable finding in 2500 lines of JSON
# parser abort a suite that passes today. The shift is in the subject. The flag
# goes on the subject.
#
# Empty when SANITIZERS is overridden away, so a toolchain without them still
# builds this rule rather than failing on a flag it cannot honour.
SUNTIME_SAN := $(if $(SANITIZERS),-fno-sanitize-recover=undefined,)
SUNTIME_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                  -I$(SCENES_DIR) -I$(ERRORS_DIR) -I$(CJEXT_DIR) \
                  -I$(CONSTANTS_DIR) \
                  -I stubs-scenes -I$(STUBS_DIR) \
                  $(SANITIZERS)
$(BUILD)/test_suntime: test_suntime.c $(SCENES_DIR)/ezlopi_core_scenes_suntime.c \
                       $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c \
                       $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(SUNTIME_CFLAGS) -c -o $(BUILD)/test_suntime_main.o test_suntime.c
	$(CC) $(SUNTIME_CFLAGS) $(SUNTIME_SAN) -c -o $(BUILD)/suntime_subject.o $(SCENES_DIR)/ezlopi_core_scenes_suntime.c
	@nm $(BUILD)/suntime_subject.o | grep -q ' T EZPI_core_suntime_parse_clock' \
	  || { echo "ezlopi_core_scenes_suntime.c compiled to an empty object -- CONFIG_EZPI_SERV_ENABLE_MESHBOTS is not reaching it"; exit 1; }
	$(CC) $(SUNTIME_CFLAGS) -c -o $(BUILD)/suntime_keywords.o $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c
	$(CC) $(SUNTIME_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/suntime_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_suntime_main.o $(BUILD)/suntime_subject.o \
	      $(BUILD)/suntime_keywords.o $(BUILD)/suntime_cjext.o $(LDFLAGS) -lm

# The isSunState flag evaluator: the five ISSUNSTATE_* helpers that set bits in
# `flag_check`, and ISSUNSTATE_check_flag_result, the switch that turns those
# bits into a fire/don't-fire answer.
#
# WHY THIS RULE EXISTS AT ALL
#
# Because the file it compiles was recorded in this repository as un-hostable,
# and that was wrong. The claim is in the Known gaps list and in the test_suntime
# comment above -- "in the 4000-line when-methods helper file and needs a radio,
# a clock and an NVS partition" -- and it is why the suntime work was split into
# a separate, smaller translation unit rather than tested where it lives. The
# cost was measured rather than inherited: with the include set test_httpurl
# already uses, *zero* IDF headers needed stubbing, and the entire blocker was
# eight `case label does not reduce to an integer constant` errors from eight
# `const uint8_t` locals. Those are now an enum, which is a no-op on target and
# the one change that made this rule possible. See the comment at
# ISSUNSTATE_check_flag_result's flag definitions.
#
# ISSUN_INC is HTTPURL_INC, deliberately shared and not copied. The two subjects
# are different files in the same component and the measured include set came out
# identical; a 20-line duplicate here would drift. The coupling is the point --
# if someone has to add a path for one of these subjects, both get it.
#
# WHAT THE THREE SUPPRESSIONS COST, EACH MEASURED
#
# -Wno-unused-parameter: 11 sites. Four are in the ISDATE_* family and seven are
# on the path this test drives -- ISSUNSTATE_get_offs_tmval (2),
# ISSUNSTATE_eval_weekdays (2), ISSUNSTATE_eval_days (2), ISSUNSTATE_eval_range
# (1). All eleven are signature uniformity: these functions are called through a
# common shape and each ignores the arguments it has no use for. Suppressed
# rather than fixed, because `(void)x` on seven parameters of the subject under
# test is a change to the subject made to quiet the test harness.
#
# -Wno-sign-compare: 10 sites, and unlike the above, *none* of them is on the
# isSunState path -- 2 in WHEN_function_for_opr, 2 in WHEN_function_for_repeat,
# 1 in WHEN_function_for_follow, 5 in WHEN_function_for_pulse. That is uint32_t
# tick arithmetic compared against an `int` threshold, and it is a real finding
# in a family of functions no host test drives yet. It is recorded in Known gaps
# rather than fixed here: those four functions need their own test, and this
# rule is what makes writing one possible. Do not read this flag as a claim the
# comparisons are correct.
#
# -Wno-error=cpp, not -Wno-cpp: the subject carries an upstream `#warning "need
# to optimize these switch-case"`. Demoting it to a warning keeps it printing on
# every build of this rule, which is what upstream wanted; -Wno-cpp would hide
# it, and deleting it would be editing someone else's note.
#
# ezlopi_core_scenes_suntime.c, ezlopi_cloud_keywords.c and cjext.c are linked,
# not stubbed, for the reasons given at length in the test_httpurl rule. The
# keywords matter most here: ISSUNSTATE_eval_range compares a field's value
# against ezlopi_at_str, ezlopi_before_str and ezlopi_after_str, and a test
# holding stubbed copies of those three would be asserting on the spelling this
# directory invented rather than the one the cloud sends. The 11 functions that
# genuinely have to be stubbed are in stub_issun.c, and all 11 abort().
#
# The nm guard is load-bearing for the same reason as test_suntime's: the subject
# is wrapped end to end in #ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS (lines 44 and
# 2334), so without stubs-scenes/ it preprocesses to nothing, the link still
# succeeds, and the suite would report a pass having compiled an empty object.
ISSUN_INC := $(HTTPURL_INC)
ISSUN_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                $(ISSUN_INC) $(SANITIZERS)
ISSUN_SUBJ_CFLAGS := $(ISSUN_CFLAGS) -Wno-unused-parameter -Wno-sign-compare \
                     -Wno-error=cpp
$(BUILD)/test_issun: test_issun.c stub_issun.c \
                     $(SCENES_DIR)/ezlopi_core_scenes_when_methods_helper_functions.c \
                     $(SCENES_DIR)/ezlopi_core_scenes_suntime.c \
                     $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c \
                     $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(ISSUN_CFLAGS) -c -o $(BUILD)/test_issun_main.o test_issun.c
	$(CC) $(ISSUN_CFLAGS) -c -o $(BUILD)/issun_stub.o stub_issun.c
	$(CC) $(ISSUN_SUBJ_CFLAGS) -c -o $(BUILD)/issun_subject.o $(SCENES_DIR)/ezlopi_core_scenes_when_methods_helper_functions.c
	@nm $(BUILD)/issun_subject.o | grep -q ' T ISSUNSTATE_check_flag_result' \
	  || { echo "ezlopi_core_scenes_when_methods_helper_functions.c compiled to an object without ISSUNSTATE_check_flag_result in it -- CONFIG_EZPI_SERV_ENABLE_MESHBOTS is not reaching it"; exit 1; }
	$(CC) $(ISSUN_CFLAGS) -c -o $(BUILD)/issun_suntime.o $(SCENES_DIR)/ezlopi_core_scenes_suntime.c
	$(CC) $(ISSUN_CFLAGS) -c -o $(BUILD)/issun_keywords.o $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c
	$(CC) $(ISSUN_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/issun_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_issun_main.o $(BUILD)/issun_stub.o \
	      $(BUILD)/issun_subject.o $(BUILD)/issun_suntime.o \
	      $(BUILD)/issun_keywords.o $(BUILD)/issun_cjext.o $(LDFLAGS) -lm

# The hub's stored location: hub.coordinates.set, hub.coordinates.get, and the
# in-RAM pair that hub.info.get reports.
#
# This is a two-reader problem, and that is why it is worth a rule. The same
# datum is read two ways -- EZPI_cloud_get_latitude()/longitude(), which return
# file statics, and EZPI_hub_coordinates_get(), which reads NVS -- and nothing
# loaded the statics at boot. So they were zero from reset until a
# hub.coordinates.set arrived, while the NVS reader reported the real stored
# pair. Both answers were served to clients, and the wrong one was the one
# hub.info.get uses.
#
# A host process is the right instrument for that, not an awkward substitute
# for a board. A freshly started process has its file statics at zero for
# exactly the reason a freshly reset chip does, so "reboot" here is
# fake_nvs_preload() followed by EZPI_cloud_coordinates_init() -- the two halves
# of a power cycle, with the flash contents kept and the RAM lost. On hardware
# this needs a flash, a set, a reset and two API calls per case; there are 2701
# coordinates swept here.
#
# The second property is the negative one, and it is the reason the write
# ordering in the subject is what it is: a request that is refused must leave
# the stored value alone, and a write that fails must not leave the cache
# claiming it succeeded. Either mistake recreates the disagreement the rest of
# this work removes, and neither is visible in the error reply -- which looks
# identical whether or not the cache moved. So every refusal case asserts the
# fake's write count, and test_set_when_the_write_fails asserts both readers
# afterwards rather than just the reply.
#
# ezlopi_cloud_keywords.c is compiled rather than stubbed, for the reason given
# at length in the test_httpurl rule: "latitude" and "longitude" are the member
# names the pair is stored under, so a stubbed copy of those constants would let
# this file pass against key names the firmware does not use -- and the stored
# form is read back by ezlopi_core_scenes_suntime.c, which gets them from the
# same place.
#
# The NVS surface is two functions, both defined in test_coords.c as recording
# fakes. The read fake returns strdup() because the real one returns an
# allocation the caller owns; a fake handing back a pointer into its own buffer
# would hide a missing or double free from ASan, which is most of what this
# suite is for.
#
# stubs-httpurl/ is on the path for one empty header, nvs_flash.h, which
# ezlopi_core_nvs.h includes and nothing here names a type from -- shared with
# test_httpurl rather than copied.
#
# -D_GNU_SOURCE is here for strdup(), which is POSIX rather than C99 and so is
# hidden by -std=c99 alone -- the same reason the test_httpurl rule defines it
# for bzero(). It is additive, so nothing the vendored cjext.c expects to find
# gets hidden by it; -D_POSIX_C_SOURCE would be the narrower choice but can
# subtract.
#
# -Wno-unused-parameter is on the subject only, not on the test, and it buys
# exactly one thing: EZPI_hub_coordinates_get() takes a cj_request it does not
# read, because every method in the table has that signature whether or not it
# has params to look at. The test file is still compiled with the warning on,
# so an argument this file stops using is still an error here.
#
# The nm guard is not load-bearing here: no #ifdef wraps this translation unit,
# so there is no macro whose absence could empty the object. It is the same
# cheap guard as test_i2c_shared_bus', kept for the other way an object goes
# empty, and it names the init function specifically because that is the symbol
# whose absence is the defect -- against the tree before this change the link
# fails rather than the test, which is the loudest a detection gets.
COORDS_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE \
                 -I$(COORDS_DIR) -I$(NVS_DIR) -I$(ERRORS_DIR) \
                 -I$(CJEXT_DIR) -I$(CONSTANTS_DIR) \
                 -I stubs-httpurl -I$(STUBS_DIR) \
                 $(SANITIZERS)
$(BUILD)/test_coords: test_coords.c $(COORDS_DIR)/ezlopi_cloud_coordinates.c \
                      $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c \
                      $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(COORDS_CFLAGS) -c -o $(BUILD)/test_coords_main.o test_coords.c
	$(CC) $(COORDS_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/coords_subject.o $(COORDS_DIR)/ezlopi_cloud_coordinates.c
	@nm $(BUILD)/coords_subject.o | grep -q ' T EZPI_cloud_coordinates_init' \
	  || { echo "ezlopi_cloud_coordinates.c compiled to an object without EZPI_cloud_coordinates_init in it"; exit 1; }
	$(CC) $(COORDS_CFLAGS) -c -o $(BUILD)/coords_keywords.o $(CONSTANTS_DIR)/ezlopi_cloud_keywords.c
	$(CC) $(COORDS_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/coords_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_coords_main.o $(BUILD)/coords_subject.o \
	      $(BUILD)/coords_keywords.o $(BUILD)/coords_cjext.o $(LDFLAGS) -lm

# The timezone, and the reason a host is not a compromise here but the better
# instrument.
#
# setenv("TZ")/tzset()/localtime_r() are C library functions, and the firmware
# calls the same ones -- newlib's on an ESP32, glibc's here. So this rule
# asserts a real conversion of a real epoch: store "Australia/Eucla", boot, and
# 1782993600 must read back as 2026-07-01T20:45:00+0845. Not that setenv was
# called with a particular string, which is all a mock could say, and which
# would pass just as happily against a POSIX TZ string that libc rejects. A
# board would prove the same thing more slowly and for one zone at a time;
# test_every_published_zone_is_settable sweeps all 425 in the table.
#
# What is being regression-tested is a two-reader disagreement, like
# test_coords', but with the shared datum being process state rather than a
# file static. setenv("TZ")+tzset() lived in exactly one place in the firmware:
# inside EZPI_core_sntp_get_local_time(), whose only caller is hub.info.get. So
# TZ was installed as a side effect of a client asking what time it was, and
# until one did, the four time-of-day scene evaluators in
# ezlopi_core_scenes_when_methods.c ran on UTC. EZPI_core_sntp_set_location()
# did not install it either, so changing the timezone did not move them.
#
# A host process is the right shape for that for the same reason it was for the
# coordinates: a fresh process has an unset TZ for the reason a fresh chip
# does. "Reboot" is fake_nvs_preload() followed by
# EZPI_core_sntp_apply_stored_timezone(). And poison_the_timezone() -- which
# installs NPT-5:45 and asserts it took -- is what makes the fallback cases
# mean something: without it, "falls back to UTC" is indistinguishable from
# "did nothing at all", because an untouched process is already on UTC.
#
# zones.c is compiled, not stubbed, and that is the point of the sweep. The
# names the table publishes through hub.location.list are the names
# hub.location.set has to accept; a stubbed table would let this file agree
# with itself about a set of zones the firmware does not have. It also costs
# nothing -- the file is a sorted array of 425 name/POSIX pairs and a binary
# search over it.
#
# The NVS surface is the same two functions as test_coords, again as recording
# fakes in the test file, and the read fake again returns strdup() because the
# real one returns an allocation the caller owns -- here that matters more than
# usual, since EZPI_core_sntp_apply_stored_timezone() frees it on every path
# including the fallback, and a fake handing out an interior pointer would hide
# that from ASan.
#
# stubs-sntp/ is two headers and both are documented in place: an esp_sntp.h
# whose sntp_get_sync_status() returns RESET forever, so EZPI_core_sntp_init()
# runs its retry loop to exhaustion and returns a failure a test can assert
# instead of a fiction about a server that replied, and a deliberately empty
# sdkconfig.h. stubs/ supplies the rest, including the freertos/task.h that now
# carries a no-op vTaskDelay and both spellings of the tick-period macro --
# portTICK_PERIOD_MS and portTICK_RATE_MS, because this one translation unit
# uses both.
#
# -D_GNU_SOURCE is for strdup(), hidden by -std=c99 alone, as in test_coords.
#
# -Wno-unused-parameter is on the subject only:
# EZPI_core_sntp_get_current_time_sec_abc() takes a function_name it no longer
# traces. The test file keeps the warning on.
#
# The nm guard IS load-bearing here, unlike test_coords'. ezlopi_core_sntp.c
# includes sdkconfig.h, and stubs-sntp/sdkconfig.h is empty -- so the failure
# mode this guards is a future CONFIG_ gate preprocessing the file down to
# nothing and the compiler agreeing with an empty object. It names
# EZPI_core_sntp_apply_stored_timezone because that function not existing is
# the defect: against the tree before this change the link fails rather than
# the test, which is the loudest a detection gets.
TZ_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE \
             -I$(SNTP_DIR) -I$(NVS_DIR) -I$(ERRORS_DIR) \
             -I$(TZDB_DIR) -I$(VERSION_DIR) \
             -I stubs-sntp -I stubs-httpurl -I$(STUBS_DIR) \
             $(SANITIZERS)
$(BUILD)/test_tz: test_tz.c $(SNTP_DIR)/ezlopi_core_sntp.c \
                  $(TZDB_DIR)/zones.c | $(BUILD)
	$(CC) $(TZ_CFLAGS) -c -o $(BUILD)/test_tz_main.o test_tz.c
	$(CC) $(TZ_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/tz_subject.o $(SNTP_DIR)/ezlopi_core_sntp.c
	@nm $(BUILD)/tz_subject.o | grep -q ' T EZPI_core_sntp_apply_stored_timezone' \
	  || { echo "ezlopi_core_sntp.c compiled to an object without EZPI_core_sntp_apply_stored_timezone in it"; exit 1; }
	$(CC) $(TZ_CFLAGS) -c -o $(BUILD)/tz_zones.o $(TZDB_DIR)/zones.c
	$(CC) -o $@ $(BUILD)/test_tz_main.o $(BUILD)/tz_subject.o \
	      $(BUILD)/tz_zones.o $(LDFLAGS) -lm

# The BLE chunk accumulator: the linked list that holds a payload too large for
# one ATT write until the last chunk arrives. Four characteristics use it and
# two of them -- provisioning and dynamic-config -- keep it in a file-static
# across writes, on a path that runs before any authentication.
#
# This is the only file in ezlopi-core-ble that can be compiled here, and the
# reason is worth stating because it is the test for whether a seam is real:
# ezlopi_core_ble_buffer.c calls no ESP-IDF function at all and touches exactly
# two fields of the GATT callback parameter. Everything else in that component
# is GATT state machine. So stubs-ble/ is two headers -- a one-symbol
# sdkconfig.h and the union member those two fields live in -- rather than a
# stand-in for the Bluetooth stack.
#
# The nm check is the same guard as in test_scenefields below, and it is here
# for the same reason: the subject file is wrapped end to end in
# `#ifdef CONFIG_EZPI_BLE_ENABLE`, so if stubs-ble/sdkconfig.h ever stops
# reaching it the object file is empty, the link still succeeds, and the suite
# passes while testing nothing.
$(BUILD)/test_ble_buffer: BLEBUF_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                           -I$(BLEBUF_DIR) \
                                           -I stubs-ble -I$(STUBS_DIR) \
                                           $(SANITIZERS)
$(BUILD)/test_ble_buffer: test_ble_buffer.c $(BLEBUF_DIR)/ezlopi_core_ble_buffer.c | $(BUILD)
	$(CC) $(BLEBUF_CFLAGS) -c -o $(BUILD)/test_ble_buffer_main.o test_ble_buffer.c
	$(CC) $(BLEBUF_CFLAGS) -c -o $(BUILD)/ble_buffer_subject.o $(BLEBUF_DIR)/ezlopi_core_ble_buffer.c
	@nm $(BUILD)/ble_buffer_subject.o | grep -q ' T EZPI_core_ble_buffer_total_len' \
	  || { echo "ezlopi_core_ble_buffer.c compiled to an empty object -- CONFIG_EZPI_BLE_ENABLE is not reaching it"; exit 1; }
	$(CC) -o $@ $(BUILD)/test_ble_buffer_main.o $(BUILD)/ble_buffer_subject.o $(LDFLAGS)

# ezlopi-hal-i2c is the first *HAL* compiled here, and it is only possible
# because the component's ESP-IDF surface is five functions and a struct. They
# are declared in stubs/driver/i2c.h and defined in the test as recording fakes,
# which is what lets the shared-bus test assert how many times the driver was
# installed -- the question the fix turns on. The semaphore is emulated in
# stubs/freertos/semphr.h; see the honesty note at the top of that file about
# what that does not prove.
#
# The nm check is the same guard used for test_ble_buffer and test_scenefields.
# It is less load-bearing here (no #ifdef wraps this file) but costs nothing and
# would catch the subject being compiled to an empty object for any other
# reason.
$(BUILD)/test_i2c_shared_bus: I2C_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                            -I$(I2C_HAL_DIR) \
                                            -I$(ERRORS_DIR) \
                                            -I$(STUBS_DIR) \
                                            $(SANITIZERS)
$(BUILD)/test_i2c_shared_bus: test_i2c_shared_bus.c $(I2C_HAL_DIR)/ezlopi_hal_i2c_master.c | $(BUILD)
	$(CC) $(I2C_CFLAGS) -c -o $(BUILD)/test_i2c_shared_bus_main.o test_i2c_shared_bus.c
	$(CC) $(I2C_CFLAGS) -c -o $(BUILD)/i2c_master_subject.o $(I2C_HAL_DIR)/ezlopi_hal_i2c_master.c
	@nm $(BUILD)/i2c_master_subject.o | grep -q ' T EZPI_hal_i2c_master_init' \
	  || { echo "ezlopi_hal_i2c_master.c compiled to an empty object"; exit 1; }
	$(CC) -o $@ $(BUILD)/test_i2c_shared_bus_main.o $(BUILD)/i2c_master_subject.o $(LDFLAGS)

# ezlopi-core-api: the reply envelope, which is the one piece of code every
# local API call passes through. A defect here is a defect in all 200-odd
# methods at once, which is why no per-method test caught the duplicate "error"
# member this rule now regression-tests -- see the header of
# test_api_envelope.c.
#
# The subject is host-compilable for a reason worth stating, because it is not
# luck: all six of ezlopi_core_api.c's own headers are ESP-IDF free. They pull
# in cjext, <time.h>, the error enum and three sets of prototypes, and the only
# ESP-IDF function the file calls is esp_restart(). So the fake surface is four
# method-table functions, the broadcast queue, the event group and that one
# reboot -- all defined in the test, all recording.
#
# Three things about the flags:
#
# -Wno-unused-parameter applies to the subject only. EZPI_core_api_consume_cjson()
# takes a `who` it never uses -- it passes __FUNCTION__ to cjext instead -- and
# the same is true of several arguments in the file. Not worth changing a
# public signature over, and the test file itself is still built -Werror with
# the parameter warning on.
#
# -I$(STUBS_DIR) supplies esp_system.h for esp_restart(), and the BIT0..BIT7
# that ezlopi_core_event_group.h uses without including anything that defines
# them. On target those arrive through FreeRTOS.h; stubs/ezlopi_util_trace.h
# reproduces that edge rather than defining the bits somewhere they do not
# belong. See the comments in both stub files.
#
# cjext.c gets the relaxed vendored-code flags it gets in every other rule
# here, and it is linked real rather than faked: the defect being tested is in
# cjext's append-on-duplicate-key semantics, so a fake tree would let the test
# agree with itself.
$(BUILD)/test_api_envelope: APIENV_CFLAGS := -std=c99 -O1 -g \
                                             -I$(COREAPI_DIR) \
                                             -I$(COREEZLOPI_DIR) \
                                             -I$(SNTP_DIR) \
                                             -I$(EVENTGRP_DIR) \
                                             -I$(ERRORS_DIR) \
                                             -I$(CONSTANTS_DIR) \
                                             -I$(CJEXT_DIR) -I$(STUBS_DIR) \
                                             $(SANITIZERS)
$(BUILD)/test_api_envelope: test_api_envelope.c $(COREAPI_DIR)/ezlopi_core_api.c \
                            $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(APIENV_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_api_envelope_main.o test_api_envelope.c
	$(CC) $(APIENV_CFLAGS) -Wall -Wextra -Werror -Wno-unused-parameter -c -o $(BUILD)/api_envelope_subject.o $(COREAPI_DIR)/ezlopi_core_api.c
	@nm $(BUILD)/api_envelope_subject.o | grep -q ' T EZPI_core_api_consume_cjson' \
	  || { echo "ezlopi_core_api.c compiled to an object without the subject in it"; exit 1; }
	$(CC) $(APIENV_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/api_envelope_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_api_envelope_main.o $(BUILD)/api_envelope_subject.o \
	      $(BUILD)/api_envelope_cjext.o $(LDFLAGS) -lm

# The pin number a device configuration is allowed to name. One function,
# EZPI_core_cjson_get_value_gpio(), is where that number enters the firmware,
# and 128 call sites shift by what it returns.
#
# This rule carries two sanitizer flags the rest of the suite does not. The
# test's own table is what catches a broken subject -- it was run against one,
# and it fails 30 checks with the bound removed, which is why the result does
# not depend on these flags and still holds with SANITIZERS= -- but both flags
# were worth keeping once measured:
#
#   -fsanitize=float-cast-overflow is NOT part of -fsanitize=undefined in gcc.
#   The subject's one narrowing cast, (int)raw, is well defined only because of
#   the bound in front of it; this flag is an independent check on that, and it
#   fires on the broken subject at a different line than the shift does.
#
#   -fno-sanitize-recover makes any such finding fail the build. gcc's default
#   is to print "runtime error: ..." and carry on with exit 0, which would have
#   put a sanitizer finding in the log of a suite reporting success. Found by
#   looking, not assumed: nothing in this tree sets UBSAN_OPTIONS.
#
# Both are scoped to this rule rather than to SANITIZERS. Turning them on for
# every test is probably right and is not something to do as a side effect of
# this one.
#
# -I$(STUBS_DIR) supplies driver/gpio.h, and it supplies the part that matters:
# GPIO_NUM_MAX, which is the bound. It is 49 in the stub and 40 on an esp32, so
# the test is written against the symbol and never against a literal.
#
# The whole file is compiled, not just the one function, because the rest of it
# is what the subject shares a translation unit with -- and EZPI_core_cjson_trace()
# references ezlopi__str, which the test defines for the same reason
# test_devcfgval.c defines its four keywords. cjext.c is linked real: the
# subject's input is a cJSON node's valuedouble, and faking that would let this
# file decide for itself what strtod did with "1e999".
# Empty when SANITIZERS is overridden away, so that a toolchain without them
# still builds this rule rather than failing on a flag it cannot honour.
# Two separate -fno-sanitize-recover= flags rather than one comma-separated
# list, because a comma inside $(if ...) would need escaping to survive make.
CJSONGPIO_SAN := $(if $(SANITIZERS),-fsanitize=float-cast-overflow \
                                    -fno-sanitize-recover=undefined \
                                    -fno-sanitize-recover=float-cast-overflow,)

$(BUILD)/test_cjsongpio: CJSONGPIO_CFLAGS := -std=c99 -O1 -g \
                                             -I$(COREEZLOPI_DIR) \
                                             -I$(CONSTANTS_DIR) \
                                             -I$(CJEXT_DIR) -I$(STUBS_DIR) \
                                             $(SANITIZERS) $(CJSONGPIO_SAN)
$(BUILD)/test_cjsongpio: test_cjsongpio.c \
                         $(COREEZLOPI_DIR)/ezlopi_core_cjson_macros.c \
                         $(CJEXT_DIR)/cjext.c | $(BUILD)
	$(CC) $(CJSONGPIO_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/test_cjsongpio_main.o test_cjsongpio.c
	$(CC) $(CJSONGPIO_CFLAGS) -Wall -Wextra -Werror -c -o $(BUILD)/cjsongpio_subject.o $(COREEZLOPI_DIR)/ezlopi_core_cjson_macros.c
	@nm $(BUILD)/cjsongpio_subject.o | grep -q ' T EZPI_core_cjson_get_value_gpio' \
	  || { echo "ezlopi_core_cjson_macros.c compiled to an object without the subject in it"; exit 1; }
	$(CC) $(CJSONGPIO_CFLAGS) -Wno-unused-parameter -c -o $(BUILD)/cjsongpio_cjext.o $(CJEXT_DIR)/cjext.c
	$(CC) -o $@ $(BUILD)/test_cjsongpio_main.o $(BUILD)/cjsongpio_subject.o \
	      $(BUILD)/cjsongpio_cjext.o $(LDFLAGS) $(CJSONGPIO_SAN) -lm

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
