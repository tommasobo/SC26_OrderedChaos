#!/usr/bin/env python3
"""Render the camera-ready two-panel PFLD probe ablation."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns


STACK_ORDER = (
    "pfld_no_probes",
    "pfld_tail_only",
    "pfld_probe16_no_rtx",
    "pfld_probe1_no_rtx",
    "pfld_probe1",
)
STACK_LABELS = (
    "PSN gaps only",
    "+ Section/tail probes",
    "+ Periodic probes (X=16)",
    "+ Denser probes (X=1)",
    "+ RTX probes",
)
STRESS_ORDER = (
    "pfld_tail_only",
    "pfld_probe16_no_rtx",
    "pfld_probe8_no_rtx",
    "pfld_probe4_no_rtx",
    "pfld_probe1_no_rtx",
    "pfld_probe_rtt_no_rtx",
)
STRESS_LABELS = (
    "Section/tail only",
    "Periodic X=16",
    "Periodic X=8",
    "Periodic X=4",
    "Periodic X=1",
    "Periodic once/RTT",
)

# The main camera-ready plots use seaborn's Set2 trim colors. Reuse those
# anchors and related teal shades so the ablation reads as part of one suite.
TEAL = "#66c2a5"
TEAL_LIGHT = "#a7d9c9"
TEAL_MID = "#45a99c"
TEAL_DARK = "#277f78"
ORANGE = "#fc8d62"
GRAY = "#a6a6a6"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compounding-summary", type=Path, required=True)
    parser.add_argument("--stress-summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--drop-rate", type=float, default=0.005)
    return parser.parse_args()


def ordered_summary(path: Path, order: tuple[str, ...], name: str) -> pd.DataFrame:
    frame = pd.read_csv(path)
    required = {
        "algorithm",
        "seeds",
        "completed_flows",
        "mean_rto_events",
        "normal95_low_rto_events",
        "normal95_high_rto_events",
    }
    missing = required - set(frame.columns)
    if missing:
        raise RuntimeError(f"{name} summary lacks columns: {sorted(missing)}")
    if frame.algorithm.duplicated().any():
        raise RuntimeError(f"{name} summary contains duplicate algorithms")
    absent = set(order) - set(frame.algorithm)
    if absent:
        raise RuntimeError(f"{name} summary lacks algorithms: {sorted(absent)}")
    ordered = frame.set_index("algorithm").loc[list(order)].copy()
    if ordered.seeds.nunique() != 1 or int(ordered.seeds.min()) < 2:
        raise RuntimeError(f"{name} summary does not use one matched multi-seed sample")
    if (ordered.completed_flows <= 0).any():
        raise RuntimeError(f"{name} summary contains incomplete flow counts")
    if (ordered.normal95_low_rto_events > ordered.mean_rto_events).any():
        raise RuntimeError(f"{name} summary contains an invalid lower confidence bound")
    if (ordered.normal95_high_rto_events < ordered.mean_rto_events).any():
        raise RuntimeError(f"{name} summary contains an invalid upper confidence bound")
    return ordered


def compact(value: float) -> str:
    if value >= 1000:
        return f"{value / 1000:.2f}k"
    if value >= 100:
        return f"{value:.1f}"
    return f"{value:.1f}".rstrip("0").rstrip(".")


def signed_reduction(reference: float, value: float) -> float:
    return 100.0 * (reference - value) / reference if reference else 0.0


def draw_stack(axis: plt.Axes, summary: pd.DataFrame) -> None:
    means = summary.mean_rto_events.to_numpy(float)
    low = summary.normal95_low_rto_events.to_numpy(float)
    high = summary.normal95_high_rto_events.to_numpy(float)
    positions = np.arange(len(STACK_ORDER))
    colors = (GRAY, TEAL_LIGHT, TEAL, TEAL_DARK, ORANGE)

    axis.barh(
        positions,
        means,
        height=0.62,
        color=colors,
        edgecolor="black",
        linewidth=0.75,
        zorder=2,
    )
    axis.errorbar(
        means,
        positions,
        xerr=[means - low, high - means],
        fmt="none",
        ecolor="black",
        elinewidth=1.0,
        capsize=3,
        zorder=4,
    )
    # A zero-height bar is otherwise invisible; keep the successful final
    # stage explicit without inventing a nonzero plotting value.
    axis.scatter([0], [positions[-1]], marker="D", s=28, color=ORANGE,
                 edgecolor="black", linewidth=0.65, zorder=5, clip_on=False)

    limit = float(high.max()) * 1.30
    axis.set_xlim(0, limit)
    axis.set_yticks(positions, STACK_LABELS)
    axis.invert_yaxis()
    axis.set_xlabel("Mean RTO events per run")
    axis.grid(axis="y", visible=False)
    axis.set_title("a  Recovery mechanisms compound", loc="left",
                   fontsize=11, fontweight="bold", pad=25)
    axis.text(0.0, 1.015, "128-node tornado · 8 MiB per flow",
              transform=axis.transAxes, ha="left", va="bottom", fontsize=9)

    for index, mean in enumerate(means):
        if index == 0:
            label = compact(mean)
        else:
            reduction = signed_reduction(means[index - 1], mean)
            change = "≈0%" if abs(reduction) < 0.5 else f"−{reduction:.0f}%"
            label = f"{compact(mean)}  ({change})"
        x = max(mean, 0.0) + limit * 0.018
        axis.text(x, index, label, ha="left", va="center", fontsize=8.5,
                  fontweight="bold" if index == len(means) - 1 else "normal")


def draw_stress(axis: plt.Axes, summary: pd.DataFrame) -> None:
    means = summary.mean_rto_events.to_numpy(float)
    low = summary.normal95_low_rto_events.to_numpy(float)
    high = summary.normal95_high_rto_events.to_numpy(float)
    positions = np.arange(len(STRESS_ORDER))
    colors = (GRAY, TEAL_LIGHT, TEAL, TEAL_MID, TEAL_DARK, ORANGE)
    origin = 20.0

    for position, mean, color in zip(positions, means, colors):
        axis.hlines(position, origin, mean, color=color, linewidth=5.0,
                    alpha=0.88, zorder=2)
    axis.errorbar(
        means,
        positions,
        xerr=[means - low, high - means],
        fmt="none",
        ecolor="black",
        elinewidth=1.0,
        capsize=3,
        zorder=4,
    )
    axis.scatter(means, positions, s=66, c=colors, edgecolor="black",
                 linewidth=0.75, zorder=5)
    axis.set_xscale("log")
    axis.set_xlim(origin, float(high.max()) * 2.0)
    axis.set_yticks(positions, STRESS_LABELS)
    axis.invert_yaxis()
    axis.set_xlabel("Mean RTO events per run (log scale)")
    axis.grid(axis="y", visible=False)
    axis.set_title("b  Periodic probes matter under contention", loc="left",
                   fontsize=11, fontweight="bold", pad=25)
    axis.text(0.0, 1.015, "32-to-1 incast · 4 MiB per sender",
              transform=axis.transAxes, ha="left", va="bottom", fontsize=9)

    baseline = means[0]
    for index, mean in enumerate(means):
        if index == 0:
            label = compact(mean)
        else:
            reduction = signed_reduction(baseline, mean)
            change = "≈ baseline" if abs(reduction) < 0.5 else f"−{reduction:.0f}%"
            label = f"{compact(mean)}  ({change})"
        axis.text(mean * 1.10, index, label, ha="left", va="center", fontsize=8.5)


def draw(compounding: pd.DataFrame, stress: pd.DataFrame,
         args: argparse.Namespace) -> None:
    if int(compounding.seeds.min()) != int(stress.seeds.min()):
        raise RuntimeError("The two panels do not use the same number of matched seeds")
    seeds = int(compounding.seeds.min())

    sns.set_theme(
        style="whitegrid",
        context="paper",
        rc={
            "axes.edgecolor": "#b8b8b8",
            "axes.linewidth": 0.8,
            "grid.color": "#d2d2d2",
            "grid.linewidth": 0.8,
            "font.family": "sans-serif",
        },
    )
    figure, (stack_axis, stress_axis) = plt.subplots(
        1,
        2,
        figsize=(12.4, 4.35),
        gridspec_kw={"width_ratios": (1.02, 1.0), "wspace": 0.34},
    )
    draw_stack(stack_axis, compounding)
    draw_stress(stress_axis, stress)

    figure.suptitle(
        "PFLD probe ablation",
        x=0.5,
        y=0.995,
        fontsize=13,
        fontweight="bold",
    )
    figure.text(
        0.5,
        0.945,
        f"Trimming off · corruption probability {args.drop_rate:g} · "
        f"{seeds} matched seeds",
        ha="center",
        va="center",
        fontsize=9.5,
    )
    figure.text(
        0.5,
        0.015,
        "Whiskers: normal 95% CI (mean ± 1.96 SEM). "
        "Percentages compare with the previous step (a) or section/tail-only (b).",
        ha="center",
        va="bottom",
        fontsize=7.8,
        color="#4f4f4f",
    )
    figure.subplots_adjust(left=0.16, right=0.985, bottom=0.17, top=0.82)

    args.output.mkdir(parents=True, exist_ok=True)
    stem = args.output / "camera_ready_probe_ablation"
    figure.savefig(stem.with_suffix(".png"), dpi=240, bbox_inches="tight")
    figure.savefig(stem.with_suffix(".pdf"), bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    compounding = ordered_summary(
        args.compounding_summary.resolve(), STACK_ORDER, "compounding"
    )
    stress = ordered_summary(args.stress_summary.resolve(), STRESS_ORDER, "stress")
    draw(compounding, stress, args)
    print(f"Wrote {args.output.resolve() / 'camera_ready_probe_ablation.png'}")
    print(f"Wrote {args.output.resolve() / 'camera_ready_probe_ablation.pdf'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
