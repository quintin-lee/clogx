/**
 * @file rotate.c
 * @brief Size-based log file rotation (shift numbered backups, rename active).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rotate.h"

int file_rotate_file(const char *base_path, int max_backups) {
    if (!base_path || max_backups <= 0)
        return 0;

    char path[512];

    /* Drop the oldest backup that would fall out of the window */
    snprintf(path, sizeof(path), "%s.%d", base_path, max_backups);
    unlink(path);

    /* Shift .N-1 -> .N, ..., .1 -> .2 */
    for (int i = max_backups - 1; i >= 1; i--) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s.%d", base_path, i);
        snprintf(dst, sizeof(dst), "%s.%d", base_path, i + 1);
        if (access(src, F_OK) == 0) {
            rename(src, dst);
        }
    }

    /* Active file becomes .1 */
    snprintf(path, sizeof(path), "%s.1", base_path);
    if (access(base_path, F_OK) == 0) {
        rename(base_path, path);
    }

    return 0;
}
