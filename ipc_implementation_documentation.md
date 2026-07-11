# IPC Implementation Documentation

## Overview

This repository benchmarks four IPC mechanisms under the same message-size sweep:

- POSIX pipes
- Unix domain sockets
- POSIX message queues
- io_uring with a shared-memory ring buffer

The benchmark uses a producer/consumer split and records throughput, latency, cache misses, and flamegraph traces. The implementations are not identical at the API level, but they share the same instrumentation strategy: the producer stamps each message with a send timestamp, the consumer reads that timestamp back, and both sides keep the payload large enough to exercise copy and synchronization overhead.

## Shared Benchmark Design

The shared header in each IPC folder defines the benchmark constants. The important values are:

- `MESSAGE_SIZES`: 64 B, 256 B, 1 KiB, 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB
- `PRODUCER_CORE`: 1
- `CONSUMER_CORE`: 2
- `SQPOLL_CORE`: 3 for io_uring when SQPOLL is available
- `MAX_PAYLOAD`: 1 MiB
- `MAX_LAT_SAMPLES`: 4 MiB latency samples buffer

The implementations all include a `MessageHeader` with the send timestamp and payload size. The consumer uses that header to reconstruct end-to-end latency in microseconds.

The benchmark code also follows a simple measurement discipline:

1. Pin producer and consumer to separate cores.
2. Run one warmup iteration before the measured runs.
3. Sweep all message sizes for the current transport.
4. Scale workload volumes depending on the IPC mechanism:
   - **POSIX Pipes & POSIX Message Queues**: Dynamically scaled (32 MB for messages $\le 1$ KiB, 256 MB for $\le 64$ KiB, and 2 GB for larger messages) to optimize execution times.
   - **UNIX Domain Sockets & `io_uring` Shared Ring**: Static 2 GB workload across all message sizes.
5. Record throughput and latency statistics in CSV output.
6. Generate flamegraphs and cache-miss summaries for the accompanying analysis.

## POSIX Pipe Implementation

### Files

- `pipe/common.h`
- `pipe/pipe_producer.cpp`
- `pipe/pipe_consumer.cpp`
- `pipe/run_pipe_bench.sh`

### Data Path

The pipe version uses named FIFOs under `/tmp`. Each message size receives its own FIFO name so the benchmark can isolate setup overhead from the steady-state transfer loop.

Producer flow:

1. Pin to the producer core.
2. Build a wire buffer containing `MessageHeader + payload`.
3. Open the FIFO once per message-size class.
4. Increase the pipe size with `F_SETPIPE_SZ`.
5. Write message after message until the target byte count is reached.

Consumer flow:

1. Pin to the consumer core.
2. Create the FIFO for the current message size.
3. Open the FIFO once for reading.
4. Read the header and payload in blocking loops.
5. Measure latency using the embedded send timestamp.
6. Write aggregated statistics to `pipe_results.csv`.

### Implementation Notes

- The FIFO is recreated for every size class to keep the transport path isolated.
- The code performs a checksum pass over the payload so the benchmark exercises actual memory traffic.
- This implementation is the simplest one in the repository, but also the most likely to spend time in kernel-managed copy and wakeup paths.

## Unix Domain Socket Implementation

### Files

- `Sockets/common.h`
- `Sockets/socket_producer.cpp`
- `Sockets/socket_consumer.cpp`
- `Sockets/run_socket_bench.sh`

### Data Path

The socket benchmark uses `AF_UNIX` `SOCK_STREAM` sockets. Each message size maps to a different filesystem socket path under `/tmp`.

Producer flow:

1. Pin to the producer core.
2. Allocate a message buffer with a header and payload.
3. Create a Unix domain socket.
4. Set `SO_SNDBUF` to reduce trivial buffer pressure.
5. Retry `connect` until the consumer has bound the socket.
6. Stream payloads until the byte target is reached.

Consumer flow:

1. Pin to the consumer core.
2. Create a listening socket.
3. Set `SO_RCVBUF` for a larger receive window.
4. Bind and listen on the size-specific socket path.
5. Accept the producer connection.
6. Read the header and payload in a loop, then record throughput and latency statistics in `socket_results.csv`.

### Implementation Notes

- The retry loop in the producer ensures the benchmark is robust against race conditions during listener startup.
- The socket path adds connection management overhead that pipes do not have, but it is a common and realistic IPC style.
- This implementation is a good middle ground between simplicity and explicit transport control.

## POSIX Message Queue Implementation

### Files

- `Updated Message Queue/common.h`
- `Updated Message Queue/mq_producer.cpp`
- `Updated Message Queue/mq_consumer.cpp`
- `Updated Message Queue/run_mq_bench.sh`

### Data Path

The message-queue implementation uses one queue per message size with a name of the form `/ipc_mq_bench_<size>`. It is the most message-oriented design in the repository because send and receive operations are explicitly framed by the kernel queue abstraction.

Producer flow:

1. Pin to the producer core.
2. Build a header-plus-payload wire buffer.
3. Retry `mq_open` until the consumer creates the queue.
4. Call `mq_send` for each message.
5. Continue until the byte target is reached.

Consumer flow:

1. Pin to the consumer core.
2. Create telemetry storage in shared memory.
3. Configure `mq_attr` with the message size and queue depth.
4. Open the queue with `O_CREAT | O_RDONLY`.
5. Receive messages with `mq_receive`.
6. Compute latency and write statistics to `mq_results.csv`.

### Implementation Notes

- The queue attribute setup is important because message queues enforce explicit size and depth constraints.
- The implementation uses shared memory only for telemetry, not for message transport itself.
- The queue abstraction is convenient, but it introduces management overhead and capacity constraints that can affect throughput.

## io_uring Shared Ring Implementation

### Architecture Diagram

```mermaid
flowchart LR
	P[Producer core] -->|writes payload + timestamp| R[(Shared ring buffer)]
	R -->|reads payload + latency| C[Consumer core]
	P --> H[head index]
	C --> T[tail index]
	H --> R
	T --> R
```

### Files

- `uring/common.h`
- `uring/uring_producer.cpp`
- `uring/uring_consumer.cpp`
- `uring/run_uring_bench.sh`

### Data Path

The io_uring version uses a cache-line-aligned shared-memory ring buffer. The producer and consumer coordinate through atomic head and tail indices. The payload is written directly into shared memory slots, so the benchmark emphasizes synchronization and memory locality instead of transport copies.

The buffer is intentionally small enough to force wraparound under load. The
producer checks the gap between head and tail before writing a new slot, and the
consumer advances tail only after it has read the slot and computed latency.
This makes the implementation a single-producer, single-consumer ring with a
strict publish/consume protocol:

1. Producer claims the next free slot.
2. Producer writes payload bytes into that slot.
3. Producer records the send timestamp.
4. Producer publishes by advancing head with release ordering.
5. Consumer observes the new head with acquire ordering.
6. Consumer reads the slot, computes latency, and advances tail.

The implementation uses io_uring primarily as the asynchronous engine around the
shared memory region. The kernel interaction is therefore lightweight compared
with stream-based IPC, but the benchmark still keeps io_uring in the control
path so the setup reflects a realistic Linux runtime.

Producer flow:

1. Pin to the producer core.
2. Open and map the shared ring buffer object.
3. Initialize io_uring with SQPOLL and SQ affinity when available.
4. Copy the payload into the next free slot.
5. Stamp the slot with a send timestamp.
6. Advance the head index and optionally submit a no-op SQE to drive the ring.

Consumer flow:

1. Pin to the consumer core.
2. Create and map the same shared ring buffer object.
3. Reset the ring indices for each run.
4. Wait until the producer publishes data.
5. Read the slot, compute latency, and advance the tail index.
6. Store the aggregate statistics in `io_uring_results.csv`.

### Implementation Notes

- The ring buffer is padded and aligned to reduce false sharing.
- The code attempts to use `IORING_SETUP_SQPOLL`, but it falls back to the regular ring setup if privileges are not available.
- This design is the most specialized in the repository and the one most closely tied to cache behavior and CPU affinity.

## Runtime and Output Artifacts

The repository already includes the benchmark outputs:

- `pipe_results.csv`
- `socket_results.csv`
- `mq_results.csv`
- `io_uring_results.csv`
- `Cache Misses`
- `FlameGraphs/*.svg`

The companion Python script `scripts/generate_visualizations.py` turns these into final presentation assets under `figures/`.

## How to Regenerate the Figures

From the repository root:

```bash
python3 scripts/generate_visualizations.py
```

This command creates:

- `figures/throughput.png`
- `figures/latency.png`
- `figures/cache_misses.png`
- `figures/cache_misses_summary.csv`
- `figures/flamegraphs/index.html`

## Practical Interpretation

- Pipes are the simplest baseline and are useful for understanding kernel copy overhead.
- Unix domain sockets are a familiar production-style IPC option and expose connection management costs.
- POSIX message queues are convenient when message framing matters, but queue limits and kernel bookkeeping are part of the cost model.
- io_uring with shared memory is the most advanced option here and is best suited to high-throughput producer/consumer paths where shared data structures and affinity control are acceptable.

The repository is therefore a compact comparison study of four IPC styles, each with different strengths in simplicity, throughput, and synchronization overhead.