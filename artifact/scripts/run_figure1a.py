#!/usr/bin/env python3
"""Regenerate Figure 1A RSS subflows-per-link time series from fresh logs."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns


REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from visualize_active_connections import bin_counts, read_logs


CASES = {
    "symmetric": {
        "topology": REPO / "scripts/topologies/128_400.topo",
        "extra_args": [],
    },
    "one_asymmetric_link": {
        "topology": REPO / "scripts/topologies/128_400.topo",
        # The paper says one output link is throttled to 20% capacity.
        "extra_args": ["-failed", "1", "-failed_link_ratio", "0.2"],
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--flow-bytes", type=int, default=250000000)
    parser.add_argument(
        "--failed-link-ratio", type=float, default=0.2,
        help="Capacity ratio passed to the aggregate-link failure path.",
    )
    parser.add_argument("--bin-us", type=float, default=0.25)
    parser.add_argument(
        "--display-window-us", type=float, default=25.0,
        help=("Non-overlapping median window used only for display. The underlying "
              "subflow counts retain --bin-us resolution."),
    )
    parser.add_argument(
        "--smooth-windows", type=int, default=1,
        help=("Centered rolling-median width over the display windows. This affects "
              "only the drawn lines; 1 disables smoothing."),
    )
    parser.add_argument(
        "--smooth-blend", type=float, default=1.0,
        help=("Weight assigned to the rolling median in the plotted line. Zero draws "
              "the display medians, one draws the full rolling median, and values in "
              "between retain a documented fraction of the original variation."),
    )
    parser.add_argument("--end-us", type=float, default=5000.0)
    parser.add_argument(
        "--workers", type=int, default=2,
        help="Number of simulator cases to run concurrently.",
    )
    parser.add_argument("--rto-us", type=float, default=None)
    parser.add_argument(
        "--raw-source-root", type=Path, default=None,
        help=("Plot already generated symmetric.out/one_asymmetric_link.out logs into "
              "a fresh output directory without rerunning the simulator."),
    )
    parser.add_argument(
        "--omit-base-csv", action="store_true",
        help=("Do not write the large base-resolution figure1a_binned.csv. This is "
              "useful for display-only sensitivity variants; summaries still use "
              "the base-resolution values held in memory."),
    )
    return parser.parse_args()


def parse_timing(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key.strip()] = value.strip()
    return result


def run_case(case: str, case_config: dict[str, object], traffic_matrix: Path,
             args: argparse.Namespace) -> dict[str, object]:
    run_dir = args.output_root / "runs" / case
    metrics = run_dir / "metrics"
    metrics.mkdir(parents=True, exist_ok=False)
    log = args.output_root / f"{case}.out"
    timing_path = run_dir / "time.txt"
    extra_args = list(case_config["extra_args"])
    if case == "one_asymmetric_link":
        extra_args = ["-failed", "1", "-failed_link_ratio", str(args.failed_link_ratio)]
    cmd = [
        str(REPO / "build/htsim_uec"),
        "-data_collection_config", str(REPO / "scripts/metrics_collection_policies/collect_default.json"),
        "-end", "10000", "-seed", str(args.seed),
        "-tm", str(traffic_matrix),
        "-topo", str(case_config["topology"]), "-q", "148", "-cwnd", "148", "-mtu", "4160",
        "-sack_threshold", "0", "-ecn", "29", "118", "-switch_random_drop_prob", "0",
        "-fail_psn", "-1",
        "-linkspeed", "400000", "-rss_parameters", "mean_rtt", "16", "15", "0", "0", "25",
        "-sender_cc_only", "-sender_cc_algo", "dctcp", "-data_collection_dir", str(metrics),
        "-nodes", "128", "-disable_trim", "-log_subflow_routes",
    ]
    if args.rto_us is not None:
        cmd.extend(["-rto_us", str(args.rto_us)])
    else:
        cmd.extend(["-rto_ratio", "1"])
    cmd.extend(["-load_balancing_algo", "rss", *extra_args])
    (run_dir / "command.txt").write_text(shlex.join(cmd) + "\n", encoding="utf-8")
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as output:
        completed = subprocess.run(
            ["/usr/bin/time", "-v", "-o", str(timing_path), *cmd], cwd=REPO,
            stdout=output, stderr=subprocess.STDOUT,
        )
    elapsed = time.monotonic() - started
    if completed.returncode != 0:
        raise RuntimeError(f"{case} failed with exit code {completed.returncode}")
    frame = read_logs([str(log)])
    if frame.empty:
        raise RuntimeError(f"{case} contains no per-switch subflow-route records")
    (run_dir / "PASS").write_text("PASS\n", encoding="utf-8")
    timing = parse_timing(timing_path)
    return {
        "case": case,
        "runtime_seconds": elapsed,
        "max_rss_kbytes": timing.get("Maximum resident set size (kbytes)", ""),
        "log_bytes": log.stat().st_size,
        "route_records": len(frame),
    }


def plot(args: argparse.Namespace, raw_root: Path) -> None:
    all_binned: list[pd.DataFrame] = []
    bin_ps = int(args.bin_us * 1e6)
    end_ps = int(args.end_us * 1e6)
    for case in CASES:
        frame = read_logs([str(raw_root / f"{case}.out")])
        binned = bin_counts(frame, bin_ps, 0, end_ps)
        binned.insert(0, "case", case)
        all_binned.append(binned)
    combined = pd.concat(all_binned, ignore_index=True)
    if not args.omit_base_csv:
        combined.to_csv(args.output_root / "figure1a_binned.csv", index=False)
    display_ps = int(args.display_window_us * 1e6)
    if display_ps < bin_ps:
        raise ValueError("--display-window-us must be at least --bin-us")
    combined["display_start_ps"] = (
        combined["bin_start_ps"] // display_ps
    ) * display_ps
    display = (
        combined.groupby(["case", "switch", "display_start_ps"], as_index=False)
        ["count"].median()
        .rename(columns={"display_start_ps": "bin_start_ps"})
    )
    display.to_csv(args.output_root / "figure1a_display.csv", index=False)
    plotted = display.sort_values(["case", "switch", "bin_start_ps"]).copy()
    plotted["smoothed_count"] = (
        plotted.groupby(["case", "switch"], sort=False)["count"]
        .transform(
            lambda values: values.rolling(
                args.smooth_windows, center=True, min_periods=1
            ).median()
        )
    )
    plotted["plot_count"] = (
        (1.0 - args.smooth_blend) * plotted["count"]
        + args.smooth_blend * plotted["smoothed_count"]
    )
    plotted.to_csv(args.output_root / "figure1a_plot.csv", index=False)
    summary: dict[str, object] = {}
    for case in CASES:
        selected = combined[combined["case"] == case]
        switch_medians = selected.groupby("switch")["count"].median().sort_values()
        positive = selected[selected["count"] > 0]
        case_summary: dict[str, object] = {
            "median_subflows_all_link_bins": float(selected["count"].median()),
            "p05_subflows_all_link_bins": float(selected["count"].quantile(0.05)),
            "p95_subflows_all_link_bins": float(selected["count"].quantile(0.95)),
            "lowest_link": str(switch_medians.index[0]),
            "lowest_link_median_subflows": float(switch_medians.iloc[0]),
            "other_links_median_subflows": float(switch_medians.iloc[1:].median()),
            "last_positive_bin_us": (
                float(positive["bin_start_ps"].max() / 1e6) if not positive.empty else None
            ),
        }
        summary[case] = case_summary
    summary["paper_visual_targets"] = {
        "active_interval_us": "approximately 0--5000",
        "symmetric_link_band": "approximately 10--25 subflows/link",
        "degraded_link_band": "approximately 4--8 subflows/link",
        "source": "visual ranges read from paper Figure 1A; original numeric data unavailable",
    }
    summary["display_processing"] = {
        "base_count_bin_us": args.bin_us,
        "display_window_us": args.display_window_us,
        "aggregation": "median of base-bin subflow counts in non-overlapping windows",
        "plot_smoothing_windows": args.smooth_windows,
        "plot_smoothing_blend": args.smooth_blend,
        "plot_smoothing": (
            "documented blend of display medians and a centered rolling median; display "
            "CSV and base-resolution scientific values remain unchanged"
        ),
        "scientific_values_computed_from": (
            "base-resolution values in memory; identical to figure1a_binned.csv"
        ),
        "base_resolution_csv_written": not args.omit_base_csv,
    }
    (args.output_root / "claim_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    sns.set_theme(style="whitegrid", context="paper", palette="Set2")
    fig, axes = plt.subplots(2, 1, figsize=(3.1, 2.6), sharex=True, sharey=True)
    titles = {"symmetric": "Symmetric Links", "one_asymmetric_link": "One Asymmetric Link"}
    for ax, case in zip(axes, CASES):
        selected = plotted[plotted["case"] == case]
        medians = combined[combined["case"] == case].groupby("switch")["count"].median().sort_values()
        degraded = medians.index[0] if case == "one_asymmetric_link" else None
        for switch, rows in selected.sort_values(["switch", "bin_start_ps"]).groupby("switch"):
            color = "#5b9e77" if switch == degraded else None
            label = "Degraded Link" if switch == degraded else None
            ax.plot(rows["bin_start_ps"] / 1e6, rows["plot_count"], linewidth=1.0,
                    alpha=0.9, color=color, label=label)
        ax.set_title(titles[case], fontsize=9, pad=2)
        ax.set_ylabel("Subflows Per Link", fontsize=8)
        ax.set_xlim(0, args.end_us)
        ax.set_ylim(0, 30)
        if degraded is not None:
            ax.legend(loc="lower center", fontsize=7, frameon=False)
    axes[-1].set_xlabel("Time [us]", fontsize=8)
    axes[-1].set_xticks([0, 2000, 4000])
    fig.tight_layout(pad=0.35)
    fig.savefig(args.output_root / "figure1a.png", dpi=220, bbox_inches="tight")
    fig.savefig(args.output_root / "figure1a.pdf", bbox_inches="tight")
    plt.close(fig)


def main() -> int:
    args = parse_args()
    if args.rto_us is not None and args.rto_us <= 0:
        raise SystemExit("--rto-us must be positive")
    if args.workers < 1:
        raise SystemExit("--workers must be positive")
    if not 0 < args.failed_link_ratio <= 1:
        raise SystemExit("--failed-link-ratio must be in (0, 1]")
    if args.smooth_windows < 1 or args.smooth_windows % 2 == 0:
        raise SystemExit("--smooth-windows must be a positive odd integer")
    if not 0.0 <= args.smooth_blend <= 1.0:
        raise SystemExit("--smooth-blend must be between 0 and 1")
    if args.output_root.exists():
        raise SystemExit(f"Refusing existing output root: {args.output_root}")
    args.output_root.mkdir(parents=True)
    raw_root = args.raw_source_root.resolve() if args.raw_source_root else args.output_root
    provenance = {
        "paper_element": "Figure 1A",
        "seed": args.seed,
        "bin_us": args.bin_us,
        "display_window_us": args.display_window_us,
        "smooth_windows": args.smooth_windows,
        "smooth_blend": args.smooth_blend,
        "plot_end_us": args.end_us,
        "flow_bytes": args.flow_bytes,
        "failed_link_ratio": args.failed_link_ratio,
        "absolute_rto_override_us": args.rto_us,
        "flow_size_basis": "configured for Figure 1A's approximately 5 ms active interval",
        "asymmetric_case": "one failed aggregate link at 20% capacity (-failed 1 -failed_link_ratio 0.2), as stated in the paper",
        "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "raw_source_root": str(raw_root),
    }
    (args.output_root / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if args.raw_source_root:
        missing = [str(raw_root / f"{case}.out") for case in CASES if not (raw_root / f"{case}.out").is_file()]
        if missing:
            raise SystemExit("Missing raw logs: " + ", ".join(missing))
        provenance["raw_source_files"] = {
            f"{case}.out": (raw_root / f"{case}.out").stat().st_size for case in CASES
        }
        (args.output_root / "provenance.json").write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        plot(args, raw_root)
        print(f"Replotted Figure 1A from {raw_root}")
        return 0

    traffic_matrix = args.output_root / f"tornado_128_{args.flow_bytes}.cm"
    matrix_lines = ["Nodes 128", "Connections 128"]
    matrix_lines.extend(
        f"{source}->{(source + 64) % 128} id {source + 1} start 0 size {args.flow_bytes}"
        for source in range(128)
    )
    traffic_matrix.write_text("\n".join(matrix_lines) + "\n", encoding="utf-8")
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        rows = list(executor.map(
            lambda item: run_case(item[0], item[1], traffic_matrix, args), CASES.items()
        ))
    with (args.output_root / "run_status.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=list(rows[0]), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)
    plot(args, raw_root)
    print(pd.DataFrame(rows).to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
