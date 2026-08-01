# Standalone SHM + Interrupt-Mode io_uring Harness

This directory contains the original standalone streaming benchmark for a cache-aligned shared-memory SPSC ring with `io_uring`-submitted FIFO wakeups.

It is retained for implementation history and direct-harness reproduction. It is **not** the controlled wakeup ablation used for the final cross-variant conclusions. Use `src/ablation/` for controlled throughput and wakeup comparisons and `src/pingpong/` for corrected depth-1 latency.

## Architecture

- Producer requested on logical CPU 1.
- Consumer requested on logical CPU 2.
- Payloads are copied by the producer into 64 shared-memory slots.
- `head`, `tail`, and `consumer_sleeping` are separated and cache-line aligned.
- The consumer reads payloads directly from the mapped shared-memory slots, avoiding a kernel payload copy after the producer's userspace `memcpy`.
- A named FIFO carries wakeup bytes only; payloads never travel through the FIFO.
- Both `io_uring_queue_init` calls use `flags=0`, so this harness is interrupt mode, not SQPOLL.

“No kernel payload copy” is more precise than “zero copy”: the producer still copies the source payload into the shared slot.

## Wakeup protocol

When the ring appears empty, the consumer:

1. publishes `consumer_sleeping = 1`;
2. checks the ring again to close the lost-wakeup window;
3. submits an `IORING_OP_READ` on `/tmp/uring_sig_fifo` and waits for its completion.

When the producer publishes a slot and observes the sleeping flag, it atomically clears the flag, submits an `IORING_OP_WRITE`, and waits for the CQE before reusing the stack-backed wakeup byte. Submission uses `io_uring`, but this producer path is not fully non-blocking because it explicitly calls `io_uring_wait_cqe()`.

## Workload

- Sizes: 64 B, 256 B, 1 KiB, 4 KiB, 16 KiB, 64 KiB, 256 KiB, and 1 MiB
- Ring depth: 64 slots
- Transfer volume: 2 GiB for every size
- Runs: one discarded warmup plus 15 recorded runs
- Output: `data/io_uring_results.csv`
- Historical column `throughput_gbps` is calculated using GiB/s units.

This workload differs from the dynamically sized controlled ablation. Its numerical results must not be placed in the same comparison table without an explicit cross-harness caveat.

## Build and run

```bash
bash run_uring_bench.sh
```

Manual build:

```bash
g++ -O3 -std=c++17 -Wall -o uring_producer uring_producer.cpp -luring -lrt
g++ -O3 -std=c++17 -Wall -o uring_consumer uring_consumer.cpp -luring -lrt

rm -f /tmp/uring_sig_fifo
./uring_consumer &
sleep 1.5
./uring_producer
wait
```

The programs create and unlink `/ipc_uring_ring_buffer`. Avoid broad `/dev/shm` cleanup commands on shared systems.

## Output columns

- `message_size_bytes`
- `run`
- `throughput_gbps` (GiB/s)
- `avg_latency_us`
- `stddev_us`
- `p50_us`
- `p95_us`
- `p99_us`

The per-message streaming latency in this harness can include time spent queued behind earlier ring entries. It is not equivalent to the unloaded RTT/2 result from the depth-1 ping-pong suite.

## Scope and limitations

- Linux and `liburing` are required.
- CPU IDs 1 and 2 are logical IDs; confirm physical topology separately.
- The standalone result has different transfer sizing and environment provenance from the final controlled experiment.
- Cache-counter correlations do not by themselves prove that alignment caused an observed end-to-end speedup.
- SQPOLL support and results belong to the controlled ablation runner (`--variant io_uring --sqpoll`), not this program.

For current reported values, see the root `report.tex` and canonical `data/ablation_*.csv` files.
