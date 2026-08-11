#!/usr/bin/env python3
"""Plot cumulative PFLD probe benefits and the final frequency tradeoff."""

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

STACK_ORDER = (
    "pfld_no_probes",
    "pfld_tail_only",
    "pfld_probe16_no_rtx",
    "pfld_probe1_no_rtx",
    "pfld_probe1",
)
STACK_LABELS = (
    "PSN gaps only",
    "+ section/tail probes",
    "+ periodic probes (X=16)",
    "+ denser periodic probes (X=1)",
    "+ RTX probes",
)
FREQUENCY_ORDER = (
    "pfld_probe_rtt_no_rtx",
    "pfld_probe16_no_rtx",
    "pfld_probe8_no_rtx",
    "pfld_probe4_no_rtx",
    "pfld_probe1_no_rtx",
)
FREQUENCY_LABELS = ("RTT", "16", "8", "4", "1")
EXPECTED = set(STACK_ORDER) | set(FREQUENCY_ORDER)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--drop-rate", type=float, default=0.005)
    parser.add_argument("--workload", default="128-node 8-MiB tornado")
    parser.add_argument("--expected-flows", type=int, default=128)
    return parser.parse_args()


def collect(root: Path, drop_rate: float) -> pd.DataFrame:
    records: list[dict[str, object]] = []
    drop_dir = root / f"drop_{drop_rate:.10g}"
    for path in sorted(drop_dir.glob("*.txt")):
        match = FILE_RE.fullmatch(path.name)
        if match is None or match.group("algorithm") not in EXPECTED:
            continue
        if match.group("trim") != "off":
            raise RuntimeError(f"Compounding ablation must be trim-off only: {path}")
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
        raise RuntimeError(f"No compounding logs found under {drop_dir}")
    if set(frame.algorithm) != EXPECTED:
        raise RuntimeError(f"Incomplete algorithms: expected {EXPECTED}, found {set(frame.algorithm)}")
    counts = frame.groupby("algorithm").seed.nunique()
    if counts.nunique() != 1:
        raise RuntimeError(f"Unmatched seed counts:\n{counts}")
    return frame


def normal_summary(values: np.ndarray) -> tuple[float, float, float, float, float]:
    mean = float(np.mean(values))
    std = float(np.std(values, ddof=1)) if len(values) > 1 else 0.0
    sem = std / np.sqrt(len(values))
    return mean, std, sem, max(0.0, mean - 1.96 * sem), mean + 1.96 * sem


def summarize(frame: pd.DataFrame) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    for algorithm, group in frame.groupby("algorithm", sort=False):
        row: dict[str, object] = {
            "algorithm": algorithm,
            "seeds": group.seed.nunique(),
            "completed_flows": int(group.completed_flows.min()),
        }
        for metric in ("rto_events", "completion_us"):
            mean, std, sem, low, high = normal_summary(group[metric].to_numpy(float))
            row[f"mean_{metric}"] = mean
            row[f"std_{metric}"] = std
            row[f"sem_{metric}"] = sem
            row[f"normal95_low_{metric}"] = low
            row[f"normal95_high_{metric}"] = high
        rows.append(row)
    return pd.DataFrame(rows)


def compact(value: float) -> str:
    if abs(value) >= 1000:
        return f"{value / 1000:.1f}k"
    if abs(value) >= 100:
        return f"{value:.0f}"
    return f"{value:.1f}".rstrip("0").rstrip(".")


def draw(summary: pd.DataFrame, args: argparse.Namespace) -> None:
    sns.set_theme(style="whitegrid", context="paper")
    figure, (stack_axis, frequency_axis) = plt.subplots(
        1, 2, figsize=(12.2, 4.55), gridspec_kw={"width_ratios": (1.28, 1)}
    )

    stack = summary.set_index("algorithm").loc[list(STACK_ORDER)]
    y = np.arange(len(STACK_ORDER))
    values = stack.mean_rto_events.to_numpy(float)
    low = stack.normal95_low_rto_events.to_numpy(float)
    high = stack.normal95_high_rto_events.to_numpy(float)
    colors = sns.color_palette("crest", len(STACK_ORDER))
    stack_axis.barh(y, values, color=colors, edgecolor="black", linewidth=0.7, height=0.66)
    stack_axis.errorbar(values, y, xerr=[values - low, high - values], fmt="none",
                        ecolor="black", capsize=3, linewidth=1.1, zorder=4)
    stack_axis.set_xlim(0, max(high) * 1.30)
    stack_axis.set_yticks(y, STACK_LABELS)
    stack_axis.invert_yaxis()
    stack_axis.set_xlabel("Mean RTO events per run")
    stack_axis.set_title("a) Building the recovery stack", loc="left", fontweight="bold")
    stack_axis.grid(axis="y", visible=False)
    previous = None
    for position, value in enumerate(values):
        suffix = ""
        if previous is not None and previous > 0:
            reduction = 100 * (previous - value) / previous
            suffix = (f"   ({reduction:.0f}% fewer vs prior step)"
                      if reduction >= 0.5 else "   (no material change)")
        stack_axis.text(value + max(high) * 0.018, position,
                        f"{compact(value)}{suffix}", va="center", fontsize=8.2,
                        fontweight="bold" if position == len(values) - 1 else "normal")
        previous = value

    frequency = summary.set_index("algorithm").loc[list(FREQUENCY_ORDER)]
    completion = frequency.mean_completion_us.to_numpy(float)
    y_rto = frequency.mean_rto_events.to_numpy(float)
    completion_low = frequency.normal95_low_completion_us.to_numpy(float)
    completion_high = frequency.normal95_high_completion_us.to_numpy(float)
    ylow = frequency.normal95_low_rto_events.to_numpy(float)
    yhigh = frequency.normal95_high_rto_events.to_numpy(float)
    positions = np.arange(len(FREQUENCY_ORDER))
    palette = sns.color_palette("flare", len(FREQUENCY_ORDER))
    frequency_axis.bar(positions, y_rto, color=palette, edgecolor="black", linewidth=0.7)
    frequency_axis.errorbar(positions, y_rto, yerr=[y_rto - ylow, yhigh - y_rto],
                            fmt="none", ecolor="black", capsize=3, linewidth=1.1, zorder=4)
    frequency_axis.set_xticks(positions, [f"X={label}" for label in FREQUENCY_LABELS])
    frequency_axis.set_ylim(0, max(yhigh) * 1.24)
    frequency_axis.set_ylabel("Mean RTO events per run")
    frequency_axis.set_title("b) Periodic frequency (RTX probes held off)",
                             loc="left", fontweight="bold")
    frequency_axis.grid(axis="x", visible=False)
    for position, value in zip(positions, y_rto):
        frequency_axis.text(position, value + max(yhigh) * 0.035, compact(value),
                            ha="center", va="bottom", fontsize=8.2, fontweight="bold")

    completion_axis = frequency_axis.twinx()
    completion_axis.errorbar(
        positions, completion,
        yerr=[completion - completion_low, completion_high - completion],
        fmt="D-", markersize=4.8, color="#2f6f9f", ecolor="#2f6f9f",
        capsize=3, linewidth=1.2, label="Maximum FCT",
    )
    margin = max(2.0, (max(completion_high) - min(completion_low)) * 0.30)
    completion_axis.set_ylim(min(completion_low) - margin, max(completion_high) + margin)
    completion_axis.set_ylabel("Mean maximum FCT (us)", color="#2f6f9f")
    completion_axis.tick_params(axis="y", colors="#2f6f9f")

    figure.suptitle(
        f"PFLD probe compounding — {args.workload}, trimming off, "
        f"corruption probability {args.drop_rate:g}, {int(summary.seeds.min())} matched seeds\n"
        "Bars/points: means; whiskers: normal 95% CI (mean +/- 1.96 SEM)",
        y=1.03, fontsize=11,
    )
    figure.tight_layout(pad=0.8)
    args.output.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output / "camera_ready_probe_compounding.png", dpi=240, bbox_inches="tight")
    figure.savefig(args.output / "camera_ready_probe_compounding.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    frame = collect(args.run_root.resolve(), args.drop_rate)
    if args.expected_flows and not (frame.completed_flows == args.expected_flows).all():
        bad = frame.loc[frame.completed_flows != args.expected_flows,
                        ["algorithm", "seed", "trim", "completed_flows"]]
        raise RuntimeError(f"Incomplete simulator runs; expected {args.expected_flows} flows:\n{bad}")
    summary = summarize(frame)
    args.output.mkdir(parents=True, exist_ok=True)
    frame.to_csv(args.output / "camera_ready_probe_compounding_runs.csv", index=False)
    summary.to_csv(args.output / "camera_ready_probe_compounding_summary.csv", index=False)
    draw(summary, args)
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
