#pragma once

#include "runtime.h"

/* This thread's guest state (created on first use: guest stack, guest TEB). */
Guest* bridge_guest(void);
/* Native handler for rt_call_indirect: calls a host function on behalf of the guest. */
int bridge_native(Guest* g, uint32_t target);
/* Common tail of every entry stub: `push guest_addr ; jmp enter_asm`. */
void enter_asm(void);
/* Installs the guest lock's safepoint yield. */
void bridge_init(void);
/* The retail FFXiMain.dll (path and mapped module): the game's own-file accesses go there. */
void bridge_set_retail(const char* path, HMODULE mapped);

/* Boundary trace: open with a path prefix (<base>.trace.txt, <base>.seq.txt); the loader names
 * each import it resolves; dumped every 30 s and on demand (process detach, fatal). */
void bridge_trace_open(const char* base);
void bridge_note_import(uint32_t addr, const char* dll, const char* name);
void bridge_trace_dump(void);
/* Nonzero while the trace probes a guest pointer (its faults are expected and handled). */
int bridge_probing(void);

typedef struct BridgeOverride
{
    const char* name;
    void* fn;
} BridgeOverride;

/* Imports replaced by host functions, by name (any DLL). NULL-terminated. */
extern const BridgeOverride bridge_overrides[];
