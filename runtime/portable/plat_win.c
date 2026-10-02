/* The platform layer on Windows x64 (R3.1). See plat.h. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "plat.h"
#include "runtime.h"

const char plat_path_sep = '\\';

void* plat_reserve(size_t size)
{
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

int plat_commit(void* p, size_t size)
{
    return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void plat_decommit(void* p, size_t size)
{
    VirtualFree(p, size, MEM_DECOMMIT);
}

uint32_t plat_thread_id(void)
{
    return GetCurrentThreadId();
}

typedef struct ThreadStart
{
    void (*fn)(void*);
    void* arg;
} ThreadStart;

static DWORD WINAPI thread_entry(LPVOID p)
{
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int plat_thread_start(void (*fn)(void*), void* arg)
{
    ThreadStart* s = (ThreadStart*)malloc(sizeof *s);
    if (!s)
        return 0;
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_entry, s, 0, NULL);
    if (!h)
    {
        free(s);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Sleep rounds up to the system timer's tick: 15.6 ms unless something raised the resolution (SDL
 * does, on the desktop; the UWP build has no SDL). The game's frame limiter sleeps in short steps, so
 * at 15.6 ms a 33 ms frame became 47 (21 fps). A high-resolution waitable timer (Windows 10 1803+,
 * allowed in UWP apps) wakes on time without touching the system's resolution. One per thread. */
void plat_sleep_ms(uint32_t ms)
{
    static __declspec(thread) HANDLE timer;
    if (ms && ms != INFINITE)
    {
        if (!timer)
        {
            timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer)
                timer = INVALID_HANDLE_VALUE; /* older Windows: Sleep */
        }
        if (timer != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)ms * 10000; /* relative, in 100 ns units */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
                return;
        }
    }
    Sleep(ms);
}

void plat_yield(void)
{
    SwitchToThread();
}

void plat_wait32(volatile uint32_t* addr, uint32_t expected)
{
    WaitOnAddress((volatile VOID*)addr, &expected, sizeof expected, INFINITE);
}

void plat_wake_all32(volatile uint32_t* addr)
{
    WakeByAddressAll((PVOID)addr);
}

uint32_t plat_atomic_add32(volatile uint32_t* p, uint32_t v)
{
    return (uint32_t)InterlockedExchangeAdd((volatile LONG*)p, (LONG)v);
}

uint32_t plat_atomic_cas32(volatile uint32_t* p, uint32_t expected, uint32_t desired)
{
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)p, (LONG)desired, (LONG)expected);
}

uint64_t rt_monotonic_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

uint64_t plat_wall_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    return t / 10000 - 11644473600000ull;
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
    OutputDebugStringA(text);
}

void plat_wait32_ms(volatile uint32_t* addr, uint32_t expected, uint32_t ms)
{
    WaitOnAddress((volatile VOID*)addr, &expected, sizeof expected, ms == ~0u ? INFINITE : ms);
}

/* --- files ------------------------------------------------------------------------------------ */

static RT_TLS PlatError t_error;

PlatError plat_error(void)
{
    return t_error;
}

static void set_error_from_win32(void)
{
    switch (GetLastError())
    {
    case ERROR_FILE_NOT_FOUND: t_error = PLAT_NOT_FOUND; break;
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: t_error = PLAT_PATH_NOT_FOUND; break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: t_error = PLAT_ACCESS; break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: t_error = PLAT_EXISTS; break;
    case ERROR_DIR_NOT_EMPTY: t_error = PLAT_NOT_EMPTY; break;
    default: t_error = PLAT_FAILED; break;
    }
}

/* UTF-8 host path -> wide, in a caller buffer */
static const wchar_t* wide(const char* path, wchar_t* buf, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, buf, n))
        buf[0] = 0;
    return buf;
}

struct PlatFile
{
    HANDLE h;
};

PlatFile* plat_file_open(const char* path, int flags)
{
    wchar_t w[1024];
    DWORD access = ((flags & PLAT_READ) ? GENERIC_READ : 0) | ((flags & PLAT_WRITE) ? GENERIC_WRITE : 0);
    DWORD disp;
    if (flags & PLAT_EXCL)
        disp = CREATE_NEW;
    else if ((flags & PLAT_CREATE) && (flags & PLAT_TRUNCATE))
        disp = CREATE_ALWAYS;
    else if (flags & PLAT_CREATE)
        disp = OPEN_ALWAYS;
    else if (flags & PLAT_TRUNCATE)
        disp = TRUNCATE_EXISTING;
    else
        disp = OPEN_EXISTING;
    HANDLE h = CreateFileW(wide(path, w, 1024), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        set_error_from_win32();
        return NULL;
    }
    PlatFile* f = (PlatFile*)malloc(sizeof *f);
    f->h = h;
    t_error = PLAT_OK;
    return f;
}

int64_t plat_file_read(PlatFile* f, void* buf, uint32_t n)
{
    DWORD got = 0;
    if (!ReadFile(f->h, buf, n, &got, NULL))
    {
        set_error_from_win32();
        return -1;
    }
    return got;
}

int64_t plat_file_write(PlatFile* f, const void* buf, uint32_t n)
{
    DWORD put = 0;
    if (!WriteFile(f->h, buf, n, &put, NULL))
    {
        set_error_from_win32();
        return -1;
    }
    return put;
}

int64_t plat_file_seek(PlatFile* f, int64_t offset, int whence)
{
    LARGE_INTEGER d, now;
    d.QuadPart = offset;
    if (!SetFilePointerEx(f->h, d, &now, whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN))
    {
        set_error_from_win32();
        return -1;
    }
    return now.QuadPart;
}

int64_t plat_file_size(PlatFile* f)
{
    LARGE_INTEGER s;
    return GetFileSizeEx(f->h, &s) ? s.QuadPart : -1;
}

int plat_file_truncate(PlatFile* f)
{
    return SetEndOfFile(f->h) != 0;
}

int plat_file_flush(PlatFile* f)
{
    return FlushFileBuffers(f->h) != 0;
}

void plat_file_close(PlatFile* f)
{
    CloseHandle(f->h);
    free(f);
}

static uint64_t filetime_ms(FILETIME ft)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000 - 11644473600000ull;
}

int plat_stat(const char* path, PlatStat* st)
{
    wchar_t w[1024];
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(path, w, 1024), GetFileExInfoStandard, &a))
    {
        set_error_from_win32();
        return 0;
    }
    st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->readonly = (a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    st->size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    st->ctime_ms = filetime_ms(a.ftCreationTime);
    st->atime_ms = filetime_ms(a.ftLastAccessTime);
    st->mtime_ms = filetime_ms(a.ftLastWriteTime);
    return 1;
}

int plat_mkdir(const char* path)
{
    wchar_t w[1024];
    if (CreateDirectoryW(wide(path, w, 1024), NULL))
        return 1;
    set_error_from_win32();
    return 0;
}

int plat_rmdir(const char* path)
{
    wchar_t w[1024];
    if (RemoveDirectoryW(wide(path, w, 1024)))
        return 1;
    set_error_from_win32();
    return 0;
}

int plat_unlink(const char* path)
{
    wchar_t w[1024];
    if (DeleteFileW(wide(path, w, 1024)))
        return 1;
    set_error_from_win32();
    return 0;
}

int plat_rename(const char* from, const char* to)
{
    wchar_t a[1024], b[1024];
    if (MoveFileExW(wide(from, a, 1024), wide(to, b, 1024), MOVEFILE_REPLACE_EXISTING))
        return 1;
    set_error_from_win32();
    return 0;
}

struct PlatDir
{
    HANDLE h;
    int first;
    WIN32_FIND_DATAW data;
    char name[MAX_PATH * 3];
};

PlatDir* plat_dir_open(const char* path)
{
    char pattern[1100];
    wchar_t w[1100];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    PlatDir* d = (PlatDir*)calloc(1, sizeof *d);
    d->h = FindFirstFileW(wide(pattern, w, 1100), &d->data);
    if (d->h == INVALID_HANDLE_VALUE)
    {
        set_error_from_win32();
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

const char* plat_dir_next(PlatDir* d)
{
    for (;;)
    {
        if (!d->first && !FindNextFileW(d->h, &d->data))
            return NULL;
        d->first = 0;
        if (!wcscmp(d->data.cFileName, L".") || !wcscmp(d->data.cFileName, L".."))
            continue;
        WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, d->name, sizeof d->name, NULL, NULL);
        return d->name;
    }
}

void plat_dir_close(PlatDir* d)
{
    FindClose(d->h);
    free(d);
}

/* --- time and memory --------------------------------------------------------------------------- */

void plat_split_time(uint64_t ms, int local, PlatTime* t)
{
    uint64_t ft = (ms + 11644473600000ull) * 10000;
    FILETIME f, lf;
    SYSTEMTIME s;
    f.dwLowDateTime = (DWORD)ft;
    f.dwHighDateTime = (DWORD)(ft >> 32);
    if (local)
    {
        FileTimeToLocalFileTime(&f, &lf);
        f = lf;
    }
    FileTimeToSystemTime(&f, &s);
    t->year = s.wYear;
    t->month = s.wMonth;
    t->day_of_week = s.wDayOfWeek;
    t->day = s.wDay;
    t->hour = s.wHour;
    t->minute = s.wMinute;
    t->second = s.wSecond;
    t->ms = s.wMilliseconds;
}

int32_t plat_utc_bias_minutes(void)
{
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    return tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : r == TIME_ZONE_ID_STANDARD ? tz.StandardBias : 0);
}

void plat_memory(uint64_t* total, uint64_t* avail)
{
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    *total = m.ullTotalPhys;
    *avail = m.ullAvailPhys;
}
