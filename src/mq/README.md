# Standalone POSIX Message Queue Harness

This directory benchmarks one producer and one consumer communicating through a POSIX message queue. It is a standalone streaming harness retained for transport-level reproduction; the corrected depth-1 MQ latency implementation is `src/pingpong/pp_mq.cpp`.

## Implementation

- Queue name: `/ipc_mq_bench_<payload-size>`
- Producer requested on logical CPU 1; consumer on logical CPU 2
- Queue depth: `mq_maxmsg = 10`
- Queue message size: `sizeof(MessageHeader) + payload_size`
- Header fields: send timestamp and payload size
- One discarded warmup followed by 15 recorded runs
- Output: `data/mq_results.csv`

The transfer target is size-scaled:

| Payload | Total bytes per run |
|---|---:|
| Up to 1 KiB | 32 MiB |
| Above 1 KiB through 64 KiB | 256 MiB |
| Above 64 KiB | 2 GiB |

The consumer touches the payload at a 64-byte stride before recording the message as consumed. The historical `throughput_gbps` column is computed in GiB/s.

## Build and run

The runner changes system-wide POSIX MQ limits and therefore requires appropriate administrative permission:

```bash
bash run_mq_bench.sh
```

It compiles the producer and consumer, applies these settings, cleans benchmark queues, and runs the pair:

- `ulimit -n 65535`
- `ulimit -q 209715200`
- `fs.mqueue.queues_max=2048`
- `fs.mqueue.msgsize_max=1048600`

Review the script before using it on a shared machine because kernel settings are system-wide. Prefer deleting only this benchmark's named queues rather than unrelated `/dev/mqueue` entries.

Manual compilation:

```bash
g++ -O3 -std=c++17 -Wall -o mq_producer mq_producer.cpp -lrt
g++ -O3 -std=c++17 -Wall -o mq_consumer mq_consumer.cpp -lrt
```

## Output columns

- `message_size_bytes`
- `run`
- `throughput_gbps` (GiB/s)
- `avg_latency_us`
- `stddev_us`
- `p50_us`
- `p95_us`
- `p99_us`

Streaming latency can include queue residence time and is not interchangeable with unloaded depth-1 RTT/2 latency.

## Relationship to the final ping-pong data

The ping-pong suite creates two queues per size and uses size-scaled round counts. In the canonical root summaries:

- POSIX MQ completed through 4 KiB; the 4 KiB run contains 50,000 round trips.
- The 16 KiB and larger rows are `unsupported` because those configurations
  exceed the recorded host's message-size limit, and the report displays them
  as `N/A`.

The standalone harness's privileged tuning does not make an absent ping-pong observation valid. To replace `N/A`, configure the new host, record its environment, rerun `src/pingpong/run_pingpong.sh --ipc posix_mq`, and retain the resulting status field.

## Limitations

- Linux POSIX MQ support and a mounted `/dev/mqueue` filesystem are required.
- Queue limits, per-user MQ memory limits, and privileges materially affect supported sizes.
- Logical CPU affinity does not guarantee separate physical cores.
- Standalone results use a different workload from the controlled shared-ring ablation.
