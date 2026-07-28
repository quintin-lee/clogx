#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "log.h"

#define NUM_THREADS 8
#define LOGS_PER_THREAD 100
#define CONFIG_PATH "build/config_mt_sync.yaml"
#define LOG_PATH "logs/mt_sync.log"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;

    fprintf(f,
            "log:\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '[%%level] [%%module] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  max_size: 100MB\n"
            "  backups: 3\n"
            "  socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

typedef struct {
    int id;
    int errors;
} thread_result_t;

static void *writer_thread(void *arg) {
    thread_result_t *res = (thread_result_t *)arg;
    char msg[64];

    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        snprintf(msg, sizeof(msg), "thread-%d msg-%d", res->id, i);
        LOG_INFO("%s", msg);
    }

    return NULL;
}

int main(void) {
    remove(LOG_PATH);

    if (write_config() != 0) {
        fprintf(stderr, "write config failed\n");
        return 1;
    }

    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    clog_thread_t threads[NUM_THREADS];
    thread_result_t results[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        results[i].id = i;
        results[i].errors = 0;
        if (clog_thread_create(&threads[i], writer_thread, &results[i]) != 0) {
            fprintf(stderr, "clog_thread_create failed\n");
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        clog_thread_join(threads[i]);
    }

    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        fprintf(stderr, "log file missing\n");
        return 1;
    }

    int lines = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        lines++;
    }
    fclose(f);

    int expected = NUM_THREADS * LOGS_PER_THREAD;
    printf("sync mt test: %d/%d log lines\n", lines, expected);

    if (lines != expected) {
        fprintf(stderr, "expected %d lines, got %d\n", expected, lines);
        return 1;
    }

    return 0;
}
