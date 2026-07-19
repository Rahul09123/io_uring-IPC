# Wakeup-Mechanism Ablation Experiment

## Purpose

This experiment answers the question:

> **What does io_uring actually buy for SPSC IPC?**

The data path (cache-aligned shared-memory SPSC ring) is held **completely fixed** across
all variants. Only the **wakeup mechanism** used when the ring is empty (consumer) or full
(producer) is varied. This directly addresses the reviewer objection that the main
benchmark conflates shared-memory ring gains with io_uring-specific gains.

---

## The 6 Wakeup Variants

| # | Name | Consumer wait | Producer signal | Syscalls/wakeup |
|---|------|--------------|-----------------|-----------------|
| 0 | `busy_poll` | spin on head≠tail | nothing | 0 |
| 1 | `spin_backoff` | spin + `_mm_pause()` + `sched_yield()` | nothing | ~0 |
| 2 | `adaptive` | spin 500 iters → `futex_wait` | `futex_wake` | rare |
| 3 | `futex` | `futex(FUTEX_WAIT)` immediately | `futex(FUTEX_WAKE)` | 2 |
| 4 | `eventfd` | blocking `read()` on eventfd | `write()` uint64 | 2 |
| 5 | `io_uring` | `io_uring_submit_and_wait` on FIFO read | `io_uring_prep_write` | ~3 |

Variant 5 (`io_uring`) is **verbatim** from the existing `src/io_uring/` implementation,
so it serves as a regression anchor — its results must be within 5% of `data/io_uring_results.csv`.

---

## The 6 Arrival Regimes

| Regime | Description | Inter-arrival |
|--------|-------------|--------------|
| `saturated` | producer as fast as possible | 0 |
| `bursty` | 1 message then 1 ms gap | 1000 µs |
| `offered_25` | ~25% of saturated throughput | 1200 µs |
| `offered_50` | ~50% | 600 µs |
| `offered_75` | ~75% | 200 µs |
| `offered_90` | ~90% | 50 µs |

---

## File Structure

```
src/ablation/
├── common.h          — enums, constants, CSV schema
├── ring.h            — SPSC ring + WakeupState + Telemetry
├── wakeup.h          — 6 wakeup implementations (inline, one namespace each)
├── consumer.cpp      — benchmarking consumer (runtime variant dispatch)
├── producer.cpp      — benchmarking producer (runtime variant dispatch)
├── run_ablation.sh   — orchestration: compile → sweep → merge → figures
└── README.md         — this file

scripts/
├── merge_ablation.py   — merge per-variant CSVs into data/ablation_results.csv
└── ablation_analysis.py — generate 6 figures from the merged CSV
```

---

## Prerequisites (Linux, kernel ≥ 5.1)

```bash
sudo apt update
sudo apt install -y liburing-dev linux-perf g++ python3-matplotlib python3-numpy

# Allow perf stat without root (needed for Figure 4 — syscalls/msg)
sudo sysctl -w kernel.perf_event_paranoid=0
```

---

## Running the Experiment

### Full sweep (~20–40 min)
```bash
cd /path/to/io_uring-IPC/src/ablation
bash run_ablation.sh
```

### Compile only (regression-check before running)
```bash
bash run_ablation.sh --dry-run
```

### Key comparison subset (io_uring vs futex, ~5 min)
```bash
bash run_ablation.sh \
    --variant futex --variant io_uring \
    --regime saturated --regime bursty
```

### With perf stat (requires sudo or paranoia=0)
```bash
bash run_ablation.sh --perf
```

### Regenerate figures from an existing CSV
```bash
cd /path/to/io_uring-IPC
python3 scripts/ablation_analysis.py \
    --data data/ablation_results.csv \
    --perf data/ablation_perf_stat.txt \
    --output figures/ablation
```

---

## Output Files

| File | Contents |
|------|----------|
| `data/ablation_results.csv` | All benchmark numbers (merged) |
| `data/ablation_perf_stat.txt` | Raw `perf stat` output |
| `figures/ablation/fig1_wakeup_latency.png` | **Core result** — wakeup latency p50/p99 |
| `figures/ablation/fig2_throughput_regime.png` | Convergence under saturation |
| `figures/ablation/fig3_cpu_latency_pareto.png` | CPU vs latency trade-off frontier |
| `figures/ablation/fig4_syscalls_per_msg.png` | Kernel entry frequency per variant |
| `figures/ablation/fig5_e2e_latency.png` | End-to-end latency vs message size |
| `figures/ablation/fig_supp_wakeup_heatmap.png` | Wakeup rate across all combos |

---

## Expected Findings

| Finding | Explanation |
|---------|-------------|
| Under **saturation**, all variants converge in throughput | Ring rarely empties → wakeup path rarely exercised |
| Under **bursty**, busy-poll has lowest wakeup latency but highest CPU | It never sleeps, so it catches data instantly |
| **io_uring adds little over futex** for single-ring SPSC | Both incur a syscall per wakeup; io_uring overhead adds ~10-20% |
| **Adaptive** is the best latency-CPU trade-off | Spins for short idle periods, falls back to futex for long ones |
| `wakeups_triggered ≈ 0` under saturation | Confirms ring-ring gains, not wakeup gains, dominate throughput |
| `wakeups_triggered ≈ total_messages` under bursty | Every message arrives to an empty ring |

---

## CSV Column Schema

```
wakeup_variant      — one of: busy_poll spin_backoff adaptive futex eventfd io_uring
regime              — one of: saturated bursty offered_25 offered_50 offered_75 offered_90
message_size_bytes  — 64 to 1048576
run                 — 1–15 (run 0 is warmup, excluded from CSV)
throughput_gbps     — observed throughput
avg_latency_us      — mean end-to-end latency
stddev_us           — standard deviation
p50_us p95_us p99_us p999_us — end-to-end latency percentiles
wakeup_latency_p50_us wakeup_latency_p99_us — wakeup-only latency percentiles
wakeups_triggered   — number of times consumer_sleeping CAS succeeded
cpu_us_per_msg      — CPU thread time / number of messages
```
