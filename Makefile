# Makefile — host build for the LFE (Lightweight Fixed-point Engine) library.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds liblfe.a with the system gcc. Used by the host test harness
# and for development iteration on the algorithms themselves. The NDS
# build is in Makefile.nds and uses devkitARM instead.
#
# Targets:
#
#   make            — build liblfe.a only
#   make test       — build liblfe.a + tests, run tests, write WAV outputs
#   make clean      — remove all build artifacts and test outputs
#
# Variables:
#
#   CC             — host C compiler (default: gcc)
#   BUILD          — build directory (default: build)
#   OUTPUT_DIR     — test WAV output directory (default: test/output)

# Force the host toolchain. We use `:=` (not `?=`) so that any
# CC=arm-none-eabi-gcc that might be exported by a devkitPro setup
# script doesn't bleed into this host build. Command-line overrides
# (e.g. `make CC=clang`) still win over `:=`.
CC      := gcc
BUILD   := build
OUTPUT_DIR := test/output

CFLAGS  := -std=c99 -Wall -Wextra -Wpedantic -O2 -g \
           -Iinclude -Isrc \
           -DLFE_PLATFORM_HOST=1
LDFLAGS := -lm

# ---- Library sources ----
SRCS := \
    src/lfe.c \
    src/util/wavetable.c \
    src/util/dbmath.c \
    src/util/envelope.c \
    src/util/lfo.c \
    src/util/noise.c \
    src/util/filter.c \
    src/util/biquad.c \
    src/util/crossover.c \
    src/util/env_follower.c \
    src/gen/gen_test_tone.c \
    src/gen/gen_drawn.c \
    src/gen/gen_drum.c \
    src/gen/gen_synth.c \
    src/gen/gen_fm4.c \
    src/gen/braids/lfe_braids_random.c \
    src/gen/braids/lfe_braids_resources.c \
    src/gen/braids/lfe_braids_analog.c \
    src/gen/braids/lfe_braids_digital.c \
    src/gen/braids/lfe_braids_macro.c \
    src/gen/gen_braids.c \
    src/fx/fx_distortion.c \
    src/fx/fx_filter.c \
    src/fx/fx_delay.c \
    src/fx/fx_env_shaper.c \
    src/fx/fx_normalize.c \
    src/fx/fx_ott.c \
    src/fx/fx_reverse.c \
    src/fx/fx_bitcrush.c

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

LIB := $(BUILD)/liblfe.a

# ---- Test sources ----
TEST_SRCS := \
    test/test_main.c \
    test/util/wav.c \
    test/test_test_tone.c \
    test/test_envelope.c \
    test/test_lfo.c \
    test/test_noise.c \
    test/test_filter.c \
    test/test_drawn.c \
    test/test_drum.c \
    test/test_synth.c \
    test/test_fm4.c \
    test/test_fx.c \
    test/test_ott.c \
    test/test_braids.c \
    test/test_dbmath.c

TEST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRCS))
TEST_BIN  := $(BUILD)/lfe_test

# ---- Targets ----
.PHONY: all test clean

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	@echo "  AR      $(notdir $@)"
	@ar rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

# Test binary links the library plus its own test sources.
$(TEST_BIN): $(TEST_OBJS) $(LIB)
	@mkdir -p $(dir $@)
	@echo "  LD      $(notdir $@)"
	@$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDFLAGS)

# Test compiles add the test directory to the include path so test
# files can find their own headers (test_main.h, util/wav.h).
$(BUILD)/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -Itest -c -o $@ $<

test: $(TEST_BIN)
	@mkdir -p $(OUTPUT_DIR)
	@echo ""
	@./$(TEST_BIN)

clean:
	@rm -rf $(BUILD)
	@rm -rf $(OUTPUT_DIR)
	@echo "  CLEAN   lib/lfe"
