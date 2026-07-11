# FlameGraphs: Profiling Artifacts for IPC Implementations

This folder contains the flamegraph SVGs used to analyze CPU time distribution for each IPC implementation in this repository.

## 1. What This Folder Contains

- `pipe_flamegraph.svg`: flamegraph for the POSIX pipe benchmark path
- `socket_flamegraph.svg`: flamegraph for the Unix domain socket benchmark path
- `mq_flamegraph.svg`: flamegraph for the POSIX message queue benchmark path
- `uring_flamegraph.svg`: flamegraph for the io_uring + shared-ring benchmark path

These flamegraphs correspond to benchmark executions aggregated over repeated runs (including the multi-iteration setup used in this project).

## 2. Why Flamegraphs Are Included

Latency and throughput CSV files show *what* performance was observed. Flamegraphs help explain *why* by showing where CPU time is spent in call stacks:

- kernel copy paths
- synchronization or waiting behavior
- syscall overhead
- memory movement and cache-sensitive operations

This makes flamegraphs a key qualitative companion to the quantitative metrics in:

- `pipe_results.csv`
- `socket_results.csv`
- `mq_results.csv`
- `io_uring_results.csv`

## 3. How to Read a Flamegraph

For each SVG:

- **X-axis**: relative share of sampled CPU time (not a timeline)
- **Y-axis**: stack depth (call hierarchy)
- **Frame width**: wider frame means more sampled time in that function/subtree
- **Top frames**: leaf execution points at sample time

Interpretation rule of thumb:

1. Find the widest stacks first.
2. Identify whether they are user-space logic, libc/syscall wrappers, or kernel internals.
3. Compare dominant stacks across IPC variants to explain throughput/latency differences.

## 4. Expected Patterns by IPC Type

### 4.1 Pipe
- More time in read/write and kernel pipe data movement paths.
- Backpressure and FIFO buffering behavior can appear as syscall-heavy stacks.

### 4.2 Unix Domain Socket
- Time in socket send/recv and connection handling paths.
- Potential extra overhead from stream framing and socket buffer management.

### 4.3 POSIX Message Queue
- Time in mqueue kernel handling and queue bookkeeping.
- Queue limits and message copy behavior can influence visible hotspots.

### 4.4 io_uring Shared Ring
- More user-space memory/ring management activity.
- Lower transport-copy emphasis, but visible spin/poll/synchronization patterns may appear.

## 5. Viewing Options

### 5.1 Open Individual SVGs
Open any `*_flamegraph.svg` file directly in a browser for interactive zoom/search (depending on SVG content).

### 5.2 Use the Generated Gallery
After running visualization generation, open:

- `figures/flamegraphs/index.html`

This renders all flamegraphs in one page for side-by-side inspection.

## 6. Recommended Analysis Workflow

1. Review throughput and latency plots in `figures/`.
2. For each IPC mechanism, inspect the corresponding flamegraph.
3. Map dominant stacks to observed behavior (high p99, low throughput, etc.).
4. Cross-check with cache miss summary (`figures/cache_misses_summary.csv`) for memory-system effects.

## 7. Reproducibility Notes

For consistent flamegraph comparisons:

- keep core affinity fixed
- keep kernel/system load stable
- use same benchmark run configuration
- avoid mixing profiles from different code versions

Profiling quality is sensitive to sampling frequency, privilege mode, and kernel configuration.

## 8. Limitations

- Flamegraphs are sample-based approximations, not exact instruction-level accounting.
- X-axis is aggregated cost share, not temporal order.
- Different runs may show slight shifts due to scheduler and background noise.

Use flamegraphs as explanatory evidence alongside the CSV metrics, not as a standalone performance verdict.

## 9. Profiling Methodology and Generation

The SVGs in this directory were captured using the Linux `perf` sub-system combined with Brendan Gregg's `FlameGraph` helper scripts.

### 9.1 Data Collection Pipeline
To capture the execution context:
1. Start the consumer and producer processes for the desired IPC benchmark.
2. Run the `perf record` command to sample the CPU call graphs:
   ```bash
   # Sample CPU stack traces at 99 Hertz across all CPUs (-a) with call graphs (-g) for 15 seconds
   sudo perf record -F 99 -a -g -- sleep 15
   ```
3. Export the raw perf binary output into text format:
   ```bash
   sudo perf script > out.perf
   ```

### 9.2 Collapsing and Rendering Stack traces
The text traces are converted into the final SVG format through a two-stage script pipeline:
1. **Stack Collapsing**: `stackcollapse-perf.pl` aggregates identical call path stacks:
   ```bash
   ./stackcollapse-perf.pl out.perf > out.folded
   ```
2. **SVG Generation**: `flamegraph.pl` colors and draws the visual call hierarchy:
   ```bash
   ./flamegraph.pl out.folded > FlameGraphs/bench_flamegraph.svg
   ```

### 9.3 Stack Interpretations for This Project
- **`pipe_flamegraph.svg`**: Typically shows high density in kernel routines like `pipe_read` and `pipe_write`, plus scheduling wakeups (`try_to_wake_up`, `autoremove_wake_function`).
- **`socket_flamegraph.svg`**: Exposes socket buffers processing overhead, involving `unix_stream_recvmsg`, `unix_stream_sendmsg`, and standard socket enqueue paths.
- **`mq_flamegraph.svg`**: Contains frames dedicated to `sys_mq_timedsend` and `sys_mq_timedreceive`, representing the kernel POSIX message queue locks and memory copying.
- **`uring_flamegraph.svg`**: Under standard mode, shows entry via `io_uring_enter` syscall. In SQPOLL mode, highlights the kernel thread spinning inside `io_sq_thread` and calling NOP callbacks, leaving userspace stacks focused entirely on lock-free atomic shared ring operations with minimal context-switch overhead.
