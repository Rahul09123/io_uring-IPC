# Analysis and Reproducibility Scripts

These scripts merge raw benchmark output, compute statistics, parse performance counters, and generate the figures used by the report. Run them from the repository root unless noted otherwise.

## Script inventory

| Script | Purpose |
|---|---|
| `ablation_analysis.py` | Generates the controlled ablation plots, including the saturated throughput size sweep. |
| `pingpong_analysis.py` | Generates depth-1 median and tail-latency figures from ping-pong summaries. |
| `generate_visualizations.py` | Generates the older standalone transport comparison, cache plots, summaries, and flamegraph gallery. |
| `statistical_analysis.py` | Computes confidence intervals and statistical comparisons for standalone CSVs. |
| `calc_percentiles.py` | Computes requested percentile summaries from compatible CSV data. |
| `merge_ablation.py` | Merges per-variant ablation CSVs and derives regime labels from filenames. |
| `parse_offered_load.py` | Summarises the 64 B offered-load sweep. |
| `parse_perf.py` | Parses recorded ablation `perf stat` output. |
| `run_offered_load.sh` | Convenience runner for offered-load experiments. The main ablation runner can also run these regimes. |
| `run_perf_stat.sh` | Convenience runner for selected `perf stat` experiments. |

`__pycache__/` is generated Python bytecode and is not a source artifact.

## Canonical analysis commands

The benchmark runners normally invoke their analysis scripts automatically. To regenerate figures manually:

```bash
python3 scripts/ablation_analysis.py \
  --data data/ablation_results.csv \
  --perf data/ablation_perf_stat.txt \
  --output figures/ablation

python3 scripts/pingpong_analysis.py \
  --data data/pingpong_results.csv \
  --output figures/pingpong
```

Check each script's current options with:

```bash
python3 scripts/ablation_analysis.py --help
python3 scripts/pingpong_analysis.py --help
python3 scripts/generate_visualizations.py --help
```

Generate the standalone/legacy transport plots:

```bash
python3 scripts/generate_visualizations.py
```

Those plots use the standalone `pipe_results.csv`, `socket_results.csv`, `mq_results.csv`, and `io_uring_results.csv` datasets. They must not be treated as the controlled shared-ring ablation because workload sizing and provenance differ.

## Inputs and outputs

- Canonical controlled input: `data/ablation_results.csv`
- Canonical depth-1 inputs: root `data/pingpong_*_summary.csv` files
- Merged depth-1 convenience file: `data/pingpong_results.csv`
- Environment metadata: `data/environment_ablation.txt` and `data/environment_pingpong.txt`
- Controlled plots: `figures/ablation/` and `figures/pingpong/`
- Standalone plots and profiling gallery: `figures/`

The paper deliberately uses canonical root summaries rather than duplicate files under `data/data/`.

## Dependencies

- Python 3
- Matplotlib
- NumPy where imported by the selected analysis
- Linux `perf` for performance-counter collection

Example:

```bash
sudo apt install -y python3 python3-matplotlib python3-numpy linux-perf
```

Performance-counter availability varies by kernel configuration, `perf_event_paranoid`, tracepoint support, and privileges. A missing counter must be reported as unavailable rather than converted to zero.
