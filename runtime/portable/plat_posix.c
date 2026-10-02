/* The platform layer on POSIX hosts (R3.1): arm64 macOS first, Linux as a by-product. See plat.h.
 *
 * Waiting on an address: macOS 14.4+ has os_sync_wait_on_address (the supported form of the
 * __ulock calls libc++ uses); Linux has futex. Address space: one PROT_NONE reservation; commit
 * is mprotect, decommit maps fresh zero pages over the range, as Win32's decommit + commit gives
 * zeroed memory back. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#endif

#include "plat.h"
#include "runtime.h"

const char plat_path_sep = '/';

void* plat_reserve(size_t size)
{
    void* p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* at exactly `at`, or NULL: a hint, never MAP_FIXED (which would replace whatever is mapped there) */
void* plat_reserve_at(void* at, size_t size)
{
    void* p = mmap(at, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (p != at)
    {
        munmap(p, size);
        return NULL;
    }
    return p;
}

int plat_commit(void* p, size_t size)
{
    return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
}

void plat_decommit(void* p, size_t size)
{
    mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

/* --- threads ------------------------------------------------------------------------------------- */
uint32_t plat_thread_id(void)
{
#if defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (uint32_t)id;
#else
    return (uint32_t)syscall(SYS_gettid);
#endif
}

typedef struct ThreadStart
{
    void (*fn)(void*);
    void* arg;
} ThreadStart;

static void* thread_entry(void* p)
{
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int plat_thread_start(void (*fn)(void*), void* arg)
{
    ThreadStart* s = (ThreadStart*)malloc(sizeof *s);
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 8u << 20); /* translated frames are large; Windows gives 1 MB+ and grows */
#if defined(__APPLE__)
    /* the game's threads are its frame: keep them on the performance cores (the default QoS lets
     * the scheduler put them on the efficiency cores, a fraction of the speed) */
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    pthread_t t;
    int r = pthread_create(&t, &a, thread_entry, s);
    pthread_attr_destroy(&a);
    if (r)
    {
        free(s);
        return 0;
    }
    return 1;
}

void plat_sleep_ms(uint32_t ms)
{
    if (ms == ~0u)
    {
        for (;;)
            pause();
    }
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
    {
    }
}

void plat_yield(void)
{
    sched_yield();
}

/* --- waiting on an address ------------------------------------------------------------------------ */
void plat_wait32(volatile uint32_t* addr, uint32_t expected)
{
#if defined(__APPLE__)
    os_sync_wait_on_address((void*)addr, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
#else
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
#endif
}

void plat_wait32_ms(volatile uint32_t* addr, uint32_t expected, uint32_t ms)
{
    if (ms == ~0u)
    {
        plat_wait32(addr, expected);
        return;
    }
#if defined(__APPLE__)
    os_sync_wait_on_address_with_timeout((void*)addr, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
        (uint64_t)ms * 1000000u);
#else
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, &t, NULL, 0);
#endif
}

void plat_wake_all32(volatile uint32_t* addr)
{
#if defined(__APPLE__)
    os_sync_wake_by_address_all((void*)addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
#else
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, NULL, NULL, 0);
#endif
}

/* --- atomics and time ------------------------------------------------------------------------------- */
uint32_t plat_atomic_add32(volatile uint32_t* p, uint32_t v)
{
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

uint32_t plat_atomic_cas32(volatile uint32_t* p, uint32_t expected, uint32_t desired)
{
    __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected; /* the old value either way */
}

uint64_t rt_monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t plat_wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

unsigned char* plat_read_file(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *size = (size_t)n;
    return buf;
}

void plat_debug(const char* text)
{
    fputs(text, stderr);
}

/* --- files ---------------------------------------------------------------------------------------- */
static RT_TLS PlatError t_error;

PlatError plat_error(void)
{
    return t_error;
}

/* errno to PlatError; ENOENT is "file" or "path" depending on whether the parent exists */
static void set_error(const char* path)
{
    switch (errno)
    {
    case ENOENT:
    {
        t_error = PLAT_NOT_FOUND;
        if (path)
        {
            char parent[1400];
            snprintf(parent, sizeof parent, "%s", path);
            char* s = strrchr(parent, '/');
            struct stat st;
            if (s && s != parent)
            {
                *s = 0;
                if (stat(parent, &st) != 0 || !S_ISDIR(st.st_mode))
                    t_error = PLAT_PATH_NOT_FOUND;
            }
        }
        break;
    }
    case ENOTDIR:
    case ENAMETOOLONG: t_error = PLAT_PATH_NOT_FOUND; break;
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR: t_error = PLAT_ACCESS; break;
    case EEXIST: t_error = PLAT_EXISTS; break;
    case ENOTEMPTY: t_error = PLAT_NOT_EMPTY; break;
    default: t_error = PLAT_FAILED; break;
    }
}

struct PlatFile
{
    int fd;
};

PlatFile* plat_file_open(const char* path, int flags)
{
    int rw = (flags & PLAT_READ) && (flags & PLAT_WRITE) ? O_RDWR : (flags & PLAT_WRITE) ? O_WRONLY : O_RDONLY;
    int o = rw | ((flags & PLAT_CREATE) ? O_CREAT : 0) | ((flags & PLAT_EXCL) ? O_CREAT | O_EXCL : 0) |
        ((flags & PLAT_TRUNCATE) ? O_TRUNC : 0);
    int fd = open(path, o | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        set_error(path);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) /* CreateFile does not open directories */
    {
        close(fd);
        t_error = PLAT_ACCESS;
        return NULL;
    }
    PlatFile* f = (PlatFile*)malloc(sizeof *f);
    f->fd = fd;
    t_error = PLAT_OK;
    return f;
}

int64_t plat_file_read(PlatFile* f, void* buf, uint32_t n)
{
    ssize_t r;
    do
        r = read(f->fd, buf, n);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        set_error(NULL);
    return r;
}

int64_t plat_file_write(PlatFile* f, const void* buf, uint32_t n)
{
    ssize_t r;
    do
        r = write(f->fd, buf, n);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        set_error(NULL);
    return r;
}

int64_t plat_file_seek(PlatFile* f, int64_t offset, int whence)
{
    off_t r = lseek(f->fd, (off_t)offset, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    if (r < 0)
        set_error(NULL);
    return r;
}

int64_t plat_file_size(PlatFile* f)
{
    struct stat st;
    return fstat(f->fd, &st) == 0 ? (int64_t)st.st_size : -1;
}

int plat_file_truncate(PlatFile* f)
{
    off_t at = lseek(f->fd, 0, SEEK_CUR);
    return at >= 0 && ftruncate(f->fd, at) == 0;
}

int plat_file_flush(PlatFile* f)
{
    return fsync(f->fd) == 0;
}

void plat_file_close(PlatFile* f)
{
    close(f->fd);
    free(f);
}

static uint64_t ts_ms(struct timespec t) { return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }

int plat_stat(const char* path, PlatStat* st)
{
    struct stat s;
    if (stat(path, &s) != 0)
    {
        set_error(path);
        return 0;
    }
    st->is_dir = S_ISDIR(s.st_mode);
    st->readonly = access(path, W_OK) != 0;
    st->size = (uint64_t)s.st_size;
#if defined(__APPLE__)
    st->ctime_ms = ts_ms(s.st_birthtimespec);
    st->atime_ms = ts_ms(s.st_atimespec);
    st->mtime_ms = ts_ms(s.st_mtimespec);
#else
    st->ctime_ms = ts_ms(s.st_ctim);
    st->atime_ms = ts_ms(s.st_atim);
    st->mtime_ms = ts_ms(s.st_mtim);
#endif
    return 1;
}

int plat_mkdir(const char* path)
{
    if (mkdir(path, 0755) == 0)
        return 1;
    set_error(path);
    return 0;
}

int plat_rmdir(const char* path)
{
    if (rmdir(path) == 0)
        return 1;
    set_error(path);
    return 0;
}

int plat_unlink(const char* path)
{
    if (unlink(path) == 0)
        return 1;
    set_error(path);
    return 0;
}

int plat_rename(const char* from, const char* to)
{
    if (rename(from, to) == 0)
        return 1;
    set_error(from);
    return 0;
}

struct PlatDir
{
    DIR* d;
};

PlatDir* plat_dir_open(const char* path)
{
    DIR* d = opendir(path);
    if (!d)
    {
        set_error(path);
        return NULL;
    }
    PlatDir* p = (PlatDir*)malloc(sizeof *p);
    p->d = d;
    return p;
}

const char* plat_dir_next(PlatDir* d)
{
    for (struct dirent* e; (e = readdir(d->d));)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            return e->d_name;
    return NULL;
}

void plat_dir_close(PlatDir* d)
{
    closedir(d->d);
    free(d);
}

/* --- time and memory ------------------------------------------------------------------------------- */
void plat_split_time(uint64_t ms, int local, PlatTime* t)
{
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    if (local)
        localtime_r(&s, &tm);
    else
        gmtime_r(&s, &tm);
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day_of_week = tm.tm_wday;
    t->day = tm.tm_mday;
    t->hour = tm.tm_hour;
    t->minute = tm.tm_min;
    t->second = tm.tm_sec;
    t->ms = (int)(ms % 1000);
}

int32_t plat_utc_bias_minutes(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return (int32_t)(-tm.tm_gmtoff / 60); /* Win32: UTC = local + bias */
}

void plat_memory(uint64_t* total, uint64_t* avail)
{
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    *total = mem;
    *avail = mem / 2; /* the guest only uses this for a "memory load" figure */
#else
    struct sysinfo si;
    sysinfo(&si);
    *total = (uint64_t)si.totalram * si.mem_unit;
    *avail = (uint64_t)si.freeram * si.mem_unit;
#endif
}
