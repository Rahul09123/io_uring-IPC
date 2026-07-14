# io_uring Shared-Ring Implementation with Asynchronous FIFO Signaling

## 1. Overview
This implementation benchmarks a cache-aligned POSIX shared-memory circular ring buffer data path. Unlike typical busy-spin designs that consume 100% CPU when idle, this architecture implements a **lock-free double-check signaling barrier** paired with `io_uring` to sleep and wake up the consumer process cleanly.

* **Data transport**: POSIX shared-memory ring buffer (direct zero-copy path between processes).
* **Signaling/Wakeup path**: Named FIFO (`/tmp/uring_sig_fifo`) coordinated via asynchronous `io_uring` read notifications.
* **Flow control**: Atomic head and tail indices with Acquire-Release memory ordering.
* **Measurement**: Throughput and latency percentiles recorded in `io_uring_results.csv`.

---

## 2. Architecture

### 2.1 Process & Signaling Topology
```
[Producer Core 1] --(Writes Slot Data)--> [Shared Ring Buffer] --(Reads Slot Data)--> [Consumer Core 2]
       |                                                                                    |
       | (Wakes up if consumer_sleeping == 1)                                               | (Sleeps on empty ring)
       v                                                                                    v
[Write to named FIFO] ------------------------------------------------------------> [io_uring_submit_and_wait]
```

* **Producer (`uring_producer`)**: Pin-affinity to Core 1. Writes payload bytes and timestamps into shared slots. If the consumer is flagged as sleeping, writes a wakeup byte to the signaling FIFO.
* **Consumer (`uring_consumer`)**: Pin-affinity to Core 2. Reads slots from the shared memory area. When the ring is empty, it publishes `consumer_sleeping = 1` and registers a read on the signaling FIFO using `io_uring`.
* **Shared memory ring (`/ipc_uring_ring_buffer`)**: The aligned memory-mapped region holding queue slots and index flags.

### 2.2 Ring Buffer Layout
Defined in `common.h`:
* `head` (atomic `uint64_t`): Next index to write.
* `tail` (atomic `uint64_t`): Next index to read.
* `consumer_sleeping` (atomic `uint32_t`): Set to `1` when the consumer is sleeping, and CAS-reset to `0` during wakeups.
* `NUM_SLOTS = 64`
* Each slot stores:
  * `send_ns` (`uint64_t`)
  * `size` (`uint32_t`)
  * `data[MAX_PAYLOAD]` (Up to 1 MiB)

All control structures (`head`, `tail`, `consumer_sleeping`) are cache-line aligned (`alignas(64)`) to avoid false sharing.

---

## 3. Data Path and Working

### 3.1 Producer Flow
1. Open and memory-map the shared ring buffer object (`/ipc_uring_ring_buffer`).
2. Open the named signaling FIFO (`/tmp/uring_sig_fifo`) in `O_RDWR | O_NONBLOCK` mode to avoid blocking during initialization.
3. For each size class and run:
   * Wait for consumer reset (`head == 0` and `tail == 0`).
   * For each block:
     * Check if ring is full (`head - tail >= NUM_SLOTS`). If so, pause and yield CPU.
     * Copy payload into slot at `head % NUM_SLOTS` and record the send timestamp.
     * Advance `head` with `std::memory_order_release` to publish the slot.
     * **Signaling Barrier**: Check `consumer_sleeping`. If `1`, execute a `compare_exchange_strong` to swap it to `0`. If successful, write a 1-byte notification (`'W'`) to the signaling FIFO to wake up the consumer.

### 3.2 Consumer Flow
1. Create the named signaling FIFO using `mkfifo` and open it in `O_RDWR` mode.
2. Open and memory-map the shared ring buffer object.
3. Initialize the consumer's `io_uring` ring context.
4. For each run:
   * Reset `tail = 0`, `head = 0`, and `consumer_sleeping = 0`.
   * Loop until `consumed` matches `TOTAL_BYTES`:
     * Read `tail` (relaxed) and `head` (acquire).
     * If the ring is empty (`tail == head`):
       * Store `1` into `consumer_sleeping` using release ordering.
       * **Double-check**: Reload `head` (acquire). If `head != tail` in the meantime, CAS `consumer_sleeping` back to `0` and abort the sleep block to prevent a lost wakeup.
       * If still empty, prepare an `io_uring_prep_read` SQE from the signaling FIFO.
       * Call **`io_uring_submit_and_wait(&ring, 1)`** to submit the SQE and sleep until the producer writes to the FIFO.
       * Once awake, consume the read completion event, clear the CQE, and continue the loop.
     * Consume slot data at `tail % NUM_SLOTS`, compute latency statistics, and verify the checksum.
     * Publish updated `tail` index using `std::memory_order_release`.

### 3.3 Synchronization and Safety Details
* **Zero-copy Data Path**: Payloads are read directly from the shared memory mapped slots.
* **Race Prevention**: The atomic double-check loop in the consumer avoids the "lost wakeup" race condition. If a producer writes after the sleep flag is set but before the read SQE is submitted, the write is buffered in the FIFO, allowing the consumer's subsequent `io_uring` submission to resolve instantly.
* **FIFO Flooding Prevention**: The producer's CAS ensures that at most 1 byte is written to the signaling FIFO per sleep cycle, keeping the signaling FIFO clean and bounding kernel-side pipe allocations.

---

## 4. Metrics and Statistics
Collected metrics are stored in `io_uring_results.csv`:
* `message_size_bytes`: Sweep target size (64 B to 1 MiB).
* `run`: Run iteration (warmup 0 is printed only, 1-5 recorded).
* `throughput_gbps`: Effective GiB/s.
* `avg_latency_us` / `stddev_us` / `p50_us` / `p95_us` / `p99_us`: Microsecond latencies.

---

## 5. Build and Run

Compile and execute using the automated script:
```bash
bash run_uring_bench.sh
```

Or manually:
```bash
# Compile producer and consumer (both require -luring and -lrt)
g++ -O3 -std=c++17 -Wall -o uring_producer uring_producer.cpp -luring -lrt
g++ -O3 -std=c++17 -Wall -o uring_consumer uring_consumer.cpp -luring -lrt

# Clear existing segments
sudo rm -f /dev/shm/ipc_uring_ring_buffer*
rm -f /tmp/uring_sig_fifo

# Start consumer in background
sudo ./uring_consumer &
sleep 1.5

# Run producer
sudo ./uring_producer
wait
```

---

## 6. Primitives & APIs Used
* **Shared memory lifecycles**: `shm_open`, `ftruncate`, `mmap`, `munmap`, `shm_unlink`
* **Signaling channel**: `mkfifo`, `open`, `write`, `close`, `unlink`
* **io_uring routines**: `io_uring_queue_init`, `io_uring_get_sqe`, `io_uring_prep_read`, `io_uring_submit_and_wait`, `io_uring_peek_cqe`, `io_uring_cqe_seen`, `io_uring_queue_exit`
* **Thread settings**: `sched_setaffinity` (Producer Core 1, Consumer Core 2)
