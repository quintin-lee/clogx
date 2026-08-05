/**
 * @file rotate.c
 * @brief Log file rotation with numbered backup chain.
 *
 * ## Rotation Strategy
 *
 * When the active log file exceeds `max_file_size` bytes, the logger
 * rotates the file using a numbered backup chain:
 *
 * ```
 * app.log       → app.log.1
 * app.log.1     → app.log.2
 * app.log.2     → app.log.3
 * ...
 * app.log.N-1   → app.log.N   (deleted if N == max_files)
 * ```
 *
 * This is the standard "logrotate" style rotation used by most logging
 * frameworks (rsyslog, syslog-ng, etc.).
 *
 * ## Thread Safety
 *
 * Rotation is triggered from the sink's write path (under sink lock).
 * The rotation itself only touches the file system and does not interact
 * with the logging pipeline, so no additional synchronisation is needed.
 */
#include "clog_port.h"
#include <stdio.h>
#include <errno.h>

/**
 * @brief Rotate a log file using a numbered backup chain.
 *
 * Shifts existing backups: .N-1 → .N, ..., .1 → .2, then renames
 * the active file to .1. The oldest backup (.N) is deleted first
 * to stay within the max_backups window.
 *
 * @param base_path    Path to the active log file (e.g. "logs/app.log").
 * @param max_backups  Maximum number of backup files to retain.
 * @retval 0          Success — all operations completed.
 * @retval -1         Invalid arguments (NULL path or max_backups <= 0).
 * @retval -EACCES    Permission denied on a rename or unlink.
 * @retval -ENOENT    Base path does not exist (no active file to rotate).
 */
int file_rotate_file(const char *base_path, int max_backups)
{
    if (!base_path || max_backups <= 0) {
        return -1;
    }

    char path[512];

    /* Drop the oldest backup that would fall out of the window */
    snprintf(path, sizeof(path), "%s.%d", base_path, max_backups);
    if (clog_unlink(path) != 0 && errno != ENOENT) {
        return -EACCES;
    }

    /* Shift .N-1 -> .N, ..., .1 -> .2 */
    for (int i = max_backups - 1; i >= 1; i--) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s.%d", base_path, i);
        snprintf(dst, sizeof(dst), "%s.%d", base_path, i + 1);
        if (clog_access(src, F_OK) == 0) {
            if (rename(src, dst) != 0) {
                return -EACCES;
            }
        }
    }

    /* Active file becomes .1 */
    snprintf(path, sizeof(path), "%s.1", base_path);
    if (clog_access(base_path, F_OK) == 0) {
        if (rename(base_path, path) != 0) {
            return -EACCES;
        }
    } else {
        return -ENOENT;
    }

    return 0;
}
