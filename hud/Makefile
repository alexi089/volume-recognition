# Makefile for volume-hud - a glass-style volume HUD.
#
# Plain swiftc + Command Line Tools only. No Xcode, no xcodebuild, no SwiftPM,
# no XCTest. Output goes to build/.
#
#   make all     build build/volume-hud
#   make test    build + run the parser tests (build/test_message)
#   make demo    build, then pipe a scripted sequence into volume-hud
#   make clean   remove build/

SWIFTC   := /usr/bin/swiftc
BUILD    := build
SRC      := Sources
TESTS    := Tests
MODCACHE := $(BUILD)/mc

# Swift 6 language mode = strict concurrency checking (must build clean).
SWIFT_FLAGS := -swift-version 6 -O -module-cache-path $(MODCACHE)

UI_SOURCES := \
	$(SRC)/HudMessage.swift \
	$(SRC)/HudOptions.swift \
	$(SRC)/VisibilityModel.swift \
	$(SRC)/LevelBarView.swift \
	$(SRC)/HudContentView.swift \
	$(SRC)/HudPanel.swift \
	$(SRC)/StdinReader.swift \
	$(SRC)/MessageCoalescer.swift \
	$(SRC)/AppController.swift \
	$(SRC)/main.swift

# The Foundation-only core (parser + visibility reducer) plus its test.
# -parse-as-library is required because the test entry point is @main in a file
# not named main.swift.
TEST_SOURCES := \
	$(SRC)/HudMessage.swift \
	$(SRC)/HudOptions.swift \
	$(SRC)/VisibilityModel.swift \
	$(TESTS)/test_message.swift

# --- Workaround for a Command Line Tools 16.2 packaging defect -----------------
# Some CLT installs ship BOTH usr/include/swift/module.modulemap (stale) and
# usr/include/swift/bridging.modulemap, each defining `module SwiftBridging`.
# The duplicate makes every Foundation/AppKit import fail with
#   error: redefinition of module 'SwiftBridging'
# which then cascades into a misleading "this SDK is not supported" error.
# We neutralise ONLY the stale file with a VFS overlay that maps it to an empty
# modulemap. This touches no system file, and is applied ONLY when the duplicate
# is actually present, so it is a no-op on a healthy toolchain. See README.
INCDIR  := $(shell xcode-select -p 2>/dev/null)/usr/include/swift
DUP_MAP := $(and $(wildcard $(INCDIR)/module.modulemap),$(wildcard $(INCDIR)/bridging.modulemap))
OVERLAY := $(BUILD)/vfs-overlay.yaml
ifneq ($(strip $(DUP_MAP)),)
VFS_FLAGS := -vfsoverlay $(OVERLAY)
VFS_DEP   := $(OVERLAY)
endif

.PHONY: all test demo clean

all: $(BUILD)/volume-hud

$(BUILD):
	@mkdir -p $(BUILD)

# Generate the overlay + empty modulemap (only depended on when DUP_MAP is set).
$(OVERLAY): | $(BUILD)
	@printf '' > $(BUILD)/empty.modulemap
	@printf '%s\n' \
		'{' \
		'  "version": 0,' \
		'  "case-sensitive": "false",' \
		'  "roots": [' \
		'    { "type": "directory", "name": "$(INCDIR)", "contents": [' \
		'      { "type": "file", "name": "module.modulemap",' \
		'        "external-contents": "$(abspath $(BUILD)/empty.modulemap)" } ] }' \
		'  ]' \
		'}' > $@
	@echo "note: applied SwiftBridging duplicate-modulemap workaround (CLT defect)"

$(BUILD)/volume-hud: $(UI_SOURCES) $(VFS_DEP) | $(BUILD)
	$(SWIFTC) $(SWIFT_FLAGS) $(VFS_FLAGS) -o $@ $(UI_SOURCES)

$(BUILD)/test_message: $(TEST_SOURCES) $(VFS_DEP) | $(BUILD)
	$(SWIFTC) $(SWIFT_FLAGS) -parse-as-library $(VFS_FLAGS) -o $@ $(TEST_SOURCES)

test: $(BUILD)/test_message
	@echo "== running parser tests =="
	@$(BUILD)/test_message

demo: all
	@bash demo.sh

clean:
	rm -rf $(BUILD)
