/**
 * @file rotate.h
 * @brief Size-based log file rotation helpers for the file sink.
 *
 * ## Rotation Strategy
 *
 * When the active log file exceeds @ref log_config_t::max_size, the file
 * sink calls @ref file_rotate_file to shift the numbered backup chain.
 * The scheme matches standard logrotate "rotate" semantics:
 *
 * ```
 * Before:  server.log  server.log.1  server.log.2
 * After:   server.log  server.log.1  server.log.2  (server.log.3 deleted)
 * ```
 *
 * Steps:
 * 1. Delete `server.log.N` (the oldest backup).
 * 2. Rename `server.log.N-1` → `server.log.N`, ... down to `server.log.1`.
 * 3. Rename `server.log` → `server.log.1`.
 * 4. The caller re-creates `server.log` for new writes.
 *
 * ## Error Handling
 *
 * This function is best-effort: rename(2) and unlink(2) failures are
 * silently ignored. Partial rotation (some renames succeed before a
 * failure) is accepted — the chain may have gaps but the active file
 * is always rotated to `.1`.
 *
 * @see log_config_t::max_size for the size threshold configuration.
 */

#ifndef ROTATE_H
#define ROTATE_H

/**
 * @brief Perform a logrotate-style rotation of the numbered backup chain.
 *
 * Implements the standard rotation sequence:
 * - Deletes `base_path.max_backups` (the oldest backup).
 * - Renames `.max_backups-1` → `.max_backups`, ..., `.1` → `.2`.
 * - Renames the active file `base_path` → `base_path.1`.
 *
 * After this call, the caller must re-create `base_path` as a new empty file.
 *
 * @param[in] base_path   Path to the active log file (e.g. `"logs/server.log"`).
 * @param[in] max_backups Maximum number of numbered backups to retain.
 *                        The oldest backup beyond this count is deleted.
 *                        Pass 0 to keep no backups (only delete the active file).
 * @retval 0  Always (best-effort — individual rename/unlink errors are logged
 *            but not propagated).
 *
 * @note Not thread-safe. The caller must ensure exclusive access to the file
 *       during rotation (the file sink uses a per-sink mutex).
 */
int file_rotate_file(const char *base_path, int max_backups);

#endif /* ROTATE_H */
