#!/usr/bin/env python3
"""Generate a deterministic many-to-one traffic matrix for probe stress tests."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nodes", type=int, default=128)
    parser.add_argument("--senders", type=int, default=64)
    parser.add_argument("--receiver", type=int, default=0)
    parser.add_argument("--flow-bytes", type=int, default=4_000_000)
    parser.add_argument("--start-ps", type=int, default=1_000_000)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.nodes < 2 or not 1 <= args.senders < args.nodes:
        raise ValueError("senders must be in [1, nodes - 1]")
    if not 0 <= args.receiver < args.nodes or args.flow_bytes <= 0:
        raise ValueError("receiver and flow-bytes are out of range")
    sources = [node for node in range(args.nodes) if node != args.receiver][: args.senders]
    lines = [f"Nodes {args.nodes}", f"Connections {len(sources)}"]
    lines.extend(
        f"{source}->{args.receiver} id {flow_id} start {args.start_ps} size {args.flow_bytes}"
        for flow_id, source in enumerate(sources, start=1)
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{args.output}: {len(sources)}-to-1, {args.flow_bytes} bytes per flow")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
