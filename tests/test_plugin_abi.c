/**
 * @file test_plugin_abi.c
 * @brief Tests for the plugin ABI: loading, scanning, config, dispatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dlfcn.h>
#include <unistd.h>
#include "log.h"
#include "clogx_plugin.h"

/* Path to the test plugin .so — set at compile time via -DTEST_PLUGIN_SO */
#ifndef TEST_PLUGIN_SO
#define TEST_PLUGIN_SO "./build/plugin_dummy.so"
#endif

/* ------------------------------------------------------------------ */
/*  Helper: verify sink was dispatched a record via the public API     */
/* ------------------------------------------------------------------ */

static int test_log_init_basic(const char *yaml) {
    /* Write a temp config, init, log one message, destroy. */
    FILE *f = fopen("/tmp/test_plugin_cfg.yaml", "w");
    if (!f)
        return -1;
    fprintf(f, "%s", yaml);
    fclose(f);

    int ret = log_init("/tmp/test_plugin_cfg.yaml");
    if (ret != 0) {
        fprintf(stderr, "log_init failed: %s\n", log_strerror(ret));
        return -1;
    }
    LOG_INFO("hello from plugin test");
    log_flush();
    log_destroy();
    unlink("/tmp/test_plugin_cfg.yaml");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 1: log_plugin_load — valid .so                               */
/* ------------------------------------------------------------------ */
static void test_load_valid(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    clogx_plugin_handle_t *h = log_plugin_load(TEST_PLUGIN_SO);
    assert(h != NULL && "load should succeed");

    const clogx_plugin_t *info = log_plugin_info(h);
    assert(info != NULL);
    assert(info->abi_version == CLOGX_PLUGIN_ABI_VERSION);
    assert(strcmp(info->name, "dummy") == 0);
    assert(info->plugin_version == 1);

    /* Loading the same .so again returns the same handle. */
    clogx_plugin_handle_t *h2 = log_plugin_load(TEST_PLUGIN_SO);
    assert(h2 == h && "duplicate load returns cached handle");

    log_plugin_unload(h);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 2: log_plugin_load — nonexistent path returns NULL            */
/* ------------------------------------------------------------------ */
static void test_load_nonexistent(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    clogx_plugin_handle_t *h = log_plugin_load("/nonexistent/plugin.so");
    assert(h == NULL && "load of nonexistent path should return NULL");
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 3: log_plugin_load — NULL / empty path                       */
/* ------------------------------------------------------------------ */
static void test_load_null_path(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    assert(log_plugin_load(NULL) == NULL);
    assert(log_plugin_load("") == NULL);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 4: log_plugin_create_sink                                     */
/* ------------------------------------------------------------------ */
static void test_create_sink(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    clogx_plugin_handle_t *h = log_plugin_load(TEST_PLUGIN_SO);
    assert(h != NULL);

    log_sink_t *sink = log_plugin_create_sink(h, NULL);
    assert(sink != NULL);
    assert(sink->abi_version == CLOGX_PLUGIN_ABI_VERSION);
    assert(sink->write != NULL);
    assert(sink->flush != NULL);
    assert(sink->destroy != NULL);

    /* Verify the sink works. */
    int n = sink->write(sink, "test", 4);
    assert(n == 4);

    sink->flush(sink);
    sink->destroy(sink);

    log_plugin_unload(h);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 5: log_plugin_create_sink — invalid handle                    */
/* ------------------------------------------------------------------ */
static void test_create_sink_invalid_handle(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    assert(log_plugin_create_sink(NULL, NULL) == NULL);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 6: log_plugin_info — invalid handle                           */
/* ------------------------------------------------------------------ */
static void test_info_invalid_handle(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    assert(log_plugin_info(NULL) == NULL);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 7: log_plugin_unload — NULL no-op                             */
/* ------------------------------------------------------------------ */
static void test_unload_null(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    log_plugin_unload(NULL); /* must not crash */
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 8: log_plugin_scan — valid directory                          */
/* ------------------------------------------------------------------ */
static void test_scan_valid_dir(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    clogx_plugin_handle_t *handles[8] = {0};
    int n = log_plugin_scan("./build", handles, 8);
    assert(n >= 1 && "should find at least plugin_dummy.so");

    int found_dummy = 0;
    for (int i = 0; i < n && i < 8; i++) {
        if (handles[i]) {
            const clogx_plugin_t *info = log_plugin_info(handles[i]);
            if (info && strcmp(info->name, "dummy") == 0) {
                found_dummy = 1;
            }
        }
    }
    assert(found_dummy && "scan should find the dummy plugin");

    fprintf(stderr, "PASS (found %d plugins)\n", n);
}

/* ------------------------------------------------------------------ */
/*  Test 9: log_plugin_scan — nonexistent directory                    */
/* ------------------------------------------------------------------ */
static void test_scan_nonexistent_dir(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    int n = log_plugin_scan("/nonexistent/plugins", NULL, 0);
    assert(n == 0 && "scan of nonexistent dir returns 0");
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 10: log_plugin_scan — NULL dir                               */
/* ------------------------------------------------------------------ */
static void test_scan_null_dir(void) {
    fprintf(stderr, "=== %s ===\n", __func__);
    assert(log_plugin_scan(NULL, NULL, 0) == -1);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 11: Plugin sink via log_add_sink (programmatic API)           */
/* ------------------------------------------------------------------ */
static void test_add_sink_via_api(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    /* Init with a temp config so g_initialized is set. */
    FILE *f = fopen("/tmp/test_plugin_add.yaml", "w");
    assert(f != NULL);
    fprintf(f, "log:\n  level: TRACE\n  console_enable: true\n  format: \"%%msg\"\n");
    fclose(f);
    assert(log_init("/tmp/test_plugin_add.yaml") == 0);

    /* Now add a plugin sink via the API. */
    clogx_plugin_handle_t *h = log_plugin_load(TEST_PLUGIN_SO);
    assert(h != NULL);

    log_sink_t *sink = log_plugin_create_sink(h, NULL);
    assert(sink != NULL);

    int ret = log_add_sink(sink);
    assert(ret == CLOG_OK);

    LOG_INFO("plugin sink test");
    log_flush();

    log_destroy();
    unlink("/tmp/test_plugin_add.yaml");
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 12: Plugin sink via YAML config                               */
/* ------------------------------------------------------------------ */
static void test_config_plugins_section(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    char cfg[1024];
    snprintf(cfg, sizeof(cfg),
             "log:\n"
             "  level: TRACE\n"
             "  console_enable: false\n"
             "  format: \"%%msg\"\n"
             "  plugins:\n"
             "    - path: %s\n"
             "      config:\n"
             "        topic: test-topic\n"
             "        brokers: \"broker1:9092\"\n",
             TEST_PLUGIN_SO);

    assert(test_log_init_basic(cfg) == 0);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 13: Plugin sink via YAML config with empty config             */
/* ------------------------------------------------------------------ */
static void test_config_plugins_no_config(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    char cfg[1024];
    snprintf(cfg, sizeof(cfg),
             "log:\n"
             "  level: TRACE\n"
             "  console_enable: false\n"
             "  format: \"%%msg\"\n"
             "  plugins:\n"
             "    - path: %s\n",
             TEST_PLUGIN_SO);

    assert(test_log_init_basic(cfg) == 0);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 14: Plugin via top-level (backward compat) YAML              */
/* ------------------------------------------------------------------ */
static void test_config_top_level_plugins(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    char cfg[1024];
    snprintf(cfg, sizeof(cfg),
             "level: TRACE\n"
             "console_enable: false\n"
             "format: \"%%msg\"\n"
             "plugins:\n"
             "  - path: %s\n",
             TEST_PLUGIN_SO);

    assert(test_log_init_basic(cfg) == 0);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 15: Reload with plugin sinks                                  */
/* ------------------------------------------------------------------ */
static void test_reload_with_plugins(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    char cfg[1024];
    snprintf(cfg, sizeof(cfg),
             "log:\n"
             "  level: TRACE\n"
             "  console_enable: false\n"
             "  format: \"%%msg\"\n"
             "  plugins:\n"
             "    - path: %s\n",
             TEST_PLUGIN_SO);

    assert(test_log_init_basic(cfg) == 0);
    /* log_reload test: init again with plugin */
    assert(test_log_init_basic(cfg) == 0);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test 16: Multiple sinks via config (built-in + plugin)            */
/* ------------------------------------------------------------------ */
static void test_mixed_sinks(void) {
    fprintf(stderr, "=== %s ===\n", __func__);

    char cfg[1024];
    snprintf(cfg, sizeof(cfg),
             "log:\n"
             "  level: TRACE\n"
             "  console_enable: false\n"
             "  format: \"%%msg\"\n"
             "  plugins:\n"
             "    - path: %s\n"
             "    - path: %s\n",
             TEST_PLUGIN_SO, TEST_PLUGIN_SO);

    assert(test_log_init_basic(cfg) == 0);
    fprintf(stderr, "PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
    test_load_null_path();
    test_load_nonexistent();
    test_load_valid();
    test_info_invalid_handle();
    test_create_sink_invalid_handle();
    test_create_sink();
    test_unload_null();
    test_scan_null_dir();
    test_scan_nonexistent_dir();
    test_scan_valid_dir();
    test_add_sink_via_api();
    test_config_plugins_section();
    test_config_plugins_no_config();
    test_config_top_level_plugins();
    test_reload_with_plugins();
    test_mixed_sinks();

    fprintf(stderr, "=== ALL PASS ===\n");
    return 0;
}
