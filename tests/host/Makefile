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

# -fsanitize is where the value is for this particular suite: the defects being
# regression-tested were buffer overruns, and an overrun that happens to land
# in unused stack passes a plain assert. Set SANITIZERS= to disable (e.g. for a
# toolchain that lacks them).
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer

CFLAGS  += -std=c99 -O1 -g \
           -Wall -Wextra -Werror \
           -I$(SANITIZE_DIR) \
           $(SANITIZERS)
LDFLAGS += $(SANITIZERS)

BUILD   := build
TESTS   := test_sanitize

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

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
