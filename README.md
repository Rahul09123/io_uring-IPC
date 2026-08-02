# Linux IPC Wakeup and Shared-Memory Ring Measurement Study

This repository contains a controlled Linux IPC performance study. It separates two questions that are often conflated:

1. How much performance comes from moving payloads through a cache-aligned shared-memory SPSC ring?
2. Once that data path is fixed, how do busy polling, spin backoff, adaptive waiting, futex, eventfd, interrupt-mode `io_uring`, and SQPOLL compare as wakeup mechanisms?

The central result is that the shared-memory ring supplies the performance gain. For a single SPSC wakeup, interrupt-mode `io_uring` is competitive with futex and eventfd under bursty traffic but has higher strict depth-1 per-event latency. SQPOLL was measured separately and did not improve this single-channel workload.

## Current canonical results

All final claims are taken from the root `data/` CSV files and the current paper source, [`report.tex`](report.tex). Older standalone transport CSVs are retained for provenance but are not mixed with the controlled ablation conclusions.

### Depth-1 ping-pong, 64 B, median single-trip latency

| Transport / wakeup | P50 (us) |
|---|---:|
| SHM busy poll | 0.185 |
| SHM adaptive | 0.211 |
| SHM futex | 2.562 |
| SHM eventfd | 2.602 |
| Pipe | 2.710 |
| UNIX socket | 2.770 |
| POSIX MQ | 2.861 |
| SHM interrupt-mode `io_uring` | 4.173 |
| SHM `io_uring` SQPOLL-assisted signaling | 6.912 |

The protocol has queue depth one. The initiator records both timestamps on CPU 1 with `CLOCK_MONOTONIC_RAW`, and single-trip latency is RTT/2.

### Controlled saturated shared-ring throughput

| Wakeup configuration | 64 KiB (GiB/s) | 1 MiB (GiB/s) |
|---|---:|---:|
| Busy poll | 15.75 | 8.65 |
| Spin backoff | 14.54 | 8.61 |
| Adaptive | 15.53 | 8.36 |
| Futex | 15.60 | 8.18 |
| Eventfd | 13.74 | 8.37 |
| Interrupt-mode `io_uring` | 13.37 | 8.12 |
| `io_uring` SQPOLL, separate run | 6.66 | 4.45 |

The legacy direct `io_uring` harness contains a 28.17 GiB/s observation from a different workload and environment. It is not used as a controlled cross-mechanism result.

### Frequency-pinned wakeup experiments

The bursty and offered-load runs were collected with the benchmark processes pinned to CPUs 1 and 2, the `performance` governor enabled on both CPUs, and turbo disabled. Environment records are stored in:

- `data/environment_ablation.txt`
- `data/environment_pingpong.txt`

At 64 B in the bursty protocol, the mean run-level wakeup P50 was 1091.92 us for futex, 1093.81 us for eventfd, 1094.98 us for interrupt-mode `io_uring`, and 1058.47 us for SQPOLL. These values include the protocol's idle-gap behavior and must not be interpreted as isolated syscall latency.

## Experimental suites

### 1. Shared-ring wakeup ablation

`src/ablation/` holds the payload ring constant and varies only waiting and signaling. The six primary variants are:

- `busy_poll`
- `spin_backoff`
- `adaptive`
- `futex`
- `eventfd`
- interrupt-mode `io_uring`

SQPOLL is an additional, separately labelled `io_uring` configuration. The arrival regimes are `saturated`, `bursty`, `offered_25`, `offered_50`, `offered_75`, and `offered_90`.

### 2. Corrected depth-1 ping-pong

`src/pingpong/` compares pipe, UNIX socket, POSIX MQ, SHM plus interrupt-mode `io_uring`, and the six shared-ring wakeup variants. It reports P50, P90, P99, P99.9, and a 95% confidence interval for every successful size.

Measured rounds are size-scaled:

| Payload | Rounds |
|---|---:|
| 64 B, 256 B, 1 KiB | 100,000 each |
| 4 KiB | 50,000 |
| 16 KiB | 20,000 |
| 64 KiB | 10,000 |
| 256 KiB | 5,000 |
| 1 MiB | 2,000 |

POSIX MQ has canonical results through 4 KiB. The 16 KiB and larger configurations are `unsupported` under the recorded kernel message-size limit; the report preserves `N/A` rather than estimating values.

## Requirements

- Bare-metal Linux is recommended for publication measurements. A VM is suitable for compilation and functional checks but adds hypervisor scheduling and virtual-timer noise.
- Linux kernel with `io_uring` support
- `g++`, `liburing-dev`, Python 3, Matplotlib, and NumPy
- `cpupower` or equivalent governor controls
- Permission to use SQPOLL where required by the host kernel

Example Ubuntu installation:

```bash
sudo apt update
sudo apt install -y build-essential g++ liburing-dev python3 python3-matplotlib python3-numpy linux-tools-common linux-tools-$(uname -r)
```

Before a final measurement run, configure the selected CPUs according to the host's driver and verify the result. Typical Intel commands are:

```bash
sudo cpupower frequency-set -g performance
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

The scripts only record and validate these settings; they do not silently change them. Use `--require-fixed-frequency` to fail instead of continuing when validation is unsuccessful.

## Running the controlled experiments

From the repository root:

```bash
cd src/ablation
bash run_ablation.sh --require-fixed-frequency

cd ../pingpong
bash run_pingpong.sh --require-fixed-frequency
```

Run SQPOLL separately so it cannot be confused with interrupt mode:

```bash
cd src/ablation
bash run_ablation.sh --variant io_uring --sqpoll --require-fixed-frequency
```

Useful subset examples:

```bash
bash src/ablation/run_ablation.sh --variant futex --variant io_uring --regime bursty --regime offered_90
bash src/pingpong/run_pingpong.sh --ipc pipe --ipc unix_socket
```

## Repository layout

- `src/ablation/`: controlled shared-ring wakeup ablation
- `src/pingpong/`: corrected depth-1 latency suite
- `src/pipe/`, `src/sockets/`, `src/mq/`, `src/io_uring/`: standalone legacy throughput harnesses
- `scripts/`: merging, analysis, statistics, perf parsing, and figure generation
- `data/`: canonical CSVs, legacy CSVs, and captured environment metadata
- `figures/`: generated publication figures
- `report.tex` and `report.pdf`: current final report
- `IEEE Submission/`: older submission packaging retained for reference; it is not the canonical report source

## Reproducibility cautions

- Do not combine results from different machines, governors, turbo states, harnesses, or transfer volumes in one comparison table.
- CPU affinity reduces scheduling variation but does not prove that two logical CPU IDs are separate physical cores. Confirm topology with `lscpu -e`.
- CPU-utilization columns describe sampled process CPU time; values near zero are not proof of exactly zero CPU consumption.
- SQPOLL results apply to the tested single-channel, 64-slot configuration and do not establish a general SQPOLL limit.
- `throughput_gbps` is a historical CSV column name; calculations use GiB/s.

## Authors

- Rahul Raman
- Yuvraj Deshmukh
- B. Thangaraju

See [`report.tex`](report.tex) for the authoritative author affiliations, complete methodology, limitations, and bibliography.
