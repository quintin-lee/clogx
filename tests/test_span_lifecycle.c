/**
 * @file test_span_lifecycle.c
 * @brief Unit tests for explicit span stack: start/end/export/join.
 */

#include "clog_port.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int is_zero(const uint8_t *id, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (id[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void test_nested_ids(void)
{
    uint8_t     tid[16];
    uint8_t     sid[8];
    uint8_t     child_tid[16];
    uint8_t     child_sid[8];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    assert(t1 == 1);
    clog_get_trace_context(tid, sid);
    assert(!is_zero(tid, 16));
    assert(!is_zero(sid, 8));

    t2 = clog_span_start();
    assert(t2 == 2);
    clog_get_trace_context(child_tid, child_sid);
    assert(memcmp(tid, child_tid, 16) == 0);
    assert(memcmp(sid, child_sid, 8) != 0);
    assert(!is_zero(child_sid, 8));

    assert(clog_span_end(t2) == CLOG_OK);
    assert(clog_span_end(t1) == CLOG_OK);
    printf("test_nested_ids passed\n");
}

static void test_out_of_order_end(void)
{
    uint8_t     before[8];
    uint8_t     after[8];
    uint8_t     tid[16];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    t2 = clog_span_start();
    clog_get_trace_context(tid, before);

    assert(clog_span_end(t1) == CLOG_ERR_INVALID_ARG);
    clog_get_trace_context(tid, after);
    assert(memcmp(before, after, 8) == 0);

    assert(clog_span_end(t2) == CLOG_OK);
    assert(clog_span_end(t1) == CLOG_OK);
    assert(clog_span_end(t1) == CLOG_ERR_INVALID_ARG);
    assert(clog_span_end(0) == CLOG_ERR_INVALID_ARG);
    printf("test_out_of_order_end passed\n");
}

static void test_depth_overflow(void)
{
    clog_span_t toks[16];
    int         i;

    clog_clear_trace_context();
    for (i = 0; i < 16; i++) {
        toks[i] = clog_span_start();
        assert(toks[i] == (clog_span_t)(i + 1));
    }
    assert(clog_span_start() == 0);
    for (i = 15; i >= 0; i--) {
        assert(clog_span_end(toks[i]) == CLOG_OK);
    }
    assert(clog_span_end(toks[0]) == CLOG_ERR_INVALID_ARG);
    printf("test_depth_overflow passed\n");
}

static void test_export_join_roundtrip(void)
{
    char        outward[64];
    char        reinward[64];
    uint8_t     tid1[16];
    uint8_t     sid1[8];
    uint8_t     tid2[16];
    uint8_t     sid2[8];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    assert(clog_span_export(outward, sizeof(outward)) == CLOG_OK);
    assert(strlen(outward) == 55);
    assert(outward[0] == '0' && outward[1] == '0' && outward[2] == '-');

    t2 = clog_span_join(outward);
    assert(t2 == 2);
    clog_get_trace_context(tid2, sid2);
    clog_span_end(t2);
    clog_get_trace_context(tid1, sid1);
    assert(memcmp(tid1, tid2, 16) == 0);
    assert(memcmp(sid1, sid2, 8) != 0);

    assert(clog_span_export(reinward, sizeof(reinward)) == CLOG_OK);
    assert(strcmp(outward, reinward) == 0);

    assert(clog_span_end(t1) == CLOG_OK);
    printf("test_export_join_roundtrip passed\n");
}

static void test_flags_passthrough(void)
{
    char        outward[64];
    clog_span_t t;

    clog_clear_trace_context();
    t = clog_span_join("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(t == 1);
    assert(clog_span_export(outward, sizeof(outward)) == CLOG_OK);
    assert(strcmp(outward, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") != 0);
    assert(strstr(outward, "4bf92f3577b34da6a3ce929d0e0e4736") != NULL);
    assert(strcmp(outward + 53, "01") == 0);
    assert(clog_span_end(t) == CLOG_OK);
    printf("test_flags_passthrough passed\n");
}

static void test_export_rejected_when_empty(void)
{
    char buf[64];
    char sentinel[64];

    clog_clear_trace_context();
    memset(sentinel, 0x5a, sizeof(sentinel));
    memcpy(buf, sentinel, sizeof(buf));
    assert(clog_span_export(buf, sizeof(buf)) == CLOG_ERR_INVALID_ARG);
    assert(memcmp(buf, sentinel, sizeof(buf)) == 0);
    assert(clog_span_export(NULL, 64) == CLOG_ERR_INVALID_ARG);
    assert(clog_span_export(buf, 55) == CLOG_ERR_INVALID_ARG);
    printf("test_export_rejected_when_empty passed\n");
}

static void test_join_rejected(void)
{
    uint8_t tid[16];
    uint8_t sid[8];

    clog_clear_trace_context();
    assert(clog_span_join(NULL) == 0);
    assert(clog_span_join("garbage") == 0);
    assert(clog_span_join("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7") == 0);
    assert(clog_span_join("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902BX-01") == 0);
    assert(clog_span_join("00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") == 0);
    clog_get_trace_context(tid, sid);
    assert(is_zero(tid, 16));
    assert(is_zero(sid, 8));
    printf("test_join_rejected passed\n");
}

static void test_clear_empties_stack(void)
{
    uint8_t     tid[16];
    uint8_t     sid[8];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    t2 = clog_span_start();
    (void)t1;
    clog_clear_trace_context();
    clog_get_trace_context(tid, sid);
    assert(is_zero(tid, 16));
    assert(is_zero(sid, 8));
    assert(clog_span_end(t2) == CLOG_ERR_INVALID_ARG);
    assert(clog_span_export(NULL, 0) == CLOG_ERR_INVALID_ARG);
    printf("test_clear_empties_stack passed\n");
}

static void test_legacy_set_get_unchanged(void)
{
    uint8_t     tid[16];
    uint8_t     sid[8];
    uint8_t     base_tid[16];
    uint8_t     base_sid[8];
    clog_span_t t;

    clog_clear_trace_context();
    assert(clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736", "00f067aa0ba902b7") ==
           CLOG_OK);
    clog_get_trace_context(base_tid, base_sid);
    assert(base_tid[0] == 0x4b && base_sid[1] == 0xf0);

    t = clog_span_start();
    clog_get_trace_context(tid, sid);
    assert(memcmp(tid, base_tid, 16) == 0);
    assert(memcmp(sid, base_sid, 8) != 0);
    assert(clog_span_end(t) == CLOG_OK);
    clog_get_trace_context(tid, sid);
    assert(memcmp(tid, base_tid, 16) == 0);
    assert(memcmp(sid, base_sid, 8) == 0);

    clog_clear_trace_context();
    printf("test_legacy_set_get_unchanged passed\n");
}

int main(void)
{
    test_nested_ids();
    test_out_of_order_end();
    test_depth_overflow();
    test_export_join_roundtrip();
    test_flags_passthrough();
    test_export_rejected_when_empty();
    test_join_rejected();
    test_clear_empties_stack();
    test_legacy_set_get_unchanged();
    printf("all span lifecycle tests passed!\n");
    return 0;
}
