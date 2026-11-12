#define _GNU_SOURCE
#include "ncl/cpus.h"

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NCL_CPUS_MAX 4096

static int cpus[NCL_CPUS_MAX];
static int ncpus; /* 0 = not initialized */

static int add_cpu(int c)
{
    if (c < 0 || c >= NCL_CPUS_MAX || ncpus >= NCL_CPUS_MAX)
        return -1;
    for (int k = 0; k < ncpus; k++)
        if (cpus[k] == c)
            return -1; /* duplicates would silently share a CPU */
    cpus[ncpus++] = c;
    return 0;
}

int ncl_cpus_set(const char *list)
{
    ncpus = 0;
    const char *p = list;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            goto bad;
        if (*end == '-') {
            const char *q = end + 1;
            b = strtol(q, &end, 10);
            if (end == q || b < a)
                goto bad;
        }
        for (long c = a; c <= b; c++)
            if (add_cpu((int)c) < 0)
                goto bad;
        if (*end == ',')
            end++;
        else if (*end)
            goto bad;
        p = end;
    }
    if (!ncpus)
        goto bad;
    return 0;
bad:
    fprintf(stderr, "ncl: bad CPU list '%s' (want e.g. 0-59,120-179, no duplicates)\n", list);
    ncpus = 0;
    return -1;
}

void ncl_cpus_init(void)
{
    if (ncpus)
        return;
    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof cs, &cs) == 0) {
        for (int c = 0; c < CPU_SETSIZE && c < NCL_CPUS_MAX; c++)
            if (CPU_ISSET(c, &cs))
                cpus[ncpus++] = c;
    }
    if (!ncpus) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        for (int c = 0; c < n && c < NCL_CPUS_MAX; c++)
            cpus[ncpus++] = c;
    }
}

int ncl_cpus_count(void)
{
    ncl_cpus_init();
    return ncpus;
}

int ncl_cpu_slot(int k)
{
    ncl_cpus_init();
    return cpus[(k < 0 ? 0 : k) % ncpus];
}

void ncl_pin_self(int cpu)
{
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
}

int ncl_cpu_node(int cpu)
{
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", cpu);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int node = -1;
    struct dirent *e;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, "node", 4) && e->d_name[4] >= '0' && e->d_name[4] <= '9') {
            node = atoi(e->d_name + 4);
            break;
        }
    closedir(d);
    return node;
}

int ncl_smt_sibling(int cpu)
{
    char path[96], buf[64];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    for (char *p = buf; *p;) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            break;
        if (*end == '-')
            b = strtol(end + 1, &end, 10);
        for (long k = a; k <= b; k++)
            if (k != cpu)
                return (int)k;
        p = *end ? end + 1 : end;
    }
    return -1;
}

int ncl_cpus_check(int nslots, int nnodes, int threads_per_node, int sibling_servers)
{
    ncl_cpus_init();
    int warn = 0;
    if (nslots > ncpus) {
        fprintf(stderr, "ncl: WARNING: %d pinned threads on %d CPUs (%s): CPUs are shared\n",
                nslots, ncpus, ncl_cpus_describe());
        warn++;
    }
    unsigned long long seen = 0; /* NUMA nodes 0..63 */
    int nodes = 0;
    for (int k = 0; k < nslots + (sibling_servers ? nnodes : 0); k++) {
        int c = k < nslots ? ncl_cpu_slot(k) : ncl_smt_sibling(ncl_cpu_slot((k - nslots) * threads_per_node));
        int n = c < 0 ? -1 : ncl_cpu_node(c);
        if (n >= 0 && n < 64 && !(seen & (1ull << n))) {
            seen |= 1ull << n;
            nodes++;
        }
    }
    if (nodes > 1) {
        fprintf(stderr, "ncl: WARNING: pinned threads span %d NUMA nodes (mask %#llx); restrict "
                        "with --cpus or numactl --cpunodebind\n", nodes, seen);
        warn++;
    }
    return warn;
}

const char *ncl_cpus_describe(void)
{
    static char buf[512];
    ncl_cpus_init();
    size_t o = 0;
    buf[0] = 0;
    for (int k = 0; k < ncpus && o < sizeof buf - 24;) {
        int j = k;
        while (j + 1 < ncpus && cpus[j + 1] == cpus[j] + 1)
            j++;
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%d", o ? "," : "", cpus[k]);
        if (j > k)
            o += (size_t)snprintf(buf + o, sizeof buf - o, "-%d", cpus[j]);
        k = j + 1;
    }
    return buf;
}
