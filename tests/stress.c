/*
 * Runtime stress test (design §5).
 *
 * One process per emulated node (scripts/ncl_run.sh ncl_stress [options];
 * bench/harness.h); each runs T threads that repeatedly acquire a random lock. Every lock guards {counter, checksum, words[W]} in
 * the cached data region. Checks:
 *   - mutual exclusion: an uncached occupant word per lock must be 0 on entry
 *     and still ours on exit;
 *   - data visibility: the checksum must match the data on entry (catches a
 *     stale/torn view, e.g. a missing invalidate);
 *   - no lost updates: sum of counters == total acquisitions (catches a
 *     missing writeback, or a whole-snapshot stale read).
 * Random delays are injected inside and outside critical sections.
 *
 * With --backend emu, --skip-wb / --skip-inv must make this test FAIL;
 * that is the check that the emulator actually models non-coherence.
 * Node 0 gathers the per-node results, checks the final data and prints the
 * verdict; --timeout is enforced by the launcher.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../bench/harness.h"
#include "ncl/lock.h"
#include "ncl/cpus.h"

struct lockdata {
    uint64_t counter;
    uint64_t checksum;
    uint64_t words[];
};

static int nnodes = 4, nthreads = 2, nlocks = 4, nwords = 16, pin = 0, precise = 0;
static long iters = 100000;
static int timeout_s = 0;
static unsigned reset_delay_ns = 0;
static unsigned grant_delay_ns = 0;
static unsigned lamport_delay_ns = 0, mode_delay_ns = 0, bakery_delay_ns = 0, retain_delay_ns = 0;
static unsigned max_delay_ns = 200;
static size_t stride;
static uint64_t delay_cycles;

/* User lines [0..nlocks): occupant of each lock. */
static ncl_line_t *occ_line(int l) { return ncl_user_line((size_t)l); }

struct node_result {
    uint64_t acq, viol, cksum_err;
};

static struct lockdata *ld(int l) { return ncl_data_ptr((ncl_off_t)l * stride); }

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static uint64_t checksum(const struct lockdata *d)
{
    uint64_t h = mix(0x1234, d->counter);
    for (int i = 0; i < nwords; i++)
        h = mix(h, d->words[i]);
    return h;
}

static uint64_t xorshift(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static void rand_delay(uint64_t *rng)
{
    if (delay_cycles)
        ncl_lat_spin_cycles(xorshift(rng) % delay_cycles);
}

struct worker {
    pthread_t tid;
    int node, idx;
    uint64_t acq, viol, cksum_err;
};

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct ncl_thread t;
    ncl_thread_init(&t, w->idx);
    /* splitmix64-scrambled seed: raw seeds differing in a few bits give
     * correlated xorshift streams. */
    uint64_t rng = (((uint64_t)w->node << 32) | (uint64_t)w->idx) + 0x9e3779b97f4a7c15ull;
    rng = (rng ^ (rng >> 30)) * 0xbf58476d1ce4e5b9ull;
    rng = (rng ^ (rng >> 27)) * 0x94d049bb133111ebull;
    rng = (rng ^ (rng >> 31)) | 1;
    uint64_t tag = (uint64_t)w->node * (uint64_t)nthreads + (uint64_t)w->idx + 1;

    if (pin) {
        ncl_pin_self(ncl_cpu_slot(w->node * nthreads + w->idx));
    }

    harness_wait_start();

    for (long it = 0; it < iters; it++) {
        int l = (int)(xorshift(&rng) % (uint64_t)nlocks);
        ncl_acquire(&t, l);

        ncl_line_t *occ = occ_line(l);
        if (ncl_uc_load(occ) != 0)
            w->viol++;
        ncl_uc_store(occ, tag);
        ncl_fence();

        struct lockdata *d = ld(l);
        if (d->checksum != checksum(d))
            w->cksum_err++;
        int k = (int)(xorshift(&rng) % (uint64_t)nwords);
        d->words[k] += xorshift(&rng) | 1;
        rand_delay(&rng);
        d->counter++;
        d->checksum = checksum(d);

        ncl_fence();
        if (ncl_uc_load(occ) != tag)
            w->viol++;
        ncl_uc_store(occ, 0);

        if (precise) {
            struct ncl_range r[2] = {
                {d, 2 * sizeof(uint64_t)},
                {&d->words[k], sizeof(uint64_t)},
            };
            ncl_release(l, r, 2);
        } else {
            ncl_release(l, NULL, 0);
        }
        w->acq++;
        rand_delay(&rng);
    }
    return NULL;
}

static int node_main(int node)
{
    if (ncl_node_attach(node) < 0)
        return 2;
    struct worker *ws = calloc((size_t)nthreads, sizeof *ws);
    for (int i = 0; i < nthreads; i++) {
        ws[i].node = node;
        ws[i].idx = i;
        pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]);
    }
    harness_start();
    uint64_t acq = 0, viol = 0, ck = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(ws[i].tid, NULL);
        acq += ws[i].acq;
        viol += ws[i].viol;
        ck += ws[i].cksum_err;
    }
    ncl_node_detach(); /* all nodes done; servers stopped */
    struct ncl_node_stats st;
    ncl_node_stats(&st);
    viol += st.srv.errors;       /* server-detected protocol violations */
    viol += st.ho.stale_announce; /* resurrected announce words (2b) */
    printf("node %2d: acq=%lu viol=%lu cksum_err=%lu", node, (unsigned long)acq,
           (unsigned long)viol, (unsigned long)ck);
    for (int p = 0; p < NCL_PATH__COUNT; p++)
        if (st.path[p])
            printf(" %s=%lu", ncl_path_names[p], (unsigned long)st.path[p]);
    if (st.base.lamport_retries)
        printf(" lamport_retries=%lu", (unsigned long)st.base.lamport_retries);
    if (st.ho.gate_entries)
        printf(" gate=%lu missed_announce=%lu sent=%lu frees=%lu", (unsigned long)st.ho.gate_entries,
               (unsigned long)st.ho.missed_announce, (unsigned long)st.ho.handoffs_sent,
               (unsigned long)st.ho.frees);
    struct ncl_base_stats sb = st.base;
    ncl_base_stats_add(&sb, &st.srv.base);
    if (sb.split_fail_closed || sb.split_fail_lost)
        printf(" split_closed=%lu split_lost=%lu", (unsigned long)sb.split_fail_closed,
               (unsigned long)sb.split_fail_lost);
    if (sb.split_resets || sb.split_reset_skips)
        printf(" door_resets=%lu reset_skips=%lu", (unsigned long)sb.split_resets,
               (unsigned long)sb.split_reset_skips);
    if (sb.react_up || sb.react_down || sb.react_stale)
        printf(" react_up=%lu react_down=%lu react_stale=%lu", (unsigned long)sb.react_up,
               (unsigned long)sb.react_down, (unsigned long)sb.react_stale);
    if (sb.bak_pred_polls || sb.bak_rescans)
        printf(" bak_pred_polls=%lu bak_rescans=%lu bak_choosing_seen=%lu bak_classic_drops=%lu",
               (unsigned long)sb.bak_pred_polls, (unsigned long)sb.bak_rescans,
               (unsigned long)sb.bak_choosing_seen, (unsigned long)sb.bak_classic_drops);
    if (sb.ret_hits || sb.ret_rel_drops || sb.ret_sweep_drops || sb.ret_lease_drops)
        printf(" ret_hits=%lu ret_skipped=%lu ret_acq_drops=%lu ret_rel_drops=%lu"
               " ret_sweep_drops=%lu ret_lease_drops=%lu ret_sweep_busy=%lu",
               (unsigned long)sb.ret_hits, (unsigned long)sb.ret_levels_skipped,
               (unsigned long)sb.ret_acq_drops, (unsigned long)sb.ret_rel_drops,
               (unsigned long)sb.ret_sweep_drops, (unsigned long)sb.ret_lease_drops,
               (unsigned long)sb.ret_sweep_busy);
    if (sb.hot_acqs || sb.hot_claims || sb.hot_r_acqs)
        printf(" hot_acqs=%lu hot_r_acqs=%lu hot_claims=%lu hot_full=%lu hot_free_fair=%lu"
               " hot_free_lease=%lu hot_mode_sets=%lu hot_mode_clears=%lu",
               (unsigned long)sb.hot_acqs, (unsigned long)sb.hot_r_acqs,
               (unsigned long)sb.hot_claims, (unsigned long)sb.hot_full,
               (unsigned long)sb.hot_free_fair, (unsigned long)sb.hot_free_lease,
               (unsigned long)sb.hot_mode_sets, (unsigned long)sb.hot_mode_clears);
    if (sb.ret_cap_lowers || sb.ret_cap_raises || sb.ret_policy_skips)
        printf(" ret_cap_lowers=%lu ret_cap_raises=%lu ret_policy_skips=%lu",
               (unsigned long)sb.ret_cap_lowers, (unsigned long)sb.ret_cap_raises,
               (unsigned long)sb.ret_policy_skips);
    if (st.ho.stale_announce)
        printf(" STALE_ANNOUNCE=%lu", (unsigned long)st.ho.stale_announce);
    if (st.global_releases != st.acquires)
        printf(" global_releases=%lu cap_releases=%lu", (unsigned long)st.global_releases,
               (unsigned long)st.cap_releases);
    if (st.srv.msgs)
        printf(" srv_msgs=%lu srv_errors=%lu top_acq=%lu", (unsigned long)st.srv.msgs,
               (unsigned long)st.srv.errors, (unsigned long)st.srv.top_acquires);
    printf("\n");
    fflush(stdout);
    struct node_result res = {acq, viol, ck};
    harness_put_result(&res); /* after printing: node 0 prints the total last */
    free(ws);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -n N          nodes (default 4)\n"
            "  -t T          threads per node (default 2)\n"
            "  -l L          locks (default 4)\n"
            "  -i I          iterations per thread (default 100000)\n"
            "  -w W          data words per lock (default 16)\n"
            "  -d NS         max random delay in/out of CS, ns (default 200)\n"
            "  -a ALG        lamport|tournament|hybrid|server|fastsrv|ak|aksrv|reactive|bakery|pthread (default lamport)\n"
            "  -b BACKEND    coherent|cached-flush|uc|emu (default emu)\n"
            "  -L NS         injected device latency, ns (default 0)\n"
            "  -m            ablation: no local submutex\n"
            "  -M MODE       phase1|local|handoff (default phase1)\n"
            "  -K CAP        local/handoff: max consecutive local handoffs (default 64)\n"
            "  -G PLACE      handoff: announce before (default) | inside\n"
            "  -X PICK       handoff: pick_waiter adaptive (default) | scan\n"
            "                (server: adaptive = follow the grant's hint bit, scan = always read succ[])\n"
            "  --wait W      handoff, gate algorithms: own (own line, default) | owner (poll owner)\n"
            "  --ho LIST     handoff, gate algorithms: all (default; drops grant with --wait owner, skip with -G inside) | none | grant,clear,skip\n"
            "  -R LINES      server handoff: chain window, power of two (default 4N rounded up)\n"
            "  -O on|off     handoff: overlap release data writeback with its loads (default on)\n"
            "  -U LAYOUT     metadata layout: uniform (default) | split (scanned arrays in a\n"
            "                cached region; uc, emu)\n"
            "  -Q MODE       multi-line reads of cached metadata (cached-flush, split): probe (default) | batch | serial\n"
            "  -F MODE       ak: fair (default, our fairness variant) | always (as published)\n"
            "  -H            precise dirty-range hints on release\n"
            "  -p            pin threads to CPUs\n"
            "  --skip-wb     DEBUG: skip data writeback (emu must fail)\n"
            "  --skip-inv    DEBUG: skip data invalidate (emu must fail)\n"
            "  --skip-meta-wb / --skip-meta-inv\n"
            "                DEBUG: split layout: skip scan-region writeback / invalidate\n"
            "  --emu-word-wb emu split: write back only the stored word of a cached metadata\n"
            "                line (old model; default: the node's whole line, as hardware)\n"
            "  --debug-no-grant-wait  DEBUG: handoff --wait own without --ho grant: clear the\n"
            "                announce without waiting for a grant still on its way (must FAIL\n"
            "                on emu split with full-line writeback; see design.md §5)\n"
            "  --react U:D[:W] -a reactive: to the tournament when >= U of the last 16 global\n"
            "                acquisitions met another node, back when <= D of the last 64, after >= W\n"
            "                acquisitions on the tournament (8:4:64)\n"
            "  --debug-react BITS  reactive: 8 = switch on every acquisition (safe); with 8, UNSAFE\n"
            "                (must FAIL): 1 no new lock, 2 no mode re-check, 4 no INVALID release,\n"
            "                16 no slow-path re-check (with --lamport-delay)\n"
            "  --lamport-delay NS  DEBUG: Lamport spins between reading y == 0 and storing y\n"
            "  --mode-delay NS  DEBUG: reactive tournament acquirer spins before reading mode\n"
            "  --react-layout mode-first|xy-first  reactive lock block layout\n"
            "  --debug-bakery BITS  bakery, UNSAFE: 1 = no choosing store, 2 = ignore choosing,\n"
            "                4 = leaf-first tree release, 8 = classic wait clears j before our ticket\n"
            "  --bakery-arity K  bakery: tree of K-way nodes (0 = flat)\n"
            "  --bakery-poll pred|all  bakery wait: predecessor poll (default) or all blockers\n"
            "  --bakery-wait classic|strict  bakery: once j was seen not choosing after our ticket\n"
            "                was visible, wait on its ticket only (classic, default), or wait\n"
            "                while j is choosing on every read (strict)\n"
            "  --bakery-delay NS  DEBUG: bakery spins between reading tickets and publishing its own\n"
            "  --retain      tournament: prefix retention (design.md §4g g1) with a per-node sweeper\n"
            "  --retain-check both|acquire|release|none  where kept levels are checked for\n"
            "                waiters (default acquire)\n"
            "  --retain-poll US|off  sweeper poll period (10; 0 = yield; off = no sweeper: UNSAFE,\n"
            "                cross-lock deadlock, must hang with several locks)\n"
            "  --retain-lease US  sweeper drops kept levels this long after they were kept (1000)\n"
            "  --retain-max N  at most N locks with kept levels per node (0 = no limit)\n"
            "  --retain-probe P  learned keep height: first probe interval in releases\n"
            "                (16; 0 = always keep depth-1 levels)\n"
            "  --debug-retain BITS  retain, UNSAFE: 1 = sweeper drops without the submutex,\n"
            "                2 = release drops only the waiter's level (not contiguous)\n"
            "  --retain-delay NS  DEBUG: acquirer spins between reading its kept levels and climbing\n"
            "  --hot-probe   -a tournament: also read hot_mode (never set) in the root's entry check\n"
            "  --hot K       -a tournament --retain: hot-slot subtree, K slots (design.md §4g g2)\n"
            "  --hot-window US / --hot-lease US / --hot-admit cap|always  as ncl_bench\n"
            "  --debug-hot BITS  hot, UNSAFE: 1 = lease free without the submutex, 2 = claim\n"
            "                without the global lock, 4 = release R after the levels below,\n"
            "                8 = read hot_mode before C-top's flag and turn, 16 = clear\n"
            "                hot_mode without reading the slots\n"
            "  --hot-delay NS  DEBUG: spin in the --debug-hot windows\n"
            "  --mix-algs    per-lock algorithms: even locks lamport, odd locks tournament\n"
            "  --grant-delay NS  DEBUG: 2b releaser spins between its owner and grant stores\n"
            "  --reset-delay NS  DEBUG: (d) spin after the reset check's door read and after its scan\n"
            "  --debug-unsafe-reopen  DEBUG: (d)/(e) fast winner probes one present[] slot\n"
            "                and reopens on a miss (the old -F hint; reset race, must FAIL\n"
            "                with --reset-delay)\n"
            "  --mem-node N  bind the shared regions (metadata, scan, data) to NUMA node N\n"
            "                and verify every page (uc: driver must be loaded with nid=N)\n"
            "  --cpus LIST   CPUs for pinned threads, e.g. 0-59,120-179 (default: this\n"
            "                process's affinity, e.g. from numactl --cpunodebind)\n"
            "  --wb MODE     writeback instruction: clflushopt (default) | clwb\n"
            "  --timeout S   the launcher kills the nodes and FAILs after S seconds (0 = none)\n"
            HARNESS_USAGE,
            argv0, argv0);
}

int main(int argc, char **argv)
{
    struct ncl_sys_cfg cfg = {
        .backend = NCL_BACKEND_EMU,
        .alg = NCL_ALG_LAMPORT,
        .mode = NCL_MODE_PHASE1,
        .handoff_opts = NCL_HO_DEFAULT,
        .check_announce = 1, /* the stress test checks the 2b announce invariant */
    };
    static struct option lopts[] = {
        {"mem-node", required_argument, 0, 1001},
        {"cpus", required_argument, 0, 1002},
        {"wb", required_argument, 0, 1003},
        {"wait", required_argument, 0, 1004},
        {"ho", required_argument, 0, 1005},
        {"skip-wb", no_argument, 0, 1},
        {"skip-inv", no_argument, 0, 2},
        {"skip-meta-wb", no_argument, 0, 3},
        {"skip-meta-inv", no_argument, 0, 4},
        {"timeout", required_argument, 0, 5},
        {"reset-delay", required_argument, 0, 6},
        {"debug-unsafe-reopen", no_argument, 0, 7},
        {"emu-word-wb", no_argument, 0, 8},
        {"debug-no-grant-wait", no_argument, 0, 9},
        {"grant-delay", required_argument, 0, 10},
        {"mix-algs", no_argument, 0, 11},
        {"hot-probe", no_argument, 0, 30},
        {"hot", required_argument, 0, 31},
        {"hot-window", required_argument, 0, 32},
        {"hot-lease", required_argument, 0, 33},
        {"hot-admit", required_argument, 0, 34},
        {"debug-hot", required_argument, 0, 35},
        {"hot-delay", required_argument, 0, 36},
        {"react", required_argument, 0, 12},
        {"debug-react", required_argument, 0, 13},
        {"lamport-delay", required_argument, 0, 14},
        {"mode-delay", required_argument, 0, 15},
        {"react-layout", required_argument, 0, 16},
        {"debug-bakery", required_argument, 0, 17},
        {"bakery-delay", required_argument, 0, 18},
        {"bakery-poll", required_argument, 0, 19},
        {"bakery-arity", required_argument, 0, 20},
        {"bakery-wait", required_argument, 0, 21},
        {"retain", no_argument, 0, 22},
        {"retain-check", required_argument, 0, 23},
        {"retain-poll", required_argument, 0, 24},
        {"retain-lease", required_argument, 0, 25},
        {"retain-max", required_argument, 0, 26},
        {"debug-retain", required_argument, 0, 27},
        {"retain-delay", required_argument, 0, 28},
        {"retain-probe", required_argument, 0, 29},
        {"help", no_argument, 0, 'h'},
        HARNESS_LONGOPTS,
        {0, 0, 0, 0},
    };
    int c;
    int wait_set = 0;
    while ((c = getopt_long(argc, argv, "n:t:l:i:w:d:a:b:L:M:K:F:G:X:R:Q:O:U:mHph", lopts, NULL)) != -1) {
        switch (c) {
        case 'n': nnodes = atoi(optarg); break;
        case 't': nthreads = atoi(optarg); break;
        case 'l': nlocks = atoi(optarg); break;
        case 'i': iters = atol(optarg); break;
        case 'w': nwords = atoi(optarg); break;
        case 'd': max_delay_ns = (unsigned)atoi(optarg); break;
        case 'a':
            if (ncl_alg_parse(optarg, &cfg.alg) < 0) { usage(argv[0]); return 2; }
            break;
        case 'b':
            if (ncl_backend_parse(optarg, &cfg.backend) < 0) { usage(argv[0]); return 2; }
            break;
        case 'L': cfg.dev_lat_ns = (uint32_t)atoi(optarg); break;
        case 'm': cfg.no_submutex = 1; break;
        case 'M':
            if (ncl_mode_parse(optarg, &cfg.mode) < 0) { usage(argv[0]); return 2; }
            break;
        case 'K': cfg.local_cap = atoi(optarg); break;
        case 'R': cfg.chain_lines = atoi(optarg); break;
        case 'O': ncl_wb_overlap = strcmp(optarg, "off") != 0; break;
        case 'Q':
            ncl_meta_batch = !strcmp(optarg, "serial") ? NCL_META_SERIAL
                             : !strcmp(optarg, "probe") ? NCL_META_PROBE : NCL_META_BATCH;
            break;
        case 'G':
            if (!strcmp(optarg, "before"))
                cfg.announce_place = NCL_ANNOUNCE_BEFORE;
            else if (!strcmp(optarg, "inside"))
                cfg.announce_place = NCL_ANNOUNCE_INSIDE;
            else {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'X':
            if (!strcmp(optarg, "adaptive"))
                cfg.pick_policy = NCL_PICK_ADAPTIVE;
            else if (!strcmp(optarg, "scan"))
                cfg.pick_policy = NCL_PICK_SCAN;
            else {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'F':
            if (!strcmp(optarg, "fair"))
                ncl_fast_release = NCL_FASTREL_SCAN;
            else if (!strcmp(optarg, "always"))
                ncl_fast_release = NCL_FASTREL_ALWAYS;
            else {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'H': precise = 1; break;
        case 'p': pin = 1; cfg.server_pin = 1; break;
        case 'U':
            if (!strcmp(optarg, "split"))
                cfg.meta_split = 1;
            else if (strcmp(optarg, "uniform")) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 3: cfg.debug_skip_meta_wb = 1; break;
        case 4: cfg.debug_skip_meta_inv = 1; break;
        case 5: timeout_s = atoi(optarg); break;
        case 6: reset_delay_ns = (unsigned)atoi(optarg); break;
        case 7: ncl_fast_release = NCL_FASTREL_HINT; break;
        case 8: cfg.emu_word_wb = 1; break;
        case 9: cfg.debug_no_grant_wait = 1; break;
        case 10: grant_delay_ns = (unsigned)atoi(optarg); break;
        case 11: cfg.alg_per_lock = 1; break;
        case 13: ncl_debug_react = atoi(optarg); break;
        case 14: lamport_delay_ns = (unsigned)atoi(optarg); break;
        case 15: mode_delay_ns = (unsigned)atoi(optarg); break;
        case 17: ncl_debug_bakery = atoi(optarg); break;
        case 18: bakery_delay_ns = (unsigned)atoi(optarg); break;
        case 20: ncl_bakery_arity = atoi(optarg); break;
        case 22: cfg.retain = 1; break;
        case 23: if (ncl_retain_opt("check", optarg) < 0) return 2; break;
        case 24: if (ncl_retain_opt("poll", optarg) < 0) return 2; break;
        case 25: if (ncl_retain_opt("lease", optarg) < 0) return 2; break;
        case 26: if (ncl_retain_opt("max", optarg) < 0) return 2; break;
        case 27: ncl_debug_retain = atoi(optarg); break;
        case 28: retain_delay_ns = (unsigned)atoi(optarg); break;
        case 29: if (ncl_retain_opt("probe", optarg) < 0) return 2; break;
        case 30: ncl_tour_hot_probe = 1; break;
        case 31: if (ncl_hot_opt(&cfg, "slots", optarg) < 0) return 2; break;
        case 32: if (ncl_hot_opt(&cfg, "window", optarg) < 0) return 2; break;
        case 33: if (ncl_hot_opt(&cfg, "lease", optarg) < 0) return 2; break;
        case 34: if (ncl_hot_opt(&cfg, "admit", optarg) < 0) return 2; break;
        case 35: if (ncl_hot_opt(&cfg, "debug", optarg) < 0) return 2; break;
        case 36: if (ncl_hot_opt(&cfg, "delay", optarg) < 0) return 2; break;
        case 21:
            if (!strcmp(optarg, "classic")) ncl_bakery_wait_classic = 1;
            else if (!strcmp(optarg, "strict")) ncl_bakery_wait_classic = 0;
            else { fprintf(stderr, "--bakery-wait: strict|classic\n"); return 2; }
            break;

        case 19:
            if (!strcmp(optarg, "all")) ncl_bakery_poll_all = 1;
            else if (!strcmp(optarg, "pred")) ncl_bakery_poll_all = 0;
            else { usage(argv[0]); return 2; }
            break;
        case 16:
            if (!strcmp(optarg, "xy-first")) ncl_react_layout = 1;
            else if (!strcmp(optarg, "mode-first")) ncl_react_layout = 0;
            else { usage(argv[0]); return 2; }
            break;
        case 12:
            if (ncl_react_parse(optarg) < 0) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 1001: cfg.mem_node = atoi(optarg) + 1; break;
        case 1002: if (ncl_cpus_set(optarg) < 0) return 2; break;
        case 1003:
            if (!strcmp(optarg, "clwb")) { if (ncl_wb_mode_set(NCL_WB_CLWB) < 0) return 2; }
            else if (strcmp(optarg, "clflushopt")) { usage(argv[0]); return 2; }
            break;
        case 1004:
            if (!strcmp(optarg, "own")) cfg.handoff_wait = NCL_WAIT_OWN;
            else if (!strcmp(optarg, "owner")) cfg.handoff_wait = NCL_WAIT_OWNER;
            else { fprintf(stderr, "--wait: owner|own\n"); return 2; }
            wait_set = 1;
            break;
        case 1005:
            if (ncl_ho_opts_parse(optarg, &cfg.handoff_opts) < 0) {
                fprintf(stderr, "--ho: none|all|grant,clear,skip\n");
                return 2;
            }
            break;
        case 1: cfg.debug_skip_data_wb = 1; break;
        case 2: cfg.debug_skip_data_inv = 1; break;
        default: {
            int h = harness_opt(c, optarg);
            if (h > 0)
                break;
            if (h == 0)
                usage(argv[0]);
            return c == 'h' ? 0 : 2;
        }
        }
    }
    if (harness_check(argc, argv, "ncl_stress") < 0)
        return 2;
    if (wait_set && (cfg.mode != NCL_MODE_HANDOFF || cfg.alg == NCL_ALG_SERVER)) {
        fprintf(stderr, "--wait applies to -M handoff with a gate algorithm (lamport, tournament, "
                        "hybrid, ak); the server chain already waits on its own slot\n");
        return 2;
    }
    if (nwords < 1 || nlocks < 1 || nthreads < 1) {
        usage(argv[0]);
        return 2;
    }

    stride = (sizeof(struct lockdata) + (size_t)nwords * sizeof(uint64_t) + NCL_CACHELINE - 1) &
             ~(size_t)(NCL_CACHELINE - 1);
    cfg.nnodes = nnodes;
    cfg.threads_per_node = nthreads;
    cfg.nlocks = nlocks;
    cfg.data_size = stride * (size_t)nlocks;
    cfg.user_meta_lines = (size_t)nlocks;

    if (cfg.backend == NCL_BACKEND_UC && access(getenv("NCL_UC_DEV") ? getenv("NCL_UC_DEV")
                                                                      : "/dev/ncl_uc", F_OK)) {
        fprintf(stderr, "stress: uc backend requested but driver not loaded; skipping\n");
        return 77; /* ctest SKIP_RETURN_CODE */
    }
    if (harness.role == HR_QUERY)
        return harness_query(nnodes, cfg.backend, timeout_s);
    if (harness_open(&cfg, sizeof(struct node_result)) < 0)
        return 2;
    if (harness.role == HR_INIT) {
        printf("mem: %s\n", ncl_mem_describe());
        if (pin) {
            int srv = cfg.alg == NCL_ALG_SERVER || ncl_alg_fastsrv_like(cfg.alg);
            ncl_cpus_check(nnodes * nthreads + (srv ? nnodes : 0), nnodes, nthreads, 0, 0);
            printf("cpus: %s\n", ncl_cpus_describe());
        }
        for (int l = 0; l < nlocks; l++) {
            if (ncl_lock_set_data(l, (ncl_off_t)l * stride, stride) < 0)
                return 2;
            if (cfg.alg_per_lock &&
                ncl_lock_set_alg(l, l % 2 ? NCL_ALG_TOURNAMENT : NCL_ALG_LAMPORT) < 0)
                return 2;
            struct lockdata *d = ld(l);
            d->checksum = checksum(d);
        }
        ncl_sys_publish();
        printf("stress: alg=%s backend=%s mode=%s%s nodes=%d threads/node=%d participants=%d "
               "locks=%d iters/thread=%ld words=%d delay<=%uns lat=%uns%s%s%s%s%s%s%s\n",
               ncl_alg_name(cfg.alg), ncl_backend_name(cfg.backend), ncl_mode_name(cfg.mode),
               cfg.no_submutex ? "+no_submutex" : "", nnodes, nthreads, ncl_participants(),
               nlocks, iters, nwords, max_delay_ns, cfg.dev_lat_ns, precise ? " precise-hints" : "",
               cfg.debug_skip_data_wb ? " SKIP-WB" : "", cfg.debug_skip_data_inv ? " SKIP-INV" : "",
               cfg.debug_skip_meta_wb ? " SKIP-META-WB" : "",
               cfg.debug_skip_meta_inv ? " SKIP-META-INV" : "",
               cfg.emu_word_wb ? " EMU-WORD-WB" : "", cfg.debug_no_grant_wait ? " NO-GRANT-WAIT" : "");
        ncl_sys_destroy();
        return 0;
    }
    delay_cycles = ncl_ns_to_cycles(max_delay_ns);
    ncl_debug_reset_delay = ncl_ns_to_cycles(reset_delay_ns);
    ncl_debug_grant_delay = ncl_ns_to_cycles(grant_delay_ns);
    ncl_debug_lamport_delay = ncl_ns_to_cycles(lamport_delay_ns);
    ncl_debug_mode_delay = ncl_ns_to_cycles(mode_delay_ns);
    ncl_debug_bakery_delay = ncl_ns_to_cycles(bakery_delay_ns);
    ncl_debug_retain_delay = ncl_ns_to_cycles(retain_delay_ns);

    int rc = node_main(harness.node);
    if (rc || harness.node != 0) {
        ncl_sys_destroy();
        return rc;
    }

    /* Node 0: gather the per-node results (all nodes are past ncl_node_detach,
     * so no more acquisitions) and check the final data. */
    uint64_t acq = 0, viol = 0, ck = 0, sum = 0, final_ck_bad = 0;
    for (int n = 0; n < nnodes; n++) {
        struct node_result r;
        harness_get_result(n, &r);
        acq += r.acq;
        viol += r.viol;
        ck += r.cksum_err;
    }
    for (int l = 0; l < nlocks; l++) {
        struct lockdata *d = ld(l);
        ncl_data_invalidate(d, stride);
        sum += d->counter;
        if (d->checksum != checksum(d))
            final_ck_bad++;
    }
    uint64_t expect = (uint64_t)nnodes * (uint64_t)nthreads * (uint64_t)iters;
    int ok = viol == 0 && ck == 0 && final_ck_bad == 0 && sum == expect && acq == expect;
    printf("total: expected=%lu acquired=%lu counter_sum=%lu mutex_violations=%lu "
           "checksum_errors=%lu final_bad_checksums=%lu -> %s\n",
           (unsigned long)expect, (unsigned long)acq, (unsigned long)sum, (unsigned long)viol,
           (unsigned long)ck, (unsigned long)final_ck_bad, ok ? "PASS" : "FAIL");
    ncl_sys_destroy();
    return ok ? 0 : 1;
}
