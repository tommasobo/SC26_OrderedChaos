#!/usr/bin/env python3
"""
Validate communication-share percentages against ATLAHS Figure-10-style method.

Method:
  - Parse PMPI trace files per rank.
  - Runtime per rank = last_end - first_start across trace lines.
  - Communication interval = any MPI call except lightweight metadata calls
    (MPI_Wtime, MPI_Comm_rank, MPI_Comm_size, MPI_Init*, MPI_Finalize, etc.).
  - Non-overlapped computation per rank = runtime - union(comm_intervals).
  - Workload-level percentage = critical-rank (max runtime) percentage.
"""

from __future__ import annotations

import argparse
import io
import re
import urllib.request
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif"],
    "mathtext.fontset": "dejavuserif",
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})


SKIP_COMM_PREFIXES: Tuple[str, ...] = (
    "MPI_Comm_rank",
    "MPI_Comm_size",
    "MPI_Wtime",
    "MPI_Init",
    "MPI_Init_thread",
    "MPI_Finalize",
    "MPI_Get_processor_name",
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Validate comm-share against Figure-10-style trace parsing.")
    p.add_argument(
        "--proxy-csv",
        default="nsdi_results/hpc_paper_final3/plots_orderedchaos/comm_share/comm_share_per_workload.csv",
        help="Existing proxy CSV to compare against.",
    )
    p.add_argument(
        "--out-dir",
        default="nsdi_results/hpc_paper_final3/plots_orderedchaos/comm_share",
        help="Output directory for validation CSVs/plots.",
    )
    p.add_argument(
        "--local-traces-root",
        default="data/hpc",
        help="Local traces root. If mpi_traces are missing, falls back to remote URL.",
    )
    p.add_argument(
        "--remote-traces-root",
        default="http://storage2.spcl.ethz.ch/traces/hpc",
        help="Remote traces root URL.",
    )
    p.add_argument(
        "--workloads",
        default="",
        help="Optional comma-separated workloads (e.g., icon_32,icon_64).",
    )
    p.add_argument(
        "--add-lulesh8",
        action="store_true",
        help="Also include lulesh_8 (appears in ATLAHS Figure 10 context).",
    )
    return p.parse_args()


def workload_to_app(workload: str) -> str:
    return workload.rsplit("_", 1)[0]


def local_rank_files(local_root: Path, app: str, workload: str) -> List[Path]:
    trace_dir = local_root / app / workload / "mpi_traces"
    if not trace_dir.is_dir():
        return []
    files = sorted(trace_dir.glob("pmpi-trace-rank-*.txt"))
    return files


def remote_rank_ids(remote_root: str, app: str, workload: str) -> List[int]:
    url = f"{remote_root}/{app}/{workload}/mpi_traces/"
    html = urllib.request.urlopen(url, timeout=30).read().decode("utf-8", "ignore")
    ranks = sorted(set(int(m.group(1)) for m in re.finditer(r"pmpi-trace-rank-(\d+)\.txt", html)))
    return ranks


def parse_trace_lines(lines: Iterable[str]) -> Optional[Tuple[int, int, int]]:
    total_start: Optional[int] = None
    total_end: Optional[int] = None

    merged_start: Optional[int] = None
    merged_end: Optional[int] = None
    comm_total = 0

    for line in lines:
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split(":")
        if len(parts) < 3:
            continue

        name = parts[0]
        try:
            st = int(parts[1])
            en = int(parts[-1])
        except ValueError:
            continue

        if total_start is None or st < total_start:
            total_start = st
        if total_end is None or en > total_end:
            total_end = en

        if any(name.startswith(prefix) for prefix in SKIP_COMM_PREFIXES):
            continue

        if merged_start is None:
            merged_start, merged_end = st, en
        elif st > int(merged_end):
            comm_total += int(merged_end) - int(merged_start)
            merged_start, merged_end = st, en
        elif en > int(merged_end):
            merged_end = en

    if merged_start is not None and merged_end is not None:
        comm_total += int(merged_end) - int(merged_start)

    if total_start is None or total_end is None:
        return None

    total = total_end - total_start
    if total <= 0:
        return None
    compute = max(total - comm_total, 0)
    return total, comm_total, compute


def parse_rank_local(path: Path) -> Optional[Tuple[int, int, int]]:
    with path.open("r", encoding="utf-8", errors="ignore") as f:
        return parse_trace_lines(f)


def parse_rank_remote(remote_root: str, app: str, workload: str, rank: int) -> Optional[Tuple[int, int, int]]:
    url = f"{remote_root}/{app}/{workload}/mpi_traces/pmpi-trace-rank-{rank}.txt"
    with urllib.request.urlopen(url, timeout=180) as resp:
        text_stream = io.TextIOWrapper(resp, encoding="utf-8", errors="ignore")
        return parse_trace_lines(text_stream)


def compute_workload_metrics(
    local_root: Path,
    remote_root: str,
    workload: str,
) -> Optional[dict]:
    app = workload_to_app(workload)
    rows: List[Tuple[int, int, int, int]] = []

    local_files = local_rank_files(local_root, app, workload)
    if local_files:
        for p in local_files:
            m = re.search(r"pmpi-trace-rank-(\d+)\.txt$", p.name)
            if not m:
                continue
            rank = int(m.group(1))
            parsed = parse_rank_local(p)
            if parsed is None:
                continue
            total, comm, comp = parsed
            rows.append((rank, total, comm, comp))
    else:
        rank_ids = remote_rank_ids(remote_root, app, workload)
        for rank in rank_ids:
            parsed = parse_rank_remote(remote_root, app, workload, rank)
            if parsed is None:
                continue
            total, comm, comp = parsed
            rows.append((rank, total, comm, comp))

    if not rows:
        return None

    critical = max(rows, key=lambda x: x[1])
    crit_rank, crit_total, crit_comm, crit_comp = critical

    mean_comm_pct = float(np.mean([100.0 * comm / total for _, total, comm, _ in rows]))
    mean_comp_pct = float(np.mean([100.0 * comp / total for _, total, _, comp in rows]))

    return {
        "workload": workload,
        "app": app,
        "num_ranks": len(rows),
        "critical_rank": crit_rank,
        "critical_runtime_us": float(crit_total),
        "comm_pct_trace_critical": 100.0 * crit_comm / crit_total,
        "compute_pct_trace_critical": 100.0 * crit_comp / crit_total,
        "comm_pct_trace_mean": mean_comm_pct,
        "compute_pct_trace_mean": mean_comp_pct,
        "trace_method": "MPI runtime - union(MPI-call intervals), critical rank",
    }


def plot_trace_stack(df: pd.DataFrame, out_png: Path) -> None:
    if df.empty:
        return
    dd = df.sort_values("workload")
    x = np.arange(len(dd))
    comp = dd["compute_pct_trace_critical"].to_numpy()
    comm = dd["comm_pct_trace_critical"].to_numpy()

    fig, ax = plt.subplots(figsize=(max(10, 1.1 * len(dd)), 4.6))
    ax.bar(x, comp, color="#1f4e79", edgecolor="black", linewidth=0.6, label="Non-overlapped computation")
    ax.bar(x, comm, bottom=comp, color="#88c0f0", edgecolor="black", linewidth=0.6, label="Communication")

    for i, v in enumerate(comp):
        y = max(3.0, v * 0.55)
        ax.text(i, y, f"{v:.1f}%", ha="center", va="center", color="white", fontsize=8, fontweight="bold")

    ax.set_xticks(x)
    ax.set_xticklabels(dd["workload"].tolist(), rotation=20, ha="right")
    ax.set_ylim(0, 100)
    ax.set_ylabel("Share of total runtime (%)")
    ax.set_title("HPC workload runtime composition (Figure-10-style trace method)")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_png, dpi=220, bbox_inches="tight")
    plt.close(fig)


def plot_proxy_vs_trace(df: pd.DataFrame, out_png: Path) -> None:
    if df.empty or "comm_pct_of_total_proxy" not in df.columns:
        return
    d = df.dropna(subset=["comm_pct_of_total_proxy"]).copy()
    if d.empty:
        return
    d = d.sort_values("workload")
    x = np.arange(len(d))
    width = 0.36

    trace_vals = d["comm_pct_trace_critical"].to_numpy()
    proxy_vals = d["comm_pct_of_total_proxy"].to_numpy()
    diff_vals = np.abs(trace_vals - proxy_vals)

    fig, ax = plt.subplots(figsize=(max(10, 1.2 * len(d)), 4.6))
    ax.bar(x - width / 2.0, trace_vals, width=width, color="#2ca02c", edgecolor="black", linewidth=0.6, label="Trace-based comm %")
    ax.bar(x + width / 2.0, proxy_vals, width=width, color="#ff7f0e", edgecolor="black", linewidth=0.6, label="Previous proxy comm %")

    for i, dv in enumerate(diff_vals):
        y = max(trace_vals[i], proxy_vals[i]) + 1.0
        ax.text(i, y, f"Δ {dv:.1f}pp", ha="center", va="bottom", fontsize=8, color="#b22222")

    ax.set_xticks(x)
    ax.set_xticklabels(d["workload"].tolist(), rotation=20, ha="right")
    ax.set_ylabel("Communication share of runtime (%)")
    ax.set_title("Validation vs previous comm-share proxy")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(out_png, dpi=220, bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    proxy_path = Path(args.proxy_csv)
    proxy_df = pd.read_csv(proxy_path) if proxy_path.exists() else pd.DataFrame()

    workloads: List[str]
    if args.workloads:
        workloads = [w.strip() for w in args.workloads.split(",") if w.strip()]
    elif not proxy_df.empty and "workload" in proxy_df.columns:
        workloads = sorted(proxy_df["workload"].dropna().astype(str).unique().tolist())
    else:
        workloads = []

    if args.add_lulesh8 and "lulesh_8" not in workloads:
        workloads.append("lulesh_8")
    workloads = sorted(set(workloads))
    if not workloads:
        raise RuntimeError("No workloads to process.")

    rows = []
    local_root = Path(args.local_traces_root)
    for workload in workloads:
        m = compute_workload_metrics(local_root, args.remote_traces_root, workload)
        if m is not None:
            rows.append(m)

    trace_df = pd.DataFrame(rows).sort_values("workload")
    if trace_df.empty:
        raise RuntimeError("Failed to compute any trace-based workload metrics.")

    trace_csv = out_dir / "comm_share_per_workload_trace_based.csv"
    trace_df.to_csv(trace_csv, index=False)

    # Keep the previous proxy table (if present) and overwrite the primary table with corrected trace-based values.
    if proxy_path.exists():
        proxy_backup = out_dir / "comm_share_per_workload_proxy_model.csv"
        proxy_df.to_csv(proxy_backup, index=False)

    # Write corrected primary CSV for downstream use.
    corrected_cols = [
        "workload",
        "num_ranks",
        "critical_rank",
        "critical_runtime_us",
        "comm_pct_trace_critical",
        "compute_pct_trace_critical",
        "comm_pct_trace_mean",
        "compute_pct_trace_mean",
        "trace_method",
    ]
    trace_df[corrected_cols].to_csv(out_dir / "comm_share_per_workload.csv", index=False)

    val_df = trace_df.copy()
    if not proxy_df.empty and "workload" in proxy_df.columns:
        keep = [c for c in ["workload", "comm_pct_of_total_proxy", "compute_pct_of_total_proxy"] if c in proxy_df.columns]
        val_df = val_df.merge(proxy_df[keep], on="workload", how="left")
        if "comm_pct_of_total_proxy" in val_df.columns:
            val_df["abs_diff_comm_pct_points"] = (
                val_df["comm_pct_trace_critical"] - val_df["comm_pct_of_total_proxy"]
            ).abs()

    validation_csv = out_dir / "fig10_comm_pct_validation.csv"
    val_df.to_csv(validation_csv, index=False)

    plot_trace_stack(trace_df, out_dir / "comm_share_percentage_per_workload_trace_based.png")
    plot_proxy_vs_trace(val_df, out_dir / "fig10_comm_pct_validation.png")

    print(f"[INFO] Wrote: {trace_csv}")
    print(f"[INFO] Wrote: {validation_csv}")
    print(f"[INFO] Plots in: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
