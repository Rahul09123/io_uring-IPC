# Senior Systems Engineering Review
## *io_uring IPC Comparative Benchmark — Code-Level Analysis*

**Reviewer:** Senior Systems Engineer
**Date:** July 2026
**Repository:** `Rahul09123/io_uring-IPC`
**Status:** Submitted for Professor Review

> **Reviewer's note:** Every claim in this document was verified against the actual source code (`src/*/common.h`, `*.cpp`), the committed CSV files in `data/`, and the raw `perf` output in `data/Cache Misses`. No README or comment was taken at face value without cross-checking the implementation.

---

## Table of Contents

1. [Architecture: Real vs. Claimed Data Paths](#1-architecture-real-vs-claimed-data-paths)
2. [Benchmark Methodology Analysis](#2-benchmark-methodology-analysis)
3. [Throughput Results](#3-throughput-results)
4. [Latency Results — p50 and p99](#4-latency-results--p50-and-p99)
5. [Speed-Up Ratio Analysis](#5-speed-up-ratio-analysis)
6. [Cache Miss and Profiling Data](#6-cache-miss-and-profiling-data)
7. [Claims Scorecard](#7-claims-scorecard)
8. [Key Findings for Supervisor Discussion](#8-key-findings-for-supervisor-discussion)
9. [Recommended Next Steps](#9-recommended-next-steps)

---

## 1. Architecture: Real vs. Claimed Data Paths

### 1.1 Actual Data Flow — All Four Mechanisms

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  MECHANISM 1: POSIX Named Pipe (FIFO)                                       │
│                                                                             │
│  Producer (Core 1)                          Consumer (Core 2)               │
│  ┌──────────────────────┐                  ┌──────────────────────┐        │
│  │ write(fd, header+    │  ──── syscall ─▶  │ read(fd, &header)    │        │
│  │   payload, sz)       │                  │ read(fd, payload,    │        │
│  │ loop until 2GB/32MB  │  KERNEL COPY ──▶  │   payload_sz)        │        │
│  │ sched_yield()        │  via pipe_write  │ strided checksum     │        │
│  └──────────────────────┘  /pipe_read      └──────────────────────┘        │
│                                                                             │
│  Verified: Two syscalls per message (header + payload separately).         │
│  Pipe buffer tuned to 1MB via fcntl(F_SETPIPE_SZ).                        │
│  Data path: user buf → kernel FIFO ring → user buf. One kernel copy.       │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│  MECHANISM 2: Unix Domain Socket (AF_UNIX / SOCK_STREAM)                    │
│                                                                             │
│  Producer (Core 1)                          Consumer (Core 2)               │
│  ┌──────────────────────┐                  ┌──────────────────────┐        │
│  │ connect() per run    │  ──── syscall ──▶ │ listen/accept()      │        │
│  │ write(fd, hdr+pay)   │                  │ read(fd, &header)    │        │
│  │ until 2GB            │  KERNEL sk_buff ─▶│ read(fd, payload)   │        │
│  │ close() per run      │  socket layer     │ strided checksum    │        │
│  └──────────────────────┘                  └──────────────────────┘        │
│                                                                             │
│  Verified: New connect/accept per run (adds setup overhead per run).       │
│  SO_SNDBUF/SO_RCVBUF tuned to 2MB via setsockopt.                         │
│  Data path: user buf → kernel sk_buff → user buf.                          │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│  MECHANISM 3: POSIX Message Queue (mqueue)                                  │
│                                                                             │
│  Producer (Core 1)                          Consumer (Core 2)               │
│  ┌──────────────────────┐                  ┌──────────────────────┐        │
│  │ mq_send(mq, buf,     │  ──── syscall ──▶ │ mq_receive(mq, buf,  │        │
│  │   hdr+payload_sz)    │                  │   maxsz, NULL)       │        │
│  │ dynamic total bytes  │  KERNEL mqueue ─▶│ extract header       │        │
│  │ (32MB/256MB/2GB)     │  inode+VFS lock  │ strided checksum     │        │
│  └──────────────────────┘                  └──────────────────────┘        │
│                                                                             │
│  Verified: mq_maxmsg=10 (shallow queue, depth 10 messages).               │
│  Large payloads may need sysctl fs.mqueue.msgsize_max — NOT documented.    │
│  Data path: user buf → kernel mqueue inode → user buf. One kernel copy.    │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│  MECHANISM 4: io_uring + Shared Memory Ring  ◀── ALL CLAIMS VERIFIED HERE  │
│                                                                             │
│  HOT DATA PATH (no syscall, no io_uring in hot loop):                      │
│  Producer (Core 1)              Shared Memory (/ipc_uring_ring_buffer)      │
│  ┌──────────────────────┐      ┌─────────────────────────────────────┐     │
│  │ check head-tail < 64 │      │ 64 slots × 1MB each = 64MB SHM     │     │
│  │ memcpy(slot.data,    │─────▶│ struct Slot { send_ns, size,        │     │
│  │   payload, sz)       │      │               char data[1048576] }  │     │
│  │ head.store(seq_cst)  │      │ head (atomic, cache-line aligned)   │     │
│  └──────────────────────┘      │ tail (atomic, cache-line aligned)   │     │
│  Consumer (Core 2)             │ consumer_sleeping (atomic flag)     │     │
│  ┌──────────────────────┐      └─────────────────────────────────────┘     │
│  │ spin: tail!=head?    │◀─────                                             │
│  │ recv_ns = now_ns()   │                                                   │
│  │ checksum over slot   │      COLD WAKEUP PATH (one syscall when empty):  │
│  │ tail.store(release)  │      Consumer: IORING_OP_READ on named FIFO      │
│  └──────────────────────┘      Producer: IORING_OP_WRITE on named FIFO     │
│                                                                             │
│  SQPOLL STATUS: flags=0 in io_uring_queue_init() → NOT ACTIVE              │
│  SQPOLL_CORE=3 is defined in common.h but NEVER PASSED to init.            │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 The Critical io_uring Finding

> **The io_uring mechanism does NOT use io_uring for data transfer.**

Verified at `uring_producer.cpp:90` (memcpy into slot) and `uring_producer.cpp:109` / `uring_consumer.cpp:129` (io_uring on FIFO):

- **Data path:** Pure `memcpy()` into POSIX shared memory slots. Zero io_uring involvement in the hot path.
- **io_uring usage:** Only for the *wakeup signaling path* when the ring is empty — `IORING_OP_WRITE` on a named FIFO `/tmp/uring_sig_fifo` (producer) and `IORING_OP_READ` on that same FIFO (consumer).
- **SQPOLL status:** `io_uring_queue_init(N, &ring, 0)` — flags=`0`. `IORING_SETUP_SQPOLL` would require `flags = IORING_SETUP_SQPOLL` (value `0x8`). `SQPOLL_CORE=3` is declared in `common.h` as dead code.
- **Net result:** This is a *shared-memory SPSC ring with io_uring-assisted wakeup*, not an io_uring data transport. The "io_uring" label is architecturally misleading for the actual bottleneck path.

### 1.3 Core Affinity and Privilege Summary

| Mechanism | Producer Core | Consumer Core | SQPOLL Core | sudo Required? |
|:---|:---:|:---:|:---:|:---:|
| Pipe | 1 | 2 | — | No |
| Socket | 1 | 2 | — | No |
| MQ | 1 | 2 | — | No (sysctl may be needed) |
| io_uring (actual) | 1 | 2 | 3 (unused) | No |
| io_uring (if SQPOLL enabled) | 1 | 2 | 3 | CAP_SYS_ADMIN or paranoid=1 |

---

## 2. Benchmark Methodology Analysis

### 2.1 SQPOLL Discrepancy — Code vs. Documentation

```
What README/figures claim:           What code actually does:

io_uring_queue_init(                 io_uring_queue_init(
  64,                                  64,          ← queue depth
  &ring,                               &ring,
  IORING_SETUP_SQPOLL          ←✗      0            ← flags = 0 (NO SQPOLL)
);                                   );

Source of claim:                     Source of truth:
  README §I "SQPOLL"                   uring_producer.cpp:63
  README §D "SQPOLL kernel polling"    uring_consumer.cpp:66
  figures/README §3.4 "io_sq_thread"
```

**Impact:** The `io_sq_thread` kernel thread described in the flamegraph README (§3.4) would only exist if `IORING_SETUP_SQPOLL` were active. Since it is not, the io_uring consumer must use `io_uring_submit_and_wait()` — which *is* a system call — to sleep when the ring is empty. This is correctly implemented; the description is what is wrong.

### 2.2 Methodology Inconsistency — Total Bytes per Run

The four mechanisms transfer **different** total bytes at the same message sizes. This is intentional (documented in README §III.D) but the data is not directly comparable at 64B–64KB.

| Size | io_uring | Pipe | Socket | MQ | Equal? |
|:---:|:---:|:---:|:---:|:---:|:---:|
| 64 B | **2.0 GB** | 32 MB | **2.0 GB** | 32 MB | ✗ 64× diff |
| 256 B | **2.0 GB** | 32 MB | **2.0 GB** | 32 MB | ✗ 64× diff |
| 1 KB | **2.0 GB** | 32 MB | **2.0 GB** | 32 MB | ✗ 64× diff |
| 4 KB | **2.0 GB** | 256 MB | **2.0 GB** | 256 MB | ✗ 8× diff |
| 16 KB | **2.0 GB** | 256 MB | **2.0 GB** | 256 MB | ✗ 8× diff |
| 64 KB | **2.0 GB** | 256 MB | **2.0 GB** | 256 MB | ✗ 8× diff |
| 256 KB | 2.0 GB | 2.0 GB | 2.0 GB | 2.0 GB | ✓ |
| 1 MB | 2.0 GB | 2.0 GB | 2.0 GB | 2.0 GB | ✓ |

> **Reproducibility issue — Run count mismatch:** `src/pipe/common.h` and `src/sockets/common.h` both define `NUM_RUNS = 5`. However, `data/pipe_results.csv` and `data/socket_results.csv` both contain **15 rows per message size**. Regenerating benchmarks from the current source code would produce CSVs with 5 rows per size, not 15. This means the committed data and current code are **not in sync**. Either the CSVs were produced from a prior version where NUM_RUNS=15, or someone edited the source back to 5 after the fact.

### 2.3 Benchmark Pipeline

```
┌─────────────────────────────────────────────────────────────────────────┐
│  BENCHMARK EXECUTION PIPELINE (all four mechanisms)                     │
│                                                                         │
│  ┌──────────┐   ┌────────────┐   ┌──────────────┐   ┌──────────────┐  │
│  │ g++ -O2  │──▶│ Pin cores  │──▶│ Warmup       │──▶│ N measured   │  │
│  │ compile  │   │ (affinity) │   │ run 0        │   │ runs 1..N    │  │
│  └──────────┘   └────────────┘   │ (discarded)  │   └──────┬───────┘  │
│                                  └──────────────┘          │          │
│                                                             ▼          │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐  │
│  │ Per-run stats:   │   │ Wall-clock timer  │   │ CSV output       │  │
│  │ sort latencies   │   │ wraps hot loop    │   │ per-run row      │  │
│  │ compute p50/p99  │──▶│ throughput=B/t    │──▶│ (no aggregation) │  │
│  │ stddev           │   │                  │   └──────────────────┘  │
│  └──────────────────┘   └──────────────────┘                         │
│                                                                         │
│  "Checksum" step:  volatile sum of payload[0], payload[64], ...        │
│  ─ Forces L1 cache loads; result cast to void. NOT data integrity.     │
│  ─ No CRC, hash, or byte-level equality check exists anywhere.         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.4 Latency Measurement — Critical Asymmetry

| Mechanism | send_ns stamped | recv_ns stamped | Includes queue wait? |
|:---|:---|:---|:---:|
| Pipe | Before `write()` | After `read()` returns | No — blocking call |
| Socket | Before `write()` | After `read()` returns | No — blocking call |
| MQ | Before `mq_send()` | After `mq_receive()` returns | No — blocking call |
| io_uring | Before `memcpy()` into slot | After consumer's spin detects slot | **Yes** — includes ring residence time |

The io_uring latency includes any time a slot sits populated but unconsumed. This is correct end-to-end latency but is semantically different from the "syscall round-trip" measured by the blocking mechanisms.

---

## 3. Throughput Results

*All numbers computed directly from committed `data/*.csv` — mean across all 15 runs per size.*

### 3.1 Throughput Comparison Table

| Size | io_uring | Pipe | Socket | MQ | Winner |
|:---:|:---:|:---:|:---:|:---:|:---|
| 64 B | **0.567** | 0.139 | 0.093 | 0.160 | io_uring (6.1× vs Socket) |
| 256 B | **2.306** | 0.541 | 0.357 | 0.578 | io_uring (6.5× vs Socket) |
| 1 KB | **7.746** | 1.675 | 1.258 | 1.776 | io_uring (6.2× vs Socket) |
| 4 KB | **18.609** | 3.480 | 3.402 | 4.405 | io_uring (5.5× vs Socket) |
| 16 KB | **28.207** | 4.442 | 9.125 | 8.281 | io_uring (6.4× vs Pipe) |
| 64 KB | **28.167** | 5.072 | 11.067 | 11.464 | io_uring (5.6× vs Pipe) |
| 256 KB | **20.601** | 6.180 | 13.198 | 13.026 | io_uring (only 1.6× gap) |
| **1 MB** | 9.704 | 4.555 | **11.100** | 10.873 | **Socket wins; io_uring is LAST** |

*(GB/s — GiB/s convention as per source code throughput formula)*

### 3.2 Throughput Trend (ASCII Chart)

```
GB/s
30 ┤                              io_uring peak
   │                            ┌──────────────┐  28.2     28.2
25 ┤                            │              └──────────────┐
   │                            │                            │ 20.6
20 ┤                       18.6 ┤                            │
   │                            │                            │
15 ┤                            │    Socket 13.2─────────┐  │
   │                            │    MQ     11.5────┐    │  │
10 ┤                            │    Socket  9.1    │    │  │
   │              7.75          │    MQ      8.3    │    │  │ 9.7 io_uring
   │                            │                   │    │  └──────────────
 5 ┤       2.3                  │    Pipe 4.4─5.1───┘    └──────────── 11.1
   │  0.57                      │
 0 ┴───────────────────────────────────────────────────────────────────
    64B  256B  1KB   4KB  16KB  64KB  256KB  1MB
```

**Takeaway:** io_uring (shared memory) dominates 64B–256KB. At 1MB it falls behind both Socket and MQ. The peak claimed is "10×" but the data shows a maximum of 6.5× at 256B.

---

## 4. Latency Results — p50 and p99

### 4.1 p50 Latency Comparison (µs) — mean across all runs

| Size | io_uring | Pipe | Socket | MQ | io_uring vs nearest |
|:---:|:---:|:---:|:---:|:---:|:---|
| 64 B | **1.916** | 5101.7 | 220.7 | 3.324 | 1.7× better than MQ |
| 256 B | 6.105 | 1473.5 | 145.2 | **3.771** | **WORSE than MQ by 1.6×** |
| 1 KB | 7.516 | 415.1 | 90.4 | **5.215** | **WORSE than MQ by 1.4×** |
| 4 KB | 12.447 | 177.4 | 59.7 | **9.243** | **WORSE than MQ by 1.3×** |
| 16 KB | 32.007 | 178.0 | **26.63** | 20.51 | **WORSE than Socket and MQ** |
| 64 KB | 27.150 | 167.8 | **25.78** | 61.09 | **WORSE than Socket** |
| 256 KB | **2.449** | 129.4 | 32.70 | 66.03 | 13× better than Socket |
| 1 MB | **3.140** | 229.4 | 97.90 | 153.1 | 31× better than Socket |

> **Anomaly at 256KB and 1MB:** io_uring p50 *drops* to 2.4µs and 3.1µs — less than small-message sizes (256B = 6.1µs). This is a measurement artifact. At large messages, fewer total messages are transferred per run, so each slot is picked up quickly relative to its enqueue time. The ring does not build up backlog, reducing measured queuing latency.

### 4.2 p99 Latency Comparison (µs) — mean across all runs

| Size | io_uring | Pipe | Socket | MQ | Notes |
|:---:|:---:|:---:|:---:|:---:|:---|
| 64 B | 6.10 | 23107 | 340.5 | **5.07** | MQ beats io_uring at p99! |
| 256 B | 8.41 | 7898 | 208.4 | **5.66** | MQ 33% lower p99 |
| 1 KB | 11.12 | 4272 | 175.5 | **7.29** | MQ 34% lower p99 |
| 4 KB | 18.55 | 303.1 | 131.0 | **12.09** | MQ 35% lower p99 |
| 16 KB | 51.61 | 441.3 | **42.07** | **25.34** | Both Socket and MQ win |
| 64 KB | 46.43 | 494.0 | **44.03** | 79.70 | Socket comparable |
| 256 KB | 83.12 | 310.3 | **47.93** | 177.7 | Socket wins at p99 |
| 1 MB | **47.12** | 825.7 | 164.1 | 1386.1 | io_uring best at 1MB p99 |

### 4.3 Latency Log-Scale Visual

```
Log scale p50 latency (µs)
100,000 ┤ ■ Pipe@64B: 5,102 µs
 10,000 ┤ ■ Pipe@256B: 1,474 µs
  1,000 ┤ ■ Pipe@1KB: 415 µs    ■ Socket@64B: 221 µs
    100 ┤                         □ Socket@256B: 145      □ Socket@1KB: 90
     10 ┤ ◆ MQ@64B: 3.3          ◆ MQ@256B: 3.8          ◆ MQ@1KB: 5.2
      3 ┤ ● io_uring@64B: 1.9   ●io_uring@256B: 6.1     ●io_uring@256KB: 2.4
      1 ┴─────────────────────────────────────────────────────────────────
         64B          256B-64KB                    256KB-1MB
  ●=io_uring  ◆=MQ  □=Socket  ■=Pipe
```

**Takeaway:** io_uring p50 latency is best only at 64B and at 256KB+. MQ has better p50 at 256B through 64KB, contradicting the README's claim that "io_uring maintains the lowest latency across the scale."

---

## 5. Speed-Up Ratio Analysis

*Ratio = io_uring GB/s ÷ baseline GB/s. Values < 1.0 = io_uring is SLOWER.*

### 5.1 Speed-Up Table

| Size | vs Pipe | vs Socket | vs MQ | io_uring SLOWER? |
|:---:|:---:|:---:|:---:|:---:|
| 64 B | 4.07× | 6.10× | 3.55× | — |
| 256 B | 4.27× | **6.46×** | 3.99× | — |
| 1 KB | 4.62× | 6.16× | 4.36× | — |
| 4 KB | 5.35× | 5.47× | 4.22× | — |
| 16 KB | **6.35×** | 3.09× | 3.41× | — |
| 64 KB | 5.55× | 2.55× | 2.46× | — |
| 256 KB | 3.33× | 1.56× | 1.58× | — (gap closing) |
| **1 MB** | 2.13× | **0.87×** | **0.89×** | **YES — SLOWER** |

### 5.2 Speed-Up Chart

```
Speed-up factor (io_uring / baseline)
 7× ┤                 6.5×
    │              ●        ●
 6× ┤            ●            ●
    │           ●               ●   ←─ vs Socket (peak impact)
 5× ┤          ●    ·   ·   ·
    │        ●      ▲            ▲  ←─ vs Pipe
 4× ┤      ●        4.6×   ■   ■
    │             ■                  ·     ·  2.1×
 3× ┤            ■   ■   ■   ■
    │               ·   ·   ·    ·    ·      · 2.5×
 2× ┤                                  ■
    │
 1× ┤ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ PARITY LINE ─ ─ ─ ─ ─ ─ ─
    │                                              ▼ 0.87×  ▼ 0.89×
 0× ┴───────────────────────────────────────────────────────────────
     64B   256B  1KB   4KB  16KB  64KB  256KB    1MB

  ● = io_uring vs Socket
  ■ = io_uring vs MQ
  · = io_uring vs Pipe
  
  ZONE BELOW PARITY (io_uring SLOWER at 1MB):
    vs Socket: 0.87× (−13%)
    vs MQ:     0.89× (−11%)
    vs Pipe:   2.13× (io_uring still faster than pipe at all sizes)
```

> **Critical finding:** The README abstract claims "up to 10× throughput speedup." The committed data shows a maximum of **6.46×** (io_uring vs Socket at 256B). The 10× figure has no basis in the CSV data. Additionally, at 1MB, io_uring is the **worst performer** among the three high-performance options.

---

## 6. Cache Miss and Profiling Data

### 6.1 Raw perf Data — Source and Data Quality

The file `data/Cache Misses` is a single `perf stat` capture per mechanism over the entire benchmark run (all sizes combined). It is not per-message-size.

| Mechanism | L1 Loads | L1 Misses | L1 Miss% | LLC Misses | LLC Miss% | Wall Time |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| Pipe | 3,577,274 | 86,607 | 2.42% | 19,550 | 33.6% | 21s |
| Socket | 2,235,749 | 25,081 | 1.12% | 9,131 | 31.5% | 483s |
| **MQ** | **1,329,826** | **0** | **0.00%** | **401** | 3.2% | 14s |
| io_uring | 164.4 **billion** | 6.53 **billion** | 3.97% | 287.9M | 4.97% | 155s |

**Data Quality Flags:**

1. **MQ L1 misses = 0 is wrong.** The raw `perf` output shows `cpu_core/L1-dcache-load-misses/` as `not counted (0.00%)` for MQ — meaning the hardware counter was inactive or not multiplexed in, not that misses were zero. The CSV records this as 0, which is a misleading artifact.
2. **io_uring's 164 billion L1 loads are from spin-polling**, not payload transfer. The consumer spins on `tail != head` atomically in a tight loop, generating enormous L1 load counts even when the ring is empty. This inflates the raw counts by ~46,000× relative to pipe without indicating better cache behavior per byte.
3. **Wall-clock times differ by 23×** (socket=483s vs MQ=14s), making absolute counts incomparable without normalization by time or by bytes transferred.
4. **io_uring's flamegraph is a JPG** (`final_io_uring.jpg`), not an interactive SVG like the other three. Stack trace exploration is impossible for the primary mechanism.

### 6.2 README Claim vs. Data

> README §VI.C: *"io_uring's alignment of head/tail atomics keeps cache invalidations at zero."*

**Verdict: Incorrect.** io_uring generates 6.53 billion L1 cache misses (3.97% miss rate), *higher* than socket (1.12%) or pipe (2.42%). The cache-line alignment (`alignas(64)`) reduces **false sharing** between producer and consumer cores, which is genuine and valuable. However, "cache invalidations at zero" is not what the data shows. The spin-polling on the ring generates continuous cache traffic.

---

## 7. Claims Scorecard

| # | Claim | Status | Evidence |
|:---|:---|:---:|:---|
| 1 | "up to 10× throughput speedup" | ❌ Not Supported | Data max = 6.46× (io_uring vs Socket at 256B). No 10× row exists in any CSV. |
| 2 | "speedup under medium message sizes" | ⚠ Partial | Direction correct; multiplier overstated by 35%. |
| 3 | "reduces median latency by several orders of magnitude" | ❌ Not Supported | True vs pipe only (2700×). MQ has lower p50 at 256B–64KB. |
| 4 | "io_uring maintains the lowest latency across the scale" | ❌ Not Supported | MQ p50 beats io_uring at 256B–64KB; Socket beats io_uring at 16KB–64KB p50. |
| 5 | "leverages Linux's io_uring kernel polling (SQPOLL)" | ❌ Not Supported | `io_uring_queue_init(N, &ring, 0)` — flags=0 → no SQPOLL anywhere in code. |
| 6 | "io_uring wakeup signaling via FIFO on both ends" | ✅ Done | Producer: `io_uring_prep_write` on FIFO. Consumer: `io_uring_submit_and_wait`. Verified. |
| 7 | "zero-copy transfers" | ✅ Done | Hot path = `memcpy` into POSIX SHM. No kernel crossing for payload. |
| 8 | "lock-free SPSC ring buffer" | ✅ Done | Only C++ atomics; no mutex, semaphore, or futex anywhere in code. |
| 9 | "seq_cst barriers prevent Store-Load reordering" | ✅ Done | `memory_order_seq_cst` at head.store, consumer_sleeping.load/store. |
| 10 | "cache-line aligned head/tail to prevent false sharing" | ✅ Done | `alignas(64)` on head, tail, consumer_sleeping, and each Slot. |
| 11 | "CPU affinity pinning for all four mechanisms" | ✅ Done | `sched_setaffinity` confirmed in all 8 source files. |
| 12 | "Pipe/Socket N=5 runs; MQ/io_uring N=15 runs" | ⚠ Partial | Code constants say pipe/socket N=5 but committed CSVs have 15 rows/size. Code ≠ data. |
| 13 | "Dynamic workload sizing by mechanism type" | ✅ Done | `get_total_bytes()` implemented consistently in pipe/mq; socket/io_uring use static 2GB. |
| 14 | "Checksum verification for data integrity" | ⚠ Partial | Strided read prevents dead-code elim. No corruption detection. No error reporting. |
| 15 | "Per-mechanism cache-miss data" | ⚠ Partial | Whole-run captures only (not per-size). MQ cpu_core counters "not counted." io_uring flamegraph is non-interactive JPG. |
| 16 | "io_uring is fastest at all message sizes" (implied) | ❌ Not Supported | io_uring is slowest at 1MB: 9.70 GB/s vs Socket 11.10 (−13%) and MQ 10.87 (−11%). |

**Summary: 7 Done / 4 Partial / 5 Not Supported**

---

## 8. Key Findings for Supervisor Discussion

### Finding 1 — io_uring handles wakeup, not data [HIGH]

The "io_uring" benchmark is architecturally a **shared-memory SPSC ring with io_uring-assisted wakeup**. The payload traverses `memcpy` into POSIX SHM — io_uring only fires when the ring is empty. This is a valid and sophisticated design, but the naming and the framing in the README imply io_uring is in the data path. Before submission, the description must clarify this distinction. The existing architecture diagram in the README correctly omits io_uring from the data arrow, but the abstract and introduction text contradict it.

### Finding 2 — SQPOLL is declared but not used [HIGH]

Three sections (README §I, §D, figures/README §3.4) reference SQPOLL. The code uses `flags=0`. `SQPOLL_CORE=3` is dead code. This is a factual error in the submitted documentation. The flamegraph description of `io_sq_thread` executing on Core 3 is therefore either describing a behavior that never happened, or the flamegraph was captured from a different version of the code.

### Finding 3 — io_uring loses to Socket and MQ at 1MB [HIGH]

At 1MB payload, io_uring achieves 9.70 GB/s vs Socket's 11.10 GB/s (Socket is 14% faster). The ring buffer's 64-slot limit means the producer stalls on slot exhaustion at large messages. The socket kernel buffer can absorb more in-flight data. This is an important finding that the current results section (README §VI) does not acknowledge.

### Finding 4 — MQ has better p50 latency at 256B–64KB [MEDIUM]

The stated claim is "io_uring maintains the lowest latency across the scale." The data shows MQ p50 is 1.3–1.6× lower than io_uring at 256B through 16KB. This is likely because mqueue's `pipelined_send` kernel path delivers directly to a waiting receiver without queuing, matching or beating the spin-then-sleep model of the SPSC ring.

### Finding 5 — Run count mismatch between code and CSV [MEDIUM]

`common.h` for pipe and socket defines `NUM_RUNS=5`. The committed CSV files contain 15 runs per size. Any attempt to reproduce results by building and running the current code will produce a different dataset. This is a direct reproducibility failure.

### Finding 6 — Unequal workload volumes confound small-message comparisons [MEDIUM]

At 64B through 64KB, pipe and MQ transfer 32MB–256MB while io_uring and socket transfer 2GB. This 8–64× difference means the benchmark runs different scenarios: shorter runs with fewer kernel context-switches for pipe/MQ vs. sustained steady-state runs for io_uring/socket. The throughput comparison is valid directionally but not apples-to-apples.

### Finding 7 — Cache miss data is not normalized [LOW]

The `perf` data is reported as raw totals over entire benchmark runs with different durations (21s vs 483s). The claim that "io_uring maintains high cache locality" cannot be supported by raw L1 miss counts that are 75,000× higher than pipe's. MQ shows zero L1 misses due to a counter-collection failure, not actual cache performance.

---

## 9. Recommended Next Steps

### Immediate — Before Supervisor Review

| Priority | Action Item |
|:---:|:---|
| 🔴 | Fix SQPOLL claim in README §I, §D, figures/README §3.4. Change to "default interrupt mode (flags=0)" |
| 🔴 | Rename mechanism or add one-sentence clarification: "io_uring handles wakeup signaling only; payload moves via memcpy into POSIX shared memory" |
| 🔴 | Reconcile NUM_RUNS: either set pipe/socket to NUM_RUNS=15 in source, or regenerate CSVs from N=5 and recommit |
| 🟡 | Correct abstract: "up to 6.5× throughput speedup" (not 10×) |
| 🟡 | Correct §VI.B: specify size ranges where io_uring has lowest latency (64B and 256KB+); MQ is better at 256B–64KB |
| 🟡 | Add a sentence to §VI.A acknowledging the 1MB inversion |

### Near-Term — To Strengthen the Research

| Priority | Action Item |
|:---:|:---|
| 🟡 | Enable actual SQPOLL: `IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF`, `sq_thread_cpu = SQPOLL_CORE`. Re-run and see if results improve. |
| 🟡 | Test `io_uring_register_buffers` to see whether io_uring-native data transfer can compete with socket at 1MB |
| 🟡 | Increase `NUM_SLOTS` from 64 to 256 or 512 and re-benchmark 1MB to test whether the slot stall is the bottleneck |
| 🟢 | Equalize total bytes: use 2GB for all four mechanisms to enable fair comparison |
| 🟢 | Add per-message-size perf captures (scope `perf stat` to each size iteration) |
| 🟢 | Convert `final_io_uring.jpg` flamegraph to interactive SVG |
| 🟢 | Add actual data integrity verification (CRC32 on a sampled message subset) |

---

## Appendix: Source File Quick Reference

| File | Key Constants | Notes |
|:---|:---|:---|
| [src/io_uring/common.h](src/io_uring/common.h) | `NUM_RUNS=15`, `TOTAL_BYTES=2GB`, `NUM_SLOTS=64`, `SQPOLL_CORE=3` | SQPOLL_CORE unused |
| [src/pipe/common.h](src/pipe/common.h) | `NUM_RUNS=5`, dynamic TOTAL_BYTES | CSV has 15 runs — mismatch |
| [src/sockets/common.h](src/sockets/common.h) | `NUM_RUNS=5`, `TOTAL_BYTES=2GB` | CSV has 15 runs — mismatch |
| [src/mq/common.h](src/mq/common.h) | `NUM_RUNS=15`, dynamic TOTAL_BYTES, `mq_maxmsg=10` | mq_maxmsg shallow |
| [src/io_uring/uring_producer.cpp L63](src/io_uring/uring_producer.cpp) | `io_uring_queue_init(64, &ring, 0)` | flags=0 → no SQPOLL |
| [src/io_uring/uring_producer.cpp L90](src/io_uring/uring_producer.cpp) | `std::memcpy(slot.data, payload.data(), sz)` | Actual data path |
| [src/io_uring/uring_producer.cpp L109](src/io_uring/uring_producer.cpp) | `io_uring_prep_write(sqe, sig_fd, &sig, 1, 0)` | io_uring = wakeup only |
| [src/io_uring/uring_consumer.cpp L129](src/io_uring/uring_consumer.cpp) | `io_uring_prep_read(sqe, sig_fd, sig_buf, 1, 0)` | io_uring = wakeup only |
| [data/Cache Misses](data/Cache%20Misses) | MQ cpu_core `not counted (0.00%)` | MQ L1 miss = 0 is invalid |

---

*Report generated July 2026. All throughput and latency numbers computed directly from committed CSV files in `data/`. Source code analyzed line-by-line. No README claim was repeated without code verification.*
