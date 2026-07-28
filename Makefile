CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Wconversion -Iinclude -O2 -D_GNU_SOURCE -fPIC -fvisibility=hidden
LDFLAGS = -lpthread
BUILD_DIR = build
LIB_TARGET = $(BUILD_DIR)/libclogx.a
SO_TARGET = $(BUILD_DIR)/libclogx.so
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
        test_module_trunc test_add_sink \
        test_multithread_sync test_socket_sink test_sink_level test_log_level
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TESTS))

# Sanitizer configs (O1 -g for meaningful stack traces)
ASAN_CFLAGS = -std=c99 -Wall -Wextra -Wconversion -Iinclude -O1 -g -D_GNU_SOURCE -fPIC -fvisibility=hidden -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls
UBSAN_CFLAGS = -std=c99 -Wall -Wextra -Wconversion -Iinclude -O1 -g -D_GNU_SOURCE -fPIC -fvisibility=hidden -fsanitize=undefined

VERSION = 0.1.0
SO_VERSION = 0
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

PUBLIC_HEADERS := include/log.h include/log_config.h include/log_record.h include/log_sink.h

.PHONY: all clean example test docs format check-format install uninstall asan ubsan test-asan test-ubsan

FORMAT_FILES := $(shell find include core sinks example tests -name '*.c' -o -name '*.h')

all: $(LIB_TARGET) $(SO_TARGET) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: core/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: sinks/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_TARGET): $(ALL_OBJS)
	ar rcs $@ $^

$(SO_TARGET): $(ALL_OBJS)
	$(CC) -shared -Wl,-soname,libclogx.so.$(SO_VERSION) -o $@ $^ $(LDFLAGS)

$(EXAMPLE_BIN): example/main.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ example/main.c $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/verify_config: tests/verify_config.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

example: $(EXAMPLE_BIN)

test: $(TEST_BINS)
	@mkdir -p logs
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		$$t || status=1; \
	done; \
	exit $$status

## Sanitizer convenience targets

asan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(ASAN_CFLAGS)"

ubsan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(UBSAN_CFLAGS)"

test-asan:
	$(MAKE) clean
	$(MAKE) all CC=$(CC) CFLAGS="$(ASAN_CFLAGS)"
	$(MAKE) test CC=$(CC) CFLAGS="$(ASAN_CFLAGS)"

test-ubsan:
	$(MAKE) clean
	$(MAKE) all CC=$(CC) CFLAGS="$(UBSAN_CFLAGS)"
	$(MAKE) test CC=$(CC) CFLAGS="$(UBSAN_CFLAGS)"

## Documentation

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

install: $(LIB_TARGET) $(SO_TARGET)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(LIB_TARGET) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SO_TARGET) $(DESTDIR)$(LIBDIR)/libclogx.so.$(SO_VERSION)
	ln -sf libclogx.so.$(SO_VERSION) $(DESTDIR)$(LIBDIR)/libclogx.so
	install -d $(DESTDIR)$(INCLUDEDIR)/clogx
	install -m 644 $(PUBLIC_HEADERS) $(DESTDIR)$(INCLUDEDIR)/clogx/
	install -d $(DESTDIR)$(LIBDIR)/pkgconfig
	sed -e 's|@prefix@|$(PREFIX)|g' \
	    -e 's|@exec_prefix@|$(PREFIX)|g' \
	    -e 's|@libdir@|$(LIBDIR)|g' \
	    -e 's|@includedir@|$(INCLUDEDIR)|g' \
	    -e 's|@CMAKE_INSTALL_PREFIX@|$(PREFIX)|g' \
	    -e 's|@CMAKE_INSTALL_LIBDIR@|lib|g' \
	    -e 's|@CMAKE_INSTALL_INCLUDEDIR@|include|g' \
	    -e 's|@PROJECT_VERSION@|$(VERSION)|g' \
	    cmake/clogx.pc.in > $(DESTDIR)$(LIBDIR)/pkgconfig/clogx.pc

uninstall:
	-rm -f $(DESTDIR)$(LIBDIR)/libclogx.a
	-rm -f $(DESTDIR)$(LIBDIR)/libclogx.so
	-rm -f $(DESTDIR)$(LIBDIR)/libclogx.so.$(SO_VERSION)
	-rm -rf $(DESTDIR)$(INCLUDEDIR)/clogx
	-rm -f $(DESTDIR)$(LIBDIR)/pkgconfig/clogx.pc

clean:
	rm -rf $(BUILD_DIR)
	rm -rf docs/api
