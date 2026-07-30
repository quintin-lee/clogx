/**
 * @file signal_handler.c
 * @brief Graceful shutdown signal handler implementation with POSIX self-pipe trick.
 */

#include "clog_port.h"
#include "log.h"
#include "log_signal.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

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

int log_get_signal_fd(void) {
    return -1;
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
static int g_signal_pipe[2] = {-1, -1};

static void setup_self_pipe(void) {
    if (g_signal_pipe[0] >= 0) {
        return;
    }
#if defined(F_SETFD) && defined(FD_CLOEXEC) && defined(O_NONBLOCK)
    /* LCOV_EXCL_START - System call failures */
    if (pipe(g_signal_pipe) == 0) {
        for (int i = 0; i < 2; i++) {
            int flags = fcntl(g_signal_pipe[i], F_GETFL, 0);
            if (flags >= 0) {
                fcntl(g_signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
            }
            flags = fcntl(g_signal_pipe[i], F_GETFD, 0);
            if (flags >= 0) {
                fcntl(g_signal_pipe[i], F_SETFD, flags | FD_CLOEXEC);
            }
        }
    }
    /* LCOV_EXCL_STOP */
#endif
}

static void close_self_pipe(void) {
    for (int i = 0; i < 2; i++) {
        if (g_signal_pipe[i] >= 0) {
            close(g_signal_pipe[i]);
            g_signal_pipe[i] = -1;
        }
    }
}

int log_get_signal_fd(void) {
    return g_signal_pipe[0];
}

void log_signal_handler(int sig) {
    /* Pure Async-Signal-Safe handler: set flag and write to non-blocking self-pipe (zero locks) */
    g_signal_pending = sig;
    if (g_signal_pipe[1] >= 0) {
        unsigned char ch = (unsigned char)sig;
        ssize_t res = write(g_signal_pipe[1], &ch, 1);
        (void)res;
    }
}

int log_get_pending_signal(void) {
    return (int)g_signal_pending;
}

void log_process_pending_signals(void) {
    int sig = (int)g_signal_pending;
    if (sig == 0 && g_signal_pipe[0] >= 0) {
        unsigned char ch = 0;
        if (read(g_signal_pipe[0], &ch, 1) > 0) {
            sig = (int)ch;
        }
    }
    if (sig == 0) {
        return;
    }
    g_signal_pending = 0;

    /* Drain any remaining bytes in self-pipe */
    if (g_signal_pipe[0] >= 0) {
        unsigned char buf[16];
        while (read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {
            /* Drain pipe */
        }
    }

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

    setup_self_pipe();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = log_signal_handler;
    sa.sa_flags = (int)SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    /* LCOV_EXCL_START - System call failures (requires seccomp/rlimit to simulate) */
    if (sigaction(SIGTERM, &sa, &g_old_sigterm) != 0) {
        close_self_pipe();
        return CLOG_ERR_INVALID_ARG;
    }
    if (sigaction(SIGINT, &sa, &g_old_sigint) != 0) {
        sigaction(SIGTERM, &g_old_sigterm, NULL);
        close_self_pipe();
        return CLOG_ERR_INVALID_ARG;
    }
    /* LCOV_EXCL_STOP */

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
    close_self_pipe();
    g_installed = false;
    g_signal_pending = 0;
}
#endif
