#!/usr/bin/env python3
"""Plot PFLD probe-component and probe-frequency ablations."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns


FILE_RE = re.compile(r"(?P<algorithm>.+)_seed(?P<seed>\d+)_trim_(?P<trim>off|on)\.txt")
FINISH_RE = re.compile(r"\bfinished at\s+([0-9.]+)")
RTO_RE = re.compile(r"\brto_events\s+(\d+)")

COMPONENT_ORDER = (
    "pfld_no_probes",
    "pfld_tail_only",
    "pfld_tail_rtx",
    "pfld_probe1_no_rtx",
    "pfld_probe1",
)
COMPONENT_LABELS = ("None", "Tail", "Tail + RTX", "Tail + X=1", "All (X=1)")
FREQUENCY_ORDER = (
    "pfld_probe_rtt",
    "pfld_probe16",
    "pfld_probe8",
    "pfld_probe4",
    "pfld_probe1",
)
FREQUENCY_LABELS = ("RTT", "16", "8", "4", "1")
EXPECTED = set(COMPONENT_ORDER) | set(FREQUENCY_ORDER)
COLORS = {"off": "#0072B2", "on": "#D55E00"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--drop-rate", type=float, default=0.005)
    parser.add_argument("--workload", default="64-node 1-MiB all-to-all")
    return parser.parse_args()


def collect(root: Path, drop_rate: float) -> pd.DataFrame:
    records: list[dict[str, object]] = []
    drop_dir = root / f"drop_{drop_rate:.10g}"
    for path in sorted(drop_dir.glob("*.txt")):
        match = FILE_RE.fullmatch(path.name)
        if match is None or match.group("algorithm") not in EXPECTED:
            continue
        rto_events = 0
        finishes: list[float] = []
        with path.open(encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                if not line.startswith("Flow ") or " finished at " not in line:
                    continue
                finish = FINISH_RE.search(line)
                if finish is not None:
                    finishes.append(float(finish.group(1)))
                rto = RTO_RE.search(line)
                rto_events += int(rto.group(1)) if rto is not None else 0
        if finishes:
            records.append({
                "algorithm": match.group("algorithm"),
                "seed": int(match.group("seed")),
                "trim": match.group("trim"),
                "rto_events": rto_events,
                "completion_us": max(finishes),
                "completed_flows": len(finishes),
                "source_file": str(path.resolve()),
            })
    frame = pd.DataFrame(records)
    if frame.empty:
        raise RuntimeError(f"No ablation logs found under {drop_dir}")
    present = set(frame["algorithm"])
    if present != EXPECTED:
        raise RuntimeError(f"Incomplete algorithms: expected {EXPECTED}, found {present}")
    counts = frame.groupby(["algorithm", "trim"])["seed"].nunique()
    if counts.nunique() != 1:
        raise RuntimeError(f"Unmatched seed counts:\n{counts}")

    baseline = (
        frame[frame.algorithm == "pfld_no_probes"]
        .set_index(["seed", "trim"])["completion_us"]
    )
    frame["completion_vs_no_probes"] = [
        value / baseline.loc[(seed, trim)]
        for value, seed, trim in zip(frame.completion_us, frame.seed, frame.trim)
    ]
    return frame


def median_ci(values: np.ndarray, rng: np.random.Generator) -> tuple[float, float, float]:
    median = float(np.median(values))
    draws = np.empty(5000)
    for index in range(len(draws)):
        draws[index] = np.median(rng.choice(values, size=len(values), replace=True))
    return median, float(np.quantile(draws, 0.025)), float(np.quantile(draws, 0.975))


def summarize(frame: pd.DataFrame) -> pd.DataFrame:
    rng = np.random.default_rng(20260808)
    rows: list[dict[str, object]] = []
    for (algorithm, trim), group in frame.groupby(["algorithm", "trim"], sort=False):
        row: dict[str, object] = {
            "algorithm": algorithm,
            "trim": trim,
            "seeds": group.seed.nunique(),
            "completed_flows": int(group.completed_flows.min()),
        }
        for metric in ("rto_events", "completion_us", "completion_vs_no_probes"):
            median, low, high = median_ci(group[metric].to_numpy(float), rng)
            row[f"median_{metric}"] = median
            row[f"ci95_low_{metric}"] = low
            row[f"ci95_high_{metric}"] = high
        rows.append(row)
    return pd.DataFrame(rows)


def draw_panel(
    axis: plt.Axes,
    summary: pd.DataFrame,
    order: tuple[str, ...],
    labels: tuple[str, ...],
    metric: str,
    title: str,
) -> None:
    x = np.arange(len(order))
    for trim in ("off", "on"):
        rows = [
            summary[(summary.algorithm == algorithm) & (summary.trim == trim)].iloc[0]
            for algorithm in order
        ]
        values = np.array([float(row[f"median_{metric}"]) for row in rows])
        low = np.array([float(row[f"ci95_low_{metric}"]) for row in rows])
        high = np.array([float(row[f"ci95_high_{metric}"]) for row in rows])
        axis.errorbar(
            x, values, yerr=[values - low, high - values],
            color=COLORS[trim], marker="o", markersize=5.5, linewidth=1.8,
            capsize=3, label=f"Trimming {trim}", zorder=3,
        )
    axis.set_xticks(x, labels)
    axis.tick_params(axis="x", rotation=20)
    axis.set_title(title, loc="left", fontweight="bold")
    axis.grid(axis="x", visible=False)


def draw(summary: pd.DataFrame, args: argparse.Namespace) -> None:
    sns.set_theme(style="whitegrid", context="paper", font="serif")
    figure, axes = plt.subplots(2, 2, figsize=(10.8, 6.1), sharex="col")
    draw_panel(axes[0, 0], summary, COMPONENT_ORDER, COMPONENT_LABELS,
               "rto_events", "a) Probe components")
    draw_panel(axes[0, 1], summary, FREQUENCY_ORDER, FREQUENCY_LABELS,
               "rto_events", "b) Periodic-probe frequency")
    draw_panel(axes[1, 0], summary, COMPONENT_ORDER, COMPONENT_LABELS,
               "completion_vs_no_probes", "c) Component overhead / benefit")
    draw_panel(axes[1, 1], summary, FREQUENCY_ORDER, FREQUENCY_LABELS,
               "completion_vs_no_probes", "d) Frequency overhead / benefit")

    for axis in axes[0]:
        axis.set_yscale("symlog", linthresh=1, linscale=0.7)
        axis.set_ylim(bottom=0)
        axis.set_ylabel("RTO events per run")
    for axis in axes[1]:
        axis.axhline(1, color="#666666", linestyle="--", linewidth=1, zorder=1)
        axis.set_ylabel("Completion time / no-probe")
    axes[1, 0].set_xlabel("Probe components")
    axes[1, 1].set_xlabel("Packets per subflow probe (X)")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", ncols=2, frameon=True,
                  bbox_to_anchor=(0.5, 0.985))
    figure.suptitle(
        f"PFLD probe ablation — {args.workload}, corruption probability {args.drop_rate:g}, "
        f"{int(summary.seeds.min())} matched seeds\n"
        "Points: medians; whiskers: 95% bootstrap confidence intervals",
        y=1.055, fontsize=11,
    )
    figure.tight_layout(rect=(0, 0, 1, 0.93), pad=0.8)
    figure.savefig(args.output / "camera_ready_probe_ablation.png", dpi=240, bbox_inches="tight")
    figure.savefig(args.output / "camera_ready_probe_ablation.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frame = collect(args.run_root.resolve(), args.drop_rate)
    summary = summarize(frame)
    frame.to_csv(args.output / "camera_ready_probe_ablation_runs.csv", index=False)
    summary.to_csv(args.output / "camera_ready_probe_ablation_summary.csv", index=False)
    draw(summary, args)
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
