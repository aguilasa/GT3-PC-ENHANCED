/* pgl_win32.h - the Win32 calls the renderer makes (its threads, events,
 * locks, address waits, clock and a few file calls), by their Windows names.
 *
 * Windows: <windows.h>, nothing else.
 *
 * Elsewhere: DXVK-native's <windows.h> (the types d3d11.h is declared
 * with: DWORD, HANDLE, LARGE_INTEGER...), the platform layer's
 * platform/os/win_compat.h (Sleep, GetTickCount64...) and these calls on POSIX
 * (pgl_win32_posix.cpp), with the Windows semantics the renderer relies on:
 *   - QueryPerformanceCounter: CLOCK_MONOTONIC in ns (frequency 1e9);
 *   - SRWLOCK / CONDITION_VARIABLE: a pthread mutex (shared acquisition is
 *     exclusive: the renderer never nests it) and a pthread condition;
 *   - WaitOnAddress / WakeByAddress*: a futex on the aligned 32-bit word
 *     (an 8-byte value is compared whole, then waited on by its low word: a
 *     change of the high word alone wakes no one, the wait ends at its
 *     timeout);
 *   - CreateThread / WaitForSingleObject / CloseHandle, CreateEventW /
 *     SetEvent: a handle of this file's own (a pthread, or an auto / manual
 *     reset event); the new thread starts in the default FPU mode and with
 *     its own signal stack, as a Windows thread (crash::prepare_thread);
 *   - SetThreadPriority: below normal lowers the thread's nice value; above
 *     normal is left as it is (raising needs privileges);
 *   - the file calls go through platform/os/fs.h ('\' and '/' both
 *     separate; GetModuleFileNameA gives the system's '/' path).
 * PGL_PATH_SEP is the separator the renderer's own dump paths are built with. */
#ifndef PGL_NATIVE_PGL_WIN32_H
#define PGL_NATIVE_PGL_WIN32_H

#ifdef _WIN32

#include <windows.h>

#define PGL_PATH_SEP "\\"

#else /* !_WIN32 */

#include <windows.h>        /* (DXVK-native's) */
#include "platform/os/win_compat.h"   /* (Sleep, GetTickCount64, the key codes: the platform layer's) */

#include <pthread.h>
#include <stdint.h>

#define PGL_PATH_SEP "/"

typedef uint64_t ULONG64;
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15

#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_RELEASE 0x00008000
#define PAGE_READWRITE 0x04

#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL 0x00000080

/* ---- the clock ---- */
BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq);
/* The calling thread's CPU time in TSC cycles (only GetCurrentThread()). */
BOOL QueryThreadCycleTime(HANDLE thread, ULONG64 *cycles);

/* ---- locks ---- */
typedef struct {
    pthread_mutex_t m;
} SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}

typedef struct {
    pthread_cond_t c;
} CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {PTHREAD_COND_INITIALIZER}

inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(&l->m); }
inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(&l->m); }
inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_mutex_lock(&l->m); }
inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_mutex_unlock(&l->m); }
inline void WakeConditionVariable(CONDITION_VARIABLE *cv) { pthread_cond_signal(&cv->c); }
inline void WakeAllConditionVariable(CONDITION_VARIABLE *cv) { pthread_cond_broadcast(&cv->c); }
/* false at the timeout */
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *lock, DWORD ms, ULONG flags);

/* ---- address waits ---- */
BOOL WaitOnAddress(volatile void *address, void *compare, SIZE_T size, DWORD ms);
void WakeByAddressSingle(void *address);
void WakeByAddressAll(void *address);

/* ---- threads and events ---- */
HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID arg, DWORD flags,
                    DWORD *id);
HANDLE CreateEventW(void *security, BOOL manualReset, BOOL initialState, LPCWSTR name);
BOOL SetEvent(HANDLE event);
/* A thread: its end; an event: set (an auto-reset one is reset).  0
 * (WAIT_OBJECT_0) or WAIT_TIMEOUT. */
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
/* The thread's name (UTF-16; the first 15 bytes of its UTF-8 for the system). */
HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name);
BOOL SwitchToThread(void);

typedef struct {
    DWORD dwNumberOfProcessors;
} SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO *si);

/* ---- memory, the stack ---- */
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type);
USHORT CaptureStackBackTrace(ULONG skip, ULONG count, void **frames, ULONG *hash);

/* ---- files (UTF-8 paths) ---- */
DWORD GetModuleFileNameA(HMODULE module, char *path, DWORD size);
BOOL CreateDirectoryA(const char *path, void *security);
DWORD GetFileAttributesA(const char *path);

#endif /* _WIN32 */

#endif /* PGL_NATIVE_PGL_WIN32_H */
