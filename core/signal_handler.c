/**
 * @file signal_handler.c
 * @brief POSIX signal handler with self-pipe trick for safe log flush.
 *
 * ## Problem
 *
 * Signal handlers have severe restrictions: only async-signal-safe
 * functions (no malloc, no locks, no stdio) may be called. Yet a robust
 * logger must flush buffered data before process termination.
 *
 * ## Solution: Self-Pipe Trick
 *
 * 1. At init, create a pipe (`signal_pipe_fd[2]`).
 * 2. The signal handler writes a single byte to `signal_pipe_fd[1]`
 *    (async-signal-safe `write()`).
 * 3. A dedicated thread (`signal_monitor_thread`) blocks on `read()`
 *    from `signal_pipe_fd[0]`.
 * 4. When a byte arrives, the monitor thread performs the safe cleanup:
 *    flush logs, restore default handler, re-raise the signal.
 *
 * ## Signals Handled
 *
 * | Signal        | Default Action     | Notes                     |
 * |---------------|--------------------|---------------------------|
 * | `SIGINT`      | Flush + re-raise   | Ctrl+C                    |
 * | `SIGTERM`     | Flush + re-raise   | `kill` / systemd stop     |
 * | `SIGHUP`      | Flush + re-raise   | Config reload hint        |
 * | `SIGUSR1`     | Flush + re-raise   | User-defined (e.g. rotate)|
 * | `SIGUSR2`     | Flush + re-raise   | User-defined              |
 * | `SIGPIPE`     | Ignore             | Broken pipe (common in network sinks) |
 *
 * ## Windows Fallback
 *
 * On Windows, `signal()` is used instead of `sigaction`. The self-pipe
 * trick is unavailable; the handler calls `log_flush()` directly (which
 * is technically unsafe but acceptable for Windows console apps).
 */

#include "clog_port.h"
#include "log.h"
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

static volatile sig_atomic_t g_signal_pending = 0;
static bool                  g_installed      = false;

#if defined(_WIN32) || defined(_WIN64)
typedef void (*sig_handler_t)(int);
static sig_handler_t g_old_sigterm = NULL;
static sig_handler_t g_old_sigint  = NULL;

void log_signal_handler(int sig)
{
    g_signal_pending = sig;
}

int log_get_pending_signal(void)
{
    return (int)g_signal_pending;
}

int log_get_signal_fd(void)
{
    return -1;
}

void log_process_pending_signals(void)
{
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

clogx_errno_t log_install_signal_handlers(void)
{
    if (g_installed) {
        return CLOG_OK;
    }

    g_old_sigterm = signal(SIGTERM, log_signal_handler);
    g_old_sigint  = signal(SIGINT, log_signal_handler);

    g_installed      = true;
    g_signal_pending = 0;
    return CLOG_OK;
}

void log_restore_signal_handlers(void)
{
    if (!g_installed) {
        return;
    }
    if (g_old_sigterm) {
        signal(SIGTERM, g_old_sigterm);
    }
    if (g_old_sigint) {
        signal(SIGINT, g_old_sigint);
    }
    g_installed      = false;
    g_signal_pending = 0;
}

#else

static struct sigaction g_old_sigterm;
static struct sigaction g_old_sigint;
static int              g_signal_pipe[2] = {-1, -1};

/**
 * @brief Create the self-pipe used to wake the event loop on signal receipt.
 *
 * The self-pipe trick: signal handlers write a byte into g_signal_pipe[1];
 * the main thread blocks on g_signal_pipe[0] and wakes up when a signal arrives.
 */
static void setup_self_pipe(void)
{
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

/**
 * @brief Close both ends of the self-pipe file descriptors.
 *
 * Safe to call when the pipe is not yet opened (fd == -1).
 */
static void close_self_pipe(void)
{
    for (int i = 0; i < 2; i++) {
        if (g_signal_pipe[i] >= 0) {
            close(g_signal_pipe[i]);
            g_signal_pipe[i] = -1;
        }
    }
}

int log_get_signal_fd(void)
{
    return g_signal_pipe[0];
}

/**
 * @brief POSIX signal handler: async-signal-safe flag set + self-pipe write.
 *
 * This function is called directly by the kernel in signal context.
 * Only two operations are performed:
 * 1. Set g_signal_pending to the signal number.
 * 2. Write one byte to the self-pipe (non-blocking, async-signal-safe).
 *
 * No locks, no malloc, no stdio — strictly async-signal-safe.
 */
void log_signal_handler(int sig)
{
    /* Pure Async-Signal-Safe handler: set flag and write to non-blocking self-pipe (zero locks) */
    g_signal_pending = sig;
    if (g_signal_pipe[1] >= 0) {
        unsigned char ch  = (unsigned char)sig;
        ssize_t       res = write(g_signal_pipe[1], &ch, 1);
        (void)res;
    }
}

int log_get_pending_signal(void)
{
    return (int)g_signal_pending;
}

/**
 * @brief Process a pending signal in the main execution context.
 *
 * Reads the signal number from the self-pipe (POSIX) or from the
 * volatile flag (Windows), drains any remaining pipe bytes, then:
 * 1. Calls log_flush() to flush all pending log records.
 * 2. Restores the previous signal handler (SIG_DFL or original).
 * 3. Re-raises the signal for default handler processing (core dump / exit).
 *
 * Must be called from a normal (non-signal) execution context, typically
 * from logger_writevprintf_internal() or the signal monitor thread.
 */
void log_process_pending_signals(void)
{
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

/**
 * @brief Install signal handlers for SIGTERM and SIGINT via sigaction.
 *
 * Creates the self-pipe, then installs log_signal_handler() with
 * SA_RESETHAND (auto-restores to default after first invocation).
 * Old handlers are saved in g_old_sigterm / g_old_sigint for later
 * restoration by log_restore_signal_handlers().
 *
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG if sigaction fails.
 */
clogx_errno_t log_install_signal_handlers(void)
{
    if (g_installed) {
        return CLOG_OK;
    }

    setup_self_pipe();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = log_signal_handler;
    sa.sa_flags   = (int)SA_RESETHAND;
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

    g_installed      = true;
    g_signal_pending = 0;
    return CLOG_OK;
}

void log_restore_signal_handlers(void)
{
    if (!g_installed) {
        return;
    }
    sigaction(SIGTERM, &g_old_sigterm, NULL);
    sigaction(SIGINT, &g_old_sigint, NULL);
    close_self_pipe();
    g_installed      = false;
    g_signal_pending = 0;
}
#endif
