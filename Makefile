# Makefile for Transbasket C implementation

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread
LIBS = -lcurl -lmicrohttpd -lcjson -luuid -lm -lssl -lcrypto

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj

# Target executable (output to root directory)
TARGET = transbasket

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Header files
HEADERS = $(wildcard $(INC_DIR)/*.h)

# Include path
INCLUDES = -I$(INC_DIR)

# Default target
all: directories $(TARGET)

# Create necessary directories
directories:
	@mkdir -p $(OBJ_DIR)

# Link object files to create executable
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TARGET)"

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	rm -rf $(OBJ_DIR) $(TARGET) core core.*
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
debug: clean directories $(TARGET)
	@echo "Debug build complete: $(TARGET) (with -g3 -O0 -DDEBUG -rdynamic)"
	@echo "Core dumps enabled. To enable system-wide: ulimit -c unlimited"

# Check library dependencies
check-deps:
	@echo "Checking required libraries..."
	@pkg-config --exists libcurl && echo "✓ libcurl found" || echo "✗ libcurl NOT found"
	@pkg-config --exists libmicrohttpd && echo "✓ libmicrohttpd found" || echo "✗ libmicrohttpd NOT found"
	@pkg-config --exists libcjson && echo "✓ libcjson found" || echo "✗ libcjson NOT found"
	@ldconfig -p | grep -q libuuid && echo "✓ libuuid found" || echo "✗ libuuid NOT found"
	@pkg-config --exists openssl && echo "✓ openssl found" || echo "✗ openssl NOT found"

.PHONY: all directories clean rebuild install uninstall debug check-deps
