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
           test_mb1013 test_gps6mv2 test_uuid test_actions

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
# portability, so there is no third uuid waiting in that list; the next lever
# is a stub sdkconfig.h, which is the first missing header for 69 files.
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

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
