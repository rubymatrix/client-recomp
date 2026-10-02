/* R2 loader: bring the retail FFXiMain.dll up as a real module whose code is our translation.
 *
 *  1. LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) maps the retail DLL as a genuine module
 *     (GetModuleFileName, FindResource and the module handle all work) without running its unpacking
 *     stub or resolving its imports. Inside the retail launcher the viewer's DLLs hold 0x10000000, so Windows
 *     relocates it (retail FFXiMain always runs relocated there); the translation follows via RD.
 *  2. The build is checked against the one the translation was made from.
 *  3. The packed section is decompressed into .text and the private .text relocations applied, as the stub would
 *     have done, so data the code reads from .text (jump tables, constants) is present and correct.
 *     The original instructions never run.
 *  4. Imports are resolved, with bridge_overrides taking precedence.
 *  5. Every translated function's entry is patched to reach its translation: a 5-byte jmp to an
 *     entry stub, or an int3 (caught by a vectored handler) where 5 bytes do not fit.
 */
#include <windows.h>

#include <stdio.h>

#include "bridge.h"
#include "loader.h"

static unsigned char* g_stubs;
static uint32_t g_base;

#define STUB_SIZE 10u

static void lzss(const unsigned char* src, uint32_t src_len, unsigned char* dst, uint32_t dst_len, uint32_t* out_len)
{
    /* Flag byte per 8 items, MSB first: 1 = literal, 0 = b0 b1 with offset ((b0<<8)|b1)&0xfff and
     * length (b0>>4)+3; offset 0 ends the stream. (tools/unpack.py) */
    uint32_t i = 0, o = 0;
    while (i < src_len)
    {
        unsigned flags = src[i++];
        for (int k = 0; k < 8; ++k, flags <<= 1)
        {
            if (flags & 0x80)
            {
                if (o >= dst_len) goto done;
                dst[o++] = src[i++];
            }
            else
            {
                unsigned b0 = src[i], b1 = src[i + 1];
                unsigned off = ((b0 << 8) | b1) & 0xFFF;
                if (!off) goto done;
                i += 2;
                unsigned len = (b0 >> 4) + 3;
                for (unsigned c = 0; c < len && o < dst_len; ++c, ++o)
                    dst[o] = dst[o - off];
            }
        }
    }
done:
    *out_len = o;
}

static int resolve_imports(unsigned char* base)
{
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d)
    {
        const char* dll = (const char*)(base + d->Name);
        HMODULE h = LoadLibraryA(dll);
        if (!h)
        {
            rt_log("[recomp] cannot load %s\n", dll);
            return 0;
        }
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat)
        {
            FARPROC fn = NULL;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
                fn = GetProcAddress(h, (LPCSTR)(uintptr_t)IMAGE_ORDINAL32(names->u1.Ordinal));
            else
            {
                const char* name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
                for (const BridgeOverride* o = bridge_overrides; o->name; ++o)
                    if (!strcmp(o->name, name))
                        fn = (FARPROC)o->fn;
                if (!fn)
                    fn = GetProcAddress(h, name);
                if (!fn)
                {
                    rt_log("[recomp] %s!%s not found\n", dll, name);
                    return 0;
                }
            }
            iat->u1.Function = (DWORD)(uintptr_t)fn;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
            {
                char ord[16];
                sprintf_s(ord, sizeof ord, "#%u", (unsigned)IMAGE_ORDINAL32(names->u1.Ordinal));
                bridge_note_import((uint32_t)(uintptr_t)fn, dll, ord);
            }
            else
                bridge_note_import((uint32_t)(uintptr_t)fn, dll,
                    (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name);
        }
    }
    return 1;
}

/* The stub's private .text relocation table at the start of .reloc: ordinary base-relocation blocks,
 * ending at the first block past .text or of size 0. (The PE directory covers .rdata/.data only,
 * and Windows has already applied those.) */
static unsigned apply_text_relocations(unsigned char* base, uint32_t delta)
{
    uint32_t text_end = rt_image_text_rva + rt_image_text_size;
    unsigned char* p = base + rt_image_reloc_rva;
    unsigned n = 0;
    for (;;)
    {
        uint32_t page, size;
        memcpy(&page, p, 4);
        memcpy(&size, p + 4, 4);
        if (size == 0 || page >= text_end)
            break;
        for (uint32_t k = 0; k < (size - 8) / 2; ++k)
        {
            uint16_t e;
            memcpy(&e, p + 8 + 2 * k, 2);
            if ((e >> 12) == IMAGE_REL_BASED_HIGHLOW)
            {
                uint32_t* at = (uint32_t*)(base + page + (e & 0xFFF));
                *at += delta;
                n++;
            }
        }
        p += size;
    }
    return n;
}

static LONG CALLBACK int3_entry(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress - rt_reloc_delta;
    int i = rt_index(at);
    if (i < 0 || !rt_table_patch[i])
        return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Eip = (DWORD)(uintptr_t)(g_stubs + i * STUB_SIZE);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void patch_entries(void)
{
    g_stubs = (unsigned char*)VirtualAlloc(NULL, rt_table_count * STUB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    unsigned jmps = 0, int3s = 0;
    for (unsigned i = 0; i < rt_table_count; ++i)
    {
        unsigned char* s = g_stubs + i * STUB_SIZE;
        uint32_t a = rt_table[i].addr;
        s[0] = 0x68; /* push guest address */
        memcpy(s + 1, &a, 4);
        s[5] = 0xE9; /* jmp enter_asm */
        int32_t rel = (int32_t)((uintptr_t)enter_asm - (uintptr_t)(s + 10));
        memcpy(s + 6, &rel, 4);

        unsigned char* p = GUEST_PTR(a + rt_reloc_delta); /* stubs push the static address */
        if (rt_table_patch[i])
        {
            p[0] = 0xCC;
            int3s++;
        }
        else
        {
            p[0] = 0xE9;
            rel = (int32_t)((uintptr_t)s - (uintptr_t)(p + 5));
            memcpy(p + 1, &rel, 4);
            jmps++;
        }
    }
    AddVectoredExceptionHandler(1, int3_entry);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    rt_log("[recomp] patched %u entries (%u jmp, %u int3)\n", jmps + int3s, jmps, int3s);
}

HMODULE rt_load_image(const char* retail_path)
{
    HMODULE h = LoadLibraryExA(retail_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!h)
    {
        rt_log("[recomp] cannot map %s (error %lu)\n", retail_path, GetLastError());
        return NULL;
    }
    unsigned char* base = (unsigned char*)h;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != rt_image_timestamp || nt->OptionalHeader.SizeOfImage != rt_image_size)
    {
        rt_log("[recomp] %s is not the build this translation was made from\n", retail_path);
        return NULL;
    }
    g_base = (uint32_t)(uintptr_t)h;
    DWORD old;
    VirtualProtect(base, rt_image_size, PAGE_EXECUTE_READWRITE, &old);

    uint32_t got = 0;
    lzss(base + rt_image_packed_rva, rt_image_packed_len, base + rt_image_text_rva, rt_image_text_size, &got);
    if (got != rt_image_text_size)
    {
        rt_log("[recomp] packed section decompressed %u bytes, expected %u\n", got, rt_image_text_size);
        return NULL;
    }
    rt_set_image(g_base);
    unsigned relocs = rt_reloc_delta ? apply_text_relocations(base, rt_reloc_delta) : 0;
    rt_log("[recomp] image at %08x (delta %08x), %u .text relocations applied\n", g_base, rt_reloc_delta, relocs);
    if (!resolve_imports(base))
        return NULL;
    rt_set_native_handler(bridge_native);
    bridge_init();
    {
        char full[MAX_PATH];
        GetFullPathNameA(retail_path, MAX_PATH, full, NULL);
        bridge_set_retail(full, h);
    }
    patch_entries();
    return h;
}
