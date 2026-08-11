#!/usr/bin/env python3
import argparse
import csv
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import matplotlib.pyplot as plt
import numpy as np

from plot_hpc_full_paper_metrics import (
    METHOD_COLOR,
    METHOD_LABEL,
    METHOD_ORDER,
    WORKLOAD_LABEL,
    filter_rows_by_workloads,
    ordered_workloads,
    parse_workload_list,
    sort_rows,
    workload_display_label,
)


METRIC_DEFS = [
    ("mean_fct_speedup_vs_ecmp", "Mean FCT"),
    ("p99_fct_speedup_vs_ecmp", "p99 FCT"),
    ("comm_only_speedup_vs_ecmp", "Comm-only Runtime"),
    ("completion_speedup_vs_ecmp", "Overall Runtime"),
]


@dataclass(frozen=True)
class PlotVariant:
    name: str
    suffix: str
    nrows: int
    ncols: int
    panel_width: float
    panel_height: float
    tick_fontsize: float
    ytick_fontsize: float
    ylabel_fontsize: float
    panel_title_fontsize: float
    figure_title_fontsize: float
    legend_fontsize: float
    legend_cols: int
    legend_anchor_y: float
    handlelength: float
    handleheight: float
    columnspacing: float
    bar_linewidth: float
    spine_linewidth: float
    baseline_linewidth: float
    grid_linewidth: float
    tick_length: float
    tick_width: float
    tight_top_with_title: float
    tight_top_no_title: float
    h_pad: float
    w_pad: float
    draw_ecmp_bars: bool = True


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Plot paper-style HPC speedups vs ECMP with an implied communication-only panel."
    )
    p.add_argument(
        "--metrics-csv",
        default="nsdi_results/hpc_paper_final3/plots_orderedchaos/rack_tlp_submission/hpc_full_run_paper_metrics.csv",
        help="CSV produced by plot_hpc_full_paper_metrics.py.",
    )
    p.add_argument(
        "--comm-share-csv",
        default="nsdi_results/hpc_paper_final3/plots_orderedchaos/comm_share/comm_share_with_larger_traces.csv",
        help="Trace-based communication-share CSV. Defaults to the larger-trace-aware paper dataset.",
    )
    p.add_argument(
        "--outdir",
        default="nsdi_results/hpc_paper_final3/plots_orderedchaos/rack_tlp_submission",
        help="Output directory for the derived CSV and figure.",
    )
    p.add_argument(
        "--out-prefix",
        default="hpc_full_run_paper_speedup",
        help="Output file prefix inside --outdir.",
    )
    p.add_argument(
        "--methods",
        default=",".join(METHOD_ORDER),
        help="Comma-separated method order to plot. ECMP must be included.",
    )
    p.add_argument(
        "--workloads",
        default="",
        help="Optional comma-separated workload order/filter.",
    )
    p.add_argument(
        "--title",
        default="",
        help="Optional figure title.",
    )
    p.add_argument(
        "--variants",
        default="paper,wide",
        help=(
            "Comma-separated output variants. Supported values: "
            "paper, stacked, wide, no_ecmp."
        ),
    )
    p.add_argument(
        "--allow-missing",
        action="store_true",
        help="Plot explicit gaps for missing method/workload rows instead of failing.",
    )
    return p.parse_args()


def load_comm_share(path: Path) -> Dict[str, float]:
    out: Dict[str, float] = {}
    with path.open("r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            out[row["workload"]] = float(row["comm_pct_trace_critical"])
    return out


def load_rows(path: Path, methods: List[str]) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    with path.open("r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["algo"] not in methods:
                continue
            rows.append(
                {
                    "workload": row["workload"],
                    "algo": row["algo"],
                    "completion_time_s": float(row["completion_time_s"]),
                    "mean_fct_us": float(row["mean_fct_us"]),
                    "p99_fct_us": float(row["p99_fct_us"]),
                    "completion_time_reduction_vs_ecmp_pct": float(
                        row["completion_time_reduction_vs_ecmp_pct"]
                    ),
                }
            )
    return rows


def derive_rows(
    rows: List[Dict[str, object]],
    methods: List[str],
    comm_share: Dict[str, float],
    allow_missing: bool = False,
) -> List[Dict[str, object]]:
    if "ecmp" not in methods:
        raise ValueError("Speedup plots require the ECMP baseline to be included in --methods.")

    grouped: Dict[str, Dict[str, Dict[str, object]]] = defaultdict(dict)
    for row in rows:
        grouped[str(row["workload"])][str(row["algo"])] = row

    workloads = sorted(grouped.keys())
    if not workloads:
        raise ValueError("No rows found for the selected methods.")

    derived: List[Dict[str, object]] = []
    for workload in workloads:
        if workload not in comm_share:
            raise ValueError(f"Missing communication share for workload {workload}")
        missing = [method for method in methods if method not in grouped[workload]]
        if missing and not allow_missing:
            raise ValueError(f"Missing rows for {workload}: {', '.join(missing)}")

        baseline = grouped[workload]["ecmp"]
        baseline_completion = float(baseline["completion_time_s"])
        baseline_mean_fct = float(baseline["mean_fct_us"])
        baseline_p99_fct = float(baseline["p99_fct_us"])
        comm_share_pct = float(comm_share[workload])
        if comm_share_pct <= 0.0:
            raise ValueError(
                f"Communication share for workload {workload} must be positive, got {comm_share_pct}"
            )

        for method in methods:
            row = grouped[workload].get(method)
            if row is None:
                continue
            overall_reduction_pct = float(row["completion_time_reduction_vs_ecmp_pct"])
            comm_only_reduction_pct = overall_reduction_pct / (comm_share_pct / 100.0)
            comm_only_factor = 1.0 - (comm_only_reduction_pct / 100.0)
            if comm_only_factor <= 0.0:
                raise ValueError(
                    f"Implied communication-only reduction is invalid for {workload}:{method}: "
                    f"{comm_only_reduction_pct}%"
                )
            derived.append(
                {
                    "workload": workload,
                    "algo": method,
                    "comm_share_pct": comm_share_pct,
                    "overall_reduction_pct": overall_reduction_pct,
                    "comm_only_reduction_pct": comm_only_reduction_pct,
                    "completion_speedup_vs_ecmp": baseline_completion
                    / float(row["completion_time_s"]),
                    "mean_fct_speedup_vs_ecmp": baseline_mean_fct / float(row["mean_fct_us"]),
                    "p99_fct_speedup_vs_ecmp": baseline_p99_fct / float(row["p99_fct_us"]),
                    "comm_only_speedup_vs_ecmp": 1.0 / comm_only_factor,
                }
            )
    return derived


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f, fieldnames=list(rows[0].keys()), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def compact_workload_label(workload: str) -> str:
    """Single-line label like 'ICON-32' for clean rotated display."""
    return WORKLOAD_LABEL.get(workload, workload.replace("_", "-"))


def speedup_ylim(metric_values: Sequence[float]) -> Tuple[float, float]:
    metric_min = min(metric_values)
    metric_max = max(metric_values)
    span = metric_max - metric_min
    pad = max(0.003, span * 0.08)
    lower = min(1.0, metric_min) - pad
    upper = max(1.0, metric_max) + pad
    return lower, upper


def output_path_for_variant(out_png: Path, suffix: str) -> Path:
    if not suffix:
        return out_png
    return out_png.with_name(f"{out_png.stem}{suffix}{out_png.suffix}")


def build_variants(
    variant_names: Sequence[str],
    num_workloads: int,
    num_methods: int,
    multiline_labels: bool,
) -> List[PlotVariant]:
    # Figure sized close to paper text-width so LaTeX scaling preserves readability.
    available = {
        "paper": PlotVariant(
            name="paper",
            suffix="",
            nrows=2,
            ncols=2,
            panel_width=4.2,
            panel_height=3.2,
            tick_fontsize=9.5,
            ytick_fontsize=9.5,
            ylabel_fontsize=10,
            panel_title_fontsize=11,
            figure_title_fontsize=12,
            legend_fontsize=10,
            legend_cols=6,
            legend_anchor_y=-0.01,
            handlelength=1.5,
            handleheight=0.9,
            columnspacing=1.2,
            bar_linewidth=0.7,
            spine_linewidth=0.6,
            baseline_linewidth=0.6,
            grid_linewidth=0.5,
            tick_length=4.0,
            tick_width=0.7,
            tight_top_with_title=0.93,
            tight_top_no_title=0.97,
            h_pad=2.5,
            w_pad=1.5,
        ),
        "stacked": PlotVariant(
            name="stacked",
            suffix="_stacked",
            nrows=4,
            ncols=1,
            panel_width=7.6,
            panel_height=2.2,
            tick_fontsize=9.5,
            ytick_fontsize=9.5,
            ylabel_fontsize=10,
            panel_title_fontsize=11,
            figure_title_fontsize=12,
            legend_fontsize=10,
            legend_cols=6,
            legend_anchor_y=-0.01,
            handlelength=1.5,
            handleheight=0.9,
            columnspacing=1.2,
            bar_linewidth=0.7,
            spine_linewidth=0.6,
            baseline_linewidth=0.6,
            grid_linewidth=0.5,
            tick_length=4.0,
            tick_width=0.7,
            tight_top_with_title=0.94,
            tight_top_no_title=0.98,
            h_pad=2.0,
            w_pad=1.0,
        ),
        "wide": PlotVariant(
            name="wide",
            suffix="_wide",
            nrows=1,
            ncols=4,
            panel_width=3.0,
            panel_height=3.6,
            tick_fontsize=9.5,
            ytick_fontsize=9.5,
            ylabel_fontsize=10,
            panel_title_fontsize=11,
            figure_title_fontsize=12,
            legend_fontsize=10,
            legend_cols=6,
            legend_anchor_y=-0.01,
            handlelength=1.5,
            handleheight=0.9,
            columnspacing=1.2,
            bar_linewidth=0.7,
            spine_linewidth=0.6,
            baseline_linewidth=0.6,
            grid_linewidth=0.5,
            tick_length=4.0,
            tick_width=0.7,
            tight_top_with_title=0.91,
            tight_top_no_title=0.97,
            h_pad=1.5,
            w_pad=1.0,
        ),
        "no_ecmp": PlotVariant(
            name="no_ecmp",
            suffix="_no_ecmp",
            nrows=2,
            ncols=2,
            panel_width=4.2,
            panel_height=3.2,
            tick_fontsize=9.5,
            ytick_fontsize=9.5,
            ylabel_fontsize=10,
            panel_title_fontsize=11,
            figure_title_fontsize=12,
            legend_fontsize=10,
            legend_cols=6,
            legend_anchor_y=-0.01,
            handlelength=1.5,
            handleheight=0.9,
            columnspacing=1.2,
            bar_linewidth=0.7,
            spine_linewidth=0.6,
            baseline_linewidth=0.6,
            grid_linewidth=0.5,
            tick_length=4.0,
            tick_width=0.7,
            tight_top_with_title=0.93,
            tight_top_no_title=0.97,
            h_pad=2.5,
            w_pad=1.5,
            draw_ecmp_bars=False,
        ),
    }

    resolved: List[PlotVariant] = []
    seen = set()
    for name in variant_names:
        key = name.strip()
        if not key or key in seen:
            continue
        if key not in available:
            raise ValueError(
                f"Unknown plot variant '{key}'. Supported values: "
                + ", ".join(sorted(available.keys()))
            )
        resolved.append(available[key])
        seen.add(key)
    if not resolved:
        raise ValueError("At least one plot variant must be selected.")
    return resolved


def plot_variant(
    grouped: Dict[str, Dict[str, Dict[str, float]]],
    methods: List[str],
    workloads: List[str],
    xlabels: List[str],
    title: str,
    variant: PlotVariant,
    out_png: Path,
) -> Path:
    draw_methods = methods if variant.draw_ecmp_bars else [m for m in methods if m != "ecmp"]
    x = np.arange(len(workloads))
    width = min(0.84 / max(len(draw_methods), 1), 0.18)
    offsets = (np.arange(len(draw_methods)) - (len(draw_methods) - 1) / 2.0) * width
    rotation = 35
    tick_align = "right"

    fig, axes_2d = plt.subplots(
        variant.nrows,
        variant.ncols,
        figsize=(variant.ncols * variant.panel_width, variant.nrows * variant.panel_height),
        dpi=180,
    )
    axes = np.atleast_1d(axes_2d).flatten()

    for idx, (ax, (metric_key, panel_title)) in enumerate(zip(axes, METRIC_DEFS)):
        metric_values: List[float] = []
        for method, off in zip(draw_methods, offsets):
            y = [grouped.get(workload, {}).get(method, {}).get(metric_key, np.nan) for workload in workloads]
            metric_values.extend(v for v in y if np.isfinite(v))
            ax.bar(
                x + off,
                y,
                width=width,
                color=METHOD_COLOR[method],
                edgecolor="black",
                linewidth=variant.bar_linewidth,
            )
        ax.set_title(panel_title, fontsize=variant.panel_title_fontsize, loc="center", pad=8)
        ax.set_xticks(x)
        row_idx = idx // variant.ncols
        col_idx = idx % variant.ncols
        show_xlabels = row_idx == variant.nrows - 1
        show_ylabel = variant.ncols == 1 or col_idx == 0
        if show_xlabels:
            ax.set_xticklabels(
                xlabels, rotation=rotation, ha=tick_align, fontsize=variant.tick_fontsize
            )
        else:
            ax.set_xticklabels([])
        ax.set_ylabel("Speedup vs ECMP" if show_ylabel else "", fontsize=variant.ylabel_fontsize)
        ax.tick_params(
            axis="x",
            labelsize=variant.tick_fontsize,
            length=variant.tick_length,
            width=variant.tick_width,
        )
        ax.tick_params(
            axis="y",
            labelsize=variant.ytick_fontsize,
            length=variant.tick_length,
            width=variant.tick_width,
        )
        for spine in ax.spines.values():
            spine.set_linewidth(variant.spine_linewidth)
        ax.grid(axis="y", alpha=0.25, linewidth=variant.grid_linewidth)
        ax.set_axisbelow(True)
        if metric_values:
            ax.set_ylim(*speedup_ylim(metric_values))
        ax.axhline(
            1.0,
            color="black",
            linewidth=variant.baseline_linewidth,
            alpha=0.55,
            linestyle="--" if not variant.draw_ecmp_bars else "-",
        )

    handles = [
        plt.Rectangle(
            (0, 0),
            1,
            1,
            facecolor=METHOD_COLOR[m],
            edgecolor="black",
            linewidth=max(0.8, variant.bar_linewidth - 0.2),
        )
        for m in draw_methods
    ]
    labels = [METHOD_LABEL[m] for m in draw_methods]
    fig.legend(
        handles,
        labels,
        loc="lower center",
        ncol=min(variant.legend_cols, max(len(draw_methods), 1)),
        frameon=False,
        bbox_to_anchor=(0.5, -0.01),
        fontsize=variant.legend_fontsize,
        handlelength=variant.handlelength,
        handleheight=variant.handleheight,
        columnspacing=variant.columnspacing,
    )
    if title.strip():
        fig.suptitle(title, y=1.03, fontsize=variant.figure_title_fontsize)
    tight_bottom = 0.12 if variant.nrows == 1 else 0.0
    fig.tight_layout(
        rect=[
            0,
            tight_bottom,
            1,
            variant.tight_top_with_title if title.strip() else variant.tight_top_no_title,
        ],
        h_pad=variant.h_pad,
        w_pad=variant.w_pad,
    )

    variant_png = output_path_for_variant(out_png, variant.suffix)
    variant_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(variant_png, bbox_inches="tight")
    fig.savefig(variant_png.with_suffix(".pdf"), bbox_inches="tight")
    plt.close(fig)
    return variant_png


def plot(
    rows: List[Dict[str, object]],
    methods: List[str],
    workload_order: List[str],
    out_png: Path,
    title: str,
    variant_names: Sequence[str],
) -> List[Path]:
    grouped: Dict[str, Dict[str, Dict[str, float]]] = defaultdict(dict)
    for row in rows:
        grouped[str(row["workload"])][str(row["algo"])] = {
            "completion_speedup_vs_ecmp": float(row["completion_speedup_vs_ecmp"]),
            "mean_fct_speedup_vs_ecmp": float(row["mean_fct_speedup_vs_ecmp"]),
            "p99_fct_speedup_vs_ecmp": float(row["p99_fct_speedup_vs_ecmp"]),
            "comm_only_speedup_vs_ecmp": float(row["comm_only_speedup_vs_ecmp"]),
        }

    workloads = workload_order
    xlabels = [compact_workload_label(workload) for workload in workloads]
    variants = build_variants(
        variant_names,
        len(workloads),
        len(methods),
        any("\n" in label for label in xlabels),
    )
    written: List[Path] = []
    for variant in variants:
        written.append(plot_variant(grouped, methods, workloads, xlabels, title, variant, out_png))
    return written


def main() -> int:
    args = parse_args()
    methods = [token.strip() for token in args.methods.split(",") if token.strip()]
    variant_names = [token.strip() for token in args.variants.split(",") if token.strip()]
    selected_workloads = parse_workload_list(args.workloads)
    metrics_csv = Path(args.metrics_csv)
    comm_share_csv = Path(args.comm_share_csv)
    outdir = Path(args.outdir)

    rows = load_rows(metrics_csv, methods)
    if selected_workloads:
        rows = filter_rows_by_workloads(rows, selected_workloads)
    workload_order = ordered_workloads(rows, selected_workloads or None)
    comm_share = load_comm_share(comm_share_csv)
    derived = derive_rows(rows, methods, comm_share, args.allow_missing)
    derived = sort_rows(derived, methods, workload_order)

    out_prefix = outdir / args.out_prefix
    write_csv(out_prefix.with_suffix(".csv"), derived)
    plot_outputs = plot(
        derived,
        methods,
        workload_order,
        out_prefix.with_suffix(".png"),
        args.title,
        variant_names,
    )
    print(f"[INFO] wrote {out_prefix.with_suffix('.csv')}")
    for plot_output in plot_outputs:
        print(f"[INFO] wrote {plot_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
