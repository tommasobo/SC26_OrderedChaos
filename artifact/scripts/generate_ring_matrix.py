#!/usr/bin/env python3
"""Generate a deterministic ring all-reduce matrix into a fresh path.

``flow-bytes`` is the size of each of the ``nodes`` ring chunks, not the total
per-rank collective payload. For an 8-MiB total on 128 nodes, use 65,536.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from gen_allreduce import gen_allreduce_text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nodes", type=int, default=128)
    parser.add_argument(
        "--flow-bytes", type=int, required=True,
        help="Bytes per ring chunk/hop (total per-rank payload divided by nodes).",
    )
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit(f"Refusing existing output: {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        gen_allreduce_text(args.nodes, args.nodes, args.nodes, args.flow_bytes, 1, 0),
        encoding="utf-8",
    )
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
