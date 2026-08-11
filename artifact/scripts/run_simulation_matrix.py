#!/usr/bin/env python3
"""Execute a fresh OrderedChaos simulation matrix and record per-run resources."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
import signal
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO / "artifact" / "config" / "paper_experiments.json"
TIME_BIN = Path("/usr/bin/time")


@dataclass(frozen=True)
class Task:
    result: str
    variant: str
    algorithm: str
    seed: int
    drop_rate: float
    trim_mode: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, help="Result key from paper_experiments.json.")
    parser.add_argument("--variant", required=True, help="Configuration variant under the result.")
    parser.add_argument("--output-root", type=Path, required=True, help="Fresh raw-output root, preferably on scratch.")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--binary", type=Path, default=None,
                        help="Simulator executable; default is build/htsim_uec.")
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--seeds", default="", help="Override seeds, e.g. 1,3-5. Empty uses the manifest.")
    parser.add_argument("--algorithms", default="", help="Optional comma-separated subset.")
    parser.add_argument(
        "--rto-us", type=float, default=None,
        help="Override the manifest ratio with an exact positive RTO in microseconds.",
    )
    parser.add_argument(
        "--traffic-matrix", type=Path, default=None,
        help="Override the manifest traffic-matrix input for a provenance-tracked ablation.",
    )
    parser.add_argument("--resume", action="store_true", help="Skip only runs with a prior PASS marker in this same run root.")
    parser.add_argument(
        "--timeout-seconds", type=float, default=None,
        help="Bound each simulator task. Timed-out tasks remain explicit failed rows.",
    )
    return parser.parse_args()


def expand_seed_spec(spec: str, default_count: int) -> list[int]:
    if not spec:
        return list(range(1, default_count + 1))
    values: set[int] = set()
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            lo_text, hi_text = token.split("-", 1)
            lo, hi = int(lo_text), int(hi_text)
            if lo <= 0 or hi < lo:
                raise ValueError(f"Invalid seed range: {token}")
            values.update(range(lo, hi + 1))
        else:
            value = int(token)
            if value <= 0:
                raise ValueError("Seeds must be positive integers")
            values.add(value)
    return sorted(values)


def resolve_path(value: str) -> Path:
    expanded = Path(os.path.expandvars(value)).expanduser()
    return expanded if expanded.is_absolute() else REPO / expanded


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: Path, result_key: str, variant_key: str) -> tuple[dict, dict, dict]:
    with path.open("r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    try:
        result = manifest["results"][result_key]
        variant = result["variants"][variant_key]
    except KeyError as exc:
        raise SystemExit(f"Unknown result/variant: {result_key}/{variant_key} ({exc})") from exc
    return manifest, result, variant


def task_slug(task: Task) -> str:
    drop = f"{task.drop_rate:.10g}"
    return f"{task.algorithm}_seed{task.seed}_trim_{task.trim_mode}_drop_{drop}"


def build_command(result: dict, variant: dict, profile: dict, task: Task,
                  metrics_dir: Path, binary: Path | None = None) -> list[str]:
    params = dict(result["parameters"])
    params.update(variant.get("parameter_overrides", {}))
    rss_subflows = int(variant.get("rss_subflows", params.get("rss_subflows", 16)))
    input_flag = "-tm" if result["input_kind"] == "tm" else "-goal"
    command = [str(binary or REPO / "build" / "htsim_uec")]
    if variant.get("collect_metrics", True):
        command.extend([
            "-data_collection_config",
            str(REPO / "scripts" / "metrics_collection_policies" / "collect_default.json"),
        ])
    command.extend([
        "-end", str(params.get("end_ms", params.get("end_us", 10000))),
        "-seed", str(task.seed),
        input_flag, str(resolve_path(variant.get("input", result["input"]))),
        "-topo", str(resolve_path(variant.get("topology", result["topology"]))),
        "-q", str(params["q"]),
        "-cwnd", str(params["cwnd"]),
        "-mtu", str(params.get("mtu_bytes", 4160)),
        "-sack_threshold", "0",
        "-ecn", str(params["ecn_min"]), str(params["ecn_max"]),
        "-switch_random_drop_prob", str(task.drop_rate),
        "-fail_psn", "-1",
        "-load_balancing_algo", profile["load_balancing_algo"],
        "-linkspeed", str(params["linkspeed_mbps"]),
        "-rss_parameters", "mean_rtt", str(rss_subflows), str(params["rss_update_us"]), "0", "0", "25",
        "-sender_cc_only", "-sender_cc_algo", params.get("sender_cc_algo", "dctcp"),
        "-nodes", str(variant.get("nodes", result["nodes"])),
    ])
    if params.get("rto_us") is not None:
        command.extend(["-rto_us", str(params["rto_us"])])
    else:
        command.extend(["-rto_ratio", str(params["rto_ratio"])])
    if variant.get("collect_metrics", True):
        command.extend(["-data_collection_dir", str(metrics_dir)])
    if params.get("lgs_percent") is not None:
        command.extend(["-lgs_percent", str(params["lgs_percent"])])
    if task.trim_mode == "off":
        command.append("-disable_trim")
    command.extend(profile["extra_args"])
    return command


def parse_time_file(path: Path) -> dict[str, str]:
    parsed: dict[str, str] = {}
    if not path.exists():
        return parsed
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if ":" not in line:
                continue
            key, value = line.strip().split(":", 1)
            parsed[key.strip()] = value.strip()
    return parsed


def elapsed_seconds_from_time_file(path: Path) -> str:
    """Recover GNU time's elapsed value when a successful task is resumed."""
    if not path.exists():
        return ""
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "Elapsed (wall clock) time" not in line:
            continue
        value = line.rsplit(": ", 1)[-1].strip()
        parts = value.split(":")
        try:
            if len(parts) == 2:
                return f"{int(parts[0]) * 60 + float(parts[1]):.6f}"
            if len(parts) == 3:
                return f"{int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2]):.6f}"
        except ValueError:
            return ""
    return ""


def run_one(task: Task, result: dict, variant: dict, profile: dict, run_root: Path,
            resume: bool, binary: Path | None = None,
            timeout_seconds: float | None = None) -> dict[str, object]:
    slug = task_slug(task)
    drop_dir = run_root / f"drop_{task.drop_rate:.10g}"
    run_dir = run_root / "runs" / slug
    metrics_dir = run_dir / "metrics"
    marker = run_dir / "PASS"
    log_path = drop_dir / f"{task.algorithm}_seed{task.seed}_trim_{task.trim_mode}.txt"
    time_path = run_dir / "time.txt"
    command_path = run_dir / "command.txt"
    command = build_command(result, variant, profile, task, metrics_dir, binary)
    command_text = shlex.join(command)

    if resume and marker.exists() and log_path.exists():
        if not command_path.is_file() or command_path.read_text(encoding="utf-8").strip() != command_text:
            raise RuntimeError(f"Refusing to resume PASS output under a changed command: {run_dir}")
        timing = parse_time_file(time_path)
        return {"result": task.result, "variant": task.variant, "algorithm": task.algorithm, "seed": task.seed, "drop_rate": task.drop_rate, "trim_mode": task.trim_mode, "status": "resumed-pass", "exit_code": 0, "runtime_seconds": elapsed_seconds_from_time_file(time_path), "max_rss_kbytes": timing.get("Maximum resident set size (kbytes)", ""), "log_bytes": log_path.stat().st_size, "command_sha256": hashlib.sha256(command_text.encode("utf-8")).hexdigest()}
    if run_dir.exists() or log_path.exists():
        raise RuntimeError(f"Refusing to reuse incomplete or untracked output: {run_dir}")

    drop_dir.mkdir(parents=True, exist_ok=True)
    run_dir.mkdir(parents=True, exist_ok=False)
    if variant.get("collect_metrics", True):
        metrics_dir.mkdir()
    command_path.write_text(command_text + "\n", encoding="utf-8")

    timed_command = [str(TIME_BIN), "-v", "-o", str(time_path), *command]
    start = time.monotonic()
    timed_out = False
    with log_path.open("w", encoding="utf-8") as log_handle:
        process = subprocess.Popen(
            timed_command, cwd=run_dir, stdout=log_handle, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            return_code = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            return_code = 124
    # The simulator's auxiliary log duplicates retained stdout and can be very large.
    (run_dir / "logout.dat").unlink(missing_ok=True)
    elapsed = time.monotonic() - start
    timing = parse_time_file(time_path)
    status = "timeout" if timed_out else (
        "pass" if return_code == 0 and log_path.stat().st_size > 0 else "fail"
    )
    if status == "pass":
        marker.write_text("PASS\n", encoding="utf-8")
    return {
        "result": task.result,
        "variant": task.variant,
        "algorithm": task.algorithm,
        "seed": task.seed,
        "drop_rate": task.drop_rate,
        "trim_mode": task.trim_mode,
        "status": status,
        "exit_code": return_code,
        "runtime_seconds": f"{elapsed:.6f}",
        "max_rss_kbytes": timing.get("Maximum resident set size (kbytes)", ""),
        "log_bytes": log_path.stat().st_size,
        "command_sha256": hashlib.sha256(command_text.encode("utf-8")).hexdigest(),
    }


def main() -> int:
    args = parse_args()
    if args.workers <= 0:
        raise SystemExit("--workers must be positive")
    if args.timeout_seconds is not None and args.timeout_seconds <= 0:
        raise SystemExit("--timeout-seconds must be positive")
    manifest, result, variant = load_manifest(args.config, args.result, args.variant)
    if args.rto_us is not None:
        if args.rto_us <= 0:
            raise SystemExit("--rto-us must be positive")
        variant = dict(variant)
        overrides = dict(variant.get("parameter_overrides", {}))
        overrides["rto_us"] = args.rto_us
        variant["parameter_overrides"] = overrides
    if args.traffic_matrix is not None:
        if result["input_kind"] != "tm":
            raise SystemExit("--traffic-matrix is valid only for traffic-matrix experiments")
        variant = dict(variant)
        variant["input"] = str(args.traffic_matrix.resolve())
    binary = (args.binary or REPO / "build" / "htsim_uec").resolve()
    if not binary.is_file():
        raise SystemExit(f"Missing simulator binary: {binary}")
    input_path = resolve_path(variant.get("input", result["input"])).resolve()
    topology_path = resolve_path(variant.get("topology", result["topology"])).resolve()
    for required in (input_path, topology_path):
        if not required.is_file():
            raise SystemExit(f"Missing required input: {required}")

    configured_algorithms = list(variant["algorithms"])
    if args.algorithms:
        requested = [token.strip() for token in args.algorithms.split(",") if token.strip()]
        unknown = sorted(set(requested) - set(configured_algorithms))
        if unknown:
            raise SystemExit("Algorithms are not part of this result variant: " + ", ".join(unknown))
        algorithms = requested
    else:
        algorithms = configured_algorithms
    profiles = {
        name: {
            **profile,
            "extra_args": list(profile.get("extra_args", [])),
        }
        for name, profile in manifest["algorithm_profiles"].items()
    }
    for name, override in variant.get("algorithm_profile_overrides", {}).items():
        if name not in profiles:
            raise SystemExit(f"Cannot override unknown algorithm profile: {name}")
        profiles[name] = {**profiles[name], **override}
        profiles[name]["extra_args"] = list(profiles[name].get("extra_args", []))
    seeds = expand_seed_spec(args.seeds, int(variant["seeds"]))
    tasks = [
        Task(args.result, args.variant, algorithm, seed, float(drop_rate), trim_mode)
        for algorithm in algorithms
        for drop_rate in variant["drop_rates"]
        for trim_mode in variant["trim_modes"]
        for seed in seeds
    ]
    task_identity = "\n".join(sorted(task_slug(task) for task in tasks))

    run_root = args.output_root.resolve() / args.result / args.variant
    provenance_path = run_root / "provenance.json"
    if args.resume and not provenance_path.is_file():
        raise SystemExit(f"Refusing to resume without existing provenance: {provenance_path}")
    if not args.resume and run_root.exists():
        raise SystemExit(f"Refusing existing output root: {run_root}")
    run_root.mkdir(parents=True, exist_ok=True)
    provenance = {
        "result": args.result,
        "variant": args.variant,
        "paper_element": result["paper_element"],
        "config": str(args.config.resolve()),
        "config_sha256": file_sha256(args.config.resolve()),
        "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
        "git_worktree_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=REPO, text=True
        ).strip()),
        "simulator": str(binary),
        "simulator_sha256": file_sha256(binary),
        "input": str(input_path),
        "input_sha256": file_sha256(input_path),
        "topology": str(topology_path),
        "topology_sha256": file_sha256(topology_path),
        "task_count": len(tasks),
        "task_identity_sha256": hashlib.sha256(task_identity.encode("utf-8")).hexdigest(),
        "workers": args.workers,
        "absolute_rto_override_us": args.rto_us,
        "task_timeout_seconds": args.timeout_seconds,
    }
    if args.resume:
        existing = json.loads(provenance_path.read_text(encoding="utf-8"))
        immutable = (
            "result", "variant", "config_sha256", "simulator_sha256",
            "input_sha256", "topology_sha256", "task_count", "task_identity_sha256",
            "task_timeout_seconds",
        )
        mismatches = [
            key for key in immutable
            if existing.get(key) != provenance.get(key)
        ]
        if mismatches:
            raise SystemExit(
                "Refusing to mix resumed output across changed provenance fields: "
                + ", ".join(mismatches)
            )
        existing.setdefault("resume_history", []).append({
            "git_commit": provenance["git_commit"],
            "git_worktree_dirty": provenance["git_worktree_dirty"],
            "workers": args.workers,
            "unix_time": time.time(),
        })
        provenance_path.write_text(
            json.dumps(existing, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    else:
        provenance_path.write_text(
            json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    rows: list[dict[str, object]] = []
    failures: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        future_map = {
            executor.submit(
                run_one, task, result, variant,
                profiles[task.algorithm], run_root,
                args.resume, binary, args.timeout_seconds,
            ): task
            for task in tasks
        }
        for completed_count, future in enumerate(concurrent.futures.as_completed(future_map), 1):
            task = future_map[future]
            try:
                row = future.result()
            except Exception as exc:
                row = {"result": task.result, "variant": task.variant, "algorithm": task.algorithm, "seed": task.seed, "drop_rate": task.drop_rate, "trim_mode": task.trim_mode, "status": "exception", "exit_code": -1, "runtime_seconds": "", "max_rss_kbytes": "", "log_bytes": "", "command_sha256": ""}
                failures.append(f"{task_slug(task)}: {exc}")
            rows.append(row)
            if row["status"] not in {"pass", "resumed-pass"}:
                failures.append(f"{task_slug(task)}: {row['status']} rc={row['exit_code']}")
            if completed_count % 25 == 0 or completed_count == len(tasks):
                print(f"[progress] {completed_count}/{len(tasks)} runs complete", flush=True)

    rows.sort(key=lambda row: (str(row["algorithm"]), float(row["drop_rate"]), str(row["trim_mode"]), int(row["seed"])))
    status_path = run_root / "run_status.csv"
    with status_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=list(rows[0].keys()), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)
    if failures:
        (run_root / "failures.txt").write_text("\n".join(failures) + "\n", encoding="utf-8")
        print(f"[error] {len(failures)} failures; see {run_root / 'failures.txt'}", file=sys.stderr)
        return 2
    print(f"[done] all {len(rows)} runs passed; status: {status_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
