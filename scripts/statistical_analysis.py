#!/usr/bin/env python3
"""Run statistical validation (95% CI and Mann-Whitney U tests) on IPC benchmarks."""

from pathlib import Path
import csv
from collections import defaultdict
import numpy as np
import scipy.stats as stats
import matplotlib.pyplot as plt

# Directory settings
ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "figures"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

MESSAGE_SIZES = [64, 256, 1024, 4096, 16384, 65536, 262144, 1048576]
BENCHMARKS = {
    "pipe": ("POSIX pipe", ROOT / "data" / "pipe_results.csv", "#1f77b4"),
    "socket": ("Unix domain socket", ROOT / "data" / "socket_results.csv", "#ff7f0e"),
    "mq": ("POSIX message queue", ROOT / "data" / "mq_results.csv", "#2ca02c"),
    "uring": ("io_uring shared ring", ROOT / "data" / "io_uring_results.csv", "#d62728"),
}

def load_data():
    raw_data = {}
    for key, (label, path, color) in BENCHMARKS.items():
        if not path.exists():
            raise FileNotFoundError(f"Missing benchmark CSV: {path}")
        
        runs = defaultdict(lambda: {"throughput": [], "latency": []})
        with path.open(newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                sz = int(float(row["message_size_bytes"]))
                runs[sz]["throughput"].append(float(row["throughput_gbps"]))
                # Using p50_us (median latency) for latency runs analysis
                runs[sz]["latency"].append(float(row["p50_us"]))
        raw_data[key] = runs
    return raw_data

def run_statistics(data):
    stats_summary = {}
    
    for sz in MESSAGE_SIZES:
        stats_summary[sz] = {}
        for key in BENCHMARKS:
            t_vals = data[key][sz]["throughput"]
            l_vals = data[key][sz]["latency"]
            
            # Confidence intervals
            t_mean = np.mean(t_vals)
            t_sem = stats.sem(t_vals) if len(t_vals) > 1 else 0
            t_ci = stats.t.interval(0.95, df=len(t_vals)-1, loc=t_mean, scale=t_sem) if t_sem > 0 else (t_mean, t_mean)
            
            l_mean = np.mean(l_vals)
            l_sem = stats.sem(l_vals) if len(l_vals) > 1 else 0
            l_ci = stats.t.interval(0.95, df=len(l_vals)-1, loc=l_mean, scale=l_sem) if l_sem > 0 else (l_mean, l_mean)
            
            stats_summary[sz][key] = {
                "t_mean": t_mean,
                "t_ci": t_ci,
                "t_vals": t_vals,
                "l_mean": l_mean,
                "l_ci": l_ci,
                "l_vals": l_vals
            }
            
        # Mann-Whitney U test comparing uring vs others
        uring_data = stats_summary[sz]["uring"]
        for key in ["pipe", "socket", "mq"]:
            other_data = stats_summary[sz][key]
            
            # Throughput U-test
            t_stat, t_p = stats.mannwhitneyu(uring_data["t_vals"], other_data["t_vals"], alternative="two-sided")
            # Latency U-test
            l_stat, l_p = stats.mannwhitneyu(uring_data["l_vals"], other_data["l_vals"], alternative="two-sided")
            
            stats_summary[sz][f"uring_vs_{key}"] = {
                "t_stat": t_stat,
                "t_p": t_p,
                "l_stat": l_stat,
                "l_p": l_p
            }
            
    return stats_summary

def plot_with_ci(stats_summary):
    plt.style.use("seaborn-v0_8-whitegrid")
    plt.rcParams.update({
        "figure.dpi": 200,
        "savefig.dpi": 300,
        "font.family": "serif",
        "font.size": 10,
        "axes.titlesize": 12,
        "axes.labelsize": 10,
        "legend.fontsize": 9,
        "axes.grid": True,
        "grid.alpha": 0.25,
        "grid.linestyle": ":",
    })
    
    # 1. Throughput Plot with 95% CI error bars
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    for key, (label, _, color) in BENCHMARKS.items():
        means = [stats_summary[sz][key]["t_mean"] for sz in MESSAGE_SIZES]
        yerr_lower = [stats_summary[sz][key]["t_mean"] - stats_summary[sz][key]["t_ci"][0] for sz in MESSAGE_SIZES]
        yerr_upper = [stats_summary[sz][key]["t_ci"][1] - stats_summary[sz][key]["t_mean"] for sz in MESSAGE_SIZES]
        yerr = [yerr_lower, yerr_upper]
        
        ax.errorbar(
            MESSAGE_SIZES, means, yerr=yerr, fmt="-o", capsize=4, elinewidth=1.2,
            linewidth=1.8, color=color, label=label, markersize=4
        )
        
    ax.set_xscale("log", base=2)
    ax.set_xticks(MESSAGE_SIZES)
    ax.set_xticklabels([str(sz) for sz in MESSAGE_SIZES])
    ax.set_xlabel("Message Size (Bytes)")
    ax.set_ylabel("Mean Throughput (GB/s)")
    ax.set_title("Throughput with 95% Confidence Intervals")
    ax.legend(loc="best", frameon=True, edgecolor="#cccccc")
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "throughput_ci.png")
    plt.close(fig)
    
    # 2. Latency Plot with 95% CI error bars
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    for key, (label, _, color) in BENCHMARKS.items():
        means = [stats_summary[sz][key]["l_mean"] for sz in MESSAGE_SIZES]
        yerr_lower = [stats_summary[sz][key]["l_mean"] - stats_summary[sz][key]["l_ci"][0] for sz in MESSAGE_SIZES]
        yerr_upper = [stats_summary[sz][key]["l_ci"][1] - stats_summary[sz][key]["l_mean"] for sz in MESSAGE_SIZES]
        yerr = [yerr_lower, yerr_upper]
        
        ax.errorbar(
            MESSAGE_SIZES, means, yerr=yerr, fmt="-s", capsize=4, elinewidth=1.2,
            linewidth=1.8, color=color, label=label, markersize=4
        )
        
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(MESSAGE_SIZES)
    ax.set_xticklabels([str(sz) for sz in MESSAGE_SIZES])
    ax.set_xlabel("Message Size (Bytes)")
    ax.set_ylabel("Mean p50 Latency (microseconds, Log Scale)")
    ax.set_title("Latency with 95% Confidence Intervals")
    ax.legend(loc="best", frameon=True, edgecolor="#cccccc")
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "latency_ci.png")
    plt.close(fig)

def print_markdown_table(stats_summary):
    md_lines = [
        "# Statistical Validation Analysis (95% Confidence Intervals)",
        "",
        "This table provides the **Mean and 95% Confidence Interval (CI)** for both throughput (GB/s) and latency ($\mu$s) across all message sizes. A narrower interval indicates higher predictability and lower run-to-run variance.",
        "",
        "| Size (B) | Metric | `io_uring` | POSIX Pipe | UNIX Socket | POSIX MQ |",
        "| :--- | :--- | :--- | :--- | :--- | :--- |"
    ]
    
    for sz in MESSAGE_SIZES:
        # Throughput row
        uring_t = f"{stats_summary[sz]['uring']['t_mean']:.3f} <br>[{stats_summary[sz]['uring']['t_ci'][0]:.3f}, {stats_summary[sz]['uring']['t_ci'][1]:.3f}]"
        pipe_t = f"{stats_summary[sz]['pipe']['t_mean']:.3f} <br>[{stats_summary[sz]['pipe']['t_ci'][0]:.3f}, {stats_summary[sz]['pipe']['t_ci'][1]:.3f}]"
        socket_t = f"{stats_summary[sz]['socket']['t_mean']:.3f} <br>[{stats_summary[sz]['socket']['t_ci'][0]:.3f}, {stats_summary[sz]['socket']['t_ci'][1]:.3f}]"
        mq_t = f"{stats_summary[sz]['mq']['t_mean']:.3f} <br>[{stats_summary[sz]['mq']['t_ci'][0]:.3f}, {stats_summary[sz]['mq']['t_ci'][1]:.3f}]"
        
        md_lines.append(f"| **{sz}** | Throughput | {uring_t} | {pipe_t} | {socket_t} | {mq_t} |")
        
        # Latency row
        uring_l = f"{stats_summary[sz]['uring']['l_mean']:.3f} <br>[{stats_summary[sz]['uring']['l_ci'][0]:.3f}, {stats_summary[sz]['uring']['l_ci'][1]:.3f}]"
        pipe_l = f"{stats_summary[sz]['pipe']['l_mean']:.3f} <br>[{stats_summary[sz]['pipe']['l_ci'][0]:.3f}, {stats_summary[sz]['pipe']['l_ci'][1]:.3f}]"
        socket_l = f"{stats_summary[sz]['socket']['l_mean']:.3f} <br>[{stats_summary[sz]['socket']['l_ci'][0]:.3f}, {stats_summary[sz]['socket']['l_ci'][1]:.3f}]"
        mq_l = f"{stats_summary[sz]['mq']['l_mean']:.3f} <br>[{stats_summary[sz]['mq']['l_ci'][0]:.3f}, {stats_summary[sz]['mq']['l_ci'][1]:.3f}]"
        
        md_lines.append(f"| | Latency | {uring_l} | {pipe_l} | {socket_l} | {mq_l} |")
        md_lines.append("| | | | | | |") # empty divider
        
    (ROOT / "figures" / "statistical_analysis.md").write_text("\n".join(md_lines), encoding="utf-8")
    print(f"Statistical validation markdown written to {ROOT / 'figures' / 'statistical_analysis.md'}")

def main():
    data = load_data()
    summary = run_statistics(data)
    plot_with_ci(summary)
    print_markdown_table(summary)
    return 0

if __name__ == "__main__":
    import sys
    sys.exit(main())
