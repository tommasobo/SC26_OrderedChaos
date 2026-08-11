#!/usr/bin/env python3
"""Regenerate Figure 9 from fresh RSS and RSS+PFLD ring logs."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns


LINE = re.compile(
    r"^Flow\s+(?P<flow>\S+)\s+flowId\s+(?P<flow_id>\d+).*?"
    r"finished at\s+(?P<fct>[0-9.]+)\s+global time\s+(?P<global>[0-9.]+)"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rss-log", type=Path, required=True)
    parser.add_argument("--pfld-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--time-bin-us", type=float, default=20.0,
        help="Display aggregation width on the all-reduce time axis.",
    )
    parser.add_argument(
        "--fct-bin-us", type=float, default=2.0,
        help="Display aggregation width on the FCT axis.",
    )
    parser.add_argument(
        "--display-mode", choices=("bins", "sample", "raw"), default="bins",
        help="Use occupied bins, a deterministic per-scheme sample, or every raw point.",
    )
    parser.add_argument(
        "--sample-points", type=int, default=12000,
        help="Maximum points per scheme in sample mode.",
    )
    parser.add_argument("--marker-size", type=float, default=None)
    parser.add_argument("--alpha", type=float, default=None)
    return parser.parse_args()


def parse_log(path: Path, scheme: str) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = LINE.search(line)
            if match:
                rows.append({
                    "scheme": scheme,
                    "flow": match.group("flow"),
                    "flow_id": int(match.group("flow_id")),
                    "allreduce_time_us": float(match.group("global")),
                    "fct_us": float(match.group("fct")),
                    "source_log": str(path.resolve()),
                })
    frame = pd.DataFrame(rows)
    if frame.empty:
        raise ValueError(f"No completion records in {path}")
    return frame


def aggregate_for_display(frame: pd.DataFrame, time_bin_us: float,
                          fct_bin_us: float) -> pd.DataFrame:
    """Collapse occupied 2-D bins while preserving all raw rows separately."""
    work = frame.copy()
    work["time_bin"] = (work["allreduce_time_us"] // time_bin_us).astype(int)
    work["fct_bin"] = (work["fct_us"] // fct_bin_us).astype(int)
    return (
        work.groupby(["scheme", "time_bin", "fct_bin"], as_index=False)
        .agg(allreduce_time_us=("allreduce_time_us", "median"),
             fct_us=("fct_us", "median"), represented_flows=("flow_id", "count"))
    )


def scatter(axis: plt.Axes, frame: pd.DataFrame, mode: str,
            marker_size: float | None, alpha: float | None) -> None:
    if mode == "bins":
        sizes = (
            marker_size
            if marker_size is not None
            else 4.0 + 1.5 * frame["represented_flows"].clip(upper=25) ** 0.5
        )
        opacity = 0.68 if alpha is None else alpha
    else:
        sizes = 2.0 if marker_size is None else marker_size
        opacity = 0.24 if alpha is None else alpha
    axis.scatter(frame["allreduce_time_us"], frame["fct_us"], s=sizes,
                 color="#66c2a5", alpha=opacity, edgecolors="none", rasterized=True)
    axis.grid(True, alpha=0.35)


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rss = parse_log(args.rss_log, "RSS Only")
    pfld = parse_log(args.pfld_log, "RSS+PFLD")
    combined = pd.concat([rss, pfld], ignore_index=True)
    combined.to_csv(args.output / "figure9_flow_completions.csv", index=False)
    if args.display_mode == "bins":
        display = aggregate_for_display(combined, args.time_bin_us, args.fct_bin_us)
    elif args.display_mode == "sample":
        sampled = []
        for _, rows in combined.groupby("scheme", sort=False):
            sampled.append(rows.sample(n=min(len(rows), args.sample_points), random_state=17))
        display = pd.concat(sampled, ignore_index=True)
        display["represented_flows"] = 1
    else:
        display = combined.copy()
        display["represented_flows"] = 1
    display.to_csv(args.output / "figure9_display_bins.csv", index=False)
    display_rss = display[display["scheme"] == "RSS Only"]
    display_pfld = display[display["scheme"] == "RSS+PFLD"]

    sns.set_theme(style="whitegrid", context="paper", font="Liberation Serif")
    figure = plt.figure(figsize=(6.5, 2.7))
    grid = figure.add_gridspec(2, 2, height_ratios=[1, 3], width_ratios=[1, 1],
                              hspace=0.08, wspace=0.25)
    rss_top = figure.add_subplot(grid[0, 0])
    rss_bottom = figure.add_subplot(grid[1, 0], sharex=rss_top)
    pfld_axis = figure.add_subplot(grid[:, 1])
    scatter(rss_top, display_rss, args.display_mode, args.marker_size, args.alpha)
    scatter(rss_bottom, display_rss, args.display_mode, args.marker_size, args.alpha)
    scatter(pfld_axis, display_pfld, args.display_mode, args.marker_size, args.alpha)

    rss_regular = rss.loc[rss["fct_us"] < 150, "fct_us"]
    regular_max = max(90.0, float(rss_regular.max()) * 1.05) if not rss_regular.empty else 90.0
    rss_bottom.set_ylim(max(0.0, float(rss["fct_us"].min()) * 0.9), regular_max)
    rto = rss.loc[rss["fct_us"] >= 150, "fct_us"]
    if rto.empty:
        rss_top.set_ylim(190, 235)
    else:
        rss_top.set_ylim(max(150.0, float(rto.min()) * 0.96), float(rto.max()) * 1.04)
    rss_top.spines["bottom"].set_visible(False)
    rss_bottom.spines["top"].set_visible(False)
    rss_top.tick_params(labeltop=False, bottom=False, labelbottom=False)
    rss_bottom.tick_params(top=False)
    marker = 0.012
    kwargs = dict(color="k", clip_on=False, linewidth=0.8)
    rss_top.plot((-marker, +marker), (-marker, +marker), transform=rss_top.transAxes, **kwargs)
    rss_top.plot((1 - marker, 1 + marker), (-marker, +marker), transform=rss_top.transAxes, **kwargs)
    rss_bottom.plot((-marker, +marker), (1 - marker, 1 + marker), transform=rss_bottom.transAxes, **kwargs)
    rss_bottom.plot((1 - marker, 1 + marker), (1 - marker, 1 + marker), transform=rss_bottom.transAxes, **kwargs)

    rss_top.set_title("RSS Only", fontsize=9)
    rss_top.text(0.88, 0.35, "RTO\nZone", transform=rss_top.transAxes,
                 ha="center", va="center", fontsize=7)
    rss_bottom.set_xlabel("All-reduce time (us)")
    rss_bottom.set_ylabel("FCT (us)")

    pfld_axis.set_title("RSS+PFLD", fontsize=9)
    pfld_axis.set_xlabel("All-reduce time (us)")
    pfld_axis.set_ylabel("FCT (us)")
    pfld_axis.set_ylim(max(0.0, float(pfld["fct_us"].min()) * 0.9),
                       max(60.0, float(pfld["fct_us"].quantile(0.999)) * 1.08))
    lower = float(pfld["fct_us"].quantile(0.2))
    # The four-hop band is a minority of the ring flows; a central quantile
    # collapses onto the dominant two-hop band.  The 93rd percentile locates
    # the second dense band without discarding or reclassifying any points.
    upper = float(pfld["fct_us"].quantile(0.93))
    pfld_axis.text(0.03, lower, "2 Hops", fontsize=7, va="bottom",
                   transform=pfld_axis.get_yaxis_transform())
    pfld_axis.text(0.03, upper, "4 Hops", fontsize=7, va="bottom",
                   transform=pfld_axis.get_yaxis_transform())
    pfld_axis.text(0.03, 0.92, "PFLD prevents\nRTOs and maintains\nFCT stable",
                   transform=pfld_axis.transAxes, fontsize=7, va="top")

    figure.tight_layout(pad=0.45)
    figure.savefig(args.output / "figure9.png", dpi=220, bbox_inches="tight")
    figure.savefig(args.output / "figure9.pdf", bbox_inches="tight")
    plt.close(figure)
    summary = combined.groupby("scheme")["fct_us"].agg(["count", "median", "max"])
    summary.to_csv(args.output / "figure9_summary.csv")
    provenance = {
        "rss_log": str(args.rss_log.resolve()),
        "pfld_log": str(args.pfld_log.resolve()),
        "raw_flow_rows": len(combined),
        "display_rows": len(display),
        "display_mode": args.display_mode,
        "display_aggregation": (
            "one median marker per occupied all-reduce-time/FCT bin; marker area "
            "encodes represented flow count"
        ),
        "time_bin_us": args.time_bin_us,
        "fct_bin_us": args.fct_bin_us,
        "sample_points_per_scheme": args.sample_points if args.display_mode == "sample" else None,
        "marker_size": args.marker_size,
        "alpha": args.alpha,
        "raw_statistics_are_not_computed_from_display_bins": True,
    }
    (args.output / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(summary.to_string())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
