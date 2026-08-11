#!/usr/bin/env python3
"""Reject generated PDFs that contain Type 3 or unembedded fonts."""

from __future__ import annotations

import argparse
from pathlib import Path

import fitz


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths", nargs="+", type=Path,
        help="PDF files or directories to inspect recursively.",
    )
    return parser.parse_args()


def pdf_paths(paths: list[Path]) -> list[Path]:
    found: set[Path] = set()
    for path in paths:
        if path.is_dir():
            found.update(item for item in path.rglob("*.pdf") if item.is_file())
        elif path.is_file() and path.suffix.lower() == ".pdf":
            found.add(path)
        else:
            raise FileNotFoundError(path)
    return sorted(found)


def main() -> int:
    args = parse_args()
    failures: list[str] = []
    checked = 0
    for path in pdf_paths(args.paths):
        document = fitz.open(path)
        fonts: dict[int, tuple[str, str, str]] = {}
        for page in document:
            for xref, extension, font_type, base_font, *_ in page.get_fonts(full=True):
                fonts[xref] = (extension, font_type, base_font)
        for xref, (extension, font_type, base_font) in sorted(fonts.items()):
            if font_type == "Type3":
                failures.append(f"{path}: Type 3 font {base_font}")
                continue
            _, extracted_extension, _, font_data = document.extract_font(xref)
            if not font_data:
                failures.append(
                    f"{path}: unembedded font {base_font} "
                    f"({font_type}, {extension or extracted_extension})"
                )
        checked += 1
        document.close()
    if failures:
        raise SystemExit("PDF font audit failed:\n" + "\n".join(failures))
    print(f"PDF font audit passed for {checked} file(s): no Type 3 or unembedded fonts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
