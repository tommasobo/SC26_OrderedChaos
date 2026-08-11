#!/usr/bin/env python3
import argparse
import re
from collections import defaultdict
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt


DROP_RE = re.compile(r'^Drop:\s*FlowID\s+(\d+)\s*-\s*Packet ID\s+(\d+)\s*-\s*Time\s+(\d+)', re.IGNORECASE)
RTX_RE  = re.compile(r'^RTX:\s*FlowID\s+(\d+)\s*-\s*Packet ID\s+(\d+)\s*-\s*Time\s+(\d+)', re.IGNORECASE)

# Use picoseconds as the base unit internally
UNITS = {
    "ps": 1.0,
    "ns": 1e-3,
    "us": 1e-6,
    "ms": 1e-9,
    "s":  1e-12,
}

# Mapping to pretty-print/rename x-axis labels in the multi-file plot.
# Fill this in, e.g., {"motiv_run1": "Baseline", "motiv_run2": "ECN On"}
X_LABEL_MAP = {
    # "old_label": "New Label",
    "nodrop_no_trim_rss": "RTO N",
    "nodrop_yes_trim_rss": "Trimming N",
    "yesdrop_yes_trim_rss": "Trimming Y",
    "nodrop_pfld_rss": "PFLD N ",
    "yesdrop_no_trim_rss": "RTO Y",
    "yesdrop_pfld_rss": "PFLD Y",
}

# Global text scale for all plots
TEXT_SCALE = 0.83


def _sns_violin_kwargs():
    """Return seaborn violinplot kwargs to normalize by width, compatible across versions."""
    try:
        # Minimal version check without extra deps
        major_minor = tuple(int(p) for p in sns.__version__.split(".")[:2])
        if major_minor >= (0, 13):
            return {"density_norm": "width"}
    except Exception:
        pass
    return {"scale": "width"}


def parse_args():
    ap = argparse.ArgumentParser(
        description="Compute and plot RTX detection delay (RTX time - Drop time) from msft-htsim logs."
    )
    ap.add_argument("logfile", type=Path, help="Path to simulator log file or directory (e.g., tmp.out or motiv/)")
    ap.add_argument("--out", type=Path, default=Path("rtx_detection_violin.png"),
                    help="Output image path (single-file mode). Directory mode saves to <dir>/combined_violin.png")
    ap.add_argument("--unit", choices=UNITS.keys(), default="us",
                    help="Time unit for plotting (ps, ns, us, ms, s). Default: us")
    ap.add_argument("--dpi", type=int, default=200, help="Figure DPI (default: 200)")
    ap.add_argument("--show", action="store_true", help="Show the plot instead of just saving")
    ap.add_argument("--bar", action="store_true", help="Plot average detection time as a bar chart instead of a violin plot")
    ap.add_argument("--bar-p99", action="store_true", help="With --bar, plot the 99th percentile instead of the average")
    # New: figure size overrides
    ap.add_argument("--fig-w", type=float, default=None, help="Figure width in inches (optional)")
    ap.add_argument("--fig-h", type=float, default=None, help="Figure height in inches (optional)")
    # New: choose a single Set2 color (0-7) for all bars in bar plots
    ap.add_argument("--bar-color", type=int, default=None,
                    help="Uniform bar color index (0-7) from seaborn Set2 palette, used with --bar")
    return ap.parse_args()


def parse_log(log_path: Path):
    # Track earliest drop per (flow, pkt) and first RTX after that drop.
    drop_time_ps = {}
    first_match = set()  # keys already matched to an RTX

    records = []  # list of dicts for DataFrame rows

    with log_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            # Fast-path: ignore non-relevant lines quickly
            if not (line.startswith("Drop:") or line.startswith("RTX:")):
                continue

            m_drop = DROP_RE.match(line)
            if m_drop:
                flow = int(m_drop.group(1))
                pkt  = int(m_drop.group(2))
                ts   = int(m_drop.group(3))  # picoseconds
                key = (flow, pkt)
                # Keep earliest drop; if duplicates occur, use the first one
                if key not in drop_time_ps or ts < drop_time_ps[key]:
                    drop_time_ps[key] = ts
                continue

            m_rtx = RTX_RE.match(line)
            if m_rtx:
                flow = int(m_rtx.group(1))
                pkt  = int(m_rtx.group(2))
                ts   = int(m_rtx.group(3))  # picoseconds
                key = (flow, pkt)

                # Only record the first RTX that occurs after the drop
                if key in drop_time_ps and key not in first_match:
                    dts = drop_time_ps[key]
                    if ts >= dts:
                        first_match.add(key)
                        records.append({
                            "flow_id": flow,
                            "packet_id": pkt,
                            "drop_time_ps": dts,
                            "rtx_time_ps": ts,
                            "detect_delay_ps": ts - dts
                        })
                continue

    return pd.DataFrame.from_records(records)


def _ensure_positive_for_log(series: pd.Series) -> pd.Series:
    vals = series.to_numpy(dtype=float)
    if not np.isfinite(vals).all():
        finite = np.isfinite(vals)
        if finite.any():
            maxf = np.nanmax(vals[finite])
        else:
            maxf = 1.0
        vals = np.nan_to_num(vals, nan=0.0, posinf=maxf, neginf=0.0)
    if (vals > 0).any():
        minpos = float(np.min(vals[vals > 0]))
    else:
        minpos = 1e-12
    vals[vals <= 0] = minpos / 10.0
    return pd.Series(vals, index=series.index)


def plot_violin(
    df: pd.DataFrame,
    out_path: Path,
    unit: str,
    dpi: int,
    show: bool,
    bar: bool,
    bar_p99: bool,
    fig_w: Optional[float] = None,
    fig_h: Optional[float] = None,
    bar_color: Optional[int] = None,  # new
):
    if df.empty:
        print("No matched Drop/RTX pairs found. Nothing to plot.")
        return

    factor = UNITS[unit]
    df = df.copy()
    df["detect_delay_plot"] = df["detect_delay_ps"] * factor
    df["detect_delay_plot"] = _ensure_positive_for_log(df["detect_delay_plot"])  # safe

    sns.set_theme(style="whitegrid", context="talk", font="Liberation Serif", font_scale=TEXT_SCALE)
    # Use a larger Set2 to allow indices 0-7
    set2 = sns.color_palette("Set2", n_colors=8)
    colors = set2.as_hex()

    # Use provided figure size or fallback to 8x6
    sw = fig_w if fig_w is not None else 8.0
    sh = fig_h if fig_h is not None else 6.0
    fig, ax = plt.subplots(figsize=(sw, sh))

    vp_kwargs = _sns_violin_kwargs()
    if bar:
        if bar_p99:
            val = float(np.quantile(df["detect_delay_plot"], 0.99))
            label = "P99"
        else:
            val = float(df["detect_delay_plot"].mean())
            label = "Average"
        # New: uniform Set2 color for all bars if requested
        if bar_color is not None:
            idx = int(max(0, min(len(set2) - 1, bar_color)))
            bar_col = set2[idx]
        else:
            bar_col = set2[3]
        ax.bar([0], [val], color=bar_col, edgecolor='k', linewidth=1.0)
        ax.set_xticks([0])
        ax.set_xticklabels([label])
    else:
        sns.violinplot(
            data=df,
            y="detect_delay_plot",
            inner="quartile",
            cut=0,
            linewidth=1.0,
            color=set2[0],
            ax=ax,
            **vp_kwargs,
        )
        # Median line and 99th percentile marker
        median_all = df["detect_delay_plot"].median()
        p99 = df["detect_delay_plot"].quantile(0.99)
        ax.axhline(median_all, ls="--", lw=1.0, color=set2[1], alpha=0.9)
        ax.scatter(0, p99, s=60, marker='D', color=set2[2], edgecolor='k', linewidths=0.6, zorder=4, clip_on=False)
        ax.set_xticks([])

    # remove legend if any
    if ax.get_legend() is not None:
        ax.get_legend().remove()

    # Replace suptitle with axes title and remove top padding
    ax.set_title("With Corruption Drops", pad=2)

    ax.set_xlabel("")
    ax.set_ylabel(f"Loss Detection Time ({unit})")
    #ax.margins(y=0.08)

    # Remove top padding
    fig.tight_layout(pad=0)
    fig.savefig(out_path, dpi=dpi, bbox_inches="tight")
    print(f"Saved plot to: {out_path.resolve()}")
    if show:
        plt.show()
    plt.close(fig)


# New: multi-file grouped violin/avg-bar plot (2 groups x 3)
def plot_violin_multi(
    df_all: pd.DataFrame,
    out_path: Path,
    unit: str,
    dpi: int,
    show: bool,
    bar: bool,
    bar_p99: bool,
    fig_w: Optional[float] = None,
    fig_h: Optional[float] = None,
    bar_color: Optional[int] = None,  # new
):
    if df_all.empty:
        print("No matched Drop/RTX pairs found across files. Nothing to plot.")
        return

    factor = UNITS[unit]
    df_all = df_all.copy()
    df_all["detect_delay_plot"] = df_all["detect_delay_ps"] * factor
    df_all["detect_delay_plot"] = _ensure_positive_for_log(df_all["detect_delay_plot"])  # safe

    # Derive grouping and Algorithm from file stem
    drop_alg = df_all["log_label"].apply(_classify_log_label)
    df_all["drop_group"] = drop_alg.apply(lambda t: t[0])
    df_all["Algorithm"] = drop_alg.apply(lambda t: t[1])

    # Orders PFLD With\nProactive Probes
    drop_order = [x for x in ["PFLD Without\nProactive Probes", "PFLD With\nProactive Probes"] if x in df_all["drop_group"].unique().tolist()]
    preferred_alg_order = ["RTO", "Trimming", "PFLD"]
    present_algs = [a for a in preferred_alg_order if a in df_all["Algorithm"].unique().tolist()]
    if any(df_all["Algorithm"] == "Other"):
        present_algs.append("Other")

    sns.set_theme(style="whitegrid", context="talk", font="Liberation Serif", font_scale=TEXT_SCALE)
    # Default palette for non-uniform cases
    default_palette = sns.color_palette("Set2", n_colors=max(3, len(present_algs)))
    # array of colors in current palette for selection
    colors = default_palette.as_hex()

    # Use provided figure size or compute width based on groups if not provided
    auto_w = max(8, min(24, 1.6 * len(drop_order) * len(present_algs) / 2))
    mw = fig_w if fig_w is not None else auto_w
    mh = fig_h if fig_h is not None else 6.0
    fig, ax = plt.subplots(figsize=(mw, mh))

    vp_kwargs = _sns_violin_kwargs()
    if bar:
        estimator_fn = (lambda x: np.quantile(x, 0.99)) if bar_p99 else np.mean
        # New: build a uniform palette mapping all hue levels to the same Set2 color if requested
        palette_arg = None
        if bar_color is not None:
            set2 = sns.color_palette("Set2", n_colors=8)
            idx = int(max(0, min(len(set2) - 1, bar_color)))
            uni = set2[idx]
            palette_arg = {alg: uni for alg in present_algs}
        else:
            palette_arg = default_palette

        sns.barplot(
            data=df_all,
            x="drop_group",
            y="detect_delay_plot",
            hue="Algorithm",
            order=drop_order,
            hue_order=present_algs,
            palette=palette_arg,
            estimator=estimator_fn,
            errorbar=None,
            edgecolor='k',
            linewidth=1.0,
            ax=ax,
        )

        # --- NEW: annotate % reduction on the right bar(s) vs the left bar(s) ---
        try:
            if len(drop_order) == 2 and len(present_algs) >= 1:
                left_group, right_group = drop_order[0], drop_order[1]
                # values used by bars (match seaborn's estimator)
                agg = (
                    df_all.groupby(["drop_group", "Algorithm"])["detect_delay_plot"]
                    .apply(lambda s: float(estimator_fn(s)))
                )

                patches = ax.patches  # bars drawn by seaborn
                n_groups = len(drop_order)  # expected 2
                n_per_group = int(len(patches) / n_groups) if n_groups else 0

                # Only proceed when the bar layout matches our expectations
                if n_groups == 2 and n_per_group == len(present_algs):
                    # group 0 = left, group 1 = right; bars within group follow hue_order (present_algs)
                    for j, alg in enumerate(present_algs):
                        left_val = agg.get((left_group, alg), np.nan)
                        right_val = agg.get((right_group, alg), np.nan)
                        if not (np.isfinite(left_val) and left_val > 0 and np.isfinite(right_val)):
                            continue
                        reduction = (left_val - right_val) / left_val * 100.0

                        left_bar = patches[0 * n_per_group + j]
                        right_bar = patches[1 * n_per_group + j]
                        x = right_bar.get_x() + right_bar.get_width() / 2.0
                        y = right_bar.get_height()
                        y_off = 0.02 * (ax.get_ylim()[1] - ax.get_ylim()[0])
                        ax.text(x, y + y_off, f"-{reduction:.0f}%", ha="center", va="bottom", fontsize=13)
        except Exception:
            # Be robust: if anything goes wrong, skip annotations silently
            pass
        # --- END NEW ---
    else:
        sns.violinplot(
            data=df_all,
            x="drop_group",
            y="detect_delay_plot",
            hue="Algorithm",
            order=drop_order,
            hue_order=present_algs,
            palette=default_palette,
            inner="quartile",
            cut=0,
            linewidth=1.0,
            ax=ax,
            **vp_kwargs,
        )

    # remove legend if any
    if ax.get_legend() is not None:
        ax.get_legend().remove()

    # Replace suptitle with axes title and remove top padding
    ax.set_title("With Corruption Drops", pad=2)

    ax.set_xlabel("")
    ax.set_ylabel(f"99th loss detection delay ({unit})", labelpad=1)
    ax.tick_params(axis='x', rotation=0)

    # Remove top padding
    fig.tight_layout(pad=0)
    # Keep existing filename but trim padding tightly
    fig.savefig("pfld_proactive.pdf", dpi=dpi, bbox_inches="tight")
    print(f"Saved combined plot to: {out_path.resolve()}")
    plt.show()
    plt.close(fig)


def _classify_log_label(stem: str):
    s = stem.lower()
    drop_group = "PFLD Without\nProactive Probes" if "nodrop" in s else "PFLD With\nProactive Probes"
    if "pfld" in s:
        alg = "PFLD"
    elif "yes_trim" in s or "yestrim" in s or "trim_yes" in s:
        alg = "Trimming"
    elif "no_trim" in s or "notrim" in s or "trim_no" in s:
        alg = "RTO"
    else:
        alg = "Other"
    return drop_group, alg


def main():
    args = parse_args()
    path = args.logfile
    if not path.exists():
        print(f"Input path not found: {path}")
        return

    if path.is_dir():
        files = sorted(list(path.glob("*.out")) + list(path.glob("*.log")))
        if not files:
            print(f"No log files (*.out, *.log) found in: {path}")
            return
        dfs = []
        for lf in files:
            df = parse_log(lf)
            print(f"[{lf.name}] Matched Drop->RTX pairs: {len(df)}")
            if len(df):
                df = df.copy()
                df["log_label"] = lf.stem
                dfs.append(df)
        df_all = pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()
        out_path = path / "combined_violin.png"
        plot_violin_multi(df_all, out_path, args.unit, args.dpi, args.show, args.bar, args.bar_p99, args.fig_w, args.fig_h, args.bar_color)
    else:
        if not path.exists():
            print(f"Log file not found: {path}")
            return
        df = parse_log(path)
        print(f"Matched Drop->RTX pairs: {len(df)}")
        if len(df):
            factor = UNITS[args.unit]
            colname = f"delay_{args.unit}"
            summary = (df.assign(**{colname: df["detect_delay_ps"] * factor})
                         .groupby("flow_id")[colname].agg(["count", "median", "mean"]) 
                         .sort_values("median", ascending=False))
            with pd.option_context("display.max_rows", None, "display.precision", 3):
                print(summary)
        plot_violin(df, args.out, args.unit, args.dpi, args.show, args.bar, args.bar_p99, args.fig_w, args.fig_h, args.bar_color)


if __name__ == "__main__":
    main()
