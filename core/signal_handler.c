/**
 * @file signal_handler.c
 * @brief Graceful shutdown signal handler implementation.
 */

#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include "log_signal.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_signal_pending = 0;
static struct sigaction g_old_sigterm;
static struct sigaction g_old_sigint;
static bool g_installed = false;

void log_signal_handler(int sig) {
    /* Pure Async-Signal-Safe handler: only set flag */
    g_signal_pending = sig;
}

int log_get_pending_signal(void) {
    return (int)g_signal_pending;
}

void log_process_pending_signals(void) {
    int sig = (int)g_signal_pending;
    if (sig == 0) {
        return;
    }
    g_signal_pending = 0;

    /* Gracefully flush all pending log records in main execution context */
    log_flush();

    /* Restore previous/default signal handler and re-raise */
    if (g_installed) {
        if (sig == SIGTERM) {
            sigaction(SIGTERM, &g_old_sigterm, NULL);
        } else if (sig == SIGINT) {
            sigaction(SIGINT, &g_old_sigint, NULL);
        }
    } else {
        signal(sig, SIG_DFL);
    }

    raise(sig);
}

clogx_errno_t log_install_signal_handlers(void) {
    if (g_installed) {
        return CLOG_OK;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = log_signal_handler;
    sa.sa_flags = (int)SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGTERM, &sa, &g_old_sigterm) != 0) {
        return CLOG_ERR_INVALID_ARG;
    }
    if (sigaction(SIGINT, &sa, &g_old_sigint) != 0) {
        sigaction(SIGTERM, &g_old_sigterm, NULL);
        return CLOG_ERR_INVALID_ARG;
    }

    g_installed = true;
    g_signal_pending = 0;
    return CLOG_OK;
}

void log_restore_signal_handlers(void) {
    if (!g_installed) {
        return;
    }
    sigaction(SIGTERM, &g_old_sigterm, NULL);
    sigaction(SIGINT, &g_old_sigint, NULL);
    g_installed = false;
    g_signal_pending = 0;
}
