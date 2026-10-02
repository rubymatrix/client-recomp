/* Retail images in the guest window (R3): mapped by our own loader. FFXiMain.dll goes at its
 * preferred base 0x10000000 (RD = 0); a second module (FFXi.dll, which prefers the same base)
 * is mapped elsewhere and relocated - its translation reads its own delta (recomp.py --module). */
#pragma once

#include <stdint.h>

#include "runtime.h"

/* Maps FFXiMain, checks the build, decompresses the packed section into .text and points every import at a
 * thunk. Returns 1 on success; the reason is logged otherwise. */
int pe_load(const char* retail_path);
/* The same for a second module, at load_base, relocated; registers it with the runtime. */
int pe_load_module(const char* retail_path, const RtModule* m, uint32_t load_base);
/* An export of the mapped image at base (pe_export: FFXiMain's), or 0. */
uint32_t pe_export_at(uint32_t base, const char* name);
uint32_t pe_export(const char* name);
/* A resource of the image at base, by integer type and name (any language): its guest address
 * and size, or 0. */
uint32_t pe_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t* size);
/* FindResource's lookup: type and name as Win32 passes them (integer ids, "#n", or ANSI/UTF-16
 * strings). Returns the guest address of the IMAGE_RESOURCE_DATA_ENTRY (the HRSRC), or 0. */
uint32_t pe_find_resource(uint32_t base, uint32_t type, uint32_t name, int wide);
/* The image's static TLS (IMAGE_TLS_DIRECTORY): its template and zero fill, for
 * gt_set_static_tls. Sets the image's _tls_index to 0 and returns the guest address of its
 * NULL-terminated TLS callback array (0 if none, or no TLS directory). */
uint32_t pe_static_tls(uint32_t base, uint32_t* start, uint32_t* size, uint32_t* zero_fill);
