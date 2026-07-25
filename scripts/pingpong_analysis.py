#!/usr/bin/env python3
"""pingpong_analysis.py — Generate 3 figures from the corrected ping-pong benchmark.

Usage:
    python3 scripts/pingpong_analysis.py \\
        --data data/pingpong_results.csv \\
        --output figures/pingpong

Figures:
    fig_A_unloaded_latency.png  — Unloaded RTT/2 latency vs. message size (all IPC types)
    fig_B_tail_latency.png      — P99 / P99.9 tail latency comparison (grouped bar)
    fig_C_ablation_latency.png  — Wakeup variant ping-pong latency (SHM only)
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ── Style ─────────────────────────────────────────────────────────────────────
plt.rcParams.update({
    "figure.dpi":        150,
    "font.family":       "DejaVu Sans",
    "font.size":         10,
    "axes.titlesize":    11,
    "axes.labelsize":    10,
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "grid.alpha":        0.3,
    "lines.linewidth":   1.8,
    "lines.markersize":  5,
})

# ── Canonical order and display names ─────────────────────────────────────────
IPC_ORDER   = ["pipe", "unix_socket", "shm_io_uring", "posix_mq"]
IPC_LABELS  = ["Pipe", "Unix Socket", "SHM + io_uring", "POSIX MQ"]
IPC_COLORS  = ["#e63946", "#2a9d8f", "#3a86ff", "#f4a261"]
IPC_MARKERS = ["o", "s", "^", "D"]
IPC_COLOR   = dict(zip(IPC_ORDER, IPC_COLORS))
IPC_MARKER  = dict(zip(IPC_ORDER, IPC_MARKERS))

VARIANT_ORDER  = ["busy_poll", "spin_backoff", "adaptive", "futex", "eventfd", "io_uring"]
VARIANT_LABELS = ["Busy-Poll", "Spin+Backoff", "Adaptive", "Futex", "EventFD", "io_uring"]
VARIANT_COLORS = ["#e63946", "#f4a261", "#2a9d8f", "#457b9d", "#8338ec", "#3a86ff"]
V_COLOR = dict(zip(VARIANT_ORDER, VARIANT_COLORS))

MSG_SIZES = [64, 256, 1024, 4096, 16384, 65536, 262144, 1048576]
SIZE_LABELS = ["64B", "256B", "1KB", "4KB", "16KB", "64KB", "256KB", "1MB"]


# ── Data loading ──────────────────────────────────────────────────────────────
def load(path: str) -> list[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))

def sf(d: dict, key: str, default=0.0) -> float:
    try:
        v = d.get(key, default)
        return float(v) if v not in (None, "", "nan") else default
    except (ValueError, TypeError):
        return default


# ─────────────────────────────────────────────────────────────────────────────
# Figure A — Unloaded RTT/2 Latency vs. Message Size
# ─────────────────────────────────────────────────────────────────────────────
def fig_A(rows: list[dict], outdir: str):
    """One line per IPC type. X = message size (log2), Y = median RTT/2 in µs.
    Error bars show [p90, p99] whiskers.  This is the CORRECT latency metric."""

    fig, ax = plt.subplots(figsize=(11, 6))

    # Group data
    by_ipc_size = defaultdict(dict)
    for r in rows:
        ipc  = r.get("ipc_type", "")
        sz   = int(r.get("message_size_bytes", 0) or 0)
        if ipc in ("shm_ablation", "unknown") or sz == 0:
            continue
        med  = sf(r, "median_us")
        p90  = sf(r, "p90_us")
        p99  = sf(r, "p99_us")
        if med > 0:
            by_ipc_size[ipc][sz] = (med, p90, p99)

    plotted = False
    for ipc, label in zip(IPC_ORDER, IPC_LABELS):
        data = by_ipc_size.get(ipc, {})
        if not data:
            continue
        sizes  = sorted(data.keys())
        medians = [data[s][0] for s in sizes]
        p90s    = [data[s][1] for s in sizes]
        p99s    = [data[s][2] for s in sizes]

        # Error bar: lo = median→p90 difference, hi = median→p99 difference
        err_lo = [max(0, p90 - med) for med, p90 in zip(medians, p90s)]
        err_hi = [max(0, p99 - med) for med, p99 in zip(medians, p99s)]

        ax.errorbar(
            sizes, medians,
            yerr=[err_lo, err_hi],
            label=label,
            color=IPC_COLOR[ipc],
            marker=IPC_MARKER[ipc],
            capsize=4,
            capthick=1.2,
            elinewidth=0.9,
            alpha=0.9,
        )
        plotted = True

    if not plotted:
        ax.text(0.5, 0.5, "No IPC data found in CSV.\nRun the benchmark first.",
                ha="center", va="center", transform=ax.transAxes, fontsize=12)
    else:
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks(MSG_SIZES)
        ax.set_xticklabels(SIZE_LABELS, rotation=25, ha="right")
        ax.set_xlabel("Message Size")
        ax.set_ylabel("One-Way Latency ≈ RTT/2  (µs,  log scale)")
        ax.set_title(
            "Figure A — Unloaded Latency vs. Message Size  [depth-1 ping-pong, "
            "CLOCK_MONOTONIC_RAW]\n"
            "Error bars: P90 (lower) and P99 (upper)",
            fontsize=10
        )
        ax.grid(True, which="both")
        ax.legend(fontsize=9)

        # Annotate physics floor: transfer time at 20 GiB/s
        for sz, sl in zip(MSG_SIZES, SIZE_LABELS):
            transfer_us = (sz / (20.0 * 1024**3)) * 1e6
            if transfer_us > 0.3:
                ax.axvline(sz, color="gray", linewidth=0.3, alpha=0.3)

    plt.tight_layout()
    out = os.path.join(outdir, "fig_A_unloaded_latency.png")
    plt.savefig(out, bbox_inches="tight")
    plt.close()
    print(f"  ✓ {out}")


# ─────────────────────────────────────────────────────────────────────────────
# Figure B — Tail Latency Comparison (P99 and P99.9)
# ─────────────────────────────────────────────────────────────────────────────
def fig_B(rows: list[dict], outdir: str):
    """Grouped bar chart: X = IPC type, Y = P99/P99.9 one-way latency.
    One bar group per small (64B) and large (1KB) message size."""

    fig, axes = plt.subplots(1, 2, figsize=(13, 6), sharey=False)
    fig.suptitle(
        "Figure B — Tail Latency Comparison  (P99 and P99.9)\n"
        "Lower is better. Systems reviewers focus on tail latency for real-time use cases.",
        fontsize=10, y=1.02
    )

    FOCUS_SIZES = [64, 1024]
    FOCUS_LABELS = ["64 B (scheduling-dominated)", "1 KB (mixed)"]

    for ax, sz, sz_label in zip(axes, FOCUS_SIZES, FOCUS_LABELS):
        subset = [r for r in rows
                  if int(r.get("message_size_bytes", 0) or 0) == sz
                  and r.get("ipc_type", "") in IPC_ORDER]

        if not subset:
            ax.text(0.5, 0.5, f"No data for {sz_label}",
                    ha="center", va="center", transform=ax.transAxes)
            continue

        labels  = []
        p99_vals  = []
        p999_vals = []

        for ipc in IPC_ORDER:
            r = next((x for x in subset if x.get("ipc_type") == ipc), None)
            if r is None:
                continue
            labels.append(IPC_LABELS[IPC_ORDER.index(ipc)])
            p99_vals.append(sf(r, "p99_us"))
            p999_vals.append(sf(r, "p999_us"))

        x = np.arange(len(labels))
        w = 0.35
        b1 = ax.bar(x - w/2, p99_vals,  w, label="P99",   color="#3a86ff", alpha=0.85)
        b2 = ax.bar(x + w/2, p999_vals, w, label="P99.9", color="#e63946", alpha=0.85)

        ax.set_title(f"{sz_label}")
        ax.set_xlabel("IPC Mechanism")
        ax.set_ylabel("One-Way Latency ≈ RTT/2  (µs)")
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=15, ha="right")
        ax.legend(fontsize=9)
        ax.grid(axis="y")

        # Annotate values on bars
        for bar in list(b1) + list(b2):
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width()/2, h * 1.02,
                        f"{h:.1f}", ha="center", va="bottom", fontsize=7)

    plt.tight_layout()
    out = os.path.join(outdir, "fig_B_tail_latency.png")
    plt.savefig(out, bbox_inches="tight")
    plt.close()
    print(f"  ✓ {out}")


# ─────────────────────────────────────────────────────────────────────────────
# Figure C — Wakeup Variant Ping-Pong Latency (SHM ablation only)
# ─────────────────────────────────────────────────────────────────────────────
def fig_C(rows: list[dict], outdir: str):
    """Bar chart: X = wakeup variant, Y = median RTT/2.
    Two groups: 64B (scheduling-dominated) and 256B (mixed).
    This directly shows futex vs. io_uring latency in the latency-focused mode."""

    fig, axes = plt.subplots(1, 2, figsize=(13, 6), sharey=False)
    fig.suptitle(
        "Figure C — Wakeup Variant Ping-Pong Latency  (SHM, depth-1)\n"
        "Error bars: P99. Shows scheduling overhead per wakeup mechanism.",
        fontsize=10, y=1.02
    )

    ablation_rows = [r for r in rows if r.get("ipc_type") == "shm_ablation"]

    FOCUS_SIZES  = [64, 256]
    FOCUS_LABELS = ["64 B", "256 B"]

    for ax, sz, sz_label in zip(axes, FOCUS_SIZES, FOCUS_LABELS):
        subset = [r for r in ablation_rows
                  if int(r.get("message_size_bytes", 0) or 0) == sz]

        if not subset:
            ax.text(0.5, 0.5, f"No ablation data for {sz_label}",
                    ha="center", va="center", transform=ax.transAxes)
            continue

        labels  = []
        medians = []
        p99s    = []

        for vname, vlabel in zip(VARIANT_ORDER, VARIANT_LABELS):
            r = next((x for x in subset
                      if x.get("wakeup_variant") == vname), None)
            if r is None:
                continue
            labels.append(vlabel)
            medians.append(sf(r, "median_us"))
            p99s.append(sf(r, "p99_us"))

        if not labels:
            ax.text(0.5, 0.5, f"No variant data for {sz_label}",
                    ha="center", va="center", transform=ax.transAxes)
            continue

        x = np.arange(len(labels))
        colors = [V_COLOR.get(v, "#888") for v in VARIANT_ORDER if v in
                  [r.get("wakeup_variant") for r in subset]]

        err_hi = [max(0, p - m) for m, p in zip(medians, p99s)]

        bars = ax.bar(x, medians, color=colors, alpha=0.85, edgecolor="white",
                      yerr=err_hi, capsize=4, error_kw={"linewidth": 1.2})

        ax.set_title(f"Message size: {sz_label}")
        ax.set_xlabel("Wakeup Variant")
        ax.set_ylabel("One-Way Latency ≈ RTT/2  (µs)")
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=20, ha="right")
        ax.grid(axis="y")

        for bar, med in zip(bars, medians):
            if med > 0:
                ax.text(bar.get_x() + bar.get_width()/2,
                        bar.get_height() * 1.02,
                        f"{med:.2f}", ha="center", va="bottom", fontsize=8)

    plt.tight_layout()
    out = os.path.join(outdir, "fig_C_ablation_latency.png")
    plt.savefig(out, bbox_inches="tight")
    plt.close()
    print(f"  ✓ {out}")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Generate ping-pong figures.")
    parser.add_argument("--data",   required=True, help="merged pingpong CSV")
    parser.add_argument("--output", required=True, help="output directory")
    args = parser.parse_args()

    if not os.path.isfile(args.data):
        print(f"ERROR: {args.data} not found", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.output, exist_ok=True)

    rows = load(args.data)
    print(f"Loaded {len(rows)} rows from {args.data}")

    print("\nGenerating figures:")
    fig_A(rows, args.output)
    fig_B(rows, args.output)
    fig_C(rows, args.output)

    print(f"\nAll figures saved to: {args.output}")


if __name__ == "__main__":
    main()
