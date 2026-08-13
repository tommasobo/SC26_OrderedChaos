#!/usr/bin/env python3
"""Regenerate Table I and Figure 5 from their analytical definitions."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, MultipleLocator
import pandas as pd
import seaborn as sns


FORMULAS = [
    ("Infinite Memory", "#BDP Packets x 32b PSN"),
    ("Fixed Path", "#Paths x 32b ePSN + 512b BitMap"),
    ("Gen-Based", "#Paths x #Gen x 32b ePSN + 512b BitMap"),
    ("PSN-Based", "512b BitMap"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--link-gbps", type=float, default=400.0)
    parser.add_argument("--rtt-us", type=float, default=10.0)
    parser.add_argument("--mtu-bytes", type=int, default=4096)
    parser.add_argument("--subflows", type=int, default=16)
    parser.add_argument("--generations", type=int, default=2)
    parser.add_argument("--max-flows", type=int, default=1024)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if min(args.link_gbps, args.rtt_us, args.mtu_bytes, args.subflows,
           args.generations, args.max_flows) <= 0:
        raise SystemExit("All analytical parameters must be positive")
    args.output.mkdir(parents=True, exist_ok=True)

    bdp_bytes = args.link_gbps * 1e9 * args.rtt_us * 1e-6 / 8.0
    bdp_packets = math.ceil(bdp_bytes / args.mtu_bytes)
    per_flow = {
        "Infinite Memory": 4 * bdp_packets,
        "Fixed Path": 4 * args.subflows + 64,
        "Gen-Based": 4 * args.subflows * args.generations + 64,
        "PSN-Based": 64,
    }

    with (args.output / "table1_formulas.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["version", "state_to_store"])
        writer.writerows(FORMULAS)
    latex_rows = [
        "\\begin{tabular}{ll}",
        "\\toprule",
        "Version & State to Store \\\\",
        "\\midrule",
    ]
    for version, formula in FORMULAS:
        escaped = formula.replace("#", r"\#").replace(" x ", r" $\times$ ")
        latex_rows.append(f"{version} & {escaped} \\\\")
    latex_rows.extend(["\\bottomrule", "\\end{tabular}", ""])
    (args.output / "table1_formulas.tex").write_text("\n".join(latex_rows), encoding="utf-8")

    flows = range(1, args.max_flows + 1)
    frame = pd.DataFrame({"active_flows": flows})
    for name, bytes_per_flow in per_flow.items():
        frame[name] = frame["active_flows"] * bytes_per_flow
    frame.to_csv(args.output / "figure5_memory_bytes.csv", index=False)

    long = frame.melt(id_vars="active_flows", var_name="Variant", value_name="Total bytes")
    sns.set_theme(
        style="whitegrid", context="paper", palette="Set2", font="serif"
    )
    plt.rcParams.update({"font.size": 11.4})
    fig, ax = plt.subplots(figsize=(4.55, 1.75))
    sns.lineplot(data=long, x="active_flows", y="Total bytes", hue="Variant",
                 linewidth=3.0, ax=ax)
    ax.legend(loc="upper left", fontsize=8.8)
    ax.yaxis.set_major_locator(MultipleLocator(100 * 1024))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value / 1024:,.0f}"))
    ax.set_xlabel("Active Flows")
    ax.set_ylabel("Memory (KiB)")
    fig.tight_layout()
    fig.savefig(args.output / "figure5_memory.png", dpi=220, bbox_inches="tight")
    fig.savefig(args.output / "figure5_memory.pdf", bbox_inches="tight")
    plt.close(fig)

    at_1000 = {name: value * 1000 / 1024.0 for name, value in per_flow.items()}
    summary = {
        "parameters": vars(args) | {"output": str(args.output.resolve())},
        "nic_bdp_bytes": bdp_bytes,
        "nic_bdp_packets_exact": bdp_bytes / args.mtu_bytes,
        "nic_bdp_packets_ceil": bdp_packets,
        "per_flow_bytes": per_flow,
        "memory_at_1000_flows_kib": at_1000,
        "paper_claim_psn_under_100_kib": at_1000["PSN-Based"] < 100,
        "paper_claim_infinite_exceeds_488_kib": at_1000["Infinite Memory"] > 488,
    }
    (args.output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
