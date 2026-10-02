/* A sampling profiler for the game thread (Windows, UWP included): FFXI_SAMPLE=<file>.
 *
 * The frame profile (FFXI_PROFILE) splits a frame into the game's code and our API calls; this says
 * where inside them the time goes. Every millisecond a thread suspends the game thread, reads its
 * context and walks its stack with the unwinder (every frame of the host executable carries unwind
 * data: recompiled functions, runtime and CRT alike), and resumes it. Each sample is written as up to
 * SAMPLE_DEPTH return addresses, relative to the host executable's base; a frame outside it (a
 * system DLL, the driver) is written as 0xFFFFFFFF. tools/sample_report.py resolves them against the
 * linker's map file.
 *
 * File: "FXSAMPL1", then records { u32 count; u32 rva[count]; }. */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sampler.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define SAMPLE_DEPTH 24
#define OUTSIDE 0xFFFFFFFFu

extern IMAGE_DOS_HEADER __ImageBase; /* the executable this is linked into */

static HANDLE g_thread;
static FILE* g_out;

static int in_image(DWORD64 a, DWORD64 base, DWORD64 end) { return a >= base && a < end; }

static DWORD WINAPI sampler_thread(LPVOID unused)
{
    (void)unused;
    DWORD64 base = (DWORD64)&__ImageBase;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)&__ImageBase + __ImageBase.e_lfanew);
    DWORD64 end = base + nt->OptionalHeader.SizeOfImage;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    uint32_t rec[1 + SAMPLE_DEPTH];
    unsigned since_flush = 0;
    for (;;)
    {
        if (timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -10000; /* 1 ms */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        else
            Sleep(1);

        if (SuspendThread(g_thread) == (DWORD)-1)
            break; /* the game thread has gone */
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        uint32_t n = 0;
        if (GetThreadContext(g_thread, &c))
        {
            /* the unwinder's lookups take no lock the game thread can hold while running its code or ours */
            for (int depth = 0; depth < SAMPLE_DEPTH && c.Rip; ++depth)
            {
                rec[1 + n++] = in_image(c.Rip, base, end) ? (uint32_t)(c.Rip - base) : OUTSIDE;
                DWORD64 image = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, NULL);
                if (f)
                {
                    PVOID handler_data;
                    DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &handler_data, &frame, NULL);
                }
                else /* a leaf: the return address is on top of the stack */
                {
                    if (!c.Rsp)
                        break;
                    c.Rip = *(DWORD64*)c.Rsp;
                    c.Rsp += 8;
                }
            }
        }
        ResumeThread(g_thread);
        if (!n)
            continue;
        rec[0] = n;
        fwrite(rec, 4, 1 + n, g_out);
        if (++since_flush >= 1000)
            fflush(g_out), since_flush = 0;
    }
    fclose(g_out);
    return 0;
}

int sampler_start(const char* path)
{
    if (g_thread)
        return 1;
    g_out = fopen(path, "wb");
    if (!g_out)
        return 0;
    fwrite("FXSAMPL1", 1, 8, g_out);
    /* a real handle to the calling thread, for the sampler to suspend */
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    {
        fclose(g_out);
        return 0;
    }
    HANDLE t = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    if (!t)
        return 0;
    CloseHandle(t);
    return 1;
}
