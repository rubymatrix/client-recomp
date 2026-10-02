/* The guest window (R3): all guest memory - the game image, heaps, stacks, TEBs, strings the
 * runtime hands the guest - lives in one reserved 4 GB range of host address space at
 * rt_guest_base, addressed by 32-bit guest addresses (guest.h, RT_GUEST_WINDOW).
 *
 * Pages: 64 KB allocation granules (as Win32's VirtualAlloc), committed 4 KB at a time.
 * Heap: a size-class allocator on top, for HeapAlloc and for the runtime's own guest objects.
 * The first 1 MB (null pointer faults) and the thunk range (thunk.h) are never handed out. */
#pragma once

#include <stdint.h>

int gwin_init(void);

/* Pages. `addr` 0 = anywhere. Reserving marks granules used without committing them. */
uint32_t gwin_reserve(uint32_t addr, uint32_t size);
int gwin_commit(uint32_t addr, uint32_t size);
void gwin_decommit(uint32_t addr, uint32_t size);
void gwin_release(uint32_t addr); /* a whole reservation, by its start */
uint32_t gwin_alloc(uint32_t size); /* reserve + commit anywhere */
int gwin_is_committed(uint32_t addr);

/* Heap. Every heap handle shares one allocator; the handles only keep identities apart.
 * Callers hold the guest lock (the runtime's own startup code runs before any other thread). */
uint32_t gheap_alloc(uint32_t size, int zero);
void gheap_free(uint32_t p);
uint32_t gheap_realloc(uint32_t p, uint32_t size, int zero, int in_place_only);
uint32_t gheap_size(uint32_t p); /* ~0u if p is not a live block */
uint32_t gheap_strdup(const char* s);
