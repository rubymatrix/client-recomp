/* Guest threads, the guest lock, and calls from the host into guest code (R3).
 *
 * The portable counterpart of runtime/win32/bridge.c. There, guest stacks and TEBs were host
 * memory and the host reached guest code through patched entry stubs; here both live in the
 * guest window, and the host calls a translation directly (guest_call). The guest lock keeps the
 * policy measured on Windows: FIFO hand-off,
 * a ~2 ms quantum, released around calls that may block. On arm64 the hand-off is also what
 * gives the guest x86's memory ordering. */
#pragma once

#include "runtime.h"

#define GT_TLS_SLOTS 64u
#define GT_STACK_SIZE (4u << 20)

typedef struct GThread
{
    Guest g; /* first: a Guest* is a GThread* */
    uint32_t stack_lo, stack_hi, teb;
    uint32_t tid;
    uint32_t* tls;                   /* TlsGetValue's slots: tls_own, or the thread's a fiber runs for */
    uint32_t tls_own[GT_TLS_SLOTS];
} GThread;

/* Makes t this host thread's guest state (a fiber's: its own registers and stack, its thread's
 * TEB, TLS slots and id - fiber.c). */
void gt_adopt(GThread* t);

/* The process environment block every TEB points at (fs:[0x30]): 0 until gt_set_peb. */
void gt_set_peb(uint32_t peb);
/* Static TLS (__declspec(thread)) of an EXE image: its .tls template [start, start + size) plus
 * zero_fill bytes. Every guest thread created after this gets its own copy, reached through the
 * TEB's ThreadLocalStoragePointer (fs:[0x2C]) at slot 0 (the image's _tls_index, set to 0). */
void gt_set_static_tls(uint32_t start, uint32_t size, uint32_t zero_fill);

/* This thread's guest state, created on first use (the caller holds the guest lock). */
GThread* gt_self(void);
/* Frees this thread's guest state; its host thread is about to end. */
void gt_exit_self(void);

/* The guest lock. gt_lock/gt_unlock bracket a shim's blocking section (Sleep, waits, I/O). */
void gt_init(void);
void gt_lock(void);
void gt_unlock(void);
/* parallel guest execution (gthread.c): guest code runs unlocked, shims lock */
extern int gt_parallel;
int gt_holds(void);
/* for shims that may run with or without the lock: let go of it if held (returns whether), and back */
static inline int gt_release(void) { int h = gt_holds(); if (h) gt_unlock(); return h; }
static inline void gt_reacquire(int held) { if (held) gt_lock(); }
/* how long this thread has waited for the guest lock, in all */
uint64_t gt_wait_ns(void);
/* this thread keeps the guest lock as long as the game thread does (the render thread) */
void gt_set_long_quantum(void);
/* how long this thread has been without the lock (blocked, sleeping or waiting for it), in all */
uint64_t gt_out_ns(void);
/* The guest lock passed between host threads as is (fibers): the holder calls gt_handoff_out and
 * stops touching guest state; the thread it wakes calls gt_handoff_in and holds the lock. */
void gt_handoff_out(void);
void gt_handoff_in(void);
/* logs each thread's share of the last window_ns holding the guest lock, and its acquisitions */
void gt_lock_report(uint64_t window_ns);
int gt_holds(void);
/* Nesting: while on, this thread keeps the lock at safepoints (translated loops don't hand it to
 * another guest thread). For host code that must not be re-entered from another thread while it
 * calls into the guest (the addon host's Lua states). Blocking shims still release the lock. */
void gt_noyield(int on);

/* Win32 last-error of the current guest thread (the TEB's LastErrorValue, fs:[0x34]). */
void gt_set_error(uint32_t e);
uint32_t gt_get_error(void);

/* Calls a guest function pointer (translated code, or a thunk to a shim) with 32-bit arguments (pushed right to left,
 * so args[0] is the first parameter), stdcall or cdecl alike; returns eax. Takes the guest lock
 * if this thread does not hold it. */
uint32_t guest_call(uint32_t fn, unsigned nargs, const uint32_t* args);
/* The same for a C++ method (thiscall): self in ecx. */
uint32_t guest_thiscall(uint32_t fn, uint32_t self, unsigned nargs, const uint32_t* args);
/* The general form: regs 0 (cdecl, stdcall), 1 (thiscall: ecx), 2 (fastcall: ecx, edx); what the
 * callee left in edx (64-bit results) and at the top of the x87 stack (float results) comes back
 * too when asked for. */
uint32_t guest_call_full(uint32_t fn, int regs, uint32_t ecx, uint32_t edx, unsigned nargs, const uint32_t* args,
    uint32_t* edx_out, double* st0_out);
