# How Much Does io_uring Help Shared-Memory IPC? A Measurement Study of Wakeup Mechanisms for Lock-Free SPSC Rings

## Abstract
Modern system design increasingly shifts toward decoupled, microservices-based, and multi-process architectures, heightening the reliance on Inter-Process Communication (IPC) throughput and latency. Traditionally, IPC mechanisms like POSIX Pipes, UNIX Domain Sockets, and POSIX Message Queues have mediated data transfers through the Linux kernel, incurring system-call transitions, context-switching latency, and memory copies. This measurement study evaluates traditional kernel-mediated IPC mechanisms against a lock-free, cache-aligned Shared Memory Ring buffer. We isolate and evaluate the wakeup mechanism itself under an ablation matrix—varying between busy-poll (spin), spin with exponential backoff, adaptive spin-then-block, futex, eventfd, and io_uring signaling. Using a corrected depth-1 ping-pong latency protocol with a single clock source and separate saturating throughput runs, we identify that the data path (the shared-memory ring) is the source of high throughput, while io_uring acts as a wakeup mechanism comparable to conventional blocking primitives like futex and eventfd.

---

## 1. Introduction & Research Goal

The core contribution of this work is to isolate the performance benefits of shared memory transports from the wakeup/blocking signaling mechanisms. When processes communicate via a shared memory ring, they can spin when empty/full to achieve minimum latency (at high CPU cost), or they can sleep to yield CPU. This study answers: *What does io_uring actually buy for IPC wakeup signaling compared to standard primitives like futex, eventfd, or busy-polling?*

---

## 2. Experimental Methodology & Two-Mode Protocol

To eliminate queueing delay bias and per-core clock synchronization errors, we separate execution into two distinct modes:

### A. Saturated Throughput Mode
Under saturating load, the producer streams messages as fast as possible to transfer a total of 2 GB. We evaluate throughput (GB/s) across:
1. Baselines: POSIX Pipes, Unix Domain Sockets, POSIX Message Queues.
2. SPSC Ring variants: running all 6 wakeup mechanisms under `saturated`, `bursty` (1ms producer idle), and `offered-load` (Poisson arrivals) sweeps.

### B. Depth-1 Ping-Pong Latency Mode
Client A writes one message and waits for the echo response from Server B before sending the next message. 
- **Single Clock Source**: timed entirely on Client A using `clock_gettime(CLOCK_MONOTONIC_RAW)` fenced by sequential consistency compiler barriers, eliminating cross-core clock synchronization jitter.
- **Approximation**: One-way latency is RTT / 2.
- **Statistical Size**: We run $10^5$ iterations per run (discarding the first 1000 warmup trips), reporting the median (P50), mean, P90, P95, P99, P99.9, and the 95% confidence interval.

---

## 3. Pluggable Wakeup Ablation Layer

We vary ONLY the mechanism used to block/wake when the SPSC ring goes empty (consumer) or full (producer) while keeping the lock-free cache-aligned data path identical:

1. **Busy-poll (spin)**: Consumer spins in userspace on the tail index; no syscalls, no sleeping. Presents the lower-bound latency and upper-bound CPU cost.
2. **Spin + backoff**: Spin with a CPU pause and exponential backoff, eventually yielding via `sched_yield()`.
3. **Adaptive spin-then-block**: Spin for 2000 iterations before falling back to a `futex` sleep block.
4. **futex**: Block via `SYS_futex` (`FUTEX_WAIT`) when empty; producer wakes via `FUTEX_WAKE` upon publishing.
5. **eventfd**: Read blocks on an eventfd descriptor; producer wakes by writing a 8-byte wakeup counter.
6. **io_uring wakeup**: Consumer blocks on `io_uring_submit_and_wait` to read from a signaling FIFO; producer writes to it via `io_uring_prep_write`.

---

## 4. Outline of Final IEEE Publication Structure

### I. Introduction
- Motivation for sub-microsecond IPC.
- Research question: does io_uring help IPC, and where?
- Contributions of the ablation study.

### II. Background & Related Work
- storage-I/O vs IPC signaling patterns.
- Lock-free SPSC rings (Lamport, Disruptor, DPDK rte_ring).
- conventional Linux wakeups (futex, eventfd).

### III. System Design
- Lock-free, cache-aligned SPSC ring.
- Pluggable wakeup layer interface.
- Sharing eventfd descriptors via Unix domain sockets using `SCM_RIGHTS`.

### IV. Methodology
- Saturating throughput vs. depth-1 ping-pong latency.
- Wakeup ablation matrix and arrival regimes (saturated, bursty, offered load).
- Environment, hardware specification, and core-pinning setup.

### V. Results & Analysis
- Ring vs. kernel-IPC throughput.
- Corrected ping-pong latency distributions (P50, P99, P99.9) and tails.
- Wakeup ablation under bursty loads (the core result).
- CPU cost (cycles/message) and context-switch counts.

### VI. Discussion
- When to use which wakeup mechanism.
- The limits of io_uring for single-ring SPSC IPC vs. multi-ring batching.

### VII. Threats to Validity
- Jitter, single-socket NUMA, and hardware microarchitecture specifics.

### VIII. Conclusion & Future Work
- Extension to MPMC rings, network/RDMA baselines, and cross-NUMA evaluations.

---

## 5. Artifact Directory Structure

- `data/`: throughput and latency CSV output sets.
- `figures/`: throughput, latency, cache misses, and ablation plots.
- `scripts/`: data processing, visualization, and statistical validation runs.
- `src/`: baseline and shared-memory ablation benchmark implementations.