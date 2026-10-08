/**
 * @file journald_sink.c
 * @brief Native systemd-journal sink (no libsystemd dependency).
 *
 * ## Design
 *
 * Each log record is sent as one `AF_UNIX`/`SOCK_DGRAM` datagram to
 * `/run/systemd/journal/socket` per the journal native protocol
 * (https://systemd.io/JOURNAL_NATIVE_PROTOCOL):
 * `PRIORITY=<n>\nSYSLOG_IDENTIFIER=<ident>\nMESSAGE=<text>\n`, with the
 * binary-safe form (`NAME\n` + LE64 length + raw bytes + `\n`) when
 * MESSAGE contains an embedded newline.
 *
 * PRIORITY mapping reuses the syslog sink table: TRACE/DEBUG->7,
 * INFO->6, WARN->4, ERROR->3, FATAL->2, unknown->6.
 *
 * ## Failure policy
 *
 * Factory fails (NULL) when the socket is unavailable. Runtime send
 * failures return -1 with no retry; drop accounting stays with the
 * dispatcher. Oversized MESSAGE is truncated to 240*1024 bytes.
 *
 * ## Platform
 *
 * Linux only. Other platforms return NULL from the factory.
 */

#include "clogx_plugin.h"
#include "log_sink.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define JOURNALD_SOCKET_PATH "/run/systemd/journal/socket"
/** @brief MESSAGE truncation cap for oversized records. */
#define JOURNALD_MAX_MESSAGE ((size_t)240 * 1024)

typedef struct {
    int   fd;
    char *ident;
} journald_sink_data_t;

static int journald_priority_from_text(const char *buf)
{
    if (strstr(buf, "[TRACE]") || strstr(buf, "[DEBUG]")) {
        return 7;
    }
    if (strstr(buf, "[WARN]") || strstr(buf, "[WARNING]")) {
        return 4;
    }
    if (strstr(buf, "[ERROR]")) {
        return 3;
    }
    if (strstr(buf, "[FATAL]")) {
        return 2;
    }
    return 6;
}

static void journald_store_le64(unsigned char *dst, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (unsigned char)(v & 0xffu);
        v >>= 8;
    }
}

/**
 * @brief Append @p n bytes from @p src at packet offset @p *off.
 *
 * Centralizes framing copies so literal lengths are compile-time
 * `sizeof`-constants at call sites (keeps clang-tidy's
 * bugprone-not-null-terminated-result quiet: packet framing is
 * newline-delimited, never NUL-terminated by design).
 */
static void journald_put(char *packet, size_t *off, const char *src, size_t n)
{
    memcpy(packet + *off, src, n);
    *off += n;
}

static int journald_write(log_sink_t *sink, const char *buf, size_t len)
{
    journald_sink_data_t *data = (journald_sink_data_t *)sink->private_data;
    char                  prio_line[32];
    int                   prio_len;
    size_t                msg_len;
    size_t                total;
    int                   use_binary;
    char                 *packet;
    size_t                off = 0;

    if (!data || data->fd < 0 || !buf) {
        return -1;
    }
    /* Strip the single trailing newline the dispatcher appends for streams. */
    msg_len = len;
    while (msg_len > 0 && (buf[msg_len - 1] == '\n' || buf[msg_len - 1] == '\r')) {
        msg_len--;
    }
    if (msg_len > JOURNALD_MAX_MESSAGE) {
        msg_len = JOURNALD_MAX_MESSAGE;
    }
    prio_len =
        snprintf(prio_line, sizeof(prio_line), "PRIORITY=%d\n", journald_priority_from_text(buf));
    if (prio_len < 0) {
        return -1;
    }
    use_binary = (memchr(buf, '\n', msg_len) != NULL);
    total      = (size_t)prio_len + sizeof("SYSLOG_IDENTIFIER=") - 1u + strlen(data->ident) + 1u;
    if (use_binary) {
        total += sizeof("MESSAGE\n") - 1u + 8u + msg_len + 1u;
    } else {
        total += sizeof("MESSAGE=") - 1u + msg_len + 1u;
    }
    packet = (char *)malloc(total);
    if (!packet) {
        return -1;
    }
    journald_put(packet, &off, prio_line, (size_t)prio_len);
    journald_put(packet, &off, "SYSLOG_IDENTIFIER=", sizeof("SYSLOG_IDENTIFIER=") - 1u);
    journald_put(packet, &off, data->ident, strlen(data->ident));
    packet[off++] = '\n';
    if (use_binary) {
        journald_put(packet, &off, "MESSAGE\n", sizeof("MESSAGE\n") - 1u);
        journald_store_le64((unsigned char *)(packet + off), (uint64_t)msg_len);
        off += 8u;
        journald_put(packet, &off, buf, msg_len);
    } else {
        journald_put(packet, &off, "MESSAGE=", sizeof("MESSAGE=") - 1u);
        journald_put(packet, &off, buf, msg_len);
    }
    packet[off++] = '\n';
    if (off != total) {
        free(packet);
        return -1;
    }
    if (send(data->fd, packet, off, MSG_NOSIGNAL) != (ssize_t)off) {
        free(packet);
        return -1;
    }
    free(packet);
    return (int)len;
}

static void journald_flush(log_sink_t *sink)
{
    (void)sink;
}

static void journald_close_fd(journald_sink_data_t *data)
{
    if (data->fd >= 0) {
        close(data->fd);
        data->fd = -1;
    }
}

static void journald_destroy(log_sink_t *sink)
{
    if (!sink) {
        return;
    }
    journald_sink_data_t *data = (journald_sink_data_t *)sink->private_data;
    if (data) {
        journald_close_fd(data);
        free(data->ident);
        free(data);
    }
    free(sink);
}

static int journald_connect(void)
{
    struct sockaddr_un addr;
    int                fd = socket(AF_UNIX, SOCK_DGRAM, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", JOURNALD_SOCKET_PATH);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void journald_atfork_child(log_sink_t *sink)
{
    journald_sink_data_t *data;

    if (!sink || !sink->private_data) {
        return;
    }
    data = (journald_sink_data_t *)sink->private_data;
    journald_close_fd(data);
    data->fd = journald_connect();
}

/**
 * @brief Create a native systemd-journal sink. See log_sink.h for contract.
 */
log_sink_t *journald_sink_create(const char *ident)
{
    log_sink_t           *sink;
    journald_sink_data_t *data;
    const char           *name = ident ? ident : "clogx";

    if (strchr(name, '\n')) {
        return NULL;
    }
    sink = (log_sink_t *)malloc(sizeof(log_sink_t));
    if (!sink) {
        return NULL;
    }
    data = (journald_sink_data_t *)malloc(sizeof(journald_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }
    data->ident = strdup(name);
    if (!data->ident) {
        free(data);
        free(sink);
        return NULL;
    }
    data->fd = journald_connect();
    if (data->fd < 0) {
        free(data->ident);
        free(data);
        free(sink);
        return NULL;
    }
    sink->abi_version  = CLOGX_PLUGIN_ABI_VERSION;
    sink->write        = journald_write;
    sink->flush        = journald_flush;
    sink->destroy      = journald_destroy;
    sink->atfork_child = journald_atfork_child;
    sink->private_data = data;
    sink->min_level    = LOG_LEVEL_TRACE;
    return sink;
}

#else /* not __linux__ */

log_sink_t *journald_sink_create(const char *ident)
{
    (void)ident;
    return NULL;
}

#endif
