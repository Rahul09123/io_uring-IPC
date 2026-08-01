# IEEE Submission Figure Snapshot

This directory is an older packaged figure snapshot under `IEEE Submission/`. The canonical current report and figure tree are at the repository root:

- `report.tex`
- `report.pdf`
- `figures/`
- `data/`

Do not assume that a file in this package is current merely because it has the same name as a root figure.

## Files in this snapshot

### Ablation

- `ablation/fig1_wakeup_latency.png`
- `ablation/fig2_throughput_regime.png`
- `ablation/fig3_cpu_latency_pareto.png`
- `ablation/fig4_syscalls_per_msg.png`
- `ablation/fig5_e2e_latency.png`
- `ablation/fig_supp_wakeup_heatmap.png`

The current root figure tree additionally contains `figures/ablation/fig2_saturated_throughput_size.png`; that file is not present in this packaged snapshot.

### Ping-pong

- `pingpong/fig_A_unloaded_latency.png`
- `pingpong/fig_B_tail_latency.png`
- `pingpong/fig_C_ablation_latency.png`

### Standalone transport analysis

- `throughput.png`, `throughput_ci.png`
- `latency.png`, `latency_ci.png`
- `speedup.png`
- `cache_misses.png`, `cache_misses_summary.csv`
- `statistical_analysis.md`, `summary.md`
- `flamegraphs/`

## Provenance and interpretation

- Controlled ablation figures are generated from root `data/ablation_results.csv` with `scripts/ablation_analysis.py`.
- Corrected depth-1 figures are generated from root ping-pong summaries with `scripts/pingpong_analysis.py`.
- Standalone throughput, latency, cache, and flamegraph artifacts come from the older per-transport harnesses through `scripts/generate_visualizations.py`.
- Standalone and controlled results use different workload sizing and must not be combined without an explicit provenance warning.
- SQPOLL is a separately labelled controlled configuration. The captured `final_io_uring.jpg` profile belongs to the default interrupt-mode standalone harness and is not an SQPOLL profile.

For submission, copy or regenerate figures from the root canonical tree and compile `report.tex`; do not build a final paper from the older TeX sources in this directory without reconciling them first.
