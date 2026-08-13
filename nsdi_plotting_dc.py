#!/usr/bin/env python3
"""
nsdi_plotting.py

Reads an experiment root folder that contains subfolders for different drop rates (e.g., drop_0, drop_0.0005).
Each drop-rate subfolder contains text files named after an algorithm plus optional seed and trim status:
  <algo>_trim_on.txt
  <algo>_trim_off.txt
  <algo>_seed1_trim_on.txt
  <algo>_seed2_trim_off.txt
Log lines contain substrings like:
  Flow Uec_28_92 flowId 29 uecSrc 28 finished at 94.5171 total packets 977 ... total bytes 1003520 ...
We extract the numeric value after 'finished at' and the flow size from 'total bytes'.

Dataframe columns: algorithm, trim_status (on/off), seed, drop_category (0 or >0), completion_time, size_bin.

Plot: 2x3 grid (two rows for drop category, three columns for flow-size bins: 0-10 KB, 10 KB-1 MiB, >1 MiB). Within each subplot violins per algorithm with hue=trim_status.
If multiple seeds are detected, all seeds' completion times are aggregated in the same violins (combined distribution).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Dict, Tuple

import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter
import numpy as np

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif", "DejaVu Serif"],
    "mathtext.fontset": "dejavuserif",
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})

# User-editable configuration (no CLI required)
FIG_WIDTH = 15   # Set a float (inches) to override automatic width; None keeps auto width
FIG_HEIGHT = 5.5   # Figure height in inches
X_LABEL_MAP: Dict[str, str] = {
    "ecmp": "ECMP",
    "reps": "REPS",
    "rss": "RSS",
    "oblivious": "Oblivious",
    "flowbender": "PLB",
    "flowlet": "Flowlet",
    "rss1": "RSS+PFLD",
    "rss2": "RSS+PFLD",
}

FINISHED_AT_RE = re.compile(r"finished at ([0-9]*\.?[0-9]+)")
GLOBAL_TIME_RE = re.compile(r"global time ([0-9]*\.?[0-9]+)")
FLOAT_RE = re.compile(r"([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)")
TOTAL_BYTES_RE = re.compile(r"total bytes (\d+)")
# Supports: algo_trim_on/off  AND  algo_seed3_trim_on/off
FILENAME_RE = re.compile(r"^(?P<algo>.+?)(?:_seed(?P<seed>\d+))?_(?P<trim>trim_on|trim_off)$")
# New: parse lost packets
LOST_PACKETS_RE = re.compile(r"lost packets (\d+)")
# New: parse flowId to deduplicate per file
FLOW_ID_RE = re.compile(r"\bflowId (\d+)")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Create violin plots over drop categories (0 vs >0) and trim status; auto-detect multi-seed.")
    p.add_argument("experiment_root", type=Path, help="Path to root experiment folder containing drop-rate subfolders")
    p.add_argument("--output", "-o", type=str, default="completion_times_violin.png", help="Output plot filename (saved inside root)")
    p.add_argument("--csv", type=str, default=None, help="Optional CSV filename to dump parsed data (inside root)")
    p.add_argument("--min-flows", type=int, default=1, help="Minimum number of flows required to include an algorithm/category")
    p.add_argument("--style", type=str, default="whitegrid", help="Seaborn style")
    p.add_argument("--palette", type=str, default="Set2", help="Seaborn palette")
    # Titles for the two subplots
    p.add_argument("--title-zero", type=str, default="No Corruption Drops", help="Title for the upper subplot (drop=0)")
    p.add_argument("--title-positive", type=str, default="With Corruption Drops", help="Title for the lower subplot (drop>0)")
    # Control inner marks to show median clearly
    p.add_argument("--inner", type=str, choices=['box','quartile','point','stick','none'], default='box', help="Inner representation for violins; 'box' and 'quartile' show the median line")
    # Axis scaling and annotations (figure size controlled by variables above)
    p.add_argument("--separate-y", action='store_true', help="Use separate y-scales for top and bottom subplots (no shared y-axis)")
    p.add_argument("--annot-stats", action='store_true', help="Annotate each violin with median and P99 text")
    # Ideal completion time reference line
    p.add_argument("--ideal-fct", type=float, default=None, help="Ideal completion time (same units as FCT) to draw as a dashed horizontal line")
    # No-show option
    p.add_argument("--no-show", action='store_true', help="Do not display the figure; save PNG and PDF only")
    # New option: only use the maximum completion time per file
    p.add_argument("--only-max", action='store_true', help="Use only the maximum completion time per text file; parses 'global time' for overall run time")
    # Add optional padding to prevent top annotation overflow
    p.add_argument("--padding", action='store_true', help="Add ~10% headroom to the top of the y-axis to avoid annotation overflow")
    # New options: single bottom panel and RSS+PFLD alias selection
    p.add_argument("--only-positive", action='store_true', help="Plot only the positive drop category (>0) and omit the zero-drop subplot")
    p.add_argument("--rss-pfld", choices=['rss1','rss2'], default='rss1', help="Select which RSS+PFLD variant to display (default: rss1)")
    # New option: custom Y-axis label
    p.add_argument("--y-label", type=str, default=None, help="Override Y-axis label (default uses automatic units)")
    # New options: report flow counts for a single configuration
    p.add_argument("--count-algo", type=str, default=None, help="Algorithm to print flow-size bin counts for")
    p.add_argument("--count-trim", choices=['on','off'], default=None, help="Trim status to filter when counting bins")
    p.add_argument("--count-drop", choices=['0','>0'], default=None, help="Drop category to filter when counting bins")
    # New option: remove top outliers per configuration
    p.add_argument("--remove-outliers",  default='none',
                   help="Remove worst outliers per file and size-bin during parsing: '1' removes top 1%, '0.1' removes top 0.1%; default 'none'.")
    # Optional: show a marker for the maximum value per violin
    p.add_argument("--mark-max", action='store_true', help="Add a marker for the maximum completion time per violin (per algorithm/trim)")
    # New: specify a single config filename to count lost packets from (across all drop subfolders)
    p.add_argument("--loss-config", type=str, default=None, help="Filename like ecmp_seed1_trim_on.txt to count lost packets per size-bin from, for column annotations")
    # New: bar plot mode for lost packets
    p.add_argument("--bar-plot", action='store_true', help="Show bar plot of lost packets per algorithm/trim instead of FCT violins; computed from all files.")
    # New: compress bar mode into a single panel (sum across size bins; show only >0 drop category)
    p.add_argument("--compress", action='store_true', help="With --bar-plot, sum lost packets across size-bins and show a single panel for drop>0.")
    # New: figure size overrides
    p.add_argument("--fig-width", type=float, default=None, help="Override figure width (inches) for plotting")
    p.add_argument("--fig-height", type=float, default=None, help="Override figure height (inches) for plotting")
    return p.parse_args()


# Remove top-X% outliers per algorithm configuration (algorithm, trim_status, drop_category, and size_bin if present)
def apply_outlier_trim(df: pd.DataFrame, mode: str) -> pd.DataFrame:
    if mode == 'none':
        return df
    try:
        pct = float(mode)
    except ValueError:
        return df
    q = 1.0 - (pct / 100.0)
    keys = ['algorithm', 'trim_status', 'drop_category']
    if 'size_bin' in df.columns:
        keys.append('size_bin')
    # Compute per-group threshold and filter
    thr = df.groupby(keys)['completion_time'].quantile(q).rename('thr')
    merged = df.merge(thr, on=keys, how='left')
    filtered = merged[merged['completion_time'] <= merged['thr']].drop(columns=['thr'])
    removed = len(df) - len(filtered)
    print(f"Outlier trimming: removed top {pct}% per group -> {removed} rows dropped")
    return filtered


def infer_drop_rate(subfolder: Path) -> float:
    m = FLOAT_RE.search(subfolder.name)
    if not m:
        return 0.0
    try:
        return float(m.group(1))
    except ValueError:
        return 0.0


def drop_category(rate: float) -> str:
    return '0' if rate == 0 else '>0'


def extract_completion_times(file_path: Path, use_global_time: bool = False) -> List[float]:
    times: List[float] = []
    try:
        with file_path.open('r', errors='ignore') as f:
            for line in f:
                if use_global_time:
                    if 'global time' not in line:
                        continue
                    m = GLOBAL_TIME_RE.search(line)
                else:
                    if 'finished at' not in line:
                        continue
                    m = FINISHED_AT_RE.search(line)
                if m:
                    try:
                        times.append(float(m.group(1)))
                    except ValueError:
                        pass
    except FileNotFoundError:
        pass
    return times


def extract_flow_records(file_path: Path) -> List[Tuple[float, int | None]]:
    """Return list of (completion_time, total_bytes or None) per finished flow line."""
    out: List[Tuple[float, int | None]] = []
    try:
        with file_path.open('r', errors='ignore') as f:
            for line in f:
                if 'finished at' not in line:
                    continue
                tm = FINISHED_AT_RE.search(line)
                if not tm:
                    continue
                szm = TOTAL_BYTES_RE.search(line)
                t: float
                b: int | None
                try:
                    t = float(tm.group(1))
                except ValueError:
                    continue
                b = int(szm.group(1)) if szm else None
                out.append((t, b))
    except FileNotFoundError:
        pass
    return out


def size_bin_from_bytes(b: int | None) -> str:
    """Map byte size to one of: 'small','medium','large'. Defaults to 'unknown' if None."""
    if b is None:
        return 'unknown'
    # 0-10 KB inclusive
    if b <= 10_000:
        return 'small'
    # 10 KB-1 MiB inclusive on upper bound
    if b <= 1 * 1024 * 1024:
        return 'medium'
    return 'large'


def parse_filename(stem: str) -> Tuple[str, str, int]:
    m = FILENAME_RE.match(stem)
    if m:
        algo = m.group('algo')
        trim = 'on' if m.group('trim') == 'trim_on' else 'off'
        seed_str = m.group('seed')
        seed = int(seed_str) if seed_str else 1
    else:
        # Fallback to legacy naming without trim/seed tagging
        algo = stem
        trim = 'unknown'
        seed = 1
    return algo, trim, seed


def collect_data(root: Path, min_flows: int, only_max: bool = False, remove_outliers_mode: str = 'none') -> pd.DataFrame:
    records: List[Dict] = []
    if not root.is_dir():
        raise FileNotFoundError(f"Experiment root {root} not found or not a directory")

    # Helper for per-file trimming (always per file when a trimming mode is set)
    def trim_file_records(file_recs: List[Tuple[float, int | None]]) -> List[Tuple[float, int | None]]:
        if remove_outliers_mode == 'none' or not file_recs:
            return file_recs
        try:
            pct = float(remove_outliers_mode)
        except ValueError:
            return file_recs
        q = 1.0 - (pct / 100.0)
        # Build a per-file dataframe with size bins
        df_file = pd.DataFrame(
            {
                't': [t for t, _ in file_recs],
                'b': [b for _, b in file_recs],
            }
        )
        df_file['bin'] = df_file['b'].apply(size_bin_from_bytes)
        # Quantile per bin
        thr = df_file.groupby('bin')['t'].quantile(q).rename('thr')
        df_file = df_file.merge(thr, on='bin', how='left')
        kept = df_file[df_file['t'] <= df_file['thr']]
        return list(zip(kept['t'].tolist(), kept['b'].tolist()))

    for sub in sorted(root.iterdir()):
        if not sub.is_dir():
            continue
        rate = infer_drop_rate(sub)
        cat = drop_category(rate)
        for txt in sorted(sub.glob('*.txt')):
            algo, trim_status, seed = parse_filename(txt.stem)
            if only_max:
                # Use 'global time' entries when only_max is enabled
                times = extract_completion_times(txt, use_global_time=True)
                if len(times) < min_flows:
                    continue
                t = max(times)
                records.append({
                    'algorithm': algo,
                    'trim_status': trim_status,
                    'seed': seed,
                    'drop_rate_value': rate,
                    'drop_category': cat,
                    'completion_time': t,
                    'source_file': str(txt.relative_to(root)),
                    # No flow size info when using only_max
                    'size_bin': 'all'
                })
            else:
                # Parse per-flow completion times and bytes
                recs = extract_flow_records(txt)
                if len(recs) < min_flows:
                    continue
                # Always apply per-file outlier trim here if requested
                recs = trim_file_records(recs)
                for t, b in recs:
                    records.append({
                        'algorithm': algo,
                        'trim_status': trim_status,
                        'seed': seed,
                        'drop_rate_value': rate,
                        'drop_category': cat,
                        'completion_time': t,
                        'source_file': str(txt.relative_to(root)),
                        'flow_bytes': b,
                        'size_bin': size_bin_from_bytes(b)
                    })
    if not records:
        raise RuntimeError("No completion times were parsed. Check folder structure or patterns.")
    return pd.DataFrame.from_records(records)


def compute_loss_counts(root: Path, loss_config_filename: str | None, restrict_to_positive: bool = False) -> Dict[str, int] | None:
    if not loss_config_filename:
        return None

    def parse_one_file(fp: Path, counts: Dict[str, int]) -> None:
        seen_flow_ids: set[int] = set()
        try:
            with fp.open('r', errors='ignore') as f:
                for line in f:
                    # Be tolerant: just require both substrings to be present
                    if ('lost packets' not in line) or ('total bytes' not in line):
                        continue
                    m_loss = LOST_PACKETS_RE.search(line)
                    m_bytes = TOTAL_BYTES_RE.search(line)
                    if not m_loss or not m_bytes:
                        continue
                    fid = None
                    m_fid = FLOW_ID_RE.search(line)
                    if m_fid:
                        try:
                            fid = int(m_fid.group(1))
                        except ValueError:
                            fid = None
                    if fid is not None:
                        if fid in seen_flow_ids:
                            continue
                        seen_flow_ids.add(fid)
                    try:
                        lost = int(m_loss.group(1))
                        b = int(m_bytes.group(1))
                    except ValueError:
                        continue
                    bin_key = size_bin_from_bytes(b)
                    if bin_key in counts:
                        counts[bin_key] += lost
        except FileNotFoundError:
            pass

    counts = {'small': 0, 'medium': 0, 'large': 0}
    cfg_path = Path(loss_config_filename)

    # Case 1: user provided an explicit path to a file (absolute or relative)
    if cfg_path.is_file():
        parse_one_file(cfg_path, counts)
        return counts
    # Case 2: path relative to the experiment root
    if (root / cfg_path).is_file():
        parse_one_file(root / cfg_path, counts)
        return counts

    # Case 3: treat the input as a bare filename and search within drop subfolders
    base_name = cfg_path.name
    for sub in sorted(root.iterdir()):
        if not sub.is_dir():
            continue
        if restrict_to_positive and drop_category(infer_drop_rate(sub)) == '0':
            continue
        fp = sub / base_name
        if fp.is_file():
            parse_one_file(fp, counts)
    return counts


def collect_loss_counts_dataset(root: Path) -> pd.DataFrame:
    """Walk the experiment root and sum lost packets per (algorithm, trim_status, drop_category, size_bin).
    Lines are parsed tolerantly: any line containing both 'lost packets' and 'total bytes' contributes.
    """
    if not root.is_dir():
        raise FileNotFoundError(f"Experiment root {root} not found or not a directory")

    counts: Dict[Tuple[str, str, str, str], int] = {}

    for sub in sorted(root.iterdir()):
        if not sub.is_dir():
            continue
        rate = infer_drop_rate(sub)
        cat = drop_category(rate)
        for txt in sorted(sub.glob('*.txt')):
            algo, trim_status, _seed = parse_filename(txt.stem)
            seen_flow_ids: set[int] = set()
            try:
                with txt.open('r', errors='ignore') as f:
                    for line in f:
                        if ('lost packets' not in line) or ('total bytes' not in line):
                            continue
                        m_loss = LOST_PACKETS_RE.search(line)
                        m_bytes = TOTAL_BYTES_RE.search(line)
                        if not m_loss or not m_bytes:
                            continue
                        # Deduplicate by flowId within a file, if present
                        fid = None
                        m_fid = FLOW_ID_RE.search(line)
                        if m_fid:
                            try:
                                fid = int(m_fid.group(1))
                            except ValueError:
                                fid = None
                        if fid is not None:
                            if fid in seen_flow_ids:
                                continue
                            seen_flow_ids.add(fid)
                        try:
                            lost = int(m_loss.group(1))
                            b = int(m_bytes.group(1))
                        except ValueError:
                            continue
                        bin_key = size_bin_from_bytes(b)
                        key = (algo, trim_status, cat, bin_key)
                        counts[key] = counts.get(key, 0) + lost
            except FileNotFoundError:
                continue

    # Convert to DataFrame
    if not counts:
        return pd.DataFrame(columns=['algorithm','trim_status','drop_category','size_bin','lost_packets','trim_status_label'])
    recs: List[Dict] = []
    for (algo, trim, cat, bin_key), val in counts.items():
        recs.append({
            'algorithm': algo,
            'trim_status': trim,
            'drop_category': cat,
            'size_bin': bin_key,
            'lost_packets': int(val),
            'trim_status_label': 'On' if trim == 'on' else 'Off',
        })
    return pd.DataFrame.from_records(recs)


def make_plot(df: pd.DataFrame, output_path: Path, style: str, palette: str, title_zero: str, title_positive: str, inner_style: str, separate_y: bool, annot_stats: bool, ideal_fct: float | None, no_show: bool, padding: bool, y_label: str | None, x_label_map: Dict[str, str], only_positive: bool, mark_max: bool, loss_counts: Dict[str, int] | None = None):
    sns.set_style(style)
    plt.rcParams["font.family"] = "serif"
    plt.rcParams["font.serif"] = ["Liberation Serif", "DejaVu Serif"]

    # Determine multi-seed mode
    unique_seeds = df['seed'].nunique()
    multi_seed_mode = unique_seeds > 1
    if multi_seed_mode:
        print(f"Detected {unique_seeds} distinct seeds. Aggregating all seeds in violins.")
    else:
        print("Single-seed mode detected.")

    # Only keep known trim statuses; create capitalized labels for hue
    known_df = df[df['trim_status'].isin(['on','off'])].copy()
    if known_df.empty:
        raise RuntimeError("No files with trim_on/trim_off naming found.")
    known_df['trim_status_label'] = known_df['trim_status'].map({'on': 'On', 'off': 'Off'})

    algorithms = sorted(known_df['algorithm'].unique())

    # Flow-size bin support (falls back to old layout if not available)
    has_bins = 'size_bin' in known_df.columns and known_df['size_bin'].nunique() > 1
    size_bins = [('small', '0-10 KB'), ('medium', '10 KB-1 MiB'), ('large', '>1 MiB')]

    # Figure size and shared axis control
    base_auto_width = max(6, 1.3 * len(algorithms))
    cols = 3 if has_bins else 1
    width = (FIG_WIDTH if FIG_WIDTH is not None else base_auto_width * cols)
    row_info = [('>0', title_positive)] if only_positive else [('0', title_zero), ('>0', title_positive)]
    nrows = len(row_info)

    # Decide label units globally, but format ticks per-panel:
    # If all non-empty panels prefer ms (max > 1000), label as ms; otherwise label as us.
    panel_maxes: List[float] = []
    for cat, _ in row_info:
        if has_bins:
            for bin_key, _ in size_bins:
                sub = known_df[(known_df['drop_category'] == cat) & (known_df['size_bin'] == bin_key)]
                if not sub.empty:
                    panel_maxes.append(float(sub['completion_time'].max()))
        else:
            sub = known_df[(known_df['drop_category'] == cat)]
            if not sub.empty:
                panel_maxes.append(float(sub['completion_time'].max()))
    use_ms_label = all(m > 1000.0 for m in panel_maxes) if panel_maxes else (known_df['completion_time'].max() > 1000.0)

    # If units are mixed and y is shared, disable shared y to allow independent formatters
    any_ms = any(m > 1000.0 for m in panel_maxes)
    any_us = any(m <= 1000.0 for m in panel_maxes)
    mixed_units = any_ms and any_us

    height = (FIG_HEIGHT / 2.0) if nrows == 1 else FIG_HEIGHT
    fig, axes = plt.subplots(
        nrows=nrows,
        ncols=cols,
        figsize=(width, height),
        sharex=True,
        sharey=False if (mixed_units and not separate_y) else (not separate_y)
    )

    # Normalize axes to 2D list
    if nrows == 1 and cols == 1:
        axes = np.array([[axes]])
    elif nrows == 1:
        axes = np.array([axes])
    elif cols == 1:
        axes = np.array([[ax] for ax in axes])

    # Inner style handling
    inner_value = None if inner_style == 'none' else inner_style

    # Helper to format values per-panel
    def fmt_value(v: float, panel_ms: bool) -> str:
        return f"{v/1000:.1f}" if panel_ms else f"{v:.0f}"

    # Use user-provided titles for drop categories via left-most ylabels
    drop_titles = {cat: ttl for cat, ttl in row_info}

    for r_idx, (cat, _) in enumerate(row_info):
        for c_idx in range(cols):
            ax = axes[r_idx, c_idx]
            if has_bins:
                bin_key, bin_title = size_bins[c_idx]
                sub = known_df[(known_df['drop_category'] == cat) & (known_df['size_bin'] == bin_key)]
            else:
                bin_title = None
                sub = known_df[(known_df['drop_category'] == cat)]

            if sub.empty:
                ax.text(0.5, 0.5, 'No data', ha='center', va='center')
                ax.set_axis_off()
                continue

            # Per-panel unit decision
            panel_use_ms = (sub['completion_time'].max() > 1000.0)

            try:
                sns.violinplot(
                    data=sub,
                    x='algorithm',
                    y='completion_time',
                    hue='trim_status_label',
                    order=algorithms,
                    hue_order=['Off','On'],
                    inner=inner_value,
                    density_norm='width',
                    cut=0,
                    palette=palette,
                    linewidth=1.0,
                    ax=ax
                )
            except TypeError:
                sns.violinplot(
                    data=sub,
                    x='algorithm',
                    y='completion_time',
                    hue='trim_status_label',
                    order=algorithms,
                    hue_order=['Off','On'],
                    inner=inner_value,
                    scale='width',
                    cut=0,
                    palette=palette,
                    linewidth=1.0,
                    ax=ax
                )

            # Draw ideal completion time as a dashed reference line, if provided
            if ideal_fct is not None:
                ax.axhline(ideal_fct, linestyle='--', color='black', linewidth=1.0, alpha=0.75, zorder=2)

            # Capture legend entries from the violinplot only
            base_handles, base_labels = ax.get_legend_handles_labels()
            base_labels = [lbl.capitalize() for lbl in base_labels]

            # Place size-range text inside the plot area (only on top row to avoid repetition)
            if r_idx == 0 and bin_title:
                ax.text(0.02, 0.98, bin_title, transform=ax.transAxes, ha='left', va='top', fontsize=10,
                        fontweight='bold', color='black', bbox=dict(facecolor='white', edgecolor='none', alpha=0.6, pad=1))
                # Also annotate dropped packets per size-bin from a single config if provided
                if loss_counts is not None:
                    drop_val = int(loss_counts.get(bin_key, 0))
                    ax.text(0.02, 0.88, f"Dropped pkts: {drop_val}", transform=ax.transAxes, ha='left', va='top', fontsize=9,
                            color='black', bbox=dict(facecolor='white', edgecolor='none', alpha=0.6, pad=1))
                ax.set_title('')
            else:
                ax.set_title('')

            # Labels
            ax.set_xlabel('')
            if c_idx == 0:
                ax.set_ylabel(y_label if y_label is not None else ('Flow Completion Time (ms)' if use_ms_label else 'Flow Completion Time (us)'))
            else:
                ax.set_ylabel('')

            # Per-panel tick formatter
            if panel_use_ms:
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, pos: f"{y/1000:.1f}"))
            else:
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, pos: f"{y:.0f}"))

            # Compute stats for markers
            p99_df = (
                sub.groupby(['algorithm', 'trim_status_label'])['completion_time']
                   .quantile(0.995)
                   .reset_index(name='p99')
            )
            med_df = (
                sub.groupby(['algorithm', 'trim_status_label'])['completion_time']
                   .median()
                   .reset_index(name='median')
            )
            stats_df = pd.merge(p99_df, med_df, on=['algorithm', 'trim_status_label'])

            # Manual scatter to place p99 markers aligned with hue dodge
            algo_idx = {a: idx for idx, a in enumerate(algorithms)}
            offset_map = {'Off': -0.2, 'On': 0.2}
            x_pts = [algo_idx[row['algorithm']] + offset_map.get(row['trim_status_label'], 0.0) for _, row in stats_df.iterrows()]
            y_pts = stats_df['p99'].tolist()
            ax.scatter(x_pts, y_pts, marker='D', s=44, c='k', alpha=0.70, zorder=3)

            # Optional: add maximum markers per group when requested
            if mark_max:
                max_df = (
                    sub.groupby(['algorithm', 'trim_status_label'])['completion_time']
                       .max()
                       .reset_index(name='max')
                )
                stats_df = pd.merge(stats_df, max_df, on=['algorithm', 'trim_status_label'], how='left')
                y_pts_max = stats_df['max'].tolist()
                ax.scatter(x_pts, y_pts_max, marker='^', s=44, c='crimson', alpha=0.70, zorder=3)

            # Optional text annotations with median and p99 per violin; force text color to black
            if annot_stats:
                y_min, y_max = ax.get_ylim()
                y_pad = 0.03 * (y_max - y_min)
                for (x, (_, row)) in zip(x_pts, stats_df.iterrows()):
                    alg_index = algo_idx[row['algorithm']]
                    med_txt = fmt_value(row['median'], panel_use_ms)
                    p99_txt = fmt_value(row['p99'], panel_use_ms)
                    if alg_index == 0 and row['trim_status_label'] == 'Off':
                        text = f"Median: {med_txt}\nP99: {p99_txt}"
                        y = row['p99'] + y_pad
                        va = 'bottom'
                    elif alg_index == 0 and row['trim_status_label'] == 'On':
                        text = f"{med_txt}\n{p99_txt}"
                        y = max(y_min + y_pad, row['median'] - y_pad)
                        va = 'top'
                    else:
                        text = f"{med_txt}\n{p99_txt}"
                        y = row['p99'] + y_pad
                        va = 'bottom'
                    ax.text(x, y, text, ha='center', va=va, fontsize=8,
                            color='black', bbox=dict(facecolor='white', edgecolor='none', alpha=0.6, pad=1))

            # If requested, add ~10% headroom at the top to prevent annotation overflow
            if padding:
                y_min, y_max = ax.get_ylim()
                span = y_max - y_min
                if span > 0:
                    ax.set_ylim(y_min, y_max + 0.10 * span)

            # Legend: keep only in top-left panel
            if r_idx == 0 and c_idx == 0:
                ax.legend(base_handles, base_labels, title='Trim', loc='best', ncols=2)
            else:
                leg = ax.get_legend()
                if leg:
                    leg.remove()

    # Apply custom x-label mapping on the bottom row (shared x-axis)
    labels = [x_label_map.get(a, a) for a in algorithms]
    for c_idx in range(cols):
        ax = axes[-1, c_idx]
        ax.set_xticks(range(len(algorithms)))
        ax.set_xticklabels(labels, rotation=20, ha='right')
        ax.set_xlabel('')

    # Add row titles on the far-left outside the axes area
    if nrows == 2:
        fig.text(0.01, 0.75, row_info[0][1], rotation=90, va='center', ha='left')
        fig.text(0.01, 0.25, row_info[1][1], rotation=90, va='center', ha='left')
    else:
        fig.text(0.01, 0.5, row_info[0][1], rotation=90, va='center', ha='left')

    plt.tight_layout(rect=(0.03, 0, 1, 0.97))
    # Always save PNG to the requested output path
    plt.savefig(output_path)
    print(f"Saved plot to {output_path}")
    # If no_show, also save a PDF and do not display
    if no_show:
        pdf_path = output_path.with_suffix('.pdf')
        plt.savefig(pdf_path)
        print(f"Saved PDF to {pdf_path}")
    else:
        plt.show()


def make_bar_plot(df_counts: pd.DataFrame, output_path: Path, style: str, palette: str, title_zero: str, title_positive: str, only_positive: bool, x_label_map: Dict[str, str], compress: bool):
    sns.set_style(style)
    plt.rcParams["font.family"] = "serif"
    plt.rcParams["font.serif"] = ["Liberation Serif", "DejaVu Serif"]

    if df_counts is None or df_counts.empty:
        print("No lost-packet data found to plot.")
        fig, ax = plt.subplots(figsize=(8, 3))
        ax.text(0.5, 0.5, 'No data', ha='center', va='center')
        ax.axis('off')
        plt.savefig(output_path)
        print(f"Saved plot to {output_path}")
        return

    # Ensure trim label exists
    if 'trim_status_label' not in df_counts.columns:
        df_counts = df_counts.copy()
        df_counts['trim_status_label'] = df_counts['trim_status'].map({'on': 'On', 'off': 'Off'})

    algorithms = sorted(df_counts['algorithm'].unique())
    base_auto_width = max(6, 1.3 * len(algorithms))

    # Compressed single-panel view: sum across size bins, prefer drop>0
    if compress:
        # Pick category to show: prefer >0, else fallback to 0 if that's all we have
        cat_to_plot = '>0' if (df_counts['drop_category'] == '>0').any() else '0'
        sub = df_counts[df_counts['drop_category'] == cat_to_plot].copy()
        if sub.empty:
            print("No data for requested category in compressed mode.")
            fig, ax = plt.subplots(figsize=(8, 3))
            ax.text(0.5, 0.5, 'No data', ha='center', va='center')
            ax.axis('off')
            plt.savefig(output_path)
            print(f"Saved plot to {output_path}")
            return
        # Aggregate across size bins
        sub = (
            sub.groupby(['algorithm', 'trim_status_label'], as_index=False)['lost_packets']
               .sum()
        )

        width = (FIG_WIDTH if FIG_WIDTH is not None else base_auto_width)
        height = FIG_HEIGHT / 2.0
        fig, ax = plt.subplots(figsize=(width, height))
        try:
            sns.barplot(
                data=sub,
                x='algorithm',
                y='lost_packets',
                hue='trim_status_label',
                order=algorithms,
                hue_order=['Off','On'],
                estimator=np.sum,
                errorbar=None,
                palette=palette,
                edgecolor='black',
                linewidth=0.8,
                ax=ax,
            )
        except TypeError:
            sns.barplot(
                data=sub,
                x='algorithm',
                y='lost_packets',
                hue='trim_status_label',
                order=algorithms,
                hue_order=['Off','On'],
                ci=None,
                palette=palette,
                edgecolor='black',
                linewidth=0.8,
                ax=ax,
            )
        # Ensure black outlines regardless of seaborn/mpl version
        for p in ax.patches:
            try:
                p.set_edgecolor('black')
                p.set_linewidth(0.8)
            except Exception:
                pass
        ax.set_ylabel('Lost Packets')
        ax.set_xlabel('')
        ax.set_yscale('log')
        # Apply custom x-label mapping
        labels = [x_label_map.get(a, a) for a in algorithms]
        ax.set_xticks(range(len(algorithms)))
        ax.set_xticklabels(labels, rotation=20, ha='right')
        ax.legend(title='Trim', loc='best', ncols=2)
        ax.legend(title='Trim', loc='lower left', ncols=2)
        # Annotate each bar with relative reduction vs worst offender (percentage); mark baseline on worst bar(s)
        worst_val = float(sub['lost_packets'].max()) if not sub.empty else 0.0
        if worst_val > 0:
            for p in ax.patches:
                h = float(p.get_height())
                # Skip zero-height bars to avoid spurious "-100%" labels on log scale
                if h <= 0:
                    continue
                # Place text slightly above the bar; multiplicative offset for log scale
                x = p.get_x() + p.get_width() / 2.0
                y = h * 1.12
                if abs(h - worst_val) < 1e-9:
                    txt = "baseline"
                else:
                    reduction_pct = max(0.0, (worst_val - h) / worst_val * 100.0)
                    # Clamp extreme cases to 99% to avoid showing -100%
                    pct_disp = 99 if reduction_pct >= 99.5 else int(round(reduction_pct))
                    txt = f"-{pct_disp}%"
                ax.text(x, y, txt, ha='center', va='bottom', fontsize=9, color='black')
        # In compressed mode, omit the left-side category text
        plt.tight_layout(rect=(0.03, 0, 1, 0.97))
        plt.savefig(output_path)
        plt.savefig("lost.pdf")
        print(f"Saved plot to {output_path}")
        return

    # Non-compressed grid: 2x3 or 1x3 (if only_positive)
    size_bins = [('small', '0-10 KB'), ('medium', '10 KB-1 MiB'), ('large', '>1 MiB')]
    row_info = [('>0', title_positive)] if only_positive else [('0', title_zero), ('>0', title_positive)]

    cols = 3
    nrows = len(row_info)
    width = (FIG_WIDTH if FIG_WIDTH is not None else base_auto_width * cols)
    height = (FIG_HEIGHT / 2.0) if nrows == 1 else FIG_HEIGHT

    fig, axes = plt.subplots(
        nrows=nrows,
        ncols=cols,
        figsize=(width, height),
        sharex=True,
        sharey=False,
    )

    if nrows == 1:
        axes = np.array([axes])

    for r_idx, (cat, _) in enumerate(row_info):
        for c_idx in range(cols):
            ax = axes[r_idx, c_idx]
            bin_key, bin_title = size_bins[c_idx]
            sub = df_counts[(df_counts['drop_category'] == cat) & (df_counts['size_bin'] == bin_key)]

            if sub.empty:
                ax.text(0.5, 0.5, 'No data', ha='center', va='center')
                ax.set_axis_off()
                continue

            try:
                sns.barplot(
                    data=sub,
                    x='algorithm',
                    y='lost_packets',
                    hue='trim_status_label',
                    order=algorithms,
                    hue_order=['Off','On'],
                    estimator=np.sum,
                    errorbar=None,
                    palette=palette,
                    ax=ax,
                )
            except TypeError:
                sns.barplot(
                    data=sub,
                    x='algorithm',
                    y='lost_packets',
                    hue='trim_status_label',
                    order=algorithms,
                    hue_order=['Off','On'],
                    ci=None,
                    palette=palette,
                    ax=ax,
                )

            if r_idx == 0:
                ax.text(0.02, 0.98, bin_title, transform=ax.transAxes, ha='left', va='top', fontsize=10,
                        fontweight='bold', color='black', bbox=dict(facecolor='white', edgecolor='none', alpha=0.6, pad=1))

            ax.set_xlabel('')
            if c_idx == 0:
                ax.set_ylabel('Lost Packets')
            else:
                ax.set_ylabel('')

            if r_idx == 0 and c_idx == 0:
                ax.legend(title='Trim', loc='best', ncols=2)
            else:
                leg = ax.get_legend()
                if leg:
                    leg.remove()

    labels = [x_label_map.get(a, a) for a in algorithms]
    for c_idx in range(cols):
        ax = axes[-1, c_idx]
        ax.set_xticks(range(len(algorithms)))
        ax.set_xticklabels(labels, rotation=20, ha='right')
        ax.set_xlabel('')

    if nrows == 2:
        fig.text(0.01, 0.75, row_info[0][1], rotation=90, va='center', ha='left')
        fig.text(0.01, 0.25, row_info[1][1], rotation=90, va='center', ha='left')
    else:
        fig.text(0.01, 0.5, row_info[0][1], rotation=90, va='center', ha='left')

    plt.tight_layout(rect=(0.03, 0, 1, 0.97))
    plt.savefig(output_path)
    print(f"Saved plot to {output_path}")


def main():
    args = parse_args()

    # Allow CLI to override figure size
    global FIG_WIDTH, FIG_HEIGHT
    if args.fig_width is not None:
        FIG_WIDTH = args.fig_width
    if args.fig_height is not None:
        FIG_HEIGHT = args.fig_height

    # Bar-plot mode: compute dataset-wide lost-packet counts and plot bars; ignore --loss-config
    if args.bar_plot:
        df_counts = collect_loss_counts_dataset(args.experiment_root)
        # Keep only the selected RSS+PFLD variant among rss1/rss2 (default keeps rss1)
        if args.rss_pfld and not df_counts.empty:
            df_counts = df_counts[(~df_counts['algorithm'].isin(['rss1','rss2'])) | (df_counts['algorithm'] == args.rss_pfld)]
        # Build x-axis label map with optional RSS+PFLD override
        x_label_map = dict(X_LABEL_MAP)
        if args.rss_pfld is not None:
            x_label_map.pop('rss1', None)
            x_label_map.pop('rss2', None)
            x_label_map[args.rss_pfld] = "RSS+PFLD"
        output_path = args.experiment_root / args.output
        make_bar_plot(
            df_counts=df_counts,
            output_path=output_path,
            style=args.style,
            palette=args.palette,
            title_zero=args.title_zero,
            title_positive=args.title_positive,
            only_positive=args.only_positive,
            x_label_map=x_label_map,
            compress=args.compress,
        )
        return

    df = collect_data(args.experiment_root, args.min_flows, args.only_max, args.remove_outliers)

    # Removed global/group outlier trimming; trimming is now applied per file during parsing.

    # Compute optional loss counts for annotations from one config file
    loss_counts = compute_loss_counts(args.experiment_root, args.loss_config, args.only_positive)
    if loss_counts is not None:
        total_loss = int(sum(loss_counts.values()))
        print(
            "Lost packets per size-bin from {}: small={}, medium={}, large={}, total={}".format(
                args.loss_config,
                int(loss_counts.get('small', 0)),
                int(loss_counts.get('medium', 0)),
                int(loss_counts.get('large', 0)),
                total_loss,
            )
        )

    # Optional: print flow-size bin counts for a single configuration
    if args.count_algo or args.count_trim or args.count_drop:
        filt = df.copy()
        if args.count_algo is not None:
            filt = filt[filt['algorithm'] == args.count_algo]
        if args.count_trim is not None:
            filt = filt[filt['trim_status'] == args.count_trim]
        if args.count_drop is not None:
            filt = filt[filt['drop_category'] == args.count_drop]
        if 'size_bin' in filt.columns:
            counts = filt['size_bin'].value_counts()
            small = int(counts.get('small', 0))
            medium = int(counts.get('medium', 0))
            large = int(counts.get('large', 0))
            print(f"Flow counts (algo={args.count_algo}, trim={args.count_trim}, drop={args.count_drop}) -> small: {small}, medium: {medium}, large: {large}")
        else:
            print("No size_bin information available to count flows.")

    if args.csv:
        csv_path = args.experiment_root / args.csv
        df.to_csv(csv_path, index=False)
        print(f"Saved CSV to {csv_path}")
    output_path = args.experiment_root / args.output

    # Keep only the selected RSS+PFLD variant among rss1/rss2 (default keeps rss1)
    if args.rss_pfld:
        df = df[(~df['algorithm'].isin(['rss1','rss2'])) | (df['algorithm'] == args.rss_pfld)]

    # Build x-axis label map with optional RSS+PFLD override
    x_label_map = dict(X_LABEL_MAP)
    if args.rss_pfld is not None:
        x_label_map.pop('rss1', None)
        x_label_map.pop('rss2', None)
        x_label_map[args.rss_pfld] = "RSS+PFLD"

    # Pass options (figure size controlled via variables)
    make_plot(
         df=df,
         output_path=output_path,
         style=args.style,
         palette=args.palette,
         title_zero=args.title_zero,
         title_positive=args.title_positive,
         inner_style=args.inner,
         separate_y=args.separate_y,
         annot_stats=args.annot_stats,
         ideal_fct=args.ideal_fct,
         no_show=args.no_show,
         padding=args.padding,
         y_label=args.y_label,
         x_label_map=x_label_map,
         only_positive=args.only_positive,
         mark_max=args.mark_max,
         loss_counts=loss_counts,
     )


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
