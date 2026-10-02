/* The platform layer (R3): everything the portable runtime needs from the host OS, and nothing
 * else. plat_win.c implements it on Windows x64, where R3.1 is built and tested; plat_posix.c
 * will implement it on arm64 macOS.
 *
 * Shims for Win32 imports (k32.c and the rest) are written against this, never against a host OS
 * API, so the same shim code runs on both. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER)
#define RT_TLS __declspec(thread)
#else
#define RT_TLS _Thread_local
#endif

/* The host's path separator. */
extern const char plat_path_sep;

/* Address space: reserve (no access), commit (read/write) and decommit, page granular. */
void* plat_reserve(size_t size);
void* plat_reserve_at(void* at, size_t size);
int plat_commit(void* p, size_t size);
void plat_decommit(void* p, size_t size);

/* Threads. */
uint32_t plat_thread_id(void);
int plat_thread_start(void (*fn)(void*), void* arg);
void plat_sleep_ms(uint32_t ms);
void plat_yield(void);

/* Futex-style waiting: plat_wait32 returns once *addr != expected (or spuriously);
 * plat_wake_all32 wakes every waiter on addr. The guest lock and guest synchronisation objects
 * are built on these two. */
void plat_wait32(volatile uint32_t* addr, uint32_t expected);
void plat_wake_all32(volatile uint32_t* addr);

/* Atomics on 32-bit values. */
uint32_t plat_atomic_add32(volatile uint32_t* p, uint32_t v); /* returns the old value */
uint32_t plat_atomic_cas32(volatile uint32_t* p, uint32_t expected, uint32_t desired); /* returns the old value */

/* Time. rt_monotonic_ns (runtime.h) is also implemented here. */
uint64_t plat_wall_ms(void); /* milliseconds since 1970 */

/* plat_wait32 with a timeout in milliseconds (~0u: none). */
void plat_wait32_ms(volatile uint32_t* addr, uint32_t expected, uint32_t ms);

/* Files: the whole file into a malloc'd buffer (NULL if unreadable). */
unsigned char* plat_read_file(const char* path, size_t* size);

/* Files and directories, by host path (UTF-8). Failures set plat_error(). */
typedef enum PlatError
{
    PLAT_OK,
    PLAT_NOT_FOUND,      /* the file */
    PLAT_PATH_NOT_FOUND, /* a directory on the way */
    PLAT_ACCESS,
    PLAT_EXISTS,
    PLAT_NOT_EMPTY,
    PLAT_FAILED,
} PlatError;
PlatError plat_error(void);

typedef struct PlatFile PlatFile;
#define PLAT_READ 1
#define PLAT_WRITE 2
#define PLAT_CREATE 4    /* create if missing */
#define PLAT_EXCL 8      /* fail if it exists */
#define PLAT_TRUNCATE 16
PlatFile* plat_file_open(const char* path, int flags);
int64_t plat_file_read(PlatFile* f, void* buf, uint32_t n);        /* bytes, or -1 */
int64_t plat_file_write(PlatFile* f, const void* buf, uint32_t n);
int64_t plat_file_seek(PlatFile* f, int64_t offset, int whence);  /* 0 set, 1 current, 2 end; new position or -1 */
int64_t plat_file_size(PlatFile* f);
int plat_file_truncate(PlatFile* f);                                /* at the current position */
int plat_file_flush(PlatFile* f);
void plat_file_close(PlatFile* f);

typedef struct PlatStat
{
    int is_dir, readonly;
    uint64_t size;
    uint64_t ctime_ms, atime_ms, mtime_ms; /* since 1970 */
} PlatStat;
int plat_stat(const char* path, PlatStat* st);
int plat_mkdir(const char* path);
int plat_rmdir(const char* path);
int plat_unlink(const char* path);
int plat_rename(const char* from, const char* to);

/* Directory listing: entry names only ("." and ".." excluded). */
typedef struct PlatDir PlatDir;
PlatDir* plat_dir_open(const char* path);
const char* plat_dir_next(PlatDir* d); /* NULL at the end */
void plat_dir_close(PlatDir* d);

/* Local time. */
typedef struct PlatTime
{
    int year, month, day_of_week, day, hour, minute, second, ms; /* month 1-12, day_of_week 0 = Sunday */
} PlatTime;
void plat_split_time(uint64_t ms_since_1970, int local, PlatTime* t);
int32_t plat_utc_bias_minutes(void); /* UTC = local + bias, as Win32 counts it */

/* Memory, as a 32-bit guest may be told. */
void plat_memory(uint64_t* total, uint64_t* avail);

/* Diagnostics output (stderr and the log). */
void plat_debug(const char* text);
