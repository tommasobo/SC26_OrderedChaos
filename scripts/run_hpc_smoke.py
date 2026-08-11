#!/usr/bin/env python3
import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple
from urllib.request import urlretrieve


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run a small ATLAHS HPC smoke validation on htsim.")
    p.add_argument("--app", default="lammps", help="HPC app folder name under traces/hpc.")
    p.add_argument("--workload", default="lammps_8", help="Workload folder and file prefix.")
    p.add_argument("--algos", default="ecmp,rss,reps", help="Comma-separated load-balancing algos.")
    p.add_argument("--drop-rates", default="0", help="Comma-separated switch_random_drop_prob values.")
    p.add_argument("--trim-modes", default="off", help="Comma-separated modes: on,off.")
    p.add_argument("--seeds", default="1", help="Comma-separated integer seeds.")
    p.add_argument("--trace-root", default="data/hpc", help="Local trace root directory.")
    p.add_argument("--output-dir", default="nsdi_results/hpc_smoke", help="Output root directory.")
    p.add_argument("--linkspeed", type=int, default=400000, help="Link speed in Mbps.")
    p.add_argument("--topo-size", type=int, default=16, help="Number of nodes in topology.")
    p.add_argument("--topo-file", default="", help="Override topology file path.")
    p.add_argument("--q", type=int, default=151, help="Queue size in packets.")
    p.add_argument("--cwnd", type=int, default=151, help="Initial cwnd in packets.")
    p.add_argument("--ecn-min", type=int, default=38, help="ECN marking minimum threshold in packets.")
    p.add_argument("--ecn-max", type=int, default=113, help="ECN marking maximum threshold in packets.")
    p.add_argument("--end", type=int, default=10000, help="Simulation end time in us.")
    p.add_argument("--seed", type=int, default=1, help="Random seed.")
    p.add_argument("--force-finish", type=int, default=200, help="Finish after N completed flows.")
    p.add_argument("--rss-num-flows", type=int, default=8, help="RSS flow count parameter.")
    p.add_argument(
        "--compute-time-override",
        type=int,
        default=None,
        help="Override trace local-compute ops to a fixed duration in ns (e.g. 0 or 1).",
    )
    p.add_argument(
        "--replace-algos",
        default="",
        help="Comma-separated algo names to re-run. Reads existing summary.csv, "
             "removes matching rows, runs only these algos, and merges back. "
             "When set, --algos is ignored and only the specified algos are executed.",
    )
    return p.parse_args()


def parse_max_host_time(log_path: Path) -> int:
    host_pattern = re.compile(r"^Host\s+\d+:\s+(\d+)$")
    max_pattern = re.compile(r"^Maximum finishing time at host \d+:\s+(\d+)")
    max_t = -1
    with log_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            stripped = line.strip()
            m = host_pattern.match(stripped)
            if m:
                max_t = max(max_t, int(m.group(1)))
                continue
            m = max_pattern.match(stripped)
            if m:
                max_t = max(max_t, int(m.group(1)))
    return max_t


def resolve_algo(token: str) -> Tuple[str, List[str]]:
    if token in {"rps", "oblivious_ps"}:
        return "oblivious", []
    if token == "pfld":
        return "rss", [
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_probe", "0",
            "-pflr_proactive_rtx_probe",
            "-no_droping_low_header",
        ]
    if token == "rss_rack":
        return "rss", ["-rack_tlp", "1"]
    if token == "rss_tlp":
        return "rss", ["-rack_tlp", "4"]
    if token == "rss_rack_tlp":
        return "rss", ["-rack_tlp", "2"]
    if token in {"pfld_rack", "pfld_rack_tlp", "rss_pfld_rack_tlp"}:
        return "rss", [
            "-rack_tlp", "2",
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_probe", "0",
            "-pflr_proactive_rtx_probe",
            "-no_droping_low_header",
        ]
    if token == "pfld_p1":
        return "rss", [
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_probe", "1",
            "-pflr_proactive_rtx_probe",
            "-no_droping_low_header",
        ]
    if token == "pfld_paced":
        return "rss", [
            "-precisefastlossrecovery", "3",
            "-pflr_proactive_probe", "0",
            "-pflr_proactive_rtx_probe",
            "-no_droping_low_header",
            "-pflr_pace_rtx",
        ]
    if token in {"rack", "ecmp_rack"}:
        return "ecmp", ["-rack_tlp", "1"]
    if token in {"rack_tlp", "ecmp_rack_tlp"}:
        return "ecmp", ["-rack_tlp", "2"]
    if token in {"rack_tlp_no6675", "ecmp_rack_tlp_no6675"}:
        return "ecmp", ["-rack_tlp", "3"]
    return token, []


def percentile(values: List[float], q: float) -> float:
    if not values:
        return -1.0
    if len(values) == 1:
        return values[0]
    s = sorted(values)
    pos = (len(s) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(s) - 1)
    if lo == hi:
        return s[lo]
    frac = pos - lo
    return s[lo] + (s[hi] - s[lo]) * frac


def parse_flow_metrics(log_path: Path) -> Dict[str, float]:
    flow_re = re.compile(r"finished at\s+([0-9.]+).*?total bytes\s+(\d+)")
    fcts_all: List[float] = []
    bins: Dict[str, List[float]] = {"small": [], "medium": [], "large": []}
    with log_path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = flow_re.search(line)
            if not m:
                continue
            fct_us = float(m.group(1))
            size_b = int(m.group(2))
            fcts_all.append(fct_us)
            if size_b <= 10 * 1024:
                bins["small"].append(fct_us)
            elif size_b <= 100 * 1024:
                bins["medium"].append(fct_us)
            else:
                bins["large"].append(fct_us)
    out: Dict[str, float] = {
        "flow_count": float(len(fcts_all)),
        "fct_median_us": percentile(fcts_all, 0.5),
        "fct_p99_us": percentile(fcts_all, 0.99),
        "small_count": float(len(bins["small"])),
        "small_p99_us": percentile(bins["small"], 0.99),
        "medium_count": float(len(bins["medium"])),
        "medium_p99_us": percentile(bins["medium"], 0.99),
        "large_count": float(len(bins["large"])),
        "large_p99_us": percentile(bins["large"], 0.99),
    }
    return out


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    htsim_bin = repo_root / "build" / "htsim_uec"
    if not htsim_bin.exists():
        print(f"[ERROR] Missing simulator binary: {htsim_bin}", file=sys.stderr)
        return 1

    speed_gbps = args.linkspeed // 1000
    topo_file = Path(args.topo_file) if args.topo_file else repo_root / "scripts" / "topologies" / f"{args.topo_size}_{speed_gbps}.topo"
    if not topo_file.exists():
        print(f"[ERROR] Missing topology file: {topo_file}", file=sys.stderr)
        return 1

    trace_dir = repo_root / args.trace_root / args.app / args.workload
    trace_dir.mkdir(parents=True, exist_ok=True)
    goal_bin = trace_dir / f"{args.workload}.bin"
    if not goal_bin.exists():
        url = f"http://storage2.spcl.ethz.ch/traces/hpc/{args.app}/{args.workload}/{args.workload}.bin"
        print(f"[INFO] Downloading {url} -> {goal_bin}")
        urlretrieve(url, goal_bin)

    out_dir = repo_root / args.output_dir / args.workload
    out_dir.mkdir(parents=True, exist_ok=True)

    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    drop_rates = [float(x.strip()) for x in args.drop_rates.split(",") if x.strip()]
    trim_modes = [x.strip().lower() for x in args.trim_modes.split(",") if x.strip()]
    seeds = [int(x.strip()) for x in args.seeds.split(",") if x.strip()]

    # --replace-algos: read existing summary, strip matching rows, re-run only those algos
    replace_set = set()
    existing_rows: List[Tuple] = []
    CANONICAL_HEADER = [
        "app", "workload", "algo", "drop_rate", "trim_mode", "seed", "compute_time_override_ns",
        "exit_code", "max_host_time_ns",
        "flow_count", "fct_median_us", "fct_p99_us",
        "small_count", "small_p99_us",
        "medium_count", "medium_p99_us",
        "large_count", "large_p99_us",
        "log_file",
    ]
    if args.replace_algos:
        replace_set = {a.strip() for a in args.replace_algos.split(",") if a.strip()}
        algos = sorted(replace_set)
        summary_path = out_dir / "summary.csv"
        if summary_path.exists():
            with summary_path.open("r", newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                old_header = reader.fieldnames or []
                for drow in reader:
                    if drow.get("algo") in replace_set:
                        continue
                    # Re-emit row in canonical column order, filling missing cols
                    existing_rows.append(tuple(
                        drow.get(col, "") for col in CANONICAL_HEADER
                    ))
            print(f"[INFO] --replace-algos: kept {len(existing_rows)} existing rows, will re-run {replace_set}")
        else:
            print(f"[WARN] No existing summary.csv at {summary_path}; running from scratch")

    for mode in trim_modes:
        if mode not in {"on", "off"}:
            print(f"[ERROR] Invalid trim mode: {mode}. Use on/off.", file=sys.stderr)
            return 1

    rows: List[Tuple] = []
    for algo in algos:
        for drop_rate in drop_rates:
            for trim_mode in trim_modes:
                for seed in seeds:
                    suffix = f"{algo}_drop{drop_rate:g}_trim{trim_mode}_seed{seed}"
                    if args.compute_time_override is not None:
                        suffix += f"_compute{args.compute_time_override}"
                    log_path = out_dir / f"{suffix}.log"
                    lb_algo, extra_algo_args = resolve_algo(algo)
                    cmd = [
                        str(htsim_bin),
                        "-goal",
                        str(goal_bin),
                        "-topo",
                        str(topo_file),
                        "-q",
                        str(args.q),
                        "-cwnd",
                        str(args.cwnd),
                        "-mtu",
                        "4160",
                        "-sack_threshold",
                        "0",
                        "-ecn",
                        str(args.ecn_min),
                        str(args.ecn_max),
                        "-switch_random_drop_prob",
                        str(drop_rate),
                        "-fail_psn",
                        "-1",
                        "-load_balancing_algo",
                        lb_algo,
                        "-linkspeed",
                        str(args.linkspeed),
                        "-rss_parameters",
                        "mean_rtt",
                        str(args.rss_num_flows),
                        "15",
                        "0",
                        "0",
                        "25",
                        "-sender_cc_only",
                        "-sender_cc_algo",
                        "dctcp",
                        "-nodes",
                        str(args.topo_size),
                        "-end",
                        str(args.end),
                        "-seed",
                        str(seed),
                        "-force_finish",
                        str(args.force_finish),
                    ]
                    if args.compute_time_override is not None:
                        cmd += ["-lgs_compute_time_override", str(args.compute_time_override)]
                    if trim_mode == "off":
                        cmd.append("-disable_trim")
                    cmd += extra_algo_args
                    print(f"[INFO] Running {suffix}: {' '.join(cmd)}")
                    with log_path.open("w", encoding="utf-8") as lf:
                        rc = subprocess.run(cmd, cwd=repo_root, stdout=lf, stderr=subprocess.STDOUT).returncode
                    max_host_time = parse_max_host_time(log_path) if rc == 0 else -1
                    metrics = parse_flow_metrics(log_path) if rc == 0 else {
                        "flow_count": -1.0,
                        "fct_median_us": -1.0,
                        "fct_p99_us": -1.0,
                        "small_count": -1.0,
                        "small_p99_us": -1.0,
                        "medium_count": -1.0,
                        "medium_p99_us": -1.0,
                        "large_count": -1.0,
                        "large_p99_us": -1.0,
                    }
                    rows.append((
                        args.app, args.workload, algo, drop_rate, trim_mode, seed, args.compute_time_override,
                        rc, max_host_time,
                        int(metrics["flow_count"]), metrics["fct_median_us"], metrics["fct_p99_us"],
                        int(metrics["small_count"]), metrics["small_p99_us"],
                        int(metrics["medium_count"]), metrics["medium_p99_us"],
                        int(metrics["large_count"]), metrics["large_p99_us"],
                        str(log_path),
                    ))

    summary_path = out_dir / "summary.csv"
    # Merge with existing rows when using --replace-algos
    all_rows = list(existing_rows) + rows if replace_set else rows
    with summary_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow([
            "app", "workload", "algo", "drop_rate", "trim_mode", "seed", "compute_time_override_ns",
            "exit_code", "max_host_time_ns",
            "flow_count", "fct_median_us", "fct_p99_us",
            "small_count", "small_p99_us",
            "medium_count", "medium_p99_us",
            "large_count", "large_p99_us",
            "log_file",
        ])
        w.writerows(all_rows)
    print(f"[INFO] Summary written to {summary_path}")
    for row in rows:
        print(
            "[RESULT] "
            f"algo={row[2]} drop={row[3]} trim={row[4]} seed={row[5]} compute={row[6]} "
            f"rc={row[7]} max_host_time_ns={row[8]} "
            f"flow_count={row[9]} fct_p99_us={row[11]}"
        )
    return 0 if all(row[7] == 0 for row in rows) else 2


if __name__ == "__main__":
    raise SystemExit(main())
