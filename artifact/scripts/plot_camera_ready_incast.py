#!/usr/bin/env python3
"""Plot matched-seed RTO counts for Camera Ready New Image 2."""

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
ORDER = ("ecmp", "flowbender", "flowlet", "oblivious", "reps", "rss", "pfld", "rss_rack_tlp")
LABELS = {
    "ecmp": "ECMP", "flowbender": "PLB", "flowlet": "Flowlet",
    "oblivious": "RPS", "reps": "REPS", "rss": "RSS",
    "pfld": "RSS+PFLD", "rss_rack_tlp": "RSS+TLP-RACK",
}
COLORS = {"off": "#66c2a5", "on": "#fc8d62"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--drop-rate", type=float, default=0.0005)
    parser.add_argument("--workload", default="32-to-1 1-MiB incast")
    parser.add_argument("--expected-flows", type=int, default=0)
    return parser.parse_args()


def compact_count(value: float) -> str:
    if abs(value) >= 1000:
        return f"{value / 1000:.1f}k"
    if value.is_integer():
        return str(int(value))
    return f"{value:g}"


def collect(root: Path, drop_rate: float) -> pd.DataFrame:
    records: list[dict[str, object]] = []
    for path in sorted((root / f"drop_{drop_rate:.10g}").glob("*.txt")):
        match = FILE_RE.fullmatch(path.name)
        if match is None or match.group("algorithm") not in ORDER:
            continue
        total = 0
        completed = 0
        completion_us = 0.0
        with path.open(encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                if not line.startswith("Flow ") or " finished at " not in line:
                    continue
                completed += 1
                found = RTO_RE.search(line)
                total += int(found.group(1)) if found else 0
                finish = FINISH_RE.search(line)
                if finish is not None:
                    completion_us = max(completion_us, float(finish.group(1)))
        if completed:
            records.append({
                "algorithm": match.group("algorithm"),
                "seed": int(match.group("seed")),
                "trim": match.group("trim"),
                "rto_events": total,
                "completion_us": completion_us,
                "completed_flows": completed,
                "source_file": str(path.resolve()),
            })
    frame = pd.DataFrame(records)
    expected = {(algorithm, trim) for algorithm in ORDER for trim in ("off", "on")}
    present = set(zip(frame["algorithm"], frame["trim"])) if not frame.empty else set()
    if present != expected:
        raise RuntimeError(f"Incomplete camera-ready data: expected {expected}, found {present}")
    return frame


def bootstrap_ci(values: np.ndarray, rng: np.random.Generator) -> tuple[float, float]:
    draws = np.empty(10000)
    for index in range(len(draws)):
        draws[index] = np.median(rng.choice(values, size=len(values), replace=True))
    return float(np.quantile(draws, 0.025)), float(np.quantile(draws, 0.975))


def summarize(frame: pd.DataFrame) -> pd.DataFrame:
    rng = np.random.default_rng(20260802)
    records: list[dict[str, object]] = []
    for (algorithm, trim), rows in frame.groupby(["algorithm", "trim"], sort=False):
        values = rows["rto_events"].to_numpy(dtype=float)
        low, high = bootstrap_ci(values, rng)
        mean = float(np.mean(values))
        sem = float(np.std(values, ddof=1) / np.sqrt(len(values))) if len(values) > 1 else 0.0
        completion_values = rows["completion_us"].to_numpy(dtype=float)
        completion_low, completion_high = bootstrap_ci(completion_values, rng)
        records.append({
            "algorithm": algorithm,
            "trim": trim,
            "seeds": rows["seed"].nunique(),
            "median_rto_events": float(np.median(values)),
            "mean_rto_events": mean,
            "std_rto_events": float(np.std(values, ddof=1)) if len(values) > 1 else 0.0,
            "sem_rto_events": sem,
            "normal95_low_rto_events": max(0.0, mean - 1.96 * sem),
            "normal95_high_rto_events": mean + 1.96 * sem,
            "ci95_low": low,
            "ci95_high": high,
            "median_completion_us": float(np.median(completion_values)),
            "mean_completion_us": float(np.mean(completion_values)),
            "ci95_low_completion_us": completion_low,
            "ci95_high_completion_us": completion_high,
        })
    return pd.DataFrame(records)


def draw(frame: pd.DataFrame, summary: pd.DataFrame, args: argparse.Namespace, scale: str) -> None:
    sns.set_theme(style="whitegrid", context="paper")
    figure, axis = plt.subplots(figsize=(12.4, 3.75))
    if scale == "log":
        # A true log axis cannot represent the zero-RTO result that this
        # experiment is designed to expose. Symlog preserves exact zero and
        # is logarithmic above one event.
        axis.set_yscale("symlog", linthresh=1, linscale=0.7)
    centers = np.arange(len(ORDER), dtype=float)
    width = 0.36
    global_high = 0.0
    for trim_index, trim in enumerate(("off", "on")):
        offset = (-0.5 if trim_index == 0 else 0.5) * width
        rows = [summary[(summary.algorithm == algorithm) & (summary.trim == trim)].iloc[0]
                for algorithm in ORDER]
        values = np.array([float(row.mean_rto_events) for row in rows])
        lows = np.array([float(row.normal95_low_rto_events) for row in rows])
        highs = np.array([float(row.normal95_high_rto_events) for row in rows])
        global_high = max(global_high, float(np.max(highs)))
        positions = centers + offset
        axis.bar(positions, values, width=width, label=f"Trimming {trim}",
                 color=COLORS[trim], edgecolor="black", linewidth=0.8, zorder=2)
        axis.errorbar(positions, values, yerr=[values - lows, highs - values],
                      fmt="none", ecolor="black", capsize=3, zorder=4)
        for index, algorithm in enumerate(ORDER):
            label_y = max(highs[index], values[index]) * 1.12
            axis.text(positions[index], label_y,
                      compact_count(values[index]), ha="center", va="bottom",
                      fontsize=8, fontweight="bold")
    suffix = "" if scale == "log" else "_linear"
    if scale == "log":
        axis.set_ylabel("RTO events per run (symlog; linear through 1)")
    else:
        axis.set_ylim(0, global_high * 1.20)
        axis.set_ylabel("RTO events per run")
    axis.set_xticks(centers, [LABELS[name] for name in ORDER])
    axis.tick_params(axis="x", rotation=18)
    axis.set_title(
        f"{args.workload}  |  corruption probability {args.drop_rate:g}  |  "
        f"{int(summary.seeds.min())} matched seeds\n"
        "Bars and labels: mean RTO count; whiskers: normal 95% CI (mean +/- 1.96 SEM)",
        fontsize=11,
    )
    axis.legend(loc="lower left", frameon=True, ncols=2)
    axis.grid(axis="x", visible=False)
    figure.tight_layout(pad=0.7)
    figure.savefig(args.output / f"camera_ready_new_image2{suffix}.png", dpi=240, bbox_inches="tight")
    figure.savefig(args.output / f"camera_ready_new_image2{suffix}.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frame = collect(args.run_root.resolve(), args.drop_rate)
    if args.expected_flows and not (frame.completed_flows == args.expected_flows).all():
        bad = frame.loc[frame.completed_flows != args.expected_flows,
                        ["algorithm", "seed", "trim", "completed_flows"]]
        raise RuntimeError(f"Incomplete simulator runs; expected {args.expected_flows} flows:\n{bad}")
    summary = summarize(frame)
    frame.to_csv(args.output / "camera_ready_new_image2_runs.csv", index=False)
    summary.to_csv(args.output / "camera_ready_new_image2_summary.csv", index=False)
    draw(frame, summary, args, "log")
    draw(frame, summary, args, "linear")
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
