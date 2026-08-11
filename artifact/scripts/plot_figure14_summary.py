#!/usr/bin/env python3
"""Regenerate Figure 14 from transparent summary CSVs (plot-only evidence)."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif"],
    "mathtext.fontset": "dejavuserif",
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})


REPO = Path(__file__).resolve().parents[2]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def label_size(value: int) -> str:
    return f"{value // 1048576}\nMiB" if value >= 1048576 else f"{value // 1024}\nKiB"


def main() -> int:
    args = parse_args()
    if args.output.exists():
        raise SystemExit(f"Refusing existing output directory: {args.output}")
    args.output.mkdir(parents=True)
    overhead = pd.read_csv(REPO / "artifact/data/figure14_probe_overhead.csv")
    fct = pd.read_csv(REPO / "artifact/data/figure14_probe_fct.csv")
    correctness = pd.read_csv(REPO / "artifact/data/figure14_probe_correctness.csv")
    labels = [label_size(int(value)) for value in overhead["message_size_bytes"]]
    x = np.arange(len(labels))

    overhead_pct = np.maximum(0.0, (overhead["measured_runtime_ratio"] - 1.0) * 100.0)
    figure, axis = plt.subplots(figsize=(4.2, 1.25), dpi=150)
    for spine in axis.spines.values():
        spine.set_edgecolor("#cccccc")
    axis.plot(x, overhead_pct, color="#8da0cb", marker="o", markeredgecolor="black",
              linewidth=1.8)
    axis.set_xticks(x)
    axis.set_xticklabels(labels, fontsize=8)
    axis.set_ylabel("% Overhead", fontsize=9.4)
    axis.set_ylim(bottom=0)
    axis.yaxis.grid(True, linestyle="-", alpha=0.3)
    figure.tight_layout()
    figure.savefig(args.output / "figure14a_probe_overhead.png", bbox_inches="tight")
    figure.savefig(args.output / "figure14a_probe_overhead.pdf", bbox_inches="tight")
    plt.close(figure)

    figure, axes = plt.subplots(1, 2, figsize=(7.2, 2.2), dpi=150)
    axes[0].plot(x, correctness["dropped_packets"], marker="o", label="Dropped packets")
    axes[0].plot(x, correctness["retransmissions"], marker="x", linestyle="--",
                 label="Retransmissions")
    axes[0].set_xticks(x)
    axes[0].set_xticklabels(labels, fontsize=7)
    axes[0].set_ylabel("Count")
    axes[0].set_title("Embedded correctness counters", fontsize=9)
    axes[0].legend(fontsize=7)
    axes[0].grid(True, axis="y", alpha=0.3)
    axes[1].plot(x, correctness["timeouts_rtx_only"], marker="o", label="RTX only")
    axes[1].plot(x, correctness["timeouts_all_probes"], marker="s", label="All probes")
    axes[1].set_xticks(x)
    axes[1].set_xticklabels(labels, fontsize=7)
    axes[1].set_ylabel("Timeout count")
    axes[1].set_title("Embedded timeout counters", fontsize=9)
    axes[1].legend(fontsize=7)
    axes[1].grid(True, axis="y", alpha=0.3)
    figure.tight_layout()
    figure.savefig(args.output / "figure14_correctness_summary.png", bbox_inches="tight")
    figure.savefig(args.output / "figure14_correctness_summary.pdf", bbox_inches="tight")
    plt.close(figure)

    series = [
        ("rss_only_ms", "RSS Only", "#F29572"),
        ("pfld_rtx_only_ms", "PFLD Only RTX", "#9DAED4"),
        ("pfld_tail_only_ms", "PFLD Tail Only", "#73C7B5"),
        ("pfld_all_probes_ms", "PFLD All Probes", "#ECCEAA"),
    ]
    figure, axis = plt.subplots(figsize=(4.3, 1.9), dpi=150)
    axis.set_axisbelow(True)
    axis.grid(True, axis="y", color="#cccccc", linestyle="-", linewidth=0.8, zorder=0)
    width = 0.18
    offsets = np.array([-1.5, -0.5, 0.5, 1.5]) * width
    for offset, (column, label, color) in zip(offsets, series):
        axis.bar(x + offset, fct[column], width, label=label, color=color,
                 edgecolor="black", linewidth=0.8, alpha=0.9, zorder=3)
    axis.set_ylabel("Average FCT (ms)")
    axis.set_yscale("log")
    axis.legend(frameon=True, fancybox=True, fontsize=8, loc="upper left")
    axis.set_xticks(x)
    axis.set_xticklabels(labels, fontsize=8)
    figure.tight_layout()
    figure.savefig(args.output / "figure14b_probe_fct.png", bbox_inches="tight")
    figure.savefig(args.output / "figure14b_probe_fct.pdf", bbox_inches="tight")
    plt.close(figure)

    final = fct[fct["message_size_bytes"] == 1048576].iloc[0]
    summary = {
        "classification": "plot-only: DPU, firmware, and raw logs are unavailable",
        "maximum_clamped_overhead_percent": float(overhead_pct.max()),
        "one_mib_speedup_all_probes_vs_rss": float(
            final["rss_only_ms"] / final["pfld_all_probes_ms"]
        ),
        "one_mib_speedup_tail_only_vs_rss": float(
            final["rss_only_ms"] / final["pfld_tail_only_ms"]
        ),
        "embedded_retransmissions_equal_drops_all_sizes": bool(
            (correctness["retransmissions"] == correctness["dropped_packets"]).all()
        ),
        "embedded_all_probe_timeouts_all_sizes_zero": bool(
            (correctness["timeouts_all_probes"] == 0).all()
        ),
        "embedded_rtx_only_timeout_total": int(correctness["timeouts_rtx_only"].sum()),
        "correctness_evidence_classification": (
            "summary-supported by arrays embedded in the versioned plotter; "
            "not independently reproducible without raw hardware logs"
        ),
    }
    (args.output / "claim_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
