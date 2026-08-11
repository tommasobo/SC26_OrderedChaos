#!/usr/bin/env python3
"""Fresh Figure 13 RSS-sensitivity matrix."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import re
import shlex
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns


REPO = Path(__file__).resolve().parents[2]
FLOWS = (4, 8, 16, 32)
UPDATES = (15, 30, 60, 120)
VARIANTS = {
    "paper_symmetric": {"seed": 1, "nodes": 64, "failed": 0, "ratio": 0.2, "bidirectional": False},
    "paper_asymmetric": {"seed": 25, "nodes": 128, "failed": 1, "ratio": 0.2, "bidirectional": False},
}


@dataclass(frozen=True)
class Task:
    subflows: int
    update_us: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=tuple(VARIANTS), required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=REPO / "build/htsim_uec")
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--seed", type=int, default=None,
                        help="Override the configured seed.")
    parser.add_argument("--drop-rate", type=float, default=0.0,
                        help="Per-switch corruption probability.")
    parser.add_argument("--rto-us", type=float, default=None,
                        help="Use an exact positive RTO in microseconds instead of the configured ratio.")
    return parser.parse_args()


def parse_timing(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key.strip()] = value.strip()
    return result


def run_one(task: Task, args: argparse.Namespace, config: dict[str, float | int]) -> dict[str, object]:
    slug = f"subflows{task.subflows}_update{task.update_us}"
    run_dir = args.output_root / "runs" / slug
    metrics = run_dir / "metrics"
    metrics.mkdir(parents=True, exist_ok=False)
    log = args.output_root / f"{slug}.out"
    timing_path = run_dir / "time.txt"
    cmd = [
        str(args.binary),
        "-data_collection_config", str(REPO / "scripts/metrics_collection_policies/collect_default.json"),
        "-end", "10000", "-seed", str(config["seed"]),
        "-tm", str(REPO / "scripts/connection_matrices/tornado_128_8000000.cm"),
        "-topo", str(REPO / "scripts/topologies/128_400.topo"),
        "-q", "148", "-cwnd", "148", "-mtu", "4160", "-sack_threshold", "0",
        "-ecn", "29", "118", "-switch_random_drop_prob", f"{args.drop_rate:.10g}", "-fail_psn", "-1",
        "-load_balancing_algo", "rss", "-linkspeed", "400000",
        "-rss_parameters", "mean_rtt", str(task.subflows), str(task.update_us), "0", "0", "25",
        "-sender_cc_only", "-sender_cc_algo", "dctcp", "-data_collection_dir", str(metrics),
        "-nodes", str(config["nodes"]), "-disable_trim", "-precisefastlossrecovery", "3",
        "-pflr_proactive_probe", "-1", "-no_droping_low_header",
        "-failed", str(config["failed"]), "-failed_link_ratio", str(config["ratio"]),
    ]
    rto_index = cmd.index("-load_balancing_algo")
    if args.rto_us is None:
        cmd[rto_index:rto_index] = ["-rto_ratio", "1"]
    else:
        cmd[rto_index:rto_index] = ["-rto_us", str(args.rto_us)]
    if config["bidirectional"]:
        cmd.append("-failed_bidirectional")
    (run_dir / "command.txt").write_text(shlex.join(cmd) + "\n", encoding="utf-8")
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as output:
        completed = subprocess.run(
            ["/usr/bin/time", "-v", "-o", str(timing_path), *cmd],
            cwd=run_dir, stdout=output, stderr=subprocess.STDOUT,
        )
    (run_dir / "logout.dat").unlink(missing_ok=True)
    elapsed = time.monotonic() - started
    if completed.returncode != 0:
        raise RuntimeError(f"{slug} failed with exit code {completed.returncode}")
    values = [float(value) for value in re.findall(
        r"finished at\s+([0-9]*\.?[0-9]+)", log.read_text(encoding="utf-8", errors="replace")
    )]
    if not values:
        raise RuntimeError(f"{slug} has no completion records")
    (run_dir / "PASS").write_text("PASS\n", encoding="utf-8")
    timing = parse_timing(timing_path)
    return {
        "variant": args.variant,
        "seed": config["seed"],
        "drop_rate": args.drop_rate,
        "subflows": task.subflows,
        "update_us": task.update_us,
        "update_rtt_ratio": task.update_us / 15.0,
        "max_fct_us": max(values),
        "flow_count": len(values),
        "runtime_seconds": elapsed,
        "max_rss_kbytes": timing.get("Maximum resident set size (kbytes)", ""),
        "log_bytes": log.stat().st_size,
    }


def main() -> int:
    args = parse_args()
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        raise SystemExit(f"Missing simulator binary: {args.binary}")
    if args.output_root.exists():
        raise SystemExit(f"Refusing existing output root: {args.output_root}")
    if args.workers <= 0:
        raise SystemExit("--workers must be positive")
    if args.seed is not None and args.seed <= 0:
        raise SystemExit("--seed must be positive")
    if not 0 <= args.drop_rate <= 1:
        raise SystemExit("--drop-rate must be between zero and one")
    if args.rto_us is not None and args.rto_us <= 0:
        raise SystemExit("--rto-us must be positive")
    args.output_root.mkdir(parents=True)
    config = dict(VARIANTS[args.variant])
    if args.seed is not None:
        config["seed"] = args.seed
    provenance = {
        "paper_element": "Figure 13",
        "variant": args.variant,
        "configuration": config,
        "absolute_rto_override_us": args.rto_us,
        "binary": str(args.binary),
        "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
    }
    (args.output_root / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    tasks = [Task(subflows, update) for subflows in FLOWS for update in UPDATES]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        rows = list(executor.map(lambda task: run_one(task, args, config), tasks))
    rows.sort(key=lambda row: (int(row["subflows"]), int(row["update_us"])))
    with (args.output_root / "sensitivity_long.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=list(rows[0]), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)
    frame = pd.DataFrame(rows).pivot(index="subflows", columns="update_us", values="max_fct_us")
    frame.to_csv(args.output_root / "sensitivity_matrix.csv")

    sns.set_theme(style="whitegrid", context="talk")
    fig, ax = plt.subplots(figsize=(5.5, 3.75))
    annotations = frame.round(0).map(lambda value: f"{int(value):d}")
    sns.heatmap(frame, annot=annotations, fmt="s", cmap="rocket_r", linewidths=0.5,
                linecolor="white", square=True, alpha=0.92,
                cbar_kws={"label": "Max FCT (us)"}, ax=ax)
    ax.set_xlabel("Update Period tau (RTT Ratio)")
    ax.set_ylabel("No. of Subflows")
    ax.set_xticklabels([1, 2, 4, 8])
    fig.tight_layout()
    fig.savefig(args.output_root / "sensitivity_heatmap.pdf", bbox_inches="tight")
    fig.savefig(args.output_root / "sensitivity_heatmap.png", dpi=220, bbox_inches="tight")
    plt.close(fig)
    print(pd.DataFrame(rows).to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
