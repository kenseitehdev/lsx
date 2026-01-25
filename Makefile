# -----------------------------
# Project
# -----------------------------
APP       := lsx
SRC_DIR   := src
BUILD_DIR := build
BIN_DIR   := bin
INC_DIR   := include
TARGET    := $(BIN_DIR)/$(APP)

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# -----------------------------
# Toolchain
# -----------------------------
CC ?= clang

WARNFLAGS := -Wall -Wextra -Werror
STD       := -std=c11
CPPFLAGS  := -I$(INC_DIR)
CFLAGS    := $(WARNFLAGS) $(STD)

DEBUG_FLAGS   := -g -O0 -DDEBUG
RELEASE_FLAGS := -O2 -DNDEBUG

UNAME_S := $(shell uname -s)

# -----------------------------
# ncurses config helpers
# -----------------------------
# We will set these via OS-specific rules:
NCURSES_CFLAGS :=
NCURSES_LIBS   :=

# Utility: detect Homebrew ncurses prefix on macOS
BREW := $(shell command -v brew 2>/dev/null)
BREW_NCURSES_PREFIX := $(shell [ -n "$(BREW)" ] && brew --prefix ncurses 2>/dev/null)

# Utility: pkg-config on Linux
PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)

# -----------------------------
# OS-specific setup (forced)
# -----------------------------
.PHONY: setup-macos setup-linux
setup-macos: | $(BUILD_DIR)
setup-linux: | $(BUILD_DIR)
setup-macos:
	@# Prefer Homebrew ncursesw if installed
	@if [ -n "$(BREW_NCURSES_PREFIX)" ]; then \
		echo "macOS: using Homebrew ncurses at $(BREW_NCURSES_PREFIX)"; \
		echo "NCURSES_CFLAGS=-I$(BREW_NCURSES_PREFIX)/include" > $(BUILD_DIR)/ncurses.mk; \
		echo "NCURSES_LIBS=-L$(BREW_NCURSES_PREFIX)/lib -lncursesw" >> $(BUILD_DIR)/ncurses.mk; \
		echo "EXTRA_RPATH=-Wl,-rpath,$(BREW_NCURSES_PREFIX)/lib" >> $(BUILD_DIR)/ncurses.mk; \
	else \
		echo "macOS: using system ncurses"; \
		echo "NCURSES_CFLAGS=" > $(BUILD_DIR)/ncurses.mk; \
		echo "NCURSES_LIBS=-lncurses" >> $(BUILD_DIR)/ncurses.mk; \
		echo "EXTRA_RPATH=" >> $(BUILD_DIR)/ncurses.mk; \
	fi

setup-linux:
	@# Prefer pkg-config ncursesw then ncurses; fallback to -lncursesw
	@if [ -n "$(PKG_CONFIG)" ] && pkg-config --exists ncursesw 2>/dev/null; then \
		echo "Linux: using pkg-config ncursesw"; \
		echo "NCURSES_CFLAGS=$$(pkg-config --cflags ncursesw)" > $(BUILD_DIR)/ncurses.mk; \
		echo "NCURSES_LIBS=$$(pkg-config --libs ncursesw)" >> $(BUILD_DIR)/ncurses.mk; \
		echo "EXTRA_RPATH=" >> $(BUILD_DIR)/ncurses.mk; \
	elif [ -n "$(PKG_CONFIG)" ] && pkg-config --exists ncurses 2>/dev/null; then \
		echo "Linux: using pkg-config ncurses"; \
		echo "NCURSES_CFLAGS=$$(pkg-config --cflags ncurses)" > $(BUILD_DIR)/ncurses.mk; \
		echo "NCURSES_LIBS=$$(pkg-config --libs ncurses)" >> $(BUILD_DIR)/ncurses.mk; \
		echo "EXTRA_RPATH=" >> $(BUILD_DIR)/ncurses.mk; \
	else \
		echo "Linux: pkg-config not available; using -lncursesw fallback"; \
		echo "NCURSES_CFLAGS=" > $(BUILD_DIR)/ncurses.mk; \
		echo "NCURSES_LIBS=-lncursesw" >> $(BUILD_DIR)/ncurses.mk; \
		echo "EXTRA_RPATH=" >> $(BUILD_DIR)/ncurses.mk; \
	fi

# -----------------------------
# Auto-setup (default)
# -----------------------------
# Default behavior: pick setup target based on uname.
# You can still force with `make macos` or `make linux`.
.PHONY: setup
setup: | $(BUILD_DIR)
ifeq ($(UNAME_S),Darwin)
	@$(MAKE) setup-macos
else ifeq ($(UNAME_S),Linux)
	@$(MAKE) setup-linux
else
	@echo "Unknown OS: $(UNAME_S). Falling back to -lncurses."
	@echo "NCURSES_CFLAGS=" > $(BUILD_DIR)/ncurses.mk
	@echo "NCURSES_LIBS=-lncurses" >> $(BUILD_DIR)/ncurses.mk
	@echo "EXTRA_RPATH=" >> $(BUILD_DIR)/ncurses.mk
endif

# Include generated ncurses settings (created by setup target)
# Safe even if file doesn't exist yet.
-include $(BUILD_DIR)/ncurses.mk

CPPFLAGS += $(NCURSES_CFLAGS)
LDLIBS   += $(NCURSES_LIBS)
LDFLAGS  += $(EXTRA_RPATH)

# -----------------------------
# Build targets
# -----------------------------
.PHONY: all release debug macos linux clean install uninstall run help print-flags

all: release

release: CFLAGS += $(RELEASE_FLAGS)
release: setup $(TARGET)

debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean setup $(TARGET)

# Force builds for a specific OS (useful in CI/matrix)
macos: CFLAGS += $(RELEASE_FLAGS)
macos: | $(BUILD_DIR)
	@$(MAKE) setup-macos
	@$(MAKE) $(TARGET)

linux: CFLAGS += $(RELEASE_FLAGS)
linux: | $(BUILD_DIR)
	@$(MAKE) setup-linux
	@$(MAKE) $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "Built $(TARGET) (OS=$(UNAME_S))"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR)/* $(BIN_DIR)/*
	@echo "Cleaned build artifacts"

install: release
	install -m 755 $(TARGET) /usr/local/bin/$(APP)
	@echo "Installed $(APP) to /usr/local/bin/$(APP)"

uninstall:
	rm -f /usr/local/bin/$(APP)
	@echo "Uninstalled $(APP)"

run: release
	./$(TARGET)

print-flags: setup
	@echo "UNAME_S=$(UNAME_S)"
	@echo "CC=$(CC)"
	@echo "CPPFLAGS=$(CPPFLAGS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS)"
	@echo "LDLIBS=$(LDLIBS)"
	@echo "NCURSES_CFLAGS=$(NCURSES_CFLAGS)"
	@echo "NCURSES_LIBS=$(NCURSES_LIBS)"
	@echo "EXTRA_RPATH=$(EXTRA_RPATH)"

help:
	@echo "$(APP) Makefile targets:"
	@echo "  release     - Build release for current OS (default)"
	@echo "  debug       - Build debug for current OS"
	@echo "  macos       - Force macOS release flags (brew ncursesw if available)"
	@echo "  linux       - Force Linux release flags (pkg-config ncursesw if available)"
	@echo "  clean       - Remove build artifacts"
	@echo "  install     - Install to /usr/local/bin (requires sudo)"
	@echo "  uninstall   - Remove from /usr/local/bin"
	@echo "  run         - Build and run"
	@echo "  print-flags - Print detected flags"
