/**
 * @file test_otel.c
 * @brief Unit tests for OpenTelemetry TraceContext and OTLP formatting/sink.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "log_formatter.h"
#include "log_sink.h"

static void test_trace_context_hex(void) {
    clog_clear_trace_context();

    uint8_t tid[16];
    uint8_t sid[8];
    clog_get_trace_context(tid, sid);

    for (int i = 0; i < 16; i++)
        assert(tid[i] == 0);
    for (int i = 0; i < 8; i++)
        assert(sid[i] == 0);

    const char *tid_hex = "4bf92f3577b34da6a3ce929d0e0e4736";
    const char *sid_hex = "00f067aa0ba902b7";

    int ret = clog_set_trace_context_hex(tid_hex, sid_hex);
    assert(ret == CLOG_OK);

    clog_get_trace_context(tid, sid);
    assert(tid[0] == 0x4b && tid[1] == 0xf9);
    assert(sid[0] == 0x00 && sid[1] == 0xf0);

    clog_clear_trace_context();
    clog_get_trace_context(tid, sid);
    for (int i = 0; i < 16; i++)
        assert(tid[i] == 0);
    printf("test_trace_context_hex passed\n");
}

static void test_pattern_trace_tokens(void) {
    clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736", "00f067aa0ba902b7");

    log_formatter_init("[%level] [%trace_id:%span_id] %msg", NULL);

    log_record_t rec = {0};
    rec.level = LOG_LEVEL_INFO;
    rec.message = "otel test msg";
    clog_get_trace_context(rec.trace_id, rec.span_id);

    char buf[512];
    int len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(
        strstr(buf, "[INFO] [4bf92f3577b34da6a3ce929d0e0e4736:00f067aa0ba902b7] otel test msg") !=
        NULL);

    clog_clear_trace_context();
    log_formatter_reset();
    printf("test_pattern_trace_tokens passed\n");
}

static void test_otlp_formatter(void) {
    clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736", "00f067aa0ba902b7");

    log_record_t rec = {0};
    rec.level = LOG_LEVEL_WARN;
    rec.timestamp = 1625000000000000ULL;
    rec.module = "auth";
    rec.file = "auth.c";
    rec.line = 42;
    rec.func = "login";
    rec.message = "failed login attempt";
    clog_get_trace_context(rec.trace_id, rec.span_id);

    char buf[1024];
    int len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"severity\":\"WARN\"") != NULL);
    assert(strstr(buf, "\"severity_number\":13") != NULL);
    assert(strstr(buf, "\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != NULL);
    assert(strstr(buf, "\"span_id\":\"00f067aa0ba902b7\"") != NULL);
    assert(strstr(buf, "\"body\":\"failed login attempt\"") != NULL);

    clog_clear_trace_context();
    printf("test_otlp_formatter passed\n");
}

static void test_otlp_sink(void) {
    log_sink_t *sink = otlp_sink_create("stdout", "test_service");
    assert(sink != NULL);
    assert(sink->write != NULL);
    assert(sink->flush != NULL);
    assert(sink->destroy != NULL);

    int n = sink->write(sink, "{\"resourceLogs\":[]}", 19);
    assert(n > 0);
    sink->flush(sink);
    sink->destroy(sink);
    printf("test_otlp_sink passed\n");
}

static void test_traceparent_env(void) {
    clog_clear_trace_context();
    setenv("TRACEPARENT", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", 1);

    log_record_t rec = {0};
    rec.level = LOG_LEVEL_INFO;
    rec.timestamp = 1625000000000000ULL;
    rec.module = "auth";
    rec.file = "auth.c";
    rec.line = 42;
    rec.func = "login";
    rec.message = "traceparent from env";

    char buf[1024];
    int len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != NULL);
    assert(strstr(buf, "\"span_id\":\"00f067aa0ba902b7\"") != NULL);

    unsetenv("TRACEPARENT");
    clog_clear_trace_context();
    printf("test_traceparent_env passed\n");
}

static void test_uppercase_format_strings(void) {
    log_record_t rec = {0};
    rec.level = LOG_LEVEL_INFO;
    rec.timestamp = 1625000000000000ULL;
    rec.module = "test";
    rec.file = "test.c";
    rec.line = 1;
    rec.func = "main";
    rec.message = "uppercase test";

    char buf[1024];

    log_formatter_init("JSON", NULL);
    int len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "{\"timestamp\":") != NULL);
    assert(strstr(buf, "\"level\":\"INFO\"") != NULL);

    log_formatter_init("OTLP", NULL);
    rec.trace_id[0] = 0x4b;
    rec.span_id[0] = 0x00;
    rec.span_id[1] = 0xf0;
    len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"severity\":\"INFO\"") != NULL);

    log_formatter_init("OTEL", NULL);
    len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"severity\":\"INFO\"") != NULL);

    log_formatter_reset();
    printf("test_uppercase_format_strings passed\n");
}

static void test_log_formatter_get_format(void) {
    log_formatter_init("[%level] %msg", NULL);
    const char *fmt = log_formatter_get_format();
    assert(fmt != NULL);
    assert(strcmp(fmt, "[%level] %msg") == 0);
    log_formatter_reset();
    printf("test_log_formatter_get_format passed\n");
}

static void test_tiny_buffer_truncation(void) {
    log_record_t rec = {0};
    rec.level = LOG_LEVEL_INFO;
    rec.timestamp = 1625000000000000ULL;
    rec.module = "test";
    rec.file = "test.c";
    rec.line = 1;
    rec.func = "main";
    rec.message = "truncation test";

    log_formatter_init("OTLP", NULL);
    char buf[16];
    int len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len == -1);
    log_formatter_reset();
    printf("test_tiny_buffer_truncation passed\n");
}

static void test_invalid_level_default(void) {
    log_formatter_init("json", NULL);
    log_record_t rec = {0};
    rec.level = (log_level_t)99;
    rec.timestamp = 1625000000000000ULL;
    rec.message = "invalid level";

    char buf[256];
    int len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "UNKNOWN") != NULL);
    log_formatter_reset();
    printf("test_invalid_level_default passed\n");
}

int main(void) {
    test_trace_context_hex();
    test_pattern_trace_tokens();
    test_otlp_formatter();
    test_traceparent_env();
    test_uppercase_format_strings();
    test_log_formatter_get_format();
    test_tiny_buffer_truncation();
    test_invalid_level_default();
    test_otlp_sink();
    printf("all otel tests passed!\n");
    return 0;
}
