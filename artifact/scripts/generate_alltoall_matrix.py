#!/usr/bin/env python3
"""Generate a deterministic bounded-concurrency all-to-all traffic matrix."""

from __future__ import annotations

import argparse
import math
import random
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nodes", type=int, default=128)
    parser.add_argument("--flow-bytes", type=int, default=1_000_000)
    parser.add_argument("--concurrency-per-source", type=int, default=4)
    parser.add_argument(
        "--active-sources", type=int, default=0,
        help=("Maximum simultaneously active source streams. Zero starts all sources. "
              "A finite limit currently requires one active flow per source."),
    )
    parser.add_argument(
        "--destination-schedule", choices=("round-robin", "independent-shuffle"),
        default="round-robin",
        help=("Use balanced permutation rounds, or independently shuffle each source's "
              "destinations (which can create transient incast)."),
    )
    parser.add_argument("--start-ps", type=int, default=1_000_000)
    parser.add_argument("--seed", type=int, default=20260810)
    return parser.parse_args()


def build(args: argparse.Namespace) -> list[str]:
    if args.nodes < 2 or args.flow_bytes <= 0 or args.concurrency_per_source <= 0:
        raise ValueError("nodes, flow-bytes, and concurrency-per-source must be positive")
    active_sources = args.nodes if args.active_sources == 0 else args.active_sources
    if not 1 <= active_sources <= args.nodes:
        raise ValueError("active-sources must be zero or in [1, nodes]")
    if active_sources < args.nodes and args.concurrency_per_source != 1:
        raise ValueError("bounded active-sources currently requires concurrency-per-source=1")
    rng = random.Random(args.seed)
    sources = list(range(args.nodes))
    rng.shuffle(sources)
    offsets = list(range(1, args.nodes))
    rng.shuffle(offsets)
    stages = math.ceil((args.nodes - 1) / args.concurrency_per_source)
    triggers_per_source = stages - 1
    connection_count = args.nodes * (args.nodes - 1)
    progress_trigger_count = args.nodes * triggers_per_source
    source_start_trigger_count = args.nodes - active_sources
    trigger_count = progress_trigger_count + source_start_trigger_count
    lines = [
        f"Nodes {args.nodes}",
        f"Connections {connection_count}",
        f"Triggers {trigger_count}",
    ]
    flow_id = 1
    for source_index, source in enumerate(sources):
        if args.destination_schedule == "round-robin":
            # For every shared offset, src -> (src + offset) mod N is a
            # permutation: each sender and receiver participates exactly once.
            # Grouping several offsets retains that balance at higher bounded
            # concurrency without manufacturing random many-to-one bursts.
            destinations = [(source + offset) % args.nodes for offset in offsets]
        else:
            destinations = [node for node in range(args.nodes) if node != source]
            random.Random(args.seed + 104729 * (source + 1)).shuffle(destinations)
        first_trigger = source_index * triggers_per_source + 1
        for offset, destination in enumerate(destinations):
            stage = offset // args.concurrency_per_source
            fields = [f"{source}->{destination}", f"id {flow_id}"]
            if stage == 0 and source_index < active_sources:
                fields.append(f"start {args.start_ps}")
            elif stage == 0:
                fields.append(
                    f"trigger {progress_trigger_count + source_index - active_sources + 1}"
                )
            else:
                fields.append(f"trigger {first_trigger + stage - 1}")
            fields.append(f"size {args.flow_bytes}")
            if stage < stages - 1:
                fields.append(f"send_done_trigger {first_trigger + stage}")
            elif source_index + active_sources < args.nodes:
                # With concurrency one, the last flow precisely marks source
                # completion and activates one successor source stream.
                fields.append(
                    f"send_done_trigger {progress_trigger_count + source_index + 1}"
                )
            lines.append(" ".join(fields))
            flow_id += 1
    lines.extend(f"trigger id {trigger_id} multishot"
                 for trigger_id in range(1, trigger_count + 1))
    return lines


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lines = build(args)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    active = args.nodes if args.active_sources == 0 else args.active_sources
    print(f"{args.output}: {args.nodes} nodes, {args.nodes * (args.nodes - 1)} flows, "
          f"at most {active} active source streams")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
