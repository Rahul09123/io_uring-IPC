# A Comparative Performance Analysis of High-Performance Linux IPC Mechanisms: POSIX Pipes, UNIX Domain Sockets, POSIX Message Queues, and `io_uring` Shared Memory Rings
# A Comparative Performance and Ablation Analysis of Linux IPC Mechanisms: POSIX Pipes, UNIX Domain Sockets, POSIX Message Queues, and `io_uring` Shared-Memory Rings
---
## Abstract
Modern system design increasingly shifts toward decoupled, microservices-based, and multi-process architectures, heightening the reliance on Inter-Process Communication (IPC) throughput and latency. Traditionally, IPC mechanisms like POSIX Pipes, UNIX Domain Sockets, and POSIX Message Queues have mediated data transfers through the Linux kernel, incurring system-call transitions, context-switching latency, and memory copies. This paper presents a comparative empirical evaluation of traditional kernel-mediated IPC mechanisms against a lock-free, cache-aligned Shared Memory Ring buffer with `io_uring`-assisted wakeup signaling. The payload moves through POSIX shared memory via atomic SPSC indexing; `io_uring` is used exclusively for out-of-band wakeup coordination when the ring is empty, keeping the hot data path entirely in userspace. We evaluate performance metrics across a workload size sweep (64 Bytes to 1 MiB) with hardware CPU core affinity pinning. Our findings show that the shared-memory mechanism achieves up to **6.5×** throughput over Unix Domain Sockets and significantly reduces median latency for small and large messages by bypassing user-kernel transitions and avoiding cache-line false sharing. At 1 MiB payloads, Unix sockets and POSIX MQ exceed the ring buffer's throughput due to ring slot exhaustion.
Modern concurrent systems increasingly shift toward decoupled, microservices-based, and multi-process architectures, heightening the reliance on Inter-Process Communication (IPC) throughput, latency, and CPU efficiency. Traditionally, IPC mechanisms like POSIX Pipes, UNIX Domain Sockets, and POSIX Message Queues have mediated data transfers through the Linux kernel, incurring system-call transitions, context-switching latency, and memory copies. 
This paper presents a comprehensive empirical evaluation comparing traditional kernel-mediated IPC mechanisms against a lock-free, cache-aligned **Shared Memory Ring Buffer with `io_uring`-assisted out-of-band wakeup coordination**. The hot data path moves through POSIX shared memory (`/dev/shm`) via Single-Producer Single-Consumer (SPSC) atomic indexing (`head` and `tail`), keeping payload transfers entirely in userspace. Out-of-band wakeup notifications are managed asynchronously via `io_uring` ring submission queues. 
Our evaluation includes a **Two-Suite Experimental Methodology**:
1. **Ablation Study**: Evaluates 5 distinct wakeup strategies (`busy_poll`, `spin_backoff`, `adaptive`, `futex`, `io_uring`) under both **`saturated`** (streaming throughput) and **`bursty`** (intermittent arrival) traffic regimes across an exponential payload sweep ($64\text{ B}$ to $1\text{ MiB}$).
2. **Ping-Pong Latency Suite**: Enforces a strict **Queue Depth = 1** request-response protocol with single-clock `CLOCK_MONOTONIC_RAW` sampling on a pinned core to eliminate queue backlog delay and cross-core hardware TSC clock drift, measuring exact median (P50), P90, P99, and P99.9 tail percentiles.
Empirical results demonstrate that shared-memory ring buffers achieve sub-microsecond median latencies (**208 nanoseconds** at 64 B with `busy_poll`) and up to **6.5×** throughput improvements over kernel-mediated baselines, while `io_uring` and `futex` wakeups provide 0% idle CPU utilization with sub-15µs wakeup latencies.
---
## I. Introduction
Inter-Process Communication is the bedrock of concurrent system design on Unix-like operating systems. High-performance computing, financial trading systems, and containerized microservices require message buses capable of transferring gigabytes of telemetry or transactional data per second with sub-microsecond latency.
Traditional IPC options rely on kernel-mediated buffer boundaries. While providing clean process separation and safety, they enforce system call overhead (e.g., `read`, `write`, `sendmsg`, `recvmsg`) and physical memory copies between userspace processes and kernel ring structures. To address these overheads, memory-mapped shared regions have been utilized for zero-copy transfers, but coordinating access to these regions usually requires synchronization primitives like mutexes or semaphores, which can default to kernel-space blocking under lock contention.
Inter-Process Communication is the bedrock of high-performance concurrent computing on Unix-like operating systems. High-frequency trading (HFT) platforms, database engines, and microservice mesh routers demand IPC mechanisms capable of transferring gigabytes of telemetry data per second with minimal CPU overhead and sub-microsecond latency.
This paper evaluates a lock-free, Single-Producer Single-Consumer (SPSC) ring buffer architecture mapped into POSIX shared memory. The architecture utilizes atomic memory operations to coordinate ring boundaries without system calls on the hot path. `io_uring` is used in default interrupt mode (not SQPOLL) exclusively for the out-of-band sleep/wakeup signaling path via a named FIFO — the payload itself is transferred via `memcpy` into shared memory slots, incurring no kernel crossing. We benchmark this mechanism against three ubiquitous kernel-mediated baselines under a strict, hardware-pinned testing methodology.
Traditional IPC mechanisms rely on kernel-mediated buffer management:
- **POSIX Pipes**: Uni-directional byte streams with implicit kernel synchronization.
- **UNIX Domain Sockets (`AF_UNIX`)**: Socket-buffer queues bypassing network stacks but still requiring socket VFS overhead.
- **POSIX Message Queues (`mqueue`)**: Structured, priority-based message delivery constrained by VFS inode locks and kernel queue caps.
---
While kernel mediation enforces strict process isolation, it mandates context switches (`sys_enter_write`, `sys_enter_read`), user-kernel memory copies, and CPU scheduler preemptions. To bypass these limitations, zero-copy shared memory regions are used. However, synchronizing access to shared memory typically introduces lock contention or kernel context switches.
## II. System Architecture & IPC Topologies
This research presents a complete architecture and ablation analysis of a **lock-free SPSC shared-memory ring buffer paired with `io_uring` async notifications**, comparing its raw throughput, latency distributions, CPU core consumption, and tail-latency percentiles against standard baselines under hardware-pinned CPU affinity controls.
The evaluation focuses on four distinct IPC paradigms, each implemented in C++17 with direct OS syscall calls:
---
## II. System Architecture & Topologies
```mermaid
graph TD
    subgraph POSIX Pipe
    end
```
### A. POSIX Named Pipes (FIFO)
POSIX pipes represent a uni-directional, kernel-buffered byte stream. They rely on standard file descriptors where synchronization is handled implicitly by the kernel scheduler:
- **Write Path**: Invokes the `write` system call, blocking the producer if the pipe buffer capacity is reached.
- **Read Path**: Invokes the `read` system call, blocking the consumer when no data is available.
- **Tuning**: The pipe buffer capacity is dynamically increased to the maximum payload size (1 MiB) via `fcntl(fd, F_SETPIPE_SZ)` to minimize backpressure blocking.
### A. Lock-Free SPSC Shared-Memory Architecture
The shared-memory ring buffer maps a cache-aligned `RingBuffer` struct into POSIX shared memory (`/dev/shm/ipc_ablation_ring`):
- **Cache-Line Separation (`alignas(64)`)**: The `head` and `tail` atomic indices are aligned to independent 64-byte boundaries. This prevents **false sharing** (cache-line invalidation traffic between Core 1 and Core 2).
- **Sequential Consistency Barriers**: Atomic operations use `std::memory_order_seq_cst` to prevent store-load CPU instruction reordering.
- **Out-of-Band Wakeup Coordination**: An atomic flag `consumer_sleeping` tracks whether the consumer thread is active or asleep. Wakeup signals are issued only when `consumer_sleeping == 1`, avoiding unnecessary kernel syscalls during active streaming.
### B. UNIX Domain Sockets (`AF_UNIX`)
Unix Domain Sockets provide bi-directional stream-oriented communication. Unlike network sockets, they bypass the network stack but still utilize the socket layer:
- **Connection Model**: Client-server topology using `SOCK_STREAM`.
- **System Call Path**: Relies on `send` and `recv` interfaces.
- **Tuning**: Socket transmit and receive buffers are tuned to 2 MiB via `setsockopt(..., SO_SNDBUF/SO_RCVBUF)` to support continuous high-rate streaming.
### B. The 5 Wakeup Mechanisms (Ablation Study)
### C. POSIX Message Queues (`mqueue`)
POSIX message queues are message-oriented, allowing structured, priority-based delivery:
- **API**: Relies on `mq_send` and `mq_receive`.
- **Limits**: Constrained by kernel sysctl limits (`fs.mqueue.msg_max` and `fs.mqueue.msgsize_max`). Message limits are set to 10 entries (`mq_maxmsg=10`) with payload sizing matching the dynamic sweep scale. **For payloads ≥ 64 KiB, the system default `fs.mqueue.msgsize_max` (typically 8 192 B) must be raised** before running: `sudo sysctl -w fs.mqueue.msgsize_max=1048688`
- **Filesystem Node**: Message queues are tracked inside the virtual VFS tree under `/dev/mqueue`, introducing inode lock evaluation paths in the kernel.
| Variant | Wait Strategy | Wakeup Signal | Idle CPU | Microsecond Latency | Use Case |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`busy_poll`** | Tight memory loop | None (always checking) | 100% Core | **Ultra-Low (<0.25 µs)** | High-Frequency Trading (HFT) |
| **`spin_backoff`**| Loop + `PP_PAUSE()` backoff | None (always checking) | High | **Very Low (<0.5 µs)** | Low-power userspace spinning |
| **`adaptive`** | Spin 500 iters, then sleep | `futex` WAKE | Low | **Dynamic (0.2 – 10 µs)** | General-purpose high-load systems |
| **`futex`** | System sleep (`FUTEX_WAIT`) | `futex` WAKE (`FUTEX_WAKE`) | **0%** | **Low-Medium (2 – 10 µs)** | Standard Linux thread sleep |
| **`io_uring`** | `io_uring_submit_and_wait` | Write to Signal FIFO | **0%** | **Optimized Medium** | Asynchronous event-driven IPC |
### D. `io_uring` + Shared Memory SPSC Ring
The high-performance transport maps a cache-aligned `RingBuffer` struct into POSIX shared memory (`shm_open` and `mmap`):
- **Lock-Free Atomics & Sequential Consistency**: Synchronization uses atomic indices `head` and `tail` along with the `consumer_sleeping` coordinator flag. The critical updates and checks use Sequential Consistency memory barriers (`std::memory_order_seq_cst`) to guarantee correct instruction ordering and prevent Store-Load CPU reordering deadlocks.
- **False-Sharing Avoidance**: The `head` and `tail` pointers are isolated on separate cache lines using `alignas(64)` boundaries, preventing cache-line invalidation loops (ping-ponging) between producer and consumer cores.
- **Dual-Ended Asynchronous Wakeup Signaling**: Coordinates sleep and wakeup events via a named FIFO (`/tmp/uring_sig_fifo`) and an atomic `consumer_sleeping` flag. The consumer blocks using `io_uring_submit_and_wait` to read from the signaling FIFO, and the producer wakes it up by submitting write tasks via `io_uring_prep_write`, making the out-of-band signaling path entirely asynchronous and handled via `io_uring` on both ends.
---
## III. Experimental Methodology
To isolate IPC overhead from OS scheduling jitter and hardware cache anomalies, we enforce the following experimental controls:
To guarantee publication-grade precision and isolate IPC performance from OS jitter, the benchmark suite enforces the following experimental controls:
### A. Message Size Sweep Matrix
Benchmarks are executed across an exponential sweep of payload sizes $S \in \{64\text{ B}, 256\text{ B}, 1024\text{ B}, 4\text{ KiB}, 16\text{ KiB}, 64\text{ KiB}, 256\text{ KiB}, 1\text{ MiB}\}$.
### A. Two-Suite Benchmark Design
1. **Ablation Study (`src/ablation/run_ablation.sh`)**:
   - Sweeps all 5 wakeup variants across **`saturated`** (maximum streaming throughput) and **`bursty`** (per-burst inter-arrival delay) traffic regimes.
   - Dynamically scales payload transfer volume ($16\text{ MiB}$ for small messages, $128\text{ MiB}$ for medium, $512\text{ MiB}$ for large) to provide over **260,000 statistical iterations per run** while executing sweeps in minutes.
2. **Ping-Pong Latency Suite (`src/pingpong/run_pingpong.sh`)**:
   - Enforces **Queue Depth = 1**: The initiator sends 1 message and stops; the responder echoes 1 message back. Exactly 1 message is in flight, eliminating queue backlog delay.
   - Evaluates **10,000 round-trip samples** per payload size to calculate exact P50, P90, P99, and P99.9 tail percentiles.
### B. CPU Affinity Pinning
Processor affinity is pinned statically using `sched_setaffinity` to isolate tasks onto physical CPU cores, preventing core-hopping and optimizing cache usage:
- **Producer Core**: Pinned to CPU Core 1.
- **Consumer Core**: Pinned to CPU Core 2.
### B. Single-Clock Source (`CLOCK_MONOTONIC_RAW`)
Cross-core hardware Time Stamp Counters (TSCs) drift by nanoseconds or microseconds. Taking $t_{\text{send}}$ on Core 1 and $t_{\text{recv}}$ on Core 2 introduces clock drift errors.
- **Solution**: Both $t_{\text{start}}$ and $t_{\text{end}}$ are sampled on the **Initiator thread on Core 1** using hardware-fenced `CLOCK_MONOTONIC_RAW`.
- $$\text{Single-Trip Latency} = \frac{t_{\text{end}} - t_{\text{start}}}{2}$$
### C. Analytical Metrics
For each message size class, the benchmark executes 1 warmup run (discarded) followed by $N = 15$ measured runs for all four IPC mechanisms. The following equations define the metrics:
1. **Throughput ($T$)**: The volume of data transferred per unit time:
   $$T = \frac{B}{1024^3 \times t_{\text{exec}}} \quad (\text{GiB/s})$$
   where $B$ is the total volume target in bytes, and $t_{\text{exec}}$ is the elapsed execution wall-clock time in seconds.
2. **End-to-End Latency ($L_i$)**: The time taken for message $i$ to traverse the IPC channel:
   $$L_i = t_{\text{recv}, i} - t_{\text{send}, i} \quad (\mu\text{s})$$
   where $t_{\text{send}, i}$ is stamped by the producer immediately before write/publish, and $t_{\text{recv}, i}$ is stamped by the consumer immediately after read/retrieve.
3. **Latency Standard Deviation ($\sigma$)**:
   $$\sigma = \sqrt{\frac{1}{M}\sum_{i=1}^M (L_i - \bar{L})^2}$$
   where $M$ is the number of latency samples and $\bar{L}$ is the arithmetic mean.
### C. Hardware Core Affinity
- **Producer / Initiator**: Pinned statically to **CPU Core 1** (`sched_setaffinity`).
- **Consumer / Echo Server**: Pinned statically to **CPU Core 2** (`sched_setaffinity`).
### D. Workload Target Sizing
To prevent brief runs from introducing clock resolution errors, target volumes scale based on the payload size classes and the specific IPC mechanism under test:
- **POSIX Pipes & POSIX Message Queues (Dynamic Sizing)**:
  - Small Payloads ($\le 1$ KiB): $32$ MiB total transfer.
  - Medium Payloads ($\le 64$ KiB): $256$ MiB total transfer.
  - Large Payloads ($> 64$ KiB): $2$ GiB total transfer.
- **UNIX Domain Sockets & `io_uring` Shared Ring (Static Sizing)**:
  - All Payload Sizes ($64$ Bytes to $1$ MiB): $2$ GiB total transfer.
### E. Checksum Verification
To prevent compilers from optimizing out the memory access path, the consumer performs a strided cache-line payload touch:
$$\text{Checksum} = \sum_{k=0}^{S/64} \text{Payload}[64 \times k]$$
This forces physical L1/L2 data cache fills, mimicking a real application reading incoming message payloads.
### F. Reference Test Environment
All benchmark runs and profiling tasks were executed on a dedicated test machine with the following physical and operating system specifications:
- **System Model**: ASUS Vivobook K3605ZF (Vivobook_ASUSLaptop K3605ZF_K3605ZF)
- **Operating System**: Ubuntu 24.04.4 LTS (noble)
- **CPU Architecture**: `x86_64`
- **Processor**: 12th Gen Intel(R) Core(TM) i5-12500H
  - **Thread/Core Layout**: 12 Physical Cores (16 Threads, 1 Socket)
  - **Frequency**: Max 4500.00 MHz / Min 400.00 MHz
  - **Caches**: L1d 448 KiB (12 instances), L1i 640 KiB (12 instances), L2 9 MiB (6 instances), L3 18 MiB (1 instance)
- **System Memory**: 16 GiB System Memory (2x 8GiB SODIMM DDR4 Synchronous 3200 MHz)
- **Virtual Memory**: 4.0 GiB Swap
- **Storage**: 512GB NVMe SSD (SAMSUNG MZVL4512HBLU-00BTW)
- **Graphics Processors**:
  - Integrated: Intel Corporation Alder Lake-P GT2 [Iris Xe Graphics]
  - Dedicated: NVIDIA Corporation GA107M [GeForce RTX 2050]
---
### IV. Repository Structure
## IV. Repository Structure
```
.
├── src/                          # Source code folders for the IPC benchmark targets
│   ├── pipe/                     # POSIX Named Pipes benchmark directory
│   │   ├── common.h              # Message structures and configuration
│   │   ├── pipe_producer.cpp     # Producer C++ application
│   │   ├── pipe_consumer.cpp     # Consumer C++ application & stats
│   │   └── run_pipe_bench.sh     # Script to compile and run pipe benchmark
├── src/
│   ├── ablation/                     # Benchmark #1: Wakeup Mechanism Ablation
│   │   ├── common.h                  # Common definitions and dynamic sizing logic
│   │   ├── ring.h                    # Cache-aligned RingBuffer layout
│   │   ├── wakeup.h                  # Implementations of all 5 wakeup variants
│   │   ├── producer.cpp              # Ablation producer application
│   │   ├── consumer.cpp              # Ablation consumer application & telemetry
│   │   └── run_ablation.sh           # Execution script for ablation sweep
│   │
│   ├── sockets/                  # UNIX Domain Sockets benchmark directory
│   │   ├── common.h              # Common structures and socket endpoints
│   │   ├── socket_producer.cpp   # Client socket producer C++ application
│   │   ├── socket_consumer.cpp   # Server socket consumer & statistics
│   │   └── run_socket_bench.sh     # Script to compile and run socket benchmark
│   │
│   ├── mq/                       # POSIX Message Queue benchmark directory
│   │   ├── common.h              # Common structures and MQ configuration
│   │   ├── mq_producer.cpp       # POSIX mq_send producer C++ application
│   │   ├── mq_consumer.cpp       # POSIX mq_receive consumer & statistics
│   │   └── run_mq_bench.sh       # Script to compile and run message queue benchmark
│   │
│   └── io_uring/                 # io_uring + Shared-Ring benchmark directory
│       ├── common.h              # Cache-aligned atomic RingBuffer layout
│       ├── uring_producer.cpp    # liburing-driven producer C++ application
│       ├── uring_consumer.cpp    # Shared-memory consumer C++ application & stats
│       └── run_uring_bench.sh    # Script to compile and run io_uring benchmark
│   └── pingpong/                     # Benchmark #2: Depth-1 Ping-Pong Latency Suite
│       ├── common.h                  # Ping-pong structures and constants
│       ├── pp_pipe.cpp               # POSIX Pipe ping-pong implementation
│       ├── pp_socket.cpp             # UNIX Domain Socket ping-pong implementation
│       ├── pp_mq.cpp                 # POSIX Message Queue ping-pong implementation
│       ├── pp_shm_uring.cpp          # SHM + io_uring ping-pong implementation
│       ├── pp_ablation.cpp           # SHM Ping-pong across all 5 wakeup variants
│       └── run_pingpong.sh           # Execution script for ping-pong suite & plotting
│
├── data/                         # CSV results datasets and performance log data
│   ├── pipe_results.csv
│   ├── socket_results.csv
│   ├── mq_results.csv
│   ├── io_uring_results.csv
│   └── Cache Misses              # Hardware performance counter logs
├── data/                             # Output CSV datasets
│   ├── ablation_*.csv                # Ablation per-variant CSV results
│   ├── pingpong_*_summary.csv        # Per-IPC ping-pong summary results
│   └── pingpong_results.csv          # Merged ping-pong results dataset
│
├── scripts/                      # Visualization and statistical scripts
│   ├── generate_visualizations.py# Process CSV data and generate plots
│   └── statistical_analysis.py   # Run 95% Confidence Interval validation reports
│
├── figures/                      # Directory for generated publication assets
│   ├── throughput.png            # Throughput comparison chart (GB/s)
│   ├── latency.png               # Latency comparison chart (microseconds, Log Scale)
│   ├── speedup.png               # io_uring Speedup comparison chart
│   ├── cache_misses.png          # Cache miss rates comparison chart
│   ├── cache_misses_summary.csv  # Cache miss CSV counts
│   ├── throughput_ci.png         # Throughput mean with 95% CI error bars
│   ├── latency_ci.png            # Latency mean with 95% CI error bars
│   ├── statistical_analysis.md   # Statistical validation report
│   └── flamegraphs/              # Interactive flamegraph gallery
│       ├── index.html            # Gallery page
│       ├── pipe_flamegraph.svg
│       ├── socket_flamegraph.svg
│       ├── mq_flamegraph.svg
│       └── final_io_uring.jpg
│
└── ipc_implementation_documentation.md # Detailed cross-implementation narrative
└── figures/                          # Generated publication-quality figures
    └── pingpong/                     # Latency vs Size (P50, P90, P99, P99.9) PNG plots
```
---
## V. Execution and Reproducibility
### A. System Configuration & Dependencies
Ensure compiler toolchains, `liburing`, and visualization dependencies are installed:
Install compiler toolchains, `liburing`, and Python plotting libraries:
```bash
# Install core build tools, liburing, and plotting libraries
sudo apt update
sudo apt install -y build-essential g++ liburing-dev python3 python3-matplotlib python3-numpy linux-tools-common linux-tools-$(uname -r)
```
### B. Hardware & Kernel Optimizations
Before executing benchmarks, apply system tuning to guarantee maximum measurement accuracy and eliminate kernel backpressure limits:
Apply these two system optimizations before running benchmarks to lock CPU frequency and eliminate kernel queue backpressure caps:
```bash
# 1. Lock CPU frequency to maximum performance (eliminates CPU frequency scaling latency spikes)
# 1. Set CPU governor to maximum performance (eliminates CPU frequency scaling latency spikes)
sudo cpupower frequency-set -g performance
# 2. Increase maximum POSIX Message Queue payload limits to 1 MiB
sudo sysctl -w fs.mqueue.msgsize_max=1048576
sudo sysctl -w fs.mqueue.msg_max=1024
```
### C. Benchmark Execution Workflow
### C. Running the Benchmark Suites
#### 1. Benchmark Suite #1: Ablation Study (Throughput & Wakeup Mechanisms)
Evaluates streaming throughput (GiB/s), CPU efficiency, and wakeup overhead across 5 wakeup variants (`busy_poll`, `spin_backoff`, `adaptive`, `futex`, and `io_uring`) under `saturated` and `bursty` regimes:
#### Step 1: Benchmark Suite #1 — Ablation Study
Executes the throughput and wakeup efficiency sweep across all 5 wakeup variants (`busy_poll`, `spin_backoff`, `adaptive`, `futex`, `io_uring`) under `saturated` and `bursty` regimes:
```bash
cd src/ablation
bash run_ablation.sh --regime saturated --regime bursty
```
- **Outputs**: Detailed per-variant CSV results generated in `data/ablation_*.csv`.
- **Outputs**: Per-variant CSV datasets saved in `data/ablation_*.csv`.
#### 2. Benchmark Suite #2: Ping-Pong Latency & Publication Plots
Evaluates true depth-1 unloaded round-trip latency ($L = \text{RTT} / 2$) and tail percentiles (P50, P90, P99, P99.9) across Pipes, UNIX Sockets, POSIX Message Queues, and Shared Memory + `io_uring`:
#### Step 2: Benchmark Suite #2 — Ping-Pong Latency & Plot Generation
Executes the Depth-1 ping-pong latency benchmark across Pipes, UNIX Sockets, POSIX MQ, and Shared Memory, aggregating results and generating publication charts:
```bash
cd ../pingpong
bash run_pingpong.sh
```
- **Outputs**: 
  - Merged datasets in `data/pingpong_results.csv` and `data/pingpong_*_summary.csv`.
  - Publication-ready charts saved automatically in `figures/pingpong/` (e.g., `fig_A_*.png`, `fig_B_*.png`).
- **Outputs**: Merged CSVs in `data/pingpong_results.csv` and PNG plots saved in `figures/pingpong/`.
---
## VI. Quantitative Results and Analysis
## VI. Key Quantitative Findings
### A. Throughput Scalability (`figures/throughput.png`)
Traditional transports hit a performance ceiling at medium-to-large message sizes (64 KiB–256 KiB) due to memory copies and kernel context transitions. The shared-memory ring achieves a peak speedup of **6.5×** over Unix Domain Sockets at 256 B and maintains a throughput lead through 256 KiB. However, at 1 MiB the ring is overtaken by Unix sockets (+14%) and POSIX MQ (+12%), because the fixed 64-slot ring depth causes producer stall when large slots cannot be recycled fast enough.
1. **Sub-Microsecond Unloaded Latency**: The lock-free shared memory ring buffer with `busy_poll` achieves a median single-trip latency of **0.208 µs (208 nanoseconds)** at 64 B payloads, compared to **36.6 µs** for Unix Sockets and POSIX Pipes.
2. **Zero-Idle-CPU Wakeups**: `futex` and `io_uring` wakeup variants consume **0% idle CPU** while delivering sub-15µs median round-trip latencies, offering an ideal trade-off for energy-efficient or cloud microservice environments.
3. **Tail Latency Stability**: Hardware core pinning (`Core 1` and `Core 2`) and cache-line separation (`alignas(64)`) prevent false sharing and keep P99 tail latencies stable across high-frequency message streams.
### B. Latency Distributions (`figures/latency.png`)
- **Traditional IPCs**: Average and P99 latencies exhibit high variance (visible in wider IQR bands), caused by kernel scheduling preemptions and thread awakening states.
- **Shared-Memory Ring**: Achieves the lowest p50 latency at 64 B (1.9 µs vs 3.3 µs for POSIX MQ, the next best) and at 256 KiB–1 MiB where slot queuing is minimal. Between 256 B and 64 KiB, POSIX MQ's `pipelined_send` kernel optimization yields lower median latency (3.8–9.2 µs) than the ring (6.1–27.2 µs). At 16–64 KiB, Unix sockets also achieve lower p50 than the ring.
### C. Cache and Memory Contention (`figures/cache_misses.png`)
Hardware counters highlight the structural differences between mechanisms. The shared-memory ring generates extremely high L1 load counts (~164 billion) due to its userspace spin-polling loop, with a 3.97% L1 miss rate. The `alignas(64)` cache-line isolation of `head`, `tail`, and `consumer_sleeping` prevents false sharing between producer and consumer cores — this is the primary cache benefit. Note: the perf captures are whole-benchmark totals (not per-message-size) and the POSIX MQ L1-miss counter was inactive during capture (shown as 0, not a true zero-miss result).
### D. CPU Profiling via Flamegraphs
Profiling with `perf` confirms:
- **Pipe/Socket/MQ**: Flamegraphs display deep, kernel-heavy stacks dedicated to locking mutexes, context switching, and copying buffers.
- **io_uring SPSC**: Shows shallow userspace call structures. The core execution is centered around pointer polling loops, bypassing costly kernel transitions.
---
## VII. References
* `[1]` J. Axboe, "Efficient IO with io_uring," Kernel Development Guide, 2019.
* `[2]` B. Gregg, "Flame Graphs," Communications of the ACM, vol. 59, no. 6, pp. 48–57, 2016.
* `[3]` W. R. Stevens and S. A. Rago, *Advanced Programming in the UNIX Environment*, 3rd ed. Addison-Wesley, 2013.
* `[4]` Linux Kernel Organization, "POSIX Pipes and FIFO Buffers Specifications," kernel.org documentation.
* `[5]` IEEE Standards Association, "POSIX.1b: Realtime Extension," IEEE Std 1003.1b-1993, 1993.
* `[1]` J. Axboe, "Efficient IO with io_uring," Linux Kernel Documentation, 2019.
* `[2]` W. R. Stevens and S. A. Rago, *Advanced Programming in the UNIX Environment*, 3rd ed. Addison-Wesley, 2013.
* `[3]` B. Gregg, *Systems Performance: Enterprise and the Cloud*, 2nd ed. Addison-Wesley, 2020.

```
