#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "log.h"

#define CONFIG_PATH "build/config_signal_test.yaml"
#define LOG_PATH "logs/signal_test.log"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f,
            "log:\n"
            "  async: true\n"
            "  queue_size: 1024\n"
            "  catch_signals: true\n"
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

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child process initializes clogx and sends SIGTERM to itself */
        if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
            _exit(1);
        }

        LOG_INFO("signal-test-msg-before-sigterm");
        raise(SIGINT);
        log_process_pending_signals();
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    /* Verify child process was terminated by SIGINT */
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGINT) {
        fprintf(stderr, "expected child terminated by SIGINT (%d), got status %d\n", SIGINT,
                status);
        return 1;
    }

    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        fprintf(stderr, "missing signal log file\n");
        return 1;
    }

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, "signal-test-msg-before-sigterm")) {
        fprintf(stderr, "signal message not flushed: %s\n", buf);
        return 1;
    }

    printf("signal handler test passed\n");
    return 0;
}
