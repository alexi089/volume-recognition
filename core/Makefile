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

TESTS   := test_filter test_pose test_gesture
TEST_BINS := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test clean

all: $(BUILD)/vol

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

# Run every test binary; stop at the first one that fails.
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

clean:
	rm -rf $(BUILD)
