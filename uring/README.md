# io_uring Shared-Ring Implementation

## 1. Overview
This implementation benchmarks a shared-memory ring buffer data path coordinated with `io_uring` submission/completion activity.

- Data transport: POSIX shared memory ring buffer
- Coordination: `io_uring` NOP submissions (SQPOLL when available)
- Pattern: Producer writes slots -> Consumer reads slots
- Measurement: Throughput and latency percentiles
- Output: `io_uring_results.csv`

## 2. Architecture

### 2.1 Process Topology
- Producer (`uring_producer`): writes timestamped payloads into shared ring slots.
- Consumer (`uring_consumer`): polls ring indices, consumes slots, measures latency.
- Shared memory ring (`/ipc_uring_ring_buffer`): data and metadata exchange area.

### 2.2 Ring Buffer Layout
Defined in `common.h`:
- `head` (atomic): next producer write index
- `tail` (atomic): next consumer read index
- `NUM_SLOTS = 64`
- Each slot stores:
  - `send_ns`
  - `size`
  - `data[MAX_PAYLOAD]`

Memory alignment (`alignas(64)`) is used to reduce false sharing and cache-line contention.

### 2.3 CPU Affinity Matrix
- Producer core: `PRODUCER_CORE = 1`
- Consumer core: `CONSUMER_CORE = 2`
- SQPOLL core: `SQPOLL_CORE = 3`

If SQPOLL setup fails (often due to missing privileges), code falls back to standard io_uring mode.

### 2.4 Why This Uses Lock-Free Atomics Instead of Mutexes
The ring is single-producer/single-consumer (SPSC), so a lock-free head/tail index scheme is sufficient:
- Producer is sole writer of `head`.
- Consumer is sole writer of `tail`.
- Each side reads the other index for flow control.

This avoids mutex contention and kernel lock transitions in the hottest path.

## 3. Data Path and Working

### 3.1 Producer Flow
1. Create/map shared memory ring object.
2. Initialize io_uring with `IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF`.
3. For each message size and run:
   - Wait for consumer reset (`head == 0` and `tail == 0`).
   - Check ring fullness (`head - tail < NUM_SLOTS`).
   - Write payload + metadata into slot at `head % NUM_SLOTS`.
   - Publish via `head.store(..., release)`.
   - Submit io_uring NOP operation (used as benchmark coordination signal path).
   - Repeat until `TOTAL_BYTES` is produced.

### 3.2 Consumer Flow
1. Create/map shared ring object.
2. For each size and run:
   - Reset `tail = 0`, then `head = 0`.
   - Wait for producer to publish first item (`head > 0`).
   - Loop while consumed bytes < `TOTAL_BYTES`:
     - If `tail == head`, spin with pause/yield macro.
     - Read slot at `tail % NUM_SLOTS`.
     - Record latency from `recv_ns - send_ns`.
     - Touch payload (checksum stride) to avoid dead-code elimination.
     - Advance `tail` with release store.
3. Compute stats and write measured runs to CSV.

### 3.3 Synchronization Model
- Lock-free SPSC semantics via atomic indices.
- Memory ordering:
  - Producer publishes slots with `release` on `head`.
  - Consumer observes with `acquire` on `head` and publishes `tail` with `release`.

### 3.4 Ring and Buffer Sizes (Exact)
From `common.h` and producer setup:

- `NUM_SLOTS = 64`
- `MAX_PAYLOAD = 1,048,576` bytes
- Each slot contains:
  - `send_ns` (`uint64_t`)
  - `size` (`uint32_t`)
  - `data[MAX_PAYLOAD]`
- Latency sample capacity: `MAX_LAT_SAMPLES = 4 * 1024 * 1024`
- io_uring SQ/CQ depth requested: `256`
- SQPOLL idle timeout: `sq_thread_idle = 100000`

### 3.5 Why These Sizes Were Chosen
- `NUM_SLOTS = 64`: enough in-flight slots to smooth producer/consumer phase skew without excessive shared memory footprint.
- `MAX_PAYLOAD = 1 MiB`: aligns with largest benchmark payload.
- Queue depth `256`: adequate for frequent NOP submissions while keeping management overhead small.
- Large latency sample cap: stable percentile estimation across long transfers.

### 3.6 Run Structure
`NUM_RUNS = 5` and loop executes `run = 0..5`:
- `run=0`: warmup
- `run=1..5`: measured runs written to CSV

Per size: 6 total runs, 5 recorded.

### 3.7 Message Size Matrix
Payload sizes in bytes:
- `64`, `256`, `1024`, `4096`, `16384`, `65536`, `262144`, `1048576`

### 3.8 Timestamping and Latency Unit
- Producer writes `send_ns` per slot.
- Consumer samples `recv_ns` during slot consumption.
- Latency unit in CSV is microseconds: `(recv_ns - send_ns) / 1000.0`.

### 3.9 Why Spin-Wait Is Used
Both producer and consumer use pause/yield busy-wait when ring is full/empty. This reduces blocking syscall overhead and keeps latency low for high-frequency handoff, at the cost of CPU occupancy.

### 3.10 Why Payload Checksum Touch Exists
Consumer touches payload bytes every 64 bytes to ensure data is actually read, avoiding unrealistic optimization where payload transfer cost is underrepresented.

## 4. Metrics and Statistics
`io_uring_results.csv` includes:
- `message_size_bytes`
- `run`
- `throughput_gbps`
- `avg_latency_us`
- `stddev_us`
- `p50_us`, `p95_us`, `p99_us`

Warmup (`run=0`) is printed only, not persisted.

## 5. Files and Their Roles
- `common.h`: ring structure, constants, affinity settings.
- `uring_producer.cpp`: producer + io_uring setup and NOP submission loop.
- `uring_consumer.cpp`: consumer polling loop, metrics generation.
- `run_uring_bench.sh`: compile, shared-memory cleanup, launch orchestration.
- `io_uring_results.csv`: benchmark output.

## 6. Build and Run
From this directory:

```bash
g++ -O3 -std=c++17 -Wall -o uring_producer uring_producer.cpp -luring -lrt
g++ -O3 -std=c++17 -Wall -o uring_consumer uring_consumer.cpp -lrt
sudo rm -f /dev/shm/ipc_uring_ring_buffer*
sudo ./uring_consumer &
sleep 1.5
sudo ./uring_producer
wait
```

Or run:

```bash
bash run_uring_bench.sh
```

## 7. Dependencies and Platform
- OS: Linux kernel with io_uring support
- Compiler: `g++` with C++17 support
- Libraries:
  - `liburing` (`-luring`)
  - POSIX realtime (`-lrt`)
- System interfaces:
  - POSIX shared memory (`shm_open`, `mmap`)
  - CPU affinity (`sched_setaffinity`)
  - atomic operations with explicit memory order

### 7.1 Detailed Primitive Inventory
- Ring setup/teardown: `shm_open`, `ftruncate`, `mmap`, `munmap`, `shm_unlink`
- io_uring lifecycle: `io_uring_queue_init_params`, `io_uring_get_sqe`, `io_uring_prep_nop`, `io_uring_submit`, `io_uring_peek_cqe`, `io_uring_cqe_seen`, `io_uring_queue_exit`
- Synchronization primitives: C++ `std::atomic<uint64_t>` with acquire/release semantics
- CPU pinning: `sched_setaffinity`

## 8. Privilege and Kernel Notes
- SQPOLL mode may require elevated privileges/capabilities.
- Script runs producer and consumer with `sudo` to improve compatibility with SQPOLL and shm permissions.
- If SQPOLL is unavailable, benchmark still runs in fallback mode.

### 8.1 SQPOLL Fallback Behavior
If `io_uring_queue_init_params` with SQPOLL flags fails:
- Code reinitializes io_uring with default params (no SQPOLL).
- Benchmark continues and still emits results.
- Throughput/latency may differ from SQPOLL-enabled runs.

## 9. Reproducibility Notes
- Keep ring size, slot count, and core pinning unchanged across runs.
- Ensure stale shared-memory objects are removed before execution.
- Use consistent kernel version and io_uring configuration.

## 10. Limitations
- The data path is shared memory; io_uring here is used for coordination activity, not direct socket/file transfer.
- Spin-wait loops can be sensitive to CPU frequency scaling and co-scheduled workloads.
- Reported throughput variable is named `gbps` but computed as GiB/s-style transfer/time.
- Shared ring footprint is large because each of 64 slots reserves 1 MiB payload space.

## 11. Constant Reference (from `common.h`)
- `PRODUCER_CORE = 1`
- `CONSUMER_CORE = 2`
- `SQPOLL_CORE = 3`
- `MESSAGE_SIZES = {64,256,1024,4096,16384,65536,262144,1048576}`
- `NUM_RUNS = 5`
- `TOTAL_BYTES = 2*1024*1024*1024`
- `NUM_SLOTS = 64`
- `MAX_PAYLOAD = 1048576`
- `MAX_LAT_SAMPLES = 4*1024*1024`
- `SHM_RING_NAME = /ipc_uring_ring_buffer`
