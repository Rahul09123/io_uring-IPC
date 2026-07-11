#!/usr/bin/env python3
"""Generate benchmark plots and flamegraph assets for the IPC study.

This script reads the benchmark CSV files in the repository root, the cache-miss
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
    csv_name: str
    color: str


BENCHMARKS = [
    BenchmarkConfig("pipe", "POSIX pipe", "pipe_results.csv", "#1f77b4"),
    BenchmarkConfig("socket", "Unix domain socket", "socket_results.csv", "#ff7f0e"),
    BenchmarkConfig("mq", "POSIX message queue", "mq_results.csv", "#2ca02c"),
    BenchmarkConfig("uring", "io_uring shared ring", "io_uring_results.csv", "#d62728"),
]


def load_csv_rows(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        rows: List[dict] = []
        for row in reader:
            rows.append(
                {
                    "message_size_bytes": int(float(row["message_size_bytes"])),
                    "run": int(float(row["run"])),
                    "throughput_gbps": float(row["throughput_gbps"]),
                    "avg_latency_us": float(row["avg_latency_us"]),
                    "stddev_us": float(row["stddev_us"]),
                    "p50_us": float(row["p50_us"]),
                    "p95_us": float(row["p95_us"]),
                    "p99_us": float(row["p99_us"]),
                }
            )
    return rows


def group_by_size(rows: Sequence[dict], metric: str) -> Dict[int, List[float]]:
    grouped: Dict[int, List[float]] = defaultdict(list)
    for row in rows:
        grouped[int(row["message_size_bytes"])]
        grouped[int(row["message_size_bytes"])].append(float(row[metric]))
    return grouped


def summarize_grouped_values(grouped: Dict[int, List[float]]) -> tuple[list[int], list[float], list[float]]:
    sizes = sorted(grouped)
    averages = [mean(grouped[size]) for size in sizes]
    deviations = [pstdev(grouped[size]) if len(grouped[size]) > 1 else 0.0 for size in sizes]
    return sizes, averages, deviations


def summarize_grouped_distribution(grouped: Dict[int, List[float]]) -> tuple[list[int], list[float], list[float], list[float], list[float]]:
    sizes = sorted(grouped)
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
            "figure.dpi": 170,
            "savefig.dpi": 260,
            "font.size": 11,
            "axes.titlesize": 15,
            "axes.labelsize": 12,
            "legend.fontsize": 10,
            "axes.titleweight": "semibold",
            "axes.labelweight": "medium",
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": True,
            "grid.alpha": 0.22,
        }
    )


def format_size_ticks(ax: plt.Axes) -> None:
    ax.set_xscale("log", base=2)
    ax.set_xticks(MESSAGE_SIZES)
    ax.set_xticklabels([f"{size}" for size in MESSAGE_SIZES])


def format_research_axis(ax: plt.Axes) -> None:
    ax.tick_params(axis="both", which="major", length=0)
    ax.margins(x=0.02)


def plot_metric_comparison(
    benchmark_data: Dict[str, List[dict]],
    metric: str,
    ylabel: str,
    title: str,
    output_path: Path,
    *,
    yscale: str = "linear",
    show_iqr: bool = False,
    show_mean: bool = False,
) -> None:
    fig, ax = plt.subplots(figsize=(11, 6))

    for benchmark in BENCHMARKS:
        rows = benchmark_data[benchmark.key]
        grouped = group_by_size(rows, metric)
        sizes, medians, q1_values, q3_values, means = summarize_grouped_distribution(grouped)
        ax.plot(
            sizes,
            medians,
            marker="o",
            linewidth=2.5,
            color=benchmark.color,
            label=benchmark.label,
        )
        if show_iqr:
            ax.fill_between(sizes, q1_values, q3_values, color=benchmark.color, alpha=0.15)
        if show_mean:
            ax.plot(
                sizes,
                means,
                linestyle="--",
                linewidth=1.2,
                color=benchmark.color,
                alpha=0.6,
            )

    format_size_ticks(ax)
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(loc="best", frameon=True)
    ax.set_yscale(yscale)
    format_research_axis(ax)
    fig.tight_layout()
    fig.savefig(output_path)
    plt.close(fig)


def plot_speedup_vs_uring(
    benchmark_data: Dict[str, List[dict]],
    output_path: Path,
) -> None:
    fig, ax = plt.subplots(figsize=(11, 6))

    uring_grouped = group_by_size(benchmark_data["uring"], "throughput_gbps")
    sizes, uring_median, _, _, _ = summarize_grouped_distribution(uring_grouped)
    size_to_uring = dict(zip(sizes, uring_median))

    baselines = [benchmark for benchmark in BENCHMARKS if benchmark.key != "uring"]

    for benchmark in BENCHMARKS:
        if benchmark.key == "uring":
            continue
        grouped = group_by_size(benchmark_data[benchmark.key], "throughput_gbps")
        bench_sizes, bench_medians, _, _, _ = summarize_grouped_distribution(grouped)
        speedups = [size_to_uring[size] / baseline if baseline else 0.0 for size, baseline in zip(bench_sizes, bench_medians)]
        ax.plot(
            bench_sizes,
            speedups,
            marker="o",
            linewidth=2.4,
            color=benchmark.color,
            label=f"io_uring vs {benchmark.label}",
        )

    ax.axhline(1.0, color="#444444", linestyle="--", linewidth=1.0, alpha=0.7)
    format_size_ticks(ax)
    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel("Throughput ratio (io_uring / baseline)")
    ax.set_title("Throughput advantage of io_uring over each baseline")
    ax.set_yscale("linear")
    ax.set_ylim(0, None)
    ax.legend(loc="best", frameon=True)
    format_research_axis(ax)
    fig.tight_layout()
    fig.savefig(output_path)
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
    aligned = {row["benchmark"]: row for row in summaries}

    l1_values = [aligned[key]["l1_miss_rate"] for key in order]
    llc_values = [aligned[key]["llc_miss_rate"] for key in order]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5.4), sharex=True)
    bar_positions = list(range(len(order)))
    labels = [next(item.label for item in BENCHMARKS if item.key == key) for key in order]
    colors = [next(item.color for item in BENCHMARKS if item.key == key) for key in order]

    axes[0].bar(bar_positions, l1_values, color=colors)
    axes[0].set_title("L1 data-cache miss rate")
    axes[0].set_ylabel("Miss rate")
    axes[0].set_ylim(0, max(l1_values) * 1.25 if l1_values else 1.0)
    axes[0].set_xticks(bar_positions, labels, rotation=18, ha="right")
    axes[0].yaxis.set_major_formatter(lambda value, _: f"{value:.1%}")

    axes[1].bar(bar_positions, llc_values, color=colors)
    axes[1].set_title("Last-level-cache miss rate")
    axes[1].set_ylabel("Miss rate")
    axes[1].set_ylim(0, max(llc_values) * 1.25 if llc_values else 1.0)
    axes[1].set_xticks(bar_positions, labels, rotation=18, ha="right")
    axes[1].yaxis.set_major_formatter(lambda value, _: f"{value:.1%}")

    fig.suptitle("Normalized cache behavior across IPC implementations")
    fig.tight_layout()
    fig.savefig(output_path)
    plt.close(fig)


def build_flamegraph_gallery(root: Path, output_dir: Path) -> Path:
    flamegraph_src = root / "FlameGraphs"
    gallery_dir = output_dir / "flamegraphs"
    gallery_dir.mkdir(parents=True, exist_ok=True)

    copied: List[Path] = []
    for svg_file in sorted(flamegraph_src.glob("*_flamegraph.svg")):
        target = gallery_dir / svg_file.name
        shutil.copy2(svg_file, target)
        copied.append(target)

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
        "object{width:100%;height:820px;border:1px solid #eef0f6;border-radius:10px;background:#fff;}",
        "p{line-height:1.55;}",
        "code{background:#eef2ff;padding:2px 5px;border-radius:4px;}",
        "</style>",
        "</head>",
        "<body>",
        "<h1>io_uring-IPC Flamegraph Gallery</h1>",
        "<p>Each SVG below is the flamegraph captured for one IPC benchmark family.</p>",
    ]

    for svg_path in copied:
        title = svg_path.stem.replace("_", " ").title()
        html_parts.extend(
            [
                "<section class=\"card\">",
                f"<h2>{title}</h2>",
                f"<object data=\"{svg_path.name}\" type=\"image/svg+xml\"></object>",
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
        "- Generated charts: throughput.png, latency.png, speedup.png, cache_misses.png",
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
        csv_path = root / benchmark.csv_name
        if not csv_path.exists():
            raise FileNotFoundError(f"Missing benchmark CSV: {csv_path}")
        benchmark_data[benchmark.key] = load_csv_rows(csv_path)

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
        metric="avg_latency_us",
        ylabel="Median latency (microseconds)",
        title="End-to-end latency across IPC mechanisms (median with IQR)",
        output_path=output_dir / "latency.png",
        yscale="log",
        show_iqr=True,
    )
    plot_speedup_vs_uring(benchmark_data, output_dir / "speedup.png")

    cache_miss_file = root / "Cache Misses"
    if cache_miss_file.exists():
        cache_summaries = load_cache_miss_summary(cache_miss_file)
        plot_cache_misses(cache_summaries, output_dir / "cache_misses.png", output_dir / "cache_misses_summary.csv")

    build_flamegraph_gallery(root, output_dir)
    write_summary_markdown(root, output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())