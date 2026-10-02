/* ADVAPI32's registry for 64-bit hosts (R3.1): an in-memory tree loaded from .reg files.
 *
 * The game keeps its settings in HKLM\SOFTWARE\<vendor>\SquareEnix\FinalFantasyXI (values
 * 0000-0044, padmode000 ...; <vendor> is the game's registry key); the vendor keys hold install
 * paths; statically linked D3D code probes SOFTWARE\Microsoft\Direct3D and expects "not found".
 * A Mac has no registry, so the tree is read from files in `reg export` format - what a player
 * produces on Windows with
 *     reg export "HKLM\SOFTWARE\WOW6432Node\<vendor>" ffxi.reg
 * (UTF-16 "Windows Registry Editor Version 5.00" or ANSI REGEDIT4; WOW6432Node is folded away,
 * since the 32-bit game sees the key without it). Changes the game makes are written, as REGEDIT4,
 * to a separate overlay file, loaded last: the imported file is never rewritten. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "k32.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_MORE_ITEMS 259u

#define REG_SZ 1u
#define REG_EXPAND_SZ 2u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define REG_MULTI_SZ 7u

typedef struct RegValue
{
    char* name; /* "" for the default value */
    uint32_t type, size;
    uint8_t* data;
    struct RegValue* next;
} RegValue;

typedef struct RegKey
{
    char* name;
    struct RegKey *parent, *children, *next;
    RegValue* values;
} RegKey;

static RegKey g_roots[4]; /* HKCR, HKCU, HKLM, HKU = 0x80000000 + index */
static const char* const ROOT_NAMES[4] = { "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS" };
static char g_overlay[1024];

static int ieq(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
    return 1;
}

static RegKey* child(RegKey* k, const char* name, size_t n, int create)
{
    for (RegKey* c = k->children; c; c = c->next)
        if (strlen(c->name) == n && ieq(c->name, name, n))
            return c;
    if (!create)
        return NULL;
    RegKey* c = (RegKey*)calloc(1, sizeof *c);
    c->name = (char*)malloc(n + 1);
    memcpy(c->name, name, n);
    c->name[n] = 0;
    c->parent = k;
    c->next = k->children;
    k->children = c;
    return c;
}

/* A subkey path ("SOFTWARE\<vendor>", may be empty or NULL) under k. */
static RegKey* walk(RegKey* k, const char* path, int create)
{
    if (!path)
        return k;
    while (k && *path)
    {
        while (*path == '\\')
            path++;
        const char* e = path;
        while (*e && *e != '\\')
            e++;
        if (e > path)
            k = child(k, path, (size_t)(e - path), create);
        path = e;
    }
    return k;
}

static RegValue* value(RegKey* k, const char* name, int create)
{
    if (!name)
        name = "";
    for (RegValue* v = k->values; v; v = v->next)
        if (ieq(v->name, name, strlen(name) + 1))
            return v;
    if (!create)
        return NULL;
    RegValue* v = (RegValue*)calloc(1, sizeof *v);
    v->name = strdup(name);
    v->next = k->values;
    k->values = v;
    return v;
}

static void set_value(RegKey* k, const char* name, uint32_t type, const void* data, uint32_t size)
{
    RegValue* v = value(k, name, 1);
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = (uint8_t*)malloc(size ? size : 1);
    memcpy(v->data, data, size);
}

/* --- .reg files --------------------------------------------------------------------------------- */

/* the file as ANSI text: UTF-16LE (BOM FF FE) is narrowed, anything above 0xFF becoming '?' */
static char* read_text(const char* path)
{
    size_t n = 0;
    unsigned char* raw = plat_read_file(path, &n);
    if (!raw)
        return NULL;
    char* text = (char*)malloc(n + 1);
    size_t o = 0;
    if (n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
        for (size_t i = 2; i + 1 < n; i += 2)
        {
            unsigned c = raw[i] | (raw[i + 1] << 8);
            text[o++] = c > 0xFF ? '?' : (char)c;
        }
    else
        memcpy(text, raw, o = n);
    text[o] = 0;
    free(raw);
    return text;
}

static RegKey* key_from_header(const char* path, int create)
{
    for (int r = 0; r < 4; ++r)
    {
        size_t l = strlen(ROOT_NAMES[r]);
        if (ieq(path, ROOT_NAMES[r], l) && (path[l] == '\\' || path[l] == 0))
        {
            const char* sub = path + l;
            char folded[1024];
            /* HKLM\SOFTWARE\WOW6432Node\X is what the 32-bit game calls HKLM\SOFTWARE\X */
            if (ieq(sub, "\\SOFTWARE\\WOW6432Node\\", 22) || ieq(sub, "\\SOFTWARE\\WOW6432Node", 21) && !sub[21])
            {
                snprintf(folded, sizeof folded, "\\SOFTWARE%s", sub + 21);
                sub = folded;
            }
            return walk(&g_roots[r], sub, create);
        }
    }
    return NULL;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* a quoted .reg string at p (after the opening quote): unescapes \\ and \" into out */
static const char* unquote(const char* p, char* out, size_t n)
{
    size_t o = 0;
    for (; *p && *p != '"'; ++p)
    {
        if (*p == '\\' && p[1])
            ++p;
        if (o + 1 < n)
            out[o++] = *p;
    }
    out[o] = 0;
    return *p == '"' ? p + 1 : p;
}

static void parse_value(RegKey* k, const char* line)
{
    char name[512];
    const char* p = line;
    if (*p == '@')
    {
        name[0] = 0;
        p++;
    }
    else
        p = unquote(p + 1, name, sizeof name);
    if (*p++ != '=')
        return;
    if (*p == '"')
    {
        char s[4096];
        unquote(p + 1, s, sizeof s);
        set_value(k, name, REG_SZ, s, (uint32_t)strlen(s) + 1);
    }
    else if (!strncmp(p, "dword:", 6))
    {
        uint32_t v = (uint32_t)strtoul(p + 6, NULL, 16);
        set_value(k, name, REG_DWORD, &v, 4);
    }
    else if (!strncmp(p, "hex", 3))
    {
        uint32_t type = REG_BINARY;
        p += 3;
        if (*p == '(')
        {
            type = (uint32_t)strtoul(p + 1, NULL, 16);
            p = strchr(p, ')') + 1;
        }
        p++; /* ':' */
        uint8_t buf[65536];
        uint32_t n = 0;
        for (; *p; ++p)
        {
            int a = hexval(p[0]), b = a >= 0 ? hexval(p[1]) : -1;
            if (a >= 0 && b >= 0 && n < sizeof buf)
            {
                buf[n++] = (uint8_t)(a * 16 + b);
                ++p;
            }
        }
        set_value(k, name, type, buf, n);
    }
}

static void load(const char* path)
{
    char* text = read_text(path);
    if (!text)
        return;
    RegKey* k = NULL;
    char line[70000];
    size_t len = 0;
    for (char* p = text;; ++p)
    {
        if (*p && *p != '\n')
        {
            if (*p != '\r' && len + 1 < sizeof line)
                line[len++] = *p;
            continue;
        }
        line[len] = 0;
        /* a trailing backslash continues a hex value on the next line */
        if (len && line[len - 1] == '\\' && *p)
        {
            len--;
            while (p[1] == ' ')
                ++p;
            continue;
        }
        if (line[0] == '[' && strchr(line, ']'))
        {
            *strchr(line, ']') = 0;
            k = line[1] == '-' ? NULL : key_from_header(line + 1, 1);
        }
        else if (k && (line[0] == '"' || line[0] == '@'))
            parse_value(k, line);
        len = 0;
        if (!*p)
            break;
    }
    free(text);
}

static void save_key(FILE* f, RegKey* k, const char* path)
{
    if (k->values)
    {
        fprintf(f, "\n[%s]\n", path);
        for (RegValue* v = k->values; v; v = v->next)
        {
            if (v->name[0])
                fprintf(f, "\"%s\"=", v->name);
            else
                fprintf(f, "@=");
            if (v->type == REG_DWORD && v->size == 4)
            {
                uint32_t d;
                memcpy(&d, v->data, 4);
                fprintf(f, "dword:%08x\n", d);
            }
            else if (v->type == REG_SZ && v->size && !memchr(v->data, '"', v->size) && !memchr(v->data, '\\', v->size))
                fprintf(f, "\"%.*s\"\n", (int)(v->size - 1), (const char*)v->data);
            else
            {
                fprintf(f, v->type == REG_BINARY ? "hex:" : "hex(%x):", v->type);
                for (uint32_t i = 0; i < v->size; ++i)
                    fprintf(f, i ? ",%02x" : "%02x", v->data[i]);
                fprintf(f, "\n");
            }
        }
    }
    for (RegKey* c = k->children; c; c = c->next)
    {
        char sub[2048];
        snprintf(sub, sizeof sub, "%s\\%s", path, c->name);
        save_key(f, c, sub);
    }
}

/* The whole tree goes to the overlay: simple, and small (the game's keys are a few KB). */
static void save(void)
{
    if (!g_overlay[0])
        return;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_overlay);
    FILE* f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "REGEDIT4\n");
    for (int r = 0; r < 4; ++r)
        save_key(f, &g_roots[r], ROOT_NAMES[r]);
    fclose(f);
    plat_rename(tmp, g_overlay);
}

/* --- the guest API ------------------------------------------------------------------------------ */
static RegKey* key_of(uint32_t h)
{
    if (h >= 0x80000000u && h <= 0x80000003u)
        return &g_roots[h - 0x80000000u];
    return (RegKey*)k_data(h, K_REGKEY);
}

static uint32_t open_key(uint32_t parent, uint32_t sub, int create, uint32_t out, uint32_t* created)
{
    RegKey* p = key_of(parent);
    if (!p)
        return ERROR_INVALID_HANDLE;
    const char* path = sub ? (const char*)GUEST_PTR(sub) : NULL;
    RegKey* k = walk(p, path, 0);
    if (created)
        *created = !k;
    if (!k && create)
        k = walk(p, path, 1);
    if (!k)
        return ERROR_FILE_NOT_FOUND;
    wr32(out, k_new(K_REGKEY, k, NULL));
    return ERROR_SUCCESS;
}

static void sh_RegOpenKeyA(Guest* g) { RET(open_key(ARG(0), ARG(1), 0, ARG(2), NULL), 3); }
static void sh_RegOpenKeyExA(Guest* g) { RET(open_key(ARG(0), ARG(1), 0, ARG(4), NULL), 5); }
static void sh_RegCreateKeyA(Guest* g) { RET(open_key(ARG(0), ARG(1), 1, ARG(2), NULL), 3); }

static void sh_RegCreateKeyExA(Guest* g)
{
    uint32_t created = 0;
    uint32_t r = open_key(ARG(0), ARG(1), 1, ARG(7), &created);
    if (!r && ARG(8))
        wr32(ARG(8), created ? 1u : 2u); /* REG_CREATED_NEW_KEY / REG_OPENED_EXISTING_KEY */
    if (created)
        save();
    RET(r, 9);
}

static void sh_RegCloseKey(Guest* g)
{
    uint32_t h = ARG(0);
    if (h < 0x80000000u)
        k_close(h);
    RET(ERROR_SUCCESS, 1);
}

static void sh_RegQueryValueExA(Guest* g)
{
    RegKey* k = key_of(ARG(0));
    uint32_t type_p = ARG(3), data = ARG(4), size_p = ARG(5);
    if (!k)
        RET(ERROR_INVALID_HANDLE, 6);
    RegValue* v = value(k, ARG(1) ? ARGS(1) : NULL, 0);
    if (!v)
        RET(ERROR_FILE_NOT_FOUND, 6);
    if (type_p)
        wr32(type_p, v->type);
    uint32_t r = ERROR_SUCCESS;
    if (data)
    {
        if (!size_p || rd32(size_p) < v->size)
            r = ERROR_MORE_DATA;
        else
            memcpy(GUEST_PTR(data), v->data, v->size);
    }
    if (size_p)
        wr32(size_p, v->size);
    RET(r, 6);
}

static void sh_RegSetValueExA(Guest* g)
{
    RegKey* k = key_of(ARG(0));
    if (!k)
        RET(ERROR_INVALID_HANDLE, 6);
    set_value(k, ARG(1) ? ARGS(1) : NULL, ARG(3), GUEST_PTR(ARG(4)), ARG(5));
    save();
    RET(ERROR_SUCCESS, 6);
}

static void sh_RegDeleteValueA(Guest* g)
{
    RegKey* k = key_of(ARG(0));
    if (!k)
        RET(ERROR_INVALID_HANDLE, 2);
    const char* name = ARG(1) ? ARGS(1) : "";
    for (RegValue** pv = &k->values; *pv; pv = &(*pv)->next)
        if (ieq((*pv)->name, name, strlen(name) + 1))
        {
            RegValue* v = *pv;
            *pv = v->next;
            free(v->name);
            free(v->data);
            free(v);
            save();
            RET(ERROR_SUCCESS, 2);
        }
    RET(ERROR_FILE_NOT_FOUND, 2);
}

static void sh_RegDeleteKeyA(Guest* g)
{
    RegKey* p = key_of(ARG(0));
    RegKey* k = p ? walk(p, ARGS(1), 0) : NULL;
    if (!k || k == p || k->children)
        RET(k ? 5u : ERROR_FILE_NOT_FOUND, 2); /* a key with subkeys: ERROR_ACCESS_DENIED, as Win32 */
    for (RegKey** pk = &k->parent->children; *pk; pk = &(*pk)->next)
        if (*pk == k)
        {
            *pk = k->next;
            break;
        }
    save(); /* the node itself is not freed: an open handle may still name it */
    RET(ERROR_SUCCESS, 2);
}

static uint32_t copy_name(const char* s, uint32_t buf, uint32_t cch_p)
{
    uint32_t len = (uint32_t)strlen(s);
    if (!cch_p || rd32(cch_p) <= len)
        return ERROR_MORE_DATA;
    memcpy(GUEST_PTR(buf), s, len + 1);
    wr32(cch_p, len);
    return ERROR_SUCCESS;
}

static void sh_RegEnumKeyExA(Guest* g)
{
    RegKey* k = key_of(ARG(0));
    if (!k)
        RET(ERROR_INVALID_HANDLE, 8);
    uint32_t i = ARG(1);
    RegKey* c = k->children;
    while (c && i--)
        c = c->next;
    if (!c)
        RET(ERROR_NO_MORE_ITEMS, 8);
    RET(copy_name(c->name, ARG(2), ARG(3)), 8);
}

static void sh_RegEnumValueA(Guest* g)
{
    RegKey* k = key_of(ARG(0));
    if (!k)
        RET(ERROR_INVALID_HANDLE, 8);
    uint32_t i = ARG(1);
    RegValue* v = k->values;
    while (v && i--)
        v = v->next;
    if (!v)
        RET(ERROR_NO_MORE_ITEMS, 8);
    uint32_t r = copy_name(v->name, ARG(2), ARG(3));
    if (ARG(5))
        wr32(ARG(5), v->type);
    if (!r && ARG(6))
    {
        if (!ARG(7) || rd32(ARG(7)) < v->size)
            r = ERROR_MORE_DATA;
        else
            memcpy(GUEST_PTR(ARG(6)), v->data, v->size);
    }
    if (ARG(7))
        wr32(ARG(7), v->size);
    RET(r, 8);
}

static void sh_RegQueryInfoKeyA(Guest* g)
{
    RegKey* k = key_of(ARG(0));
    if (!k)
        RET(ERROR_INVALID_HANDLE, 12);
    uint32_t subkeys = 0, maxsub = 0, values = 0, maxname = 0, maxdata = 0;
    for (RegKey* c = k->children; c; c = c->next, ++subkeys)
        if (strlen(c->name) > maxsub)
            maxsub = (uint32_t)strlen(c->name);
    for (RegValue* v = k->values; v; v = v->next, ++values)
    {
        if (strlen(v->name) > maxname)
            maxname = (uint32_t)strlen(v->name);
        if (v->size > maxdata)
            maxdata = v->size;
    }
    if (ARG(2))
        wr32(ARG(2), 0);
    uint32_t outs[] = { ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10) };
    uint32_t vals[] = { subkeys, maxsub, 0, values, maxname, maxdata, 0 };
    for (int i = 0; i < 7; ++i)
        if (outs[i])
            wr32(outs[i], vals[i]);
    if (ARG(11))
        wr64(ARG(11), 0);
    RET(ERROR_SUCCESS, 12);
}

static const ShimDef REG[] = {
    { "advapi32.dll", "RegOpenKeyA", sh_RegOpenKeyA },
    { "advapi32.dll", "RegOpenKeyExA", sh_RegOpenKeyExA },
    { "advapi32.dll", "RegCreateKeyA", sh_RegCreateKeyA },
    { "advapi32.dll", "RegCreateKeyExA", sh_RegCreateKeyExA },
    { "advapi32.dll", "RegCloseKey", sh_RegCloseKey },
    { "advapi32.dll", "RegQueryValueExA", sh_RegQueryValueExA },
    { "advapi32.dll", "RegSetValueExA", sh_RegSetValueExA },
    { "advapi32.dll", "RegDeleteValueA", sh_RegDeleteValueA },
    { "advapi32.dll", "RegDeleteKeyA", sh_RegDeleteKeyA },
    { "advapi32.dll", "RegEnumKeyExA", sh_RegEnumKeyExA },
    { "advapi32.dll", "RegEnumValueA", sh_RegEnumValueA },
    { "advapi32.dll", "RegQueryInfoKeyA", sh_RegQueryInfoKeyA },
    { NULL, NULL, NULL },
};

void reg_init(const char* const* files, unsigned nfiles, const char* overlay)
{
    for (unsigned i = 0; i < nfiles; ++i)
        load(files[i]);
    if (overlay)
    {
        snprintf(g_overlay, sizeof g_overlay, "%s", overlay);
        load(overlay);
    }
    thunk_register(REG);
}

void reg_load_final(const char* path)
{
    load(path);
}

int reg_get_string(const char* path, const char* name, char* out, size_t n)
{
    RegKey* k = key_from_header(path, 0);
    RegValue* v = k ? value(k, name, 0) : NULL;
    if (!v || (v->type != REG_SZ && v->type != REG_EXPAND_SZ) || !n)
        return 0;
    size_t c = v->size < n ? v->size : n - 1;
    memcpy(out, v->data, c);
    out[c] = 0;
    return 1;
}

void reg_set_string(const char* path, const char* name, const char* value)
{
    RegKey* k = key_from_header(path, 1);
    if (k)
        set_value(k, name, REG_SZ, value, (uint32_t)strlen(value) + 1);
}

int reg_get_dword(const char* path, const char* name, uint32_t* out)
{
    RegKey* k = key_from_header(path, 0);
    RegValue* v = k ? value(k, name, 0) : NULL;
    if (!v || v->size != 4)
        return 0;
    memcpy(out, v->data, 4);
    return 1;
}
