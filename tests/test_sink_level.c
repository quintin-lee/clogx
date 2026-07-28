#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "log_sink.h"

#define LOG_PATH "logs/sink_level.log"

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) n++;
    fclose(f);
    return n;
}

int main(void) {
    remove(LOG_PATH);

    /* Init with no config file — no sinks enabled yet. */
    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init (null) failed\n");
        return 1;
    }
    log_set_level(LOG_LEVEL_TRACE);

    log_sink_t *sink = file_sink_create(LOG_PATH, 0, 0);
    if (!sink) {
        fprintf(stderr, "file_sink_create failed\n");
        return 1;
    }

    /* Only accept WARN and above for this sink. */
    log_sink_set_level(sink, LOG_LEVEL_WARN);

    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "log_add_sink failed\n");
        return 1;
    }

    /* These should be filtered out (below WARN). */
    LOG_TRACE("trace-msg");
    LOG_DEBUG("debug-msg");
    LOG_INFO("info-msg");

    /* These should pass through. */
    LOG_WARN("warn-msg");
    LOG_ERROR("error-msg");
    LOG_FATAL("fatal-msg");

    log_flush();
    log_destroy();

    int lines = count_lines(LOG_PATH);
    printf("sink level test: %d/3 lines\n", lines);
    if (lines != 3) {
        fprintf(stderr, "expected 3 lines (WARN+), got %d\n", lines);
        return 1;
    }

    /* Quick content check */
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return 1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "TRACE") || strstr(line, "DEBUG") || strstr(line, "INFO")) {
            fprintf(stderr, "unexpected low-level line: %s", line);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    return 0;
}
