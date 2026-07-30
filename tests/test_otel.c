/**
 * @file test_otel.c
 * @brief Unit tests for OpenTelemetry TraceContext and OTLP formatting/sink.
 */

#include <assert.h>
#include <stdio.h>
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

int main(void) {
    test_trace_context_hex();
    test_pattern_trace_tokens();
    test_otlp_formatter();
    test_otlp_sink();
    printf("all otel tests passed!\n");
    return 0;
}
