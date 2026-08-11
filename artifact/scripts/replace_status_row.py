#!/usr/bin/env python3
"""Replace one failed matrix status row with a successful recovery row."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--replacement", type=Path, required=True)
    parser.add_argument("--algorithm", required=True)
    parser.add_argument(
        "--append-if-missing", action="store_true",
        help="Append the successful row when the base matrix intentionally omitted it.",
    )
    parser.add_argument(
        "--allow-failed", action="store_true",
        help="Allow an explicit failed or timed-out replacement row.",
    )
    args = parser.parse_args()

    with args.base.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        fields = list(reader.fieldnames or ())
        rows = list(reader)
    with args.replacement.open(newline="", encoding="utf-8") as handle:
        replacements = list(csv.DictReader(handle))
    selected = [row for row in replacements if row["algorithm"] == args.algorithm]
    allowed_status = {"pass", "resumed-pass"}
    if args.allow_failed:
        allowed_status.update({"fail", "timeout", "exception"})
    if len(selected) != 1 or selected[0]["status"] not in allowed_status:
        raise RuntimeError("Expected one permitted replacement row")
    matches = [index for index, row in enumerate(rows) if row["algorithm"] == args.algorithm]
    if len(matches) == 1:
        rows[matches[0]] = selected[0]
    elif not matches and args.append_if_missing:
        rows.append(selected[0])
    else:
        raise RuntimeError("Expected one base row to replace")
    with args.base.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
