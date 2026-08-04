/**
 * @file clog_port.h
 * @brief Cross-platform OS abstraction layer (POSIX / Windows).
 *
 * Provides unified APIs for threading, synchronization, sockets, file I/O,
 * time, and console control that hide platform differences between POSIX
 * (pthreads) and Windows (Win32 / Winsock).
 *
 * ## Abstraction Categories
 *
 * | Category     | Types / Functions                                      |
 * |-------------|--------------------------------------------------------|
 * | Mutex       | clog_mutex_t, clog_mutex_{init,destroy,lock,unlock}   |
 * | RWLock      | clog_rwlock_t, clog_rwlock_{rd,wr}{lock,unlock}       |
 * | Cond Var    | clog_cond_t, clog_cond_{init,destroy,wait,signal,broadcast} |
 * | Thread      | clog_thread_t, clog_thread_{create,join}              |
 * | Socket      | clog_socket_t, clog_{net_init,close_socket,is_invalid_socket} |
 * | File/Proc   | clog_{getpid,sleep_ms,access,unlink,mkdir,fstat}      |
 * | Time        | clog_{get_timestamp_us,get_now_ms,localtime_r,gmtime_r} |
 * | Thread ID   | clog_get_thread_id                                     |
 * | RAII Mutex  | CLOG_MUTEXGUARDED(m, code)                             |
 * | Thread-Loc  | clog_thread_local                                      |
 * | Console     | clog_console_enable_vt_mode (Windows VT processing)    |
 */

#ifndef CLOG_PORT_H
#define CLOG_PORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef strdup
#define strdup _strdup
#endif
#ifndef fileno
#define fileno _fileno
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#endif

#ifndef R_OK
#define R_OK 4
#endif
#ifndef F_OK
#define F_OK 0
#endif

#if defined(_WIN32) || defined(_WIN64)
#ifndef ssize_t
typedef SSIZE_T ssize_t;
#endif
#ifndef setenv
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif
#ifndef unsetenv
#define unsetenv(name) _putenv_s(name, "")
#endif
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
typedef int    clog_sock_size_t;
#define CLOG_INVALID_SOCKET INVALID_SOCKET
#define clog_is_invalid_socket(s) ((s) == INVALID_SOCKET)
static inline void clog_close_socket(clog_socket_t s)
{
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

static inline int clog_net_init(void)
{
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data);
}
static inline void clog_net_cleanup(void)
{
    WSACleanup();
}

/* ── Mutex via SRWLOCK ── */

typedef SRWLOCK clog_mutex_t;
#define CLOG_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int clog_mutex_init(clog_mutex_t *m)
{
    InitializeSRWLock(m);
    return 0;
}
static inline void clog_mutex_destroy(clog_mutex_t *m)
{
    (void)m;
}
static inline void clog_mutex_lock(clog_mutex_t *m)
{
    AcquireSRWLockExclusive(m);
}
static inline void clog_mutex_unlock(clog_mutex_t *m)
{
    ReleaseSRWLockExclusive(m);
}

/* ── RWLock via SRWLOCK ── */

typedef SRWLOCK clog_rwlock_t;
#define CLOG_RWLOCK_INITIALIZER SRWLOCK_INIT

static inline int clog_rwlock_init(clog_rwlock_t *rw)
{
    InitializeSRWLock(rw);
    return 0;
}
static inline void clog_rwlock_destroy(clog_rwlock_t *rw)
{
    (void)rw;
}
static inline void clog_rwlock_rdlock(clog_rwlock_t *rw)
{
    AcquireSRWLockShared(rw);
}
static inline void clog_rwlock_rdunlock(clog_rwlock_t *rw)
{
    ReleaseSRWLockShared(rw);
}
static inline void clog_rwlock_wrlock(clog_rwlock_t *rw)
{
    AcquireSRWLockExclusive(rw);
}
static inline void clog_rwlock_wrunlock(clog_rwlock_t *rw)
{
    ReleaseSRWLockExclusive(rw);
}

/* ── Condition Variable via CONDITION_VARIABLE ── */

typedef CONDITION_VARIABLE clog_cond_t;
#define CLOG_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int clog_cond_init(clog_cond_t *c)
{
    InitializeConditionVariable(c);
    return 0;
}
static inline void clog_cond_destroy(clog_cond_t *c)
{
    (void)c;
}
static inline void clog_cond_wait(clog_cond_t *c, clog_mutex_t *m)
{
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline void clog_cond_signal(clog_cond_t *c)
{
    WakeConditionVariable(c);
}
static inline void clog_cond_broadcast(clog_cond_t *c)
{
    WakeAllConditionVariable(c);
}

/* ── Threads via Win32 CreateThread ── */

typedef HANDLE clog_thread_t;

typedef struct {
    void *(*func)(void *);
    void *arg;
} clog_thread_arg_t;

static inline DWORD WINAPI clog_thread_proc(LPVOID lpParam)
{
    clog_thread_arg_t *targ = (clog_thread_arg_t *)lpParam;
    void *(*func)(void *)   = targ->func;
    void *arg               = targ->arg;
    free(targ);
    func(arg);
    return 0;
}

static inline int clog_thread_create(clog_thread_t *t, void *(*func)(void *), void *arg)
{
    clog_thread_arg_t *targ = (clog_thread_arg_t *)malloc(sizeof(clog_thread_arg_t));
    if (!targ) {
        return -1;
    }
    targ->func = func;
    targ->arg  = arg;
    *t         = CreateThread(NULL, 0, clog_thread_proc, targ, 0, NULL);
    if (*t == NULL) {
        free(targ);
        return -1;
    }
    return 0;
}

static inline int clog_thread_join(clog_thread_t t)
{
    if (t != NULL && t != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
    return 0;
}

static inline struct tm *clog_localtime_r(const time_t *timep, struct tm *result)
{
    return (localtime_s(result, timep) == 0) ? result : NULL;
}
static inline struct tm *clog_gmtime_r(const time_t *timep, struct tm *result)
{
    return (gmtime_s(result, timep) == 0) ? result : NULL;
}

#else

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__SANITIZE_THREAD__) && defined(__APPLE__)
#include <sanitizer/tsan_interface.h>
#define CLOG_TSAN_MUTEX_CREATE(m) __tsan_mutex_create((m), __tsan_mutex_not_static)
#define CLOG_TSAN_MUTEX_DESTROY(m) __tsan_mutex_destroy((m), __tsan_mutex_not_static)
#else
#define CLOG_TSAN_MUTEX_CREATE(m)
#define CLOG_TSAN_MUTEX_DESTROY(m)
#endif

typedef struct stat clog_stat_t;
#define clog_fstat(fd, st) fstat((fd), (st))
#define clog_stat(path, st) stat((path), (st))

#define clog_getpid() ((uint32_t)getpid())
#define clog_sleep_ms(ms) usleep((useconds_t)(ms) * 1000)
#define clog_access(path, mode) access((path), (mode))
#define clog_unlink(path) unlink(path)
#define clog_mkdir(path) mkdir((path), 0755)

typedef int       clog_socket_t;
typedef size_t    clog_sock_size_t;
#define CLOG_INVALID_SOCKET (-1)
#define clog_is_invalid_socket(s) ((s) < 0)
#define clog_close_socket(s) close(s)
static inline int clog_net_init(void)
{
    return 0;
}
static inline void clog_net_cleanup(void)
{
}

/* ── Mutex via pthread_mutex_t ── */

typedef pthread_mutex_t clog_mutex_t;
#define CLOG_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline int clog_mutex_init(clog_mutex_t *m)
{
    int ret = pthread_mutex_init(m, NULL);
    CLOG_TSAN_MUTEX_CREATE(m);
    return ret;
}
static inline void clog_mutex_destroy(clog_mutex_t *m)
{
    CLOG_TSAN_MUTEX_DESTROY(m);
    pthread_mutex_destroy(m);
}
static inline void clog_mutex_lock(clog_mutex_t *m)
{
    pthread_mutex_lock(m);
}
static inline void clog_mutex_unlock(clog_mutex_t *m)
{
    pthread_mutex_unlock(m);
}

/* ── RWLock via pthread_rwlock_t ── */

typedef pthread_rwlock_t clog_rwlock_t;
#define CLOG_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER

static inline int clog_rwlock_init(clog_rwlock_t *rw)
{
    return pthread_rwlock_init(rw, NULL);
}
static inline void clog_rwlock_destroy(clog_rwlock_t *rw)
{
    pthread_rwlock_destroy(rw);
}
static inline void clog_rwlock_rdlock(clog_rwlock_t *rw)
{
    pthread_rwlock_rdlock(rw);
}
static inline void clog_rwlock_rdunlock(clog_rwlock_t *rw)
{
    pthread_rwlock_unlock(rw);
}
static inline void clog_rwlock_wrlock(clog_rwlock_t *rw)
{
    pthread_rwlock_wrlock(rw);
}
static inline void clog_rwlock_wrunlock(clog_rwlock_t *rw)
{
    pthread_rwlock_unlock(rw);
}

/* ── Condition Variable via pthread_cond_t ── */

typedef pthread_cond_t clog_cond_t;
#define CLOG_COND_INITIALIZER PTHREAD_COND_INITIALIZER

static inline int clog_cond_init(clog_cond_t *c)
{
    return pthread_cond_init(c, NULL);
}
static inline void clog_cond_destroy(clog_cond_t *c)
{
    pthread_cond_destroy(c);
}
static inline void clog_cond_wait(clog_cond_t *c, clog_mutex_t *m)
{
    pthread_cond_wait(c, m);
}
static inline void clog_cond_signal(clog_cond_t *c)
{
    pthread_cond_signal(c);
}
static inline void clog_cond_broadcast(clog_cond_t *c)
{
    pthread_cond_broadcast(c);
}

/* ── Threads via pthreads ── */

typedef pthread_t clog_thread_t;
static inline int clog_thread_create(clog_thread_t *t, void *(*func)(void *), void *arg)
{
    return pthread_create(t, NULL, func, arg);
}
static inline int clog_thread_join(clog_thread_t t)
{
    return pthread_join(t, NULL);
}

static inline struct tm *clog_localtime_r(const time_t *timep, struct tm *result)
{
    return localtime_r(timep, result);
}
static inline struct tm *clog_gmtime_r(const time_t *timep, struct tm *result)
{
    return gmtime_r(timep, result);
}

#endif

#ifndef F_OK
#define F_OK 0
#endif

/**
 * @def CLOG_MUTEXGUARDED(m, code)
 * @brief RAII-style scoped mutex lock — auto-unlocks when the enclosing block exits.
 *
 * On GCC/Clang: uses __attribute__((cleanup)) for automatic unlock.
 * On MSVC: uses __try/__finally. On other compilers: manual lock/unlock.
 */
#if defined(__GNUC__) || defined(__clang__)

/** @brief Cleanup helper for CLOG_MUTEXGUARDED; unlocks on scope exit. */
static inline void clog_mutex_unlock_ptr(clog_mutex_t **m)
{
    if (*m) {
        clog_mutex_unlock(*m);
    }
}
#define CLOG_MUTEXGUARDED(m, code)                                                                 \
    do {                                                                                           \
        clog_mutex_lock(m);                                                                        \
        __attribute__((cleanup(clog_mutex_unlock_ptr))) clog_mutex_t *__clog_g = (m);              \
        (void)__clog_g;                                                                            \
        code;                                                                                      \
    } while (0)

#elif defined(_MSC_VER)

#define CLOG_MUTEXGUARDED(m, code)                                                                 \
    do {                                                                                           \
        clog_mutex_lock(m);                                                                        \
        __try {                                                                                    \
            code;                                                                                  \
        } __finally {                                                                              \
            clog_mutex_unlock(m);                                                                  \
        }                                                                                          \
    } while (0)

#else

#define CLOG_MUTEXGUARDED(m, code)                                                                 \
    do {                                                                                           \
        clog_mutex_lock(m);                                                                        \
        code;                                                                                      \
        clog_mutex_unlock(m);                                                                      \
    } while (0)

#endif

#if defined(_MSC_VER)
#define clog_thread_local __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define clog_thread_local __thread
#else
#define clog_thread_local _Thread_local
#endif

/* ── Atomic 64-bit operations ── */

static inline uint64_t clog_atomic_inc64(volatile uint64_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_fetch_add(ptr, 1ULL, __ATOMIC_RELAXED) + 1ULL;
#elif defined(_WIN32) || defined(_WIN64)
    return (uint64_t)InterlockedIncrement64((volatile LONG64 *)ptr);
#else
    uint64_t val = ++(*ptr);
    return val;
#endif
}

static inline uint64_t clog_atomic_get64(volatile uint64_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
#elif defined(_WIN32) || defined(_WIN64)
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)ptr, 0, 0);
#else
    return *ptr;
#endif
}

/* ── Atomic size_t operations (for lock-free ring buffers) ── */

static inline size_t clog_atomic_load_sz(const volatile size_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    return (size_t)InterlockedCompareExchange64((volatile LONG64 *)ptr, 0, 0);
#else
    return *ptr;
#endif
}

static inline void clog_atomic_store_sz(volatile size_t *ptr, size_t val)
{
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
#elif defined(_WIN32) || defined(_WIN64)
    InterlockedExchange64((volatile LONG64 *)ptr, (LONG64)val);
#else
    *ptr = val;
#endif
}

static inline size_t clog_atomic_fetch_add_sz(volatile size_t *ptr, size_t n)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_fetch_add(ptr, n, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    return (size_t)InterlockedExchangeAdd64((volatile LONG64 *)ptr, (LONG64)n);
#else
    size_t old = *ptr;
    *ptr += n;
    return old;
#endif
}

static inline size_t clog_atomic_fetch_sub_sz(volatile size_t *ptr, size_t n)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_fetch_sub(ptr, n, __ATOMIC_RELEASE);
#elif defined(_WIN32) || defined(_WIN64)
    return (size_t)InterlockedExchangeAdd64((volatile LONG64 *)ptr, -(LONG64)n);
#else
    size_t old = *ptr;
    *ptr -= n;
    return old;
#endif
}

static inline int clog_atomic_cas_sz(volatile size_t *ptr, size_t *expected, size_t desired)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_compare_exchange_n(
        ptr, expected, desired, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    size_t old = (size_t)InterlockedCompareExchange64(
        (volatile LONG64 *)ptr, (LONG64)desired, (LONG64)*expected);
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
#else
    if (*ptr == *expected) {
        *ptr = desired;
        return 1;
    }
    *expected = *ptr;
    return 0;
#endif
}

/* ── Atomic int operations (for closed flags in ring buffers) ── */

static inline int clog_atomic_load_int(const volatile int *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    return (int)InterlockedCompareExchange((LONG *)ptr, 0, 0);
#else
    return *ptr;
#endif
}

static inline void clog_atomic_store_int(volatile int *ptr, int val)
{
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
#elif defined(_WIN32) || defined(_WIN64)
    InterlockedExchange((LONG *)ptr, (LONG)val);
#else
    *ptr = val;
#endif
}

/* ── Atomic uint64_t operations with acquire/release ordering (for seq counters) ── */

static inline uint64_t clog_atomic_load_u64(const volatile uint64_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)ptr, 0, 0);
#else
    return *ptr;
#endif
}

static inline void clog_atomic_store_u64(volatile uint64_t *ptr, uint64_t val)
{
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
#elif defined(_WIN32) || defined(_WIN64)
    InterlockedExchange64((volatile LONG64 *)ptr, (LONG64)val);
#else
    *ptr = val;
#endif
}

/* ── Semaphore ── */

#if defined(_WIN32) || defined(_WIN64)

typedef HANDLE clog_sem_t;

static inline int clog_sem_init(clog_sem_t *sem, long initial)
{
    *sem = CreateSemaphoreA(NULL, initial, 0x7FFFFFFF, NULL);
    return *sem ? 0 : -1;
}

static inline void clog_sem_destroy(clog_sem_t *sem)
{
    if (sem && *sem) {
        CloseHandle(*sem);
        *sem = NULL;
    }
}

static inline void clog_sem_wait(clog_sem_t *sem)
{
    WaitForSingleObject(*sem, INFINITE);
}

static inline int clog_sem_trywait(clog_sem_t *sem)
{
    DWORD result = WaitForSingleObject(*sem, 0);
    return (result == WAIT_OBJECT_0) ? 0 : -1;
}

static inline void clog_sem_post(clog_sem_t *sem)
{
    ReleaseSemaphore(*sem, 1, NULL);
}

#elif defined(__APPLE__)

/*
 * macOS does not implement POSIX unnamed semaphores: sem_init() always
 * returns -1 with ENOSYS. Named semaphores (sem_open) ARE supported and are
 * the fork-safe choice: they live in the kernel and a pthread_atfork child
 * handler may close + re-open them to obtain a fresh, independent object.
 *
 * (Grand Central Dispatch semaphores are NOT fork-safe: using a
 * dispatch_semaphore_t from a child process after fork() traps with
 * __builtin_trap → SIGTRAP. That broke log_async_atfork_child_for().)
 */
#include <fcntl.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Named semaphore wrapper. The name embeds the pid (isolation between
 * processes) and the address of this object (isolation between the multiple
 * queues a single process may own). macOS limits names to PSEMNAMLEN (31).
 */
typedef struct {
    sem_t *ptr;      /**< Kernel semaphore handle (NULL when uninitialised). */
    char   name[32]; /**< Registered name, used by sem_unlink(). */
} clog_sem_t;

static inline int clog_sem_init(clog_sem_t *sem, long initial)
{
    if (!sem) {
        return -1;
    }
    snprintf(sem->name,
             sizeof(sem->name),
             "/clogx%ld%lx",
             (long)getpid(),
             (unsigned long)(uintptr_t)sem & 0xFFFFFFFFu);
    /* Clear any stale object left behind by a crashed process. */
    sem_unlink(sem->name);
    sem->ptr = sem_open(sem->name, O_CREAT | O_EXCL, 0600, (unsigned int)initial);
    if (sem->ptr == SEM_FAILED) {
        sem->ptr = NULL;
        return -1;
    }
    return 0;
}

static inline void clog_sem_destroy(clog_sem_t *sem)
{
    if (sem && sem->ptr) {
        sem_close(sem->ptr);
        sem_unlink(sem->name);
        sem->ptr = NULL;
    }
}

static inline void clog_sem_wait(clog_sem_t *sem)
{
    while (sem->ptr && sem_wait(sem->ptr) != 0) {
        /* Retry on EINTR. */
    }
}

static inline int clog_sem_trywait(clog_sem_t *sem)
{
    if (!sem || !sem->ptr) {
        return -1;
    }
    return sem_trywait(sem->ptr);
}

static inline void clog_sem_post(clog_sem_t *sem)
{
    if (sem && sem->ptr) {
        sem_post(sem->ptr);
    }
}

#else /* POSIX */

#include <semaphore.h>

typedef sem_t clog_sem_t;

static inline int clog_sem_init(clog_sem_t *sem, long initial)
{
    return sem_init(sem, 0, (unsigned int)initial);
}

static inline void clog_sem_destroy(clog_sem_t *sem)
{
    sem_destroy(sem);
}

static inline void clog_sem_wait(clog_sem_t *sem)
{
    while (sem_wait(sem) != 0) {
        /* Retry on EINTR. */
    }
}

static inline int clog_sem_trywait(clog_sem_t *sem)
{
    return sem_trywait(sem);
}

static inline void clog_sem_post(clog_sem_t *sem)
{
    sem_post(sem);
}

#endif /* POSIX / Windows */

/* ── Time & Thread utilities ── */

/**
 * @brief Get current wall-clock time as microseconds since Unix epoch.
 *
 * Uses CLOCK_REALTIME on POSIX, FILETIME on Windows (with epoch adjustment).
 */
static inline uint64_t clog_get_timestamp_us(void)
{
#if defined(_WIN32) || defined(_WIN64)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
#endif
}

/**
 * @brief Get the current thread ID as a uint32_t.
 *
 * On POSIX, pthread_t is hashed (XOR-fold) to fit in 32 bits.
 * On Windows, GetCurrentThreadId() is used directly.
 */
static inline uint32_t clog_get_thread_id(void)
{
#if defined(_WIN32) || defined(_WIN64)
    return (uint32_t)GetCurrentThreadId();
#else
    pthread_t self = pthread_self();
    uint32_t  h    = (uint32_t)((uintptr_t)self >> 32);
    uint32_t  l    = (uint32_t)(uintptr_t)self;
    return (h ^ l ^ 0x9e3779b9u) + 1u;
#endif
}

/**
 * @brief Get monotonic time in milliseconds (for rate limiter / timeouts).
 *
 * Uses CLOCK_MONOTONIC on POSIX, QueryPerformanceCounter on Windows.
 * Not wall-clock; immune to NTP adjustments.
 */
static inline uint64_t clog_get_now_ms(void)
{
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

/* ── Console VT mode (ANSI escape support) ── */

/** @brief Enable Windows VT100 escape processing for ANSI color output. No-op on POSIX. */
static inline void clog_console_enable_vt_mode(FILE *stream)
{
    (void)stream;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE hOut = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif
}

/* ── Dynamic Library Abstractions (dlopen / LoadLibrary) ── */

#if defined(_WIN32) || defined(_WIN64)
typedef HMODULE                clog_dl_handle_t;
static inline clog_dl_handle_t clog_dlopen(const char *filename)
{
    return LoadLibraryA(filename);
}
static inline void *clog_dlsym(clog_dl_handle_t handle, const char *symbol)
{
    return (void *)GetProcAddress(handle, symbol);
}
static inline int clog_dlclose(clog_dl_handle_t handle)
{
    return FreeLibrary(handle) ? 0 : -1;
}
static inline const char *clog_dlerror(void)
{
    return "Win32 LoadLibrary error";
}
#else
#include <dlfcn.h>
typedef void                  *clog_dl_handle_t;
static inline clog_dl_handle_t clog_dlopen(const char *filename)
{
    return dlopen(filename, RTLD_NOW | RTLD_LOCAL);
}
static inline void *clog_dlsym(clog_dl_handle_t handle, const char *symbol)
{
    return dlsym(handle, symbol);
}
static inline int clog_dlclose(clog_dl_handle_t handle)
{
    return dlclose(handle);
}
static inline const char *clog_dlerror(void)
{
    return dlerror();
}
#endif

#endif /* CLOG_PORT_H */
