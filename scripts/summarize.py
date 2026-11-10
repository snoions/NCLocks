#!/usr/bin/env python3
"""Summarize a sweep CSV: group repetitions, report median and [min, max].

Usage: scripts/summarize.py sweep.csv [--metrics lock_p50,lock_p99,lock_p999]
Groups by every configuration column (everything except measurements/rep).
A point is marked SAT if any repetition left >0.1% of arrivals unserved.
"""
import argparse, csv, statistics, sys
from collections import OrderedDict

CONFIG = ["alg", "backend", "nodes", "threads_per_node", "locks", "cs_ns", "think_ns",
          "footprint", "dev_lat_ns", "no_submutex", "mode", "local_cap", "spin_backoff", "server_backoff", "arrival", "target_rps"]

def fmt_ns(v):
    if v >= 1e6: return f"{v/1e6:.1f}ms"
    if v >= 1e3: return f"{v/1e3:.1f}us"
    return f"{v:.0f}ns"

def fmt_rate(v):
    return f"{v/1e6:.2f}M" if v >= 1e6 else f"{v/1e3:.0f}k"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--metrics", default="lock_p50,lock_p99,lock_p999")
    args = ap.parse_args()
    rows = list(csv.DictReader(open(args.csv)))
    if not rows:
        sys.exit("no rows")
    metrics = args.metrics.split(",")
    # Only show config columns that vary, to keep the table narrow.
    varying = [c for c in CONFIG if len({r[c] for r in rows}) > 1]
    groups = OrderedDict()
    for r in rows:
        groups.setdefault(tuple(r[c] for c in CONFIG), []).append(r)

    hdr = varying + ["n", "achieved"] + metrics + ["fast%", ""]
    out = [hdr]
    for key, rs in groups.items():
        cfg = dict(zip(CONFIG, key))
        line = [cfg[c] if c != "target_rps" else fmt_rate(float(cfg[c])) for c in varying]
        line.append(str(len(rs)))
        ach = [float(r["achieved_rps"]) for r in rs]
        line.append(fmt_rate(statistics.median(ach)))
        for m in metrics:
            v = [float(r[m]) for r in rs]
            med = statistics.median(v)
            line.append(fmt_ns(med) if len(v) == 1 else
                        f"{fmt_ns(med)} [{fmt_ns(min(v))}-{fmt_ns(max(v))}]")
        line.append(f"{100*statistics.median(float(r['fast_frac']) for r in rs):.0f}")
        line.append("SAT" if any(float(r["unserved_frac"]) > 0.001 for r in rs) else "")
        out.append(line)
    w = [max(len(row[i]) for row in out) for i in range(len(hdr))]
    for row in out:
        print("  ".join(c.ljust(w[i]) for i, c in enumerate(row)).rstrip())

if __name__ == "__main__":
    main()
