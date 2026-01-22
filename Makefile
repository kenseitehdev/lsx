# Compiler and flags
CC = clang
CFLAGS = -Wall -Wextra -Werror -std=c11 -Iinclude
LDFLAGS = -lncurses
DEBUG_FLAGS = -g -O0 -DDEBUG
RELEASE_FLAGS = -O2 -DNDEBUG

# Directories
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
INCLUDE_DIR = include

# Target executable
TARGET = $(BIN_DIR)/lsx

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Default target
.PHONY: all
all: CFLAGS += $(RELEASE_FLAGS)
all: $(TARGET)

# Debug build
.PHONY: debug
debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean $(TARGET)

# Link the executable
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Built $(TARGET)"

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create directories if they don't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts"

# Install to system (requires sudo)
.PHONY: install
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installed lsx to /usr/local/bin/"

# Uninstall from system
.PHONY: uninstall
uninstall:
	rm -f /usr/local/bin/lsx
	@echo "Uninstalled lsx"

# Run the program
.PHONY: run
run: $(TARGET)
	./$(TARGET)

# Show help
.PHONY: help
help:
	@echo "lsx Makefile targets:"
	@echo "  all        - Build release version (default)"
	@echo "  debug      - Build debug version with symbols"
	@echo "  clean      - Remove build artifacts"
	@echo "  install    - Install to /usr/local/bin (requires sudo)"
	@echo "  uninstall  - Remove from /usr/local/bin"
	@echo "  run        - Build and run the program"
	@echo "  help       - Show this help message"
