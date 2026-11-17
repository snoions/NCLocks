#!/usr/bin/env python3
"""Median tables for a scripts/cxl_sweep.sh result directory (needs pandas).

Usage: scripts/cxl_summary.py results/cxl_<date>/ [phase ...]
Phases: p1 abl 2a 2b own ho locks base open dram conc open2b mt kv (default: those
whose CSV exists).
Closed loop: throughput in k acq/s (median over reps), node fairness (min/max
completions, median) and the rep spread (max-min)/min in %. Open loop: achieved
rate, unserved fraction and sojourn p50/p99 in us.
"""
import os, sys
import pandas as pd

pd.set_option("display.width", 250)
pd.set_option("display.max_rows", 1000)
pd.set_option("display.max_columns", 80)


def closed(d, extra=()):
    d = d.copy()
    d["NT"] = d.nodes.astype(str) + "x" + d.threads_per_node.astype(str)
    if d["locks"].nunique() > 1:
        d["NT"] = d.NT + " L" + d["locks"].map("{:>3}".format)
    g = d.groupby(["alg", "NT", "config"])
    r = g.agg(thr=("achieved_rps", "median"), lo=("achieved_rps", "min"),
              hi=("achieved_rps", "max"), fair=("node_min_max_ratio", "median"),
              **{e: (e, "median") for e in extra})
    r["thr"] = (r.thr / 1e3).round(0)
    r["spread%"] = ((r.hi - r.lo) / r.lo * 100).round(0)
    r = r.drop(columns=["lo", "hi"])
    return r.unstack("config")


def open_loop(d):
    r = d.groupby(["alg", "config", "target_rps"]).agg(
        ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
        p50=("soj_p50", "median"), p99=("soj_p99", "median"),
        fair=("node_min_max_ratio", "median"))
    r["ach"] = (r.ach / 1e3).round(0)
    r["p50"] = (r.p50 / 1e3).round(1)
    r["p99"] = (r.p99 / 1e3).round(1)
    return r.unstack("config")


def conc(d):
    """ncl_memconc: aggregate Mops/s (median) and ns per op per thread, by op and T."""
    r = d.groupby(["op", "threads", "config"]).agg(
        mops=("mops", "median"), ns=("ns_per_op_per_thread", "median"))
    return r.round(1).unstack("config")


def open2b(d):
    """Open-loop rows by variant and offered load; closed-loop rows separately."""
    d = d.copy()
    d["variant"] = (d.alg + "/" + d["mode"] + "/" + d.ho_opts.astype(str) + "/"
                    + d.pick.astype(str)).str.replace("/-/-", "", regex=False)
    o = d[d.arrival != "closed"]
    r = o.groupby(["locks", "target_rps", "variant"]).agg(
        ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
        p50=("lock_p50", "median"), p99=("lock_p99", "median"),
        missed=("missed_announce_per_acq", "median"))
    r["ach"] = (r.ach / 1e3).round(0)
    r["p50"] = (r.p50 / 1e3).round(2)
    r["p99"] = (r.p99 / 1e3).round(1)
    out = r.to_string()
    c = d[d.arrival == "closed"]
    if len(c):
        c = c.assign(config=c.config + "/" + c.pick.astype(str))
        out += "\n\nclosed loop, L=256:\n" + closed(c, ["lock_p99", "missed_announce_per_acq"]).to_string()
    return out


def mt(d):
    """Phase mt: one table per part (config column <cfg>:<part>)."""
    d = d.copy()
    d["part"] = d.config.str.split(":").str[1]
    d["cfg"] = d.config.str.split(":").str[0]
    d["variant"] = d.alg + "/" + d["mode"]
    d["NT"] = d.nodes.astype(str) + "x" + d.threads_per_node.astype(str)
    out = []

    def agg(g):
        r = g.agg(thr=("achieved_rps", "median"), fair=("node_min_max_ratio", "median"),
                  p99=("lock_p99", "median"), local=("local_handoff_frac", "median"))
        r["thr"] = (r.thr / 1e3).round(0)
        r["p99"] = (r.p99 / 1e3).round(1)
        return r.round(3)

    c = d[d.part == "cap"]
    if len(c):
        out.append("cap (MT_MAIN, L=1): k/s, fairness, lock p99 us, local handoff fraction\n"
                   + agg(c.groupby(["variant", "local_cap", "cfg"])).unstack("local_cap").to_string())
    c = d[d.part == "locks"]
    if len(c):
        r = agg(c.groupby(["locks", "variant", "cfg"]))[["thr", "fair", "p99"]]
        out.append("locks (MT_MAIN, K=64):\n" + r.unstack("locks").to_string())
    c = d[d.part == "nt"]
    if len(c):
        r = agg(c.groupby(["locks", "NT", "variant", "cfg"]))[["thr", "fair"]]
        out.append("threads per node (K=64):\n" + r.unstack("variant").to_string())
    c = d[d.part == "open"]
    if len(c):
        r = c.groupby(["locks", "target_rps", "variant", "cfg"]).agg(
            ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
            p50=("lock_p50", "median"), p99=("lock_p99", "median"),
            fair=("node_min_max_ratio", "median"))
        r["ach"] = (r.ach / 1e3).round(0)
        r["p50"] = (r.p50 / 1e3).round(2)
        r["p99"] = (r.p99 / 1e3).round(1)
        out.append("open loop (MT_MAIN, Poisson): k/s achieved, lock p50 / p99 us\n" + r.round(3).to_string())
    return "\n\n".join(out)


def kv(d):
    """Phase kv: one table per part (config column <cfg>:<part>)."""
    d = d.copy()
    d["part"] = d.config.str.split(":").str[1]
    d["variant"] = d.apply(lambda r: "server-chain" if r.alg == "server"
                           else "all-lamport" if r.tournament_locks == 0
                           else "all-tournament" if r.tournament_locks == r.locks
                           else "top%d-tournament" % r.tournament_locks, axis=1)
    d["dist"] = d.apply(lambda r: "uniform" if r.dist == "uniform" else "zipf%.2f" % r.theta, axis=1)

    def agg(g):
        r = g.agg(thr=("achieved_rps", "median"), p99=("lock_p99", "median"),
                  fair=("node_min_max_ratio", "median"), err=("errors", "max"))
        r["thr"] = (r.thr / 1e3).round(0)
        r["p99"] = (r.p99 / 1e3).round(1)
        return r

    out = []
    p = d[d.part == "map"]
    if len(p):
        out.append("map (closed loop): k ops/s, lock p99 us, fairness\n"
                   + agg(p.groupby(["workload", "locks", "variant"])).to_string())
    p = d[d.part == "skew"]
    if len(p):
        r = agg(p.groupby(["dist", "affinity", "variant"]))[["thr", "p99", "fair"]]
        out.append("skew x affinity (L=1024, A):\n" + r.unstack("variant").to_string())
    p = d[d.part == "data"]
    if len(p):
        out.append("data maintenance (all tournament):\n"
                   + agg(p.groupby(["locks", "data"])).to_string())
    p = d[d.part == "open"]
    if len(p):
        r = p.groupby(["target_rps", "variant"]).agg(
            ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
            p50=("lock_p50", "median"), p99=("lock_p99", "median"), op99=("op_p99", "median"))
        r["ach"] = (r.ach / 1e3).round(0)
        for k in ("p50", "p99", "op99"):
            r[k] = (r[k] / 1e3).round(2)
        out.append("open loop (L=1024, A, Zipf 0.99): k/s, lock p50/p99 and op p99 in us\n"
                   + r.to_string())
    if d.errors.max() > 0:
        out.append("!!! %d runs with KV check errors" % (d.errors > 0).sum())
    return "\n\n".join(out)


EXTRA = {
    "p1": ["fast_frac", "lock_p99"],
    "2a": ["local_handoff_frac"],
    "2b": ["global_handoff_frac", "owner_polls_per_acq", "lock_p99"],
    "own": ["owner_polls_per_acq", "grant_polls_per_acq"],
    "locks": ["fast_frac", "lock_p99"],
    "ho": ["lock_p99", "missed_announce_per_acq", "gate_entries_per_acq"],
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    phases = sys.argv[2:] or ["p1", "abl", "2a", "2b", "own", "ho", "locks", "base", "open", "dram",
                              "conc", "open2b", "mt", "kv"]
    for ph in phases:
        f = os.path.join(root, ph + ".csv")
        if not os.path.exists(f):
            continue
        d = pd.read_csv(f)
        print(f"=== {ph} ({len(d)} rows)")
        if ph == "own":
            d["config"] = d.config + "/" + d["wait"]
        if ph == "ho":
            d["config"] = d["mode"] + "/" + d["ho_opts"]
        if ph == "open":
            print(open_loop(d).to_string())
        elif ph == "conc":
            print(conc(d).to_string())
        elif ph == "open2b":
            print(open2b(d))
        elif ph == "mt":
            print(mt(d))
        elif ph == "kv":
            print(kv(d))
        else:
            print(closed(d, EXTRA.get(ph, ())).to_string())
        print()
    err = os.path.join(root, "errors.log")
    if os.path.exists(err) and os.path.getsize(err):
        print(f"!!! {err} is not empty")


if __name__ == "__main__":
    main()
