// ablation_consumer.cpp
//
// Usage: ./ablation_consumer <variant_int> <output_csv>
//   variant_int  : 0=busy_poll  1=spin_backoff  2=adaptive
//                  3=futex  4=eventfd  5=io_uring
//   output_csv   : path for per-run CSV output

#include "common.h"
#include "ring.h"
#include "wakeup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <liburing.h>

// ── CPU affinity ──────────────────────────────────────────────────────────────
static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// ── Timing helpers ────────────────────────────────────────────────────────────
static inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}
static inline double cpu_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ── Stats ─────────────────────────────────────────────────────────────────────
struct Stats {
    double throughput_gbps;
    double avg_us, stddev_us, p50_us, p95_us, p99_us, p999_us;
    double wakeup_p50_us, wakeup_p99_us;
    uint64_t wakeups_triggered;
    double   cpu_us_per_msg;
};

static double percentile(std::vector<double>& v, double pct) {
    if (v.empty()) return 0.0;
    size_t idx = static_cast<size_t>(pct / 100.0 * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

static Stats compute_stats(Telemetry& tel, size_t msg_sz) {
    Stats s{};
    if (tel.latency_count == 0) return s;

    std::vector<double> lats(tel.latencies, tel.latencies + tel.latency_count);
    lats.erase(std::remove_if(lats.begin(), lats.end(),
                              [](double v){ return v < 0.0; }), lats.end());
    if (lats.empty()) return s;
    std::sort(lats.begin(), lats.end());

    double sum = 0.0;
    for (double v : lats) sum += v;
    s.avg_us = sum / lats.size();
    double var = 0.0;
    for (double v : lats) var += (v - s.avg_us) * (v - s.avg_us);
    s.stddev_us = std::sqrt(var / lats.size());
    s.p50_us    = percentile(lats, 50);
    s.p95_us    = percentile(lats, 95);
    s.p99_us    = percentile(lats, 99);
    s.p999_us   = percentile(lats, 99.9);

    // throughput
    size_t num_msgs = get_total_bytes(msg_sz) / msg_sz;
    s.throughput_gbps =
        (static_cast<double>(num_msgs) * msg_sz / (1024.0 * 1024.0 * 1024.0))
        / tel.execution_time_sec;

    // wakeup latency
    if (tel.wakeup_count > 0) {
        std::vector<double> wl(tel.wakeup_latencies,
                               tel.wakeup_latencies + tel.wakeup_count);
        std::sort(wl.begin(), wl.end());
        s.wakeup_p50_us = percentile(wl, 50);
        s.wakeup_p99_us = percentile(wl, 99);
    }

    s.wakeups_triggered = tel.wakeups_triggered;
    s.cpu_us_per_msg    = tel.cpu_time_sec * 1e6
                          / static_cast<double>(num_msgs);
    return s;
}

// ── Generic benchmark loop parameterised on variant ──────────────────────────
template<typename WV>
static Stats run_one(RingBuffer* rb, WakeupState& ws,
                     struct io_uring* ring,
                     size_t msg_sz, Telemetry& tel) {
    std::memset(&tel, 0, sizeof(tel));
    rb->tail.store(0, std::memory_order_relaxed);
    rb->head.store(0, std::memory_order_release);
    rb->consumer_sleeping.store(0, std::memory_order_relaxed);

    size_t   consumed  = 0;
    size_t   num_msgs  = get_total_bytes(msg_sz) / msg_sz;
    uint64_t lat_idx   = 0;
    uint64_t wl_idx    = 0;
    uint64_t wakeups   = 0;

    double cpu_start = cpu_time_sec();
    auto   wall_start = std::chrono::high_resolution_clock::now();

    while (consumed < get_total_bytes(msg_sz)) {
        uint64_t t = rb->tail.load(std::memory_order_relaxed);
        uint64_t h = rb->head.load(std::memory_order_acquire);

        if (t == h) {
            // ring empty — record wakeup publish time BEFORE sleeping
            uint64_t sleep_start = now_ns();
            WV::consumer_wait(rb, ws, ring, t);
            uint64_t woke_at = now_ns();

            // After waking, check if there really is data
            if (rb->head.load(std::memory_order_acquire) == t)
                continue; // spurious wake or still empty

            if (wl_idx < MAX_LAT_SAMPLES) {
                tel.wakeup_latencies[wl_idx++] =
                    static_cast<double>(woke_at - sleep_start) / 1000.0;
            }
            ++wakeups;
            continue;
        }

        uint64_t recv_ns = now_ns();
        auto& slot = rb->slots[t % NUM_SLOTS];

        // touch data (cache simulation)
        volatile char cksum = 0;
        for (size_t i = 0; i < slot.size; i += 64)
            cksum += slot.data[i];
        (void)cksum;

        if (lat_idx < MAX_LAT_SAMPLES) {
            tel.latencies[lat_idx++] =
                static_cast<double>(recv_ns - slot.send_ns) / 1000.0;
        }

        consumed += slot.size;
        rb->tail.store(t + 1, std::memory_order_release);
    }

    auto wall_end = std::chrono::high_resolution_clock::now();
    tel.execution_time_sec =
        std::chrono::duration<double>(wall_end - wall_start).count();
    tel.cpu_time_sec    = cpu_time_sec() - cpu_start;
    tel.latency_count   = lat_idx;
    tel.wakeup_count    = wl_idx;
    tel.wakeups_triggered = wakeups;
    (void)num_msgs;

    return compute_stats(tel, msg_sz);
}

// ── Runtime dispatch ──────────────────────────────────────────────────────────
static Stats dispatch(WakeupVariant v,
                      RingBuffer* rb, WakeupState& ws,
                      struct io_uring* ring,
                      size_t msg_sz, Telemetry& tel) {
    switch (v) {
    case BUSY_POLL:    return run_one<wakeup::BusyPoll>   (rb, ws, ring, msg_sz, tel);
    case SPIN_BACKOFF: return run_one<wakeup::SpinBackoff> (rb, ws, ring, msg_sz, tel);
    case ADAPTIVE:     return run_one<wakeup::Adaptive>    (rb, ws, ring, msg_sz, tel);
    case FUTEX:        return run_one<wakeup::FutexWakeup> (rb, ws, ring, msg_sz, tel);
    case EVENTFD:      return run_one<wakeup::EventFD>     (rb, ws, ring, msg_sz, tel);
    case IO_URING:     return run_one<wakeup::IoUring>     (rb, ws, ring, msg_sz, tel);
    }
    return {};
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <variant_int> <output.csv>\n";
        return 1;
    }

    WakeupVariant variant = static_cast<WakeupVariant>(std::atoi(argv[1]));
    const char*   csv_path = argv[2];

    if (variant < 0 || variant >= NUM_VARIANTS) {
        std::cerr << "variant must be 0-" << (NUM_VARIANTS-1) << "\n";
        return 1;
    }

    set_affinity(CONSUMER_CORE);

    // ── Shared memory ────────────────────────────────────────────────────────
    shm_unlink(SHM_RING_NAME);
    int shm_fd = shm_open(SHM_RING_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { std::perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, sizeof(RingBuffer)) < 0) {
        std::perror("ftruncate"); return 1;
    }
    auto* rb = static_cast<RingBuffer*>(
        mmap(nullptr, sizeof(RingBuffer),
             PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (rb == MAP_FAILED) { std::perror("mmap"); return 1; }

    // ── WakeupState setup ────────────────────────────────────────────────────
    WakeupState ws;
    struct io_uring ring{};

    if (variant == EVENTFD) {
        ws.eventfd_fd = eventfd(0, EFD_SEMAPHORE);
        if (ws.eventfd_fd < 0) { std::perror("eventfd"); return 1; }
        // Signal eventfd fd to producer via a well-known /dev/shm file
        char efd_path[64];
        snprintf(efd_path, sizeof(efd_path), "/dev/shm/ablation_efd_%d",
                 static_cast<int>(variant));
        // Write the eventfd number so producer can open /proc/self/fd via
        // the shared fd approach — we use a sync file instead.
        // Simpler: we pass via a second shm integer.
        // Easiest approach for cross-process eventfd: use a Unix socket pair
        // or just have producer call eventfd again with same semantics and
        // use a FIFO signal — but that defeats the point.
        // PRACTICAL SOLUTION: producer opens /proc/<consumer_pid>/fd/<efd_fd>
        // which requires root, OR we use a socketpair in a wrapper script.
        // For portability we use a FIFO for efd handoff:
        char handoff_path[64];
        snprintf(handoff_path, sizeof(handoff_path),
                 "/tmp/ablation_efd_handoff");
        mkfifo(handoff_path, 0666);
        // Write our pid+fd so producer can reconstruct
        int hfd = open(handoff_path, O_WRONLY);
        if (hfd < 0) { std::perror("handoff open (w)"); return 1; }
        // We send the fd number via a short message; producer will re-use the
        // SAME eventfd by inheriting via /proc/PID/fd symlink.
        // Better: just pass the fd via a Unix domain socket.
        // SIMPLEST correct approach: create the eventfd BEFORE forking in a
        // shell script. Here we fall back to a named-pipe + dup trick:
        // We write the eventfd fd num as text; producer reads our PID from
        // /tmp/ablation_consumer_pid, then opens /proc/<pid>/fd/<efd_fd>.
        {
            char pid_file[] = "/tmp/ablation_consumer_pid";
            FILE* pf = fopen(pid_file, "w");
            if (pf) {
                fprintf(pf, "%d %d\n", getpid(), ws.eventfd_fd);
                fclose(pf);
            }
        }
        // Write a byte to unblock producer's open() on handoff FIFO
        char dummy = 'R';
        if (write(hfd, &dummy, 1) < 0) { /* ignore */ }
        close(hfd);
    } else if (variant == IO_URING) {
        io_uring_queue_init(256, &ring, 0);
        mkfifo(SIGNAL_PATH, 0666);
        ws.fifo_fd = open(SIGNAL_PATH, O_RDWR);
        if (ws.fifo_fd < 0) { std::perror("open fifo"); return 1; }
    }

    // ── CSV output ───────────────────────────────────────────────────────────
    std::ofstream csv(csv_path);
    csv << CSV_HEADER;

    auto* tel = new Telemetry{};

    std::cout << "[Consumer:" << VARIANT_NAMES[variant] << "] Ready\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "\n=== " << VARIANT_NAMES[variant]
                  << " | size=" << sz << " ===\n";

        for (int run = 0; run <= NUM_RUNS; ++run) {
            Stats s = dispatch(variant, rb, ws, &ring, sz, *tel);

            if (run == 0) {
                std::cout << "  [warmup] tput=" << s.throughput_gbps << " GB/s\n";
            } else {
                std::cout << "  run " << run
                          << " tput=" << s.throughput_gbps
                          << " GB/s  p50=" << s.p50_us << " µs"
                          << "  wakeups=" << s.wakeups_triggered << "\n";
                // wakeup_variant,regime,message_size_bytes,run,...
                // regime is written by the orchestrator (run_ablation.sh merges)
                // here we embed the variant name only; regime comes from producer args
                csv << VARIANT_NAMES[variant] << ",unknown," << sz << "," << run
                    << "," << s.throughput_gbps
                    << "," << s.avg_us
                    << "," << s.stddev_us
                    << "," << s.p50_us
                    << "," << s.p95_us
                    << "," << s.p99_us
                    << "," << s.p999_us
                    << "," << s.wakeup_p50_us
                    << "," << s.wakeup_p99_us
                    << "," << s.wakeups_triggered
                    << "," << s.cpu_us_per_msg
                    << "\n";
            }
            usleep(2000);
        }
    }

    csv.close();
    delete tel;

    if (variant == EVENTFD) close(ws.eventfd_fd);
    if (variant == IO_URING) {
        io_uring_queue_exit(&ring);
        close(ws.fifo_fd);
        unlink(SIGNAL_PATH);
    }
    munmap(rb, sizeof(RingBuffer));
    close(shm_fd);
    shm_unlink(SHM_RING_NAME);

    std::cout << "[Consumer:" << VARIANT_NAMES[variant] << "] Done. CSV: "
              << csv_path << "\n";
    return 0;
}
