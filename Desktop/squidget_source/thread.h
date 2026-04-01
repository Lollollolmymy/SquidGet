#pragma once

#ifndef _WIN32
#  define _DEFAULT_SOURCE   /* exposes usleep, strdup, etc. on glibc */
#endif

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
#  define sqt_thread_join(t)    do { WaitForSingleObject(t, INFINITE); CloseHandle(t); } while(0)
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
#  define sqt_sleep_ms(ms)              usleep((unsigned)((ms) * 1000))
#endif
