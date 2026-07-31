/**
 * @file test_file_mkdir.c
 * @brief Regression test: file sink auto-creates intermediate directories.
 */

#include "clog_port.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DIR_PATH "logs/nested_dir_test"
#define LOG_PATH "logs/nested_dir_test/app.log"

static void cleanup(void)
{
    remove(LOG_PATH);
    rmdir(DIR_PATH);
}

int main(void)
{
    cleanup();

    log_sink_t *sink = file_sink_create(LOG_PATH, 1024 * 1024, 2);
    if (!sink) {
        fprintf(stderr, "file_sink_create should create parent dirs\n");
        return 1;
    }

    const char *msg = "hello nested\n";
    if (sink->write(sink, msg, strlen(msg)) < 0) {
        fprintf(stderr, "write failed\n");
        sink->destroy(sink);
        return 1;
    }
    sink->flush(sink);
    sink->destroy(sink);

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "log file missing under nested dir\n");
        return 1;
    }
    char buf[64] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    printf("mkdir sink: '%s'\n", buf);
    if (strcmp(buf, msg) != 0) {
        fprintf(stderr, "unexpected content\n");
        return 1;
    }

    cleanup();
    return 0;
}
