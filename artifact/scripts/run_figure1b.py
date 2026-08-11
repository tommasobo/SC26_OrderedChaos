#!/usr/bin/env python3
"""Fresh Figure 1B reaction-time experiment with resource/provenance capture."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import seaborn as sns
from pandas.errors import EmptyDataError


REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "build" / "htsim_uec"
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

DROP_RE = re.compile(
    r"^Drop:\s*FlowID\s+(\d+)\s*-\s*Packet ID\s+(\d+)\s*-\s*Time\s+(\d+)",
    re.IGNORECASE,
)
RTX_RE = re.compile(
    r"^RTX:\s*FlowID\s+(\d+)\s*-\s*Packet ID\s+(\d+)\s*-\s*Time\s+(\d+)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Case:
    name: str
    drop: bool
    extra: tuple[str, ...]


CASES = (
    Case("nodrop_no_trim_rss", False, ("-disable_trim",)),
    Case("nodrop_yes_trim_rss", False, ()),
    Case("nodrop_pfld_rss", False, (
        "-no_droping_low_header", "-disable_trim", "-precisefastlossrecovery", "3",
        "-pflr_proactive_probe", "1", "-pflr_proactive_rtx_probe",
    )),
    Case("yesdrop_no_trim_rss", True, ("-disable_trim",)),
    Case("yesdrop_yes_trim_rss", True, ()),
    Case("yesdrop_pfld_rss", True, (
        "-no_droping_low_header", "-disable_trim", "-precisefastlossrecovery", "3",
        "-pflr_proactive_probe", "1", "-pflr_proactive_rtx_probe",
    )),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=BIN)
    parser.add_argument("--positive-drop-rate", type=float, required=True)
    parser.add_argument("--seed", type=int, default=5)
    parser.add_argument(
        "--seeds", default="",
        help="Seed list/range (for example 1-8 or 1,5,9). Overrides --seed.",
    )
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--rto-us", type=float, default=None)
    parser.add_argument(
        "--raw-source-root", type=Path, default=None,
        help="Recompute the multiseed estimator from an existing fresh run root.",
    )
    parser.add_argument(
        "--timeout-seconds", type=float, default=300.0,
        help="Per-simulation watchdog; timed-out seeds are recorded and excluded from error bars.",
    )
    return parser.parse_args()


def expand_seeds(spec: str, fallback: int) -> list[int]:
    if not spec:
        return [fallback]
    result: set[int] = set()
    for token in spec.split(","):
        token = token.strip()
        if "-" in token:
            lo, hi = (int(part) for part in token.split("-", 1))
            if lo <= 0 or hi < lo:
                raise ValueError(f"Invalid seed range: {token}")
            result.update(range(lo, hi + 1))
        else:
            value = int(token)
            if value <= 0:
                raise ValueError("Seeds must be positive")
            result.add(value)
    return sorted(result)


def command(case: Case, args: argparse.Namespace, metrics: Path, seed: int) -> list[str]:
    command_line = [
        str(args.binary),
        "-data_collection_config", str(REPO / "scripts/metrics_collection_policies/collect_default.json"),
        "-end", "10000", "-seed", str(seed),
        "-tm", str(REPO / "scripts/connection_matrices/example_motiv_64_8_2502656.cm"),
        "-topo", str(REPO / "scripts/topologies/64_400.topo"),
        "-q", "148", "-cwnd", "148", "-mtu", "4160", "-sack_threshold", "0",
        "-ecn", "29", "118",
        "-switch_random_drop_prob", f"{args.positive_drop_rate if case.drop else 0:.10g}",
        "-load_balancing_algo", "rss", "-linkspeed", "400000",
        "-rss_parameters", "mean_rtt", "16", "150", "0", "0", "25",
        "-sender_cc_only", "-sender_cc_algo", "smartt_ecn_aifd",
        "-data_collection_dir", str(metrics), "-nodes", "64",
        "-log_reaction_events", *case.extra,
    ]
    if args.rto_us is not None:
        command_line.extend(["-rto_us", str(args.rto_us)])
    else:
        command_line.extend(["-rto_ratio", "1"])
    return command_line


def parse_time(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key.strip()] = value.strip()
    return result


def parse_reaction_pairs(path: Path) -> pd.DataFrame:
    drop_times: dict[tuple[int, int], int] = {}
    matched: set[tuple[int, int]] = set()
    records: list[dict[str, int]] = []
    with path.open(encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            if line.startswith("Drop:"):
                match = DROP_RE.match(line)
                if match is None:
                    continue
                flow, packet, timestamp = map(int, match.groups())
                key = (flow, packet)
                if key not in drop_times or timestamp < drop_times[key]:
                    drop_times[key] = timestamp
            elif line.startswith("RTX:"):
                match = RTX_RE.match(line)
                if match is None:
                    continue
                flow, packet, timestamp = map(int, match.groups())
                key = (flow, packet)
                if key not in drop_times or key in matched:
                    continue
                drop_timestamp = drop_times[key]
                if timestamp < drop_timestamp:
                    continue
                matched.add(key)
                records.append(
                    {
                        "flow_id": flow,
                        "packet_id": packet,
                        "drop_time_ps": drop_timestamp,
                        "rtx_time_ps": timestamp,
                        "detect_delay_ps": timestamp - drop_timestamp,
                    }
                )
    return pd.DataFrame.from_records(records)


def run_one(case: Case, seed: int, args: argparse.Namespace) -> dict[str, object]:
    run_dir = args.output_root / "runs" / f"seed{seed}" / case.name
    metrics = run_dir / "metrics"
    metrics.mkdir(parents=True, exist_ok=False)
    log = args.output_root / "logs" / f"{case.name}_seed{seed}.out"
    log.parent.mkdir(parents=True, exist_ok=True)
    err = run_dir / "stderr.log"
    timing_path = run_dir / "time.txt"
    cmd = command(case, args, metrics, seed)
    (run_dir / "command.txt").write_text(shlex.join(cmd) + "\n", encoding="utf-8")
    started = time.monotonic()
    timed_out = False
    with log.open("w", encoding="utf-8") as stdout, err.open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(
            ["/usr/bin/time", "-v", "-o", str(timing_path), *cmd],
            cwd=run_dir, stdout=stdout, stderr=stderr, start_new_session=True,
        )
        try:
            returncode = process.wait(timeout=args.timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    (run_dir / "logout.dat").unlink(missing_ok=True)
    elapsed = time.monotonic() - started
    if not timed_out and returncode != 0:
        raise RuntimeError(f"{case.name} failed with exit code {returncode}")

    pairs = parse_reaction_pairs(log) if not timed_out else pd.DataFrame()
    if not pairs.empty:
        pairs.insert(0, "case", case.name)
    pairs.to_csv(run_dir / "reaction_pairs.csv", index=False)
    marker = "TIMEOUT" if timed_out else "PASS"
    (run_dir / marker).write_text(marker + "\n", encoding="utf-8")
    timing = parse_time(timing_path)
    return {
        "case": case.name,
        "status": "timeout" if timed_out else "pass",
        "seed": seed,
        "positive_drop_rate": args.positive_drop_rate,
        "absolute_rto_override_us": args.rto_us,
        "matched_pairs": len(pairs),
        "p99_us": (
            float(np.quantile(pairs["detect_delay_ps"] * 1e-6, 0.99))
            if not pairs.empty else float("nan")
        ),
        "runtime_seconds": elapsed,
        "max_rss_kbytes": timing.get("Maximum resident set size (kbytes)", ""),
        "log_bytes": log.stat().st_size,
    }


def plot_multiseed(rows: list[dict[str, object]], output_root: Path,
                   positive_drop_rate: float, raw_root: Path | None = None) -> None:
    frame = pd.DataFrame(rows)
    mapping = {
        "nodrop_no_trim_rss": ("No corruption", "RTO"),
        "nodrop_yes_trim_rss": ("No corruption", "Trim"),
        "nodrop_pfld_rss": ("No corruption", "PFLD"),
        "yesdrop_no_trim_rss": ("Corruption", "RTO"),
        "yesdrop_yes_trim_rss": ("Corruption", "Trim"),
        "yesdrop_pfld_rss": ("Corruption", "PFLD"),
    }
    frame[["condition", "mechanism"]] = frame["case"].map(mapping).apply(pd.Series)
    per_seed = (
        frame.groupby(["condition", "mechanism"], sort=False)["p99_us"]
        .agg(["count", "mean", "std", "min", "max"])
        .reset_index()
    )
    per_seed["ci95_us"] = 1.96 * per_seed["std"].fillna(0) / np.sqrt(per_seed["count"])
    per_seed.to_csv(output_root / "per_seed_p99_summary.csv", index=False)

    source = raw_root or output_root
    pair_rows: list[dict[str, object]] = []
    for path in sorted((source / "runs").glob("seed*/*/reaction_pairs.csv")):
        case = path.parent.name
        seed = int(path.parent.parent.name.removeprefix("seed"))
        try:
            pairs = pd.read_csv(path)
        except EmptyDataError:
            continue
        if pairs.empty:
            continue
        for delay in pairs["detect_delay_ps"] * 1e-6:
            pair_rows.append({"case": case, "seed": seed, "delay_us": float(delay)})
    pair_frame = pd.DataFrame(pair_rows)
    if pair_frame.empty:
        raise RuntimeError("No matched reaction pairs are available for pooled P99 analysis")
    pair_frame[["condition", "mechanism"]] = pair_frame["case"].map(mapping).apply(pd.Series)

    rng = np.random.default_rng(20260713)
    pooled_rows: list[dict[str, object]] = []
    for (condition, mechanism), selected in pair_frame.groupby(["condition", "mechanism"], sort=False):
        by_seed = {
            int(seed): values["delay_us"].to_numpy()
            for seed, values in selected.groupby("seed")
        }
        seed_ids = np.array(sorted(by_seed))
        bootstrap = []
        for _ in range(2000):
            chosen = rng.choice(seed_ids, size=len(seed_ids), replace=True)
            sample = np.concatenate([by_seed[int(seed)] for seed in chosen])
            bootstrap.append(float(np.quantile(sample, 0.99)))
        estimate = float(np.quantile(selected["delay_us"], 0.99))
        pooled_rows.append({
            "condition": condition,
            "mechanism": mechanism,
            "seed_count_with_pairs": len(seed_ids),
            "matched_pairs": len(selected),
            "pooled_p99_us": estimate,
            "bootstrap_ci95_low_us": float(np.quantile(bootstrap, 0.025)),
            "bootstrap_ci95_high_us": float(np.quantile(bootstrap, 0.975)),
            "estimator": "P99 pooled across matched events; seed-stratified bootstrap CI",
        })
    aggregate = pd.DataFrame(pooled_rows)
    aggregate.to_csv(output_root / "aggregate_summary.csv", index=False)

    sns.set_theme(style="whitegrid", context="paper")
    figure, axis = plt.subplots(figsize=(3.2, 2.8))
    order = ["RTO", "Trim", "PFLD"]
    palette = ["#66c2a5", "#fc8d62", "#8da0cb"]
    centers = np.arange(2)
    width = 0.23
    for mechanism_index, (mechanism, color) in enumerate(zip(order, palette)):
        means = []
        errors = []
        counts = []
        for condition in ("No corruption", "Corruption"):
            values = aggregate[
                (aggregate["condition"] == condition)
                & (aggregate["mechanism"] == mechanism)
            ].iloc[0]
            means.append(float(values["pooled_p99_us"]))
            errors.append((
                float(values["pooled_p99_us"] - values["bootstrap_ci95_low_us"]),
                float(values["bootstrap_ci95_high_us"] - values["pooled_p99_us"]),
            ))
            counts.append(int(values["matched_pairs"]))
        positions = centers + (mechanism_index - 1) * width
        asymmetric_errors = np.array(errors).T
        label = "Trimming" if mechanism == "Trim" else mechanism
        axis.bar(positions, means, width, yerr=asymmetric_errors, capsize=3,
                 color=color, edgecolor="black", linewidth=0.7, label=label)
    axis.set_xticks(centers, ("No Corruption Drops", "With Corruption Drops"))
    axis.set_ylabel("P99 loss detection delay (us)")
    axis.legend(frameon=True, fontsize=7)
    axis.set_ylim(0, max(100.0, axis.get_ylim()[1] * 1.05))
    figure.tight_layout(pad=0.6)
    figure.savefig(output_root / "figure1b_multiseed.png", dpi=240, bbox_inches="tight")
    figure.savefig(output_root / "figure1b_multiseed.pdf", bbox_inches="tight")
    plt.close(figure)


def main() -> int:
    args = parse_args()
    if args.rto_us is not None and args.rto_us <= 0:
        raise SystemExit("--rto-us must be positive")
    seeds = expand_seeds(args.seeds, args.seed)
    if args.output_root.exists():
        raise SystemExit(f"Refusing existing output root: {args.output_root}")
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        raise SystemExit(f"Missing simulator binary: {args.binary}")
    args.output_root.mkdir(parents=True)
    provenance = {
        "paper_element": "Figure 1B",
        "positive_drop_rate": args.positive_drop_rate,
        "binary": str(args.binary),
        "seeds": seeds,
        "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
    }
    (args.output_root / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    if args.raw_source_root is not None:
        source = args.raw_source_root.resolve()
        summary = pd.read_csv(source / "summary.csv")
        summary.to_csv(args.output_root / "summary.csv", index=False)
        provenance["raw_source_root"] = str(source)
        (args.output_root / "provenance.json").write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        plot_multiseed(summary.to_dict("records"), args.output_root,
                       args.positive_drop_rate, raw_root=source)
        print(f"Replotted Figure 1B from {source}")
        return 0

    tasks = [(case, seed) for seed in seeds for case in CASES]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        rows = list(executor.map(lambda item: run_one(item[0], item[1], args), tasks))
    rows.sort(key=lambda row: str(row["case"]))
    with (args.output_root / "summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=list(rows[0]), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)

    plot_multiseed(rows, args.output_root, args.positive_drop_rate)
    print(pd.DataFrame(rows).to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
