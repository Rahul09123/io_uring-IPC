# POSIX Message Queue Implementation

## 1. Overview
This implementation benchmarks IPC using POSIX message queues (`mqueue`) with one producer and one consumer.

- Transport: POSIX message queue (`mq_open`, `mq_send`, `mq_receive`)
- Pattern: Producer -> named queue -> Consumer
- Measurement: Throughput and latency percentiles
- Output: `mq_results.csv`

## 2. Architecture

### 2.1 Process Topology
- Producer (`mq_producer`): sends framed messages to queue.
- Consumer (`mq_consumer`): receives frames, computes latency and throughput, writes CSV.
- Shared telemetry memory (`/ipc_mq_telemetry`): used by consumer stats path.

### 2.2 Queue Naming Strategy
A distinct queue name is used per message size:
- Queue name format: `/ipc_mq_bench_<size>`

This isolates each size class and avoids stale queue state between phases.

### 2.3 CPU Affinity
From `common.h`:
- Producer core: `PRODUCER_CORE = 1`
- Consumer core: `CONSUMER_CORE = 2`

### 2.4 Why No Explicit Locks Are Used
No user-space lock is required for producer/consumer coordination:
- Queue serialization and synchronization are provided by kernel mqueue implementation.
- `mq_send` and `mq_receive` define ordering and blocking semantics.
- Producer/consumer communicate only via queue payloads (plus consumer-local telemetry memory), so no shared mutable userspace structure needs mutex protection.

## 3. Data Path and Working

### 3.1 Message Format
Queue payload is a packed frame:
- `MessageHeader`
  - `send_ns`
  - `payload_size`
- Payload bytes

`mq_msgsize` is configured as `sizeof(MessageHeader) + current_payload_size`.

### 3.2 Producer Flow
For each message size:
1. Derive unique queue name.
2. Open queue in write mode (retry until consumer creates it).
3. For each run (warmup + measured):
   - Stamp `send_ns`.
   - Send frame via `mq_send` until size-specific total bytes are produced.
4. Close queue.

### 3.3 Consumer Flow
For each message size:
1. Remove old queue name (`mq_unlink`).
2. Create queue with attributes:
   - `mq_maxmsg = 10`
   - `mq_msgsize = sizeof(MessageHeader) + sz`
3. For each run (warmup + measured):
   - Receive messages using `mq_receive`.
   - Compute per-message latency from send/receive timestamps.
   - Continue until target byte volume for that size is consumed.
4. Compute stats and append measured runs to CSV.
5. Close and unlink queue.

### 3.4 Workload Sizing
Transfer target scales by payload size (`get_total_bytes`):
- `<= 1 KiB`: 32 MiB
- `<= 64 KiB`: 256 MiB
- `> 64 KiB`: 2 GiB

This avoids excessive runtime for tiny messages while preserving stress at larger sizes.

### 3.5 Queue and Buffer Sizes (Exact)
From current code:

- `MAX_PAYLOAD = 1,048,576` bytes
- Queue depth: `mq_maxmsg = 10`
- Queue message size: `mq_msgsize = sizeof(MessageHeader) + payload_size`
- Consumer receive buffer: `MAX_PAYLOAD + sizeof(MessageHeader) + 4096`
   - Rationale: safe headroom over declared queue message size to avoid receive buffer truncation risk.

### 3.6 Why These Sizes Were Chosen
- `mq_maxmsg = 10` balances queue memory footprint and buffering for burst traffic.
- Dynamic `mq_msgsize` per message-size phase avoids over-allocating kernel queue memory for small payload phases.
- Large static receive buffer avoids repeated allocations in hot path.

### 3.7 Run Structure
`NUM_RUNS = 15` with loop `run = 0..15`:
- `run=0`: warmup
- `run=1..15`: measured runs persisted to CSV

Per message size: 16 total runs, 15 recorded runs.

### 3.8 Message Size Matrix
Configured payload sizes (bytes):
- `64`, `256`, `1024`, `4096`, `16384`, `65536`, `262144`, `1048576`

### 3.9 Timestamping and Latency Unit
- Producer sets `send_ns` in message header.
- Consumer samples `recv_ns` after `mq_receive`.
- Stored latency unit: microseconds via `(recv_ns - send_ns) / 1000.0`.

### 3.10 Why Checksum Touch Exists
Consumer touches payload bytes (stride 64 bytes) to ensure payload is materially consumed by CPU and to avoid unrealistic optimization artifacts.

## 4. Metrics and Statistics
`mq_results.csv` columns:
- `message_size_bytes`
- `run`
- `throughput_gbps`
- `avg_latency_us`
- `stddev_us`
- `p50_us`, `p95_us`, `p99_us`

Warmup (`run=0`) is excluded from CSV.

## 5. Files and Their Roles
- `common.h`: constants, telemetry structures, dynamic total-byte helper.
- `mq_producer.cpp`: queue producer loop.
- `mq_consumer.cpp`: queue consumer and metrics aggregator.
- `run_mq_bench.sh`: system tuning and benchmark launch script.
- `mq_results.csv`: output dataset.

## 6. Build and Run
From this directory:

```bash
g++ -O3 -std=c++17 -Wall -o mq_producer mq_producer.cpp -lrt
g++ -O3 -std=c++17 -Wall -o mq_consumer mq_consumer.cpp -lrt
bash run_mq_bench.sh
```

`run_mq_bench.sh` also updates queue-related kernel limits and clears `/dev/mqueue/*` before execution.

## 7. Dependencies and Platform
- OS: Linux (POSIX mqueue and `/dev/mqueue` required)
- Compiler: `g++` with C++17 support
- Libraries/interfaces:
  - POSIX message queues (`-lrt`)
  - Shared memory (`shm_open`, `mmap`)
  - Scheduler affinity (`sched_setaffinity`)
  - `sysctl` for queue tuning (script)

## 8. Runtime Tuning Used
The script applies:
- `ulimit -n 65535`
- `ulimit -q 209715200`
- `fs.mqueue.queues_max=2048`
- `fs.mqueue.msgsize_max=1048600`

These settings prevent descriptor and queue-size bottlenecks for large-message runs.

### 8.1 Why These Kernel Limits Matter
- `ulimit -n`: raises per-process open descriptor cap.
- `ulimit -q`: raises per-user bytes permitted in POSIX message queues.
- `fs.mqueue.queues_max`: raises total number of mqueues system-wide.
- `fs.mqueue.msgsize_max`: allows large message payloads (close to 1 MiB).

Without these settings, large-payload queue creation or send operations can fail with limit-related errors.

## 9. Detailed Primitive Inventory
- Queue lifecycle: `mq_open`, `mq_close`, `mq_unlink`
- Transfer: `mq_send`, `mq_receive`
- Telemetry memory: `shm_open`, `ftruncate`, `mmap`, `munmap`, `shm_unlink`
- CPU affinity: `sched_setaffinity`
- System tuning in script: `ulimit`, `sysctl`, cleanup of `/dev/mqueue/*`

## 10. Reproducibility Notes
- Run with same kernel and queue limits for comparable results.
- Ensure `mq_*` objects are cleaned between runs.
- Minimize background load on pinned cores.

## 11. Limitations
- POSIX mqueue performance is sensitive to kernel queue limits and message size ceiling.
- Privileged tuning commands in script may differ across distros/policies.
- Throughput label says `gbps`, but code computes GiB/s-style value.

## 12. Constant Reference (from `common.h`)
- `PRODUCER_CORE = 1`
- `CONSUMER_CORE = 2`
- `MESSAGE_SIZES = {64,256,1024,4096,16384,65536,262144,1048576}`
- `NUM_RUNS = 15`
- `MAX_PAYLOAD = 1048576`
- `MAX_LAT_SAMPLES = 4*1024*1024`
- `SHM_TEL_NAME = /ipc_mq_telemetry`
- `get_total_bytes(sz): 32 MiB (<=1KiB), 256 MiB (<=64KiB), 2 GiB (>64KiB)`
