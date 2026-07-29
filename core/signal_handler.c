/**
 * @file signal_handler.c
 * @brief Graceful shutdown signal handler implementation.
 */

#include "clog_port.h"
#include "log.h"
#include "log_signal.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_signal_pending = 0;
static bool g_installed = false;

#if defined(_WIN32) || defined(_WIN64)
typedef void (*sig_handler_t)(int);
static sig_handler_t g_old_sigterm = NULL;
static sig_handler_t g_old_sigint = NULL;

void log_signal_handler(int sig) {
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

    log_flush();

    if (g_installed) {
        if (sig == SIGTERM && g_old_sigterm) {
            signal(SIGTERM, g_old_sigterm);
        } else if (sig == SIGINT && g_old_sigint) {
            signal(SIGINT, g_old_sigint);
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

    g_old_sigterm = signal(SIGTERM, log_signal_handler);
    g_old_sigint = signal(SIGINT, log_signal_handler);

    g_installed = true;
    g_signal_pending = 0;
    return CLOG_OK;
}

void log_restore_signal_handlers(void) {
    if (!g_installed) {
        return;
    }
    if (g_old_sigterm) {
        signal(SIGTERM, g_old_sigterm);
    }
    if (g_old_sigint) {
        signal(SIGINT, g_old_sigint);
    }
    g_installed = false;
    g_signal_pending = 0;
}

#else

static struct sigaction g_old_sigterm;
static struct sigaction g_old_sigint;

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
#if defined(__GNUC__)
    extern void __gcov_dump(void) __attribute__((weak));
#endif

    if (g_installed) {
        if (sig == SIGTERM) {
            sigaction(SIGTERM, &g_old_sigterm, NULL);
        } else if (sig == SIGINT) {
            sigaction(SIGINT, &g_old_sigint, NULL);
        }
    } else {
        signal(sig, SIG_DFL);
    }

#if defined(__GNUC__)
    if (__gcov_dump) {
        __gcov_dump();
    }
#endif

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
#endif
