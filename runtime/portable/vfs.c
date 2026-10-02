/* Guest paths to host paths. See vfs.h. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "vfs.h"

#define MAX_MOUNTS 16

static char g_cwd[512] = "C:\\";
static struct
{
    char guest[256];
    char host[512];
} g_mounts[MAX_MOUNTS];
static unsigned g_nmounts;

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

void vfs_init(const char* guest_cwd)
{
    char full[512];
    if (guest_cwd && vfs_full_path(guest_cwd, full, sizeof full))
        snprintf(g_cwd, sizeof g_cwd, "%s", full);
}

void vfs_mount(const char* guest_prefix, const char* host_prefix)
{
    if (g_nmounts < MAX_MOUNTS)
    {
        vfs_full_path(guest_prefix, g_mounts[g_nmounts].guest, sizeof g_mounts[0].guest);
        snprintf(g_mounts[g_nmounts].host, sizeof g_mounts[0].host, "%s", host_prefix);
        g_nmounts++;
    }
}

/* Appends one path onto an absolute one ("C:\a\b"), resolving ".", "..", empty components and
 * both separators. The drive root keeps its trailing backslash ("C:\"); nothing else does. */
static int join(char* out, size_t n, const char* base, const char* rel)
{
    char buf[1024];
    size_t len = strlen(base);
    if (len >= sizeof buf)
        return 0;
    memcpy(buf, base, len + 1);
    if (len > 3 && buf[len - 1] == '\\')
        buf[--len] = 0;
    const char* p = rel;
    while (*p)
    {
        while (*p == '\\' || *p == '/')
            p++;
        const char* e = p;
        while (*e && *e != '\\' && *e != '/')
            e++;
        size_t c = (size_t)(e - p);
        if (c == 0)
            break;
        if (c == 1 && p[0] == '.')
        {
        }
        else if (c == 2 && p[0] == '.' && p[1] == '.')
        {
            char* s = strrchr(buf, '\\');
            if (s && s > buf + 2)
                *s = 0, len = (size_t)(s - buf);
            else
                buf[3] = 0, len = 3; /* stays at the root */
        }
        else
        {
            if (len + c + 2 >= sizeof buf)
                return 0;
            if (buf[len - 1] != '\\')
                buf[len++] = '\\';
            memcpy(buf + len, p, c);
            len += c;
            buf[len] = 0;
        }
        p = e;
    }
    if (strlen(buf) + 1 > n)
        return 0;
    strcpy(out, buf);
    return 1;
}

int vfs_full_path(const char* guest, char* out, size_t n)
{
    char root[4] = "C:\\";
    if (((guest[0] >= 'A' && guest[0] <= 'Z') || (guest[0] >= 'a' && guest[0] <= 'z')) && guest[1] == ':')
    {
        root[0] = (char)(guest[0] & ~0x20);
        if (guest[2] == '\\' || guest[2] == '/')
            return join(out, n, root, guest + 2);
        /* "C:foo" - relative to that drive's current directory; there is only one */
        return join(out, n, g_cwd[0] == root[0] ? g_cwd : root, guest + 2);
    }
    if (guest[0] == '\\' || guest[0] == '/')
    {
        root[0] = g_cwd[0];
        return join(out, n, root, guest);
    }
    return join(out, n, g_cwd, guest);
}

/* Guest strings are code page 1252: bytes 0x80-0xFF become their Unicode code points in UTF-8
 * (the 0x80-0x9F block of 1252 is not special-cased yet, as in k32.c). */
static int to_utf8(const char* in, char* out, size_t n)
{
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p)
    {
        if (*p < 0x80)
        {
            if (o + 1 >= n)
                return 0;
            out[o++] = (char)*p;
        }
        else
        {
            if (o + 2 >= n)
                return 0;
            out[o++] = (char)(0xC0 | (*p >> 6));
            out[o++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    out[o] = 0;
    return 1;
}

/* --- DAT overlays --------------------------------------------------------------------------------- */

/* One file an overlay supplies: its key (lower case, from the ROM or sound folder on, '\'
 * separators: "rom2\12\34.dat") and its host path. Open addressing over a power-of-two table. */
typedef struct
{
    char* key;
    char* host;
} Overlay;
static Overlay* g_ov;
static unsigned g_ov_cap, g_ov_n;
static int g_ov_trace = -1;

static uint32_t ov_hash(const char* s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static Overlay* ov_slot(const char* key)
{
    for (uint32_t i = ov_hash(key) & (g_ov_cap - 1);; i = (i + 1) & (g_ov_cap - 1))
        if (!g_ov[i].key || !strcmp(g_ov[i].key, key))
            return &g_ov[i];
}

static char* dup(const char* s)
{
    size_t l = strlen(s) + 1;
    char* d = (char*)malloc(l);
    memcpy(d, s, l);
    return d;
}

/* Adds key -> host unless an earlier overlay has the key. 1 if added. */
static int ov_add(const char* key, const char* host)
{
    if (2 * (g_ov_n + 1) > g_ov_cap)
    {
        Overlay* old = g_ov;
        unsigned old_cap = g_ov_cap;
        g_ov_cap = g_ov_cap ? g_ov_cap * 2 : 4096;
        g_ov = (Overlay*)calloc(g_ov_cap, sizeof *g_ov);
        for (unsigned i = 0; i < old_cap; ++i)
            if (old[i].key)
                *ov_slot(old[i].key) = old[i];
        free(old);
    }
    Overlay* s = ov_slot(key);
    if (s->key)
        return 0;
    s->key = dup(key);
    s->host = dup(host);
    g_ov_n++;
    return 1;
}

/* "rom", "rom2", "sound", "sound3", in any case: the folders the game's DAT paths go through. */
static int prefix_ci(const char* name, size_t len, const char* word)
{
    size_t k = strlen(word);
    if (len < k)
        return 0;
    for (size_t i = 0; i < k; ++i)
        if (lower((unsigned char)name[i]) != word[i])
            return 0;
    return 1;
}

static int dat_root(const char* name, size_t len)
{
    size_t k = prefix_ci(name, len, "rom") ? 3 : prefix_ci(name, len, "sound") ? 5 : 0;
    if (!k)
        return 0;
    for (size_t i = k; i < len; ++i)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    return 1;
}

/* Every file under host (a ROM or sound folder or below), keyed by key + its relative path. */
static unsigned ov_scan(const char* host, const char* key, int depth)
{
    PlatDir* d = plat_dir_open(host);
    if (!d)
        return 0;
    unsigned added = 0;
    for (const char* e; (e = plat_dir_next(d));)
    {
        char name[256], sub_host[1400], sub_key[512];
        PlatStat st;
        snprintf(name, sizeof name, "%s", e);
        if (name[0] == '.')
            continue;
        snprintf(sub_host, sizeof sub_host, "%s%c%s", host, plat_path_sep, name);
        snprintf(sub_key, sizeof sub_key, "%s\\%s", key, name);
        for (char* p = sub_key; *p; ++p)
            *p = (char)lower((unsigned char)*p);
        if (!plat_stat(sub_host, &st))
            continue;
        if (st.is_dir)
            added += depth < 6 ? ov_scan(sub_host, sub_key, depth + 1) : 0;
        else
            added += (unsigned)ov_add(sub_key, sub_host);
    }
    plat_dir_close(d);
    return added;
}

/* The ROM or sound children of one overlay folder. -1 if it has none (then it is a folder of overlays). */
static long ov_add_one(const char* host_dir)
{
    PlatDir* d = plat_dir_open(host_dir);
    if (!d)
        return -1;
    long added = -1;
    for (const char* e; (e = plat_dir_next(d));)
    {
        char name[256], sub[1400];
        PlatStat st;
        snprintf(name, sizeof name, "%s", e);
        snprintf(sub, sizeof sub, "%s%c%s", host_dir, plat_path_sep, name);
        if (!dat_root(name, strlen(name)) || !plat_stat(sub, &st) || !st.is_dir)
            continue;
        char key[256];
        snprintf(key, sizeof key, "%s", name);
        for (char* p = key; *p; ++p)
            *p = (char)lower((unsigned char)*p);
        added = (added < 0 ? 0 : added) + (long)ov_scan(sub, key, 0);
    }
    plat_dir_close(d);
    return added;
}

static int by_name(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

unsigned vfs_add_overlay(const char* host_dir, void (*report)(const char* name, unsigned files))
{
    long direct = ov_add_one(host_dir);
    if (direct >= 0)
    {
        if (report)
            report(host_dir, (unsigned)direct);
        return (unsigned)direct;
    }
    /* a folder of overlays (XIPivot's layout: DATs\era-dats, DATs\<mod>, ...): each in name order */
    PlatDir* d = plat_dir_open(host_dir);
    if (!d)
        return 0;
    char** names = NULL;
    unsigned n = 0, cap = 0, total = 0;
    for (const char* e; (e = plat_dir_next(d));)
    {
        if (e[0] == '.')
            continue;
        if (n == cap)
            names = (char**)realloc(names, (cap = cap ? cap * 2 : 16) * sizeof *names);
        names[n++] = dup(e);
    }
    plat_dir_close(d);
    qsort(names, n, sizeof *names, by_name);
    for (unsigned i = 0; i < n; ++i)
    {
        char sub[1400];
        snprintf(sub, sizeof sub, "%s%c%s", host_dir, plat_path_sep, names[i]);
        long added = ov_add_one(sub);
        if (added >= 0)
        {
            total += (unsigned)added;
            if (report)
                report(names[i], (unsigned)added);
        }
        free(names[i]);
    }
    free(names);
    return total;
}

/* The key of an absolute guest path: from its first ROM or sound component on, lower case. */
static int ov_key(const char* full, char* key, size_t n)
{
    for (const char* p = full; (p = strchr(p, '\\'));)
    {
        const char* c = ++p;
        const char* e = c;
        while (*e && *e != '\\')
            e++;
        if (*e == '\\' && dat_root(c, (size_t)(e - c)))
        {
            size_t l = strlen(c);
            if (l + 1 > n)
                return 0;
            for (size_t i = 0; i <= l; ++i)
                key[i] = (char)lower((unsigned char)c[i]);
            return 1;
        }
    }
    return 0;
}

int vfs_overlay_path(const char* guest, char* host, size_t n)
{
    char full[1024], key[512];
    if (!g_ov_n || !vfs_full_path(guest, full, sizeof full) || !ov_key(full, key, sizeof key))
        return 0;
    Overlay* s = ov_slot(key);
    if (g_ov_trace < 0)
        g_ov_trace = getenv("FFXI_DATS_TRACE") && getenv("FFXI_DATS_TRACE")[0] == '1';
    if (!s->key || strlen(s->host) + 1 > n)
        return 0;
    if (g_ov_trace)
        fprintf(stderr, "[dats] %s -> %s\n", full, s->host);
    strcpy(host, s->host);
    return 1;
}

int vfs_host_path(const char* guest, char* host, size_t n)
{
    char full[1024], mapped[1400];
    if (!vfs_full_path(guest, full, sizeof full))
        return 0;
    if (vfs_overlay_path(full, host, n))
        return 1;
    int best = -1;
    size_t best_len = 0;
    for (unsigned i = 0; i < g_nmounts; ++i)
    {
        size_t l = strlen(g_mounts[i].guest);
        int match = l > best_len && strlen(full) >= l && (full[l] == 0 || full[l] == '\\' || g_mounts[i].guest[l - 1] == '\\');
        for (size_t k = 0; match && k < l; ++k)
            match = lower((unsigned char)full[k]) == lower((unsigned char)g_mounts[i].guest[k]);
        if (match)
            best = (int)i, best_len = l;
    }
    if (best >= 0)
        snprintf(mapped, sizeof mapped, "%s%s", g_mounts[best].host, full + best_len);
    else if (plat_path_sep == '\\')
        snprintf(mapped, sizeof mapped, "%s", full); /* a Windows host: the guest path is the host path */
    else
        return 0;
    if (plat_path_sep != '\\')
        for (char* p = mapped; *p; ++p)
            if (*p == '\\')
                *p = plat_path_sep;
    return to_utf8(mapped, host, n);
}

const char* vfs_cwd(void)
{
    return g_cwd;
}

int vfs_set_cwd(const char* guest)
{
    char full[512], host[1400];
    PlatStat st;
    if (!vfs_full_path(guest, full, sizeof full) || !vfs_host_path(full, host, sizeof host) || !plat_stat(host, &st) || !st.is_dir)
        return 0;
    strcpy(g_cwd, full);
    return 1;
}
