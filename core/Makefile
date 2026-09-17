# Build output goes to core/build/ so the source tree stays clean.
CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Wpedantic -g -Iinclude
AUDIO   := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
BUILD   := build

.PHONY: all clean

all: $(BUILD)/vol

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/vol: tools/vol.c src/volume_macos.c include/volume.h | $(BUILD)
	$(CC) $(CFLAGS) tools/vol.c src/volume_macos.c $(AUDIO) -o $@

clean:
	rm -rf $(BUILD)
