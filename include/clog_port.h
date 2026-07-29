/**
 * @file clog_port.h
 * @brief Cross-platform OS abstraction header (POSIX / Windows).
 */

#ifndef CLOG_PORT_H
#define CLOG_PORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef R_OK
#define R_OK 4
#endif
#ifndef F_OK
#define F_OK 0
#endif

typedef struct _stat64 clog_stat_t;
#define clog_fstat(fd, st) _fstat64((fd), (st))
#define clog_stat(path, st) _stat64((path), (st))

/* Process & File utilities */
#define clog_getpid() ((uint32_t)GetCurrentProcessId())
#define clog_sleep_ms(ms) Sleep(ms)
#define clog_access(path, mode) _access((path), (mode))
#define clog_unlink(path) _unlink(path)
#define clog_mkdir(path) _mkdir(path)

/* Sockets */
typedef SOCKET clog_socket_t;
typedef int clog_sock_size_t;
#define CLOG_INVALID_SOCKET INVALID_SOCKET
#define clog_is_invalid_socket(s) ((s) == INVALID_SOCKET)
#define clog_close_socket(s) closesocket(s)

static inline int clog_net_init(void) {
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data);
}
static inline void clog_net_cleanup(void) {
    WSACleanup();
}

/* Mutex via SRWLOCK */
typedef SRWLOCK clog_mutex_t;
#define CLOG_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int clog_mutex_init(clog_mutex_t *m) {
    InitializeSRWLock(m);
    return 0;
}
static inline void clog_mutex_destroy(clog_mutex_t *m) {
    (void)m;
}
static inline void clog_mutex_lock(clog_mutex_t *m) {
    AcquireSRWLockExclusive(m);
}
static inline void clog_mutex_unlock(clog_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
}

/* RWLock via SRWLOCK */
typedef SRWLOCK clog_rwlock_t;
#define CLOG_RWLOCK_INITIALIZER SRWLOCK_INIT

static inline int clog_rwlock_init(clog_rwlock_t *rw) {
    InitializeSRWLock(rw);
    return 0;
}
static inline void clog_rwlock_destroy(clog_rwlock_t *rw) {
    (void)rw;
}
static inline void clog_rwlock_rdlock(clog_rwlock_t *rw) {
    AcquireSRWLockShared(rw);
}
static inline void clog_rwlock_rdunlock(clog_rwlock_t *rw) {
    ReleaseSRWLockShared(rw);
}
static inline void clog_rwlock_wrlock(clog_rwlock_t *rw) {
    AcquireSRWLockExclusive(rw);
}
static inline void clog_rwlock_wrunlock(clog_rwlock_t *rw) {
    ReleaseSRWLockExclusive(rw);
}

/* Condition Variable via CONDITION_VARIABLE */
typedef CONDITION_VARIABLE clog_cond_t;
#define CLOG_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int clog_cond_init(clog_cond_t *c) {
    InitializeConditionVariable(c);
    return 0;
}
static inline void clog_cond_destroy(clog_cond_t *c) {
    (void)c;
}
static inline void clog_cond_wait(clog_cond_t *c, clog_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline void clog_cond_signal(clog_cond_t *c) {
    WakeConditionVariable(c);
}
static inline void clog_cond_broadcast(clog_cond_t *c) {
    WakeAllConditionVariable(c);
}

/* Threads */
typedef HANDLE clog_thread_t;

typedef struct {
    void *(*func)(void *);
    void *arg;
} clog_thread_arg_t;

static inline DWORD WINAPI clog_thread_proc(LPVOID lpParam) {
    clog_thread_arg_t *targ = (clog_thread_arg_t *)lpParam;
    void *(*func)(void *) = targ->func;
    void *arg = targ->arg;
    free(targ);
    func(arg);
    return 0;
}

static inline int clog_thread_create(clog_thread_t *t, void *(*func)(void *), void *arg) {
    clog_thread_arg_t *targ = (clog_thread_arg_t *)malloc(sizeof(clog_thread_arg_t));
    if (!targ)
        return -1;
    targ->func = func;
    targ->arg = arg;
    *t = CreateThread(NULL, 0, clog_thread_proc, targ, 0, NULL);
    if (*t == NULL) {
        free(targ);
        return -1;
    }
    return 0;
}

static inline int clog_thread_join(clog_thread_t t) {
    if (t != NULL && t != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
    return 0;
}

static inline struct tm *clog_localtime_r(const time_t *timep, struct tm *result) {
    return (localtime_s(result, timep) == 0) ? result : NULL;
}
static inline struct tm *clog_gmtime_r(const time_t *timep, struct tm *result) {
    return (gmtime_s(result, timep) == 0) ? result : NULL;
}

#else

#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef struct stat clog_stat_t;
#define clog_fstat(fd, st) fstat((fd), (st))
#define clog_stat(path, st) stat((path), (st))

#define clog_getpid() ((uint32_t)getpid())
#define clog_sleep_ms(ms) usleep((useconds_t)(ms) * 1000)
#define clog_access(path, mode) access((path), (mode))
#define clog_unlink(path) unlink(path)
#define clog_mkdir(path) mkdir((path), 0755)

typedef int clog_socket_t;
typedef size_t clog_sock_size_t;
#define CLOG_INVALID_SOCKET (-1)
#define clog_is_invalid_socket(s) ((s) < 0)
#define clog_close_socket(s) close(s)
static inline int clog_net_init(void) {
    return 0;
}
static inline void clog_net_cleanup(void) {
}

typedef pthread_mutex_t clog_mutex_t;
#define CLOG_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline int clog_mutex_init(clog_mutex_t *m) {
    return pthread_mutex_init(m, NULL);
}
static inline void clog_mutex_destroy(clog_mutex_t *m) {
    pthread_mutex_destroy(m);
}
static inline void clog_mutex_lock(clog_mutex_t *m) {
    pthread_mutex_lock(m);
}
static inline void clog_mutex_unlock(clog_mutex_t *m) {
    pthread_mutex_unlock(m);
}

typedef pthread_rwlock_t clog_rwlock_t;
#define CLOG_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER

static inline int clog_rwlock_init(clog_rwlock_t *rw) {
    return pthread_rwlock_init(rw, NULL);
}
static inline void clog_rwlock_destroy(clog_rwlock_t *rw) {
    pthread_rwlock_destroy(rw);
}
static inline void clog_rwlock_rdlock(clog_rwlock_t *rw) {
    pthread_rwlock_rdlock(rw);
}
static inline void clog_rwlock_rdunlock(clog_rwlock_t *rw) {
    pthread_rwlock_unlock(rw);
}
static inline void clog_rwlock_wrlock(clog_rwlock_t *rw) {
    pthread_rwlock_wrlock(rw);
}
static inline void clog_rwlock_wrunlock(clog_rwlock_t *rw) {
    pthread_rwlock_unlock(rw);
}

typedef pthread_cond_t clog_cond_t;
#define CLOG_COND_INITIALIZER PTHREAD_COND_INITIALIZER

static inline int clog_cond_init(clog_cond_t *c) {
    return pthread_cond_init(c, NULL);
}
static inline void clog_cond_destroy(clog_cond_t *c) {
    pthread_cond_destroy(c);
}
static inline void clog_cond_wait(clog_cond_t *c, clog_mutex_t *m) {
    pthread_cond_wait(c, m);
}
static inline void clog_cond_signal(clog_cond_t *c) {
    pthread_cond_signal(c);
}
static inline void clog_cond_broadcast(clog_cond_t *c) {
    pthread_cond_broadcast(c);
}

typedef pthread_t clog_thread_t;
static inline int clog_thread_create(clog_thread_t *t, void *(*func)(void *), void *arg) {
    return pthread_create(t, NULL, func, arg);
}
static inline int clog_thread_join(clog_thread_t t) {
    return pthread_join(t, NULL);
}

static inline struct tm *clog_localtime_r(const time_t *timep, struct tm *result) {
    return localtime_r(timep, result);
}
static inline struct tm *clog_gmtime_r(const time_t *timep, struct tm *result) {
    return gmtime_r(timep, result);
}

#endif

#ifndef F_OK
#define F_OK 0
#endif

#if defined(_MSC_VER)
#define clog_thread_local __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define clog_thread_local __thread
#else
#define clog_thread_local _Thread_local
#endif

#endif /* CLOG_PORT_H */
