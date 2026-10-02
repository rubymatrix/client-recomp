/* KERNEL32 for 64-bit hosts, part 2 (R3.1): files and directories (through vfs), events,
 * mutexes, semaphores, waits and threads (through kobj), and time - the rest of what the
 * translated game called live in the R3.0 trace. Written against plat.h only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"
#include "vfs.h"

#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_GEN_FAILURE 31u

static uint32_t win32_error(PlatError e)
{
    switch (e)
    {
    case PLAT_OK: return 0;
    case PLAT_NOT_FOUND: return ERROR_FILE_NOT_FOUND;
    case PLAT_PATH_NOT_FOUND: return ERROR_PATH_NOT_FOUND;
    case PLAT_ACCESS: return ERROR_ACCESS_DENIED;
    case PLAT_EXISTS: return ERROR_FILE_EXISTS;
    case PLAT_NOT_EMPTY: return ERROR_DIR_NOT_EMPTY;
    default: return ERROR_GEN_FAILURE;
    }
}

static void fail_plat(void) { gt_set_error(win32_error(plat_error())); }

/* A guest path argument to a host path; sets ERROR_PATH_NOT_FOUND if it maps nowhere. */
static int host_of(uint32_t guest_str, char* host, size_t n)
{
    if (!guest_str || !vfs_host_path((const char*)GUEST_PTR(guest_str), host, n))
    {
        gt_set_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    return 1;
}

/* Host names (UTF-8) back to the guest's code page 1252; anything outside it becomes '?'. */
static void to_guest_name(const char* utf8, char* out, size_t n)
{
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)utf8; *p && o + 1 < n;)
    {
        unsigned c = *p++;
        if (c >= 0x80)
        {
            unsigned len = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            unsigned v = c & (0x3F >> len);
            for (unsigned k = 0; k < len && (*p & 0xC0) == 0x80; ++k)
                v = (v << 6) | (*p++ & 0x3F);
            c = v <= 0xFF ? v : '?';
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

/* --- time -------------------------------------------------------------------------------------- */
#define FT_EPOCH 116444736000000000ull /* 1601 -> 1970 in 100 ns */

static uint64_t ms_to_ft(uint64_t ms) { return ms * 10000u + FT_EPOCH; }
static uint64_t ft_to_ms(uint64_t ft) { return ft >= FT_EPOCH ? (ft - FT_EPOCH) / 10000u : 0; }

static void write_systemtime(uint32_t p, const PlatTime* t)
{
    wr16(p + 0, (uint16_t)t->year);
    wr16(p + 2, (uint16_t)t->month);
    wr16(p + 4, (uint16_t)t->day_of_week);
    wr16(p + 6, (uint16_t)t->day);
    wr16(p + 8, (uint16_t)t->hour);
    wr16(p + 10, (uint16_t)t->minute);
    wr16(p + 12, (uint16_t)t->second);
    wr16(p + 14, (uint16_t)t->ms);
}

/* days since 1970-01-01 of a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void sh_GetLocalTime(Guest* g) { PlatTime t; plat_split_time(plat_wall_ms(), 1, &t); write_systemtime(ARG(0), &t); RET(0, 1); }
static void sh_GetSystemTime(Guest* g) { PlatTime t; plat_split_time(plat_wall_ms(), 0, &t); write_systemtime(ARG(0), &t); RET(0, 1); }

static void sh_FileTimeToSystemTime(Guest* g)
{
    PlatTime t;
    plat_split_time(ft_to_ms(rd64(ARG(0))), 0, &t);
    write_systemtime(ARG(1), &t);
    RET(1, 2);
}

static void sh_SystemTimeToFileTime(Guest* g)
{
    uint32_t p = ARG(0);
    int64_t days = days_from_civil(rd16(p), rd16(p + 2), rd16(p + 6));
    uint64_t ms = (uint64_t)(days * 86400000 + rd16(p + 8) * 3600000 + rd16(p + 10) * 60000 + rd16(p + 12) * 1000 + rd16(p + 14));
    wr64(ARG(1), ms_to_ft(ms));
    RET(1, 2);
}

static void sh_FileTimeToLocalFileTime(Guest* g)
{
    int64_t bias = (int64_t)plat_utc_bias_minutes() * 60 * 10000000;
    wr64(ARG(1), (uint64_t)((int64_t)rd64(ARG(0)) - bias));
    RET(1, 2);
}

static void sh_LocalFileTimeToFileTime(Guest* g)
{
    int64_t bias = (int64_t)plat_utc_bias_minutes() * 60 * 10000000;
    wr64(ARG(1), (uint64_t)((int64_t)rd64(ARG(0)) + bias));
    RET(1, 2);
}

static void sh_CompareFileTime(Guest* g)
{
    uint64_t a = rd64(ARG(0)), b = rd64(ARG(1));
    RET(a < b ? (uint32_t)-1 : a > b, 2);
}

/* TIME_ZONE_INFORMATION: Bias, then names and transition dates the game does not read. */
static void sh_GetTimeZoneInformation(Guest* g)
{
    memset(ARGP(0), 0, 172);
    wr32(ARG(0), (uint32_t)plat_utc_bias_minutes());
    RET(0, 1); /* TIME_ZONE_ID_UNKNOWN: the bias already includes daylight time */
}

static void sh_GlobalMemoryStatus(Guest* g)
{
    uint64_t total, avail;
    plat_memory(&total, &avail);
    uint32_t p = ARG(0);
    uint32_t t = total > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)total, a = avail > t ? t : (uint32_t)avail;
    wr32(p + 0, 32);
    wr32(p + 4, t ? 100 - (uint32_t)((uint64_t)a * 100 / t) : 0); /* dwMemoryLoad */
    wr32(p + 8, t);
    wr32(p + 12, a);
    wr32(p + 16, 0x7FFFFFFFu);
    wr32(p + 20, 0x7FFFFFFFu);
    wr32(p + 24, 0x7FFE0000u); /* the 2 GB user address space of a 32-bit process */
    wr32(p + 28, 0x60000000u);
    RET(0, 1);
}

/* --- files ------------------------------------------------------------------------------------- */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u

static void file_close(void* f) { plat_file_close((PlatFile*)f); }

/* FFXI_RECOMP_TRACE=1: the guest path of every file open or search that fails */
static void trace_miss(const char* api, const char* path)
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("FFXI_RECOMP_TRACE");
        on = e && *e == '1';
    }
    if (on)
        fprintf(stderr, "[trace] %s failed: %s\n", api, path);
}

static void sh_CreateFileA(Guest* g)
{
    char host[1400];
    uint32_t access = ARG(1), disp = ARG(4);
    if (!host_of(ARG(0), host, sizeof host))
    {
        trace_miss("CreateFileA", ARGS(0));
        RET(INVALID_HANDLE, 7);
    }
    PlatStat st;
    int existed = plat_stat(host, &st);
    int flags = ((access & GENERIC_READ) || !(access & GENERIC_WRITE) ? PLAT_READ : 0) | ((access & GENERIC_WRITE) ? PLAT_WRITE : 0);
    switch (disp)
    {
    case 1: flags |= PLAT_CREATE | PLAT_EXCL; break;     /* CREATE_NEW */
    case 2: flags |= PLAT_CREATE | PLAT_TRUNCATE; break; /* CREATE_ALWAYS */
    case 3: break;                                       /* OPEN_EXISTING */
    case 4: flags |= PLAT_CREATE; break;                 /* OPEN_ALWAYS */
    case 5: flags |= PLAT_TRUNCATE; break;               /* TRUNCATE_EXISTING */
    default: gt_set_error(ERROR_INVALID_PARAMETER); RET(INVALID_HANDLE, 7);
    }
    PlatFile* f = plat_file_open(host, flags);
    if (!f)
    {
        fail_plat();
        trace_miss("CreateFileA", ARGS(0));
        RET(INVALID_HANDLE, 7);
    }
    uint32_t h = k_new(K_FILE, f, file_close);
    gt_set_error((disp == 2 || disp == 4) && existed ? ERROR_ALREADY_EXISTS : 0);
    RET(h, 7);
}

static void sh_ReadFile(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    uint32_t n = ARG(2), out = ARG(3);
    if (!f)
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0, 5);
    }
    gt_unlock(); /* file I/O may block: other guest threads run meanwhile */
    int64_t got = plat_file_read(f, GUEST_PTR(ARG(1)), n);
    gt_lock();
    if (out)
        wr32(out, got > 0 ? (uint32_t)got : 0);
    if (got < 0)
    {
        fail_plat();
        RET(0, 5);
    }
    RET(1, 5);
}

static void sh_WriteFile(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    uint32_t n = ARG(2), out = ARG(3);
    if (!f)
    {
        /* the console handles a GUI process never has: accept and discard, as Windows does for NUL */
        if (out)
            wr32(out, n);
        RET(ARG(0) ? 1 : 0, 5);
    }
    gt_unlock();
    int64_t put = plat_file_write(f, GUEST_PTR(ARG(1)), n);
    gt_lock();
    if (out)
        wr32(out, put > 0 ? (uint32_t)put : 0);
    if (put < 0)
    {
        fail_plat();
        RET(0, 5);
    }
    RET(1, 5);
}

static void sh_SetFilePointer(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    uint32_t hi_p = ARG(2);
    if (!f)
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0xFFFFFFFFu, 4);
    }
    int64_t off = hi_p ? (int64_t)(((uint64_t)rd32(hi_p) << 32) | ARG(1)) : (int64_t)(int32_t)ARG(1);
    int64_t pos = plat_file_seek(f, off, (int)ARG(3));
    if (pos < 0)
    {
        gt_set_error(pos == -1 ? ERROR_INVALID_PARAMETER : win32_error(plat_error()));
        RET(0xFFFFFFFFu, 4);
    }
    if (hi_p)
        wr32(hi_p, (uint32_t)((uint64_t)pos >> 32));
    gt_set_error(0);
    RET((uint32_t)pos, 4);
}

static void sh_GetFileSize(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    if (!f)
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0xFFFFFFFFu, 2);
    }
    int64_t s = plat_file_size(f);
    if (ARG(1))
        wr32(ARG(1), (uint32_t)((uint64_t)s >> 32));
    gt_set_error(0);
    RET((uint32_t)s, 2);
}

static void sh_SetEndOfFile(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    RET(f && plat_file_truncate(f), 1);
}

static void sh_FlushFileBuffers(Guest* g)
{
    PlatFile* f = (PlatFile*)k_data(ARG(0), K_FILE);
    RET(f && plat_file_flush(f), 1);
}

static void sh_GetFileType(Guest* g) { RET(k_data(ARG(0), K_FILE) ? 1u : 0u, 1); } /* FILE_TYPE_DISK, else UNKNOWN */

static void sh_CloseHandle(Guest* g)
{
    if (!k_close(ARG(0)))
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0, 1);
    }
    RET(1, 1);
}

static void sh_DuplicateHandle(Guest* g)
{
    uint32_t d = k_duplicate(ARG(1));
    if (ARG(3))
        wr32(ARG(3), d);
    if (ARG(6) & 1) /* DUPLICATE_CLOSE_SOURCE */
        k_close(ARG(1));
    RET(d != 0, 7);
}

static void sh_DeleteFileA(Guest* g)
{
    char host[1400];
    if (!host_of(ARG(0), host, sizeof host))
        RET(0, 1);
    if (!plat_unlink(host))
    {
        fail_plat();
        RET(0, 1);
    }
    RET(1, 1);
}

static void sh_CreateDirectoryA(Guest* g)
{
    char host[1400];
    if (!host_of(ARG(0), host, sizeof host))
        RET(0, 2);
    if (!plat_mkdir(host))
    {
        gt_set_error(plat_error() == PLAT_EXISTS ? ERROR_ALREADY_EXISTS : win32_error(plat_error()));
        RET(0, 2);
    }
    RET(1, 2);
}

static void sh_RemoveDirectoryA(Guest* g)
{
    char host[1400];
    if (!host_of(ARG(0), host, sizeof host))
        RET(0, 1);
    if (!plat_rmdir(host))
    {
        fail_plat();
        RET(0, 1);
    }
    RET(1, 1);
}

static void sh_MoveFileA(Guest* g)
{
    char a[1400], b[1400];
    if (!host_of(ARG(0), a, sizeof a) || !host_of(ARG(1), b, sizeof b))
        RET(0, 2);
    if (!plat_rename(a, b))
    {
        fail_plat();
        RET(0, 2);
    }
    RET(1, 2);
}

static uint32_t attributes(const PlatStat* st)
{
    uint32_t a = st->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (st->readonly)
        a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

static void sh_GetFileAttributesA(Guest* g)
{
    char host[1400];
    PlatStat st;
    if (!host_of(ARG(0), host, sizeof host))
        RET(0xFFFFFFFFu, 1);
    if (!plat_stat(host, &st))
    {
        fail_plat();
        RET(0xFFFFFFFFu, 1);
    }
    RET(attributes(&st), 1);
}

static void sh_SetFileAttributesA(Guest* g) { RET(1, 2); }

/* FindFirstFileA / FindNextFileA: the directory is listed once, filtered by the wildcard, and
 * handed out one WIN32_FIND_DATAA (320 bytes) at a time. "." and ".." are listed for a
 * non-root directory, as Windows does. */
typedef struct Find
{
    char dir_host[1400];
    char** names;
    unsigned n, next;
} Find;

static void find_close(void* p)
{
    Find* f = (Find*)p;
    for (unsigned i = 0; i < f->n; ++i)
        free(f->names[i]);
    free(f->names);
    free(f);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int wild(const char* pat, const char* s)
{
    if (!strcmp(pat, "*.*"))
        return 1;
    for (; *pat; ++pat, ++s)
    {
        if (*pat == '*')
        {
            while (pat[1] == '*')
                pat++;
            if (!pat[1])
                return 1;
            for (; *s; ++s)
                if (wild(pat + 1, s))
                    return 1;
            return wild(pat + 1, s);
        }
        if (!*s || (*pat != '?' && lower((unsigned char)*pat) != lower((unsigned char)*s)))
            return *pat == '.' && !*s && !pat[1]; /* "name." matches "name" */
    }
    return !*s;
}

static int find_fill(Find* f, uint32_t out)
{
    while (f->next < f->n)
    {
        const char* name = f->names[f->next++];
        char host[1700];
        PlatStat st;
        snprintf(host, sizeof host, "%s%c%s", f->dir_host, plat_path_sep, name);
        if (!strcmp(name, ".") || !strcmp(name, ".."))
        {
            memset(&st, 0, sizeof st);
            st.is_dir = 1;
        }
        else if (!plat_stat(host, &st))
            continue;
        memset(GUEST_PTR(out), 0, 320);
        wr32(out, attributes(&st));
        wr64(out + 4, ms_to_ft(st.ctime_ms));
        wr64(out + 12, ms_to_ft(st.atime_ms));
        wr64(out + 20, ms_to_ft(st.mtime_ms));
        wr32(out + 28, (uint32_t)(st.size >> 32));
        wr32(out + 32, (uint32_t)st.size);
        to_guest_name(name, (char*)GUEST_PTR(out + 44), 260);
        return 1;
    }
    return 0;
}

static void sh_FindFirstFileA(Guest* g)
{
    char full[1024];
    if (!vfs_full_path(ARGS(0), full, sizeof full))
    {
        gt_set_error(ERROR_PATH_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    char* slash = strrchr(full, '\\');
    char pattern[300];
    snprintf(pattern, sizeof pattern, "%s", slash + 1);
    char ov_host[1400];
    if (!strchr(pattern, '*') && !strchr(pattern, '?') && vfs_overlay_path(full, ov_host, sizeof ov_host))
    {
        /* one file a DAT overlay supplies (its folder may not exist in the install: ROM255\) */
        Find* f = (Find*)calloc(1, sizeof *f);
        char* sep = strrchr(ov_host, plat_path_sep);
        *sep = 0;
        snprintf(f->dir_host, sizeof f->dir_host, "%s", ov_host);
        f->names = (char**)malloc(sizeof *f->names);
        f->names[f->n++] = strdup(sep + 1);
        if (find_fill(f, ARG(1)))
            RET(k_new(K_FIND, f, find_close), 2);
        find_close(f);
    }
    if (slash == full + 2)
        slash[1] = 0; /* the root keeps its backslash */
    else
        *slash = 0;
    Find* f = (Find*)calloc(1, sizeof *f);
    if (!vfs_host_path(full, f->dir_host, sizeof f->dir_host))
    {
        free(f);
        gt_set_error(ERROR_PATH_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    int wildcard = strchr(pattern, '*') || strchr(pattern, '?');
    PlatDir* d = plat_dir_open(f->dir_host);
    if (!d)
    {
        free(f);
        trace_miss("FindFirstFileA", ARGS(0));
        gt_set_error(ERROR_PATH_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    unsigned cap = 0;
    if (wildcard && strlen(full) > 3)
        for (int k = 0; k < 2; ++k)
            if (wild(pattern, k ? ".." : "."))
            {
                if (f->n == cap)
                    f->names = (char**)realloc(f->names, (cap = cap ? cap * 2 : 64) * sizeof *f->names);
                f->names[f->n++] = strdup(k ? ".." : ".");
            }
    for (const char* e; (e = plat_dir_next(d));)
    {
        char gname[300];
        to_guest_name(e, gname, sizeof gname);
        if (!wild(pattern, gname)) /* without wildcards: a case-insensitive name match */
            continue;
        if (f->n == cap)
            f->names = (char**)realloc(f->names, (cap = cap ? cap * 2 : 64) * sizeof *f->names);
        f->names[f->n++] = strdup(e);
    }
    plat_dir_close(d);
    if (!find_fill(f, ARG(1)))
    {
        find_close(f);
        trace_miss("FindFirstFileA", ARGS(0));
        gt_set_error(ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE, 2);
    }
    RET(k_new(K_FIND, f, find_close), 2);
}

static void sh_FindNextFileA(Guest* g)
{
    Find* f = (Find*)k_data(ARG(0), K_FIND);
    if (!f)
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0, 2);
    }
    if (!find_fill(f, ARG(1)))
    {
        gt_set_error(ERROR_NO_MORE_FILES);
        RET(0, 2);
    }
    RET(1, 2);
}

static void sh_FindClose(Guest* g) { RET(k_close(ARG(0)), 1); }

static uint32_t copy_out(const char* s, uint32_t buf, uint32_t n)
{
    uint32_t len = (uint32_t)strlen(s);
    if (len + 1 > n)
        return len + 1; /* too small: the size needed, including the terminator */
    memcpy(GUEST_PTR(buf), s, len + 1);
    return len;
}

static void sh_GetFullPathNameA(Guest* g)
{
    char full[1024];
    uint32_t n = ARG(1), buf = ARG(2), part = ARG(3);
    if (!vfs_full_path(ARGS(0), full, sizeof full))
    {
        gt_set_error(ERROR_PATH_NOT_FOUND);
        RET(0, 4);
    }
    uint32_t r = copy_out(full, buf, n);
    if (part && r < n)
    {
        const char* s = strrchr(full, '\\');
        wr32(part, s && s[1] ? buf + (uint32_t)(s + 1 - full) : 0);
    }
    RET(r, 4);
}

static void sh_GetCurrentDirectoryA(Guest* g) { RET(copy_out(vfs_cwd(), ARG(1), ARG(0)), 2); }

static void sh_SetCurrentDirectoryA(Guest* g)
{
    if (!vfs_set_cwd(ARGS(0)))
    {
        gt_set_error(ERROR_PATH_NOT_FOUND);
        RET(0, 1);
    }
    RET(1, 1);
}

/* Long and short names are the same here: 8.3 names do not exist on the hosts. */
static void sh_GetLongPathNameA(Guest* g) { RET(copy_out(ARGS(0), ARG(1), ARG(2)), 3); }

/* --- synchronisation --------------------------------------------------------------------------- */
typedef struct NamedObject
{
    char name[128];
    uint32_t handle;
} NamedObject;
static NamedObject g_named[64];

/* A named object created twice returns the first (and ERROR_ALREADY_EXISTS): FFXi.dll's
 * single-instance mutex relies on this. */
static uint32_t named(uint32_t name_arg, uint32_t (*create)(void))
{
    const char* name = name_arg ? (const char*)GUEST_PTR(name_arg) : NULL;
    if (name && *name)
        for (unsigned i = 0; i < 64; ++i)
            if (g_named[i].handle && !strcmp(g_named[i].name, name))
            {
                gt_set_error(ERROR_ALREADY_EXISTS);
                return k_duplicate(g_named[i].handle);
            }
    uint32_t h = create();
    if (name && *name)
        for (unsigned i = 0; i < 64; ++i)
            if (!g_named[i].handle)
            {
                snprintf(g_named[i].name, sizeof g_named[i].name, "%s", name);
                g_named[i].handle = k_duplicate(h);
                break;
            }
    gt_set_error(0);
    return h;
}

static uint32_t g_arg_a, g_arg_b; /* creation parameters handed to the create callbacks */
static uint32_t make_event(void) { return k_event((int)g_arg_a, (int)g_arg_b); }
static uint32_t make_mutex(void) { return k_mutex((int)g_arg_a); }
static uint32_t make_semaphore(void) { return k_semaphore((int32_t)g_arg_a, (int32_t)g_arg_b); }

static void sh_CreateEventA(Guest* g) { g_arg_a = ARG(1); g_arg_b = ARG(2); RET(named(ARG(3), make_event), 4); }
static void sh_SetEvent(Guest* g) { RET(k_event_set(ARG(0), 1), 1); }
static void sh_ResetEvent(Guest* g) { RET(k_event_set(ARG(0), 0), 1); }
static void sh_CreateMutexA(Guest* g) { g_arg_a = ARG(1); RET(named(ARG(2), make_mutex), 3); }
static void sh_ReleaseMutex(Guest* g) { RET(k_mutex_release(ARG(0)), 1); }
static void sh_CreateSemaphoreA(Guest* g) { g_arg_a = ARG(1); g_arg_b = ARG(2); RET(named(ARG(3), make_semaphore), 4); }

static void sh_ReleaseSemaphore(Guest* g)
{
    int32_t prev = 0;
    int ok = k_semaphore_release(ARG(0), (int32_t)ARG(1), &prev);
    if (ok && ARG(2))
        wr32(ARG(2), (uint32_t)prev);
    RET(ok, 3);
}

static void sh_WaitForSingleObject(Guest* g)
{
    uint32_t h = ARG(0);
    RET(k_wait(&h, 1, 0, ARG(1)), 2);
}

static void sh_WaitForMultipleObjects(Guest* g)
{
    uint32_t n = ARG(0), hs[64];
    if (n > 64)
        RET(K_WAIT_FAILED, 4);
    for (uint32_t i = 0; i < n; ++i)
        hs[i] = rd32(ARG(1) + 4 * i);
    RET(k_wait(hs, n, ARG(2) != 0, ARG(3)), 4);
}

/* --- threads ----------------------------------------------------------------------------------- */
static void sh_CreateThread(Guest* g)
{
    uint32_t tid = 0;
    uint32_t h = k_thread_create(ARG(2), ARG(3), (ARG(4) & 4) != 0 /* CREATE_SUSPENDED */, &tid);
    if (ARG(5))
        wr32(ARG(5), tid);
    if (!h)
        gt_set_error(ERROR_NOT_ENOUGH_MEMORY);
    RET(h, 6);
}

static void sh_ResumeThread(Guest* g)
{
    uint32_t prev = 0;
    RET(k_thread_resume(ARG(0), &prev) ? prev : 0xFFFFFFFFu, 1);
}

static void sh_ExitThread(Guest* g) { k_thread_exit(ARG(0)); }

static void sh_TerminateThread(Guest* g)
{
    rt_log("[recomp] TerminateThread(%08x) is not supported: the thread keeps running\n", ARG(0));
    RET(0, 2);
}

static void sh_GetExitCodeThread(Guest* g)
{
    uint32_t code;
    if (!k_thread_exit_code(ARG(0), &code))
        RET(0, 2);
    wr32(ARG(1), code);
    RET(1, 2);
}

static void sh_SetThreadPriority(Guest* g) { RET(1, 2); }
static void sh_GetThreadPriority(Guest* g) { RET(0, 1); }
static void sh_SetPriorityClass(Guest* g) { RET(1, 2); }
static void sh_GetPriorityClass(Guest* g) { RET(0x20, 1); } /* NORMAL_PRIORITY_CLASS */

/* --- the wide and copying file calls ------------------------------------------------------------- */
/* CreateFileW: the path to code page 1252 (as every guest path is), then CreateFileA */
static void sh_CreateFileW(Guest* g)
{
    uint32_t w = ARG(0), n = 0;
    while (w && rd16(w + 2 * n))
        n++;
    uint32_t a = gheap_alloc(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint16_t c = rd16(w + 2 * i);
        wr8(a + i, c > 0xFF ? '?' : (uint8_t)c);
    }
    wr8(a + n, 0);
    wr32(g->esp + 4, a);
    sh_CreateFileA(g); /* pops the arguments */
    gheap_free(a);
}

/* CopyFileA(existing, new, bFailIfExists) */
static void sh_CopyFileA(Guest* g)
{
    char from[1400], to[1400];
    if (!host_of(ARG(0), from, sizeof from) || !host_of(ARG(1), to, sizeof to))
        RET(0, 3);
    gt_unlock();
    size_t size = 0;
    unsigned char* data = plat_read_file(from, &size);
    PlatFile* f = data ? plat_file_open(to, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE | (ARG(2) ? PLAT_EXCL : 0)) : NULL;
    int ok = f && plat_file_write(f, data, (uint32_t)size) == (int64_t)size;
    PlatError e = plat_error();
    if (f)
        plat_file_close(f);
    free(data);
    gt_lock();
    if (!ok)
        gt_set_error(data ? win32_error(e) : ERROR_FILE_NOT_FOUND);
    RET(ok, 3);
}

/* --- file mappings ------------------------------------------------------------------------------ *
 * A view of a file is a private copy of its bytes (the game maps files to read them); a mapping of
 * the paging file (hFile INVALID_HANDLE_VALUE) is one block of guest memory every view shares, and
 * named ones are found again by name. Writes to a file view are not written back. */
#define MAPPING_MAGIC 0x50414D46u

typedef struct Mapping
{
    uint32_t magic;
    PlatFile* file; /* borrowed: the file handle stays the guest's */
    uint32_t size, shared;
    char name[64];
} Mapping;

static Mapping* g_named_maps[16];

static void mapping_close(void* p)
{
    Mapping* m = (Mapping*)p;
    for (int i = 0; i < 16; ++i)
        if (g_named_maps[i] == m)
            g_named_maps[i] = NULL;
    if (m->shared)
        gwin_release(m->shared);
    free(m);
}

/* CreateFileMappingA(hFile, lpAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName) */
static void sh_CreateFileMappingA(Guest* g)
{
    const char* name = ARG(5) ? ARGS(5) : NULL;
    if (name)
        for (int i = 0; i < 16; ++i)
            if (g_named_maps[i] && !strcmp(g_named_maps[i]->name, name))
            {
                gt_set_error(ERROR_ALREADY_EXISTS);
                RET(k_new(K_OTHER, g_named_maps[i], NULL), 6); /* the first handle owns it */
            }
    Mapping* m = (Mapping*)calloc(1, sizeof *m);
    m->magic = MAPPING_MAGIC;
    m->size = ARG(4);
    if (ARG(0) != INVALID_HANDLE)
    {
        m->file = (PlatFile*)k_data(ARG(0), K_FILE);
        if (!m->file)
        {
            free(m);
            gt_set_error(ERROR_INVALID_HANDLE);
            RET(0, 6);
        }
        if (!m->size)
            m->size = (uint32_t)plat_file_size(m->file);
    }
    else if (!m->size || !(m->shared = gwin_alloc(m->size)))
    {
        gt_set_error(m->size ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
        free(m);
        RET(0, 6);
    }
    if (name)
    {
        snprintf(m->name, sizeof m->name, "%s", name);
        for (int i = 0; i < 16; ++i)
            if (!g_named_maps[i])
            {
                g_named_maps[i] = m;
                break;
            }
    }
    gt_set_error(0);
    RET(k_new(K_OTHER, m, mapping_close), 6);
}

static uint32_t g_views[64]; /* file views, released by UnmapViewOfFile */

/* MapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap) */
static void sh_MapViewOfFile(Guest* g)
{
    Mapping* m = (Mapping*)k_data(ARG(0), K_OTHER);
    uint32_t off = ARG(3), n = ARG(4);
    if (!m || m->magic != MAPPING_MAGIC || off > m->size)
    {
        gt_set_error(ERROR_INVALID_HANDLE);
        RET(0, 5);
    }
    if (!n)
        n = m->size - off;
    if (m->shared)
        RET(m->shared + off, 5);
    uint32_t v = gwin_alloc(n ? n : 1);
    if (!v)
    {
        gt_set_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0, 5);
    }
    gt_unlock();
    int64_t at = plat_file_seek(m->file, 0, 1);
    plat_file_seek(m->file, off, 0);
    plat_file_read(m->file, GUEST_PTR(v), n);
    plat_file_seek(m->file, at, 0);
    gt_lock();
    for (int i = 0; i < 64; ++i)
        if (!g_views[i])
        {
            g_views[i] = v;
            break;
        }
    RET(v, 5);
}

static void sh_UnmapViewOfFile(Guest* g)
{
    for (int i = 0; i < 64; ++i)
        if (g_views[i] == ARG(0))
        {
            gwin_release(ARG(0));
            g_views[i] = 0;
        }
    RET(1, 1); /* views of shared memory go with their mapping */
}

static void sh_GetDriveTypeA(Guest* g) { RET(3, 1); } /* DRIVE_FIXED */

static const ShimDef K32_IO[] = {
    { "kernel32.dll", "GetLocalTime", sh_GetLocalTime },
    { "kernel32.dll", "GetSystemTime", sh_GetSystemTime },
    { "kernel32.dll", "FileTimeToSystemTime", sh_FileTimeToSystemTime },
    { "kernel32.dll", "SystemTimeToFileTime", sh_SystemTimeToFileTime },
    { "kernel32.dll", "FileTimeToLocalFileTime", sh_FileTimeToLocalFileTime },
    { "kernel32.dll", "LocalFileTimeToFileTime", sh_LocalFileTimeToFileTime },
    { "kernel32.dll", "CompareFileTime", sh_CompareFileTime },
    { "kernel32.dll", "GetTimeZoneInformation", sh_GetTimeZoneInformation },
    { "kernel32.dll", "GlobalMemoryStatus", sh_GlobalMemoryStatus },
    { "kernel32.dll", "CreateFileA", sh_CreateFileA },
    { "kernel32.dll", "ReadFile", sh_ReadFile },
    { "kernel32.dll", "WriteFile", sh_WriteFile },
    { "kernel32.dll", "SetFilePointer", sh_SetFilePointer },
    { "kernel32.dll", "GetFileSize", sh_GetFileSize },
    { "kernel32.dll", "SetEndOfFile", sh_SetEndOfFile },
    { "kernel32.dll", "FlushFileBuffers", sh_FlushFileBuffers },
    { "kernel32.dll", "GetFileType", sh_GetFileType },
    { "kernel32.dll", "CloseHandle", sh_CloseHandle },
    { "kernel32.dll", "DuplicateHandle", sh_DuplicateHandle },
    { "kernel32.dll", "DeleteFileA", sh_DeleteFileA },
    { "kernel32.dll", "CreateDirectoryA", sh_CreateDirectoryA },
    { "kernel32.dll", "RemoveDirectoryA", sh_RemoveDirectoryA },
    { "kernel32.dll", "MoveFileA", sh_MoveFileA },
    { "kernel32.dll", "GetFileAttributesA", sh_GetFileAttributesA },
    { "kernel32.dll", "SetFileAttributesA", sh_SetFileAttributesA },
    { "kernel32.dll", "FindFirstFileA", sh_FindFirstFileA },
    { "kernel32.dll", "FindNextFileA", sh_FindNextFileA },
    { "kernel32.dll", "FindClose", sh_FindClose },
    { "kernel32.dll", "GetFullPathNameA", sh_GetFullPathNameA },
    { "kernel32.dll", "GetCurrentDirectoryA", sh_GetCurrentDirectoryA },
    { "kernel32.dll", "SetCurrentDirectoryA", sh_SetCurrentDirectoryA },
    { "kernel32.dll", "GetLongPathNameA", sh_GetLongPathNameA },
    { "kernel32.dll", "GetShortPathNameA", sh_GetLongPathNameA },
    { "kernel32.dll", "CreateEventA", sh_CreateEventA },
    { "kernel32.dll", "SetEvent", sh_SetEvent },
    { "kernel32.dll", "ResetEvent", sh_ResetEvent },
    { "kernel32.dll", "CreateMutexA", sh_CreateMutexA },
    { "kernel32.dll", "ReleaseMutex", sh_ReleaseMutex },
    { "kernel32.dll", "CreateSemaphoreA", sh_CreateSemaphoreA },
    { "kernel32.dll", "ReleaseSemaphore", sh_ReleaseSemaphore },
    { "kernel32.dll", "WaitForSingleObject", sh_WaitForSingleObject },
    { "kernel32.dll", "WaitForMultipleObjects", sh_WaitForMultipleObjects },
    { "kernel32.dll", "CreateThread", sh_CreateThread },
    { "kernel32.dll", "ResumeThread", sh_ResumeThread },
    { "kernel32.dll", "ExitThread", sh_ExitThread },
    { "kernel32.dll", "TerminateThread", sh_TerminateThread },
    { "kernel32.dll", "GetExitCodeThread", sh_GetExitCodeThread },
    { "kernel32.dll", "SetThreadPriority", sh_SetThreadPriority },
    { "kernel32.dll", "GetThreadPriority", sh_GetThreadPriority },
    { "kernel32.dll", "SetPriorityClass", sh_SetPriorityClass },
    { "kernel32.dll", "GetPriorityClass", sh_GetPriorityClass },
    { "kernel32.dll", "CreateFileW", sh_CreateFileW },
    { "kernel32.dll", "CopyFileA", sh_CopyFileA },
    { "kernel32.dll", "CreateFileMappingA", sh_CreateFileMappingA },
    { "kernel32.dll", "MapViewOfFile", sh_MapViewOfFile },
    { "kernel32.dll", "UnmapViewOfFile", sh_UnmapViewOfFile },
    { "kernel32.dll", "GetDriveTypeA", sh_GetDriveTypeA },
    { NULL, NULL, NULL },
};

void k32_io_init(void)
{
    thunk_register(K32_IO);
}
