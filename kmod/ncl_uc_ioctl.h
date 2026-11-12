/* ioctl ABI shared by kmod/ncl_uc.c and src/mem.c. */
#ifndef NCL_UC_IOCTL_H
#define NCL_UC_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t __u64;
typedef int32_t __s32;
#endif

#define NCL_UC_DEV_PATH "/dev/ncl_uc"

/* Values of the x86 kernel's enum page_cache_mode (stable since 4.x). */
enum {
    NCL_UC_CM_WB = 0,
    NCL_UC_CM_WC = 1,
    NCL_UC_CM_UC_MINUS = 2,
    NCL_UC_CM_UC = 3,
    NCL_UC_CM_WT = 4,
    NCL_UC_CM_WP = 5,
    NCL_UC_CM_UNKNOWN = -1,
};

struct ncl_uc_info {
    __u64 size;         /* pool size in bytes */
    __s32 memtype;      /* NCL_UC_CM_* the pool was converted to */
    __s32 pat_enabled;  /* 0 => PAT off: cache-mode requests are not honored */
    __s32 nid;          /* NUMA node of every pool page; -1 = pages on several nodes */
    __s32 pad;
};

struct ncl_uc_probe {
    __u64 addr;         /* in: user virtual address in the caller's mm */
    __s32 cache_mode;   /* out: NCL_UC_CM_* decoded from the live PTE */
    __s32 pad;
};

#define NCL_UC_IOC_INFO _IOR('N', 1, struct ncl_uc_info)
#define NCL_UC_IOC_PROBE _IOWR('N', 2, struct ncl_uc_probe)
#define NCL_UC_IOC_ZERO _IOW('N', 3, __u64) /* zero first N bytes of pool */

#endif
