CC = gcc
override CFLAGS := -std=c99 -Wall -Wextra -Wconversion -Iinclude -O2 -D_GNU_SOURCE -fPIC -fvisibility=hidden $(CFLAGS)
LDFLAGS = -lpthread

TLS ?= 0
ifeq ($(TLS),1)
override CFLAGS += -DCLOG_USE_TLS
LDFLAGS += -lssl -lcrypto
endif

YAML_LIBS = $(shell pkg-config --libs yaml-0.1 2>/dev/null || echo "-lyaml")
LDFLAGS += $(YAML_LIBS)
BUILD_DIR = build
LIB_TARGET = $(BUILD_DIR)/libclogx.a
SO_TARGET = $(BUILD_DIR)/libclogx.so
EXAMPLE_BIN = $(BUILD_DIR)/example

# Auto-download libyaml if not available via pkg-config
ifeq ($(shell pkg-config --exists yaml-0.1 && echo yes 2>/dev/null),)
$(info libyaml not found via pkg-config, downloading...)
YAML_BUILD := deps/libyaml
YAML_STATIC_LIB := $(BUILD_DIR)/libyaml.a
YAML_CFLAGS := -I$(abspath $(YAML_BUILD)/include) -DYAML_VERSION_MAJOR=0 -DYAML_VERSION_MINOR=2 -DYAML_VERSION_PATCH=5 -DYAML_VERSION_STRING=\"0.2.5\"
YAML_SRCS := $(YAML_BUILD)/src/api.c $(YAML_BUILD)/src/dumper.c \
             $(YAML_BUILD)/src/emitter.c $(YAML_BUILD)/src/loader.c \
             $(YAML_BUILD)/src/parser.c $(YAML_BUILD)/src/reader.c \
             $(YAML_BUILD)/src/scanner.c $(YAML_BUILD)/src/writer.c
YAML_OBJS := $(patsubst $(YAML_BUILD)/%.c,$(BUILD_DIR)/yaml_%.o,$(YAML_SRCS))
override CFLAGS += $(YAML_CFLAGS)

$(YAML_STATIC_LIB): $(YAML_OBJS)
	ar rcs $@ $^

$(YAML_BUILD)/src/%.c: $(YAML_BUILD)/Makefile
	@:

$(BUILD_DIR)/yaml_%.o: $(YAML_BUILD)/%.c | $(BUILD_DIR) $(YAML_BUILD)/Makefile
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(YAML_BUILD)/src/api.c: $(YAML_BUILD)/include/yaml.h

$(YAML_BUILD)/include/yaml.h: $(YAML_BUILD)/Makefile
	@:

$(YAML_BUILD)/Makefile:
	mkdir -p deps && \
	cd deps && \
	rm -rf libyaml libyaml-0.2.5 libyaml.tar.gz && \
	curl -L -o libyaml.tar.gz https://codeload.github.com/yaml/libyaml/tar.gz/refs/tags/0.2.5 && \
	tar xzf libyaml.tar.gz && \
	mv libyaml-0.2.5 libyaml && \
	rm libyaml.tar.gz

$(LIB_TARGET): $(YAML_STATIC_LIB)
$(SO_TARGET): $(YAML_STATIC_LIB)
$(EXAMPLE_BIN): $(YAML_STATIC_LIB)
$(BUILD_DIR)/test_%: $(YAML_STATIC_LIB)
$(BUILD_DIR)/verify_config: $(YAML_STATIC_LIB)
endif

CORE_SRCS = config.c formatter.c dispatcher.c queue.c async.c log.c rotate.c rate_limit.c signal_handler.c
SINK_SRCS = console_sink.c file_sink.c socket_sink.c custom_sink.c syslog_sink.c
CLOG_SRCS = $(addprefix core/,$(CORE_SRCS)) $(addprefix sinks/,$(SINK_SRCS))
CORE_OBJS = $(addprefix $(BUILD_DIR)/,$(CORE_SRCS:.c=.o))
SINK_OBJS = $(addprefix $(BUILD_DIR)/,$(SINK_SRCS:.c=.o))
ALL_OBJS = $(CORE_OBJS) $(SINK_OBJS)

TESTS = test_async_lifecycle test_async_reload test_dispatcher_lifecycle \
        test_file_rotate test_file_mkdir test_config_reload \
        test_pipeline verify_config \
        test_invalid_config test_double_init test_empty_sink \
        test_async_fallback test_queue_try_put test_max_size test_console_stderr \
        test_module_trunc test_add_sink \
        test_multithread_sync test_config_set test_boundary_config \
        test_socket_sink test_sink_level test_log_level test_json_formatter \
        test_rate_limit test_fork_safety test_signal_handler test_custom_sink \
        test_observability_stats test_syslog_sink test_thread_context test_coverage_boost test_mutex_guard_raii test_coverage_deep
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TESTS))

BENCHMARK_SOURCES = $(wildcard benchmarks/*.c)
BENCHMARK_BINS = $(patsubst benchmarks/%.c,$(BUILD_DIR)/benchmark_%,$(BENCHMARK_SOURCES))

# Sanitizer configs (O1 -g for meaningful stack traces)
ASAN_CFLAGS = -std=c99 -Wall -Wextra -Wconversion -Iinclude -O1 -g -D_GNU_SOURCE -fPIC -fvisibility=hidden -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls
UBSAN_CFLAGS = -std=c99 -Wall -Wextra -Wconversion -Iinclude -O1 -g -D_GNU_SOURCE -fPIC -fvisibility=hidden -fsanitize=undefined

VERSION = 0.1.0
SO_VERSION = 0
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

PUBLIC_HEADERS := include/log.h include/log_config.h include/log_limits.h include/log_record.h include/log_sink.h

.PHONY: all clean example test docs format check-format check test-valgrind install uninstall asan ubsan test-asan test-ubsan fuzz-build fuzz-config fuzz-formatter benchmark

FORMAT_FILES := $(shell find include core sinks example tests fuzz benchmarks -name '*.c' -o -name '*.h' 2>/dev/null)

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
	$(CC) $(EXTRA_LDFLAGS) -shared -Wl,-soname,libclogx.so.$(SO_VERSION) -o $@ $^ $(LDFLAGS)

$(EXAMPLE_BIN): example/main.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ example/main.c $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/verify_config: tests/verify_config.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/fuzz_config: fuzz/fuzz_config.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/fuzz_formatter: fuzz/fuzz_formatter.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/fuzz_pipeline: fuzz/fuzz_pipeline.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS)

$(BUILD_DIR)/benchmark_%: benchmarks/%.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) $(EXTRA_LDFLAGS) $(CFLAGS) -o $@ $< $(LIB_TARGET) $(LDFLAGS) -lm

fuzz-build: $(BUILD_DIR)/fuzz_config $(BUILD_DIR)/fuzz_formatter $(BUILD_DIR)/fuzz_pipeline

fuzz-config: $(BUILD_DIR)/fuzz_config
	@mkdir -p fuzz/out_config
	afl-fuzz -i fuzz/seeds -o fuzz/out_config -- $(BUILD_DIR)/fuzz_config @@

fuzz-formatter: $(BUILD_DIR)/fuzz_formatter
	@mkdir -p fuzz/out_formatter
	afl-fuzz -i fuzz/seeds -o fuzz/out_formatter -- $(BUILD_DIR)/fuzz_formatter @@

fuzz-pipeline: $(BUILD_DIR)/fuzz_pipeline
	@mkdir -p fuzz/out_pipeline
	afl-fuzz -i fuzz/seeds -o fuzz/out_pipeline -- $(BUILD_DIR)/fuzz_pipeline @@

example: $(EXAMPLE_BIN)

test: $(TEST_BINS)
	@mkdir -p logs
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		$$t || status=1; \
	done; \
	exit $$status

benchmark: $(BENCHMARK_BINS)
	@mkdir -p logs
	@for b in $(BENCHMARK_BINS); do \
		echo "=== $$b ==="; \
		$$b; \
	done

## Sanitizer convenience targets

asan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(ASAN_CFLAGS)" EXTRA_LDFLAGS="$(ASAN_CFLAGS)"

ubsan:
	$(MAKE) clean
	$(MAKE) test CC=$(CC) CFLAGS="$(UBSAN_CFLAGS)" EXTRA_LDFLAGS="$(UBSAN_CFLAGS)"

ASAN_SO := $(shell $(CC) -print-file-name=libasan.so 2>/dev/null)

test-asan:
	$(MAKE) clean
	$(MAKE) $(TEST_BINS) CC=$(CC) CFLAGS="$(ASAN_CFLAGS)" EXTRA_LDFLAGS="$(ASAN_CFLAGS)"
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		if [ -f "$(ASAN_SO)" ]; then \
			LD_PRELOAD="$(ASAN_SO)" ASAN_OPTIONS=detect_leaks=0 ./$$t || status=1; \
		else \
			ASAN_OPTIONS=detect_leaks=0 ./$$t || status=1; \
		fi; \
	done; \
	exit $$status

test-ubsan:
	$(MAKE) clean
	$(MAKE) all CC=$(CC) CFLAGS="$(UBSAN_CFLAGS)" EXTRA_LDFLAGS="$(UBSAN_CFLAGS)"
	$(MAKE) test CC=$(CC) CFLAGS="$(UBSAN_CFLAGS)" EXTRA_LDFLAGS="$(UBSAN_CFLAGS)"

coverage:
	@command -v lcov >/dev/null || { echo "lcov not found; install it to generate coverage report"; exit 1; }
	$(MAKE) clean
	$(MAKE) test CC=gcc CFLAGS="$(CFLAGS) -O0 -g --coverage -fprofile-arcs -ftest-coverage -fprofile-update=atomic" EXTRA_LDFLAGS="$(EXTRA_LDFLAGS) --coverage"
	lcov --capture --rc branch_coverage=1 --ignore-errors unused,negative,empty --directory build --output-file coverage.info || lcov --capture --rc branch_coverage=1 --directory build --output-file coverage.info
	lcov --remove coverage.info 'tests/*' 'deps/*' 'example/*' 'benchmarks/*' 'fuzz/*' 'include/*' --rc branch_coverage=1 --ignore-errors unused,negative,empty --output-file coverage.info || lcov --remove coverage.info 'tests/*' --rc branch_coverage=1 --output-file coverage.info
	lcov --summary coverage.info --rc branch_coverage=1 --ignore-errors unused,negative,empty || true

## Quality check

check:
	$(MAKE) check-format
	@if command -v clang-tidy >/dev/null 2>&1; then $(MAKE) check-tidy; fi
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test
	@echo "=== check passed ==="

test-valgrind:
	@command -v valgrind >/dev/null || { echo "valgrind not found; test-valgrind skipped"; exit 0; }
	$(MAKE) clean
	$(MAKE) test
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "=== valgrind $$t ==="; \
		valgrind --leak-check=full --error-exitcode=1 --track-origins=yes --child-silent-after-fork=yes -q $$t 2>&1; \
	done; \
	exit $$status

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

tidy:
	@command -v clang-tidy >/dev/null || { echo "clang-tidy not found"; exit 1; }
	clang-tidy $(CLOG_SRCS) -- -Iinclude -D_GNU_SOURCE

check-tidy:
	@command -v clang-tidy >/dev/null || { echo "clang-tidy not found; check-tidy skipped"; exit 0; }
	clang-tidy $(CLOG_SRCS) -- -Iinclude -D_GNU_SOURCE

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
