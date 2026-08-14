#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt

LOG_RE = re.compile(
    r"Switch is\s+(?P<switch>\S+)\s+-\s+Flow is\s+(?P<flow>\d+)\s+-\s+Subflow id is\s+(?P<subflow>\d+)\s+-\s+Time is\s+(?P<time>\d+)\s*$"
)

UNIT_TO_PS = {
    "ps": 1.0,
    "ns": 1e3,
    "us": 1e6,
    "µs": 1e6,
    "ms": 1e9,
    "s": 1e12,
}

def parse_time_str(s: Optional[str]) -> Optional[int]:
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    m = re.match(r"^\s*([0-9]*\.?[0-9]+)\s*([a-zA-Zµ]+)?\s*$", s)
    if not m:
        raise ValueError(f"Invalid time string: {s}")
    val = float(m.group(1))
    unit = (m.group(2) or "ps").lower()
    if unit not in UNIT_TO_PS:
        raise ValueError(f"Unsupported time unit: {unit}")
    return int(round(val * UNIT_TO_PS[unit]))

def format_time_ps(ps: int, unit: str) -> float:
    # Convert picoseconds to chosen unit value
    if unit not in UNIT_TO_PS:
        raise ValueError(f"Unsupported plot unit: {unit}")
    return ps / UNIT_TO_PS[unit]

def read_logs(paths: List[str]) -> pd.DataFrame:
    records = []
    def iter_lines(path: str) -> Iterable[str]:
        if path == "-" or path == "/dev/stdin":
            for line in sys.stdin:
                yield line
        else:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    yield line

    for p in paths:
        for line in iter_lines(p):
            m = LOG_RE.search(line)
            if not m:
                continue
            switch = m.group("switch")
            # Only consider switches with 'Switch_UpperPod' in the name
            if "Switch_UpperPod" not in switch:
                continue
            flow = int(m.group("flow"))
            subflow = int(m.group("subflow"))
            time_ps = int(m.group("time"))
            records.append((switch, flow, subflow, time_ps))

    if not records:
        return pd.DataFrame(columns=["switch", "flow", "subflow", "time_ps", "flow_key"])

    df = pd.DataFrame(records, columns=["switch", "flow", "subflow", "time_ps"])
    # Unique flow identity = (flow, subflow)
    df["flow_key"] = df["flow"].astype(str) + "-" + df["subflow"].astype(str)
    # Drop exact duplicates, if any
    df = df.drop_duplicates(subset=["switch", "flow", "subflow", "time_ps"])
    return df

def filter_df(
    df: pd.DataFrame,
    switches: Optional[List[str]],
    link_pattern: Optional[str],
    tstart_ps: Optional[int],
    tend_ps: Optional[int],
) -> pd.DataFrame:
    out = df
    if switches:
        out = out[out["switch"].isin(switches)]
    if link_pattern:
        out = out[out["switch"].str.contains(link_pattern, regex=True)]
    if tstart_ps is not None:
        out = out[out["time_ps"] >= tstart_ps]
    if tend_ps is not None:
        out = out[out["time_ps"] <= tend_ps]
    return out

def bin_counts(df: pd.DataFrame, bin_ps: int, t0_ps: int, t1_ps: int) -> pd.DataFrame:
    if df.empty:
        return pd.DataFrame(columns=["switch", "bin_start_ps", "count"])
    # Align bins to [t0, t1]
    # Compute bin start for each record
    bin_index = (df["time_ps"] - t0_ps) // bin_ps
    bin_start_ps = bin_index * bin_ps + t0_ps
    temp = df.assign(bin_start_ps=bin_start_ps)
    # Count unique flow_key per bin per switch
    g = temp.groupby(["switch", "bin_start_ps"])["flow_key"].nunique().reset_index(name="count")
    # Ensure empty bins are present with zero count for nice plotting
    all_bins = np.arange(t0_ps, t1_ps + 1, bin_ps, dtype=np.int64)
    switches = sorted(temp["switch"].unique())
    idx = pd.MultiIndex.from_product([switches, all_bins], names=["switch", "bin_start_ps"])
    g_full = g.set_index(["switch", "bin_start_ps"]).reindex(idx, fill_value=0).reset_index()
    return g_full

def summarize_unique(df: pd.DataFrame) -> pd.DataFrame:
    if df.empty:
        return pd.DataFrame(columns=["switch", "unique_subflows"])
    return df.groupby("switch")["flow_key"].nunique().reset_index(name="unique_subflows")

def plot_timeseries(
    binned: pd.DataFrame,
    unit: str,
    title: Optional[str],
    out_path: Optional[str],
    dpi: int,
):
    sns.set_theme(style="whitegrid", palette="Set2", font="serif")
    plt.figure(figsize=(10, 5.5))
    ax = plt.gca()

    if binned.empty:
        ax.text(0.5, 0.5, "No data to plot", ha="center", va="center", fontsize=12, transform=ax.transAxes)
        plt.tight_layout()
        if out_path:
            plt.savefig(out_path, dpi=dpi, bbox_inches="tight")
        else:
            plt.show()
        return

    # Plot each switch as a step line for crisp interpretation
    for i, (switch, g) in enumerate(binned.sort_values(["switch", "bin_start_ps"]).groupby("switch")):
        x = format_time_ps(g["bin_start_ps"].to_numpy(), unit)
        y = g["count"].to_numpy()
        plt.step(x, y, where="post", linewidth=2.0, label=switch)
        plt.plot(x, y, marker="o", linestyle="none", markersize=3, alpha=0.8)

    ax.set_xlabel(f"Time [{unit}]")
    ax.set_ylabel("Unique subflows per bin")
    if title:
        ax.set_title(title)
    sns.despine()
    plt.tight_layout()

    if out_path:
        plt.savefig(out_path, dpi=dpi, bbox_inches="tight")
    else:
        plt.show()

def auto_bin_ps(t0_ps: int, t1_ps: int) -> int:
    # Heuristic: ~100 bins across window, clamped to reasonable picoseconds
    span = max(1, t1_ps - t0_ps)
    target_bins = 100
    raw = max(1, int(round(span / target_bins)))
    # Snap to 1-2-5 engineering steps in ps
    steps = np.array([1, 2, 5])
    exp = int(np.floor(np.log10(raw))) if raw > 0 else 0
    candidates = steps * (10 ** exp)
    bin_ps = int(candidates[np.argmin(np.abs(candidates - raw))])
    return max(1, bin_ps)

def main():
    parser = argparse.ArgumentParser(
        description="Analyze logs and plot how many unique subflows transit a link (switch) over time."
    )
    parser.add_argument("logs", nargs="+", help="Log file(s) to parse, or - for stdin")
    parser.add_argument("--link", dest="switches", nargs="*", default=None, help="One or more switch names to include")
    parser.add_argument("--link-pattern", default=None, help="Regex to match switch names")
    parser.add_argument("--tstart", default=None, help="Start time (e.g., 90us, 0.1ms, 90435520ps)")
    parser.add_argument("--tend", default=None, help="End time (inclusive) with units, same format as --tstart")
    parser.add_argument("--bin", dest="bin_str", default=None, help="Bin size (e.g., 100ns, 1us). Auto if omitted.")
    parser.add_argument("--unit", default="us", choices=list(UNIT_TO_PS.keys()), help="Time unit for x-axis")
    parser.add_argument("--out", default=None, help="Output plot file (e.g., plot.png). If omitted, shows window.")
    parser.add_argument("--dpi", type=int, default=160, help="Figure DPI")
    parser.add_argument("--title", default=None, help="Custom plot title")
    parser.add_argument("--no-plot", action="store_true", help="Skip plotting and only print summary")
    args = parser.parse_args()

    df = read_logs(args.logs)
    if df.empty:
        print("No valid log lines found.", file=sys.stderr)
        sys.exit(1)

    # Determine time window
    tmin_ps = int(df["time_ps"].min())
    tmax_ps = int(df["time_ps"].max())
    tstart_ps = parse_time_str(args.tstart) if args.tstart is not None else tmin_ps
    tend_ps = parse_time_str(args.tend) if args.tend is not None else tmax_ps
    if tstart_ps > tend_ps:
        print("Error: tstart must be <= tend", file=sys.stderr)
        sys.exit(1)

    # Filter
    dff = filter_df(df, args.switches, args.link_pattern, tstart_ps, tend_ps)
    if dff.empty:
        print("No records match the filters (link/time).", file=sys.stderr)
        sys.exit(1)

    # Summary of unique subflows in window
    summary = summarize_unique(dff)
    print("Unique subflows in window per link (switch):")
    for _, row in summary.iterrows():
        print(f"  {row['switch']}: {row['unique_subflows']}")

    # NEW: list all active unique flows (flow id + subflow id) in the filtered window
    active_flows = sorted({(f, s) for f, s in zip(dff['flow'], dff['subflow'])})
    print("Active unique flows (flow_id + subflow_id):")
    for f_id, sub_id in active_flows:
        print(f"  {f_id}+{sub_id}")

    # Bin size
    if args.bin_str:
        bin_ps = parse_time_str(args.bin_str)
        if not bin_ps or bin_ps <= 0:
            print("Invalid bin size.", file=sys.stderr)
            sys.exit(1)
    else:
        bin_ps = auto_bin_ps(tstart_ps, tend_ps)

    # Binning and plotting
    binned = bin_counts(dff, bin_ps=bin_ps, t0_ps=tstart_ps, t1_ps=tend_ps)

    # Title
    if args.title:
        title = args.title
    else:
        window_txt = f"{format_time_ps(tstart_ps, args.unit):.3f}-{format_time_ps(tend_ps, args.unit):.3f} {args.unit}"
        if args.switches:
            which = ", ".join(args.switches)
        elif args.link_pattern:
            which = f"pattern: {args.link_pattern}"
        else:
            which = "all links"
        title = f"Unique subflows per bin on {which} | Window: {window_txt} | Bin: {format_time_ps(bin_ps, args.unit):.3f} {args.unit}"

    plot_timeseries(binned, unit=args.unit, title=title, out_path=args.out, dpi=args.dpi)

if __name__ == "__main__":
    main()
