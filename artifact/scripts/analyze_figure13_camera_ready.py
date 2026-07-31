#!/usr/bin/env python3
"""Aggregate the Figure 13 seed repetitions."""

from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd


VARIANTS = ("paper_symmetric", "paper_asymmetric")
SEEDS = (1, 25, 42)
DROPS = (0.0,)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def drop_token(value: float) -> str:
    return f"{value:.10g}"


def main() -> int:
    args = parse_args()
    if args.output.exists():
        raise SystemExit(f"Refusing existing output root: {args.output}")
    args.output.mkdir(parents=True)
    summaries: list[dict[str, object]] = []

    for variant in VARIANTS:
        for drop in DROPS:
            frames = []
            for seed in SEEDS:
                path = (
                    args.raw_root
                    / variant
                    / f"seed{seed}"
                    / f"drop{drop_token(drop)}"
                    / "sensitivity_long.csv"
                )
                frame = pd.read_csv(path)
                frame["seed"] = seed
                frames.append(frame)
            all_rows = pd.concat(frames, ignore_index=True)
            aggregated = (
                all_rows.groupby(["subflows", "update_us"], as_index=False)
                .agg(
                    median_max_fct_us=("max_fct_us", "median"),
                    mean_max_fct_us=("max_fct_us", "mean"),
                    std_max_fct_us=("max_fct_us", "std"),
                    min_max_fct_us=("max_fct_us", "min"),
                    max_max_fct_us=("max_fct_us", "max"),
                )
            )
            aggregated["seed_cv_percent"] = (
                aggregated["std_max_fct_us"]
                / aggregated["mean_max_fct_us"]
                * 100.0
            )
            slug = f"{variant}_drop_{drop_token(drop)}"
            aggregated.to_csv(args.output / f"{slug}.csv", index=False)
            summaries.append(
                {
                    "variant": variant,
                    "drop_rate": drop,
                    "seeds": ",".join(map(str, SEEDS)),
                    "median_seed_cv_percent": float(
                        aggregated["seed_cv_percent"].median()
                    ),
                    "maximum_seed_cv_percent": float(
                        aggregated["seed_cv_percent"].max()
                    ),
                }
            )

    summary = pd.DataFrame(summaries)
    summary.to_csv(args.output / "summary.csv", index=False)
    print(summary.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
