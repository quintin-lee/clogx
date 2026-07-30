/**
 * @file log_prometheus.h
 * @brief Prometheus exposition format rendering and HTTP metrics exporter for clogx.
 */

#ifndef LOG_PROMETHEUS_H
#define LOG_PROMETHEUS_H

#include <stddef.h>
#include <stdbool.h>
#include "log_record.h"

#ifndef CLOGX_API
#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render current log statistics into Prometheus Text Format 0.0.4.
 *
 * Exposes counters for log events by level, dropped events, suppressed events,
 * and current async queue depth gauge.
 *
 * @param[out] buf      Destination buffer.
 * @param[in]  buf_size Capacity of @p buf in bytes.
 * @return Bytes written (excluding NUL terminator), or negative error code if truncated/invalid.
 */
CLOGX_API int clog_prometheus_render_metrics(char *buf, size_t buf_size);

/**
 * @brief Start a lightweight HTTP /metrics server for Prometheus scraping.
 *
 * @param[in] port TCP port to bind (1..65535, e.g. 9090).
 * @return 0 on success, negative clogx_errno_t on failure.
 */
CLOGX_API int clog_prometheus_exporter_start(int port);

/**
 * @brief Stop the Prometheus HTTP metrics exporter server if running.
 */
CLOGX_API void clog_prometheus_exporter_stop(void);

/**
 * @brief Query whether the Prometheus HTTP metrics exporter server is currently running.
 * @return true if running.
 */
CLOGX_API bool clog_prometheus_exporter_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_PROMETHEUS_H */
