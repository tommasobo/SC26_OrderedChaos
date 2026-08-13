#!/usr/bin/env python3
"""Create Figure 11 from the paper configuration."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns


REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nsdi_plotting import collect_data


ORDER = ("ecmp", "flowbender", "flowlet", "oblivious", "reps", "rss", "rss1", "rss_rack_tlp")
LABELS = {
    "ecmp": "ECMP",
    "flowbender": "PLB",
    "flowlet": "Flowlet",
    "oblivious": "RPS",
    "reps": "REPS",
    "rss": "RSS",
    "rss1": "RSS+\nPFLD",
    "rss_rack_tlp": "RSS+\nTLP-RACK",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--rack-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compute-fraction", type=float, default=0.54)
    return parser.parse_args()


def read_rows(root: Path, algorithms: tuple[str, ...]) -> pd.DataFrame:
    frame = collect_data(
        root / "figure11" / "paper_configuration",
        min_flows=1,
        only_max=True,
    )
    return frame[frame["algorithm"].isin(algorithms)].copy()


def prepare_frame(source: pd.DataFrame, rack: pd.DataFrame, compute_fraction: float) -> pd.DataFrame:
    frame = pd.concat([source, rack], ignore_index=True)
    frame = frame[frame["algorithm"].isin(ORDER)].copy()
    baseline_rows = frame[
        (frame["algorithm"] == "ecmp") & (frame["trim_status"] == "off")
    ]
    if baseline_rows.empty:
        raise RuntimeError("Figure 11 ECMP baseline is missing")
    baseline = float(baseline_rows["completion_time"].median())
    subtraction = compute_fraction * baseline
    frame["raw_iteration_time_us"] = frame["completion_time"]
    frame["compute_subtraction_us"] = subtraction
    frame["exposed_communication_ms"] = (
        (frame["completion_time"] - subtraction).clip(lower=0) / 1000.0
    )
    exposed_baseline = float(
        frame[
            (frame["algorithm"] == "ecmp") & (frame["trim_status"] == "off")
        ]["exposed_communication_ms"].median()
    )
    frame["reduction_vs_ecmp_off_percent"] = 100.0 * (
        1.0 - frame["exposed_communication_ms"] / exposed_baseline
    )
    return frame


def plot(frame: pd.DataFrame, output: Path) -> None:
    work = frame.copy()
    work["trim"] = work["trim_status"].map({"off": "Off", "on": "On"})
    sns.set_theme(style="whitegrid", context="paper", font="serif")
    figure, axis = plt.subplots(figsize=(7.0, 2.5))
    sns.barplot(
        data=work,
        x="algorithm",
        y="exposed_communication_ms",
        hue="trim",
        order=ORDER,
        hue_order=("Off", "On"),
        palette="Set2",
        estimator=np.median,
        errorbar=None,
        edgecolor="black",
        linewidth=0.8,
        ax=axis,
    )
    axis.set_xlabel("")
    axis.set_ylabel("Training Comm. Only Time (ms)")
    axis.set_xticks(range(len(ORDER)))
    axis.set_xticklabels([LABELS[item] for item in ORDER])
    axis.legend(title="Trim", loc="lower right")
    axis.set_ylim(bottom=0)
    figure.tight_layout()
    figure.savefig(output / "figure11.png", dpi=240, bbox_inches="tight")
    figure.savefig(output / "figure11.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    if args.output.exists():
        raise SystemExit(f"Refusing existing output: {args.output}")
    args.output.mkdir(parents=True)
    source_algorithms = tuple(item for item in ORDER if item != "rss_rack_tlp")
    source = read_rows(args.source_root, source_algorithms)
    rack = read_rows(args.rack_root, ("rss_rack_tlp",))
    frame = prepare_frame(source, rack, args.compute_fraction)
    frame.to_csv(args.output / "figure11_metrics.csv", index=False)
    plot(frame, args.output)

    medians = (
        frame.groupby(["algorithm", "trim_status"], as_index=False)
        .median(numeric_only=True)
    )
    finding = {
        "algorithms": sorted(frame["algorithm"].unique()),
        "compute_fraction": args.compute_fraction,
        "median_exposed_communication_ms": {
            f"{row.algorithm}:{row.trim_status}": row.exposed_communication_ms
            for row in medians.itertuples()
        },
    }
    (args.output / "claim_summary.json").write_text(
        json.dumps(finding, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(finding, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
