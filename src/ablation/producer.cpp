// ablation_producer.cpp
//
// Usage: ./ablation_producer <variant_int> <regime_name> [inter_arrival_us]
//   variant_int     : 0=busy_poll  1=spin_backoff  2=adaptive
//                     3=futex  4=eventfd  5=io_uring
//   regime_name     : saturated | bursty | offered_25 | offered_50 |
//                     offered_75 | offered_90
//   inter_arrival_us: override sleep between messages (0 = none)

#include "common.h"
#include "ring.h"
#include "wakeup.h"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <liburing.h>
#include <cstdio>

// ── CPU affinity ──────────────────────────────────────────────────────────────
static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// ── Regime → inter-arrival µs ─────────────────────────────────────────────────
static int regime_to_inter_arrival(Regime r) {
    switch (r) {
    case SATURATED:  return 0;
    case BURSTY:     return 1000; // 1 ms gap = heavily bursty
    case OFFERED_25: return INTER_ARRIVAL_US_25;
    case OFFERED_50: return INTER_ARRIVAL_US_50;
    case OFFERED_75: return INTER_ARRIVAL_US_75;
    case OFFERED_90: return INTER_ARRIVAL_US_90;
    }
    return 0;
}

// ── Generic producer loop parameterised on variant ────────────────────────────
template<typename WV>
static void run_all(RingBuffer* rb, WakeupState& ws,
                    struct io_uring* ring,
                    Regime regime, const char* regime_name) {
    int inter_arrival_us = regime_to_inter_arrival(regime);

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "[Producer] size=" << sz << " regime=" << regime_name << "\n";
        std::vector<char> payload(sz, 'X');

        for (int run = 0; run <= NUM_RUNS; ++run) {
            // ── Sync barrier: wait for consumer to reset head=0 tail=0 ────────
            int retries = 0;
            while (rb->head.load(std::memory_order_acquire) != 0 ||
                   rb->tail.load(std::memory_order_acquire) != 0) {
                usleep(100);
                if (++retries > 100000) { // 10s timeout
                    std::cerr << "[Producer] TIMEOUT waiting for sync barrier!\n";
                    return;
                }
            }

            size_t produced = 0;
            uint64_t wakeup_count = 0;

            while (produced < TOTAL_BYTES) {
                uint64_t h = rb->head.load(std::memory_order_relaxed);
                uint64_t t = rb->tail.load(std::memory_order_acquire);

                if ((h - t) >= NUM_SLOTS) {
                    CPU_PAUSE();
                    continue;
                }

                auto& slot      = rb->slots[h % NUM_SLOTS];
                std::memcpy(slot.data, payload.data(), sz);
                slot.size       = static_cast<uint32_t>(sz);
                slot.send_ns    = now_ns();

                rb->head.store(h + 1, std::memory_order_seq_cst);

                // Mark wakeup_publish_ns just before signalling
                slot.wakeup_publish_ns = now_ns();
                WV::producer_signal(rb, ws, ring, &wakeup_count);

                produced += sz;

                // Inject inter-arrival gap per burst (every NUM_SLOTS messages)
                if (inter_arrival_us > 0 && ((h + 1) % NUM_SLOTS == 0))
                    usleep(static_cast<useconds_t>(inter_arrival_us));
            }
            usleep(2000); // brief pause before next run
        }
    }
}

// ── Runtime dispatch ──────────────────────────────────────────────────────────
static void dispatch(WakeupVariant v, RingBuffer* rb, WakeupState& ws,
                     struct io_uring* ring,
                     Regime regime, const char* regime_name) {
    switch (v) {
    case BUSY_POLL:    run_all<wakeup::BusyPoll>   (rb, ws, ring, regime, regime_name); break;
    case SPIN_BACKOFF: run_all<wakeup::SpinBackoff>(rb, ws, ring, regime, regime_name); break;
    case ADAPTIVE:     run_all<wakeup::Adaptive>   (rb, ws, ring, regime, regime_name); break;
    case FUTEX:        run_all<wakeup::FutexWakeup>(rb, ws, ring, regime, regime_name); break;
    case EVENTFD:      run_all<wakeup::EventFD>    (rb, ws, ring, regime, regime_name); break;
    case IO_URING:     run_all<wakeup::IoUring>    (rb, ws, ring, regime, regime_name); break;
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <variant_int> <regime_name> [inter_arrival_us]\n";
        return 1;
    }

    WakeupVariant variant = static_cast<WakeupVariant>(std::atoi(argv[1]));
    const char*   regime_name = argv[2];

    if (variant < 0 || variant >= NUM_VARIANTS) {
        std::cerr << "variant must be 0-" << (NUM_VARIANTS-1) << "\n";
        return 1;
    }

    // parse regime
    Regime regime = SATURATED;
    for (int i = 0; i < NUM_REGIMES; ++i) {
        if (std::strcmp(REGIME_NAMES[i], regime_name) == 0) {
            regime = static_cast<Regime>(i);
            break;
        }
    }

    set_affinity(PRODUCER_CORE);
    std::cout << "[Producer:" << VARIANT_NAMES[variant]
              << "] regime=" << regime_name << "\n";

    // ── Shared memory ────────────────────────────────────────────────────────
    // Consumer creates the SHM; wait for it to appear
    int shm_fd = -1;
    for (int retry = 0; retry < 50; ++retry) {
        shm_fd = shm_open(SHM_RING_NAME, O_RDWR, 0666);
        if (shm_fd >= 0) break;
        usleep(100000); // 100 ms
    }
    if (shm_fd < 0) { std::perror("shm_open (producer)"); return 1; }

    auto* rb = static_cast<RingBuffer*>(
        mmap(nullptr, sizeof(RingBuffer),
             PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (rb == MAP_FAILED) { std::perror("mmap"); return 1; }

    // ── WakeupState setup ────────────────────────────────────────────────────
    WakeupState ws;
    struct io_uring ring{};

    if (variant == EVENTFD) {
        // Read consumer's PID + fd from the pid file
        const char* pid_file = "/tmp/ablation_consumer_pid";
        for (int retry = 0; retry < 30; ++retry) {
            FILE* pf = fopen(pid_file, "r");
            if (pf) {
                int cpid = 0, cfd = 0;
                if (fscanf(pf, "%d %d", &cpid, &cfd) == 2) {
                    fclose(pf);
                    char fd_path[128];
                    snprintf(fd_path, sizeof(fd_path),
                             "/proc/%d/fd/%d", cpid, cfd);
                    ws.eventfd_fd = open(fd_path, O_RDWR);
                    if (ws.eventfd_fd >= 0) break;
                } else {
                    fclose(pf);
                }
            }
            usleep(100000);
        }
        if (ws.eventfd_fd < 0) { std::perror("open eventfd via /proc"); return 1; }
    } else if (variant == IO_URING) {
        if (io_uring_queue_init(64, &ring, 0) < 0) {
            std::perror("io_uring_queue_init"); return 1;
        }
        // Wait for FIFO to be created by consumer
        for (int retry = 0; retry < 50; ++retry) {
            ws.fifo_fd = open(SIGNAL_PATH, O_RDWR | O_NONBLOCK);
            if (ws.fifo_fd >= 0) break;
            usleep(100000);
        }
        if (ws.fifo_fd < 0) { std::perror("open signal fifo"); return 1; }
    }

    // ── Run ──────────────────────────────────────────────────────────────────
    dispatch(variant, rb, ws, &ring, regime, regime_name);

    if (variant == EVENTFD && ws.eventfd_fd >= 0) close(ws.eventfd_fd);
    if (variant == IO_URING) {
        io_uring_queue_exit(&ring);
        if (ws.fifo_fd >= 0) close(ws.fifo_fd);
    }
    munmap(rb, sizeof(RingBuffer));
    close(shm_fd);

    std::cout << "[Producer:" << VARIANT_NAMES[variant] << "] Done.\n";
    return 0;
}
