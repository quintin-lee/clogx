#ifndef LOG_SINK_H
#define LOG_SINK_H

#include <stddef.h>
#include "log_record.h"

// Sink function pointers
typedef struct log_sink log_sink_t;

struct log_sink {
    int (*write)(log_sink_t *, const char *, size_t);
    void (*flush)(log_sink_t *);
    void (*destroy)(log_sink_t *);
    void *private_data;
};

// Create a console sink
log_sink_t *console_sink_create(void);

// Create a file sink with parameters
// path: log file path, max_size: max size in bytes before rotation, backups: number of backup files
log_sink_t *file_sink_create(const char *path, uint64_t max_size_t, int backups);

// Create a socket sink (TCP)
// host: server address, port: server port
log_sink_t *socket_sink_create(const char *host, int port);

#endif // LOG_SINK_H
