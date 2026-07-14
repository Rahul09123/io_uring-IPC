#include "common.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <liburing.h>
#include <memory>
#include <numeric>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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

struct Stats {
  double avg, stddev, p50, p95, p99, throughput_gbps;
};

static Stats compute_stats(Telemetry *tel, size_t msg_sz) {
  Stats s{};
  if (tel->latency_count == 0)
    return s;
  std::vector<double> lats(tel->latencies, tel->latencies + tel->latency_count);
  lats.erase(std::remove_if(lats.begin(), lats.end(),
                            [](double v) { return v < 0.0; }),
             lats.end());
  if (lats.empty())
    return s;

  std::sort(lats.begin(), lats.end());
  double sum = std::accumulate(lats.begin(), lats.end(), 0.0);
  s.avg = sum / lats.size();

  double var = 0.0;
  for (double v : lats)
    var += (v - s.avg) * (v - s.avg);
  s.stddev = std::sqrt(var / lats.size());

  s.p50 = lats[lats.size() * 50 / 100];
  s.p95 = lats[lats.size() * 95 / 100];
  s.p99 = lats[lats.size() * 99 / 100];
  s.throughput_gbps =
      (TOTAL_BYTES / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
  return s;
}

int main() {
  set_affinity(CONSUMER_CORE);

  struct io_uring ring;
  io_uring_queue_init(256, &ring, 0);

  mkfifo(SIGNAL_PATH, 0666);
  int sig_fd = open(SIGNAL_PATH, O_RDWR);
  if (sig_fd < 0) {
    std::perror("open signal fifo");
    return 1;
  }

  std::ofstream csv("io_uring_results.csv");
  csv << "message_size_bytes,run,throughput_gbps,avg_latency_us,stddev_us,p50_"
         "us,p95_us,p99_us\n";

  shm_unlink(SHM_RING_NAME);
  int shm_fd = shm_open(SHM_RING_NAME, O_CREAT | O_RDWR, 0666);
  if (ftruncate(shm_fd, sizeof(RingBuffer)) < 0) {
    std::perror("ftruncate");
    return 1;
  }

  auto *rb = static_cast<RingBuffer *>(mmap(nullptr, sizeof(RingBuffer),
                                            PROT_READ | PROT_WRITE, MAP_SHARED,
                                            shm_fd, 0));

  auto tel = std::make_unique<Telemetry>();
  std::cout << "[Consumer] Sync established. Awaiting io_uring signals...\n";

  for (size_t sz : MESSAGE_SIZES) {
    std::cout << "\n=========================================\n";
    std::cout << "Testing Message Size: " << sz << " Bytes\n";
    std::cout << "=========================================\n";

    for (int run = 0; run <= NUM_RUNS; ++run) {
      std::memset(tel.get(), 0, sizeof(Telemetry));
      rb->tail.store(0, std::memory_order_relaxed);
      rb->head.store(0, std::memory_order_release);
      rb->consumer_sleeping.store(0, std::memory_order_relaxed);

      size_t consumed = 0;
      uint64_t lat_idx = 0;
      char sig_buf[8];

      auto wall_start = std::chrono::high_resolution_clock::now();
      while (consumed < TOTAL_BYTES) {
        uint64_t t = rb->tail.load(std::memory_order_relaxed);
        uint64_t h = rb->head.load(std::memory_order_acquire);

        if (t == h) {
          rb->consumer_sleeping.store(1, std::memory_order_release);

          // Double check to prevent race condition if data arrived instantly
          if (rb->head.load(std::memory_order_acquire) != t) {
            uint32_t expected = 1;
            rb->consumer_sleeping.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel);
            continue;
          }

          // Use io_uring to sleep cleanly
          struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_read(sqe, sig_fd, sig_buf, 1, 0);
            struct io_uring_cqe *cqe;
            if (io_uring_submit_and_wait(&ring, 1) >= 0) {
              if (io_uring_peek_cqe(&ring, &cqe) == 0) {
                io_uring_cqe_seen(&ring, cqe);
              }
            }
          }
          continue;
        }

        uint64_t recv_ns = now_ns();
        auto &slot = rb->slots[t % NUM_SLOTS];

        volatile char checksum = 0;
        for (size_t i = 0; i < slot.size; i += 64)
          checksum += slot.data[i];
        (void)checksum;

        if (lat_idx < MAX_LAT_SAMPLES) {
          tel->latencies[lat_idx++] =
              static_cast<double>(recv_ns - slot.send_ns) / 1000.0;
        }

        consumed += slot.size;
        rb->tail.store(t + 1, std::memory_order_release);
      }
      auto wall_end = std::chrono::high_resolution_clock::now();
      tel->execution_time_sec =
          std::chrono::duration<double>(wall_end - wall_start).count();
      tel->latency_count = lat_idx;

      Stats s = compute_stats(tel.get(), sz);
      if (run == 0) {
        std::cout << "  [Warmup]  throughput=" << s.throughput_gbps
                  << " GB/s\n";
      } else {
        std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps
                  << " GB/s | p50: " << s.p50 << " us\n";
        csv << sz << "," << run << "," << s.throughput_gbps << "," << s.avg
            << "," << s.stddev << "," << s.p50 << "," << s.p95 << "," << s.p99
            << "\n";
      }
      usleep(1000);
    }
  }

  io_uring_queue_exit(&ring);
  csv.close();
  munmap(rb, sizeof(RingBuffer));
  close(shm_fd);
  close(sig_fd);
  shm_unlink(SHM_RING_NAME);
  unlink(SIGNAL_PATH);
  return 0;
}