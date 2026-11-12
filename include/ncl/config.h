/* Compile-time limits and common definitions. */
#ifndef NCL_CONFIG_H
#define NCL_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define NCL_CACHELINE 64

/* Max nodes sharing the device (design §1: target N <= 16, kept a parameter;
 * 64 for runs on large hosts). */
#ifndef NCL_MAX_NODES
#define NCL_MAX_NODES 64
#endif

/* Max participants in a global algorithm. Normally == nnodes; in the
 * no-submutex ablation it is the total thread count (design §6.4). */
#ifndef NCL_MAX_PARTICIPANTS
#define NCL_MAX_PARTICIPANTS 256
#endif

#define NCL_ALIGNED(n) __attribute__((aligned(n)))
#define NCL_LIKELY(x) __builtin_expect(!!(x), 1)
#define NCL_UNLIKELY(x) __builtin_expect(!!(x), 0)

static inline void ncl_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* Spin-then-yield for waits that depend on ANOTHER thread making progress
 * (submutex handoff, lock-server grants). Pure spinning starves that thread
 * when CPUs are oversubscribed (e.g. a server thread needed for every grant
 * gets a CFS timeslice only every few ms). sched_yield() returns immediately
 * when nothing else is runnable. */
#ifndef NCL_SPIN_BEFORE_YIELD
#define NCL_SPIN_BEFORE_YIELD 1024 /* pause iterations, ~40 us on recent x86 */
#endif

#include <sched.h>
static inline void ncl_spin_wait(unsigned *n)
{
    if (*n < NCL_SPIN_BEFORE_YIELD) {
        (*n)++;
        ncl_cpu_relax();
    } else {
        sched_yield();
    }
}

#endif
