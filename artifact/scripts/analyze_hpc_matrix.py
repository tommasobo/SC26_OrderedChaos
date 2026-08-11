#!/usr/bin/env python3
"""Convert fresh Figure 12 matrix logs into paper metrics and plots."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
if str(REPO / "scripts") not in sys.path:
    sys.path.insert(0, str(REPO / "scripts"))

from run_hpc_smoke import parse_flow_metrics, parse_max_host_time


WORKLOADS = ("icon_32", "icon_64", "lammps_32", "lammps_64", "milc_32", "milc_64")
METHODS = ("ecmp", "oblivious", "reps", "rss", "pfld", "rss_rack_tlp")
HEADER = [
    "app", "workload", "algo", "drop_rate", "trim_mode", "seed",
    "compute_time_override_ns", "exit_code", "max_host_time_ns", "flow_count",
    "fct_median_us", "fct_p99_us", "small_count", "small_p99_us",
    "medium_count", "medium_p99_us", "large_count", "large_p99_us", "log_file",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-root", type=Path, required=True)
    parser.add_argument(
        "--extra-raw-root", type=Path, default=None,
        help="Optional second root containing complementary methods for the same matrix.",
    )
    parser.add_argument(
        "--variant",
        choices=("paper_configuration",),
        required=True,
    )
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--comm-share-csv", type=Path, required=True)
    parser.add_argument("--allow-failed", action="store_true", help="Preserve failed rows as explicit gaps.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.output_root.exists():
        raise SystemExit(f"Refusing existing analysis output root: {args.output_root}")
    compat_root = args.output_root / "compat_summaries"
    plot_root = args.output_root / "plots"
    compat_root.mkdir(parents=True)

    detected_drop: float | None = None
    incomplete_rows: list[dict[str, object]] = []
    for workload in WORKLOADS:
        result = f"figure12_{workload}"
        roots = [args.raw_root]
        if args.extra_raw_root is not None:
            roots.append(args.extra_raw_root)
        rows: list[dict[str, str]] = []
        for source_root in roots:
            run_root = source_root / result / args.variant
            status_path = run_root / "run_status.csv"
            if not status_path.is_file():
                raise FileNotFoundError(status_path)
            for row in csv.DictReader(status_path.open(encoding="utf-8")):
                row["_source_root"] = str(source_root)
                rows.append(row)
        if len(rows) != len(METHODS):
            raise RuntimeError(f"Expected six rows for {result}, found {len(rows)}")
        failed = [row for row in rows if row["status"] not in {"pass", "resumed-pass"}]
        if failed and not args.allow_failed:
            raise RuntimeError(f"Failed rows in {status_path}: {failed}")
        for row in failed:
            incomplete_rows.append({
                "workload": workload,
                "algorithm": row["algorithm"],
                "status": row["status"],
                "exit_code": row["exit_code"],
                "runtime_seconds": row["runtime_seconds"],
                "log_bytes": row["log_bytes"],
            })
        rows = [row for row in rows if row["status"] in {"pass", "resumed-pass"}]
        output_rows: list[list[object]] = []
        for row in rows:
            if row["algorithm"] not in METHODS:
                raise RuntimeError(f"Unexpected method in {status_path}: {row['algorithm']}")
            drop = float(row["drop_rate"])
            if detected_drop is None:
                detected_drop = drop
            elif abs(detected_drop - drop) > 1e-15:
                raise RuntimeError("Mixed drop rates in a single Figure 12 analysis")
            run_root = Path(row["_source_root"]) / result / args.variant
            log = run_root / f"drop_{drop:.10g}" / f"{row['algorithm']}_seed{row['seed']}_trim_{row['trim_mode']}.txt"
            metrics = parse_flow_metrics(log)
            max_host_time = parse_max_host_time(log)
            if max_host_time <= 0 or metrics["flow_count"] <= 0:
                raise RuntimeError(f"Incomplete application/flow metrics in {log}")
            output_rows.append([
                workload.rsplit("_", 1)[0], workload, row["algorithm"], drop,
                row["trim_mode"], int(row["seed"]), "", 0, max_host_time,
                int(metrics["flow_count"]), metrics["fct_median_us"], metrics["fct_p99_us"],
                int(metrics["small_count"]), metrics["small_p99_us"],
                int(metrics["medium_count"]), metrics["medium_p99_us"],
                int(metrics["large_count"]), metrics["large_p99_us"], str(log.resolve()),
            ])
        workload_dir = compat_root / workload
        workload_dir.mkdir()
        with (workload_dir / "summary.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle, lineterminator="\n")
            writer.writerow(HEADER)
            writer.writerows(output_rows)

    assert detected_drop is not None
    with (args.output_root / "incomplete_rows.csv").open("w", newline="", encoding="utf-8") as handle:
        fields = ["workload", "algorithm", "status", "exit_code", "runtime_seconds", "log_bytes"]
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(incomplete_rows)
    (args.output_root / "analysis_provenance.json").write_text(
        json.dumps({"variant": args.variant, "allow_failed": args.allow_failed,
                    "raw_root": str(args.raw_root),
                    "extra_raw_root": str(args.extra_raw_root) if args.extra_raw_root else None,
                    "incomplete_rows": incomplete_rows}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    env = os.environ | {"MPLBACKEND": "Agg"}
    methods = ",".join(METHODS)
    workloads = ",".join(WORKLOADS)
    subprocess.run([
        sys.executable, str(REPO / "scripts/plot_hpc_full_paper_metrics.py"),
        "--root", str(compat_root), "--extra-roots", "", "--outdir", str(plot_root),
        "--methods", methods, "--workloads", workloads,
        "--figure-context", f"Fresh {args.variant.replace('_', ' ')} Figure 12",
        "--drop-rate", f"{detected_drop:.10g}",
        *(["--allow-missing"] if args.allow_failed else []),
    ], cwd=REPO, env=env, check=True)
    subprocess.run([
        sys.executable, str(REPO / "scripts/plot_hpc_speedup_with_comm.py"),
        "--metrics-csv", str(plot_root / "hpc_full_run_paper_metrics.csv"),
        "--comm-share-csv", str(args.comm_share_csv), "--outdir", str(plot_root),
        "--methods", methods, "--workloads", workloads, "--variants", "wide",
        "--title", "",
        *(["--allow-missing"] if args.allow_failed else []),
    ], cwd=REPO, env=env, check=True)
    print(f"Wrote Figure 12 analysis to {args.output_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
