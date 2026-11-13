#!/usr/bin/env python3
"""Median tables for a scripts/cxl_sweep.sh result directory (needs pandas).

Usage: scripts/cxl_summary.py results/cxl_<date>/ [phase ...]
Phases: p1 abl 2a 2b own locks base open dram (default: those whose CSV exists).
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


EXTRA = {
    "p1": ["fast_frac", "lock_p99"],
    "2a": ["local_handoff_frac"],
    "2b": ["global_handoff_frac", "owner_polls_per_acq", "lock_p99"],
    "own": ["owner_polls_per_acq", "grant_polls_per_acq"],
    "locks": ["fast_frac", "lock_p99"],
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    phases = sys.argv[2:] or ["p1", "abl", "2a", "2b", "own", "locks", "base", "open", "dram"]
    for ph in phases:
        f = os.path.join(root, ph + ".csv")
        if not os.path.exists(f):
            continue
        d = pd.read_csv(f)
        print(f"=== {ph} ({len(d)} rows)")
        if ph == "own":
            d["config"] = d.config + "/" + d["wait"]
        if ph == "open":
            print(open_loop(d).to_string())
        else:
            print(closed(d, EXTRA.get(ph, ())).to_string())
        print()
    err = os.path.join(root, "errors.log")
    if os.path.exists(err) and os.path.getsize(err):
        print(f"!!! {err} is not empty")


if __name__ == "__main__":
    main()
