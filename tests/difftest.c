/* Differential test: original FFXiMain code vs its translation, same inputs, same process.
 *
 * The unpacked image (generated/FFXiMain.unpacked.dll) is mapped at its preferred base
 * 0x10000000 so the original functions can be called directly; imports are left unresolved, so
 * only functions that make no import calls can be tested this way. The translation runs on the
 * same memory (GUEST_BASE is 0 on x86), with its own guest stack and TEB.
 *
 * For every call it checks: return value(s), output buffers, callee-saved registers
 * (ebx/esi/edi/ebp), the guest stack pointer after return (caller- vs callee-cleanup), and for
 * x87 functions the FPU stack depth.
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "funcs.h"
#include "runtime.h"

static Guest G;
static int g_fail = 0;
static int g_checks = 0;

/* --- image --------------------------------------------------------------------------------- */

static int map_image(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        printf("cannot open %s (run tools/prepare.py)\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* file = (unsigned char*)malloc(size);
    fread(file, 1, size, f);
    fclose(f);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)file;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(file + dos->e_lfanew);
    unsigned char* base = (unsigned char*)VirtualAlloc((void*)(uintptr_t)nt->OptionalHeader.ImageBase,
        nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (unsigned char*)(uintptr_t)nt->OptionalHeader.ImageBase)
    {
        printf("could not map the image at %08lx\n", nt->OptionalHeader.ImageBase);
        return 0;
    }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (s[i].SizeOfRawData)
            memcpy(base + s[i].VirtualAddress, file + s[i].PointerToRawData,
                min(s[i].SizeOfRawData, s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData));
    free(file);
    return 1;
}

/* --- guest calls ----------------------------------------------------------------------------- */

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static uint64_t rnd64(void) { return ((uint64_t)rnd() << 32) | rnd(); }

static void fail(const char* what, const char* fmt, ...)
{
    va_list ap;
    if (g_fail < 25)
    {
        printf("  MISMATCH %s: ", what);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
    g_fail++;
}

/* Call a translated function with stack arguments; checks esp and callee-saved registers. */
static void gcall(const char* what, GuestFn fn, const uint32_t* args, int n, int callee_pops)
{
    uint32_t base_sp = G.esp;
    for (int i = n - 1; i >= 0; --i)
    {
        G.esp -= 4;
        wr32(G.esp, args[i]);
    }
    G.esp -= 4;
    wr32(G.esp, 0xDEADBEEFu);
    uint32_t ebx = G.ebx = rnd(), esi = G.esi = rnd(), edi = G.edi = rnd(), ebp = G.ebp = rnd();
    uint32_t expect = G.esp + 4 + (callee_pops ? 4u * n : 0u);
    fn(&G);
    g_checks++;
    if (G.esp != expect)
        fail(what, "esp after return %08x, expected %08x", G.esp, expect);
    if (G.ebx != ebx || G.esi != esi || G.edi != edi || G.ebp != ebp)
        fail(what, "callee-saved register clobbered");
    G.esp = base_sp;
}

/* --- originals ------------------------------------------------------------------------------- */

typedef size_t(__cdecl* strlen_t)(const char*);
typedef char*(__cdecl* strrchr_t)(const char*, int);
typedef int(__cdecl* strncmp_t)(const char*, const char*, size_t);
typedef char*(__cdecl* strncpy_t)(char*, const char*, size_t);
typedef int(__cdecl* memcmp_t)(const void*, const void*, size_t);
typedef void*(__cdecl* memset_t)(void*, int, size_t);
typedef void*(__cdecl* memcpy_t)(void*, const void*, size_t);

#define O_STRLEN ((strlen_t)CRT_STRLEN)
#define O_STRRCHR ((strrchr_t)CRT_STRRCHR)
#define O_STRNCMP ((strncmp_t)CRT_STRNCMP)
#define O_STRNCPY ((strncpy_t)CRT_STRNCPY)
#define O_MEMCMP ((memcmp_t)CRT_MEMCMP)
#define O_MEMSET ((memset_t)CRT_MEMSET)
#define O_MEMCPY ((memcpy_t)CRT_MEMCPY)
typedef char*(__cdecl* strpbrk_t)(const char*, const char*);
#define O_STRPBRK ((strpbrk_t)CRT_STRPBRK) /* bts/bt on a 256-bit set on the stack */

/* 64-bit helpers: two 64-bit stack arguments, callee pops, result in edx:eax. */
static uint64_t o_helper2(uint32_t fn, uint64_t a, uint64_t b)
{
    uint32_t alo = (uint32_t)a, ahi = (uint32_t)(a >> 32), blo = (uint32_t)b, bhi = (uint32_t)(b >> 32), lo, hi;
    __asm {
        push bhi
        push blo
        push ahi
        push alo
        call fn
        mov lo, eax
        mov hi, edx
    }
    return ((uint64_t)hi << 32) | lo;
}

/* shift helpers: value in edx:eax, count in cl. */
static uint64_t o_shift(uint32_t fn, uint64_t v, uint32_t n)
{
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    __asm {
        mov eax, lo
        mov edx, hi
        mov ecx, n
        call fn
        mov lo, eax
        mov hi, edx
    }
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t o_ftol(double x)
{
    uint32_t fn = CRT_FTOL, lo, hi;
    __asm {
        fld x
        call fn
        mov lo, eax
        mov hi, edx
    }
    return ((uint64_t)hi << 32) | lo;
}

/* --- tests ----------------------------------------------------------------------------------- */

static char A[4096], B[4096], S1[1024], S2[1024];

static void fill_string(char* s, int len, int alphabet)
{
    for (int i = 0; i < len; ++i)
        s[i] = (char)('a' + rnd() % alphabet);
    s[len] = 0;
}

static void test_strings(void)
{
    for (int it = 0; it < 4000; ++it)
    {
        int off = rnd() % 8, len = rnd() % 300;
        char* s = S1 + off;
        fill_string(s, len, 4);
        uint32_t args[3] = { (uint32_t)(uintptr_t)s };

        size_t o = O_STRLEN(s);
        gcall("strlen", F_STRLEN, args, 1, 0);
        if (G.eax != o) fail("strlen", "len %d off %d: %u vs %u", len, off, (unsigned)o, G.eax);

        int c = 'a' + rnd() % 5;
        args[1] = (uint32_t)c;
        char* r = O_STRRCHR(s, c);
        gcall("strrchr", F_STRRCHR, args, 2, 0);
        if (G.eax != (uint32_t)(uintptr_t)r) fail("strrchr", "%p vs %08x", r, G.eax);

        char* t = S2 + rnd() % 8;
        memcpy(t, s, len + 1);
        if (len && rnd() % 2) t[rnd() % len] = (char)('a' + rnd() % 4);
        size_t n = rnd() % 320;
        args[1] = (uint32_t)(uintptr_t)t;
        args[2] = (uint32_t)n;
        int oc = O_STRNCMP(s, t, n);
        gcall("strncmp", F_STRNCMP, args, 3, 0);
        if ((int)G.eax != oc) fail("strncmp", "%d vs %d", oc, (int)G.eax);
        {
            /* strpbrk over the full byte range, so bit offsets reach far past the 32-bit operand */
            static char set[64];
            int sl = rnd() % 12;
            for (int i = 0; i < sl; ++i)
                set[i] = (char)(1 + rnd() % 255);
            set[sl] = 0;
            char* hay = S2 + 512;
            int hl = rnd() % 200;
            for (int i = 0; i < hl; ++i)
                hay[i] = (char)(1 + rnd() % 255);
            hay[hl] = 0;
            char* pr = O_STRPBRK(hay, set);
            uint32_t pa[2] = { (uint32_t)(uintptr_t)hay, (uint32_t)(uintptr_t)set };
            gcall("strpbrk", F_STRPBRK, pa, 2, 0);
            if (G.eax != (uint32_t)(uintptr_t)pr) fail("strpbrk", "%p vs %08x", pr, G.eax);
        }
        int om = O_MEMCMP(s, t, n < (size_t)len ? n : (size_t)len);
        args[2] = (uint32_t)(n < (size_t)len ? n : (size_t)len);
        gcall("memcmp", F_MEMCMP, args, 3, 0);
        if ((int)G.eax != om) fail("memcmp", "%d vs %d", om, (int)G.eax);
    }
}

static void test_buffers(void)
{
    for (int it = 0; it < 4000; ++it)
    {
        for (int i = 0; i < (int)sizeof A; ++i)
            A[i] = B[i] = (char)rnd();
        int kind = rnd() % 3;
        int doff = rnd() % 64, n = rnd() % 700;
        if (kind == 0) /* memset */
        {
            int c = (int)(rnd() & 0x1FF);
            void* r = O_MEMSET(A + doff, c, n);
            uint32_t args[3] = { (uint32_t)(uintptr_t)(B + doff), (uint32_t)c, (uint32_t)n };
            gcall("memset", F_MEMSET, args, 3, 0);
            if (G.eax - (uint32_t)(uintptr_t)B != (uint32_t)((char*)r - A)) fail("memset", "return value");
        }
        else if (kind == 1) /* memcpy, overlapping both ways */
        {
            int soff = rnd() % 1400;
            void* r = O_MEMCPY(A + doff + 700, A + soff, n);
            uint32_t args[3] = { (uint32_t)(uintptr_t)(B + doff + 700), (uint32_t)(uintptr_t)(B + soff), (uint32_t)n };
            gcall("memcpy", F_MEMCPY, args, 3, 0);
            if (G.eax - (uint32_t)(uintptr_t)B != (uint32_t)((char*)r - A)) fail("memcpy", "return value");
        }
        else /* strncpy */
        {
            int len = rnd() % 300;
            fill_string(S1, len, 26);
            char* r = O_STRNCPY(A + doff, S1, n);
            uint32_t args[3] = { (uint32_t)(uintptr_t)(B + doff), (uint32_t)(uintptr_t)S1, (uint32_t)n };
            gcall("strncpy", F_STRNCPY, args, 3, 0);
            if (G.eax - (uint32_t)(uintptr_t)B != (uint32_t)(r - A)) fail("strncpy", "return value");
        }
        if (memcmp(A, B, sizeof A))
        {
            int i = 0;
            while (A[i] == B[i]) ++i;
            fail(kind == 0 ? "memset" : kind == 1 ? "memcpy" : "strncpy", "buffers differ at %d (n=%d)", i, n);
        }
    }
}

static void test_int64(void)
{
    static const struct { const char* name; uint32_t addr; GuestFn fn; int signed_div; } H[] = {
        { "__allmul", CRT_ALLMUL, F_ALLMUL, 0 },
        { "__alldiv", CRT_ALLDIV, F_ALLDIV, 1 },
        { "__aulldiv", CRT_AULLDIV, F_AULLDIV, 2 },
        { "__aullrem", CRT_AULLREM, F_AULLREM, 2 },
    };
    for (int h = 0; h < 4; ++h)
        for (int it = 0; it < 20000; ++it)
        {
            uint64_t a = rnd64(), b = rnd64();
            switch (rnd() % 4) /* vary magnitudes so every path (32-bit divisor, shifts) is hit */
            {
            case 0: b >>= 32; break;
            case 1: b >>= rnd() % 64; a >>= rnd() % 64; break;
            case 2: a >>= 32; break;
            }
            if (H[h].signed_div && (b == 0 || (a == 0x8000000000000000ull && b == ~0ull))) continue;
            if (b == 0) continue;
            uint64_t o = o_helper2(H[h].addr, a, b);
            uint32_t args[4] = { (uint32_t)a, (uint32_t)(a >> 32), (uint32_t)b, (uint32_t)(b >> 32) };
            gcall(H[h].name, H[h].fn, args, 4, 1);
            uint64_t t = ((uint64_t)G.edx << 32) | G.eax;
            if (o != t) fail(H[h].name, "%016llx, %016llx: %016llx vs %016llx", a, b, o, t);
        }
    static const struct { const char* name; uint32_t addr; GuestFn fn; } S[] = {
        { "__allshl", CRT_ALLSHL, F_ALLSHL },
        { "__aullshr", CRT_AULLSHR, F_AULLSHR },
    };
    for (int h = 0; h < 2; ++h)
        for (int it = 0; it < 5000; ++it)
        {
            uint64_t v = rnd64();
            uint32_t n = rnd() % 70;
            uint64_t o = o_shift(S[h].addr, v, n);
            G.eax = (uint32_t)v;
            G.edx = (uint32_t)(v >> 32);
            G.ecx = n;
            gcall(S[h].name, S[h].fn, NULL, 0, 0);
            uint64_t t = ((uint64_t)G.edx << 32) | G.eax;
            if (o != t) fail(S[h].name, "%016llx << %u: %016llx vs %016llx", v, n, o, t);
        }
}

static void test_ftol(void)
{
    for (int it = 0; it < 20000; ++it)
    {
        double x;
        switch (rnd() % 4)
        {
        case 0: x = (double)(int32_t)rnd() + (rnd() % 1000) / 1000.0; break;
        case 1: x = ((double)(int32_t)rnd()) * 1e6; break;
        case 2: x = (rnd() % 2001 - 1000) / 7.0; break;
        default: x = -((double)(rnd() % 100) + 0.5); break;
        }
        uint64_t o = o_ftol(x);
        uint32_t top = G.top;
        fpush(&G, x);
        gcall("__ftol", F_FTOL, NULL, 0, 0);
        uint64_t t = ((uint64_t)G.edx << 32) | G.eax;
        if (o != t) fail("__ftol", "%.17g: %016llx vs %016llx", x, o, t);
        if (G.top != top) fail("__ftol", "x87 stack not popped");
        if (G.fcw != 0x027F) fail("__ftol", "control word not restored: %04x", G.fcw);
    }
}

int main(int argc, char** argv)
{
    const char* image = argc > 1 ? argv[1] : "generated\\FFXiMain.unpacked.dll";
    if (!map_image(image))
        return 2;
    rt_set_image(0x10000000u); /* mapped at the preferred base: delta 0 */
    uint32_t stack = (uint32_t)(uintptr_t)VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t teb = (uint32_t)(uintptr_t)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(&G, 0, sizeof G);
    G.esp = stack + (1 << 20) - 256;
    G.fs_base = teb;
    wr32(teb, 0xFFFFFFFFu); /* empty SEH chain */
    G.fcw = 0x027F;         /* Windows thread default: 53-bit precision, round to nearest */

    struct { const char* name; void (*fn)(void); } T[] = {
        { "strlen/strrchr/strncmp/strpbrk/memcmp", test_strings },
        { "memset/memcpy/strncpy", test_buffers },
        { "64-bit helpers", test_int64 },
        { "__ftol", test_ftol },
    };
    for (int i = 0; i < (int)(sizeof T / sizeof T[0]); ++i)
    {
        int before = g_fail, checks = g_checks;
        T[i].fn();
        printf("%-32s %6d calls  %s\n", T[i].name, g_checks - checks, g_fail == before ? "ok" : "FAILED");
    }
    printf("%s: %d calls, %d mismatches\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
