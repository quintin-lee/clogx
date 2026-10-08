/**
 * @file test_sync_hotpath.c
 * @brief Semantics locks for the sync hot-path optimization: pid stays
 * correct across fork (cached_pid + atfork refresh), and concurrent
 * set_module never corrupts line integrity (module seqlock).
 */

#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONFIG_PATH "build/config_sync_hotpath_test.yaml"
#define LOG_PATH "logs/sync_hotpath_test.log"

static int write_config(const char *format)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return -1;
    }
    fprintf(f,
            "log:\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '%s'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  socket_enable: false\n",
            format,
            LOG_PATH);
    fclose(f);
    return 0;
}

static int check(int ok, const char *msg)
{
    if (!ok) {
        fprintf(stderr, "%s\n", msg);
    }
    return ok ? 1 : 0;
}

static int test_fork_pid(void)
{
    remove(LOG_PATH);
    if (write_config("[%pid] %msg") != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "fork_pid: init failed\n");
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "fork_pid: fork failed\n");
        log_destroy();
        return 1;
    }
    if (child == 0) {
        LOG_INFO("child-line");
        log_flush();
        _exit(0);
    }

    int   status = 0;
    pid_t waited = waitpid(child, &status, 0);
    log_destroy();
    if (!check(waited == child, "fork_pid: waitpid failed")) {
        return 1;
    }
    if (!check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork_pid: child failed")) {
        return 1;
    }

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "[%d]", (int)child);

    FILE *f = fopen(LOG_PATH, "rb");
    if (!check(f != NULL, "fork_pid: missing log file")) {
        return 1;
    }
    char   buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!check(strstr(buf, pid_str) != NULL, "fork_pid: child pid missing")) {
        fprintf(stderr, "got:\n%s\n", buf);
        return 1;
    }
    fprintf(stderr, "fork_pid: ok (child pid %d in log)\n", (int)child);
    return 0;
}

#define RACE_THREADS 8
#define RACE_LINES 10000

static void *race_worker(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < RACE_LINES; i++) {
        if (i % 1000 == 0) {
            log_set_module(id % 2 == 0 ? "alpha" : "beta");
        }
        LOG_INFO("pressure-%ld-%d", id, i);
    }
    return NULL;
}

static int test_module_race(void)
{
    remove(LOG_PATH);
    if (write_config("[%module] %msg") != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "module_race: init failed\n");
        return 1;
    }

    pthread_t workers[RACE_THREADS];
    for (long i = 0; i < RACE_THREADS; i++) {
        if (pthread_create(&workers[i], NULL, race_worker, (void *)i) != 0) {
            fprintf(stderr, "module_race: thread create failed\n");
            log_destroy();
            return 1;
        }
    }
    for (int i = 0; i < RACE_THREADS; i++) {
        pthread_join(workers[i], NULL);
    }
    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "rb");
    if (!check(f != NULL, "module_race: missing log file")) {
        return 1;
    }
    int    lines = 0;
    int    c;
    int    prev = '\n';
    int    bad  = 0;
    char   line[1024];
    size_t llen = 0;
    while ((c = fgetc(f)) != EOF) {
        if (llen < sizeof(line) - 1) {
            line[llen++] = (char)c;
        }
        if (c == '\n') {
            line[llen] = '\0';
            lines++;
            if (strstr(line, "pressure-") == NULL) {
                bad = 1;
                fprintf(stderr, "module_race: garbled line: %s\n", line);
                break;
            }
            llen = 0;
        }
        prev = c;
    }
    fclose(f);
    (void)prev;
    if (bad) {
        return 1;
    }
    if (!check(lines == RACE_THREADS * RACE_LINES, "module_race: line count mismatch")) {
        fprintf(stderr, "got %d lines\n", lines);
        return 1;
    }
    fprintf(stderr, "module_race: ok (%d lines, no garbling)\n", lines);
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_fork_pid();
    rc |= test_module_race();
    if (rc == 0) {
        fprintf(stderr, "sync_hotpath: all semantics locks hold\n");
    }
    return rc;
}
