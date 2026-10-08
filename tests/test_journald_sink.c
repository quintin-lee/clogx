/**
 * @file test_journald_sink.c
 * @brief Tests for the native journald sink: factory semantics + journal round-trip.
 *
 * Layer 1 (runs everywhere): factory NULL/ident validation, platform skip.
 * Layer 2 (journal machines only): write one ERROR record with a unique token,
 * flush, then read it back via journalctl and check token + PRIORITY=3.
 * Missing socket or missing journalctl -> skip whole layer, exit 0.
 */

#include "log.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JOURNAL_SOCKET_PATH "/run/systemd/journal/socket"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                                \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static int has_journal_socket(void)
{
    return access(JOURNAL_SOCKET_PATH, W_OK) == 0;
}

static int has_journalctl(void)
{
    /* system(3) return is shell status; check exit code of the command. */
    int rc = system("journalctl --version >/dev/null 2>&1");
    return rc == 0;
}

static void test_factory_bad_ident(void)
{
#if defined(__linux__)
    if (!has_journal_socket()) {
        fprintf(stderr, "SKIP: no journal socket, factory test skipped\n");
        return;
    }
    log_sink_t *bad = journald_sink_create("has\nnewline");
    CHECK(bad == NULL, "ident with newline must be rejected");
    /* bad is NULL on success path; nothing to destroy. */
#else
    log_sink_t *s = journald_sink_create("test_clogx");
    CHECK(s == NULL, "non-Linux factory must return NULL");
    fprintf(stderr, "SKIP: journald sink not supported on this platform\n");
#endif
}

static void test_round_trip(void)
{
#if defined(__linux__)
    char  token[128];
    char  cmd[512];
    char  line[1024];
    FILE *fp;
    int   found_token   = 0;
    int   found_prio    = 0;
    int   journal_calls = 0;

    if (!has_journal_socket() || !has_journalctl()) {
        fprintf(stderr, "SKIP: no journal socket or journalctl, round-trip skipped\n");
        return;
    }
    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init failed\n");
        g_failures++;
        return;
    }
    log_sink_t *sink = journald_sink_create("test_clogx_journald");
    if (!sink) {
        fprintf(stderr, "FAIL: factory returned NULL despite journal socket\n");
        g_failures++;
        log_destroy();
        return;
    }
    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "FAIL: log_add_sink failed\n");
        g_failures++;
        log_destroy();
        return;
    }
    snprintf(token, sizeof(token), "clogx_journald_%d", (int)getpid());
    LOG_ERROR("%s", token);
    log_flush();

    snprintf(cmd,
             sizeof(cmd),
             "journalctl --since \"1 minute ago\" -t test_clogx_journald -o verbose 2>/dev/null");
    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "FAIL: popen journalctl failed\n");
        g_failures++;
        log_destroy();
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        journal_calls++;
        if (strstr(line, token)) {
            found_token = 1;
        }
        if (strstr(line, "PRIORITY=3")) {
            found_prio = 1;
        }
    }
    pclose(fp);
    log_destroy();
    CHECK(journal_calls > 0, "journalctl returned no output (access denied?)");
    CHECK(found_token, "token not found in journal");
    CHECK(found_prio, "PRIORITY=3 not found in journal");
#else
    fprintf(stderr, "SKIP: round-trip test needs Linux with systemd\n");
#endif
}

int main(void)
{
    test_factory_bad_ident();
    test_round_trip();
    if (g_failures == 0) {
        fprintf(stderr, "journald sink test passed\n");
        return 0;
    }
    fprintf(stderr, "journald sink test FAILED (%d failures)\n", g_failures);
    return 1;
}
