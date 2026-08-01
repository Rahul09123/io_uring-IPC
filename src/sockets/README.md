# Standalone UNIX Domain Socket Harness

This directory benchmarks a one-way `AF_UNIX`, `SOCK_STREAM` connection between a producer client and consumer server. It is retained for standalone transport reproduction. The corrected depth-1 socket implementation is `src/pingpong/pp_socket.cpp`.

## Implementation

- Socket path: `/tmp/ipc_socket_bench_<payload-size>`
- Producer requested on logical CPU 1; consumer on logical CPU 2
- Frame: send timestamp and payload size, followed by payload bytes
- Requested send and receive buffers: 2 MiB
- Transfer target: 2 GiB for every message size
- Sizes: 64 B through 1 MiB
- One discarded warmup followed by 15 recorded runs
- Consumer performs a 64-byte-stride payload checksum
- Output: `src/sockets/socket_results.csv` and the repository copy under `data/`

The server creates, binds, listens, and accepts a fresh socket for each run. Throughput timing starts **after** `accept()` and stops after payload reception, so connection setup is excluded from the measured transfer interval.

## Build and run

```bash
bash run_socket_bench.sh
```

Manual execution:

```bash
g++ -O3 -std=c++17 -Wall -o socket_producer socket_producer.cpp
g++ -O3 -std=c++17 -Wall -o socket_consumer socket_consumer.cpp
rm -f /tmp/ipc_socket_bench_*
./socket_consumer &
sleep 1.5
./socket_producer
wait
```

## Output columns

- `message_size_bytes`
- `run`
- `throughput_gbps` (historical name; value is GiB/s)
- `avg_latency_us`
- `stddev_us`
- `p50_us`
- `p95_us`
- `p99_us`

The streaming latency can include time queued in socket buffers. It is not equivalent to the unloaded depth-1 RTT/2 latency.

## Reproducibility and scope

- Linux or another Unix-like OS with UNIX domain sockets is required.
- Linux may double or cap requested socket-buffer values; inspect effective values when reproducing low-level behavior.
- Affinity reduces migration but does not eliminate preemption or guarantee distinct physical cores.
- This fixed-2-GiB standalone workload is not the controlled shared-ring wakeup ablation.

For current final latency comparisons, use `data/pingpong_socket_summary.csv` and the root `report.tex`.
