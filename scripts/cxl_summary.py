#!/usr/bin/env python3
"""Median tables for a scripts/cxl_sweep.sh result directory (needs pandas).

Usage: scripts/cxl_summary.py results/cxl_<date>/ [phase ...]
Phases: p1 abl 2a 2b own ho locks base open dram conc open2b mt kv kvreact kviso
kvbase klayout kstride kfix g1bound g1 g2bound (default: those whose CSV exists).
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


def g1_variant(r):
    """Label a g1/g1bound row: retain variants share alg=tournament, so use `retain`."""
    ret = str(getattr(r, "retain", "-"))
    if r.alg != "tournament" or ret in ("-", "nan"):
        return r.alg
    check, *_, pol = ret.split("/")
    # Labels name the checks (the default changed from both to acquire on 2026-11-25).
    name = {"both": "retain_both", "acquire": "retain_acq"}.get(check, "retain_" + check)
    if pol.endswith("pen0") or pol.endswith("probe0"):
        name += "_always"
    elif pol.startswith("pen"):
        name += "(onoff)"  # the first, on/off policy (before the learned keep height)
    return name


def g1(d):
    """Phases g1, g1bound: throughput (k acq/s, median over reps) per variant at each
    --active subset, the CPU-matched bounds, and every node at L = 1, 16, 256; then
    each retain variant relative to the plain tournament and to the best of
    tournament / reactive / lamport."""
    d = d.copy()
    d["variant"] = d.apply(g1_variant, axis=1)
    d["subset"] = d.config.str.split(":").str[1]
    d["NT"] = d.nodes.astype(str) + "x" + d.threads_per_node.astype(str)
    d["mode"] = d["mode"].replace({"phase1": "P1", "local": "2a"})
    d["think"] = (d.think_ns / 1000).map("{:g}us".format)
    out = []
    sub = d[~d.subset.isin(["all"]) & ~d.subset.str.startswith("bound")]
    bnd = d[d.subset.str.startswith("bound")]
    # A bound's NT is its own -n; key it by the T and the subset it bounds.
    bmap = {"one": "bound1", "far2": "bound2", "far4": "bound4", "near2": "bound2", "near4": "bound4"}
    key = ["mode", "NT", "think", "subset"]
    t = sub.pivot_table(index=key, columns="variant", values="achieved_rps", aggfunc="median") / 1e3
    if len(bnd):
        b = bnd.groupby(["mode", "threads_per_node", "think", "subset"]).achieved_rps.median() / 1e3
        t["bound"] = [b.get((m, int(nt.split("x")[1]), th, bmap.get(s, "")), float("nan"))
                      for m, nt, th, s in t.index]
    rets = [c for c in t.columns if c.startswith("retain")]
    if len(sub):
        out.append("k acq/s, median over reps (subsets; bound = CPU-matched tournament over the "
                   "levels retention still climbs)\n" + t.round(0).to_string())
        if rets:
            base = [c for c in ("tournament", "reactive", "lamport") if c in t.columns]
            rel = pd.DataFrame(index=t.index)
            for c in rets:
                rel[c + "/tour"] = t[c] / t["tournament"]
                rel[c + "/best"] = t[c] / t[base].max(axis=1)
                if "bound" in t.columns:
                    rel[c + "/bound"] = t[c] / t["bound"]
            out.append("ratios\n" + rel.round(2).to_string())
            summ = rel.groupby(level="subset").agg(["min", "median", "max"]).round(2)
            out.append("ratios by subset (min / median / max over cells)\n" + summ.T.to_string())
    al = d[d.subset == "all"]
    if len(al):
        a = al.pivot_table(index=["mode", "NT", "locks"], columns="variant",
                           values="achieved_rps", aggfunc="median") / 1e3
        cols = list(a.columns)
        for c in [c for c in cols if c.startswith("retain")]:
            a[c + "/tour"] = a[c] / a["tournament"]
        out.append("every node active, think 0 (k acq/s; ratios vs tournament)\n" + a.round(2).to_string())
    cnt = [c for c in ("ret_hits_per_acq", "ret_skipped_per_acq", "ret_policy_skips_per_acq",
                       "ret_cap_lowers_per_acq", "ret_cap_raises_per_acq") if c in d.columns]
    rr = d[d.variant.str.startswith("retain")]
    if len(rr) and cnt:
        c = rr.groupby(["variant", "subset", "mode", "NT", "think"])[cnt].median()
        c.columns = [x.replace("ret_", "").replace("_per_acq", "") for x in cnt]
        out.append("retention counters per acquisition (median)\n" + c.round(4).to_string())
    return "\n\n".join(out)


def g2bound(d):
    """Phase g2bound: g2's hot-path bounds vs the best existing lock (tournament,
    reactive, g1) at the subsets they model, and the cold path's extra level."""
    d = d.copy()
    d["variant"] = d.apply(g1_variant, axis=1).replace({"retain_acq": "g1", "tournament": "tour",
                                                         "reactive": "react"})
    tag = d.config.str.split(":").str[1]
    d["tag"] = tag.str.split("@").str[0]
    d["N"] = tag.str.split("@").str[1].astype(int)
    d["NT"] = d.N.astype(str) + "x" + d.threads_per_node.astype(str)
    d["mode"] = d["mode"].replace({"phase1": "P1", "local": "2a"})
    d["think"] = (d.think_ns / 1000).map("{:g}us".format)
    bnd = d.tag.str.startswith(("hot", "cold"))
    d.loc[bnd, "col"] = d.tag
    d.loc[~bnd, "col"] = d.tag + ":" + d.variant
    t = d.pivot_table(index=["mode", "NT", "think"], columns="col", values="achieved_rps",
                      aggfunc="median") / 1e3
    def best(sub):
        return t[[c for c in t.columns if c.startswith(sub + ":")]].max(axis=1)
    r = pd.DataFrame(index=t.index)
    r["near2 hot2_k2/best"] = t["hot2_k2"] / best("near2")
    r["near2 hot2_k4/best"] = t["hot2_k4"] / best("near2")
    r["near4 hot4_k4/best"] = t["hot4_k4"] / best("near4")
    r["one hot1_k2/best"] = t["hot1_k2"] / best("one")
    r["far2 hot2_k4/best"] = t["hot2_k4"] / best("far2")
    r["far4 hot4_k4/best"] = t["hot4_k4"] / best("far4")
    r["cold1/one:tour"] = t["cold1"] / t["one:tour"]
    r["coldall/all:tour"] = t["coldall"] / t["all:tour"]
    cols = [c for c in ["near2:tour", "near2:react", "near2:g1", "hot2_k2", "hot2_k4",
                        "near4:tour", "near4:react", "near4:g1", "hot4_k4",
                        "one:tour", "one:react", "one:g1", "hot1_k2", "cold1",
                        "far2:g1", "far4:g1", "all:tour", "coldall"] if c in t.columns]
    out = ["k acq/s, median over reps (best = max of tournament, reactive, g1 at that subset)\n"
           + t[cols].round(0).to_string(),
           "ratios\n" + r.round(2).to_string(),
           "ratios (min / median / max over cells)\n"
           + r.agg(["min", "median", "max"]).T.round(2).to_string()]
    g = r[["near2 hot2_k2/best", "near2 hot2_k4/best"]].max(axis=1)
    g2 = g[g.index.get_level_values("think") == "2us"]
    out.append("go criterion: near2 cells where max(hot2_k2, hot2_k4) >= 1.2x best: %d of %d "
               "(think 2us: %d of %d)" % ((g >= 1.2).sum(), len(g), (g2 >= 1.2).sum(), len(g2)))
    return "\n\n".join(out)


def kfix(d):
    """Phase kfix: the stride rule (default) vs --raw-layout."""
    d = d.copy()
    def lab(r):
        if r.alg == "reactive":
            return "reactive " + str(r.react)
        if r.alg == "lamport":
            return "plain lamport"
        return "fixed top%d" % r.tournament_locks
    d["variant"] = d.apply(lab, axis=1)
    d["layout"] = d.raw_layout.map({0: "rule", 1: "raw"})
    r = d.groupby(["config", "variant", "layout"]).agg(
        lines=("lines_per_lock", "first"), thr=("achieved_rps", "median"),
        lo=("achieved_rps", "min"), hi=("achieved_rps", "max"),
        p50=("lock_p50", "median"), p99=("lock_p99", "median"), err=("errors", "max"))
    for k in ("thr", "lo", "hi"):
        r[k] = (r[k] / 1e3).round(0)
    r["p99"] = (r.p99 / 1e3).round(1)
    out = "k ops/s (median, min, max), lock p50 ns / p99 us; lines = meta lines per lock\n" + r.to_string()
    if d.errors.max() > 0:
        out += "\n!!! %d runs with KV check errors" % (d.errors > 0).sum()
    return out


def kstride(d):
    """Phase kstride: throughput vs lock-block stride."""
    d = d.copy()
    d["stride_B"] = d.lines_per_lock * 64
    r = d.groupby(["config", "lines_per_lock", "stride_B"]).agg(
        n=("achieved_rps", "size"), thr=("achieved_rps", "median"), lo=("achieved_rps", "min"),
        hi=("achieved_rps", "max"), p50=("lock_p50", "median"), err=("errors", "max"))
    for k in ("thr", "lo", "hi"):
        r[k] = (r[k] / 1e3).round(0)
    r["stride_mod_1K"] = r.index.get_level_values("stride_B") % 1024
    out = "k ops/s (median, min, max) by lines per lock (stride = 64 B x lines)\n" + r.to_string()
    if d.errors.max() > 0:
        out += "\n!!! %d runs with KV check errors" % (d.errors > 0).sum()
    return out


def klayout(d):
    """Phase klayout: reactive block layout and stride vs fixed Lamport."""
    d = d.copy()
    def lab(r):
        if r.alg == "reactive":
            return "reactive %s %s" % (r.react, r.react_layout)
        if r.alg == "lamport":
            return "plain lamport (2 lines)"
        return "fixed top%d%s" % (r.tournament_locks, " pad%d" % r.pad_lines if r.pad_lines else "")
    d["variant"] = d.apply(lab, axis=1)
    r = d.groupby(["config", "variant"]).agg(
        n=("achieved_rps", "size"), thr=("achieved_rps", "median"), lo=("achieved_rps", "min"),
        hi=("achieved_rps", "max"), p50=("lock_p50", "median"), p99=("lock_p99", "median"),
        slow=("lamport_slow_frac", "median"), smr=("slow_mode_reads_per_acq", "median"),
        err=("errors", "max"))
    for k in ("thr", "lo", "hi"):
        r[k] = (r[k] / 1e3).round(0)
    r["p99"] = (r.p99 / 1e3).round(1)
    out = "k ops/s (median, min, max), lock p50 ns / p99 us, Lamport slow-path share and slow-path mode reads per global acquisition\n" + r.to_string()
    if d.errors.max() > 0:
        out += "\n!!! %d runs with KV check errors" % (d.errors > 0).sum()
    return out


def kvbase(d):
    """Phase kvbase: coherent baselines vs the non-coherent locks."""
    d = d.copy()
    def lab(r):
        if r.backend == "coherent":
            if r.alg == "pthread":
                return "coh pthread" + (" no-submutex" if r.no_submutex else " +submutex")
            return "coh %s 2a" % r.alg
        if r.alg == "reactive":
            return "cf reactive " + str(r.react)
        return "cf top%d map" % r.tournament_locks
    d["variant"] = d.apply(lab, axis=1)
    d["keys"] = d.apply(lambda r: "uniform" if r.dist == "uniform" else "zipf%.2f" % r.theta, axis=1)
    out = []
    p = d[d.config == "closed"]
    if len(p):
        r = p.groupby(["keys", "locks", "variant"]).agg(
            thr=("achieved_rps", "median"), p50=("lock_p50", "median"), p99=("lock_p99", "median"),
            fair=("node_min_max_ratio", "median"), err=("errors", "max"))
        r["thr"] = (r.thr / 1e3).round(0)
        r["p99"] = (r.p99 / 1e3).round(1)
        out.append("closed loop: k ops/s, lock p50 ns, p99 us\n" + r.to_string())
    p = d[d.config == "open"]
    if len(p):
        r = p.groupby(["target_rps", "variant"]).agg(
            ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
            p50=("lock_p50", "median"), p99=("lock_p99", "median"))
        r["ach"] = (r.ach / 1e3).round(0)
        r["p50"] = (r.p50 / 1e3).round(2)
        r["p99"] = (r.p99 / 1e3).round(1)
        out.append("open loop (Zipf 0.99, L=1024): k/s, lock latency us\n" + r.to_string())
    if d.errors.max() > 0:
        out.append("!!! %d runs with KV check errors" % (d.errors > 0).sum())
    return "\n\n".join(out)


def kviso(d):
    """Phase kviso: reactive pinned (switching off) vs fixed maps vs reactive."""
    d = d.copy()
    d["part"] = d.config.str.split(":").str[1]
    def lab(r):
        if r.alg == "reactive" and r.react == "off":
            return "reactive pinned top%d" % r.tournament_locks
        if r.alg == "reactive":
            return "reactive " + str(r.react)
        return "fixed top%d" % r.tournament_locks
    d["variant"] = d.apply(lab, axis=1)
    d["keys"] = d.apply(lambda r: "uniform" if r.dist == "uniform" else "zipf%.2f" % r.theta, axis=1)
    out = []
    p = d[d.part == "closed"]
    if len(p):
        r = p.groupby(["keys", "locks", "variant"]).agg(
            thr=("achieved_rps", "median"), p50=("lock_p50", "median"), p99=("lock_p99", "median"),
            tour=("tournament_acq_frac", "median"), err=("errors", "max"))
        r["thr"] = (r.thr / 1e3).round(0)
        r["p99"] = (r.p99 / 1e3).round(1)
        out.append("closed loop: k ops/s, lock p50 ns, p99 us, tournament share\n" + r.to_string())
    p = d[d.part == "open"]
    if len(p):
        r = p.groupby(["target_rps", "variant"]).agg(
            ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
            p50=("lock_p50", "median"), p99=("lock_p99", "median"), p999=("lock_p999", "median"),
            tour=("tournament_acq_frac", "median"))
        r["ach"] = (r.ach / 1e3).round(0)
        for k in ("p50", "p99", "p999"):
            r[k] = (r[k] / 1e3).round(2)
        out.append("open loop (Zipf 0.99, L=1024): k/s, lock latency us\n" + r.to_string())
    if d.errors.max() > 0:
        out.append("!!! %d runs with KV check errors" % (d.errors > 0).sum())
    return "\n\n".join(out)


def kvreact(d):
    """Phase kvreact: reactive lock vs fixed maps and the server chain."""
    d = d.copy()
    d["part"] = d.config.str.split(":").str[1]
    d["variant"] = d.apply(lambda r: "server-chain" if r.alg == "server"
                           else "reactive " + str(r.react) if r.alg == "reactive"
                           else "all-lamport" if r.tournament_locks == 0
                           else "all-tournament" if r.tournament_locks == r.locks
                           else "top%d-tournament" % r.tournament_locks, axis=1)
    d["keys"] = d.apply(lambda r: ("uniform" if r.dist == "uniform" else "zipf%.2f" % r.theta)
                        + ("+aff%.1f" % r.affinity if r.affinity > 0 else ""), axis=1)
    out = []
    p = d[d.part == "closed"]
    if len(p):
        r = p.groupby(["keys", "locks", "variant"]).agg(
            thr=("achieved_rps", "median"), p99=("lock_p99", "median"),
            fair=("node_min_max_ratio", "median"), tour=("tournament_acq_frac", "median"),
            switches=("mode_switches", "median"), err=("errors", "max"))
        r["thr"] = (r.thr / 1e3).round(0)
        r["p99"] = (r.p99 / 1e3).round(1)
        out.append("closed loop (k ops/s, lock p99 us, tour = tournament share of global acq.)\n"
                   + r.round(3).to_string())
    p = d[d.part == "open"]
    if len(p):
        r = p.groupby(["target_rps", "variant"]).agg(
            ach=("achieved_rps", "median"), unserved=("unserved_frac", "median"),
            p50=("lock_p50", "median"), p99=("lock_p99", "median"))
        r["ach"] = (r.ach / 1e3).round(0)
        r["p50"] = (r.p50 / 1e3).round(2)
        r["p99"] = (r.p99 / 1e3).round(1)
        out.append("open loop (L=1024, Zipf 0.99): k/s, lock p50/p99 us\n" + r.to_string())
    if d.errors.max() > 0:
        out.append("!!! %d runs with KV check errors" % (d.errors > 0).sum())
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
                              "conc", "open2b", "mt", "kv", "kvreact", "kviso", "kvbase", "klayout", "kstride", "kfix", "g1bound", "g1", "g2bound"]
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
        elif ph == "kvreact":
            print(kvreact(d))
        elif ph == "kviso":
            print(kviso(d))
        elif ph == "kvbase":
            print(kvbase(d))
        elif ph == "klayout":
            print(klayout(d))
        elif ph == "kstride":
            print(kstride(d))
        elif ph == "kfix":
            print(kfix(d))
        elif ph in ("g1", "g1bound"):
            print(g1(d))
        elif ph == "g2bound":
            print(g2bound(d))
        else:
            print(closed(d, EXTRA.get(ph, ())).to_string())
        print()
    err = os.path.join(root, "errors.log")
    if os.path.exists(err) and os.path.getsize(err):
        print(f"!!! {err} is not empty")


if __name__ == "__main__":
    main()
