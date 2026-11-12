// SPDX-License-Identifier: GPL-2.0
/*
 * ncl_uc: expose a pool of RAM to user space with an uncached (or WC/WB)
 * memory type, for the "uc" backend of the NCLocks memory-access layer.
 *
 *   insmod ncl_uc.ko [size_mb=64] [memtype=uc|wc|wb] [nid=N] [dev_mode=0600]
 *   -> /dev/ncl_uc ; mmap(MAP_SHARED) offset 0..size
 *
 * Why set_pages_array_uc() and not just pgprot_noncached():
 *   On x86 with PAT, remap_pfn_range() of RAM pages goes through
 *   track_pfn_remap()/reserve_pfn_range(), which REPLACES the requested cache
 *   mode with the page's tracked memtype (WB for ordinary RAM). A driver that
 *   only sets pgprot_noncached() silently gets a WB mapping. Converting the
 *   pages' memtype (and kernel direct mapping) first makes the tracked type
 *   UC-, so the user mapping really is UC- (= UC on WB-MTRR RAM), and avoids
 *   conflicting-alias memory types between the direct map and user mapping.
 *
 * NCL_UC_IOC_PROBE decodes the cache bits of the caller's live PTE so user
 * space can verify the memory type rather than assume it.
 *
 * One opener at a time (an experiment = one process tree that inherits the
 * mapping across fork).
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/atomic.h>
#include <asm/set_memory.h>
#include <asm/memtype.h>
#include <asm/pgtable.h>

#include "ncl_uc_ioctl.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0) || LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
#error "ncl_uc: follow_pte()/vm_flags handling written for kernels 5.12..6.9; port before use"
#endif

static unsigned long size_mb = 64;
module_param(size_mb, ulong, 0444);
MODULE_PARM_DESC(size_mb, "Pool size in MiB (default 64)");

static char *memtype = "uc";
module_param(memtype, charp, 0444);
MODULE_PARM_DESC(memtype, "uc (default), wc, or wb (for comparison runs)");

static int nid = NUMA_NO_NODE;
module_param(nid, int, 0444);
MODULE_PARM_DESC(nid, "NUMA node to allocate the pool on (default: any, usually the loading CPU's); "
		 "every page is checked to be on it");

static ushort dev_mode = 0600;
module_param(dev_mode, ushort, 0444);
MODULE_PARM_DESC(dev_mode, "Permissions of /dev/ncl_uc (default 0600)");

static struct page **pages;
static unsigned long npages;
static int pool_cm; /* NCL_UC_CM_* */
static int pool_nid = NUMA_NO_NODE; /* node of every page, or NUMA_NO_NODE if mixed */
static atomic_t busy = ATOMIC_INIT(0);

static int decode_cache_mode(pte_t pte)
{
	unsigned long bits = pte_val(pte) & _PAGE_CACHE_MASK;
	int m;

	for (m = NCL_UC_CM_WB; m <= NCL_UC_CM_WP; m++)
		if ((cachemode2protval(m) & _PAGE_CACHE_MASK) == bits)
			return m;
	return NCL_UC_CM_UNKNOWN;
}

static int ncl_open(struct inode *inode, struct file *f)
{
	if (atomic_cmpxchg(&busy, 0, 1))
		return -EBUSY;
	return 0;
}

static int ncl_release(struct inode *inode, struct file *f)
{
	atomic_set(&busy, 0);
	return 0;
}

static int ncl_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long n = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long off = vma->vm_pgoff, i;
	int ret;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (off > npages || n > npages - off)
		return -EINVAL;

	if (pool_cm == NCL_UC_CM_UC_MINUS)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	else if (pool_cm == NCL_UC_CM_WC)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
#endif
	for (i = 0; i < n; i++) {
		ret = remap_pfn_range(vma, vma->vm_start + (i << PAGE_SHIFT),
				      page_to_pfn(pages[off + i]), PAGE_SIZE,
				      vma->vm_page_prot);
		if (ret)
			return ret;
	}
	return 0;
}

static long ncl_probe(struct ncl_uc_probe __user *up)
{
	struct ncl_uc_probe p;
	struct mm_struct *mm = current->mm;
	pte_t *ptep;
	spinlock_t *ptl;
	pte_t pte;
	int ret;

	if (copy_from_user(&p, up, sizeof(p)))
		return -EFAULT;
	mmap_read_lock(mm);
	ret = follow_pte(mm, p.addr, &ptep, &ptl);
	if (!ret) {
		pte = *ptep;
		pte_unmap_unlock(ptep, ptl);
	}
	mmap_read_unlock(mm);
	if (ret)
		return ret; /* not mapped / not faulted in */
	p.cache_mode = decode_cache_mode(pte);
	return copy_to_user(up, &p, sizeof(p)) ? -EFAULT : 0;
}

static long ncl_zero(u64 len)
{
	unsigned long i, n;

	if (len > (u64)npages << PAGE_SHIFT)
		return -EINVAL;
	n = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (i = 0; i < n; i++) {
		/* Direct map is UC too: plain stores go straight to memory. */
		memset(page_address(pages[i]), 0, PAGE_SIZE);
		cond_resched();
	}
	return 0;
}

static long ncl_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case NCL_UC_IOC_INFO: {
		struct ncl_uc_info info = {
			.size = (u64)npages << PAGE_SHIFT,
			.memtype = pool_cm,
			.pat_enabled = pat_enabled(),
			.nid = pool_nid,
		};
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
	}
	case NCL_UC_IOC_PROBE:
		return ncl_probe((struct ncl_uc_probe __user *)arg);
	case NCL_UC_IOC_ZERO: {
		u64 len;

		if (copy_from_user(&len, (void __user *)arg, sizeof(len)))
			return -EFAULT;
		return ncl_zero(len);
	}
	}
	return -ENOTTY;
}

static const struct file_operations ncl_fops = {
	.owner = THIS_MODULE,
	.open = ncl_open,
	.release = ncl_release,
	.mmap = ncl_mmap,
	.unlocked_ioctl = ncl_ioctl,
};

static struct miscdevice ncl_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ncl_uc",
	.fops = &ncl_fops,
};

static void free_pool(void)
{
	unsigned long i;

	if (!pages)
		return;
	if (pool_cm != NCL_UC_CM_WB)
		set_pages_array_wb(pages, npages);
	for (i = 0; i < npages; i++)
		if (pages[i])
			__free_page(pages[i]);
	kvfree(pages);
	pages = NULL;
}

static int __init ncl_init(void)
{
	unsigned long i;
	int ret;

	if (!strcmp(memtype, "uc"))
		pool_cm = NCL_UC_CM_UC_MINUS;
	else if (!strcmp(memtype, "wc"))
		pool_cm = NCL_UC_CM_WC;
	else if (!strcmp(memtype, "wb"))
		pool_cm = NCL_UC_CM_WB;
	else
		return -EINVAL;

	if (pool_cm != NCL_UC_CM_WB && !pat_enabled()) {
		pr_err("ncl_uc: PAT disabled; cannot provide memtype=%s\n", memtype);
		return -ENODEV;
	}

	npages = (size_mb << 20) >> PAGE_SHIFT;
	if (!npages || npages > INT_MAX)
		return -EINVAL;
	pages = kvcalloc(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	if (nid != NUMA_NO_NODE && (nid < 0 || nid >= MAX_NUMNODES || !node_online(nid))) {
		pr_err("ncl_uc: nid=%d is not an online NUMA node\n", nid);
		ret = -EINVAL;
		goto err_nowb;
	}
	for (i = 0; i < npages; i++) {
		/* __GFP_THISNODE: no fallback to another node. CXL memory onlined
		 * into ZONE_NORMAL is reachable with GFP_KERNEL; memory onlined
		 * as ZONE_MOVABLE is not, and the load fails here (-ENOMEM). */
		if (nid != NUMA_NO_NODE)
			pages[i] = alloc_pages_node(nid, GFP_KERNEL | __GFP_THISNODE | __GFP_ZERO, 0);
		else
			pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[i]) {
			if (nid != NUMA_NO_NODE)
				pr_err("ncl_uc: cannot allocate page %lu on node %d (ZONE_MOVABLE only, "
				       "or full?)\n", i, nid);
			ret = -ENOMEM;
			goto err_nowb;
		}
		if (i == 0)
			pool_nid = page_to_nid(pages[0]);
		else if (page_to_nid(pages[i]) != pool_nid)
			pool_nid = NUMA_NO_NODE;
	}
	if (nid != NUMA_NO_NODE && pool_nid != nid) {
		pr_err("ncl_uc: pool pages not all on node %d\n", nid);
		ret = -EIO;
		goto err_nowb;
	}

	if (pool_cm == NCL_UC_CM_UC_MINUS)
		ret = set_pages_array_uc(pages, npages);
	else if (pool_cm == NCL_UC_CM_WC)
		ret = set_pages_array_wc(pages, npages);
	else
		ret = 0;
	if (ret) {
		pr_err("ncl_uc: set_pages_array_%s failed: %d\n", memtype, ret);
		goto err_nowb;
	}

	ncl_misc.mode = dev_mode;
	ret = misc_register(&ncl_misc);
	if (ret)
		goto err;
	pr_info("ncl_uc: %lu MiB pool, memtype=%s, node=%d, /dev/ncl_uc mode %o\n", size_mb,
		memtype, pool_nid, dev_mode);
	return 0;

err_nowb:
	pool_cm = NCL_UC_CM_WB; /* nothing converted (or conversion failed) */
err:
	free_pool();
	return ret;
}

static void __exit ncl_exit(void)
{
	misc_deregister(&ncl_misc);
	free_pool();
}

module_init(ncl_init);
module_exit(ncl_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Uncached RAM pool for NCLocks non-coherent memory experiments");
