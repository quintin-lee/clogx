#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_formatter.h"
#include "log_record.h"

static char g_default_format[64] = "%msg";
static char *g_format_ptr = g_default_format;

int log_formatter_init(const char *format) {
    if (format && strlen(format) > 0) {
        strncpy(g_format_ptr, format, sizeof(g_format_ptr) - 1);
        g_format_ptr[sizeof(g_format_ptr) - 1] = '\0';
    } else {
        strcpy(g_format_ptr, g_default_format);
    }
    return 0;
}

void log_formatter_reset(void) {
    strcpy(g_format_ptr, g_default_format);
}

const char *log_formatter_get_format(void) {
    return g_format_ptr;
}

int log_formatter_format(log_record_t *record, char *buf, size_t buf_size) {
    // Simple implementation: just copy the message
    const char *msg = record->message ? record->message : "(no message)";
    size_t len = strlen(msg);
    if (len >= buf_size) {
        buf[buf_size - 1] = '\0';
        memcpy(buf, msg, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return (int)(buf_size - 1);
    }
    strcpy(buf, msg);
    return (int)len;
}
