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

TESTS   := test_filter test_pose test_gesture test_step test_trace
TEST_BINS := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test replay clean

all: $(BUILD)/vol $(BUILD)/libgesture.dylib

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/vol: tools/vol.c src/volume_macos.c include/volume.h | $(BUILD)
	$(CC) $(CFLAGS) tools/vol.c src/volume_macos.c $(AUDIO) -o $@

$(BUILD)/test_filter: tests/test_filter.c src/filter.c src/filter.h tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_filter.c src/filter.c -lm -o $@

$(BUILD)/test_pose: tests/test_pose.c tests/fixtures.h src/pose.c src/pose.h include/gesture.h tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_pose.c src/pose.c -lm -o $@

CORE_SRC := src/gesture.c src/pose.c src/filter.c
CORE_HDR := include/gesture.h src/pose.h src/filter.h

$(BUILD)/test_gesture: tests/test_gesture.c tests/fixtures.h $(CORE_SRC) $(CORE_HDR) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_gesture.c $(CORE_SRC) -lm -o $@

# The shared library the Python side loads with ctypes. No sanitizers here: an
# ASan-instrumented dylib cannot be loaded into an uninstrumented Python.
LIB_SRC := $(CORE_SRC) src/step.c src/step_system.c src/volume_macos.c

$(BUILD)/libgesture.dylib: $(LIB_SRC) $(CORE_HDR) include/step.h include/volume.h | $(BUILD)
	$(CC) $(CFLAGS) -O2 -dynamiclib $(LIB_SRC) $(AUDIO) -lm -o $@

# test_step links step.c but NOT volume_macos.c: it supplies a fake backend,
# and gv_system_volume / gv_step_system live in step_system.c for that reason.
$(BUILD)/test_step: tests/test_step.c tests/fixtures.h src/step.c include/step.h $(CORE_SRC) $(CORE_HDR) tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_step.c src/step.c $(CORE_SRC) -lm -o $@

$(BUILD)/test_trace: tests/test_trace.c src/trace.c src/trace.h include/gesture.h tests/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) tests/test_trace.c src/trace.c -lm -o $@

$(BUILD)/replay: tools/replay.c src/trace.c src/trace.h $(CORE_SRC) $(CORE_HDR) | $(BUILD)
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
