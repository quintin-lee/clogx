#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "log_record.h"
#include "log_sink.h"

// Initialize the dispatcher with sinks from config
int log_dispatcher_init(void);

// Destroy the dispatcher and all sinks
void log_dispatcher_destroy(void);

// Flush all active sinks
void log_dispatcher_flush(void);

// Dispatch a log record to all sinks
int log_dispatcher_dispatch(log_record_t *record);

// Add a sink to the dispatcher
int log_dispatcher_add_sink(log_sink_t *sink);

// Remove a sink from the dispatcher
int log_dispatcher_remove_sink(log_sink_t *sink);

#endif // DISPATCHER_H
