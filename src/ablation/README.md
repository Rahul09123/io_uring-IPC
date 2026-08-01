# Shared-Ring Wakeup Ablation

This suite holds the cache-aligned SPSC shared-memory ring and payload path constant while changing only `consumer_wait()` and `producer_signal()`. It is the controlled throughput, wakeup-latency, and CPU-efficiency experiment used by the final report.

## Configurations

The six primary variants are compiled from the same producer, consumer, and ring code:

| Name | Wait/signal behavior |
|---|---|
| `busy_poll` | Tight userspace polling; no wakeup syscall |
| `spin_backoff` | `_mm_pause()` with backoff/yield; no explicit wakeup |
| `adaptive` | Bounded spin followed by futex sleep/wake |
| `futex` | `FUTEX_WAIT` / `FUTEX_WAKE` |
| `eventfd` | `poll`/`read` and `write` on eventfd |
| `io_uring` | FIFO read/write submitted through interrupt-mode `io_uring` |

`io_uring` can also be run with `IORING_SETUP_SQPOLL` by setting `USE_SQPOLL=1`; the runner exposes this as `--sqpoll`. SQPOLL output is labelled `io_uring_sqpoll` and must be analysed separately from the six primary variants.

## Arrival regimes

- `saturated`: producer sends as quickly as possible.
- `bursty`: a 64-message burst followed by a fixed 1 ms gap.
- `offered_25`, `offered_50`, `offered_75`, `offered_90`: controlled offered-load levels derived from the configured inter-arrival gaps.

The offered-load and bursty protocols use different gap structures and are not repeated measurements of the same condition.

## Measurement controls

- Producer is requested on logical CPU 1 and consumer on logical CPU 2.
- `benchmark_env.sh` records the CPU model, kernel, selected cores, governor, and turbo/boost state.
- `--require-fixed-frequency` rejects a run unless the selected CPUs use the `performance` governor and turbo/boost is disabled.
- Each payload/configuration has one discarded warmup plus 15 recorded runs.
- Payload sizes are 64 B through 1 MiB.
- Transfer volume is size-scaled by `get_total_bytes()` in `common.h`.

Affinity reduces scheduler movement but does not guarantee that CPU IDs 1 and 2 are different physical cores on every machine. Check the host topology with `lscpu -e`.

## Build and run

From this directory:

```bash
bash run_ablation.sh --dry-run
bash run_ablation.sh --require-fixed-frequency
```

Run selected variants or regimes:

```bash
bash run_ablation.sh \
  --variant futex --variant eventfd --variant io_uring \
  --regime bursty --regime offered_90 \
  --require-fixed-frequency
```

Run SQPOLL separately:

```bash
bash run_ablation.sh \
  --variant io_uring --sqpoll \
  --require-fixed-frequency
```

Optional `--perf` runs the consumer under `perf stat` and records selected syscall, context-switch, and cache events. Availability depends on kernel permissions and tracepoints.

## Outputs

- `data/ablation_<variant>_<regime>.csv`: individual runs
- `data/ablation_results.csv`: merged data with corrected regime labels
- `data/environment_ablation.txt`: environment capture
- `data/ablation_perf_stat.txt`: optional performance-counter output
- `figures/ablation/fig1_wakeup_latency.png`
- `figures/ablation/fig2_throughput_regime.png`
- `figures/ablation/fig2_saturated_throughput_size.png`
- `figures/ablation/fig3_cpu_latency_pareto.png`
- `figures/ablation/fig4_syscalls_per_msg.png`
- `figures/ablation/fig5_e2e_latency.png`
- `figures/ablation/fig_supp_wakeup_heatmap.png`

## Current controlled findings

- At 64 KiB under saturation, the six primary variants span 13.37--15.75 GiB/s. Interrupt-mode `io_uring` records 13.37 GiB/s.
- At 1 MiB, the primary variants converge within 8.12--8.65 GiB/s.
- The separately measured SQPOLL configuration records 6.66 GiB/s at 64 KiB and 4.45 GiB/s at 1 MiB in this single-channel setup.
- At 64 B in the bursty protocol, futex, eventfd, interrupt-mode `io_uring`, and SQPOLL record median wakeup latencies of 1091.92, 1093.81, 1094.98, and 1058.47 us, respectively.

These numbers are protocol-level measurements, not isolated primitive syscall costs. Use the root `data/` files and `report.tex` as the source of truth; do not substitute legacy standalone transport results.
