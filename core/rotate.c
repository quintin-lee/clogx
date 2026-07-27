#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include "rotate.h"

// Ensure directory exists, create if not (static helper)
static int ensure_directory(const char *path) {
    char dir[1024];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *last_slash = strrchr(dir, '/');
    if (!last_slash || last_slash == dir) return 0;

    *last_slash = '\0';
    if (strlen(dir) == 0) return 0;

    if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

int file_rotate_file(const char *base_path, int max_backups) {
    if (!base_path || max_backups <= 0) return 0;

    for (int i = max_backups - 1; i >= 1; i--) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s.%d", base_path, i - 1);
        snprintf(dst, sizeof(dst), "%s.%d", base_path, i);

        if (access(src, F_OK) == 0) {
            rename(src, dst);
        }
    }

    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%.511s.1", base_path);
    if (access(base_path, F_OK) == 0) {
        rename(base_path, backup_path);
    }

    return 0;
}