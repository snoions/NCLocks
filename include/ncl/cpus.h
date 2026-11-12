/*
 * CPU placement for pinned threads. Slot k (worker k = node*T + idx; with -Y
 * next, node n's server is slot N*T + n) runs on cpus[k % ncpus], where cpus
 * is the list given with ncl_cpus_set (--cpus) or, by default, the CPUs this
 * process may run on, in increasing order. So `numactl --cpunodebind=0 ...`
 * keeps every pinned thread on node 0 without a list, and on a host without
 * restrictions the placement is the old one (CPU k % ncpu).
 *
 * Initialize in the parent (ncl_sys_create does it), before nodes fork and
 * before any thread pins itself: the default list is read from the calling
 * thread's affinity.
 */
#ifndef NCL_CPUS_H
#define NCL_CPUS_H

/* "0-59,120-179". Returns 0, or -1 with a message. */
int ncl_cpus_set(const char *list);
/* Idempotent; reads the default list if none was set. */
void ncl_cpus_init(void);
int ncl_cpus_count(void);
int ncl_cpu_slot(int k);
/* Pin the calling thread to one CPU. */
void ncl_pin_self(int cpu);
/* NUMA node of a CPU (sysfs), -1 if unknown. */
int ncl_cpu_node(int cpu);
/* First SMT sibling of `cpu` other than itself (sysfs), or -1. */
int ncl_smt_sibling(int cpu);
/* Warn on stderr if `nslots` pinned threads exceed the list (CPUs shared) or
 * the CPUs they use (plus `sibling_of_workers` SMT siblings of slot n*T for
 * n < nnodes) span more than one NUMA node. Returns the number of warnings. */
int ncl_cpus_check(int nslots, int nnodes, int threads_per_node, int sibling_servers);
/* "0-59,120-179" form of the list, for logs. */
const char *ncl_cpus_describe(void);

#endif
