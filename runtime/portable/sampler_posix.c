/* XIV_SAMPLE=<file>: a sampling profiler for the recompiled game. SIGPROF at ~1 kHz of CPU time;
 * each sample is the interrupted thread's pc, its link register and its Mach thread id, written as
 * binary records (uint64 pc, uint64 lr, uint32 tid) to <file> by a writer thread.
 * tools/xiv_sample.py turns that into a per-function report (translated functions are f_XXXXXXXX:
 * the guest address). `sample` cannot walk these stacks; this needs no stack walk. */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <fcntl.h>
#include <unistd.h>

#include "runtime.h"

typedef struct { uint64_t pc, lr; uint32_t tid, ms; uint64_t fr[6]; } Sample; /* ms: since sampling began */ /* fr: return addresses up the frame chain */

#define RING (1u << 20)
static Sample g_ring[RING];
static volatile uint32_t g_head, g_tail;
static uint64_t g_t0;

static void on_prof(int sig, siginfo_t* si, void* ctx)
{
    (void)sig, (void)si;
    struct __darwin_ucontext* uc = (struct __darwin_ucontext*)ctx;
    uint32_t h = __atomic_fetch_add(&g_head, 1, __ATOMIC_RELAXED);
    Sample* s = &g_ring[h & (RING - 1)];
    s->pc = uc->uc_mcontext->__ss.__pc;
    s->lr = uc->uc_mcontext->__ss.__lr;
    s->tid = pthread_mach_thread_np(pthread_self());
    s->ms = (uint32_t)((rt_monotonic_ns() - g_t0) / 1000000u);
    /* walk the frame-pointer chain (arm64: [fp] = caller fp, [fp + 8] = return address) within this stack */
    uint64_t fp = uc->uc_mcontext->__ss.__fp, sp = uc->uc_mcontext->__ss.__sp;
    for (int i = 0; i < 6; ++i)
    {
        if (fp < sp || fp - sp > (64u << 20) || (fp & 7))
        {
            s->fr[i] = 0;
            continue;
        }
        s->fr[i] = ((uint64_t*)fp)[1];
        uint64_t next = ((uint64_t*)fp)[0];
        sp = fp;
        fp = next;
    }
}

static void* writer(void* arg)
{
    FILE* f = (FILE*)arg;
    g_t0 = rt_monotonic_ns();
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
    for (;;)
    {
        usleep(200000);
        uint32_t h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
        while (g_tail != h)
        {
            fwrite(&g_ring[g_tail & (RING - 1)], sizeof(Sample), 1, f);
            g_tail++;
        }
        fflush(f);
    }
    return NULL;
}

void sampler_start(void)
{
    const char* path = getenv("XIV_SAMPLE");
    if (!path)
        return;
    FILE* f0 = fopen(path, "wb");
    if (!f0)
        return;
    /* far from the low descriptors the game's file handles reuse */
    int fd = fcntl(fileno(f0), F_DUPFD, 900);
    FILE* f = fd >= 0 ? fdopen(fd, "wb") : f0;
    if (f != f0)
        fclose(f0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, writer, f);
    extern int main(int, char**);
    rt_log("[sample] profiling to %s; main at %p\n", path, (void*)main);
}
