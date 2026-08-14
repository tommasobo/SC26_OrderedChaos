#!/usr/bin/env python3
"""
Generate a single-direction ring all-reduce traffic matrix, optionally repeated sequentially.

Usage:
  python gen_allreduce.py <outfile> <nodes> <conns> <groupsize> <flowsize> <locality> <randseed> [--randstart SECONDS] [--npasses N]

Args:
  outfile    Path to write the schedule.
  nodes      Total nodes in the topology (integer).
  conns      Number of participating ranks (must be <= nodes and a multiple of groupsize).
  groupsize  Ranks per all-reduce group (>= 2).
  flowsize   Bytes per hop (e.g., 1000000 for ~1 MB).
  locality   0 = random groups; 1 = “local” groups (participants sorted before grouping).
  randseed   Seed for RNG; 0 = system default.
  randstart  Max random start time for first-hop sends in the first pass (inclusive upper bound). 0 disables. Default: 0.
  npasses    Repeat the full 2*(G-1)-hop ring this many times sequentially. Default: 1.

Notes:
- Each pass is the classic ring all-reduce: G chunks (one per rank) over 2*(G-1) serialized hops per chunk.
- Passes are chained per chunk: the last hop of pass p emits a trigger that starts pass p+1's first hop
  of the same chunk. This adds no parallel rings; it simply loops the ring again.
- When randstart > 0, each chunk’s first hop in the first pass starts at a random absolute time in [0, randstart].
- Header counts are computed from the generated schedule.
"""

import sys
from random import seed as rndseed, shuffle, randint
import argparse


def gen_allreduce_text(nodes: int, conns: int, groupsize: int, flowsize: int,
                       locality: int, randseed: int, randstart: int = 0, npasses: int = 1) -> str:
    assert groupsize >= 2, "groupsize must be >= 2"
    assert conns % groupsize == 0, "conns must be a multiple of groupsize"
    assert 0 < conns <= nodes, "conns must be >0 and <= nodes"
    assert npasses >= 1, "npasses must be >= 1"
    assert randstart >= 0, "randstart must be >= 0"

    if randseed != 0:
        rndseed(randseed)

    # choose participating ranks
    ranks = list(range(nodes))
    shuffle(ranks)
    participants = ranks[:conns]
    if locality:
        participants.sort()

    # partition into groups contiguously within participants
    groups = [participants[i:i + groupsize] for i in range(0, conns, groupsize)]

    lines = []
    id_counter = 1
    trig_id = 1  # global trigger id space

    for group in groups:
        g = len(group)
        # one chunk per rank
        for c in range(g):
            next_start_trig = None  # will hold the trigger to start the next pass for this chunk
            for p in range(npasses):
                # exactly 2*(g-1) hops per chunk per pass
                for d in range(1, 2 * g - 1):  # 1..(2g-2)
                    src = group[(c + d - 1) % g]
                    dst = group[(c + d) % g]

                    parts = [f"{src}->{dst}", f"id {id_counter}"]

                    if d == 1:
                        if p == 0:
                            # randomize the absolute start time of the first-hop sends in the first pass
                            start_time = randint(0, randstart) if randstart > 0 else 0
                            parts.append(f"start {start_time}")
                        else:
                            # start this pass of this chunk only after the previous pass finished
                            parts.append(f"trigger {next_start_trig}")
                    else:
                        parts.append(f"trigger {trig_id}")
                        trig_id += 1

                    parts.append(f"size {flowsize}")

                    if d != 2 * g - 2:
                        # normal intra-pass chaining
                        parts.append(f"send_done_trigger {trig_id}")
                    else:
                        # final hop of the pass: emit a trigger only if there's another pass to run
                        if p != npasses - 1:
                            parts.append(f"send_done_trigger {trig_id}")
                            # record the trigger that will start the next pass for this chunk
                            next_start_trig = trig_id

                    lines.append(" ".join(parts))
                    id_counter += 1

    last_trigger_id = trig_id - 1  # total triggers used

    header = [
        f"Nodes {nodes}",
        f"Connections {len(lines)}",
        f"Triggers {last_trigger_id}",
    ]

    triggers_block = [f"trigger id {i} oneshot" for i in range(1, last_trigger_id + 1)]

    return "\n".join(header + lines + triggers_block) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(
        description="Generate a single-direction ring all-reduce traffic matrix.")
    parser.add_argument("outfile", help="Path to write the schedule")
    parser.add_argument("nodes", type=int)
    parser.add_argument("conns", type=int)
    parser.add_argument("groupsize", type=int)
    parser.add_argument("flowsize", type=int)
    parser.add_argument("locality", type=int, choices=(0, 1))
    parser.add_argument("randseed", type=int)
    parser.add_argument("--randstart", type=int, default=0,
                        help="Max random start time for first-hop sends in pass 1; 0 disables (default: 0)")
    parser.add_argument("--npasses", type=int, default=1,
                        help="Repeat the full ring this many times sequentially (default: 1)")

    args = parser.parse_args(argv[1:])

    text = gen_allreduce_text(args.nodes, args.conns, args.groupsize, args.flowsize,
                              args.locality, args.randseed, args.randstart, args.npasses)
    with open(args.outfile, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main(sys.argv)
