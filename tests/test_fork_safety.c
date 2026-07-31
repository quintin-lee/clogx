/**
 * @file test_fork_safety.c
 * @brief Tests fork safety: pthread_atfork handlers and child-process recovery.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
int main(void) {
    printf("fork safety test skipped on Windows\n");
    return 0;
}
#else
#include "clog_port.h"
#include <sys/wait.h>
#include "log.h"

#define CONFIG_PATH "build/config_fork_test.yaml"
#define LOG_PATH "logs/fork_test.log"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f,
            "log:\n"
            "  async: true\n"
            "  queue_size: 1024\n"
            "  color: false\n"
            "  format: \"[%%level] %%msg\\n\"\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

int main(void) {
    remove(LOG_PATH);
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "fork test log_init failed\n");
        return 1;
    }

    LOG_INFO("parent-before-fork");
    log_flush();

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child process */
        LOG_INFO("child-after-fork-msg-1");
        LOG_INFO("child-after-fork-msg-2");
        log_flush();
        log_destroy();
        _exit(0);
    } else {
        /* Parent process */
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "child process failed\n");
            return 1;
        }

        LOG_INFO("parent-after-fork");
        log_flush();
        log_destroy();
    }

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "missing log file\n");
        return 1;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, "parent-before-fork")) {
        fprintf(stderr, "parent-before-fork missing\n");
        return 1;
    }
    if (!strstr(buf, "child-after-fork-msg-1")) {
        fprintf(stderr, "child-after-fork-msg-1 missing\n");
        return 1;
    }
    if (!strstr(buf, "parent-after-fork")) {
        fprintf(stderr, "parent-after-fork missing\n");
        return 1;
    }

    printf("fork safety test passed\n");
    return 0;
}
#endif
