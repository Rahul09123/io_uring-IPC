# Scripts Directory

This directory contains the automation and visualization tooling used to analyze the results of the IPC benchmarks.

## 1. Overview of Tools

The primary utility in this directory is:
- **`generate_visualizations.py`**: A Python 3 script that reads the benchmark output CSV files, parses cache-miss hardware perf logs, processes profiling flamegraphs, and outputs publication-grade visual aids.

---

## 2. Detailed Code Explanation: `generate_visualizations.py`

The script is structured as a modular data processing and plotting pipeline. Below is a comprehensive breakdown of its key components, functions, and logic:

### 2.1 Configuration and Constants
- **`MESSAGE_SIZES`**: Defines the list of message payload sizes sweeped during testing: `[64, 256, 1024, 4096, 16384, 65536, 262144, 1048576]`.
- **`BenchmarkConfig`**: A frozen dataclass storing metadata for each IPC transport:
  - `key`: Internal identifier (`pipe`, `socket`, `mq`, `uring`).
  - `label`: Pretty-printed display label for graph legends.
  - `csv_name`: The filename of the raw input benchmark data.
  - `color`: Hex color codes assigned to keep the visualization color schemes consistent.

### 2.2 Data Ingestion Functions
- **`load_csv_rows(path: Path) -> List[dict]`**: Opens a benchmark CSV file and parses every column, converting fields to correct types (integers for sizes/runs, floats for latencies/throughput).
- **`group_by_size(rows, metric) -> Dict[int, List[float]]`**: Groups all recorded runs by their message size class. For example, it aggregates the throughputs of all 15 runs for the 4096-byte message class.

### 2.3 Statistical Aggregators
- **`summarize_grouped_distribution(grouped) -> (sizes, medians, Q1, Q3, means)`**:
  - Takes the grouped metric values for each size.
  - Computes the mathematical **median** (P50) and **mean**.
  - Calculates the **Interquartile Range (IQR)** by identifying the lower quartile ($Q_1$ or 25th percentile) and upper quartile ($Q_3$ or 75th percentile) of the distribution. These quartiles are used to plot error bands, providing insight into the variance and consistency of each IPC mechanism.

### 2.4 Matplotlib Style Configuration
- **`setup_plot_style()`**: Configures Matplotlib parameters (`plt.rcParams`) for high-quality figures:
  - Style: Uses the clean `seaborn-v0_8-whitegrid` styling theme.
  - DPI: Sets `dpi=170` for screen viewing and `savefig.dpi=260` for crisp image exports.
  - Grid: Enables a subtle grid with an alpha transparency of `0.22` for easy reading.
- **`format_size_ticks(ax)`**: Configures the X-axis as log-scale base 2 to match the exponential increments in the message sizes, labeling ticks with the exact byte counts.

### 2.5 Plotting Implementations
- **`plot_metric_comparison(benchmark_data, metric, ylabel, title, output_path, ...)`**:
  - Sweeps through the four benchmark types.
  - Plots the median metric values as a solid line.
  - If `show_iqr` is enabled, it uses `ax.fill_between` to draw a light-colored band between the $Q_1$ and $Q_3$ quartiles, illustrating latencies or throughput variance across runs.
- **`plot_speedup_vs_uring(benchmark_data, output_path)`**:
  - Compares `io_uring` throughput directly against the other baselines.
  - For each message size, computes:
    $$\text{Ratio} = \frac{\text{Median Throughput of io\_uring}}{\text{Median Throughput of Baseline IPC}}$$
  - Plots this ratio to show the factor-speedup of the shared memory ring over POSIX Pipes, Unix Sockets, and Message Queues.
- **`load_cache_miss_summary(path: Path) -> List[dict]`**:
  - Parses the raw text output of the hardware performance counters logger (`Cache Misses`).
  - Utilizes regular expressions (`re.compile`) to match Command Headers and retrieve counts for:
    - `L1-dcache-loads` & `L1-dcache-load-misses`
    - `LLC-loads` & `LLC-load-misses`
  - Computes corresponding L1 and LLC data-cache miss rates.
- **`plot_cache_misses(summaries, output_path, summary_csv)`**:
  - Generates a side-by-side sub-plot comparison bar chart (`1` row, `2` columns).
  - The left chart displays the L1 Data Cache miss rate.
  - The right chart displays the Last Level Cache (LLC) miss rate.
  - Normalizes the rates as percentages for straightforward hardware efficiency comparisons.

### 2.6 HTML Gallery and Markdown Exporter
- **`build_flamegraph_gallery(root, output_dir)`**:
  - Discovers SVG flamegraph traces inside the `FlameGraphs` folder.
  - Copies them into the `figures/flamegraphs` directory.
  - Dynamically builds a responsive, clean `index.html` dashboard, embedding the SVGs as interactive `<object>` elements. This allows users to zoom and search within call stacks directly in their browsers.
- **`write_summary_markdown(root, output_dir)`**: Exports a simple `summary.md` overview detailing the location of generated assets.

---

## 3. How to Run

Before running, ensure you have completed the IPC benchmark runs so that the results CSV files exist in the repository root directory.

Run the visualization script from the repository root:
```bash
python3 scripts/generate_visualizations.py
```

### 3.1 Custom Paths
You can specify custom source or destination folders using command-line arguments:
```bash
python3 scripts/generate_visualizations.py --root /path/to/repo --output /path/to/output_figures
```

---

## 4. Dependencies

The scripts require standard Python 3 along with:
- **`matplotlib`**: The plotting library.

Install it via `pip` if not present:
```bash
pip3 install matplotlib
```
