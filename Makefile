# Makefile for Transbasket C implementation

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread
LIBS = -lcurl -lmicrohttpd -lcjson -luuid -lm -lssl -lcrypto -lsqlite3

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj

# Target executables (output to root directory)
TARGET = transbasket
CACHE_TOOL = cache_tool

# Source files
# Main server sources (exclude cache_tool.c)
SERVER_SRCS = $(filter-out $(SRC_DIR)/cache_tool.c, $(wildcard $(SRC_DIR)/*.c))
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Cache tool sources (needs trans_cache.c, cache backends and utils.c)
CACHE_TOOL_SRCS = $(SRC_DIR)/cache_tool.c $(SRC_DIR)/trans_cache.c $(SRC_DIR)/cache_backend_text.c $(SRC_DIR)/cache_backend_sqlite.c $(SRC_DIR)/utils.c
CACHE_TOOL_OBJS = $(CACHE_TOOL_SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Header files
HEADERS = $(wildcard $(INC_DIR)/*.h)

# Include path
INCLUDES = -I$(INC_DIR)

# Default target
all: directories $(TARGET) $(CACHE_TOOL)

# Create necessary directories
directories:
	@mkdir -p $(OBJ_DIR)

# Link object files to create main executable
$(TARGET): $(SERVER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TARGET)"

# Link object files to create cache tool
$(CACHE_TOOL): $(CACHE_TOOL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lcjson -lssl -lcrypto -luuid -lsqlite3
	@echo "Build complete: $(CACHE_TOOL)"

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build cache tool only
cache-tool: directories $(CACHE_TOOL)

# Clean build artifacts
clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(CACHE_TOOL) core core.*
	@echo "Clean complete"

# Rebuild everything
rebuild: clean all

# Install (optional)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/transbasket
	@echo "Installed to /usr/local/bin/transbasket"

# Uninstall
uninstall:
	rm -f /usr/local/bin/transbasket
	@echo "Uninstalled"

# Debug build with DEBUG macro and core dump support
debug: CFLAGS = -Wall -Wextra -std=c11 -g3 -O0 -DDEBUG -D_POSIX_C_SOURCE=200809L
debug: LDFLAGS += -rdynamic
debug: clean directories $(TARGET) $(CACHE_TOOL)
	@echo "Debug build complete: $(TARGET) and $(CACHE_TOOL) (with -g3 -O0 -DDEBUG -rdynamic)"
	@echo "Core dumps enabled. To enable system-wide: ulimit -c unlimited"

# Check library dependencies
check-deps:
	@echo "Checking required libraries..."
	@pkg-config --exists libcurl && echo "✓ libcurl found" || echo "✗ libcurl NOT found"
	@pkg-config --exists libmicrohttpd && echo "✓ libmicrohttpd found" || echo "✗ libmicrohttpd NOT found"
	@pkg-config --exists libcjson && echo "✓ libcjson found" || echo "✗ libcjson NOT found"
	@ldconfig -p | grep -q libuuid && echo "✓ libuuid found" || echo "✗ libuuid NOT found"
	@pkg-config --exists openssl && echo "✓ openssl found" || echo "✗ openssl NOT found"
	@pkg-config --exists sqlite3 && echo "✓ sqlite3 found" || echo "✗ sqlite3 NOT found"

.PHONY: all directories cache-tool clean rebuild install uninstall debug check-deps
