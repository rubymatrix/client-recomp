/* The game image loader for 64-bit hosts. See pe.h. PE32 is parsed by offset, without windows.h,
 * so this file builds on macOS unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gwin.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"

static uint32_t rd32b(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16b(const unsigned char* p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* The packed section: a flag byte per 8 items, MSB first; 1 = literal byte, 0 = b0 b1 with offset
 * ((b0<<8)|b1)&0xfff and length (b0>>4)+3; offset 0 ends the stream. (tools/unpack.py;
 * the same decoder as runtime/win32/loader.c.) */
static uint32_t unpack_text(const unsigned char* src, uint32_t src_len, unsigned char* dst, uint32_t dst_len)
{
    uint32_t i = 0, o = 0;
    while (i < src_len)
    {
        unsigned flags = src[i++];
        for (int k = 0; k < 8; ++k, flags <<= 1)
        {
            if (flags & 0x80)
            {
                if (o >= dst_len || i >= src_len)
                    return o;
                dst[o++] = src[i++];
            }
            else
            {
                if (i + 1 >= src_len)
                    return o;
                unsigned b0 = src[i], b1 = src[i + 1];
                unsigned off = ((b0 << 8) | b1) & 0xFFF;
                if (!off)
                    return o;
                i += 2;
                unsigned len = (b0 >> 4) + 3;
                for (unsigned c = 0; c < len && o < dst_len; ++c, ++o)
                    dst[o] = o >= off ? dst[o - off] : 0;
            }
        }
    }
    return o;
}

/* What mapping one pinned build needs: FFXiMain's rt_image_* constants, or an RtModule. */
typedef struct Build
{
    const char* name;
    uint32_t base, timestamp, size, text_rva, text_size, packed_rva, packed_len, reloc_rva;
} Build;

/* Applies base-relocation blocks from `at` until a block of size 0, one starting past `end_rva`
 * (0: no limit), or `limit` bytes. Returns the number of HIGHLOW fixups applied. */
static unsigned apply_relocs(uint32_t base, uint32_t at, uint32_t limit, uint32_t end_rva, uint32_t delta)
{
    unsigned n = 0;
    for (uint32_t p = at; p + 8 <= at + limit;)
    {
        uint32_t page = rd32(p), size = rd32(p + 4);
        if (size < 8 || (end_rva && page >= end_rva))
            break;
        for (uint32_t k = 0; k < (size - 8) / 2; ++k)
        {
            uint16_t e = rd16(p + 8 + 2 * k);
            if ((e >> 12) == 3) /* IMAGE_REL_BASED_HIGHLOW */
            {
                uint32_t a = base + page + (e & 0xFFFu);
                wr32(a, rd32(a) + delta);
                n++;
            }
        }
        p += size;
    }
    return n;
}

/* Maps a pinned build at load_base: sections, the packed section into .text, relocations (when load_base is not
 * the preferred base: the PE directory, which covers .rdata/.data, and the unpacking stub's private
 * .text table at the start of .reloc - unless the directory is that table), imports to thunks. */
static int map_build(const char* path, const Build* b, uint32_t load_base)
{
    size_t size = 0;
    unsigned char* f = plat_read_file(path, &size);
    if (!f)
    {
        rt_log("[recomp] cannot read %s\n", path);
        return 0;
    }
    uint32_t nt = rd32b(f + 0x3C);
    const unsigned char* fh = f + nt + 4;
    const unsigned char* oh = fh + 20;
    uint16_t nsec = rd16b(fh + 2);
    uint32_t stamp = rd32b(fh + 4), image_base = rd32b(oh + 28), image_size = rd32b(oh + 56), headers = rd32b(oh + 60);
    if (rd32b(f + nt) != 0x00004550u || image_base != b->base || stamp != b->timestamp || image_size != b->size)
    {
        rt_log("[recomp] %s is not the %s build this translation was made from\n", path, b->name);
        free(f);
        return 0;
    }
    if (gwin_reserve(load_base, image_size) != load_base || !gwin_commit(load_base, image_size))
    {
        rt_log("[recomp] cannot map %s at %08x\n", b->name, load_base);
        free(f);
        return 0;
    }
    memcpy(GUEST_PTR(load_base), f, headers < size ? headers : size);
    const unsigned char* sec = oh + rd16b(fh + 16);
    for (unsigned s = 0; s < nsec; ++s, sec += 40)
    {
        uint32_t va = rd32b(sec + 12), raw = rd32b(sec + 16), ptr = rd32b(sec + 20);
        if (raw && ptr + raw <= size)
            memcpy(GUEST_PTR(load_base + va), f + ptr, raw);
    }
    free(f);

    /* FFXiMain's .text is packed; an image that is not (ffxivgame.exe) has packed_len 0 */
    uint32_t got = b->packed_len ? unpack_text(GUEST_PTR(load_base + b->packed_rva), b->packed_len,
                                               GUEST_PTR(load_base + b->text_rva), b->text_size) : b->text_size;
    if (got != b->text_size)
    {
        rt_log("[recomp] %s: packed section decompressed %u bytes, expected %u\n", b->name, got, b->text_size);
        return 0;
    }

    uint32_t opt = load_base + nt + 24; /* the optional header, in the mapped headers */
    uint32_t delta = load_base - image_base;
    unsigned relocs = 0;
    if (delta)
    {
        uint32_t dir = rd32(opt + 96 + 8 * 5), dir_size = rd32(opt + 96 + 8 * 5 + 4);
        if (dir && dir_size)
            relocs += apply_relocs(load_base, load_base + dir, dir_size, 0, delta);
        if (b->reloc_rva && b->reloc_rva != dir)
            relocs += apply_relocs(load_base, load_base + b->reloc_rva, 0x100000u, b->text_rva + b->text_size, delta);
    }

    uint32_t imp = rd32(opt + 96 + 8 * 1);
    unsigned count = 0;
    for (uint32_t d = load_base + imp; imp && rd32(d + 12); d += 20)
    {
        const char* dll = (const char*)GUEST_PTR(load_base + rd32(d + 12));
        uint32_t names = load_base + (rd32(d + 0) ? rd32(d + 0) : rd32(d + 16));
        uint32_t iat = load_base + rd32(d + 16);
        for (; rd32(names); names += 4, iat += 4, ++count)
        {
            uint32_t e = rd32(names);
            char ord[16];
            const char* name;
            if (e & 0x80000000u)
            {
                snprintf(ord, sizeof ord, "#%u", e & 0xFFFFu);
                name = ord;
            }
            else
                name = (const char*)GUEST_PTR(load_base + e + 2);
            uint32_t t = thunk_for(dll, name);
            if (!t)
            {
                rt_log("[recomp] out of thunks at %s!%s\n", dll, name);
                return 0;
            }
            wr32(iat, t);
        }
    }
    rt_log("[recomp] %s mapped at %08x (host %p), %u relocations, %u imports bound to thunks\n", b->name, load_base,
        (void*)GUEST_PTR(load_base), relocs, count);
    return 1;
}

int pe_load(const char* retail_path)
{
    Build b = { "FFXiMain.dll", rt_image_base, rt_image_timestamp, rt_image_size, rt_image_text_rva, rt_image_text_size,
                rt_image_packed_rva, rt_image_packed_len, rt_image_reloc_rva };
    if (!map_build(retail_path, &b, rt_image_base))
        return 0;
    rt_set_image(rt_image_base); /* at the preferred base: RD = 0 */
    return 1;
}

int pe_load_module(const char* retail_path, const RtModule* m, uint32_t load_base)
{
    Build b = { m->name, m->base, m->timestamp, m->size, m->text_rva, m->text_size, m->packed_rva, m->packed_len,
                m->reloc_rva };
    if (!map_build(retail_path, &b, load_base))
        return 0;
    rt_add_module(m, load_base);
    return 1;
}

uint32_t pe_export_at(uint32_t base, const char* name)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t ed = base + rd32(opt + 96);
    uint32_t n = rd32(ed + 24), funcs = base + rd32(ed + 28), names = base + rd32(ed + 32), ords = base + rd32(ed + 36);
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp((const char*)GUEST_PTR(base + rd32(names + 4 * i)), name))
            return base + rd32(funcs + 4 * rd16(ords + 2 * i));
    return 0;
}

uint32_t pe_export(const char* name)
{
    return pe_export_at(rt_image_base, name);
}

/* One level of a resource directory: the entry for an integer id (0 = the first entry). */
static uint32_t res_entry(uint32_t root, uint32_t dir, uint32_t id)
{
    uint16_t named = rd16(dir + 12), ids = rd16(dir + 14);
    for (uint32_t i = 0; i < (uint32_t)named + ids; ++i)
    {
        uint32_t e = dir + 16 + 8 * i, name = rd32(e);
        if (!id || (!(name & 0x80000000u) && name == id))
            return rd32(e + 4);
    }
    return 0xFFFFFFFFu;
}

/* One level by a Win32 resource name argument: an integer (< 0x10000), "#123", or a string -
 * ANSI or UTF-16 - compared without case against the directory's counted UTF-16 names. */
static uint32_t res_entry_named(uint32_t root, uint32_t dir, uint32_t arg, int wide)
{
    if (arg < 0x10000u)
        return res_entry(root, dir, arg);
    char want[128];
    unsigned n = 0;
    for (uint32_t p = arg; n + 1 < sizeof want; p += wide ? 2 : 1)
    {
        uint32_t c = wide ? rd16(p) : rd8(p);
        if (!c)
            break;
        want[n++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    want[n] = 0;
    if (want[0] == '#')
        return res_entry(root, dir, (uint32_t)strtoul(want + 1, NULL, 10));
    uint16_t named = rd16(dir + 12);
    for (uint32_t i = 0; i < named; ++i)
    {
        uint32_t e = dir + 16 + 8 * i, name = rd32(e);
        if (!(name & 0x80000000u))
            continue;
        uint32_t s = root + (name & 0x7FFFFFFFu), len = rd16(s), k = 0;
        for (; k < len && k < n; ++k)
        {
            uint16_t c = rd16(s + 2 + 2 * k);
            if ((c >= 'a' && c <= 'z' ? c - 32 : c) != (uint8_t)want[k])
                break;
        }
        if (k == len && k == n)
            return rd32(e + 4);
    }
    return 0xFFFFFFFFu;
}

uint32_t pe_find_resource(uint32_t base, uint32_t type, uint32_t name, int wide)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t rva = rd32(opt + 96 + 8 * 2);
    if (!rva)
        return 0;
    uint32_t root = base + rva;
    uint32_t t = res_entry_named(root, root, type, wide);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u))
        return 0;
    uint32_t n = res_entry_named(root, root + (t & 0x7FFFFFFFu), name, wide);
    if (n == 0xFFFFFFFFu || !(n & 0x80000000u))
        return 0;
    uint32_t l = res_entry(root, root + (n & 0x7FFFFFFFu), 0);
    return l == 0xFFFFFFFFu || (l & 0x80000000u) ? 0 : root + l;
}

uint32_t pe_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t* size)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t rva = rd32(opt + 96 + 8 * 2);
    if (!rva)
        return 0;
    uint32_t root = base + rva;
    uint32_t t = res_entry(root, root, type);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u))
        return 0;
    uint32_t n = res_entry(root, root + (t & 0x7FFFFFFFu), name);
    if (n == 0xFFFFFFFFu || !(n & 0x80000000u))
        return 0;
    uint32_t l = res_entry(root, root + (n & 0x7FFFFFFFu), 0); /* whatever language is there */
    if (l == 0xFFFFFFFFu || (l & 0x80000000u))
        return 0;
    uint32_t data = root + l;
    if (size)
        *size = rd32(data + 4);
    return base + rd32(data);
}

uint32_t pe_static_tls(uint32_t base, uint32_t* start, uint32_t* size, uint32_t* zero_fill)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t dir = rd32(opt + 96 + 8 * 9); /* IMAGE_DIRECTORY_ENTRY_TLS */
    if (!dir)
        return 0;
    uint32_t t = base + dir; /* IMAGE_TLS_DIRECTORY32: addresses are absolute (VAs) */
    *start = rd32(t + 0);
    *size = rd32(t + 4) - rd32(t + 0);
    *zero_fill = rd32(t + 16);
    wr32(rd32(t + 8), 0); /* _tls_index: the image's block is slot 0 */
    return rd32(t + 12);  /* the callback array (NULL-terminated), or 0 */
}
