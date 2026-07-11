# POSIX Pipe (FIFO) Implementation

## 1. Overview
This implementation benchmarks one-way IPC using a named pipe (FIFO) with a producer process and a consumer process.

- Transport: POSIX FIFO (`mkfifo`, `open`, `read`, `write`)
- Pattern: Producer -> FIFO -> Consumer
- Measurement: Throughput and per-message latency distribution
- Output: `pipe_results.csv`

The benchmark sends payloads across multiple message sizes and repeats each size with one warmup run plus measured runs.

## 2. Architecture

### 2.1 Process Topology
- Producer process (`pipe_producer`): writes `MessageHeader + payload` records to FIFO.
- Consumer process (`pipe_consumer`): reads records, computes latency and throughput, writes CSV.
- Shared telemetry memory (`/ipc_pipe_telemetry`): consumer-owned metrics buffer used internally by stats pipeline.

### 2.2 Channel Topology
A unique FIFO path is created per message size:
- Base path: `/tmp/ipc_pipe_bench`
- Actual path per size: `/tmp/ipc_pipe_bench_<size>`

This avoids cross-size channel reuse artifacts.

### 2.3 CPU Affinity
CPU pinning is configured in `common.h`:
- Producer core: `PRODUCER_CORE = 1`
- Consumer core: `CONSUMER_CORE = 2`

Pinning reduces scheduler noise and improves repeatability.

### 2.4 Why No Explicit Locks Are Used
No application-level mutex/spinlock is used:
- Producer and consumer are separate processes communicating through kernel FIFO buffering.
- FIFO semantics already guarantee ordered byte stream behavior.
- Blocking `read`/`write` and kernel pipe buffering provide synchronization/backpressure.

Adding user-space locks here would not protect additional shared state and would add overhead.

## 3. Data Path and Working

### 3.1 Message Format
Each message on the wire is:
- `MessageHeader`
  - `send_ns` (send timestamp in ns)
  - `payload_size`
- Payload bytes (`X`-filled)

### 3.2 Producer Flow
For each message size:
1. Open corresponding FIFO for write (retry until consumer is ready).
2. Set pipe buffer size via `fcntl(..., F_SETPIPE_SZ, MAX_PAYLOAD)`.
3. For each run (warmup + measured):
   - Loop until target bytes reached.
   - Stamp `send_ns`.
   - Write full record (`header + payload`) using `write_all`.

### 3.3 Consumer Flow
For each message size:
1. Create FIFO path.
2. Open FIFO once for read.
3. For each run (warmup + measured):
   - Read header and payload with `read_all`.
   - Capture receive timestamp.
   - Compute latency: `(recv_ns - send_ns) / 1000.0` (microseconds).
   - Touch payload (checksum stride) to prevent dead-code elimination.
   - Stop when total target bytes for that size are consumed.
4. Compute statistics and append measured runs to CSV.

### 3.4 Workload Sizing
Traffic volume is message-size dependent (from `get_total_bytes`):
- `<= 1 KiB`: 32 MiB
- `<= 64 KiB`: 256 MiB
- `> 64 KiB`: 2 GiB

This keeps run time manageable for tiny messages while preserving stress for large payloads.

### 3.5 Buffer Sizes and Why They Were Chosen
Exact values from code:

- `MAX_PAYLOAD = 1,048,576` bytes (1 MiB)
  - Upper bound of message size matrix.
  - Used to size read buffer and pipe capacity request.

- Producer wire buffer:
  - `sizeof(MessageHeader) + payload_size`
  - Allocated once per message size and reused for all runs.

- Pipe capacity request:
  - `fcntl(fd, F_SETPIPE_SZ, MAX_PAYLOAD)`
  - Rationale: request enough FIFO capacity to smooth producer progress for large messages.

- Consumer payload buffer:
  - `std::vector<char> payload_buf(MAX_PAYLOAD)`
  - Rationale: avoid per-message dynamic allocations in hot path.

- Latency buffer:
  - `MAX_LAT_SAMPLES = 4 * 1024 * 1024`
  - Rationale: retain enough samples for stable percentile estimates while bounding memory.

Kernel may clamp the actual pipe size to system limits; benchmark records behavior under effective system configuration.

### 3.6 Run Structure
`NUM_RUNS = 5`, loop executes `run = 0..5`:
- `run=0`: warmup (not written to CSV)
- `run=1..5`: measured runs (written to CSV)

Per message size: 6 total runs, 5 recorded runs.

### 3.7 Message Size Matrix
Configured payload sizes (bytes):
- `64`, `256`, `1024`, `4096`, `16384`, `65536`, `262144`, `1048576`

### 3.8 Timestamping and Latency Unit
- Producer stamps `send_ns` for every message.
- Consumer samples `recv_ns` on receive.
- Latency is stored in microseconds: `(recv_ns - send_ns) / 1000.0`.

### 3.9 Why Checksum Touch Exists
Consumer touches payload bytes every 64 bytes (`i += 64`) to ensure the payload is read and not optimized away, making measured latency/throughput more representative of real consumption work.

## 4. Metrics and Statistics
Consumer computes:
- `throughput_gbps` (GiB/s in code naming)
- `avg_latency_us`
- `stddev_us`
- `p50_us`, `p95_us`, `p99_us`

Warmup run (`run=0`) is printed but not written to CSV.

## 5. Files and Their Roles
- `common.h`: shared constants, message structures, telemetry structure.
- `pipe_producer.cpp`: FIFO writer and sender loop.
- `pipe_consumer.cpp`: FIFO reader, latency capture, stats, CSV generation.
- `run_pipe_bench.sh`: compile, cleanup, launch orchestration.
- `pipe_results.csv`: benchmark output.

## 6. Build and Run
From this directory:

```bash
g++ -O3 -std=c++17 -Wall -o pipe_producer pipe_producer.cpp -lrt
g++ -O3 -std=c++17 -Wall -o pipe_consumer pipe_consumer.cpp -lrt
sudo rm -f /tmp/ipc_pipe_bench_*
sudo rm -f /dev/shm/ipc_pipe_telemetry*
sudo ./pipe_consumer &
sleep 1.5
sudo ./pipe_producer
wait
```

Or run:

```bash
bash run_pipe_bench.sh
```

## 7. Dependencies and Platform
- OS: Linux (required for FIFO path, POSIX shared memory behavior used here)
- Compiler: `g++` with C++17 support
- Libraries/system interfaces:
  - POSIX I/O (`read`, `write`, `open`, `close`)
  - FIFO (`mkfifo`)
  - Shared memory (`shm_open`, `mmap`)
  - Scheduler affinity (`sched_setaffinity`)

### 7.1 Detailed Syscall/Primitive Inventory
- FIFO lifecycle: `mkfifo`, `open`, `unlink`
- Data transfer: `read`, `write`
- Pipe tuning: `fcntl` with `F_SETPIPE_SZ`
- Telemetry memory: `shm_open`, `ftruncate`, `mmap`, `munmap`, `shm_unlink`
- CPU pinning: `sched_setaffinity`
- Timing/stats: C++ chrono and STL numeric/sort operations

## 8. Reproducibility Notes
- Keep CPU frequency scaling consistent for fair runs.
- Ensure no stale FIFO nodes and shm segments before each benchmark.
- Run producer/consumer under similar privilege mode as script (`sudo`) to match behavior.
- Avoid background heavy workloads on pinned cores.

## 9. Limitations
- FIFO is unidirectional in this benchmark shape.
- Backpressure and kernel pipe buffering can affect latency tails.
- Timing uses host clock in process context; extreme scheduling jitter still impacts tails.
- Effective pipe capacity depends on kernel policy/caps and may vary across systems.

## 10. Constant Reference (from `common.h`)
- `PRODUCER_CORE = 1`
- `CONSUMER_CORE = 2`
- `MESSAGE_SIZES = {64,256,1024,4096,16384,65536,262144,1048576}`
- `NUM_RUNS = 5`
- `MAX_PAYLOAD = 1048576`
- `MAX_LAT_SAMPLES = 4*1024*1024`
- `PIPE_FIFO_PATH = /tmp/ipc_pipe_bench`
- `SHM_TEL_NAME = /ipc_pipe_telemetry`
