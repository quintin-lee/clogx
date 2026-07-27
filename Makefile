CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Iinclude -O2 -D_GNU_SOURCE
LDFLAGS = -lpthread
BUILD_DIR = build
LIB_TARGET = $(BUILD_DIR)/libclogx.a
EXAMPLE_BIN = $(BUILD_DIR)/example

CORE_SRCS = config.c formatter.c dispatcher.c queue.c async.c log.c rotate.c
SINK_SRCS = console_sink.c file_sink.c socket_sink.c
CORE_OBJS = $(addprefix $(BUILD_DIR)/,$(CORE_SRCS:.c=.o))
SINK_OBJS = $(addprefix $(BUILD_DIR)/,$(SINK_SRCS:.c=.o))
ALL_OBJS = $(CORE_OBJS) $(SINK_OBJS)

TESTS = test_async_lifecycle test_async_reload test_dispatcher_lifecycle \
        test_file_rotate test_file_mkdir test_config_reload \
        test_pipeline verify_config \
        test_invalid_config test_double_init test_empty_sink \
        test_async_fallback test_queue_try_put test_max_size test_console_stderr \
        test_module_trunc test_add_sink
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TESTS))

.PHONY: all clean example test docs format check-format

FORMAT_FILES := $(shell find include core sinks example tests -name '*.c' -o -name '*.h')

all: $(LIB_TARGET) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: core/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: sinks/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_TARGET): $(ALL_OBJS)
	ar rcs $@ $^

$(EXAMPLE_BIN): example/main.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ example/main.c -L$(BUILD_DIR) -lclogx $(LDFLAGS)

$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< -L$(BUILD_DIR) -lclogx $(LDFLAGS)

$(BUILD_DIR)/verify_config: tests/verify_config.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< -L$(BUILD_DIR) -lclogx $(LDFLAGS)

example: $(EXAMPLE_BIN)

test: $(TEST_BINS)
	@mkdir -p logs
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		$$t || status=1; \
	done; \
	exit $$status

docs:
	@command -v doxygen >/dev/null || { echo "doxygen not found; install it to generate API docs"; exit 1; }
	@mkdir -p docs/api
	doxygen Doxyfile
	@echo "API docs: docs/api/html/index.html"

format:
	clang-format -i $(FORMAT_FILES)

check-format:
	@clang-format --dry-run --Werror $(FORMAT_FILES) || \
		(echo "Formatting check failed! Run 'make format' to fix." && exit 1)

clean:
	rm -rf $(BUILD_DIR)
	rm -rf docs/api
