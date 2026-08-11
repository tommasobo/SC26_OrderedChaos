#!/usr/bin/env python3
"""Create the two Camera Ready New Images from flow and timeout measurements."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

import sys

REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from nsdi_plotting_dc import collect_data


ORDER = ("ecmp", "flowbender", "flowlet", "oblivious", "reps", "rss", "pfld", "rss_rack_tlp")
LABELS = {
    "ecmp": "ECMP", "flowbender": "PLB", "flowlet": "Flowlet", "oblivious": "RPS",
    "reps": "REPS", "rss": "RSS", "pfld": "RSS+PFLD", "rss_rack_tlp": "RSS+TLP-RACK",
}
OLD_FIGURE10_WIDTH = 15.0
OLD_FIGURE10_HEIGHT = 2.75
OLD_FIGURE9_WIDTH = 15.0
OLD_FIGURE9_HEIGHT = 5.5
SIZE_BINS = (("small", "0-10 KB"), ("medium", "10 KB-1 MiB"), ("large", ">1 MiB"))
RTO_RE = re.compile(r"\blost packets (\d+)")
FILE_RE = re.compile(r"(.+?)_seed(\d+)_trim_(off|on)\.txt$")

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--rack-root", type=Path, required=True)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executed-rto-us", type=float, required=True)
    parser.add_argument("--sample-per-group", type=int, default=5000)
    parser.add_argument("--fct-rate", type=float, required=True)
    parser.add_argument("--rto-rate", type=float, required=True)
    parser.add_argument("--pfld-profile", default="pfld_probe16")
    parser.add_argument(
        "--size-binning", choices=("figure-axis", "paper-prose"), default="figure-axis",
        help=(
            "Flow-size boundaries. figure-axis uses the committed plot labels and helper "
            "(10,000 B and 1 MiB); paper-prose uses the ranges stated in Section 6.3 "
            "(10 KiB and 100 KiB)."
        ),
    )
    return parser.parse_args()


def run_root(base: Path, variant: str) -> Path:
    return base / "camera_ready_new_images" / variant


def load_fct(
    source: Path, rack: Path, sample_per_group: int, size_binning: str,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    history = collect_data(source, min_flows=1, only_max=False, remove_outliers_mode="none")
    rack_frame = collect_data(rack, min_flows=1, only_max=False, remove_outliers_mode="none")
    rack_frame = rack_frame[rack_frame["algorithm"] == "rss_rack_tlp"].copy()
    frame = pd.concat([history, rack_frame], ignore_index=True)
    if size_binning == "paper-prose":
        sizes = pd.to_numeric(frame["flow_bytes"], errors="coerce")
        frame["size_bin"] = np.select(
            (sizes <= 10 * 1024, sizes <= 100 * 1024),
            ("small", "medium"),
            default="large",
        )
    stats = (
        frame.groupby(["drop_rate_value", "size_bin", "algorithm", "trim_status"])
        ["completion_time"]
        .agg(
            samples="count", median="median",
            p99=lambda values: values.quantile(0.99),
            p995=lambda values: values.quantile(0.995), maximum="max",
        )
        .reset_index()
    )
    sampled = []
    keys = ["drop_rate_value", "size_bin", "algorithm", "trim_status"]
    for _, rows in frame.groupby(keys, sort=False):
        sampled.append(rows.sample(n=min(len(rows), sample_per_group), random_state=31))
    return stats, pd.concat(sampled, ignore_index=True)


def plot_fct(stats: pd.DataFrame, sampled: pd.DataFrame, rate: float, pfld_source: str,
             percentile: str, output: Path, stem: str, size_binning: str) -> None:
    keep_rates = np.isclose(sampled["drop_rate_value"], 0) | np.isclose(sampled["drop_rate_value"], rate)
    selected = sampled[keep_rates].copy()
    selected = selected[selected["algorithm"].isin((*ORDER, pfld_source))].copy()
    if pfld_source != "pfld":
        selected = selected[selected["algorithm"] != "pfld"].copy()
    selected.loc[selected["algorithm"] == pfld_source, "algorithm"] = "pfld"
    selected = selected.drop_duplicates(
        subset=["drop_rate_value", "size_bin", "algorithm", "trim_status", "completion_time", "source_file"]
    )
    selected["trim"] = selected["trim_status"].map({"off": "Off", "on": "On"})

    # Keep the paper's 2 by 3 layout, trim colors, violin geometry, and
    # statistic convention. RACK is appended as one additional method.
    sns.set_style("whitegrid")
    figure, axes = plt.subplots(
        2, 3, figsize=(OLD_FIGURE9_WIDTH, OLD_FIGURE9_HEIGHT),
        sharex=True, sharey=False,
    )
    labels = SIZE_BINS if size_binning == "figure-axis" else (
        ("small", "0-10 KiB"), ("medium", "10-100 KiB"), ("large", ">100 KiB")
    )
    for row_index, plot_rate in enumerate((0.0, rate)):
        for column_index, (size_bin, title) in enumerate(labels):
            axis = axes[row_index, column_index]
            panel = selected[
                np.isclose(selected["drop_rate_value"], plot_rate)
                & (selected["size_bin"] == size_bin)
            ]
            sns.violinplot(
                data=panel, x="algorithm", y="completion_time", hue="trim",
                order=ORDER, hue_order=("Off", "On"), palette="Set2", inner="box",
                cut=0, density_norm="width", width=0.8, linewidth=1.0, ax=axis,
            )
            stat_rows = stats[
                np.isclose(stats["drop_rate_value"], plot_rate)
                & (stats["size_bin"] == size_bin)
            ].copy()
            if pfld_source != "pfld":
                stat_rows = stat_rows[stat_rows["algorithm"] != "pfld"].copy()
            stat_rows.loc[stat_rows["algorithm"] == pfld_source, "algorithm"] = "pfld"
            for algorithm_index, algorithm in enumerate(ORDER):
                for trim_index, trim in enumerate(("off", "on")):
                    row = stat_rows[
                        (stat_rows["algorithm"] == algorithm) & (stat_rows["trim_status"] == trim)
                    ]
                    if row.empty:
                        continue
                    value = row.iloc[0]
                    x_value = algorithm_index + (-0.20 if trim_index == 0 else 0.20)
                    axis.scatter(
                        x_value, value[percentile], marker="D", s=44,
                        color="black", alpha=0.70, zorder=5,
                    )

                    y_min, y_max = axis.get_ylim()
                    y_pad = 0.03 * (y_max - y_min)
                    median_text = f"{value['median']:.0f}"
                    percentile_text = f"{value[percentile]:.0f}"
                    if algorithm_index == 0 and trim == "off":
                        annotation = f"Median: {median_text}\nP99: {percentile_text}"
                        annotation_y = value[percentile] + y_pad
                        vertical_alignment = "bottom"
                    elif algorithm_index == 0 and trim == "on":
                        annotation = f"{median_text}\n{percentile_text}"
                        annotation_y = max(y_min + y_pad, value["median"] - y_pad)
                        vertical_alignment = "top"
                    else:
                        annotation = f"{median_text}\n{percentile_text}"
                        annotation_y = value[percentile] + y_pad
                        vertical_alignment = "bottom"
                    axis.text(
                        x_value, annotation_y, annotation,
                        ha="center", va=vertical_alignment, fontsize=8,
                        color="black",
                        bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.6, "pad": 1},
                    )
            y_min, y_max = axis.get_ylim()
            y_span = y_max - y_min
            if y_span > 0:
                axis.set_ylim(y_min, y_max + 0.10 * y_span)
            if row_index == 0:
                axis.text(
                    0.02, 0.98, title, transform=axis.transAxes,
                    ha="left", va="top", fontsize=10, fontweight="bold",
                    color="black",
                    bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.6, "pad": 1},
                )
            axis.set_title("")
            axis.set_xlabel("")
            axis.set_xticks(range(len(ORDER)))
            axis.set_xticklabels([LABELS[item] for item in ORDER], rotation=20, ha="right")
            axis.set_ylabel("Flow Completion Time (us)" if column_index == 0 else "")
            legend = axis.get_legend()
            if row_index == 0 and column_index == 0:
                if legend is not None:
                    legend.set_title("Trim")
                    legend.set_loc("upper right")
                    legend.set_ncols(2)
            elif legend is not None:
                legend.remove()
    figure.text(0.01, 0.75, "No Corruption Drops", rotation=90, va="center", ha="left")
    figure.text(0.01, 0.25, "With Corruption Drops", rotation=90, va="center", ha="left")
    figure.tight_layout(rect=(0.03, 0, 1, 0.97))
    figure.savefig(output / f"{stem}.png", dpi=220)
    figure.savefig(output / f"{stem}.pdf")
    plt.close(figure)


def collect_rtos(*roots: Path) -> pd.DataFrame:
    records = []
    for root in roots:
        for drop_dir in sorted(root.glob("drop_*")):
            if not drop_dir.is_dir():
                continue
            try:
                rate = float(drop_dir.name.removeprefix("drop_"))
            except ValueError:
                continue
            for path in sorted(drop_dir.glob("*.txt")):
                matched = FILE_RE.fullmatch(path.name)
                if not matched:
                    continue
                algorithm, seed, trim = matched.groups()
                total = 0
                flows = 0
                completed_flows = 0
                with path.open(encoding="utf-8", errors="ignore") as handle:
                    for line in handle:
                        if " finished at " not in line:
                            continue
                        completed_flows += 1
                        value = RTO_RE.search(line)
                        if value:
                            count = int(value.group(1))
                            total += count
                            flows += int(count > 0)
                if completed_flows == 0:
                    continue
                records.append({
                    "drop_rate": rate, "algorithm": algorithm, "seed": int(seed),
                    "trim_status": trim,
                    "rto_count": total, "flows_with_rto": flows, "source_file": str(path),
                })
    return pd.DataFrame(records)


def rto_plot_frame(rtos: pd.DataFrame, rate: float, pfld_source: str) -> pd.DataFrame:
    rows = rtos[np.isclose(rtos["drop_rate"], rate)].copy()
    rows = rows[rows["algorithm"].isin((*ORDER, pfld_source))].copy()
    rows = rows[(rows["algorithm"] != "pfld") | (pfld_source == "pfld")]
    rows.loc[rows["algorithm"] == pfld_source, "algorithm"] = "pfld"
    rows = (
        rows.groupby(["algorithm", "trim_status"], as_index=False)
        .agg(
            rto_count=("rto_count", "median"),
            flows_with_rto=("flows_with_rto", "median"),
            seeds=("seed", "nunique"),
        )
    )
    baseline = float(rows[(rows["algorithm"] == "ecmp") & (rows["trim_status"] == "off")]["rto_count"].iloc[0])
    rows["reduction_percent"] = 100.0 * (1.0 - rows["rto_count"] / baseline)
    rows["trim"] = rows["trim_status"].map({"off": "Off", "on": "On"})
    return rows


def draw_rto_panel(axis: plt.Axes, frame: pd.DataFrame, title: str) -> None:
    """Draw the compressed grouped bar chart used in the paper."""
    sns.barplot(
        data=frame, x="algorithm", y="rto_count", hue="trim", order=ORDER,
        hue_order=("Off", "On"), palette="Set2", errorbar=None,
        edgecolor="black", linewidth=0.7, ax=axis,
    )
    axis.set_yscale("log")
    axis.set_title(title, fontsize=9)
    axis.set_xlabel("")
    axis.set_ylabel("RTOs Triggered")
    axis.set_xticks(range(len(ORDER)))
    axis.set_xticklabels([LABELS[item] for item in ORDER], rotation=20, ha="right")
    annotation_rows = []
    for trim in ("Off", "On"):
        for algorithm in ORDER:
            selected = frame[(frame["algorithm"] == algorithm) & (frame["trim"] == trim)]
            if not selected.empty:
                annotation_rows.append(selected.iloc[0])
    for bar, row in zip(axis.patches, annotation_rows):
        if bar.get_height() <= 0:
            continue
        if row["algorithm"] == "ecmp" and row["trim_status"] == "off":
            label = "Baseline"
        else:
            reduction = float(row["reduction_percent"])
            displayed = 99 if reduction >= 99.5 else int(round(max(0.0, reduction)))
            label = f"-{displayed}%"
        axis.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 1.12,
                  label, ha="center", va="bottom", fontsize=9, color="black")
    legend = axis.get_legend()
    if legend is not None:
        legend.remove()
    axis.legend(title="Trim", loc="lower left", ncols=2)


def plot_rto(
    rtos: pd.DataFrame,
    output: Path,
    rate: float,
    pfld_profile: str,
) -> None:
    available_rates = sorted(rate for rate in rtos["drop_rate"].unique() if rate > 0)
    if not any(np.isclose(rate, available) for available in available_rates):
        raise ValueError(f"Requested RTO plot rate {rate:g} is absent from the raw data")
    if pfld_profile not in set(rtos["algorithm"]):
        raise ValueError(f"Requested PFLD profile {pfld_profile} is absent from the raw data")
    sns.set_style("whitegrid")
    final_frame = rto_plot_frame(rtos, rate, pfld_profile)
    final_frame.to_csv(output / "camera_ready_new_image2.csv", index=False)
    figure, axis = plt.subplots(figsize=(OLD_FIGURE10_WIDTH, OLD_FIGURE10_HEIGHT))
    draw_rto_panel(axis, final_frame, "")
    legend = axis.get_legend()
    if legend is not None:
        legend.set_title("Trim")
        legend.set_loc("lower left")
    figure.tight_layout(pad=0.6)
    figure.savefig(output / "camera_ready_new_image2.png", dpi=220, bbox_inches="tight")
    figure.savefig(output / "camera_ready_new_image2.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    if args.output.exists():
        raise SystemExit(f"Refusing existing output directory: {args.output}")
    args.output.mkdir(parents=True)
    source = run_root(args.source_root, args.variant)
    rack = run_root(args.rack_root, args.variant)

    stats, sampled = load_fct(source, rack, args.sample_per_group, args.size_binning)
    stats.to_csv(args.output / "camera_ready_fct_summary.csv", index=False)
    rates = set(stats["drop_rate_value"])
    if not any(np.isclose(args.fct_rate, rate) for rate in rates):
        raise SystemExit(f"Requested FCT plot rate {args.fct_rate:g} is absent from the raw data")
    if args.pfld_profile not in set(stats["algorithm"]):
        raise SystemExit(f"Requested PFLD profile {args.pfld_profile} is absent from the raw data")
    plot_fct(
        stats, sampled, args.fct_rate, args.pfld_profile,
        "p99", args.output, "camera_ready_new_image1",
        args.size_binning,
    )

    rtos = collect_rtos(source, rack)
    rtos.to_csv(args.output / "camera_ready_rto_counts.csv", index=False)
    plot_rto(rtos, args.output, args.rto_rate, args.pfld_profile)

    finding = {
        "variant": args.variant,
        "size_binning": args.size_binning,
        "executed_rto_us": args.executed_rto_us,
        "camera_ready_new_image1_drop_rate": args.fct_rate,
        "camera_ready_new_image1_pfld_profile": args.pfld_profile,
        "camera_ready_new_image1_percentile_marker": "p99",
        "figure10_metric": "median across seeds of summed per-flow timeout events (lost packets)",
        "camera_ready_new_image2_drop_rate": args.rto_rate,
        "camera_ready_new_image2_pfld_profile": args.pfld_profile,
    }
    (args.output / "finding.json").write_text(
        json.dumps(finding, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(finding, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
