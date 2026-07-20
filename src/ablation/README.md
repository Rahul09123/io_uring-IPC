# Wakeup Mechanism Ablation Study

## Overview

The ablation study isolates and benchmarks the performance of **6 distinct wakeup coordination strategies** on top of the same Single-Producer Single-Consumer (SPSC) shared-memory ring buffer layout. 

The goal of this study is to measure the latency, throughput, and CPU overhead trade-offs between spinning in userspace (ultra-low latency, 100% CPU usage) versus sleeping in the kernel (higher latency, 0% idle CPU usage) under different traffic regimes.

---

## Wakeup Variants

The 6 wakeup mechanisms evaluated are:

| Variant | Wait Strategy | Wakeup Signal | Idle CPU | Microsecond Latency | Use Case |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`busy_poll`** | Tight memory loop | None (always checking) | 100% Core | **Ultra-Low (<0.25 µs)** | High-Frequency Trading (HFT) |
| **`spin_backoff`**| Loop + `PP_PAUSE()` backoff | None (always checking) | High | **Very Low (<0.5 µs)** | Low-power userspace spinning |
| **`adaptive`** | Spin 500 iters, then sleep | `futex` WAKE | Low | **Dynamic (0.2 – 10 µs)** | General-purpose high-load systems |
| **`futex`** | System sleep (`FUTEX_WAIT`) | `futex` WAKE (`FUTEX_WAKE`) | **0%** | **Low-Medium (2 – 10 µs)** | Standard Linux thread sleep |
| **`eventfd`** | System block (`poll`/`read`) | Eventfd write | **0%** | **Low-Medium (2 – 10 µs)** | Kernel file-descriptor event loop |
| **`io_uring`** | `io_uring_submit_and_wait` | Write to Signal FIFO | **0%** | **Optimized Medium** | Asynchronous event-driven IPC |

---

## Traffic Regimes

The variants are swept across two distinct arrival traffic patterns:
1. **`saturated`**: The producer streams messages back-to-back as fast as possible to measure maximum possible throughput.
2. **`bursty`**: The producer groups messages into bursts (every 64 messages) followed by a **1 ms sleep gap** to force the consumer thread to sleep and exercise core wake-up latencies.

---

## Directory Structure

* **`common.h`**: Defines target affinity, message sizes, and shared-memory naming configurations.
* **`ring.h`**: Configures the cache-aligned (`alignas(64)`) `RingBuffer` struct to prevent false sharing.
* **`wakeup.h`**: Implements the wait/signal interfaces for all 6 variants.
* **`consumer.cpp`**: Spawns the consumer thread, maps shared memory, executes the benchmark hot loops, and records telemetry.
* **`producer.cpp`**: Spawns the producer thread, synchronizes with the consumer via the mapping barrier, and sends payload streams.
* **`run_ablation.sh`**: Orchestrates the entire compilation, execution, CSV merging, and performance tracking flow.

---

## Execution and Reproducibility

### 1. Basic Run
To run the full suite (variant × regime × payload sizes) cleanly:
```bash
cd src/ablation
bash run_ablation.sh
```
*Outputs are saved under `data/ablation_*.csv` and merged into `data/ablation_results.csv`.*

### 2. Dry Run (Compile Only)
To verify compilation on your machine:
```bash
bash run_ablation.sh --dry-run
```

### 3. Profiling Cache Misses & Syscalls
To capture context-switches, system calls, L1 cache loads/misses, and LLC loads/misses using `perf stat`:
```bash
bash run_ablation.sh --perf
```
*Performance stats are stored in `data/ablation_perf_stat.txt`.*

### 4. Running a Specific Configuration
To sweep only a subset of variants or regimes:
```bash
bash run_ablation.sh --variant futex --variant eventfd --regime bursty
```

---

## Analysis & Visualizations

After completing the runs, you can parse the raw data and generate figures using the Python analysis scripts:
```bash
cd ../../
python3 scripts/ablation_analysis.py --data data/ablation_results.csv --perf data/ablation_perf_stat.txt --output figures/ablation/
```
This produces:
* `figures/ablation/fig1_wakeup_latency.png` (evaluates wakeup latencies in bursty conditions)
* `figures/ablation/fig2_throughput_regime.png` (compares saturated throughput vs bursty payload sweeps)
