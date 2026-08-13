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
  Flow Uec_28_92 flowId 29 uecSrc 28 finished at 94.5171 total packets 977 ...
We extract the numeric value after 'finished at'.

Dataframe columns: algorithm, trim_status (on/off), seed, drop_category (0 or >0), completion_time.

Plot: two stacked subplots (top: drop=0, bottom: drop>0). Within each subplot violins per algorithm with hue=trim_status.
If multiple seeds are detected (more than one distinct seed), all seeds' completion times are aggregated in the same violins (combined distribution).
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
FIG_WIDTH = 6   # Set a float (inches) to override automatic width; None keeps auto width
FIG_HEIGHT = 5   # Figure height in inches
X_LABEL_MAP: Dict[str, str] = {
    "ecmp": "ECMP",
    "reps": "REPS",
    "rss": "RSS",
    "oblivious": "Oblivious",
    "flowbender": "PLB",
    "flowlet": "Flowlet",
    "rss1": "RSS+\nPFLD",
    "rss2": "RSS+\nPFLD",
    "rss3": "RSS+\nPFLD",
    "pfld_probe1": "RSS+\nPFLD",
    "rss_rack_tlp": "RSS+\nRACK-TLP",
}
PLOT_ALGORITHM_ORDER = [
    "ecmp", "flowbender", "flowlet", "oblivious", "reps", "rss",
    "rss1", "rss2", "rss3", "pfld_probe1", "rss_rack_tlp",
]

FINISHED_AT_RE = re.compile(r"finished at ([0-9]*\.?[0-9]+)")
GLOBAL_TIME_RE = re.compile(r"global time ([0-9]*\.?[0-9]+)")
FLOAT_RE = re.compile(r"([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)")
# Supports: algo_trim_on/off  AND  algo_seed3_trim_on/off
FILENAME_RE = re.compile(r"^(?P<algo>.+?)(?:_seed(?P<seed>\d+))?_(?P<trim>trim_on|trim_off)$")


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
    p.add_argument("--rss-pfld", choices=['rss1','rss2','rss3'], default='rss1', help="Select which RSS+PFLD variant to display (default: rss1)")
    # New option: custom Y-axis label
    p.add_argument("--y-label", type=str, default=None, help="Override Y-axis label (default uses automatic units)")
    # New option: subtract a percentage of ECMP max runtime from all results
    p.add_argument(
        "--comm-only-percentage",
        type=float,
        default=None,
        help=(
            "If set, subtract this percentage of ECMP's maximum runtime from all completion times. "
            "Accepts 0-100 (percentage) or 0-1 (fraction)."
        ),
    )
    return p.parse_args()


def infer_drop_rate(subfolder: Path) -> float:
    try:
        token = subfolder.name.removeprefix("drop_")
        return float(token)
    except ValueError:
        m = FLOAT_RE.search(subfolder.name)
        return float(m.group(1)) if m else 0.0


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


def collect_data(root: Path, min_flows: int, only_max: bool = False) -> pd.DataFrame:
    records: List[Dict] = []
    if not root.is_dir():
        raise FileNotFoundError(f"Experiment root {root} not found or not a directory")

    for sub in sorted(root.iterdir()):
        if not sub.is_dir():
            continue
        rate = infer_drop_rate(sub)
        cat = drop_category(rate)
        for txt in sorted(sub.glob('*.txt')):
            algo, trim_status, seed = parse_filename(txt.stem)
            # Use 'global time' entries when only_max is enabled; otherwise use per-flow 'finished at'
            times = extract_completion_times(txt, use_global_time=only_max)
            if len(times) < min_flows:
                continue
            if only_max:
                # Record just the maximum overall time for this file
                t = max(times)
                records.append({
                    'algorithm': algo,
                    'trim_status': trim_status,
                    'seed': seed,
                    'drop_rate_value': rate,
                    'drop_category': cat,
                    'completion_time': t,
                    'source_file': str(txt.relative_to(root))
                })
            else:
                for t in times:
                    records.append({
                        'algorithm': algo,
                        'trim_status': trim_status,
                        'seed': seed,
                        'drop_rate_value': rate,
                        'drop_category': cat,
                        'completion_time': t,
                        'source_file': str(txt.relative_to(root))
                    })
    if not records:
        raise RuntimeError("No completion times were parsed. Check folder structure or patterns.")
    return pd.DataFrame.from_records(records)


def make_plot(df: pd.DataFrame, output_path: Path, style: str, palette: str, title_zero: str, title_positive: str, inner_style: str, separate_y: bool, annot_stats: bool, ideal_fct: float | None, no_show: bool, padding: bool, y_label: str | None, x_label_map: Dict[str, str], only_positive: bool, comm_only: bool):
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

    order_index = {name: index for index, name in enumerate(PLOT_ALGORITHM_ORDER)}
    algorithms = sorted(
        known_df['algorithm'].unique(),
        key=lambda name: (order_index.get(name, len(order_index)), name),
    )

    # Decide unit scale globally for the figure (handles shared y-axis)
    use_ms = known_df['completion_time'].max() > 1000
    def _fmt_value(v: float) -> str:
        return f"{v/1000:.1f}" if use_ms else f"{v:.0f}"

    # Figure size and shared y-axis control
    auto_width = max(6, 1.3 * len(algorithms))
    width = FIG_WIDTH if FIG_WIDTH is not None else auto_width
    # Create one or two panels depending on only_positive; halve height if single panel
    nrows = 1 if only_positive else 2
    height = (FIG_HEIGHT / 2.0) if only_positive else FIG_HEIGHT
    fig, axes = plt.subplots(nrows=nrows, ncols=1, figsize=(width, height), sharex=True, sharey=not separate_y)
    if nrows == 1:
        axes = [axes]

    # Inner style handling
    inner_value = None if inner_style == 'none' else inner_style

    # Use user-provided titles for easier customization
    panel_info = [('>0', title_positive)] if only_positive else [('0', title_zero), ('>0', title_positive)]
    for i, (ax, (cat, title)) in enumerate(zip(axes, panel_info)):
        sub = known_df[known_df['drop_category'] == cat]
        if sub.empty:
            ax.text(0.5, 0.5, f'No data for {title}', ha='center', va='center')
            ax.set_axis_off()
            continue

        if comm_only:
            # Barplot of the maximum completion time per (algorithm, trim)
            sub_agg = (
                sub.groupby(['algorithm', 'trim_status_label'])['completion_time']
                   .max()
                   .reset_index(name='max_time')
            )
            sns.barplot(
                data=sub_agg,
                x='algorithm',
                y='max_time',
                hue='trim_status_label',
                order=algorithms,
                hue_order=['Off','On'],
                palette=palette,
                ax=ax,
                linewidth=0.8,
                edgecolor='black'
            )
            # Draw ideal completion time reference if provided
            if ideal_fct is not None:
                ax.axhline(ideal_fct, linestyle='--', color='black', linewidth=1.0, alpha=0.75, zorder=2)

            # Annotate each bar with percent improvement vs ECMP (trim Off)
            base_row = sub_agg[(sub_agg['algorithm'] == 'ecmp') & (sub_agg['trim_status_label'] == 'Off')]
            base_val = None if base_row.empty else float(base_row['max_time'].max())
            ax.set_title(title)
            ax.set_xlabel('')
            ax.set_ylabel(y_label if y_label is not None else ('Flow Completion Time (ms)' if use_ms else 'Flow Completion Time (µs)'))
            if use_ms:
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, pos: f"{y/1000:.1f}"))

            if base_val is not None and base_val > 0:
                y_min, y_max = ax.get_ylim()
                y_pad = 0.02 * (y_max - y_min)
                for bar in ax.patches:
                    height = bar.get_height()
                    if height <= 0:
                        continue
                    impr = (base_val - height) / base_val * 100.0
                    x = bar.get_x() + bar.get_width() / 2.0
                    ax.text(x, height + y_pad, f"{impr:.0f}%", ha='center', va='bottom', fontsize=8, color='black')

            # Optional padding to reduce overlap of labels
            if padding:
                y_min, y_max = ax.get_ylim()
                span = y_max - y_min
                if span > 0:
                    ax.set_ylim(y_min, y_max + 0.10 * span)
        else:
            # Aggregation over seeds happens implicitly because we do not facet by seed.
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
                # Fallback for older seaborn versions that don't support density_norm
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

            ax.set_title(title)
            ax.set_xlabel('')
            ax.set_ylabel(y_label if y_label is not None else ('Flow Completion Time (ms)' if use_ms else 'Flow Completion Time (µs)'))
            if use_ms:
                ax.yaxis.set_major_formatter(FuncFormatter(lambda y, pos: f"{y/1000:.1f}"))

            # Compute a single 99th percentile and median per configuration (algorithm, trim) across all seeds
            p99_df = (
                sub.groupby(['algorithm', 'trim_status_label'])['completion_time']
                   .quantile(0.99)
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

            # Optional text annotations with median and p99 per violin; force text color to black
            if annot_stats:
                y_min, y_max = ax.get_ylim()
                y_pad = 0.03 * (y_max - y_min)
                for (x, (_, row)) in zip(x_pts, stats_df.iterrows()):
                    alg_index = algo_idx[row['algorithm']]
                    med_txt = _fmt_value(row['median'])
                    p99_txt = _fmt_value(row['p99'])
                    # Place the second violin of the first group below to avoid overlap
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

        # Place legend on the bottom subplot, upper right
        if i == len(axes) - 1:
            ax.legend(title='Trim', loc='upper right', ncols=2)
        else:
            leg = ax.get_legend()
            if leg:
                leg.remove()

    axes[-1].set_xlabel('')

    # Install multiline algorithm labels before layout calculation so their
    # second line is included in the bottom margin rather than clipped.
    labels = [x_label_map.get(a, a) for a in algorithms]
    for ax in axes:
        ax.set_xticks(range(len(algorithms)))
        ax.set_xticklabels(labels, fontsize=9)

    plt.tight_layout()

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


def main():
    args = parse_args()
    df = collect_data(args.experiment_root, args.min_flows, args.only_max)
    if args.csv:
        csv_path = args.experiment_root / args.csv
        df.to_csv(csv_path, index=False)
        print(f"Saved CSV to {csv_path}")
    output_path = args.experiment_root / args.output

    # Keep only the selected RSS+PFLD variant among rss1/rss2/rss3 (default keeps rss1)
    if args.rss_pfld:
        df = df[(~df['algorithm'].isin(['rss1','rss2','rss3'])) | (df['algorithm'] == args.rss_pfld)]

    # If requested, subtract a communication-only percentage of ECMP max runtime from all results
    if args.comm_only_percentage is not None:
        pct = args.comm_only_percentage
        pct = (pct / 100.0) if pct > 1 else pct
        ecmp_df = df[df['algorithm'] == 'ecmp']
        if ecmp_df.empty:
            print("Warning: --comm-only-percentage ignored: no ECMP data found.")
        else:
            max_runtime = ecmp_df['completion_time'].max()
            subtract_val = pct * max_runtime
            # Subtract and clip to zero to avoid negatives
            df['completion_time'] = (df['completion_time'] - subtract_val).clip(lower=0)

    # Build x-axis label map with optional RSS+PFLD override
    x_label_map = dict(X_LABEL_MAP)
    if args.rss_pfld is not None:
        x_label_map.pop('rss1', None)
        x_label_map.pop('rss2', None)
        x_label_map[args.rss_pfld] = "RSS+\nPFLD"

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
         comm_only=(args.comm_only_percentage is not None),
     )


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
