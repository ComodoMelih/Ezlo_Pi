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
MQTTCFG_DIR  := ../../ezlopi-util/ezlopi-util-mqttcfg
MQTTAPI_DIR  := ../../ezlopi-cloud/ezlopi-cloud-mqtt-config
MQTTCORE_DIR := ../../ezlopi-core/ezlopi-core-mqtt-config
ERRORS_DIR   := ../../ezlopi-core/ezlopi-core-errors
CONSTANTS_DIR := ../../ezlopi-cloud/ezlopi-cloud-constants

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
           test_mqttcfg test_mqtt_api

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

# ezlopi-util-uuid ties MB1013 for the shortest command line here -- one -I,
# its own directory, and no stubs -- but it got there differently, and the
# difference is the point. MB1013 needed a split: mb1013_parse.c was carved out
# of a driver that configures GPIOs. This file needed nothing at all.
# ezlopi_util_uuid.c includes <stdlib.h>, <string.h> and <time.h>, its header
# includes nothing, and the whole translation unit compiles against the system
# compiler unmodified.
#
# Which is the uncomfortable part: this component could have been tested from
# the day it was written, and was not. ezlopi_util_uuid_parse() turned out to
# have four defects, including a read past the end of its input buffer that
# ASan reports on any string shorter than 32 hex digits. None of them was
# reachable, because nothing in the repository calls this component -- it is
# dead code that compiles into the image. The argument for a seam is usually
# about ESP-IDF dependencies; this one needed no seam and no argument, just
# somebody writing the file.
$(BUILD)/test_uuid: UUID_CFLAGS := -std=c99 -O1 -g -Wall -Wextra -Werror \
                                   -I$(UUID_DIR) \
                                   $(SANITIZERS)
$(BUILD)/test_uuid: test_uuid.c $(UUID_DIR)/ezlopi_util_uuid.c | $(BUILD)
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
# ten other binaries include; see the comment in the file. The ordinary stubs/
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

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
