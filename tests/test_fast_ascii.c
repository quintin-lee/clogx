#include "fast_ascii.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_u32toa(void)
{
    char   buf[32];
    size_t len;

    len      = clog_u32toa(0, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "0") == 0);

    len      = clog_u32toa(9, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "9") == 0);

    len      = clog_u32toa(10, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "10") == 0);

    len      = clog_u32toa(99, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "99") == 0);

    len      = clog_u32toa(100, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "100") == 0);

    len      = clog_u32toa(1234567, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "1234567") == 0);

    len      = clog_u32toa(4294967295U, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "4294967295") == 0);
}

static void test_i32toa(void)
{
    char   buf[32];
    size_t len;

    len      = clog_i32toa(0, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "0") == 0);

    len      = clog_i32toa(-1, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "-1") == 0);

    len      = clog_i32toa(-99, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "-99") == 0);

    len      = clog_i32toa(12345, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "12345") == 0);

    len      = clog_i32toa(-2147483647 - 1, buf);
    buf[len] = '\0';
    assert(strcmp(buf, "-2147483648") == 0);
}

static void test_padding(void)
{
    char buf[16];

    clog_u32toa_pad2(5, buf);
    buf[2] = '\0';
    assert(strcmp(buf, "05") == 0);

    clog_u32toa_pad2(99, buf);
    buf[2] = '\0';
    assert(strcmp(buf, "99") == 0);

    clog_u32toa_pad4(2026, buf);
    buf[4] = '\0';
    assert(strcmp(buf, "2026") == 0);

    clog_u32toa_pad6(123, buf);
    buf[6] = '\0';
    assert(strcmp(buf, "000123") == 0);

    clog_u32toa_pad6(999999, buf);
    buf[6] = '\0';
    assert(strcmp(buf, "999999") == 0);
}

static void test_iso_datetime(void)
{
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = 2026 - 1900;
    tm_val.tm_mon  = 7; /* August = 7 (0-indexed) */
    tm_val.tm_mday = 1;
    tm_val.tm_hour = 15;
    tm_val.tm_min  = 22;
    tm_val.tm_sec  = 59;

    char buf[32];
    clog_format_iso_datetime(&tm_val, buf);
    assert(strcmp(buf, "2026-08-01 15:22:59") == 0);
}

int main(void)
{
    test_u32toa();
    test_i32toa();
    test_padding();
    test_iso_datetime();
    printf("all fast ascii tests passed!\n");
    return 0;
}
