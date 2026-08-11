#!/usr/bin/env python3
"""Build the 4 MiB camera-ready and periodic-probe follow-up report."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import fitz


WIDTH, HEIGHT = 1190.55, 720.0
NAVY = (0.055, 0.122, 0.200)
ORANGE = (0.915, 0.485, 0.145)
BLUE = (0.105, 0.350, 0.610)
GREEN = (0.075, 0.550, 0.390)
WHITE = (1.0, 1.0, 1.0)
LIGHT = (0.945, 0.960, 0.975)
LINE = (0.760, 0.810, 0.855)
TEXT = (0.100, 0.140, 0.190)
MUTED = (0.335, 0.395, 0.465)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--job", default="3051492")
    return parser.parse_args()


def text(page, rect, value, size=10, color=TEXT, font="helv", align=0):
    return page.insert_textbox(rect, value, fontsize=size, fontname=font,
                               color=color, align=align, lineheight=1.14)


def card(page, rect, heading, body, color=BLUE):
    page.draw_rect(rect, color=LINE, fill=LIGHT, width=0.7)
    x0, y0, x1, y1 = rect
    page.draw_rect((x0, y0, x0 + 8, y1), color=color, fill=color)
    text(page, (x0 + 22, y0 + 14, x1 - 14, y0 + 40), heading, 11, NAVY, "hebo")
    text(page, (x0 + 22, y0 + 47, x1 - 14, y1 - 12), body, 9.2, TEXT)


def title(page, heading, subtitle, number):
    page.draw_rect((0, 0, WIDTH, 8), color=ORANGE, fill=ORANGE)
    text(page, (36, 24, WIDTH - 36, 57), heading, 20, NAVY, "hebo")
    text(page, (36, 60, WIDTH - 36, 87), subtitle, 8.8, MUTED)
    text(page, (36, 698, WIDTH - 36, 714),
         f"OrderedChaos 4 MiB camera-ready follow-up  |  page {number}", 7.2, MUTED, align=2)


def insert_image(page, path: Path, rect: fitz.Rect):
    pix = fitz.Pixmap(str(path))
    image_ratio = pix.width / pix.height
    target_ratio = rect.width / rect.height
    if image_ratio > target_ratio:
        height = rect.width / image_ratio
        fitted = fitz.Rect(rect.x0, rect.y0 + (rect.height - height) / 2,
                           rect.x1, rect.y0 + (rect.height + height) / 2)
    else:
        width = rect.height * image_ratio
        fitted = fitz.Rect(rect.x0 + (rect.width - width) / 2, rect.y0,
                           rect.x0 + (rect.width + width) / 2, rect.y1)
    page.insert_image(fitted, filename=str(path), keep_proportion=True)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def camera_value(rows, algorithm, trim, key):
    return float(next(row[key] for row in rows
                      if row["algorithm"] == algorithm and row["trim"] == trim))


def stress_value(rows, algorithm, key):
    return float(next(row[key] for row in rows if row["algorithm"] == algorithm))


def cover(doc, results: Path, job: str):
    camera = read_csv(results / "camera_ready_new_image2_summary.csv")
    stress = read_csv(results / "camera_ready_periodic_probe_stress_summary.csv")
    page = doc.new_page(width=WIDTH, height=HEIGHT)
    page.draw_rect(page.rect, color=NAVY, fill=NAVY)
    page.draw_rect((0, 0, 20, HEIGHT), color=ORANGE, fill=ORANGE)
    text(page, (64, 52, 1125, 102), "4 MiB camera-ready follow-up", 31, WHITE, "hebo")
    text(page, (64, 116, 1125, 153),
         "All-baseline rerun, probe semantics audit, and periodic-probe stress alternative",
         15, WHITE, "hebo")
    text(page, (64, 174, 1125, 229),
         "Fresh results use 100 matched seeds. Camera Ready New Image 2 retains the balanced "
         "128-node all-to-all schedule and changes only each flow from 1 MiB to 4 MiB.",
         11, (0.82, 0.88, 0.94))
    pfld_off = camera_value(camera, "pfld", "off", "mean_rto_events")
    pfld_on = camera_value(camera, "pfld", "on", "mean_rto_events")
    tlp_off = camera_value(camera, "rss_rack_tlp", "off", "mean_rto_events")
    tlp_on = camera_value(camera, "rss_rack_tlp", "on", "mean_rto_events")
    x1 = stress_value(stress, "pfld_probe1_no_rtx", "mean_rto_events")
    tail = stress_value(stress, "pfld_tail_only", "mean_rto_events")
    card(page, (64, 270, 548, 424), "CAMERA READY IMAGE 2",
         f"PFLD mean RTOs: {pfld_off:.2f} / {pfld_on:.2f} (trim off/on)\n"
         f"TLP-RACK: {tlp_off:.2f} / {tlp_on:.2f}\n"
         "128 nodes; 16,256 directed flows; 4,000,000 B/flow; p=0.005",
         GREEN)
    card(page, (582, 270, 1066, 424), "PERIODIC-PROBE STRESS CASE",
         f"Section/tail only: {tail:.2f} mean RTOs\n"
         f"Periodic X=1: {x1:.2f} ({100 * (tail - x1) / tail:.1f}% fewer)\n"
         "32-to-1 incast; 4,000,000 B/sender; trim off; p=0.005",
         ORANGE)
    text(page, (64, 472, 1125, 552),
         "Interpretation guardrail: the incast panel is a deliberately bursty sensitivity case. "
         "It demonstrates faster loss detection in RTO counts; maximum FCT remains bottleneck-"
         "dominated and does not improve materially. Probes are 64-byte, low-FIFO witnesses and "
         "are outside the simulator's >500-byte corruption-drop condition.",
         10.2, (0.82, 0.88, 0.94))
    text(page, (64, 620, 1125, 670),
         f"Definitive Slurm job {job}  |  simulator/config commit 82d0f1488b6157f1ef7d3416ab0325a7902c714d\n"
         "Whiskers in all generated figures: normal 95% CI of the mean (mean +/- 1.96 SEM).",
         8.8, (0.68, 0.77, 0.86), "cour")


def image_page(doc, heading, subtitle, image: Path, number):
    page = doc.new_page(width=WIDTH, height=HEIGHT)
    page.draw_rect(page.rect, color=WHITE, fill=WHITE)
    title(page, heading, subtitle, number)
    insert_image(page, image, fitz.Rect(28, 96, WIDTH - 28, 686))


def main() -> int:
    args = parse_args()
    results = args.results.resolve()
    required = (
        results / "camera_ready_new_image2.png",
        results / "camera_ready_new_image2_linear.png",
        results / "camera_ready_new_image2_summary.csv",
        results / "camera_ready_probe_compounding.png",
        results / "camera_ready_periodic_probe_stress.png",
        results / "camera_ready_periodic_probe_stress_summary.csv",
    )
    for path in required:
        if not path.is_file():
            raise FileNotFoundError(path)
    doc = fitz.open()
    cover(doc, results, args.job)
    image_page(doc, "Camera Ready New Image 2", "Symlog view preserves exact and near-zero counts.",
               results / "camera_ready_new_image2.png", 2)
    image_page(doc, "Camera Ready New Image 2", "Linear-scale companion view; all eight baselines were freshly rerun.",
               results / "camera_ready_new_image2_linear.png", 3)
    image_page(doc, "Probe compounding in balanced traffic",
               "The corrected label is section/tail probes; periodic density is flat in this tornado workload.",
               results / "camera_ready_probe_compounding.png", 4)
    image_page(doc, "Periodic probes in a bursty stress case",
               "RTO sensitivity only: the shared receiver bottleneck dominates maximum FCT.",
               results / "camera_ready_periodic_probe_stress.png", 5)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    doc.save(args.output, garbage=4, deflate=True)
    doc.close()
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
