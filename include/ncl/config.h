/* Compile-time limits and common definitions. */
#ifndef NCL_CONFIG_H
#define NCL_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define NCL_CACHELINE 64

/* Max nodes sharing the device (design §1: target N <= 16, kept a parameter). */
#ifndef NCL_MAX_NODES
#define NCL_MAX_NODES 16
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

#endif
