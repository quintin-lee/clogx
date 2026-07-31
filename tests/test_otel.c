/**
 * @file test_otel.c
 * @brief Unit tests for OpenTelemetry TraceContext and OTLP formatting/sink.
 */

#include "clog_port.h"
#include "log.h"
#include "log_formatter.h"
#include "log_sink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_trace_context_hex(void)
{
    clog_clear_trace_context();

    uint8_t tid[16];
    uint8_t sid[8];
    clog_get_trace_context(tid, sid);

    for (int i = 0; i < 16; i++) {
        assert(tid[i] == 0);
    }
    for (int i = 0; i < 8; i++) {
        assert(sid[i] == 0);
    }

    const char *tid_hex = "4bf92f3577b34da6a3ce929d0e0e4736";
    const char *sid_hex = "00f067aa0ba902b7";

    int ret = clog_set_trace_context_hex(tid_hex, sid_hex);
    assert(ret == CLOG_OK);

    clog_get_trace_context(tid, sid);
    assert(tid[0] == 0x4b && tid[1] == 0xf9);
    assert(sid[0] == 0x00 && sid[1] == 0xf0);

    clog_clear_trace_context();
    clog_get_trace_context(tid, sid);
    for (int i = 0; i < 16; i++) {
        assert(tid[i] == 0);
    }
    printf("test_trace_context_hex passed\n");
}

static void test_trace_context_hex_uppercase(void)
{
    clog_clear_trace_context();

    const char *tid_hex = "4BF92F3577B34DA6A3CE929D0E0E4736";
    const char *sid_hex = "00F067AA0BA902B7";

    int ret = clog_set_trace_context_hex(tid_hex, sid_hex);
    assert(ret == CLOG_OK);

    uint8_t tid[16];
    uint8_t sid[8];
    clog_get_trace_context(tid, sid);
    assert(tid[0] == 0x4b && tid[1] == 0xf9);
    assert(sid[0] == 0x00 && sid[1] == 0xf0);

    clog_clear_trace_context();
    printf("test_trace_context_hex_uppercase passed\n");
}

static void test_trace_context_hex_empty(void)
{
    clog_clear_trace_context();

    int ret = clog_set_trace_context_hex("", "");
    assert(ret == CLOG_OK);

    uint8_t tid[16];
    uint8_t sid[8];
    clog_get_trace_context(tid, sid);
    for (int i = 0; i < 16; i++) {
        assert(tid[i] == 0);
    }
    for (int i = 0; i < 8; i++) {
        assert(sid[i] == 0);
    }

    clog_clear_trace_context();
    printf("test_trace_context_hex_empty passed\n");
}

static void test_trace_context_hex_short(void)
{
    clog_clear_trace_context();

    int ret = clog_set_trace_context_hex("short", "00f067aa0ba902b7");
    assert(ret == CLOG_ERR_INVALID_ARG);

    ret = clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736", "short");
    assert(ret == CLOG_ERR_INVALID_ARG);

    clog_clear_trace_context();
    printf("test_trace_context_hex_short passed\n");
}

static void test_pattern_trace_tokens(void)
{
    clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736", "00f067aa0ba902b7");

    log_formatter_init("[%level] [%trace_id:%span_id] %msg", NULL);

    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.message      = "otel test msg";
    clog_get_trace_context(rec.trace_id, rec.span_id);

    char buf[512];
    int  len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(
        strstr(buf, "[INFO] [4bf92f3577b34da6a3ce929d0e0e4736:00f067aa0ba902b7] otel test msg") !=
        NULL);

    clog_clear_trace_context();
    log_formatter_reset();
    printf("test_pattern_trace_tokens passed\n");
}

static void test_otlp_formatter(void)
{
    clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736", "00f067aa0ba902b7");

    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_WARN;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "auth";
    rec.file         = "auth.c";
    rec.line         = 42;
    rec.func         = "login";
    rec.message      = "failed login attempt";
    clog_get_trace_context(rec.trace_id, rec.span_id);

    char buf[1024];
    int  len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"severity\":\"WARN\"") != NULL);
    assert(strstr(buf, "\"severity_number\":13") != NULL);
    assert(strstr(buf, "\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != NULL);
    assert(strstr(buf, "\"span_id\":\"00f067aa0ba902b7\"") != NULL);
    assert(strstr(buf, "\"body\":\"failed login attempt\"") != NULL);

    clog_clear_trace_context();
    printf("test_otlp_formatter passed\n");
}

static void test_otlp_sink(void)
{
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

static void test_traceparent_env(void)
{
    clog_clear_trace_context();
    setenv("TRACEPARENT", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", 1);

    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "auth";
    rec.file         = "auth.c";
    rec.line         = 42;
    rec.func         = "login";
    rec.message      = "traceparent from env";

    char buf[1024];
    int  len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != NULL);
    assert(strstr(buf, "\"span_id\":\"00f067aa0ba902b7\"") != NULL);

    unsetenv("TRACEPARENT");
    clog_clear_trace_context();
    printf("test_traceparent_env passed\n");
}

static void test_uppercase_format_strings(void)
{
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "test";
    rec.file         = "test.c";
    rec.line         = 1;
    rec.func         = "main";
    rec.message      = "uppercase test";

    char buf[1024];

    log_formatter_init("JSON", NULL);
    int len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "{\"timestamp\":") != NULL);
    assert(strstr(buf, "\"level\":\"INFO\"") != NULL);

    log_formatter_init("OTLP", NULL);
    rec.trace_id[0] = 0x4b;
    rec.span_id[0]  = 0x00;
    rec.span_id[1]  = 0xf0;
    len             = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"severity\":\"INFO\"") != NULL);

    log_formatter_init("OTEL", NULL);
    len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"severity\":\"INFO\"") != NULL);

    log_formatter_reset();
    printf("test_uppercase_format_strings passed\n");
}

static void test_log_formatter_get_format(void)
{
    log_formatter_init("[%level] %msg", NULL);
    const char *fmt = log_formatter_get_format();
    assert(fmt != NULL);
    assert(strcmp(fmt, "[%level] %msg") == 0);
    log_formatter_reset();
    printf("test_log_formatter_get_format passed\n");
}

static void test_tiny_buffer_truncation(void)
{
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "test";
    rec.file         = "test.c";
    rec.line         = 1;
    rec.func         = "main";
    rec.message      = "truncation test";

    log_formatter_init("OTLP", NULL);
    char buf[16];
    int  len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len == -1);
    log_formatter_reset();
    printf("test_tiny_buffer_truncation passed\n");
}

static void test_invalid_level_default(void)
{
    log_formatter_init("json", NULL);
    log_record_t rec = {0};
    rec.level        = (log_level_t)99;
    rec.timestamp    = 1625000000000000ULL;
    rec.message      = "invalid level";

    char buf[256];
    int  len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "UNKNOWN") != NULL);
    log_formatter_reset();
    printf("test_invalid_level_default passed\n");
}

static void test_format_json_ex_trace_id_span_id(void)
{
    log_formatter_init("json", NULL);
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "trace_mod";
    rec.file         = "trace.c";
    rec.line         = 10;
    rec.func         = "trace_func";
    rec.message      = "trace message";
    memset(rec.trace_id, 0, 16);
    memset(rec.span_id, 0, 8);
    uint8_t tid[16] = {0x4b,
                       0xf9,
                       0x2f,
                       0x35,
                       0x77,
                       0xb3,
                       0x4d,
                       0xa6,
                       0xa3,
                       0xce,
                       0x92,
                       0x9d,
                       0x0e,
                       0x0e,
                       0x47,
                       0x36};
    uint8_t sid[8]  = {0x00, 0xf0, 0x67, 0xaa, 0x0b, 0xa9, 0x02, 0xb7};
    memcpy(rec.trace_id, tid, 16);
    memcpy(rec.span_id, sid, 8);

    char buf[512];
    int  len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"trace_id\"") != NULL);
    assert(strstr(buf, "\"span_id\"") != NULL);
    log_formatter_reset();
    printf("test_format_json_ex_trace_id_span_id passed\n");
}

static void test_format_json_ex_tiny_buffer(void)
{
    log_formatter_init("json", NULL);
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "test";
    rec.file         = "test.c";
    rec.line         = 1;
    rec.func         = "main";
    rec.message      = "x";

    char buf[8];
    int  len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len == -1);
    log_formatter_reset();
    printf("test_format_json_ex_tiny_buffer passed\n");
}

static void test_format_json_ex_null_time_format(void)
{
    log_formatter_init("json", NULL);
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.message      = "null time test";

    char buf[256];
    int  len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "{\"timestamp\":") != NULL);
    log_formatter_reset();
    printf("test_format_json_ex_null_time_format passed\n");
}

static void test_level_to_string_trace_debug(void)
{
    log_formatter_init("json", NULL);
    log_record_t rec = {0};
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "trace_mod";
    rec.file         = "trace.c";
    rec.line         = 10;
    rec.func         = "trace_func";
    rec.message      = "trace debug level test";

    rec.level = LOG_LEVEL_TRACE;
    char buf[512];
    log_set_level(LOG_LEVEL_TRACE);
    int len = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"level\":\"TRACE\"") != NULL);

    rec.level = LOG_LEVEL_DEBUG;
    len       = log_formatter_format(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"level\":\"DEBUG\"") != NULL);

    log_set_level(LOG_LEVEL_INFO);
    log_formatter_reset();
    printf("test_level_to_string_trace_debug passed\n");
}

static void test_otlp_sink_stderr(void)
{
    log_sink_t *sink = otlp_sink_create("stderr", "stderr_svc");
    assert(sink != NULL);
    assert(sink->write != NULL);
    assert(sink->flush != NULL);
    assert(sink->destroy != NULL);
    sink->destroy(sink);
    printf("test_otlp_sink_stderr passed\n");
}

static void test_otlp_sink_file_endpoint(void)
{
    const char *path = "/tmp/clog_otlp_file_test.log";
    remove(path);
    log_sink_t *sink = otlp_sink_create(path, "file_svc");
    assert(sink != NULL);
    assert(sink->write != NULL);
    int n = sink->write(sink, "{\"resourceLogs\":[]}", 19);
    assert(n > 0);
    sink->flush(sink);
    sink->destroy(sink);
    remove(path);
    printf("test_otlp_sink_file_endpoint passed\n");
}

static void test_otlp_sink_null_and_empty_endpoint(void)
{
    log_sink_t *sink1 = otlp_sink_create(NULL, "null_svc");
    assert(sink1 != NULL);
    assert(sink1->write != NULL);
    sink1->destroy(sink1);

    log_sink_t *sink2 = otlp_sink_create("", "empty_svc");
    assert(sink2 != NULL);
    assert(sink2->write != NULL);
    sink2->destroy(sink2);

    printf("test_otlp_sink_null_and_empty_endpoint passed\n");
}

static void test_otlp_sink_empty_service_name(void)
{
    log_sink_t *sink = otlp_sink_create("stdout", NULL);
    assert(sink != NULL);
    assert(sink->destroy != NULL);
    sink->destroy(sink);
    printf("test_otlp_sink_empty_service_name passed\n");
}

static void test_traceparent_uppercase_hex(void)
{
    clog_clear_trace_context();
    setenv("TRACEPARENT", "00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01", 1);

    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "auth";
    rec.file         = "auth.c";
    rec.line         = 42;
    rec.func         = "login";
    rec.message      = "uppercase traceparent";

    char buf[1024];
    int  len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != NULL);
    assert(strstr(buf, "\"span_id\":\"00f067aa0ba902b7\"") != NULL);

    unsetenv("TRACEPARENT");
    clog_clear_trace_context();
    printf("test_traceparent_uppercase_hex passed\n");
}

static void test_traceparent_invalid_hex(void)
{
    clog_clear_trace_context();
    setenv("TRACEPARENT", "00-4BF9XZ3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-01", 1);

    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1625000000000000ULL;
    rec.module       = "auth";
    rec.file         = "auth.c";
    rec.line         = 42;
    rec.func         = "login";
    rec.message      = "invalid traceparent";

    char buf[1024];
    int  len = log_formatter_format_otlp(&rec, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "\"trace_id\"") == NULL);
    assert(strstr(buf, "\"body\":\"invalid traceparent\"") != NULL);

    unsetenv("TRACEPARENT");
    clog_clear_trace_context();
    printf("test_traceparent_invalid_hex passed\n");
}

int main(void)
{
    test_trace_context_hex();
    test_trace_context_hex_uppercase();
    test_trace_context_hex_empty();
    test_trace_context_hex_short();
    test_pattern_trace_tokens();
    test_otlp_formatter();
    test_traceparent_env();
    test_uppercase_format_strings();
    test_log_formatter_get_format();
    test_tiny_buffer_truncation();
    test_invalid_level_default();
    test_format_json_ex_trace_id_span_id();
    test_format_json_ex_tiny_buffer();
    test_format_json_ex_null_time_format();
    test_level_to_string_trace_debug();
    test_otlp_sink();
    test_otlp_sink_stderr();
    test_otlp_sink_file_endpoint();
    test_otlp_sink_null_and_empty_endpoint();
    test_otlp_sink_empty_service_name();
    test_traceparent_uppercase_hex();
    test_traceparent_invalid_hex();
    printf("all otel tests passed!\n");
    return 0;
}
