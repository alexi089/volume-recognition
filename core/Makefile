# Build output goes to core/build/ so the source tree stays clean.
CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Wpedantic -g -Iinclude -Isrc
AUDIO   := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
BUILD   := build

# Tests are built with the sanitizers on. AddressSanitizer catches out-of-bounds
# and use-after-free at the moment they happen; UndefinedBehaviorSanitizer
# catches signed overflow, bad shifts, misaligned access and the like. In C
# these bugs otherwise pass silently until they corrupt something unrelated.
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer

# Every target depends on every header. Hand-picked header lists drift (one
# already had: test_step missed volume.h, which it includes through step.h),
# and a stale binary tests the wrong definitions without saying so. The whole
# project compiles in about a second, so precision here buys nothing.
HDRS    := $(wildcard include/*.h src/*.h tests/*.h)

TESTS   := test_filter test_pose test_gesture test_resize test_step test_trace test_window_math
TEST_BINS := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test replay clean

all: $(BUILD)/vol $(BUILD)/libgesture.dylib

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/vol: tools/vol.c src/volume_macos.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) tools/vol.c src/volume_macos.c $(AUDIO) -o $@

$(BUILD)/test_filter: tests/test_filter.c src/filter.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_filter.c src/filter.c -lm -o $@

$(BUILD)/test_pose: tests/test_pose.c src/pose.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_pose.c src/pose.c -lm -o $@

CORE_SRC := src/gesture.c src/gestures/volume.c src/gestures/resize.c src/pose.c src/filter.c

$(BUILD)/test_gesture: tests/test_gesture.c $(CORE_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_gesture.c $(CORE_SRC) -lm -o $@

$(BUILD)/test_resize: tests/test_resize.c $(CORE_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_resize.c $(CORE_SRC) -lm -o $@

$(BUILD)/test_window_math: tests/test_window_math.c src/window_math.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_window_math.c src/window_math.c -lm -o $@

# The shared library the Python side loads with ctypes. No sanitizers here: an
# ASan-instrumented dylib cannot be loaded into an uninstrumented Python.
LIB_SRC := $(CORE_SRC) src/step.c src/step_system.c src/volume_macos.c

$(BUILD)/libgesture.dylib: $(LIB_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -O2 -dynamiclib $(LIB_SRC) $(AUDIO) -lm -o $@

# test_step links step.c but NOT volume_macos.c: it supplies a fake backend,
# and gv_system_volume / gv_step_system live in step_system.c for that reason.
$(BUILD)/test_step: tests/test_step.c src/step.c $(CORE_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_step.c src/step.c $(CORE_SRC) -lm -o $@

$(BUILD)/test_trace: tests/test_trace.c src/trace.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_trace.c src/trace.c -lm -o $@

$(BUILD)/replay: tools/replay.c src/trace.c $(CORE_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tools/replay.c src/trace.c $(CORE_SRC) -lm -o $@

# Run every test binary; stop at the first one that fails.
test: $(TEST_BINS) replay
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

# Replay every trace listed in traces/manifest.txt. All of them run even if
# one fails, so a single run shows the whole picture; the braces keep `fail`
# and the loop in the same subshell, so the final exit status reaches make.
replay: $(BUILD)/replay
	@grep -v -E '^[[:space:]]*(#|$$)' ../traces/manifest.txt | { \
		fail=0; \
		while read -r file expect; do \
			./$(BUILD)/replay "../traces/$$file" "$$expect" || fail=1; \
		done; \
		exit $$fail; }

clean:
	rm -rf $(BUILD)
