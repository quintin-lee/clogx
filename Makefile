CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Iinclude -O2 -D_GNU_SOURCE
LDFLAGS = -lpthread
BUILD_DIR = build
LIB_TARGET = $(BUILD_DIR)/libclog.a

# Object files from core and sinks (built first)
CORE_OBJS = $(BUILD_DIR)/config.o $(BUILD_DIR)/formatter.o $(BUILD_DIR)/dispatcher.o \
            $(BUILD_DIR)/queue.o $(BUILD_DIR)/async.o $(BUILD_DIR)/log.o $(BUILD_DIR)/rotate.o
SINK_OBJS = $(BUILD_DIR)/console_sink.o $(BUILD_DIR)/file_sink.o $(BUILD_DIR)/socket_sink.o
ALL_OBJS = $(CORE_OBJS) $(SINK_OBJS)

.PHONY: all clean example

all: $(LIB_TARGET) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

# Compile each core source file
$(BUILD_DIR)/config.o: core/config.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/formatter.o: core/formatter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/dispatcher.o: core/dispatcher.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/queue.o: core/queue.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/async.o: core/async.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/log.o: core/log.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rotate.o: core/rotate.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/console_sink.o: sinks/console_sink.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/file_sink.o: sinks/file_sink.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/socket_sink.o: sinks/socket_sink.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_TARGET): $(ALL_OBJS)
	ar rcs $@ $^

# Build example by compiling main.c and linking with the library
EXAMPLE_BIN = $(BUILD_DIR)/example
$(EXAMPLE_BIN): example/main.c $(LIB_TARGET) | $(BUILD_DIR)
	$(CC) -Iinclude -o $@ example/main.c $(ALL_OBJS) $(LDFLAGS)

example: $(EXAMPLE_BIN)

clean:
	rm -rf $(BUILD_DIR)
