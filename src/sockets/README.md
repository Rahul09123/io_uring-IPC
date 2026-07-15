# Unix Domain Socket Implementation

## 1. Overview
This implementation benchmarks local IPC over Unix domain stream sockets (`AF_UNIX`, `SOCK_STREAM`) using a producer-client and consumer-server model.

- Transport: Unix domain sockets (filesystem path endpoints)
- Pattern: Producer client -> Consumer server
- Measurement: Throughput and latency percentiles
- Output: `socket_results.csv`

## 2. Architecture

### 2.1 Process Topology
- Producer (`socket_producer`): creates client sockets and sends framed messages.
- Consumer (`socket_consumer`): creates listening socket, accepts producer, receives messages, computes stats.

### 2.2 Endpoint Strategy
A unique socket file path is used per message size:
- Base path: `/tmp/ipc_socket_bench`
- Actual path per size: `/tmp/ipc_socket_bench_<size>`

The consumer creates/binds this endpoint before each run and removes it after completion.

### 2.3 CPU Affinity
From `common.h`:
- Producer core: `PRODUCER_CORE = 1`
- Consumer core: `CONSUMER_CORE = 2`

This reduces run-to-run scheduler variance.

### 2.4 Why No Explicit Locks Are Used
No user-space mutex/spinlock is used in this implementation because data ownership is naturally separated:
- The producer only writes to its connected socket descriptor.
- The consumer only reads from the accepted socket descriptor.
- Kernel socket queues provide synchronization, ordering, blocking, and backpressure.

Locking in user space would add overhead without improving correctness for this one-producer, one-consumer stream model.

## 3. Data Path and Working

### 3.1 Message Format
On-wire frame:
- `MessageHeader`
  - `send_ns`
  - `payload_size`
- Payload bytes (`X`-filled)

### 3.2 Producer Flow
For each message size and run:
1. Create a socket (`socket(AF_UNIX, SOCK_STREAM, 0)`).
2. Configure send buffer (`SO_SNDBUF`).
3. Retry `connect` until consumer listener is available.
4. Send framed messages until `TOTAL_BYTES` is reached.
5. Close socket and short cooldown.

### 3.3 Consumer Flow
For each message size and run:
1. Create listening socket.
2. Configure receive buffer (`SO_RCVBUF`).
3. `bind` + `listen` on dynamic path.
4. `accept` producer connection.
5. Receive full header and payload (`read_all`).
6. Timestamp receive, compute per-message latency, and advance byte counter.
7. Close client/server sockets and unlink socket path.
8. Compute run stats and write measured runs to CSV.

### 3.4 Workload Volume
This implementation uses a fixed total transfer target:
- `TOTAL_BYTES = 2 GiB` for every message size.

### 3.5 Buffer Sizes and Why They Were Chosen
The following constants and buffer sizes are used directly by code:

- `MAX_PAYLOAD = 1,048,576` bytes (1 MiB)
  - Maximum payload size in test matrix.
  - Also used to size receiver payload buffer.

- Producer socket send buffer:
  - `SO_SNDBUF = MAX_PAYLOAD * 2` (2 MiB requested)
  - Rationale: reduce sender-side stalls during burst transmission of large frames.

- Consumer socket receive buffer:
  - `SO_RCVBUF = MAX_PAYLOAD * 2` (2 MiB requested)
  - Rationale: absorb producer bursts and reduce packetization-induced jitter at large sizes.

- Consumer payload read buffer:
  - `std::vector<char> payload_buf(MAX_PAYLOAD)`
  - Rationale: fixed reusable buffer avoids heap churn in inner receive loop.

- Latency sample storage:
  - `MAX_LAT_SAMPLES = 4 * 1024 * 1024`
  - Rationale: cap memory while keeping enough samples for stable percentile estimates.

Note: Linux may internally adjust effective socket buffer sizes; values above are requested values from userspace.

### 3.6 Run Structure
`NUM_RUNS = 15`, and loop form is `for (run = 0; run <= NUM_RUNS; ++run)`:
- Run `0`: warmup (printed only)
- Runs `1..15`: measured and written to CSV

So each message size executes 16 total runs, 15 recorded runs.

### 3.7 Message Size Matrix
Sizes in bytes:
- `64`, `256`, `1024`, `4096`, `16384`, `65536`, `262144`, `1048576`

This spans tiny control-like payloads up to 1 MiB bulk-transfer frames.

### 3.8 Timestamping and Latency Unit
- Producer writes `send_ns` in `MessageHeader` using `high_resolution_clock` nanosecond count.
- Consumer captures `recv_ns` similarly.
- Latency is stored as microseconds: `(recv_ns - send_ns) / 1000.0`.

### 3.9 Why Checksum Touch Exists
Consumer computes a stride checksum over payload (`i += 64`) to ensure payload bytes are actually touched by CPU. This avoids unrealistic optimization paths where data movement appears cheaper because payload bytes are never read.

## 4. Metrics and Statistics
Recorded columns in `socket_results.csv`:
- `message_size_bytes`
- `run`
- `throughput_gbps`
- `avg_latency_us`
- `stddev_us`
- `p50_us`, `p95_us`, `p99_us`

Run 0 is warmup and not persisted to CSV.

## 5. Files and Their Roles
- `common.h`: shared constants and structs.
- `socket_producer.cpp`: sender/client implementation.
- `socket_consumer.cpp`: receiver/server implementation and stats pipeline.
- `run_socket_bench.sh`: build/run orchestration script.
- `socket_results.csv`: benchmark output.

## 6. Build and Run
From this directory:

```bash
g++ -O3 -std=c++17 -Wall -o socket_producer socket_producer.cpp
g++ -O3 -std=c++17 -Wall -o socket_consumer socket_consumer.cpp
rm -f /tmp/ipc_socket_bench_*
./socket_consumer &
sleep 1.5
./socket_producer
wait
```

Or run:

```bash
bash run_socket_bench.sh
```

## 7. Dependencies and Platform
- OS: Linux or Unix-like system with AF_UNIX sockets
- Compiler: `g++` with C++17 support
- System interfaces used:
  - Unix sockets (`socket`, `bind`, `listen`, `accept`, `connect`)
  - Scheduler affinity (`sched_setaffinity`)
  - Standard POSIX file/socket ops

### 7.1 Detailed Syscall/Primitive Inventory
- Connection lifecycle: `socket`, `bind`, `listen`, `accept`, `connect`, `close`, `unlink`
- Data transfer: `read`, `write`
- Socket tuning: `setsockopt` with `SO_SNDBUF`, `SO_RCVBUF`
- CPU pinning: `sched_setaffinity`
- Timing and stats: C++ chrono, sort/accumulate for percentile and average/stddev computation

## 8. Reproducibility Notes
- Ensure stale socket files are removed before runs.
- Keep same CPU/core placement and system load profile across runs.
- Use the same privilege mode for repeated experiments.

## 9. Limitations
- Stream sockets include protocol framing overhead relative to raw shared memory.
- Endpoint setup/teardown per run contributes measurable overhead for small payloads.
- Throughput value name is `gbps`, but computation uses GiB/s convention in code.
- Because each run creates/binds/listens anew, connection setup cost is included in measured time.

## 10. Constant Reference (from `common.h`)
- `PRODUCER_CORE = 1`
- `CONSUMER_CORE = 2`
- `MESSAGE_SIZES = {64,256,1024,4096,16384,65536,262144,1048576}`
- `NUM_RUNS = 15`
- `TOTAL_BYTES = 2*1024*1024*1024`
- `MAX_PAYLOAD = 1048576`
- `MAX_LAT_SAMPLES = 4*1024*1024`
- `SOCKET_PATH = /tmp/ipc_socket_bench`
