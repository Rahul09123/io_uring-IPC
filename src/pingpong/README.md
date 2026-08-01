# Corrected Depth-1 Ping-Pong Suite

This suite measures unloaded request/response latency without pipelining or queue-backlog delay. The initiator sends one payload, waits for its echo, and only then begins the next round.

## Timing protocol

- Queue depth is exactly one.
- Both timestamps are taken by the initiator on logical CPU 1.
- The clock is `CLOCK_MONOTONIC_RAW`.
- Reported single-trip latency is round-trip time divided by two.
- The echo process is requested on logical CPU 2.
- 10,000 warmup rounds are discarded for each supported size.
- P50, P90, P99, P99.9, mean, and 95% confidence bounds are written to the summary CSV.

Using one clock avoids cross-core timestamp subtraction. CPU affinity reduces scheduler movement but does not itself prove that the selected logical CPUs are different physical cores.

## Implementations

- `pp_pipe.cpp`: pair of POSIX pipes
- `pp_socket.cpp`: UNIX domain socket
- `pp_mq.cpp`: pair of POSIX message queues
- `pp_shm_uring.cpp`: shared memory with interrupt-mode or SQPOLL `io_uring` FIFO wakeups
- `pp_ablation.cpp`: shared-memory ring across busy poll, spin backoff, adaptive, futex, eventfd, and interrupt-mode `io_uring`

SQPOLL is evaluated by the throughput/wakeup ablation and can also be run as a
separate depth-1 `shm_io_uring` configuration.  It is never mixed into the
primary six-variant ablation table; its summary is written separately.

## Payloads and measured rounds

| Payload | Measured rounds |
|---|---:|
| 64 B | 100,000 |
| 256 B | 100,000 |
| 1 KiB | 100,000 |
| 4 KiB | 50,000 |
| 16 KiB | 20,000 |
| 64 KiB | 10,000 |
| 256 KiB | 5,000 |
| 1 MiB | 2,000 |

The counts are deliberately size-scaled to keep execution time manageable while retaining tail-percentile capture at large sizes.

## POSIX MQ size support

The MQ implementation creates queues with `mq_msgsize` equal to the current payload size. Kernel limits can reject larger sizes. In the canonical repository data:

- 64 KiB completed with 10,000 rounds.
- 1 MiB is `N/A` and must remain explicitly unavailable.

Changing `fs.mqueue.msgsize_max` permits a new run but does not retroactively create a valid result. Record the new environment and rerun the suite before replacing `N/A`.

## Build and run

```bash
bash run_pingpong.sh --dry-run
bash run_pingpong.sh --require-fixed-frequency
```

Run a subset:

```bash
bash run_pingpong.sh --ipc pipe --ipc unix_socket
bash run_pingpong.sh --ipc posix_mq
bash run_pingpong.sh --ipc shm_ablation --variant futex --variant io_uring
bash run_pingpong.sh --require-fixed-frequency --ipc shm_io_uring --sqpoll
```

Accepted IPC names are `pipe`, `unix_socket`, `shm_io_uring`, `posix_mq`, and `shm_ablation`.

## Outputs

- `data/pingpong_<ipc>_summary.csv`
- `data/pingpong_ablation_<variant>_summary.csv`
- `data/pingpong_shm_uring_sqpoll_summary.csv`
- `data/pingpong_results.csv`
- `data/environment_pingpong.txt`
- `figures/pingpong/fig_A_unloaded_latency.png`
- `figures/pingpong/fig_B_tail_latency.png`
- `figures/pingpong/fig_C_ablation_latency.png`

The root per-transport summary CSVs are authoritative. Do not substitute older duplicate summaries under `data/data/`.
