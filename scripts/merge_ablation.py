#!/usr/bin/env python3
"""merge_ablation.py — merge per-variant-regime CSV files into one master file.

Usage:
    python3 scripts/merge_ablation.py data/ablation_*.csv > data/ablation_results.csv
    # or
    python3 scripts/merge_ablation.py data/ablation_*.csv -o data/ablation_results.csv
"""

import csv
import sys
import os
import argparse
import re

VARIANT_NAMES = ["busy_poll", "spin_backoff", "adaptive", "futex", "eventfd", "io_uring"]
REGIME_NAMES  = ["saturated", "bursty", "offered_25", "offered_50", "offered_75", "offered_90"]


def detect_regime(filename: str) -> str:
    """Detect regime from filename like 'ablation_futex_saturated.csv'."""
    base = os.path.splitext(os.path.basename(filename))[0]
    for r in sorted(REGIME_NAMES, key=len, reverse=True):
        if base.endswith(f"_{r}"):
            return r
    return "unknown"


def main():
    parser = argparse.ArgumentParser(description="Merge ablation CSV files.")
    parser.add_argument("csvfiles", nargs="+", help="Input CSV files")
    parser.add_argument("-o", "--output", default=None,
                        help="Output file (default: stdout)")
    args = parser.parse_args()

    out = open(args.output, "w", newline="") if args.output else sys.stdout

    header_written = False
    fieldnames = None
    writer = None

    for fname in sorted(args.csvfiles):
        if not os.path.isfile(fname):
            print(f"WARN: {fname} not found, skipping", file=sys.stderr)
            continue

        regime = detect_regime(fname)

        with open(fname, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                if not header_written:
                    fieldnames = reader.fieldnames
                    writer = csv.DictWriter(out, fieldnames=fieldnames)
                    writer.writeheader()
                    header_written = True

                # patch regime column if it's still "unknown"
                if row.get("regime", "unknown") == "unknown":
                    row["regime"] = regime

                writer.writerow(row)

    if args.output:
        out.close()
        print(f"Merged {len(args.csvfiles)} file(s) -> {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
