**io_uring IPC --- Ablation Design, Latency Protocol & Reframed
Outline**

*For: Rahul Raman & Yuvraj Deshmukh (P03). Turning the report into a
publishable measurement study.*

**The core problem to fix.** As written, io_uring only handles the
occasional sleep/wakeup, so the reported gains actually come from the
shared-memory ring --- not io_uring. Part A isolates io_uring\'s real
contribution; Part B fixes the latency numbers; Part C reframes the
paper around what you can then defensibly claim.

# Part A --- Wakeup-Mechanism Ablation

**Goal.** Hold the data path identical (the same lock-free,
cache-aligned SPSC shared-memory ring) and vary ONLY the mechanism used
to block/wake when the ring goes empty (consumer) or full (producer).
This is the experiment that answers "what does io_uring actually buy for
IPC?"

## Wakeup variants (all on the identical ring)

-   **Busy-poll (spin).** Consumer spins on the tail index; no syscall,
    no sleep. Lower bound on latency, upper bound on CPU.

-   **Spin + backoff.** Spin with a CPU pause / exponential backoff,
    then yield. Practical middle ground.

-   **Adaptive spin-then-block.** Spin briefly (e.g., N iterations or
    \~2--10 us), then fall back to a blocking primitive. This is what
    production ring buffers actually do.

-   **futex.** futex_wait on a state word when empty; producer
    futex_wake when it publishes into a previously-empty ring. The
    standard conventional blocking primitive --- the key comparison
    point.

-   **eventfd.** Producer writes an eventfd; consumer read()/blocks on
    it. A common kernel-signaled wakeup.

-   **io_uring wakeup (current design).** io_uring services the
    empty-to-non-empty transition (e.g., poll / read on a FIFO or
    eventfd). This is what the report has today.

*Keep everything else fixed: same ring capacity, cache-line alignment,
seq_cst publication, CPU pinning, message-size sweep, and N. Only the
empty/full blocking mechanism changes --- that is the sole independent
variable.*

## Arrival regimes (this is what makes the wakeup matter)

The wakeup path is only exercised when the ring empties. Under a
saturating, back-to-back producer the ring rarely empties, so all
variants converge --- which is largely what the current report measures.
To expose the differences, run three regimes:

1.  **Saturated.** Producer always ahead; ring rarely empties. Baseline
    throughput; wakeup rarely triggered (state the convergence
    explicitly).

2.  **Bursty / rate-limited.** Producer sends then idles so the ring
    empties on most messages, forcing a sleep+wake per message. This
    regime is where io_uring vs. futex vs. busy-poll genuinely differ
    --- the heart of the ablation.

3.  **Offered-load sweep.** Poisson (or fixed-interval) arrivals at
    several rates below saturation, to trace how each mechanism behaves
    from lightly loaded to near-saturated.

## Metrics to capture per (variant, size, regime)

-   **Wakeup latency.** Time from the producer publishing into an empty
    ring to the consumer resuming and reading it --- the direct measure
    of the wakeup mechanism.

-   **Ping-pong latency.** Depth-1 round-trip latency (Part B), with
    tail percentiles.

-   **CPU cost.** Cycles per message and percent core utilization (perf
    stat). Busy-poll wins latency but burns a core; futex/io_uring
    should save CPU --- the latency-vs-CPU trade-off is the story.

-   **Syscalls per message and context switches.** perf stat / strace
    counts --- shows how often each mechanism actually enters the
    kernel.

-   **Throughput.** As already measured, per regime.

**Expected finding (why it is worth publishing).** The honest, likely
result is that for a single occasional SPSC wakeup, io_uring is roughly
on par with futex/eventfd (all cross into the kernel), while busy-poll
leads on latency at a large CPU cost. If so, the defensible claim
becomes: the shared-memory ring delivers the gains; io_uring adds little
over futex for single-ring SPSC wakeup, and its advantage would appear
only under batched or multi-ring coordination. That is a genuine,
citable result --- far stronger than an unsupported "io_uring IPC is 10x
faster" claim.

# Part B --- Corrected Ping-Pong Latency Protocol

**Why the current numbers are wrong.** Latency is currently computed as
t_recv - t_send during a saturating throughput run, with the producer
pipelining ahead --- so it measures queuing delay, not latency. Two
symptoms: a 5101 us "latency" for a 64 B pipe (implausible), and
io_uring reporting 2.449 us for a 256 KiB message that, at its own 20.6
GiB/s, must take \~12 us to transfer --- latency cannot be shorter than
the transfer. Both point to pipelined/queued measurement.

## The fix: a separate depth-1 ping-pong mode

4.  **Round-trip, one message in flight.** Client A sends one message;
    echo server B receives and immediately echoes the same message; A
    receives it. Never more than one message outstanding --- no
    pipelining, no queuing.

5.  **Single clock source.** Time the round trip entirely on core A
    (start before send, stop after receive). Do NOT subtract timestamps
    taken on two different cores --- per-core TSCs are not synchronized,
    which corrupts the current cross-core t_recv - t_send. One-way
    latency is approximately RTT / 2.

6.  **High-resolution, fenced clock.** Use
    clock_gettime(CLOCK_MONOTONIC_RAW) or rdtscp with an invariant TSC
    and a serializing fence; pin both processes; set the performance CPU
    governor and disable turbo to reduce jitter (optionally isolcpus /
    nohz_full for cleaner tails).

7.  **Distribution, not just the mean.** Run 10\^5 to 10\^6 round trips,
    discard warmup, and report median, mean, P90, P99, P99.9, and
    95% CI. Tail latency (P99/P99.9) matters more than the mean for the
    trading / real-time cases you cite.

8.  **Two clearly separated modes.** Report throughput from the
    saturating mode and latency from the ping-pong mode. Never derive
    latency from the throughput run again, and label each table with its
    mode.

*Run the ping-pong protocol across all four IPC mechanisms and all
Part-A wakeup variants, so latency, tails, and CPU cost are directly
comparable.*

# Part C --- Condensed IEEE Outline (reframed study)

## Suggested title

**How Much Does io_uring Help Shared-Memory IPC? A Measurement Study of
Wakeup Mechanisms for Lock-Free SPSC Rings**

*Honest framing: the study is about the shared-memory ring and the
wakeup ablation, not an "io_uring IPC mechanism."*

## Contributions

-   A controlled comparison of lock-free shared-memory ring IPC against
    kernel-mediated IPC (pipe, UNIX socket, POSIX MQ) across a 64 B to 1
    MiB message-size sweep.

-   A wakeup-mechanism ablation on an identical ring --- busy-poll,
    spin+backoff, adaptive, futex, eventfd, and io_uring --- isolating
    what io_uring actually contributes to IPC.

-   A corrected two-mode methodology: saturating throughput, plus
    depth-1 ping-pong latency with a single clock source and P99/P99.9
    tails.

-   A CPU-efficiency characterization (cycles/message, syscalls/message,
    context switches) quantifying the latency-vs-CPU trade-off across
    wakeup mechanisms.

-   A reproducible open-source artifact.

## Section structure (target \~8--10 pages)

**I. Introduction ---** IPC performance motivation; the research
question (does io_uring help IPC, and where?); contributions; roadmap.

**II. Background & Related Work ---** io_uring \[1\]; storage-I/O
studies (Didona et al. \[2\], Ren & Trivedi \[3\]) and how IPC differs;
lock-free SPSC rings (Lamport, Disruptor, DPDK rte_ring); futex/eventfd
wakeup primitives.

**III. Design ---** the lock-free, cache-aligned SPSC ring; the
pluggable wakeup layer (the six variants); why io_uring is kept off the
data path.

**IV. Methodology ---** the two measurement modes (throughput vs.
ping-pong latency); the wakeup-ablation matrix and arrival regimes;
CPU/syscall/context-switch counters; environment; N and confidence
intervals.

**V. Results ---** (A) ring vs. kernel-IPC throughput; (B) corrected
ping-pong latency with tails; (C) wakeup ablation under bursty load ---
the core new result; (D) CPU efficiency / cycles per message; (E)
cache-counter and flamegraph corroboration.

**VI. Discussion ---** when to use which mechanism; what io_uring buys
for IPC and its limits.

**VII. Threats to Validity ---** single machine / single
microarchitecture; SPSC only; clock and pinning caveats; workload
approximates real consumer work.

**VIII. Conclusion & Future Work ---** MPMC rings, cross-NUMA, a network
baseline (TCP loopback / RDMA), deeper tail analysis.

**References + Artifact Appendix ---** keep \[1\]--\[3\]; add
SPSC/Disruptor/DPDK and futex references. Move the repository-structure
and file-by-file walkthrough (report sections 5--6) here.

## What to cut from the 27-page report

-   Section 5 (repository structure) and Section 6 (file-by-file
    walkthrough) --- compress to a short design section plus an artifact
    appendix.

-   Trim the cache-counter discussion: report the relative LLC miss rate
    only, and drop the raw counts whose five-to-six order-of-magnitude
    gap is a workload-size artifact, not a finding.

-   Fold the deliverable-alignment / project-card framing into a
    one-line acknowledgment --- it is not paper content.
