#!/usr/bin/env python3
"""Generate benchmark plots and flamegraph assets for the IPC study.

This script reads the separate throughput and latency CSV files, the cache-miss
perf log, and the existing SVG flamegraphs. It writes publication-friendly
figures into a figures/ directory and builds a small HTML gallery for the
flamegraphs.
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, median, pstdev
from typing import Dict, List, Sequence

import matplotlib.pyplot as plt

MESSAGE_SIZES = [64, 256, 1024, 4096, 16384, 65536, 262144, 1048576]

@dataclass(frozen=True)
class BenchmarkConfig:
    key: str
    label: str
    t_csv: str
    l_csv: str
    color: str

BENCHMARKS = [
    BenchmarkConfig("pipe", "POSIX pipe", "pipe_throughput.csv", "pipe_latency.csv", "#1f77b4"),
    BenchmarkConfig("socket", "Unix domain socket", "socket_throughput.csv", "socket_latency.csv", "#ff7f0e"),
    BenchmarkConfig("mq", "POSIX message queue", "mq_throughput.csv", "mq_latency.csv", "#2ca02c"),
    BenchmarkConfig("uring", "io_uring shared ring", "uring_uring_throughput.csv", "uring_uring_latency.csv", "#d62728"),
]

ABLATION_VARIANTS = [
    ("spin", "Busy-poll (spin)", "#7f7f7f"),
    ("backoff", "Spin + backoff", "#bcbd22"),
    ("adaptive", "Adaptive spin-then-block", "#17becf"),
    ("futex", "futex", "#9467bd"),
    ("eventfd", "eventfd", "#8c564b"),
    ("uring", "io_uring", "#d62728"),
]

def load_csv_rows(path: Path) -> List[dict]:
    if not path.exists():
        print(f"Warning: File not found {path}")
        return []
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows: List[dict] = []
        for row in reader:
            parsed = {}
            for k, v in row.items():
                if k == "message_size_bytes" or k == "run":
                    parsed[k] = int(float(v))
                else:
                    parsed[k] = float(v)
            rows.append(parsed)
    return rows

def group_by_size(rows: Sequence[dict], metric: str) -> Dict[int, List[float]]:
    grouped: Dict[int, List[float]] = defaultdict(list)
    for row in rows:
        if metric in row:
            grouped[int(row["message_size_bytes"])].append(float(row[metric]))
    return grouped

def summarize_grouped_distribution(grouped: Dict[int, List[float]]) -> tuple[list[int], list[float], list[float], list[float], list[float]]:
    sizes = sorted(grouped)
    if not sizes:
        return [], [], [], [], []
    medians = [median(grouped[size]) for size in sizes]
    lower_quartiles: list[float] = []
    upper_quartiles: list[float] = []
    means: list[float] = []
    for size in sizes:
        values = sorted(grouped[size])
        count = len(values)
        if count == 1:
            q1 = q3 = values[0]
        else:
            mid = count // 2
            if count % 2 == 0:
                lower_half = values[:mid]
                upper_half = values[mid:]
            else:
                lower_half = values[:mid]
                upper_half = values[mid + 1 :]
            q1 = median(lower_half) if lower_half else values[0]
            q3 = median(upper_half) if upper_half else values[-1]
        lower_quartiles.append(q1)
        upper_quartiles.append(q3)
        means.append(mean(values))
    return sizes, medians, lower_quartiles, upper_quartiles, means

def setup_plot_style() -> None:
    plt.style.use("seaborn-v0_8-whitegrid")
    plt.rcParams.update(
        {
            "figure.dpi": 200,
            "savefig.dpi": 300,
            "font.family": "serif",
            "font.serif": ["Times New Roman", "DejaVu Serif", "Liberation Serif", "serif"],
            "font.size": 10,
            "axes.titlesize": 13,
            "axes.labelsize": 11,
            "legend.fontsize": 9,
            "axes.titleweight": "bold",
            "axes.labelweight": "normal",
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": True,
            "grid.alpha": 0.25,
            "grid.linestyle": ":",
            "axes.edgecolor": "#111111",
            "axes.linewidth": 0.8,
        }
    )

def format_size_ticks(ax: plt.Axes) -> None:
    ax.set_xscale("log", base=2)
    ax.set_xticks(MESSAGE_SIZES)
    ax.set_xticklabels([f"{size}" for size in MESSAGE_SIZES])

def format_research_axis(ax: plt.Axes) -> None:
    ax.tick_params(axis="both", which="major", length=4, width=0.8, direction="out")
    ax.margins(x=0.02)

LINE_STYLES = {
    "pipe": "-.",
    "socket": "--",
    "mq": ":",
    "uring": "-",
}

MARKERS = {
    "pipe": "^",
    "socket": "s",
    "mq": "d",
    "uring": "o",
}

def plot_metric_comparison(
    benchmark_data: Dict[str, List[dict]],
    metric: str,
    ylabel: str,
    title: str,
    output_path: Path,
    *,
    yscale: str = "linear",
    show_iqr: bool = False,
) -> None:
    fig, ax = plt.subplots(figsize=(7.5, 4.2))

    for benchmark in BENCHMARKS:
        rows = benchmark_data[benchmark.key]
        if not rows:
            continue
        grouped = group_by_size(rows, metric)
        sizes, medians, q1_values, q3_values, _ = summarize_grouped_distribution(grouped)
        if not sizes:
            continue
        
        ax.plot(
            sizes,
            medians,
            marker=MARKERS[benchmark.key],
            linestyle=LINE_STYLES[benchmark.key],
            linewidth=1.8,
            markersize=5,
            color=benchmark.color,
            label=benchmark.label,
        )
        if show_iqr:
            ax.fill_between(sizes, q1_values, q3_values, color=benchmark.color, alpha=0.12)

    format_size_ticks(ax)
    ax.set_xlabel("Message Size (Bytes)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(loc="best", frameon=True, fancybox=False, edgecolor="#cccccc")
    ax.set_yscale(yscale)
    format_research_axis(ax)
    fig.tight_layout()
    fig.savefig(output_path, dpi=300)
    plt.close(fig)

def plot_speedup_vs_uring(
    benchmark_data: Dict[str, List[dict]],
    output_path: Path,
) -> None:
    if "uring" not in benchmark_data or not benchmark_data["uring"]:
        return
    fig, ax = plt.subplots(figsize=(7.5, 4.2))

    uring_grouped = group_by_size(benchmark_data["uring"], "throughput_gbps")
    sizes, uring_median, _, _, _ = summarize_grouped_distribution(uring_grouped)
    if not sizes:
        return
    size_to_uring = dict(zip(sizes, uring_median))

    for benchmark in BENCHMARKS:
        if benchmark.key == "uring":
            continue
        rows = benchmark_data[benchmark.key]
        if not rows:
            continue
        grouped = group_by_size(rows, "throughput_gbps")
        bench_sizes, bench_medians, _, _, _ = summarize_grouped_distribution(grouped)
        if not bench_sizes:
            continue
        speedups = [size_to_uring[size] / baseline if baseline else 0.0 for size, baseline in zip(bench_sizes, bench_medians)]
        
        ax.plot(
            bench_sizes,
            speedups,
            marker=MARKERS[benchmark.key],
            linestyle=LINE_STYLES[benchmark.key],
            linewidth=1.8,
            markersize=5,
            color=benchmark.color,
            label=f"io_uring vs {benchmark.label}",
        )

    ax.axhline(1.0, color="#444444", linestyle="--", linewidth=0.8, alpha=0.7)
    format_size_ticks(ax)
    ax.set_xlabel("Message Size (Bytes)")
    ax.set_ylabel("Throughput Ratio (io_uring / Baseline)")
    ax.set_title("Throughput Advantage of io_uring over Each Baseline")
    ax.set_yscale("linear")
    ax.set_ylim(0, None)
    ax.legend(loc="best", frameon=True, fancybox=False, edgecolor="#cccccc")
    format_research_axis(ax)
    fig.tight_layout()
    fig.savefig(output_path)
    plt.close(fig)

def plot_ablation_throughput(root: Path, output_dir: Path) -> None:
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    has_data = False

    for var_key, label, color in ABLATION_VARIANTS:
        csv_path = root / "data" / f"uring_{var_key}_throughput.csv"
        rows = load_csv_rows(csv_path)
        if not rows:
            continue
        has_data = True
        grouped = group_by_size(rows, "throughput_gbps")
        sizes, medians, _, _, _ = summarize_grouped_distribution(grouped)
        
        ax.plot(
            sizes,
            medians,
            marker="o",
            linewidth=1.8,
            markersize=4,
            color=color,
            label=label,
        )

    if not has_data:
        plt.close(fig)
        return

    format_size_ticks(ax)
    ax.set_xlabel("Message Size (Bytes)")
    ax.set_ylabel("Median Throughput (GB/s)")
    ax.set_title("Ablation Study: Saturated Throughput across Wakeup Mechanisms")
    ax.legend(loc="best", frameon=True, fancybox=False, edgecolor="#cccccc")
    format_research_axis(ax)
    fig.tight_layout()
    fig.savefig(output_dir / "ablation_throughput.png")
    plt.close(fig)

def plot_ablation_latency(root: Path, output_dir: Path) -> None:
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    has_data = False

    for var_key, label, color in ABLATION_VARIANTS:
        csv_path = root / "data" / f"uring_{var_key}_latency.csv"
        rows = load_csv_rows(csv_path)
        if not rows:
            continue
        has_data = True
        grouped = group_by_size(rows, "p50_us")
        sizes, medians, _, _, _ = summarize_grouped_distribution(grouped)
        
        ax.plot(
            sizes,
            medians,
            marker="s",
            linewidth=1.8,
            markersize=4,
            color=color,
            label=label,
        )

    if not has_data:
        plt.close(fig)
        return

    format_size_ticks(ax)
    ax.set_xlabel("Message Size (Bytes)")
    ax.set_ylabel("Median p50 Latency (microseconds)")
    ax.set_yscale("log")
    ax.set_title("Ablation Study: Ping-Pong Latency across Wakeup Mechanisms")
    ax.legend(loc="best", frameon=True, fancybox=False, edgecolor="#cccccc")
    format_research_axis(ax)
    fig.tight_layout()
    fig.savefig(output_dir / "ablation_latency.png")
    plt.close(fig)

def plot_ablation_offered_load(root: Path, output_dir: Path) -> None:
    rates = [10000, 50000, 100000, 250000]
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    has_data = False

    # Plot at 4KB message size for different rates
    target_size = 4096

    for var_key, label, color in ABLATION_VARIANTS:
        x_rates = []
        y_latencies = []
        
        for r in rates:
            # Look for offered load CSV at this rate
            csv_path = root / "data" / f"uring_{var_key}_offered_{r}_throughput.csv"
            # Wait, offered load latency would be in uring_<wakeup>_offered_<rate>_latency.csv if recorded
            # Since we sweep arrival regimes, let's check what results are generated.
            # If we don't have rate-specific files, we skip
            pass
            
    # We will draw a schematic comparison if offered load files are populated
    # Let's keep it simple: if rate-specific files exist, plot them.
    plt.close(fig)

def parse_perf_value(raw: str) -> int | None:
    token = raw.strip().replace(",", "")
    if token.startswith("<"):
        return None
    try:
        return int(float(token))
    except ValueError:
        return None

def benchmark_key_from_command(command: str) -> str:
    command = command.lower()
    if "pipe" in command:
        return "pipe"
    if "socket" in command:
        return "socket"
    if "mq" in command:
        return "mq"
    if "uring" in command:
        return "uring"
    return command.replace("./", "").replace("run_", "").replace("_bench.sh", "")

def load_cache_miss_summary(path: Path) -> List[dict]:
    section_re = re.compile(r"Performance counter stats for '(.+?)':")
    counter_re = re.compile(r"^\s*([<\d,]+)\s+([A-Za-z0-9_./-]+)")

    summaries: List[dict] = []
    current_key: str | None = None
    current_values = defaultdict(int)

    def flush() -> None:
        if current_key is None:
            return
        loads_l1 = current_values["l1_loads"]
        misses_l1 = current_values["l1_misses"]
        loads_llc = current_values["llc_loads"]
        misses_llc = current_values["llc_misses"]
        summaries.append(
            {
                "benchmark": current_key,
                "l1_loads": loads_l1,
                "l1_misses": misses_l1,
                "llc_loads": loads_llc,
                "llc_misses": misses_llc,
                "l1_miss_rate": (misses_l1 / loads_l1) if loads_l1 else 0.0,
                "llc_miss_rate": (misses_llc / loads_llc) if loads_llc else 0.0,
            }
        )

    for line in path.read_text().splitlines():
        section_match = section_re.search(line)
        if section_match:
            flush()
            current_key = benchmark_key_from_command(section_match.group(1))
            current_values = defaultdict(int)
            continue

        counter_match = counter_re.match(line)
        if not counter_match or current_key is None:
            continue

        count = parse_perf_value(counter_match.group(1))
        if count is None:
            continue

        metric_name = counter_match.group(2)
        if "L1-dcache-loads" in metric_name:
            current_values["l1_loads"] += count
        elif "L1-dcache-load-misses" in metric_name:
            current_values["l1_misses"] += count
        elif "LLC-loads" in metric_name:
            current_values["llc_loads"] += count
        elif "LLC-load-misses" in metric_name:
            current_values["llc_misses"] += count

    flush()
    return summaries

def plot_cache_misses(summaries: List[dict], output_path: Path, summary_csv: Path) -> None:
    fieldnames = [
        "benchmark",
        "l1_loads",
        "l1_misses",
        "llc_loads",
        "llc_misses",
        "l1_miss_rate",
        "llc_miss_rate",
    ]
    with summary_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in summaries:
            writer.writerow(row)

    order = [benchmark.key for benchmark in BENCHMARKS]
    aligned = {row["benchmark"]: row for row in summaries if row["benchmark"] in order}

    # Ensure all order items exist in aligned
    for key in order:
        if key not in aligned:
            aligned[key] = {
                "l1_miss_rate": 0.0,
                "llc_miss_rate": 0.0
            }

    l1_values = [aligned[key]["l1_miss_rate"] for key in order]
    llc_values = [aligned[key]["llc_miss_rate"] for key in order]

    fig, axes = plt.subplots(1, 2, figsize=(8.5, 4.0), sharex=True)
    bar_positions = list(range(len(order)))
    labels = [next(item.label for item in BENCHMARKS if item.key == key) for key in order]
    colors = [next(item.color for item in BENCHMARKS if item.key == key) for key in order]
    
    hatches = ["//", "\\\\", "||", "++"]

    bars0 = axes[0].bar(bar_positions, l1_values, color=colors, edgecolor="#111111", linewidth=0.8)
    for bar, hatch in zip(bars0, hatches):
        bar.set_hatch(hatch)
    axes[0].set_title("L1 Data Cache Miss Rate")
    axes[0].set_ylabel("Miss Rate (%)")
    axes[0].set_ylim(0, max(l1_values) * 1.25 if max(l1_values) > 0 else 1.0)
    axes[0].set_xticks(bar_positions, labels, rotation=15, ha="right")
    axes[0].yaxis.set_major_formatter(lambda value, _: f"{value * 100:.1f}%")
    axes[0].tick_params(axis="both", which="major", length=4, width=0.8, direction="out")

    bars1 = axes[1].bar(bar_positions, llc_values, color=colors, edgecolor="#111111", linewidth=0.8)
    for bar, hatch in zip(bars1, hatches):
        bar.set_hatch(hatch)
    axes[1].set_title("Last Level Cache (LLC) Miss Rate")
    axes[1].set_ylabel("Miss Rate (%)")
    axes[1].set_ylim(0, max(llc_values) * 1.25 if max(llc_values) > 0 else 1.0)
    axes[1].set_xticks(bar_positions, labels, rotation=15, ha="right")
    axes[1].yaxis.set_major_formatter(lambda value, _: f"{value * 100:.2f}%")
    axes[1].tick_params(axis="both", which="major", length=4, width=0.8, direction="out")

    fig.suptitle("Hardware Cache Miss Rates Comparison", fontweight="bold", fontsize=12)
    fig.tight_layout()
    fig.savefig(output_path, dpi=300)
    plt.close(fig)

def build_flamegraph_gallery(root: Path, output_dir: Path) -> Path:
    gallery_dir = output_dir / "flamegraphs"
    gallery_dir.mkdir(parents=True, exist_ok=True)

    assets: List[Path] = []
    for suffix in ["*_flamegraph.svg", "*.jpg", "*.jpeg"]:
        for file in sorted(gallery_dir.glob(suffix)):
            if file.name != "index.html":
                assets.append(file)

    html_path = gallery_dir / "index.html"
    html_parts = [
        "<!doctype html>",
        "<html lang=\"en\">",
        "<head>",
        "<meta charset=\"utf-8\"/>",
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>",
        "<title>io_uring-IPC Flamegraph Gallery</title>",
        "<style>",
        "body{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:32px;background:#f7f7fb;color:#172033;}",
        "h1{margin-top:0;}",
        ".card{background:#fff;border:1px solid #d9dbe7;border-radius:14px;padding:18px;margin:18px 0;box-shadow:0 10px 28px rgba(18,24,40,.06);}",
        "object, img{width:100%;height:820px;border:1px solid #eef0f6;border-radius:10px;background:#fff;object-fit:contain;}",
        "p{line-height:1.55;}",
        "code{background:#eef2ff;padding:2px 5px;border-radius:4px;}",
        "</style>",
        "</head>",
        "<body>",
        "<h1>io_uring-IPC Flamegraph Gallery</h1>",
        "<p>The flamegraphs below show CPU execution profiles captured for the IPC benchmark families.</p>",
    ]

    for path in assets:
        title = path.stem.replace("_", " ").title()
        if path.suffix.lower() == ".svg":
            html_parts.extend(
                [
                    "<section class=\"card\">",
                    f"<h2>{title}</h2>",
                    f"<object data=\"{path.name}\" type=\"image/svg+xml\"></object>",
                    "</section>",
                ]
            )
        else:
            html_parts.extend(
                [
                    "<section class=\"card\">",
                    f"<h2>{title}</h2>",
                    f"<img src=\"{path.name}\" alt=\"{title}\" />",
                    "</section>",
                ]
            )

    html_parts.extend(["</body>", "</html>"])
    html_path.write_text("\n".join(html_parts), encoding="utf-8")
    return html_path

def write_summary_markdown(root: Path, output_dir: Path) -> Path:
    summary_path = output_dir / "summary.md"
    entries = [
        "# Visualization Output Summary",
        "",
        f"- Repository root: {root}",
        "- Generated charts: throughput.png, latency.png, speedup.png, cache_misses.png, ablation_throughput.png, ablation_latency.png",
        "- Statistical framing: median lines with interquartile bands for performance plots",
        "- Cache plot: normalized miss rates instead of raw counts",
        "- Flamegraph gallery: figures/flamegraphs/index.html",
        "",
        "Open the HTML gallery in a browser to inspect the existing SVG flamegraphs.",
    ]
    summary_path.write_text("\n".join(entries), encoding="utf-8")
    return summary_path

def main() -> int:
    parser = argparse.ArgumentParser(description="Generate IPC benchmark figures and gallery assets.")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help="Repository root")
    parser.add_argument("--output", type=Path, default=None, help="Output directory for generated figures")
    args = parser.parse_args()

    root = args.root.resolve()
    output_dir = (args.output or (root / "figures")).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    setup_plot_style()

    benchmark_data: Dict[str, List[dict]] = {}
    for benchmark in BENCHMARKS:
        csv_path = root / "data" / benchmark.t_csv
        rows_t = load_csv_rows(csv_path)
        
        csv_path_l = root / "data" / benchmark.l_csv
        rows_l = load_csv_rows(csv_path_l)
        
        # Merge lists or keep them key-specific
        # In our plotting:
        # plot_metric_comparison will look for metrics in benchmark_data[key]
        # We can just put both sets of rows in benchmark_data[key]
        benchmark_data[benchmark.key] = rows_t + rows_l

    plot_metric_comparison(
        benchmark_data,
        metric="throughput_gbps",
        ylabel="Median throughput (GB/s)",
        title="Throughput across IPC mechanisms (median with IQR)",
        output_path=output_dir / "throughput.png",
        show_iqr=True,
    )
    plot_metric_comparison(
        benchmark_data,
        metric="p50_us",
        ylabel="Median p50 latency (microseconds)",
        title="End-to-end latency across IPC mechanisms (median with IQR)",
        output_path=output_dir / "latency.png",
        yscale="log",
        show_iqr=True,
    )
    plot_speedup_vs_uring(benchmark_data, output_dir / "speedup.png")

    # Generate Ablation Study plots
    plot_ablation_throughput(root, output_dir)
    plot_ablation_latency(root, output_dir)

    cache_miss_file = root / "data" / "Cache Misses"
    if cache_miss_file.exists():
        cache_summaries = load_cache_miss_summary(cache_miss_file)
        plot_cache_misses(cache_summaries, output_dir / "cache_misses.png", output_dir / "cache_misses_summary.csv")

    build_flamegraph_gallery(root, output_dir)
    write_summary_markdown(root, output_dir)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())