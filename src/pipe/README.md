# Standalone POSIX Pipe Harness

This directory contains the original one-way POSIX pipe streaming benchmark. It is retained for transport-level reproduction. The corrected unloaded depth-1 implementation is `src/pingpong/pp_pipe.cpp`.

## Implementation

- Producer requested on logical CPU 1; consumer on logical CPU 2
- Framing: `MessageHeader` followed by the payload
- Data path: blocking `write()` and `read()` through the kernel pipe buffer
- Producer calls `sched_yield()` after each framed message
- Consumer performs a 64-byte-stride payload checksum
- One discarded warmup followed by 15 recorded runs
- Output: `src/pipe/pipe_results.csv` and the repository copy under `data/`

Transfer volume is size-scaled:

| Payload | Total bytes per run |
|---|---:|
| Up to 1 KiB | 32 MiB |
| Above 1 KiB through 64 KiB | 256 MiB |
| Above 64 KiB | 2 GiB |

## Build and run

```bash
bash run_pipe_bench.sh
```

Manual compilation:

```bash
g++ -O3 -std=c++17 -Wall -o pipe_producer pipe_producer.cpp -lrt
g++ -O3 -std=c++17 -Wall -o pipe_consumer pipe_consumer.cpp -lrt
```

The runner manages the named FIFO and process ordering. Review any privilege use before running on a shared machine.

## Output columns

- `message_size_bytes`
- `run`
- `throughput_gbps` (historical name; value is GiB/s)
- `avg_latency_us`
- `stddev_us`
- `p50_us`
- `p95_us`
- `p99_us`

The one-way streaming timestamps can include pipe residence and backlog. Do not compare them directly with the depth-1 RTT/2 values without explaining the different protocol.

## Reproducibility and scope

- Linux/POSIX pipe and affinity interfaces are required.
- Affinity to CPU IDs 1 and 2 reduces migration but does not eliminate preemption or prove that those IDs are distinct physical cores. Check `lscpu -e`.
- Pipe capacity and scheduling policy can affect blocking and throughput.
- This standalone workload is not the controlled shared-ring wakeup ablation.

For current final latency comparisons, use `data/pingpong_pipe_summary.csv` and the root `report.tex`.
