# Makefile for Transbasket C implementation

CC = gcc
# _DEFAULT_SOURCE is required for realpath() (glibc guards it behind
# __USE_MISC/__USE_XOPEN_EXTENDED). Without it the -O0 debug build only gets an
# implicit declaration, truncating the returned pointer to int -> startup crash.
CFLAGS = -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
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
CACHE_TOOL_SRCS = $(SRC_DIR)/cache_tool.c $(SRC_DIR)/trans_cache.c $(SRC_DIR)/cache_backend_text.c $(SRC_DIR)/cache_backend_sqlite.c $(SRC_DIR)/utils.c $(SRC_DIR)/source_hint.c
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
debug: CFLAGS = -Wall -Wextra -std=c11 -g3 -O0 -DDEBUG -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
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

test-auto: all
	python3 tests/test_source_auto.py

INFLIGHT_TEST_BIN ?= $(OBJ_DIR)/test_inflight
test-inflight-unit: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) $(TEST_SANITIZERS) -o $(INFLIGHT_TEST_BIN) tests/test_inflight_translation.c src/inflight_translation.c src/trans_cache.c src/cache_backend_text.c src/cache_backend_sqlite.c src/utils.c -Wl,--wrap=strdup -lcjson -lcrypto -luuid -lsqlite3
	$(INFLIGHT_TEST_BIN)

test-coalescing: all test-inflight-unit
	python3 tests/test_request_coalescing.py $(COALESCING_TESTS)

# source_hint.c includes the generated table from src/, which HEADERS does not cover.
$(OBJ_DIR)/source_hint.o: $(SRC_DIR)/source_hint_table.h

# Differential test of the script classifier against tests/source_hint_ref.py.
SOURCE_HINT_TEST_BIN ?= $(OBJ_DIR)/test_source_hint
test-source-hint: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(SOURCE_HINT_TEST_BIN) tests/test_source_hint.c src/source_hint.c
	python3 -I tests/test_source_hint.py $(SOURCE_HINT_TEST_BIN) $(SOURCE_HINT_TEST_ARGS)

# Storage tests: hint side table, transactions, migration, text backend.
SOURCE_HINT_STORE_BIN ?= $(OBJ_DIR)/test_source_hint_store
test-source-hint-store: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) $(TEST_SANITIZERS) -o $(SOURCE_HINT_STORE_BIN) tests/test_source_hint_store.c src/trans_cache.c src/cache_backend_text.c src/cache_backend_sqlite.c src/utils.c -lcjson -lcrypto -luuid -lsqlite3
	$(SOURCE_HINT_STORE_BIN)

# Cleanup-days arithmetic, both backends, cache_tool arguments and config parsing
# (PLAN_CLEANUP_DAYS_AND_NUL.md section 5 V5); expected values come from python.
CLEANUP_DAYS_BIN ?= $(OBJ_DIR)/test_cleanup_days
test-cleanup-days: all
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) $(TEST_SANITIZERS) -o $(CLEANUP_DAYS_BIN) tests/test_cleanup_days.c src/trans_cache.c src/cache_backend_text.c src/cache_backend_sqlite.c src/utils.c -lcjson -lcrypto -luuid -lsqlite3
	python3 tests/test_cleanup_days.py $(CLEANUP_DAYS_BIN) ./$(CACHE_TOOL) ./$(TARGET)

# Differential test of the strict structured-output validator against tests/strict_json_ref.py.
STRICT_JSON_TEST_BIN ?= $(OBJ_DIR)/test_strict_json
test-strict-json: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(STRICT_JSON_TEST_BIN) tests/test_strict_json.c src/strict_json.c src/source_hint.c
	python3 -I tests/test_strict_json.py $(STRICT_JSON_TEST_BIN) $(STRICT_JSON_TEST_ARGS)

# json_decodes_nul(): spec vs the real cJSON (allocator-hook oracle) and C vs spec.
CJSON_NUL_TEST_BIN ?= $(OBJ_DIR)/test_cjson_nul
test-cjson-nul: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(CJSON_NUL_TEST_BIN) tests/test_cjson_nul.c src/strict_json.c src/source_hint.c
	python3 -I tests/test_cjson_nul.py $(CJSON_NUL_TEST_BIN) $(CJSON_NUL_TEST_ARGS)

# LISTEN address parser and LISTEN line recognizer vs tests/listen_address_ref.py, plus mutants.
LISTEN_PARSE_BIN ?= $(OBJ_DIR)/test_listen_address
test-listen-parse: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(LISTEN_PARSE_BIN) tests/test_listen_address.c src/listen_address.c
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -DWITH_CONFIG_LOADER -o $(LISTEN_PARSE_BIN)_cl tests/test_listen_address.c src/listen_address.c src/utils.c -lcjson -luuid
	python3 -I tests/test_listen_parse.py $(LISTEN_PARSE_BIN) $(LISTEN_PARSE_BIN)_cl --mutants

# strip_emoji_and_shortcodes()/unescape_string() vs tests/strip_ref.py, properties, mutants.
test-strip: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(OBJ_DIR)/test_strip tests/test_strip.c src/utils.c -luuid
	python3 -I tests/test_strip.py $(OBJ_DIR)/test_strip --mutants

# -d start report, cwd/umask, -l log file, launcher deadline (a 3 s build), inside a namespace.
test-daemon: all
	$(CC) $(CFLAGS) -DDAEMON_READY_TIMEOUT=3 $(INCLUDES) $(LDFLAGS) -o $(OBJ_DIR)/transbasket_daemon3 $(SERVER_SRCS) $(LIBS)
	python3 -I tests/test_daemon.py $(abspath $(TARGET)) $(OBJ_DIR)/transbasket_daemon3

# Forged log lines, source text in the log, codex error text, trace file safety; log_escape() vs python.
test-log-safety: all
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(OBJ_DIR)/test_log_escape tests/test_log_escape.c src/utils.c -luuid
	python3 -I tests/test_log_safety.py $(abspath $(TARGET)) $(OBJ_DIR)/test_log_escape

# handle_streaming_response() vs tests/sse_ref.py, allocation failure at every malloc, mutants.
test-sse: directories
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(OBJ_DIR)/test_sse tests/test_sse.c $(filter-out $(SRC_DIR)/backend_normal.c $(SRC_DIR)/main.c, $(SERVER_SRCS)) $(LIBS) $(LDFLAGS)
	python3 -I tests/test_sse.py $(OBJ_DIR)/test_sse --mutants

# normal backend reply handling through the real server with a fake upstream.
test-normal-output: all
	python3 -I tests/test_normal_output.py $(abspath $(TARGET))

# Numeric settings: effective values, warnings and startup errors computed in python.
test-config-numbers: all
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -o $(OBJ_DIR)/test_config_numbers tests/test_config_numbers.c src/config_loader.c src/utils.c src/listen_address.c -lcjson -luuid -lm
	python3 -I tests/test_config_numbers.py $(OBJ_DIR)/test_config_numbers $(abspath $(TARGET))

# parse_config_line() vs tests/config_line_ref.py, quoted-value errors and the tracked
# templates starting unmodified (server rows inside an unprivileged namespace).
test-codex-interface: all
	python3 -I tests/test_codex_interface.py $(abspath $(TARGET))

# PLAN_BACKEND_MODULES.md V8: the codex backend with a fake driver, and the exec driver's init paths.
test-codex-backend: directories
	$(CC) $(CFLAGS) -DTRANSBASKET_TEST_HOOKS $(INCLUDES) $(TEST_SANITIZERS) -o $(OBJ_DIR)/test_codex_backend tests/test_codex_backend.c $(filter-out $(SRC_DIR)/main.c, $(SERVER_SRCS)) $(LIBS) $(LDFLAGS)
	d=$$(mktemp -d) && $(OBJ_DIR)/test_codex_backend $$d $(abspath language_tables.conf); rc=$$?; rm -rf $$d; exit $$rc

# PLAN_BACKEND_MODULES.md V3b/V4/V5: compare against an earlier build, e.g. PARITY_OLD_BIN=/path/to/old/transbasket
# (that binary needs language_tables.conf next to it: the language table resolves against the executable's directory).
test-backend-parity: all
	@if [ -z "$(PARITY_OLD_BIN)" ]; then echo "set PARITY_OLD_BIN=<earlier transbasket build>"; exit 2; fi
	python3 -I tests/test_backend_parity.py $(abspath $(PARITY_OLD_BIN)) $(abspath $(TARGET))

test-config-syntax: all
	$(CC) $(CFLAGS) $(INCLUDES) $(TEST_SANITIZERS) -DWITH_CONFIG_LOADER -o $(OBJ_DIR)/test_config_line tests/test_listen_address.c src/listen_address.c src/utils.c -lcjson -luuid
	python3 -I tests/test_config_syntax.py $(abspath $(TARGET)) $(OBJ_DIR)/test_config_line

# LISTEN through the real server, inside an unprivileged network + PID namespace
# (loopback rows only where unshare is unavailable). LISTEN_OLD_BIN=<pre-fix binary>
# also checks that the old binary fails the same tables.
test-listen-address: all
	python3 -I tests/test_listen_address.py $(abspath $(TARGET)) $(if $(LISTEN_OLD_BIN),--old $(abspath $(LISTEN_OLD_BIN)))

# Structured mode through the real server; a TRANSBASKET_TEST_HOOKS build covers
# the schema-write and prompt-construction failure paths (hooks never compiled into $(TARGET)).
TEST_HOOKS_BIN ?= $(OBJ_DIR)/transbasket_test_hooks
test-structured-hint: all
	$(CC) $(CFLAGS) -DTRANSBASKET_TEST_HOOKS $(INCLUDES) $(LDFLAGS) -o $(TEST_HOOKS_BIN) $(SERVER_SRCS) $(LIBS)
	TB_HOOKS_BIN=$(abspath $(TEST_HOOKS_BIN)) python3 -W ignore::ResourceWarning tests/test_structured_hint.py

# Requests and replies that cJSON would cut at a decoded NUL, through the real server.
test-nul-guard: all
	python3 -W ignore::ResourceWarning tests/test_nul_guard.py $(abspath $(TARGET))

test-source-hint-server: all
	python3 tests/test_source_hint_server.py

# cache_tool hint commands; also builds a variant that believes the classifier is v2,
# to exercise backfill's upgrade path (unreachable in v1).
test-cache-tool-hints: all
	$(CC) $(CFLAGS) $(INCLUDES) -DSOURCE_HINT_VERSION=2 $(LDFLAGS) -o $(OBJ_DIR)/cache_tool_hint_v2 $(CACHE_TOOL_SRCS) -lcjson -lssl -lcrypto -luuid -lsqlite3
	python3 tests/test_cache_tool_hints.py ./cache_tool $(OBJ_DIR)/cache_tool_hint_v2

.PHONY: all directories cache-tool clean rebuild install uninstall debug check-deps test-auto test-inflight-unit test-coalescing test-source-hint test-source-hint-store test-source-hint-server test-cache-tool-hints test-strict-json test-structured-hint test-cleanup-days test-cjson-nul test-nul-guard test-listen-parse test-listen-address test-config-syntax test-config-numbers test-strip test-normal-output test-sse test-log-safety test-daemon test-codex-interface test-backend-parity test-codex-backend
