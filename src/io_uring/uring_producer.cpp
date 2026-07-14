#include "common.h"
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <liburing.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#else
#define CPU_PAUSE() ((void)0)
#endif

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

int main() {
  set_affinity(PRODUCER_CORE);
  std::cout
      << "[Producer] Initializing io_uring Ring Buffer Benchmark Loops...\n";

  int shm_fd = shm_open(SHM_RING_NAME, O_CREAT | O_RDWR, 0666);
  if (shm_fd < 0) {
    std::perror("shm_open");
    return 1;
  }
  if (ftruncate(shm_fd, sizeof(RingBuffer)) < 0) {
    std::perror("ftruncate");
    return 1;
  }

  auto *rb = static_cast<RingBuffer *>(mmap(nullptr, sizeof(RingBuffer),
                                            PROT_READ | PROT_WRITE, MAP_SHARED,
                                            shm_fd, 0));
  if (rb == MAP_FAILED) {
    std::perror("mmap");
    return 1;
  }

  int sig_fd = open(SIGNAL_PATH, O_RDWR | O_NONBLOCK);
  if (sig_fd < 0) {
    std::perror("open signal fifo");
    return 1;
  }

  // --- NEW: Initialize io_uring for the Producer ---
  struct io_uring ring;
  if (io_uring_queue_init(64, &ring, 0) < 0) {
    std::perror("io_uring_queue_init");
    return 1;
  }

  for (size_t sz : MESSAGE_SIZES) {
    std::cout << "[Producer] Processing Size Target: " << sz << " Bytes\n";
    std::vector<char> payload(sz, 'X');

    for (int run = 0; run <= NUM_RUNS; ++run) {
      // Critical Run-Boundary Synchronization Barrier
      while (rb->head.load(std::memory_order_acquire) != 0 ||
             rb->tail.load(std::memory_order_acquire) != 0) {
        CPU_PAUSE();
      }

      size_t produced = 0;
      while (produced < TOTAL_BYTES) {
        uint64_t h = rb->head.load(std::memory_order_relaxed);
        uint64_t t = rb->tail.load(std::memory_order_acquire);

        if ((h - t) >= NUM_SLOTS) {
          CPU_PAUSE();
          continue;
        }

        auto &slot = rb->slots[h % NUM_SLOTS];
        std::memcpy(slot.data, payload.data(), sz);
        slot.size = static_cast<uint32_t>(sz);
        slot.send_ns = now_ns();

        // FIXED: Strict Sequential Consistency to prevent Store-Load reordering
        // deadlock
        rb->head.store(h + 1, std::memory_order_seq_cst);

        // Atomic Exchange + io_uring Wakeup
        if (rb->consumer_sleeping.load(std::memory_order_seq_cst) == 1) {
          uint32_t expected = 1;

          if (rb->consumer_sleeping.compare_exchange_strong(
                  expected, 0, std::memory_order_acq_rel)) {
            char sig = 'W';

            // --- NEW: Using io_uring instead of POSIX write() ---
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (sqe) {
              io_uring_prep_write(sqe, sig_fd, &sig, 1, 0);
              io_uring_submit(&ring);

              // Safely block and wait for completion.
              // This keeps `sig` alive on the stack and prevents the SQ from
              // overflowing!
              struct io_uring_cqe *cqe;
              if (io_uring_wait_cqe(&ring, &cqe) == 0) {
                io_uring_cqe_seen(&ring, cqe);
              }
            }
          }
        }

        produced += sz;
      }
      usleep(2000);
    }
  }

  io_uring_queue_exit(&ring); // Clean up the ring
  munmap(rb, sizeof(RingBuffer));
  close(shm_fd);
  close(sig_fd);
  std::cout << "[Producer] io_uring Production complete.\n";
  return 0;
}
