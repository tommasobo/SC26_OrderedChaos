#!/usr/bin/env python3
"""Plot the benefit of periodic PFLD probes in a disclosed incast stress case."""

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
RTO_RE = re.compile(r"\brto_events\s+(\d+)")
FINISH_RE = re.compile(r"\bfinished at\s+([0-9.]+)")
ORDER = (
    "pfld_tail_only",
    "pfld_probe16_no_rtx",
    "pfld_probe8_no_rtx",
    "pfld_probe4_no_rtx",
    "pfld_probe1_no_rtx",
    "pfld_probe_rtt_no_rtx",
)
LABELS = (
    "Section/tail only",
    "Periodic X=16",
    "Periodic X=8",
    "Periodic X=4",
    "Periodic X=1",
    "Periodic once/RTT",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--drop-rate", type=float, default=0.005)
    parser.add_argument("--expected-flows", type=int, default=32)
    parser.add_argument("--workload", default="32-to-1 incast, 4 MiB per sender")
    return parser.parse_args()


def collect(root: Path, drop_rate: float) -> pd.DataFrame:
    records: list[dict[str, object]] = []
    for path in sorted((root / f"drop_{drop_rate:.10g}").glob("*.txt")):
        match = FILE_RE.fullmatch(path.name)
        if match is None or match.group("algorithm") not in ORDER:
            continue
        if match.group("trim") != "off":
            raise RuntimeError(f"The periodic stress figure must be trim-off only: {path}")
        rto_events = 0
        finishes: list[float] = []
        with path.open(encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                if not line.startswith("Flow ") or " finished at " not in line:
                    continue
                found_rto = RTO_RE.search(line)
                rto_events += int(found_rto.group(1)) if found_rto is not None else 0
                found_finish = FINISH_RE.search(line)
                if found_finish is not None:
                    finishes.append(float(found_finish.group(1)))
        if finishes:
            records.append({
                "algorithm": match.group("algorithm"),
                "seed": int(match.group("seed")),
                "rto_events": rto_events,
                "maximum_fct_us": max(finishes),
                "completed_flows": len(finishes),
                "source_file": str(path.resolve()),
            })
    frame = pd.DataFrame(records)
    if frame.empty or set(frame.algorithm) != set(ORDER):
        raise RuntimeError(f"Incomplete stress data; found {set(frame.algorithm) if not frame.empty else set()}")
    counts = frame.groupby("algorithm").seed.nunique()
    if counts.nunique() != 1:
        raise RuntimeError(f"Unmatched seeds:\n{counts}")
    return frame


def summarize(frame: pd.DataFrame) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    baseline = float(frame.loc[frame.algorithm == ORDER[0], "rto_events"].mean())
    for algorithm in ORDER:
        group = frame[frame.algorithm == algorithm]
        values = group.rto_events.to_numpy(float)
        mean = float(values.mean())
        std = float(values.std(ddof=1)) if len(values) > 1 else 0.0
        sem = std / np.sqrt(len(values))
        rows.append({
            "algorithm": algorithm,
            "seeds": group.seed.nunique(),
            "completed_flows": int(group.completed_flows.min()),
            "mean_rto_events": mean,
            "std_rto_events": std,
            "sem_rto_events": sem,
            "normal95_low_rto_events": max(0.0, mean - 1.96 * sem),
            "normal95_high_rto_events": mean + 1.96 * sem,
            "reduction_vs_section_tail_percent": 100.0 * (baseline - mean) / baseline,
            "mean_maximum_fct_us": float(group.maximum_fct_us.mean()),
        })
    return pd.DataFrame(rows)


def compact(value: float) -> str:
    return f"{value / 1000:.2f}k" if value >= 1000 else f"{value:.1f}"


def draw(summary: pd.DataFrame, args: argparse.Namespace) -> None:
    ordered = summary.set_index("algorithm").loc[list(ORDER)]
    means = ordered.mean_rto_events.to_numpy(float)
    low = ordered.normal95_low_rto_events.to_numpy(float)
    high = ordered.normal95_high_rto_events.to_numpy(float)
    reductions = ordered.reduction_vs_section_tail_percent.to_numpy(float)
    positions = np.arange(len(ORDER))

    sns.set_theme(style="whitegrid", context="paper", font="serif")
    figure, axis = plt.subplots(figsize=(7.9, 4.25))
    colors = ["#8c8c8c", *sns.color_palette("crest", 4), "#d95f02"]
    for y, mean, color in zip(positions, means, colors):
        axis.hlines(y, 10, mean, color=color, linewidth=3.0, alpha=0.75)
    axis.errorbar(means, positions, xerr=[means - low, high - means], fmt="none",
                  ecolor="black", capsize=3, linewidth=1.1, zorder=3)
    axis.scatter(means, positions, s=74, c=colors, edgecolor="black", linewidth=0.75, zorder=4)
    axis.set_xscale("log")
    axis.set_xlim(10, max(high) * 1.85)
    axis.set_yticks(positions, LABELS)
    axis.invert_yaxis()
    axis.set_xlabel("Mean RTO events per run (log scale)")
    axis.set_title(
        f"Periodic probes under bursty receiver contention\n"
        f"{args.workload}; trimming off; corruption {args.drop_rate:g}; "
        f"{int(ordered.seeds.min())} matched seeds",
        loc="left", fontsize=10.5,
    )
    axis.grid(axis="y", visible=False)
    for index, (mean, reduction) in enumerate(zip(means, reductions)):
        suffix = "" if index == 0 else f"  ({reduction:.1f}% fewer)"
        axis.text(mean * 1.11, index, f"{compact(mean)}{suffix}", va="center", fontsize=8.4)
    figure.tight_layout(pad=0.8)
    args.output.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output / "camera_ready_periodic_probe_stress.png", dpi=240,
                   bbox_inches="tight")
    figure.savefig(args.output / "camera_ready_periodic_probe_stress.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    frame = collect(args.run_root.resolve(), args.drop_rate)
    if args.expected_flows and not (frame.completed_flows == args.expected_flows).all():
        bad = frame.loc[frame.completed_flows != args.expected_flows,
                        ["algorithm", "seed", "completed_flows"]]
        raise RuntimeError(f"Expected {args.expected_flows} completed flows:\n{bad}")
    summary = summarize(frame)
    args.output.mkdir(parents=True, exist_ok=True)
    frame.to_csv(args.output / "camera_ready_periodic_probe_stress_runs.csv", index=False)
    summary.to_csv(args.output / "camera_ready_periodic_probe_stress_summary.csv", index=False)
    draw(summary, args)
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
