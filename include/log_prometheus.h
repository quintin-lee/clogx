/**
 * @file log_prometheus.h
 * @brief Prometheus exposition format rendering and HTTP metrics exporter for clogx.
 *
 * ## Overview
 *
 * This module exposes clogx's internal operational metrics in
 * [Prometheus Text Format
 * 0.0.4](https://github.com/prometheus/docs/blob/main/content/docs/instrumenting/exposition_formats.md)
 * so that a Prometheus scraper can monitor logging performance.
 *
 * ## Exported Metrics
 *
 * | Metric name                  | Type    | Labels           | Description |
 * |------------------------------|---------|------------------|-----------------------------------------|
 * | `clog_log_total`            | Counter | `level`          | Total log events by severity level |
 * | `clog_log_dropped_total`    | Counter | —                | Events dropped due to full async
 * queue  | | `clog_log_suppressed_total` | Counter | —                | Events suppressed by rate
 * limiter       | | `clog_queue_depth`          | Gauge   | —                | Current number of
 * records in async queue|
 *
 * ## Scraping
 *
 * Two usage modes are supported:
 * 1. **Embedded HTTP server**: call @ref clog_prometheus_exporter_start(port)
 *    to start a lightweight HTTP listener on `/metrics`.
 * 2. **Pull via callback**: call @ref clog_prometheus_render_metrics into a
 *    buffer and serve the content through an existing HTTP server.
 *
 * @see clog_stats_t in log.h for the raw stats structure.
 */

#ifndef LOG_PROMETHEUS_H
#define LOG_PROMETHEUS_H

#include <stddef.h>
#include <stdbool.h>

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
 * @brief Render clogx internal statistics as Prometheus Text Format 0.0.4.
 *
 * Produces output such as:
 * ```
 * # HELP clog_log_total Total log events by severity level.
 * # TYPE clog_log_total counter
 * clog_log_total{level="info"} 42
 * clog_log_total{level="error"} 7
 * # HELP clog_log_dropped_total Log events dropped due to full async queue.
 * # TYPE clog_log_dropped_total counter
 * clog_log_dropped_total 0
 * # HELP clog_log_suppressed_total Log events suppressed by rate limiter.
 * # TYPE clog_log_suppressed_total counter
 * clog_log_suppressed_total 0
 * # HELP clog_queue_depth Current async queue depth.
 * # TYPE clog_queue_depth gauge
 * clog_queue_depth 0
 * ```
 *
 * @param[out] buf      Destination buffer for the text-format output.
 * @param[in]  buf_size Capacity of @p buf in bytes.
 * @return Number of bytes written (excluding NUL terminator), or a negative
 *         error code if @p buf_size is too small to hold the full output.
 */
CLOGX_API int clog_prometheus_render_metrics(char *buf, size_t buf_size);

/**
 * @brief Start the embedded HTTP /metrics server on a background thread.
 *
 * The server listens on TCP @p port and responds to GET /metrics with the
 * output of @ref clog_prometheus_render_metrics. The response includes a
 * `Content-Type: text/plain; version=0.0.4` header.
 *
 * Only one server instance may run at a time. If a server is already
 * running, this call returns an error.
 *
 * @param[in] port  TCP port number (1–65535, e.g. 9090). Privileged ports
 *                  (< 1024) may require root or CAP_NET_BIND_SERVICE.
 * @retval 0   Server started successfully.
 * @retval <0  Negative @ref clogx_errno_t value on failure (port in use,
 *             binding error, thread creation failure, or server already running).
 */
CLOGX_API int clog_prometheus_exporter_start(int port);

/**
 * @brief Stop the Prometheus HTTP metrics exporter server.
 *
 * Signals the server thread to shut down and joins it. Safe to call even
 * if the server was never started (no-op).
 */
CLOGX_API void clog_prometheus_exporter_stop(void);

/**
 * @brief Check whether the embedded metrics server is currently running.
 * @return true if the server thread is active, false otherwise.
 */
CLOGX_API bool clog_prometheus_exporter_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_PROMETHEUS_H */
