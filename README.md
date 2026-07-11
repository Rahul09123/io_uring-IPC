# io_uring-IPC: Comparative IPC Benchmark Suite

This repository contains a complete experimental study of four Linux IPC implementations under a unified benchmark methodology:

- POSIX Pipe (FIFO)
- Unix Domain Socket
- POSIX Message Queue
- io_uring-assisted Shared Memory Ring

The project is designed for performance comparison using throughput, latency percentiles, cache behavior, and flamegraph-based profiling.

## 1. Study Objective

The objective is to compare practical end-to-end behavior of different IPC mechanisms for a producer-consumer workload across payload sizes from 64 bytes to 1 MiB.

Each implementation follows the same high-level structure:

1. Producer stamps each message with send timestamp.
2. Consumer receives payload and computes per-message latency.
3. Benchmark executes one warmup run and multiple measured runs for each payload size.
4. Results are exported to CSV for analysis and visualization.

## 2. Repository Structure

- [pipe](pipe): POSIX FIFO benchmark implementation
- [Sockets](Sockets): Unix domain socket benchmark implementation
- [Updated Message Queue](Updated%20Message%20Queue): POSIX mqueue benchmark implementation
- [uring](uring): io_uring + shared-ring benchmark implementation
- [scripts](scripts): figure generation tooling
- [figures](figures): generated plots and flamegraph gallery
- [FlameGraphs](FlameGraphs): raw flamegraph SVG assets
- [Cache Misses](Cache%20Misses): perf counter log used for cache-miss analysis
- [ipc_implementation_documentation.md](ipc_implementation_documentation.md): detailed cross-implementation narrative

## 3. Detailed Implementation Documents

Each implementation now has a detailed README with architecture, working, lock model, buffer sizing rationale, constants, syscalls, tuning notes, and limitations:

- [pipe/README.md](pipe/README.md)
- [Sockets/README.md](Sockets/README.md)
- [Updated Message Queue/README.md](Updated%20Message%20Queue/README.md)
- [uring/README.md](uring/README.md)

## 4. Benchmark Matrix and Shared Methodology

### 4.1 Message Size Sweep

The benchmark uses the same payload set for all implementations:

- 64
- 256
- 1024
- 4096
- 16384
- 65536
- 262144
- 1048576

### 4.2 Core Pinning

All implementations pin producer and consumer to fixed CPU cores to reduce scheduling noise.

### 4.3 Metrics Captured

Per measured run, each implementation records:

- Throughput
- Average latency
- Latency standard deviation
- P50 latency
- P95 latency
- P99 latency

### 4.4 Warmup Policy

Run 0 is a warmup run and is not written to CSV.

### 4.5 Payload Touch Policy

Consumer-side checksum touch is intentionally used to ensure payload memory is actually read, avoiding unrealistic compiler/runtime optimization artifacts.

## 5. High-Level Architecture Comparison

### 5.1 Pipe

- Kernel FIFO-backed stream
- No userspace locks
- Synchronization via blocking FIFO semantics

### 5.2 Unix Domain Socket

- AF_UNIX SOCK_STREAM transport
- Producer-connect and consumer-listen model
- Kernel socket queues provide ordering and backpressure

### 5.3 POSIX Message Queue

- Message-oriented kernel queue API
- Explicit queue attributes (depth and message size)
- Sensitive to kernel queue limits

### 5.4 io_uring Shared Ring

- Shared memory ring data path with atomic head and tail indices
- Lock-free SPSC synchronization
- io_uring SQPOLL attempted when supported, fallback when unavailable

## 6. Build and Run

Run each implementation via its own script from the implementation folder:

- Pipe: run_pipe_bench.sh
- Socket: run_socket_bench.sh
- Message Queue: run_mq_bench.sh
- io_uring: run_uring_bench.sh

After runs are complete, results CSV files should be available in repository root and implementation folders.

## 7. Visualization and Analysis

Generate publication-ready plots from root:

```bash
python3 scripts/generate_visualizations.py
```

This generates:

- [figures/throughput.png](figures/throughput.png)
- [figures/latency.png](figures/latency.png)
- [figures/speedup.png](figures/speedup.png)
- [figures/cache_misses.png](figures/cache_misses.png)
- [figures/cache_misses_summary.csv](figures/cache_misses_summary.csv)
- [figures/flamegraphs/index.html](figures/flamegraphs/index.html)
- [figures/summary.md](figures/summary.md)

## 8. Reproducibility Checklist

For reliable comparison, keep the following fixed between benchmark sessions:

1. Same kernel and machine.
2. Same core affinity mapping.
3. Same privilege mode when running scripts.
4. Same queue and descriptor limits for mqueue tests.
5. Cleanup of stale IPC objects before each run.
6. Minimal competing CPU load.

## 9. Environment and Dependencies

### 9.1 Required Platform

- Linux is required for full benchmark functionality (mqueue, io_uring, and Linux-specific tuning paths).

### 9.2 Build Dependencies

- g++ with C++17 support
- POSIX realtime support for relevant binaries
- liburing for io_uring implementation
- Python 3 with matplotlib for plotting

### 9.3 Optional Profiling Tools

- perf for cache-miss capture
- flamegraph generation tooling if regenerating SVGs

## 10. Output Artifacts in This Repository

Current repository includes sample outputs already generated:

- [pipe_results.csv](pipe_results.csv)
- [socket_results.csv](socket_results.csv)
- [mq_results.csv](mq_results.csv)
- [io_uring_results.csv](io_uring_results.csv)
- [Cache Misses](Cache%20Misses)
- [FlameGraphs](FlameGraphs)
- [figures](figures)

## 11. Academic Submission Notes

This repository now contains:

- Detailed implementation-level documentation in every IPC folder
- Project-level methodology and architecture explanation in this root README
- Reproducible run workflow
- Exported raw benchmark data and generated visual evidence

This is suitable as a detailed implementation submission package for review.
