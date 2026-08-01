/**
 * @file fast_ascii.h
 * @brief Fast integer-to-ASCII and timestamp formatting utilities using 2-digit LUT.
 */
#ifndef FAST_ASCII_H
#define FAST_ASCII_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static const char g_clog_digits_lut[] = "0001020304050607080910111213141516171819"
                                        "2021222324252627282930313233343536373839"
                                        "4041424344454647484950515253545556575859"
                                        "6061626364656667686970717273747576777879"
                                        "8081828384858687888990919293949596979899";

/**
 * @brief Convert a 2-digit integer (0-99) to 2 ASCII characters with leading zeros.
 * Does not write a null-terminator.
 */
static inline void clog_u32toa_pad2(uint32_t val, char *buf)
{
    uint32_t idx = (val % 100) * 2;
    buf[0]       = g_clog_digits_lut[idx];
    buf[1]       = g_clog_digits_lut[idx + 1];
}

/**
 * @brief Convert a 4-digit integer (0-9999) to 4 ASCII characters with leading zeros.
 * Does not write a null-terminator.
 */
static inline void clog_u32toa_pad4(uint32_t val, char *buf)
{
    uint32_t d1 = (val / 100) * 2;
    uint32_t d2 = (val % 100) * 2;
    buf[0]      = g_clog_digits_lut[d1];
    buf[1]      = g_clog_digits_lut[d1 + 1];
    buf[2]      = g_clog_digits_lut[d2];
    buf[3]      = g_clog_digits_lut[d2 + 1];
}

/**
 * @brief Convert a 6-digit integer (0-999999) to 6 ASCII characters with leading zeros.
 * Used for microsecond formatting (%06u). Does not write a null-terminator.
 */
static inline void clog_u32toa_pad6(uint32_t val, char *buf)
{
    uint32_t v1 = val / 100;
    uint32_t d3 = (val % 100) * 2;
    uint32_t d2 = (v1 % 100) * 2;
    uint32_t d1 = (v1 / 100) * 2;
    buf[0]      = g_clog_digits_lut[d1];
    buf[1]      = g_clog_digits_lut[d1 + 1];
    buf[2]      = g_clog_digits_lut[d2];
    buf[3]      = g_clog_digits_lut[d2 + 1];
    buf[4]      = g_clog_digits_lut[d3];
    buf[5]      = g_clog_digits_lut[d3 + 1];
}

/**
 * @brief Fast uint32_t to ASCII string conversion.
 * Writes output to @p buf. Does NOT write a null-terminator.
 * @return Number of ASCII characters written.
 */
static inline size_t clog_u32toa(uint32_t val, char *buf)
{
    if (val < 10) {
        buf[0] = (char)('0' + val);
        return 1;
    }
    if (val < 100) {
        uint32_t idx = val * 2;
        buf[0]       = g_clog_digits_lut[idx];
        buf[1]       = g_clog_digits_lut[idx + 1];
        return 2;
    }

    char  tmp[12];
    char *p = tmp + sizeof(tmp);
    while (val >= 100) {
        uint32_t idx = (val % 100) * 2;
        val /= 100;
        p -= 2;
        p[0] = g_clog_digits_lut[idx];
        p[1] = g_clog_digits_lut[idx + 1];
    }
    if (val < 10) {
        *--p = (char)('0' + val);
    } else {
        uint32_t idx = val * 2;
        p -= 2;
        p[0] = g_clog_digits_lut[idx];
        p[1] = g_clog_digits_lut[idx + 1];
    }

    size_t len = (size_t)(tmp + sizeof(tmp) - p);
    memcpy(buf, p, len);
    return len;
}

/**
 * @brief Fast int32_t to ASCII string conversion.
 * Writes output to @p buf. Does NOT write a null-terminator.
 * @return Number of ASCII characters written.
 */
static inline size_t clog_i32toa(int32_t val, char *buf)
{
    if (val < 0) {
        buf[0]        = '-';
        uint32_t uval = (uint32_t)(-(int64_t)val);
        return 1 + clog_u32toa(uval, buf + 1);
    }
    return clog_u32toa((uint32_t)val, buf);
}

/**
 * @brief Fast uint64_t to ASCII string conversion.
 * Writes output to @p buf. Does NOT write a null-terminator.
 * @return Number of ASCII characters written.
 */
static inline size_t clog_u64toa(uint64_t val, char *buf)
{
    if (val <= 0xFFFFFFFFULL) {
        return clog_u32toa((uint32_t)val, buf);
    }
    char  tmp[24];
    char *p = tmp + sizeof(tmp);
    while (val >= 100) {
        uint32_t idx = (uint32_t)(val % 100) * 2;
        val /= 100;
        p -= 2;
        p[0] = g_clog_digits_lut[idx];
        p[1] = g_clog_digits_lut[idx + 1];
    }
    if (val < 10) {
        *--p = (char)('0' + val);
    } else {
        uint32_t idx = (uint32_t)val * 2;
        p -= 2;
        p[0] = g_clog_digits_lut[idx];
        p[1] = g_clog_digits_lut[idx + 1];
    }
    size_t len = (size_t)(tmp + sizeof(tmp) - p);
    memcpy(buf, p, len);
    return len;
}

/**
 * @brief Fast int64_t to ASCII string conversion.
 * Writes output to @p buf. Does NOT write a null-terminator.
 * @return Number of ASCII characters written.
 */
static inline size_t clog_i64toa(int64_t val, char *buf)
{
    if (val < 0) {
        buf[0]        = '-';
        uint64_t uval = (uint64_t)(-(val + 1)) + 1;
        return 1 + clog_u64toa(uval, buf + 1);
    }
    return clog_u64toa((uint64_t)val, buf);
}

/**
 * @brief Fast timestamp renderer for default format "%Y-%m-%d %H:%M:%S".
 * Writes 19 ASCII characters plus null-terminator into @p out_buf (must be >= 20 bytes).
 */
static inline void clog_format_iso_datetime(const struct tm *tm, char *out_buf)
{
    clog_u32toa_pad4((uint32_t)(tm->tm_year + 1900), out_buf);
    out_buf[4] = '-';
    clog_u32toa_pad2((uint32_t)(tm->tm_mon + 1), out_buf + 5);
    out_buf[7] = '-';
    clog_u32toa_pad2((uint32_t)tm->tm_mday, out_buf + 8);
    out_buf[10] = ' ';
    clog_u32toa_pad2((uint32_t)tm->tm_hour, out_buf + 11);
    out_buf[13] = ':';
    clog_u32toa_pad2((uint32_t)tm->tm_min, out_buf + 14);
    out_buf[16] = ':';
    clog_u32toa_pad2((uint32_t)tm->tm_sec, out_buf + 17);
    out_buf[19] = '\0';
}

#endif /* FAST_ASCII_H */
