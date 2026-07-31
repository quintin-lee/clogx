/**
 * @file plugin_loader.h
 * @brief Internal declarations for the plugin loader module.
 *
 * These functions are used by the config parser, dispatcher, and log
 * lifecycle but are NOT part of the public API.
 */
#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include "log_config.h"
#include "log_sink.h"

/**
 * @brief Load and create plugin sinks from config entries.
 *
 * @param[in]  cfg        Current configuration.
 * @param[out] out_sinks  Array receiving the created sinks.
 * @param[in]  max_sinks  Capacity of @p out_sinks.
 * @return Number of sinks created, or -1 on error.
 */
int log_plugin_create_sinks_from_config(const log_config_t *cfg,
                                        log_sink_t        **out_sinks,
                                        int                 max_sinks);

/**
 * @brief Unload all cached plugin handles.
 *
 * Called from @ref log_destroy.  All sinks MUST have been destroyed first.
 * Idempotent.
 */
void log_plugin_shutdown_all(void);

#endif /* PLUGIN_LOADER_H */
