#ifndef ABLATION_RING_H
#define ABLATION_RING_H

// ── SPSC ring — identical layout to src/io_uring/common.h ────────────────────
// The extra wakeup_publish_ns field in each slot allows the consumer to measure
// pure wakeup latency (time from "producer wrote the signal" to "consumer
// resumed"), independently of end-to-end latency.

#include "common.h"

#include <atomic>
#include <cstdint>

struct alignas(64) RingBuffer {
    alignas(64) std::atomic<uint64_t> head;
    alignas(64) std::atomic<uint64_t> tail;
    alignas(64) std::atomic<uint32_t> consumer_sleeping;

    struct alignas(64) Slot {
        uint64_t send_ns;           // timestamp written by producer before memcpy
        uint64_t wakeup_publish_ns; // timestamp written by producer just before signal
        uint32_t size;
        char     data[MAX_PAYLOAD];
    };
    Slot slots[NUM_SLOTS];
};

// ── WakeupState — fd-based variants share this struct ────────────────────────
// The consumer initialises this before starting the benchmark loop; the
// producer opens the same fds after the consumer has created them.
struct WakeupState {
    int eventfd_fd = -1; // used by EVENTFD variant
    int fifo_fd    = -1; // used by IO_URING variant
    // FUTEX / ADAPTIVE use RingBuffer::consumer_sleeping directly (no fd)
};

// ── Telemetry ─────────────────────────────────────────────────────────────────
struct Telemetry {
    double   latencies[MAX_LAT_SAMPLES];       // end-to-end µs
    double   wakeup_latencies[MAX_LAT_SAMPLES];// wakeup-only µs
    uint64_t latency_count;
    uint64_t wakeup_count;
    double   execution_time_sec;
    uint64_t wakeups_triggered;
    double   cpu_time_sec;
};

#endif // ABLATION_RING_H
