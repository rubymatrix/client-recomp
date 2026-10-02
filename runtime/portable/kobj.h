/* Win32 kernel objects for the guest (R3): one handle table for everything the guest holds a
 * HANDLE to, and the waitable objects - events, mutexes, semaphores, threads - with Win32 wait
 * semantics.
 *
 * Object state is guarded by its own small lock, not the guest lock: host code signals objects
 * too (a socket's network event, a thread ending). A waiter releases the guest lock and sleeps
 * on a single epoch that every signal advances, then re-checks; the guest has a handful of
 * threads, so the broadcast costs nothing that matters. */
#pragma once

#include <stdint.h>

typedef enum KKind
{
    K_FREE,
    K_FILE,
    K_FIND,
    K_EVENT,
    K_MUTEX,
    K_SEMAPHORE,
    K_THREAD,
    K_REGKEY, /* an open registry key (reg.c) */
    K_OTHER,  /* module-specific (sockets ...): closed through its own callback */
} KKind;

typedef struct KObject KObject;

/* Handles are what the guest sees: multiples of 4 from 0x100, as Windows' are. */
uint32_t k_new(KKind kind, void* data, void (*close)(void* data));
void* k_data(uint32_t h, KKind kind); /* NULL if h is not a handle of that kind */
int k_close(uint32_t h);
uint32_t k_duplicate(uint32_t h);

/* Waitables. */
uint32_t k_event(int manual, int initial);
int k_event_set(uint32_t h, int state); /* 1 set, 0 reset */
uint32_t k_mutex(int owned);
int k_mutex_release(uint32_t h);
uint32_t k_semaphore(int32_t initial, int32_t max);
int k_semaphore_release(uint32_t h, int32_t n, int32_t* previous);

/* Win32 waits: returns WAIT_OBJECT_0 + i, WAIT_ABANDONED_0 + i, WAIT_TIMEOUT or WAIT_FAILED.
 * The caller holds the guest lock; it is released while blocked. */
#define K_WAIT_OBJECT_0 0x00000000u
#define K_WAIT_ABANDONED_0 0x00000080u
#define K_WAIT_TIMEOUT 0x00000102u
#define K_WAIT_FAILED 0xFFFFFFFFu
uint32_t k_wait(const uint32_t* handles, uint32_t n, int all, uint32_t ms);

/* Threads. */
uint32_t k_thread_create(uint32_t start, uint32_t param, int suspended, uint32_t* tid);
int k_thread_resume(uint32_t h, uint32_t* previous);
int k_thread_exit_code(uint32_t h, uint32_t* code);
/* The current guest thread ends with this code (ExitThread). Does not return. */
void k_thread_exit(uint32_t code);

/* Something external changed: every waiter re-checks (used by host-side signallers). */
void k_poke(void);
