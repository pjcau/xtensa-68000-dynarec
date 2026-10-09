/* PROF=1: a flat PC histogram of this process, sampled off ITIMER_PROF and
   symbolised afterwards by `prof.py`. The host is not the LX7, but which C
   function of the GBA hardware model or of the renderer takes the time is the
   same code. */
#ifndef GBAHOST_PROF_H
#define GBAHOST_PROF_H
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <sys/ucontext.h>   /* <ucontext.h> is gated on _XOPEN_SOURCE here */
#else
#include <ucontext.h>
#endif

#define PROF_N 65536
static struct { uintptr_t pc; unsigned n; } prof_tbl[PROF_N];
static unsigned prof_total;

static void prof_tick(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    uintptr_t pc;
#if defined(__APPLE__) && defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
    return;
#endif
    prof_total++;
    unsigned h = (unsigned)((pc >> 2) * 2654435761u) & (PROF_N - 1);
    for (int i = 0; i < 32; i++, h = (h + 1) & (PROF_N - 1))
        if (prof_tbl[h].pc == pc || !prof_tbl[h].n)
        {
            prof_tbl[h].pc = pc;
            prof_tbl[h].n++;
            return;
        }
}

static void prof_start(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = prof_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
}

int main(int c, char **v);

static void prof_dump(void)
{
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_PROF, &off, NULL);
    for (int i = 0; i < PROF_N; i++)
        if (prof_tbl[i].n)
            printf("PROF %p %u\n", (void *)prof_tbl[i].pc, prof_tbl[i].n);
    printf("PROF total %u\n", prof_total);
    printf("PROF main %p\n", (void *)(uintptr_t)&main);
}
#endif
