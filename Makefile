# Tanmatsu Music Player Plugin Makefile
# Wraps CMake build for convenience

BUILD_DIR := build
PLUGIN_NAME := musicplayer
APP_SLUG_NAME := at.cavac.musicplayer
PLUGIN_SDK := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))/../tanmatsu-launcher/tools/plugin-sdk
TOOLCHAIN := $(PLUGIN_SDK)/toolchain-plugin.cmake
BADGEDIR := /tmp/mnt
DEST := $(BADGEDIR)/sd/plugins

.PHONY: all build clean rebuild install info apprepo

all: build

build:
	@if [ -z "$$IDF_PATH" ]; then \
		echo "Error: IDF_PATH not set. Please run 'source /path/to/esp-idf/export.sh' first."; \
		exit 1; \
	fi
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) ..
	@cd $(BUILD_DIR) && make

clean:
	@rm -rf $(BUILD_DIR)
	@echo "Build directory cleaned."

rebuild: clean build

# Show plugin info after build
info:
	@if [ -f $(BUILD_DIR)/$(PLUGIN_NAME).plugin ]; then \
		echo "Plugin: $(BUILD_DIR)/$(PLUGIN_NAME).plugin"; \
		ls -lh $(BUILD_DIR)/$(PLUGIN_NAME).plugin; \
	else \
		echo "Plugin not built yet. Run 'make build' first."; \
	fi

# Install to a target directory (usage: make install DEST=/path/to/plugins)
install: build
	@echo BadgeFS mount point: $(BADGEDIR)
	@echo Plugin path: $(DEST)
	@mkdir -p $(BADGEDIR)
	@badgefs $(BADGEDIR)
	@mkdir -p $(DEST)/$(APP_SLUG_NAME)
	@cp $(BUILD_DIR)/$(PLUGIN_NAME).plugin $(DEST)/$(APP_SLUG_NAME)/
	@cp metadata/plugin.json $(DEST)/$(APP_SLUG_NAME)/
	badgefs -u $(BADGEDIR)
	@echo "Installed to $(DEST)/$(APP_SLUG_NAME)/"

APP_REPO_PATH ?= ../tanmatsu-app-repository/$(APP_SLUG_NAME)

apprepo: build
	@echo "=== Updating app repository ==="
	mkdir -p $(APP_REPO_PATH)
	cp metadata/metadata.json $(APP_REPO_PATH)/metadata.json
	cp metadata/plugin.json $(APP_REPO_PATH)/plugin.json
	cp metadata/icon16.png $(APP_REPO_PATH)/icon16.png
	cp metadata/icon32.png $(APP_REPO_PATH)/icon32.png
	cp metadata/icon64.png $(APP_REPO_PATH)/icon64.png
	cp $(BUILD_DIR)/$(PLUGIN_NAME).plugin $(APP_REPO_PATH)/$(PLUGIN_NAME).plugin
	@echo "=== App repository updated at $(APP_REPO_PATH) ==="
