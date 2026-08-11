#!/usr/bin/env python3
import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import matplotlib.pyplot as plt
import numpy as np


METHOD_ORDER = ["ecmp", "rss", "rack_tlp", "rss_rack", "rss_tlp", "rss_rack_tlp", "pfld"]
PAPER_METHOD_ORDER = ["ecmp", "oblivious", "reps", "rss", "pfld", "rss_rack_tlp"]
METHOD_LABEL = {
    "ecmp": "ECMP",
    "flowbender": "PLB",
    "flowlet": "Flowlet",
    "oblivious": "RPS",
    "rps": "RPS",
    "reps": "REPS",
    "rss": "RSS",
    "rack_tlp": "ECMP+RACK+TLP",
    "rss_rack": "RSS+RACK",
    "rss_tlp": "RSS+TLP",
    "rss_rack_tlp": "RSS+ TLP-RACK",
    "pfld": "RSS+ PFLD",
}
METHOD_COLOR = {
    "ecmp": "#A6A6A6",
    "flowbender": "#C4B7A6",
    "flowlet": "#A8C5D6",
    "oblivious": "#E8A5A5",
    "rps": "#E8A5A5",
    "reps": "#ECCEAA",
    "rss": "#F29572",
    "rack_tlp": "#9DAED4",
    "rss_rack": "#9DAED4",
    "rss_tlp": "#9DAED4",
    "rss_rack_tlp": "#9DAED4",
    "pfld": "#73C7B5",
}
RTO_RE = re.compile(r"rtx timer expired")
ABSOLUTE_METRICS = [
    ("completion_time_s", "App completion time (s)", False),
    ("mean_fct_us", "Mean FCT (us)", False),
    ("p99_fct_us", "p99 FCT (us)", False),
    ("total_lost_packets", "RTO-triggered losses", True),
]
REDUCTION_METRICS = [
    ("completion_time_reduction_vs_ecmp_pct", "Completion-time reduction vs ECMP (%)", False),
    ("mean_fct_reduction_vs_ecmp_pct", "Mean-FCT reduction vs ECMP (%)", False),
    ("p99_fct_reduction_vs_ecmp_pct", "p99-FCT reduction vs ECMP (%)", False),
    ("lost_packet_reduction_vs_ecmp_pct", "RTO-loss reduction vs ECMP (%)", False),
]
WORKLOAD_ORDER = [
    "cloverleaf_8",
    "hpcg_32",
    "hpcg_64",
    "icon_32",
    "icon_64",
    "lammps_32",
    "lammps_64",
    "lulesh_27",
    "lulesh_64",
    "milc_32",
    "milc_64",
    "openmx_32",
]
WORKLOAD_LABEL = {
    "cloverleaf_8": "CloverLeaf-8",
    "hpcg_32": "HPCG-32",
    "hpcg_64": "HPCG-64",
    "icon_32": "ICON-32",
    "icon_64": "ICON-64",
    "lammps_32": "LAMMPS-32",
    "lammps_64": "LAMMPS-64",
    "lulesh_27": "LULESH-27",
    "lulesh_64": "LULESH-64",
    "milc_32": "MILC-32",
    "milc_64": "MILC-64",
    "openmx_32": "OpenMX-32",
}


def parse_workload_list(value: str) -> List[str]:
    return [token.strip() for token in value.split(",") if token.strip()]


def workload_display_label(workload: str) -> str:
    label = WORKLOAD_LABEL.get(workload, workload.replace("_", "-"))
    match = re.match(r"^(.*)-(\d+)$", label)
    if not match:
        return label
    name, proc_count = match.groups()
    return f"{name}\n({proc_count},128)"


def ordered_workloads(
    rows: List[Dict[str, object]], preferred: Optional[Sequence[str]] = None
) -> List[str]:
    present = {str(row["workload"]) for row in rows}
    if preferred is not None:
        missing = [workload for workload in preferred if workload not in present]
        if missing:
            raise ValueError("Missing workloads in selected rows: " + ", ".join(missing))
        return list(preferred)

    ordered = [workload for workload in WORKLOAD_ORDER if workload in present]
    ordered_set = set(ordered)
    ordered.extend(sorted(workload for workload in present if workload not in ordered_set))
    return ordered


def filter_rows_by_workloads(
    rows: List[Dict[str, object]], workloads: Sequence[str]
) -> List[Dict[str, object]]:
    wanted = set(workloads)
    filtered = [row for row in rows if str(row["workload"]) in wanted]
    present = {str(row["workload"]) for row in filtered}
    missing = [workload for workload in workloads if workload not in present]
    if missing:
        raise ValueError("Selected workloads are missing from the collected rows: " + ", ".join(missing))
    return filtered


def sort_rows(
    rows: List[Dict[str, object]], methods: Sequence[str], workloads: Sequence[str]
) -> List[Dict[str, object]]:
    workload_index = {workload: idx for idx, workload in enumerate(workloads)}
    method_index = {method: idx for idx, method in enumerate(methods)}

    def _key(row: Dict[str, object]) -> Tuple[int, int, int, float, str]:
        seed = int(row.get("seed", 0))
        drop_rate = float(row.get("drop_rate", 0.0))
        trim_mode = str(row.get("trim_mode", ""))
        return (
            workload_index.get(str(row["workload"]), len(workload_index)),
            method_index.get(str(row["algo"]), len(method_index)),
            seed,
            drop_rate,
            trim_mode,
        )

    return sorted(rows, key=_key)


def figure_size(
    num_workloads: int,
    num_methods: int,
    num_panels: int,
    *,
    min_panel_width: float = 3.3,
    panel_height: float = 5.8,
) -> Tuple[float, float]:
    panel_width = max(min_panel_width, 0.35 * max(num_workloads, 1) + 0.12 * max(num_methods, 1) + 1.0)
    return num_panels * panel_width, panel_height


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Plot paper-style metrics from full HPC runs.")
    p.add_argument(
        "--root",
        default="nsdi_results/hpc_fullmix_cap30_drop005",
        help="Root directory with per-workload summary.csv and logs.",
    )
    p.add_argument(
        "--extra-roots",
        default="nsdi_results/hpc_rss_lr_variants_drop005",
        help="Optional comma-separated overlay roots with additional per-workload summary.csv files.",
    )
    p.add_argument(
        "--outdir",
        default="nsdi_results/hpc_paper_final3/plots_orderedchaos/rack_tlp_submission",
        help="Output directory for figure and CSV.",
    )
    p.add_argument(
        "--methods",
        default=",".join(METHOD_ORDER),
        help="Comma-separated method order to plot. All listed methods must be complete across workloads.",
    )
    p.add_argument(
        "--workloads",
        default="",
        help="Optional comma-separated workload order/filter.",
    )
    p.add_argument(
        "--figure-context",
        default="Validated full-run HPC",
        help="Prefix used in generated figure titles.",
    )
    p.add_argument(
        "--drop-rate",
        type=float,
        default=0.005,
        help="Drop probability represented by the selected rows.",
    )
    p.add_argument(
        "--allow-missing",
        action="store_true",
        help="Plot explicit gaps for missing method/workload rows instead of failing.",
    )
    return p.parse_args()


def parse_log_metrics(log_path: Path) -> Dict[str, float]:
    flow_re = re.compile(r"finished at\s+([0-9.]+).*?lost packets\s+(\d+)")
    fcts: List[float] = []
    total_lost = 0
    rto_count = 0
    with log_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if RTO_RE.search(line):
                rto_count += 1
            m = flow_re.search(line)
            if not m:
                continue
            fct = float(m.group(1))
            lost = int(m.group(2))
            fcts.append(fct)
            total_lost += lost
    if not fcts:
        return {
            "flow_count": 0.0,
            "mean_fct_us": float("nan"),
            "max_fct_us": float("nan"),
            "p99_fct_us_logparse": float("nan"),
            "total_lost_packets": float("nan"),
            "rto_count": float(rto_count),
        }
    s = sorted(fcts)
    p99 = s[min(int(0.99 * (len(s) - 1)), len(s) - 1)]
    return {
        "flow_count": float(len(fcts)),
        "mean_fct_us": float(sum(fcts) / len(fcts)),
        "max_fct_us": float(max(fcts)),
        "p99_fct_us_logparse": float(p99),
        "total_lost_packets": float(total_lost),
        "rto_count": float(rto_count),
    }


def _row_key(row: Dict[str, object]) -> Tuple[str, str, int, float, str]:
    return (
        str(row["workload"]),
        str(row["algo"]),
        int(row["seed"]),
        float(row["drop_rate"]),
        str(row["trim_mode"]),
    )


def collect_rows(roots: Iterable[Path], methods: List[str]) -> List[Dict[str, object]]:
    keyed_rows: Dict[Tuple[str, str, int, float, str], Dict[str, object]] = {}
    algo_order = {algo: idx for idx, algo in enumerate(methods)}

    for root in roots:
        if not root.exists():
            continue
        for wl_dir in sorted([p for p in root.iterdir() if p.is_dir()]):
            summary = wl_dir / "summary.csv"
            if not summary.exists():
                continue
            with summary.open("r", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    if int(row.get("exit_code", "0")) != 0:
                        continue
                    if row["algo"] not in algo_order:
                        continue
                    log_file = Path(row["log_file"])
                    if not log_file.is_absolute():
                        log_file = Path.cwd() / log_file
                    if not log_file.exists():
                        continue
                    parsed = parse_log_metrics(log_file)
                    normalized = {
                        "workload": row["workload"],
                        "algo": row["algo"],
                        "seed": int(row["seed"]),
                        "drop_rate": float(row["drop_rate"]),
                        "trim_mode": row["trim_mode"],
                        "flow_count": int(parsed["flow_count"]),
                        "max_host_time_ns": int(row["max_host_time_ns"]),
                        "completion_time_s": float(row["max_host_time_ns"]) / 1e9,
                        "mean_fct_us": parsed["mean_fct_us"],
                        "max_fct_us": parsed["max_fct_us"],
                        "p99_fct_us": float(row["fct_p99_us"]),
                        "p99_fct_us_logparse": parsed["p99_fct_us_logparse"],
                        "rto_count": int(parsed["rto_count"]),
                        "total_lost_packets": int(parsed["total_lost_packets"]),
                        "log_file": str(log_file),
                    }
                    keyed_rows[_row_key(normalized)] = normalized

    return sorted(
        keyed_rows.values(),
        key=lambda row: (
            str(row["workload"]),
            algo_order.get(str(row["algo"]), len(algo_order)),
            int(row["seed"]),
            float(row["drop_rate"]),
            str(row["trim_mode"]),
        ),
    )


def validate_rows(
    rows: List[Dict[str, object]], methods: List[str], workload_order: Optional[Sequence[str]] = None,
    allow_missing: bool = False,
) -> List[str]:
    grouped: Dict[str, Dict[str, Dict[str, object]]] = defaultdict(dict)
    for row in rows:
        grouped[str(row["workload"])][str(row["algo"])] = row

    if workload_order is None:
        workloads = ordered_workloads(rows)
    else:
        workloads = list(workload_order)
        missing_workloads = [workload for workload in workloads if workload not in grouped]
        if missing_workloads:
            raise ValueError("Missing workloads in collected rows: " + ", ".join(missing_workloads))
    if not workloads:
        raise ValueError("No successful rows found for the selected methods.")

    missing: List[str] = []
    bad_rows: List[str] = []
    for workload in workloads:
        flow_counts: Dict[str, int] = {}
        for method in methods:
            row = grouped[workload].get(method)
            if row is None:
                missing.append(f"{workload}:{method}")
                continue
            flow_count = int(row["flow_count"])
            max_host_time_ns = int(row["max_host_time_ns"])
            if flow_count <= 0 or max_host_time_ns <= 0:
                bad_rows.append(f"{workload}:{method}")
                continue
            flow_counts[method] = flow_count
        if flow_counts and len(set(flow_counts.values())) != 1:
            raise ValueError(f"Flow-count mismatch for {workload}: {flow_counts}")

    if missing and not allow_missing:
        raise ValueError("Missing successful rows for selected methods: " + ", ".join(missing))
    if bad_rows:
        raise ValueError("Incomplete rows detected: " + ", ".join(bad_rows))
    return workloads


def _percent_reduction(value: float, baseline: float) -> float:
    if baseline < 0:
        raise ValueError(f"Baseline must be non-negative, got {baseline}")
    if baseline == 0:
        if value == 0:
            return 0.0
        raise ValueError("Cannot compute percentage reduction against a zero baseline.")
    return 100.0 * (1.0 - (value / baseline))


def add_relative_metrics(
    rows: List[Dict[str, object]], methods: List[str], workload_order: Optional[Sequence[str]] = None,
    allow_missing: bool = False,
) -> None:
    if "ecmp" not in methods:
        raise ValueError("Relative metrics require the ECMP baseline to be included in --methods.")
    workloads = validate_rows(rows, methods, workload_order, allow_missing)
    grouped: Dict[str, Dict[str, Dict[str, object]]] = defaultdict(dict)
    for row in rows:
        grouped[str(row["workload"])][str(row["algo"])] = row

    for workload in workloads:
        baseline = grouped[workload]["ecmp"]
        completion_base = float(baseline["completion_time_s"])
        mean_fct_base = float(baseline["mean_fct_us"])
        p99_base = float(baseline["p99_fct_us"])
        lost_base = float(baseline["total_lost_packets"])
        for method in methods:
            row = grouped[workload].get(method)
            if row is None:
                continue
            row["completion_time_reduction_vs_ecmp_pct"] = _percent_reduction(
                float(row["completion_time_s"]), completion_base
            )
            row["mean_fct_reduction_vs_ecmp_pct"] = _percent_reduction(
                float(row["mean_fct_us"]), mean_fct_base
            )
            row["p99_fct_reduction_vs_ecmp_pct"] = _percent_reduction(
                float(row["p99_fct_us"]), p99_base
            )
            row["lost_packet_reduction_vs_ecmp_pct"] = _percent_reduction(
                float(row["total_lost_packets"]), lost_base
            )


def plot_metrics(
    rows: List[Dict[str, object]],
    methods: List[str],
    metric_defs: List[Tuple[str, str, bool]],
    out_png: Path,
    title: str,
    workload_order: Optional[Sequence[str]] = None,
    allow_missing: bool = False,
) -> None:
    grouped: Dict[str, Dict[str, Dict[str, float]]] = defaultdict(dict)
    for r in rows:
        grouped[str(r["workload"])][str(r["algo"])] = {
            metric_key: float(r[metric_key]) for metric_key, _, _ in metric_defs
        }

    workloads = validate_rows(rows, methods, workload_order, allow_missing)
    x = np.arange(len(workloads))
    width = min(0.82 / max(len(methods), 1), 0.14)
    offsets = (np.arange(len(methods)) - (len(methods) - 1) / 2.0) * width

    fig, axes = plt.subplots(
        1,
        len(metric_defs),
        figsize=figure_size(len(workloads), len(methods), len(metric_defs), panel_height=3.65),
        dpi=180,
    )
    axes = np.atleast_1d(axes)
    xlabels = [workload_display_label(workload) for workload in workloads]
    multiline_labels = any("\n" in label for label in xlabels)
    rotation = 0 if multiline_labels else (20 if len(workloads) <= 6 else 28)
    tick_fontsize = 8 if multiline_labels else (10 if len(workloads) <= 6 else 9)
    tick_align = "center" if multiline_labels else "right"

    for ax, (metric_key, ylabel, use_log) in zip(axes, metric_defs):
        metric_values: List[float] = []
        for method, off in zip(methods, offsets):
            y = []
            for wl in workloads:
                val = grouped[wl].get(method, {}).get(metric_key, np.nan)
                y.append(val)
            metric_values.extend(v for v in y if np.isfinite(v))
            ax.bar(
                x + off,
                y,
                width=width,
                color=METHOD_COLOR[method],
                edgecolor="black",
                linewidth=0.8,
            )
        ax.set_xticks(x)
        ax.set_xticklabels(xlabels, rotation=rotation, ha=tick_align)
        ax.set_ylabel(ylabel)
        ax.tick_params(axis="x", labelsize=tick_fontsize)
        if use_log:
            ax.set_yscale("log")
        elif metric_values:
            metric_min = min(metric_values)
            metric_max = max(metric_values)
            if metric_min >= 0:
                upper = metric_max * 1.08 if metric_max > 0 else 1.0
                ax.set_ylim(0, upper)
            else:
                span = metric_max - metric_min
                pad = span * 0.08 if span > 0 else 1.0
                ax.set_ylim(metric_min - pad, metric_max + pad)
            ax.axhline(0, color="black", linewidth=0.6, alpha=0.5)
        ax.grid(axis="y", alpha=0.25)

    handles = [
        plt.Rectangle((0, 0), 1, 1, facecolor=METHOD_COLOR[m], edgecolor="black", linewidth=0.4)
        for m in methods
    ]
    labels = [METHOD_LABEL[m] for m in methods]
    fig.legend(
        handles,
        labels,
        loc="upper center",
        ncol=min(len(methods), 6),
        frameon=False,
        bbox_to_anchor=(0.5, 1.01),
    )
    if title.strip():
        fig.suptitle(title, y=1.03)
    fig.tight_layout(rect=[0, 0, 1, 0.92 if title.strip() else 0.95])
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png, bbox_inches="tight")
    fig.savefig(out_png.with_suffix(".pdf"), bbox_inches="tight")
    plt.close(fig)


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(
            f, fieldnames=list(rows[0].keys()), lineterminator="\n"
        )
        w.writeheader()
        w.writerows(rows)


def main() -> int:
    args = parse_args()
    roots = [Path(args.root)]
    roots.extend(Path(token.strip()) for token in args.extra_roots.split(",") if token.strip())
    methods = [token.strip() for token in args.methods.split(",") if token.strip()]
    selected_workloads = parse_workload_list(args.workloads)
    outdir = Path(args.outdir)

    rows = collect_rows(roots, methods)
    if selected_workloads:
        rows = filter_rows_by_workloads(rows, selected_workloads)
    workload_order = ordered_workloads(rows, selected_workloads or None)
    rows = sort_rows(rows, methods, workload_order)
    validate_rows(rows, methods, workload_order, args.allow_missing)
    add_relative_metrics(rows, methods, workload_order, args.allow_missing)
    write_csv(outdir / "hpc_full_run_paper_metrics.csv", rows)
    reduction_title = (
        f"{args.figure_context} reduction vs ECMP"
        if args.figure_context else ""
    )
    absolute_title = (
        f"{args.figure_context} absolute metrics"
        if args.figure_context else ""
    )
    plot_metrics(
        rows,
        methods,
        REDUCTION_METRICS,
        outdir / "hpc_full_run_paper_metrics.png",
        reduction_title,
        workload_order,
        args.allow_missing,
    )
    plot_metrics(
        rows,
        methods,
        ABSOLUTE_METRICS,
        outdir / "hpc_full_run_paper_metrics_absolute.png",
        absolute_title,
        workload_order,
        args.allow_missing,
    )

    print(f"[INFO] wrote {outdir / 'hpc_full_run_paper_metrics.csv'}")
    print(f"[INFO] wrote {outdir / 'hpc_full_run_paper_metrics.png'}")
    print(f"[INFO] wrote {outdir / 'hpc_full_run_paper_metrics_absolute.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
