#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import os
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from visualize_headroom import parse_log as parse_headroom_log
from visualize_probe import parse_log as parse_probe_log

BIN = ROOT / "build" / "htsim_uec"
METRICS_CFG = ROOT / "scripts" / "metrics_collection_policies" / "collect_default.json"
X_LABEL_SIZE_SCALE = 0.85
Y_LABEL_SIZE_SCALE = 0.95


@dataclass(frozen=True)
class RunCase:
    panel: str
    case_id: str
    x_label: str
    title: str
    parse_kind: str
    tm: Path
    topo: Path
    base_args: tuple[str, ...]
    extra_args: tuple[str, ...]


CASES: tuple[RunCase, ...] = (
    RunCase(
        panel="probe",
        case_id="without_proactive_probes",
        x_label="PFLD Without\nProactive Probes",
        title="With Corruption Drops",
        parse_kind="probe",
        tm=ROOT / "scripts" / "connection_matrices" / "proactive.cm",
        topo=ROOT / "scripts" / "topologies" / "64_400.topo",
        base_args=(
            "-q", "148",
            "-cwnd", "148",
            "-mtu", "4160",
            "-sack_threshold", "0",
            "-ecn", "29", "118",
            "-switch_random_drop_prob", "0.0005",
            "-fail_psn", "-1",
            "-rto_ratio", "5",
            "-load_balancing_algo", "rss",
            "-linkspeed", "400000",
            "-rss_parameters", "mean_rtt", "16", "15", "0", "0", "25",
            "-sender_cc_only",
            "-sender_cc_algo", "dctcp",
            "-nodes", "64",
            "-disable_trim",
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_rtx_probe",
            "-no_droping_low_header",
        ),
        extra_args=("-pflr_proactive_probe", "-1"),
    ),
    RunCase(
        panel="probe",
        case_id="with_proactive_probes",
        x_label="PFLD With\nProactive Probes",
        title="With Corruption Drops",
        parse_kind="probe",
        tm=ROOT / "scripts" / "connection_matrices" / "proactive.cm",
        topo=ROOT / "scripts" / "topologies" / "64_400.topo",
        base_args=(
            "-q", "148",
            "-cwnd", "148",
            "-mtu", "4160",
            "-sack_threshold", "0",
            "-ecn", "29", "118",
            "-switch_random_drop_prob", "0.0005",
            "-fail_psn", "-1",
            "-rto_ratio", "5",
            "-load_balancing_algo", "rss",
            "-linkspeed", "400000",
            "-rss_parameters", "mean_rtt", "16", "15", "0", "0", "25",
            "-sender_cc_only",
            "-sender_cc_algo", "dctcp",
            "-nodes", "64",
            "-disable_trim",
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_rtx_probe",
            "-no_droping_low_header",
        ),
        extra_args=("-pflr_proactive_probe", "1"),
    ),
    RunCase(
        panel="headroom",
        case_id="without_buffer_headroom",
        x_label="PFLD Without\nBuff. Headroom",
        title="With Corruption Drops",
        parse_kind="headroom",
        tm=ROOT / "scripts" / "connection_matrices" / "proactive.cm",
        topo=ROOT / "scripts" / "topologies" / "64_400.topo",
        base_args=(
            "-q", "148",
            "-cwnd", "148",
            "-mtu", "4160",
            "-sack_threshold", "0",
            "-ecn", "29", "118",
            "-switch_random_drop_prob", "0.0005",
            "-fail_psn", "-1",
            "-rto_ratio", "5",
            "-load_balancing_algo", "rss",
            "-linkspeed", "400000",
            "-rss_parameters", "mean_rtt", "16", "15", "0", "0", "25",
            "-sender_cc_only",
            "-sender_cc_algo", "dctcp",
            "-nodes", "64",
            "-disable_trim",
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_rtx_probe",
            "-pflr_proactive_probe", "1",
        ),
        extra_args=(),
    ),
    RunCase(
        panel="headroom",
        case_id="with_buffer_headroom",
        x_label="PFLD With\nBuff. Headroom",
        title="With Corruption Drops",
        parse_kind="headroom",
        tm=ROOT / "scripts" / "connection_matrices" / "proactive.cm",
        topo=ROOT / "scripts" / "topologies" / "64_400.topo",
        base_args=(
            "-q", "148",
            "-cwnd", "148",
            "-mtu", "4160",
            "-sack_threshold", "0",
            "-ecn", "29", "118",
            "-switch_random_drop_prob", "0.0005",
            "-fail_psn", "-1",
            "-rto_ratio", "5",
            "-load_balancing_algo", "rss",
            "-linkspeed", "400000",
            "-rss_parameters", "mean_rtt", "16", "15", "0", "0", "25",
            "-sender_cc_only",
            "-sender_cc_algo", "dctcp",
            "-nodes", "64",
            "-disable_trim",
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_rtx_probe",
            "-pflr_proactive_probe", "1",
        ),
        extra_args=("-no_droping_low_header",),
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Regenerate the PFLD proactive-probe/headroom figure with fresh multi-seed data."
    )
    parser.add_argument("--num-seeds", type=int, default=50, help="Number of seeds per config.")
    parser.add_argument("--binary", type=Path, default=BIN, help="Simulator binary to execute.")
    parser.add_argument("--seed-start", type=int, default=1, help="Starting seed value.")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4), help="Parallel simulator jobs.")
    parser.add_argument(
        "--sim-out-root",
        type=Path,
        default=ROOT / "nsdi_results" / "pfld_probe_headroom_50seeds",
        help="Directory to store fresh simulator logs and per-run artifacts.",
    )
    parser.add_argument(
        "--figure-dir",
        type=Path,
        default=ROOT / "SC_Final" / "figures",
        help="Directory to save the final figure and CSV summaries.",
    )
    parser.add_argument(
        "--prefix",
        default="pfld_probe_headroom_errorbars_50seeds",
        help="Filename prefix for saved outputs.",
    )
    parser.add_argument(
        "--drop-rate",
        type=float,
        default=0.0005,
        help="Per-switch corruption probability.",
    )
    parser.add_argument(
        "--rto-us", type=float, default=None,
        help="Use an exact positive RTO in microseconds instead of the case ratio.",
    )
    parser.add_argument(
        "--errorbar",
        choices=("std", "sem", "ci95"),
        default="ci95",
        help="Error-bar aggregation mode across seeds.",
    )
    parser.add_argument(
        "--reuse-raw-csv",
        type=Path,
        default=None,
        help="Reuse an existing raw per-seed CSV instead of rerunning simulations.",
    )
    parser.add_argument("--fig-w", type=float, default=5.6, help="Figure width in inches.")
    parser.add_argument("--fig-h", type=float, default=2.35, help="Figure height in inches.")
    return parser.parse_args()


def _build_command(case: RunCase, seed: int, metrics_dir: Path, drop_rate: float,
                   rto_us: float | None, binary: Path) -> list[str]:
    base_args = list(case.base_args)
    drop_index = base_args.index("-switch_random_drop_prob")
    base_args[drop_index + 1] = f"{drop_rate:.10g}"
    if rto_us is not None:
        rto_index = base_args.index("-rto_ratio")
        base_args[rto_index:rto_index + 2] = ["-rto_us", str(rto_us)]
    return [
        str(binary),
        "-data_collection_config", str(METRICS_CFG),
        "-end", "10000",
        "-seed", str(seed),
        "-tm", str(case.tm),
        "-topo", str(case.topo),
        *base_args,
        "-data_collection_dir", str(metrics_dir),
        "-log_reaction_events",
        *case.extra_args,
    ]


def _parse_time_file(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            values[key.strip()] = value.strip()
    return values


def _run_case(case: RunCase, seed: int, output_root: Path, drop_rate: float,
              rto_us: float | None, binary: Path) -> dict[str, object]:
    panel_dir = output_root / case.panel / case.case_id
    metrics_dir = panel_dir / "metrics" / f"seed_{seed:03d}"
    metrics_dir.mkdir(parents=True, exist_ok=True)

    log_path = panel_dir / f"{case.case_id}_seed{seed:03d}.out"
    err_path = panel_dir / f"{case.case_id}_seed{seed:03d}.err"
    time_path = panel_dir / f"{case.case_id}_seed{seed:03d}.time"
    command_path = panel_dir / f"{case.case_id}_seed{seed:03d}.command"
    marker_path = panel_dir / f"{case.case_id}_seed{seed:03d}.PASS"
    cmd = _build_command(case, seed, metrics_dir, drop_rate, rto_us, binary)
    command_path.write_text(shlex.join(cmd) + "\n", encoding="utf-8")

    start = time.monotonic()
    with log_path.open("w", encoding="utf-8") as stdout_f, err_path.open("w", encoding="utf-8") as stderr_f:
        result = subprocess.run(
            ["/usr/bin/time", "-v", "-o", str(time_path), *cmd],
            cwd=metrics_dir, stdout=stdout_f, stderr=stderr_f, text=True,
        )
    (metrics_dir / "logout.dat").unlink(missing_ok=True)
    elapsed = time.monotonic() - start

    if result.returncode != 0:
        raise RuntimeError(f"htsim failed for {case.case_id} seed {seed}: rc={result.returncode}")

    parser = parse_probe_log if case.parse_kind == "probe" else parse_headroom_log
    df = parser(log_path)
    if df.empty:
        raise RuntimeError(f"No Drop/RTX pairs found for {case.case_id} seed {seed}")
    marker_path.write_text("PASS\n", encoding="utf-8")
    timing = _parse_time_file(time_path)

    p99_us = float(np.quantile(df["detect_delay_ps"].to_numpy(dtype=float) * 1e-6, 0.99))
    return {
        "panel": case.panel,
        "case_id": case.case_id,
        "label": case.x_label,
        "seed": seed,
        "matched_pairs": int(len(df)),
        "p99_us": p99_us,
        "drop_rate": drop_rate,
        "absolute_rto_override_us": rto_us,
        "runtime_seconds": elapsed,
        "max_rss_kbytes": timing.get("Maximum resident set size (kbytes)", ""),
        "log_bytes": log_path.stat().st_size,
        "log_path": str(log_path),
    }


def _compute_error(values: pd.Series, mode: str) -> float:
    if len(values) <= 1:
        return 0.0
    std = float(values.std(ddof=1))
    if mode == "std":
        return std
    sem = std / math.sqrt(len(values))
    if mode == "sem":
        return sem
    return 1.96 * sem


def _summarize(df: pd.DataFrame, errorbar: str) -> pd.DataFrame:
    grouped = (
        df.groupby(["panel", "case_id", "label"], as_index=False)
        .agg(
            mean_p99_us=("p99_us", "mean"),
            std_p99_us=("p99_us", "std"),
            num_seeds=("p99_us", "count"),
        )
    )
    errors = (
        df.groupby(["panel", "case_id", "label"])["p99_us"]
        .apply(lambda s: _compute_error(s, errorbar))
        .reset_index(name="error_us")
    )
    return grouped.merge(errors, on=["panel", "case_id", "label"], how="left")


def _panel_title(drop_rate: float) -> str:
    return "No Corruption Drops" if drop_rate == 0 else "With Corruption Drops"


def _panel_prefix(panel: str, num_seeds: int) -> str:
    stem = "pfld_proactive_errorbars" if panel == "probe" else "pfld_headroom_errorbars"
    return f"{stem}_{num_seeds}seeds"


def _plot_panel(panel_df: pd.DataFrame, figure_pdf: Path, figure_png: Path,
                fig_w: float, fig_h: float, drop_rate: float) -> None:
    sns.set_theme(
        style="whitegrid", context="paper", font="serif", font_scale=1.15
    )
    bar_color = sns.color_palette("Set2", n_colors=8)[0]
    case_orders = {
        "probe": ["without_proactive_probes", "with_proactive_probes"],
        "headroom": ["without_buffer_headroom", "with_buffer_headroom"],
    }

    panel = str(panel_df["panel"].iloc[0])
    panel_df = panel_df.copy()
    panel_df["order"] = panel_df["case_id"].map({cid: idx for idx, cid in enumerate(case_orders[panel])})
    panel_df = panel_df.sort_values("order")

    fig, ax = plt.subplots(1, 1, figsize=(fig_w, fig_h))

    x = np.arange(len(panel_df))
    vals = panel_df["mean_p99_us"].to_numpy(dtype=float)
    errs = panel_df["error_us"].to_numpy(dtype=float)
    labels = panel_df["label"].tolist()

    ax.bar(
        x,
        vals,
        yerr=errs,
        width=0.66,
        color=bar_color,
        edgecolor="k",
        linewidth=0.8,
        ecolor="k",
        capsize=4,
        zorder=3,
    )
    ax.set_title(_panel_title(drop_rate), pad=3)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("99th loss detection delay (us)")
    for tick in ax.get_xticklabels():
        tick.set_fontsize(tick.get_fontsize() * X_LABEL_SIZE_SCALE)
    ax.yaxis.label.set_size(ax.yaxis.label.get_size() * Y_LABEL_SIZE_SCALE)
    ax.tick_params(axis="x", rotation=0)
    ax.grid(axis="y", zorder=0)

    ymax = float(np.max(vals + errs)) if len(vals) else 1.0
    ax.set_ylim(0, ymax * 1.08 if ymax > 0 else 1.0)

    fig.tight_layout(pad=0.35)
    fig.savefig(figure_pdf, bbox_inches="tight")
    fig.savefig(figure_png, dpi=220, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    args = parse_args()
    args.binary = args.binary.resolve()
    if args.rto_us is not None and args.rto_us <= 0:
        raise SystemExit("--rto-us must be positive")
    args.figure_dir.mkdir(parents=True, exist_ok=True)

    if args.reuse_raw_csv is not None:
        raw_df = pd.read_csv(args.reuse_raw_csv)
        print(f"Reused raw per-seed data from {args.reuse_raw_csv}")
    else:
        if not args.binary.exists():
            raise FileNotFoundError(f"Missing simulator binary: {args.binary}")

        if args.sim_out_root.exists():
            raise FileExistsError(
                f"Refusing to reuse or delete an existing raw-output root: {args.sim_out_root}"
            )
        args.sim_out_root.mkdir(parents=True, exist_ok=True)

        futures = []
        results: list[dict[str, object]] = []
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
            for seed in range(args.seed_start, args.seed_start + args.num_seeds):
                for case in CASES:
                    futures.append(
                        executor.submit(
                            _run_case, case, seed, args.sim_out_root,
                            args.drop_rate, args.rto_us, args.binary,
                        )
                    )

            completed = 0
            total = len(futures)
            for future in as_completed(futures):
                row = future.result()
                results.append(row)
                completed += 1
                if completed % 20 == 0 or completed == total:
                    print(f"Completed {completed}/{total} runs")

        raw_df = pd.DataFrame(results).sort_values(["panel", "case_id", "seed"]).reset_index(drop=True)
        raw_csv = args.figure_dir / f"{args.prefix}_raw.csv"
        raw_df.to_csv(raw_csv, index=False)
        print(f"Saved raw per-seed data to {raw_csv}")

    summary_df = _summarize(raw_df, args.errorbar)
    summary_csv = args.figure_dir / f"{args.prefix}_summary.csv"
    summary_df.to_csv(summary_csv, index=False)
    print(f"Saved summary data to {summary_csv}")

    for panel in ("probe", "headroom"):
        panel_df = summary_df[summary_df["panel"] == panel].copy()
        panel_prefix = _panel_prefix(panel, args.num_seeds)
        figure_pdf = args.figure_dir / f"{panel_prefix}.pdf"
        figure_png = args.figure_dir / f"{panel_prefix}.png"
        _plot_panel(panel_df, figure_pdf, figure_png, args.fig_w / 2.0,
                    args.fig_h, args.drop_rate)
        print(f"Saved figure to {figure_pdf}")
        print(f"Saved figure to {figure_png}")


if __name__ == "__main__":
    main()
