#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log_sink.h"

#define LOG_PATH "logs/rotate_unit.log"
#define BACKUP_PATH "logs/rotate_unit.log.1"

static void cleanup(void) {
    remove(LOG_PATH);
    remove(BACKUP_PATH);
    remove("logs/rotate_unit.log.2");
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

int main(void) {
    cleanup();

    /* Tiny rotation threshold so a few writes trigger rotate */
    log_sink_t *sink = file_sink_create(LOG_PATH, 64, 2);
    if (!sink) {
        fprintf(stderr, "file_sink_create failed\n");
        return 1;
    }

    const char *chunk = "abcdefghijklmnopqrstuvwxyz012345\n"; /* 33 bytes */
    /* Two chunks (66) exceed max_size 64 and trigger rotation. */
    for (int i = 0; i < 2; i++) {
        if (sink->write(sink, chunk, strlen(chunk)) < 0) {
            fprintf(stderr, "write %d failed\n", i);
            sink->destroy(sink);
            return 1;
        }
    }

    const char *after = "post-rotation-line\n";
    if (sink->write(sink, after, strlen(after)) < 0) {
        fprintf(stderr, "post-rotation write failed\n");
        sink->destroy(sink);
        return 1;
    }
    sink->flush(sink);
    sink->destroy(sink);

    long active = file_size(LOG_PATH);
    long backup = file_size(BACKUP_PATH);
    printf("file rotate: active=%ld backup=%ld\n", active, backup);

    if (backup <= 0) {
        fprintf(stderr, "expected rotated backup %s\n", BACKUP_PATH);
        return 1;
    }
    if (active != (long)strlen(after)) {
        fprintf(stderr, "expected active size %zu, got %ld\n", strlen(after), active);
        return 1;
    }

    FILE *f = fopen(LOG_PATH, "r");
    if (!f)
        return 1;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    if (strcmp(buf, after) != 0) {
        fprintf(stderr, "active log content corrupted: '%s'\n", buf);
        return 1;
    }

    cleanup();
    return 0;
}
