#pragma once

#include "guest.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

typedef struct RtEntry
{
    uint32_t addr;
    GuestFn fn;
} RtEntry;

/* Translated function at a guest address, or NULL. */
GuestFn rt_lookup(uint32_t addr);
/* Index of a translated function in rt_table, or -1. */
int rt_index(uint32_t addr);

/* Generated with the translation (table.c). */
extern const RtEntry rt_table[];
extern const unsigned rt_table_count;
extern const unsigned char rt_table_patch[];
extern const uint32_t rt_image_base, rt_image_timestamp, rt_image_size, rt_image_text_rva, rt_image_text_size,
    rt_image_packed_rva, rt_image_packed_len, rt_image_oep, rt_image_reloc_rva;

/* The image is at runtime_base: sets the relocation delta and the image range. */
void rt_set_image(uint32_t runtime_base);

/* A second translated module (recomp.py --module, e.g. FFXi.dll): its table and pinned build in
 * one descriptor, and the variable its translation reads as RD. */
typedef struct RtModule
{
    const char* name;
    const RtEntry* table;
    unsigned count;
    uint32_t* delta;
    uint32_t base, timestamp, size, text_rva, text_size, packed_rva, packed_len, oep, reloc_rva;
} RtModule;

/* The module is mapped at runtime_base: sets its delta; rt_call_indirect and rt_lookup_any then
 * resolve guest addresses inside it. */
void rt_add_module(const RtModule* m, uint32_t runtime_base);
/* The translation at a runtime guest address, in FFXiMain or any added module. */
GuestFn rt_lookup_any(uint32_t runtime_addr);

#include <stdio.h>

/* Diagnostics: everything goes to stderr and, once set, to a log file. The fatal hook runs just
 * before abort (the Windows host shows a message box there). */
void rt_log(const char* fmt, ...);
void rt_set_log(FILE* f);
typedef void (*RtFatalHook)(uint32_t addr, const char* what);
void rt_set_fatal_hook(RtFatalHook h);

/* Monotonic nanoseconds, from the platform layer (64-bit hosts: rdtsc is built on it). */
uint64_t rt_monotonic_ns(void);

/* The guest lock's yield (release, let a waiter in, reacquire), run at safepoints. */
void rt_set_yield(void (*yield)(void));

/* Called for indirect targets with no translation (imports, mostly). Returns nonzero if handled. */
typedef int (*RtNativeHandler)(Guest* g, uint32_t target);
void rt_set_native_handler(RtNativeHandler h);

/* recomp.py --wraps: a host wrapper runs at the wrapped function's entry, where esp holds the return
 * address and the arguments sit above it. rt_arg reads argument n (0-based); rt_return returns for
 * the function without running it (argbytes: what a stdcall callee pops, 0 for cdecl). To run the
 * original, call rt_orig_<name>(g) as is: the stack is still the caller's. */
static inline uint32_t rt_arg(Guest* g, unsigned n) { return rd32(g->esp + 4 + 4 * n); }
static inline void rt_return(Guest* g, uint32_t value, unsigned argbytes)
{
    g->eax = value;
    g->esp += 4 + argbytes;
}
