/**
 * @file rotate.h
 * @brief Size-based log file rotation helpers for the file sink.
 */

#ifndef ROTATE_H
#define ROTATE_H

#include <stdint.h>

/**
 * @brief Rotate @p base_path into a numbered backup chain.
 *
 * Deletes \c base_path.N , shifts \c .1  \c .2  \c .(N-1)  \c .N , then renames
 * the active file to `.1`.
 *
 * @param[in] base_path   Active log file path (e.g. `"logs/server.log"`).
 * @param[in] max_backups Number of numbered backups to retain (`N`).
 * @return 0 always (best-effort; rename/unlink errors are ignored).
 */
int file_rotate_file(const char *base_path, int max_backups);

#endif /* ROTATE_H */
