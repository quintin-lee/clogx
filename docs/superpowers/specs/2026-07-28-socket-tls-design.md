# Design Spec: Optional OpenSSL TLS Support for Socket Sink

**Date**: 2026-07-28  
**Topic**: OpenSSL TLS Encryption for Socket Sink (`CLOG_USE_TLS`)  
**Status**: Approved  

---

## 1. Overview

Currently, `clogx` only supports plain TCP output in `sinks/socket_sink.c`. This spec adds optional TLS/SSL transport layer security for the socket sink via an optional OpenSSL integration guarded by `#ifdef CLOG_USE_TLS`.

---

## 2. Configuration & API Extensions

### 2.1 Configuration Struct (`include/log_config.h`)

Add three TLS fields to `log_config_t`:

```c
typedef struct {
    ...
    int socket_enable;       /**< Non-zero to enable TCP socket sink. */
    char socket_host[CLOG_MAX_PATH_SIZE];   /**< Socket peer IPv4 address. */
    int socket_port;         /**< Socket peer port. */
    bool socket_tls;         /**< Enable TLS encryption for socket sink. */
    char socket_tls_ca_file[CLOG_MAX_PATH_SIZE]; /**< Path to CA cert file. */
    bool socket_tls_skip_verify; /**< Skip SSL cert verification. */
} log_config_t;
```

### 2.2 YAML Parsing (`core/config.c`)

Add parser support for:
- `socket_tls` / `tls_enable`: boolean
- `socket_tls_ca_file` / `tls_ca_file`: string
- `socket_tls_skip_verify` / `tls_skip_verify`: boolean

Defaults: `socket_tls = false`, `socket_tls_ca_file = ""`, `socket_tls_skip_verify = false`.

### 2.3 Socket Sink Creator (`include/log_sink.h` & `sinks/socket_sink.c`)

Add a creator function for TLS-capable socket sink:

```c
log_sink_t *socket_sink_create_tls(const char *host, int port, bool use_tls,
                                   const char *ca_file, bool skip_verify);
```

`socket_sink_create(host, port)` becomes a inline wrapper calling `socket_sink_create_tls(host, port, false, NULL, false)`.

---

## 3. Implementation Details (`sinks/socket_sink.c`)

### 3.1 Data Structure

```c
typedef struct {
    int sockfd;
    const char *host;
    int port;
    int connected;
    bool use_tls;
    char *ca_file;
    bool skip_verify;
#ifdef CLOG_USE_TLS
    SSL_CTX *ssl_ctx;
    SSL *ssl;
#endif
} socket_sink_data_t;
```

### 3.2 Connection & Handshake (`socket_connect`)

1. Establish standard TCP socket connection.
2. If `data->use_tls`:
   - `#ifdef CLOG_USE_TLS`:
     - Initialize `SSL_CTX_new(TLS_client_method())`.
     - If `ca_file` is set, `SSL_CTX_load_verify_locations(ctx, ca_file, NULL)`.
     - If `skip_verify` is set, `SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL)`.
     - Create `SSL_new(ctx)` and bind socket `SSL_set_fd(ssl, sockfd)`.
     - Call `SSL_connect(ssl)`. If it fails, clean up SSL and socket.
   - `#ifndef CLOG_USE_TLS`:
     - Output error to `stderr`: `"TLS support requested for socket sink, but clogx was compiled without OpenSSL (CLOG_USE_TLS)"`.

### 3.3 Data Transmission & Teardown

- `socket_write`: If `data->use_tls` and `#ifdef CLOG_USE_TLS`, send via `SSL_write(data->ssl, buf + total_sent, len - total_sent)`. Otherwise, use `send()`.
- `socket_destroy`: Shutdown SSL (`SSL_shutdown`, `SSL_free`, `SSL_CTX_free`) before closing socket descriptor.

---

## 4. Build System & CMake Integration

### 4.1 Makefile Integration

Add `TLS ?= 0` flag:
```makefile
ifeq ($(TLS),1)
CFLAGS += -DCLOG_USE_TLS
LDFLAGS += -lssl -lcrypto
endif
```

### 4.2 CMakeLists.txt Integration

Add option:
```cmake
option(CLOG_ENABLE_TLS "Enable OpenSSL TLS support for socket sink" OFF)

if(CLOG_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(clogx_objects PRIVATE CLOG_USE_TLS)
    target_link_libraries(clogx_objects PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
```
And add `include/log_limits.h` to installed `PUBLIC_HEADERS`.

---

## 5. Verification Plan

1. **Unit Tests**:
   - Add test cases in `tests/test_socket_sink.c` testing TLS config parameters parsing, `socket_sink_create_tls` initialization, and non-TLS fallback behavior.
2. **Build Checks**:
   - Test default build (`make clean && make test`).
   - Test TLS build (`make clean && make TLS=1 test`).
   - Test CMake build (`cmake -B build_cmake -DCLOG_ENABLE_TLS=ON && cmake --build build_cmake`).
