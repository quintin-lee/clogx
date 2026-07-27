#ifndef ROTATE_H
#define ROTATE_H

#include <stdint.h>

// Rotate a log file based on max size and backup count
// base_path: path without extension (e.g., "logs/server.log")
// max_size: max size before rotation
// backups: number of backup files to keep
int file_rotate_file(const char *base_path, int max_backups);

#endif // ROTATE_H
