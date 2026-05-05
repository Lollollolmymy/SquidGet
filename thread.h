#pragma once
#ifndef _WIN32
#  define _DEFAULT_SOURCE   /* exposes usleep, strdup, etc. on glibc */
#  include <time.h>         /* nanosleep */
#endif
#include <stdint.h>

#ifdef _WIN32
#  include <windows.h>

typedef HANDLE           sqt_thread_t;
typedef CRITICAL_SECTION sqt_mutex_t;
#  define SQT_THREAD_FN  DWORD WINAPI

static inline int sqt_thread_create(sqt_thread_t *t,
                                     LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t ? 0 : -1;
}
static inline void sqt_thread_join_impl(sqt_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#  define sqt_thread_join(t)    sqt_thread_join_impl(t)
#  define sqt_mutex_init(m)     InitializeCriticalSection(m)
#  define sqt_mutex_destroy(m)  DeleteCriticalSection(m)
#  define sqt_mutex_lock(m)     EnterCriticalSection(m)
#  define sqt_mutex_unlock(m)   LeaveCriticalSection(m)
#  define sqt_sleep_ms(ms)      Sleep(ms)

#else
#  include <pthread.h>
#  include <unistd.h>

typedef pthread_t        sqt_thread_t;
typedef pthread_mutex_t  sqt_mutex_t;
#  define SQT_THREAD_FN  void *

#  define sqt_thread_create(t, fn, arg) pthread_create(t, NULL, fn, arg)
#  define sqt_thread_join(t)            pthread_join(t, NULL)
#  define sqt_mutex_init(m)             pthread_mutex_init(m, NULL)
#  define sqt_mutex_destroy(m)          pthread_mutex_destroy(m)
#  define sqt_mutex_lock(m)             pthread_mutex_lock(m)
#  define sqt_mutex_unlock(m)           pthread_mutex_unlock(m)
/* nanosleep; usleep can't handle > 1000ms */
static inline void sqt_sleep_ms_impl(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}
#  define sqt_sleep_ms(ms)              sqt_sleep_ms_impl(ms)
#endif

static inline uint64_t sqt_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}
