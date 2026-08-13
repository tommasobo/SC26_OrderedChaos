#!/usr/bin/env python3
"""Create final no-title Figure 13 heatmaps from aggregated measurements."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executed-drop-rate", type=float, required=True)
    parser.add_argument("--topology-note", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    frame = pd.read_csv(args.input)
    matrix = frame.pivot(index="subflows", columns="update_us", values="median_max_fct_us")
    sns.set_theme(style="whitegrid", context="talk", font="serif")
    figure, axis = plt.subplots(figsize=(5.5, 3.75))
    annotations = matrix.round(0).map(lambda value: f"{int(value):d}")
    sns.heatmap(
        matrix, annot=annotations, fmt="s", cmap="rocket_r", linewidths=0.5,
        linecolor="white", square=True, alpha=0.92,
        cbar_kws={"label": "Median max FCT (us), 3 seeds"}, ax=axis,
    )
    axis.set_xlabel("Update Period tau (RTT Ratio)")
    axis.set_ylabel("No. of Subflows")
    axis.set_xticklabels([1, 2, 4, 8])
    figure.tight_layout()
    figure.savefig(args.output / "figure13.png", dpi=240, bbox_inches="tight")
    figure.savefig(args.output / "figure13.pdf", bbox_inches="tight")
    plt.close(figure)
    provenance = {
        "input": str(args.input.resolve()),
        "executed_drop_rate": args.executed_drop_rate,
        "topology": args.topology_note,
        "seeds": [1, 25, 42],
    }
    (args.output / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
