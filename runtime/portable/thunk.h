/* Imports on 64-bit hosts (R3): every Win32 function the game can reach - through its import
 * table or GetProcAddress - gets a synthetic guest address in a range that is never mapped. The
 * game calls it like any function pointer; rt_call_indirect finds no translation there and hands
 * it to thunk_dispatch, which runs the shim registered for that name. A shim knows its
 * signature: it reads its arguments off the guest stack and pops them itself (stdcall).
 *
 * An import with no shim still gets an address, and traps with its name on first call - the list
 * of shims to write grows from real runs, as it did for the R3.0 boundary trace. */
#pragma once

#include "runtime.h"

#define THUNK_BASE 0xFFF00000u
#define THUNK_STRIDE 16u
#define THUNK_MAX ((0xFFFF0000u - THUNK_BASE) / THUNK_STRIDE)

typedef void (*Shim)(Guest* g);

typedef struct ShimDef
{
    const char* dll; /* NULL: any DLL */
    const char* name; /* "#n" for an import by ordinal */
    Shim fn;
} ShimDef;

/* Adds a NULL-terminated table of shims (k32.c and friends call this from their init). */
void thunk_register(const ShimDef* defs);
/* Adds a table that wins over every table registered before it (ui_appkit.m takes over the
 * window calls for its own windows and hands the rest to the shims below it). */
void thunk_override(const ShimDef* defs);
/* The shim registered for dll!name, or NULL: lets one shim forward to another (the W calls of
 * k32_xiv.c convert their strings and run the A shim). */
Shim thunk_shim(const char* dll, const char* name);
/* The guest address for dll!name, created on first request. */
uint32_t thunk_for(const char* dll, const char* name);
/* rt_call_indirect's native handler. */
int thunk_dispatch(Guest* g, uint32_t target);
/* "DLL!name" for a thunk address, or NULL. */
const char* thunk_name(uint32_t addr);
/* Profiling: called with the time of every outermost shim call (nested ones - a window procedure
 * calling back into shims - are inside it), on the calling thread. NULL: not timed. */
extern void (*thunk_timer)(uint64_t ns);
/* ... and per shim, for this thread (the game's: host64 sets it at Present); thunk_prof_report
 * logs the top shims by time since the last report. */
extern uint32_t thunk_prof_thread;
void thunk_prof_report(void);
/* Logs every bound import that has no shim yet; returns how many. */
unsigned thunk_report_missing(void);

/* Writing shims. Arguments are counted from 0 (the first parameter). */
#define ARG(n) rd32(g->esp + 4u + 4u * (n))
#define ARGP(n) ((void*)GUEST_PTR(ARG(n)))
#define ARGS(n) ((const char*)GUEST_PTR(ARG(n)))
/* stdcall return: pops the return address and nargs arguments */
#define RET(v, nargs) do { g->eax = (uint32_t)(v); g->esp += 4u + 4u * (nargs); return; } while (0)
/* cdecl return (varargs APIs such as wsprintfA): the caller pops the arguments */
#define RETC(v) do { g->eax = (uint32_t)(v); g->esp += 4u; return; } while (0)
