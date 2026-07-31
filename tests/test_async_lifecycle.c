/**
 * @file test_async_lifecycle.c
 * @brief Tests async worker thread start, drain, and shutdown lifecycle.
 */
#include <stdio.h>
#include <string.h>
#include "clog_port.h"
#include "log.h"

#define NUM_THREADS 4
#define LOGS_PER_THREAD 50
#define CONFIG_PATH "build/config_async_test.yaml"
#define LOG_PATH "logs/async_test.log"

static int write_async_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;

    fprintf(f,
            "log:\n"
            "  async: true\n"
            "  queue_size: 8192\n"
            "  color: false\n"
            "  format: '[%%time] [%%level] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  max_size: 100MB\n"
            "  backups: 10\n"
            "  socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

static void *writer_thread(void *arg) {
    int id = *(int *)arg;
    char msg[64];

    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        snprintf(msg, sizeof(msg), "thread-%d msg-%d", id, i);
        LOG_INFO("%s", msg);
    }

    return NULL;
}

int main(void) {
    remove(LOG_PATH);

    if (write_async_config() != 0) {
        fprintf(stderr, "failed to write async test config\n");
        return 1;
    }

    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    clog_thread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        if (clog_thread_create(&threads[i], writer_thread, &ids[i]) != 0) {
            fprintf(stderr, "clog_thread_create failed\n");
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        clog_thread_join(threads[i]);
    }

    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "async log file missing\n");
        return 1;
    }

    int lines = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        lines++;
    }
    fclose(f);

    int expected = NUM_THREADS * LOGS_PER_THREAD;
    printf("async lifecycle test: %d/%d log lines\n", lines, expected);

    return lines == expected ? 0 : 1;
}
