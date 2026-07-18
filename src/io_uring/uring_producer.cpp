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
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <fstream>
#include <atomic>
#include <random>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

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
  struct timespec ts;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

struct LatencyStats {
  double avg;
  double stddev;
  double p50;
  double p90;
  double p95;
  double p99;
  double p999;
};

static LatencyStats compute_latency_stats(std::vector<double>& lats) {
  LatencyStats s{};
  if (lats.empty()) return s;
  std::sort(lats.begin(), lats.end());
  double sum = std::accumulate(lats.begin(), lats.end(), 0.0);
  s.avg = sum / lats.size();
  
  double var = 0.0;
  for (double v : lats) var += (v - s.avg) * (v - s.avg);
  s.stddev = std::sqrt(var / lats.size());
  
  s.p50 = lats[lats.size() * 50 / 100];
  s.p90 = lats[lats.size() * 90 / 100];
  s.p95 = lats[lats.size() * 95 / 100];
  s.p99 = lats[lats.size() * 99 / 100];
  s.p999 = lats[lats.size() * 999 / 1000];
  return s;
}

static int recv_fd(int socket) {
  struct msghdr msg = {0};
  char buf[CMSG_SPACE(sizeof(int))];
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  struct iovec io;
  char dummy;
  io.iov_base = &dummy;
  io.iov_len = 1;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  if (recvmsg(socket, &msg, 0) < 0) {
    std::perror("recvmsg");
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
    return *((int *)CMSG_DATA(cmsg));
  }
  return -1;
}

void publish(RingBuffer& rb, uint64_t h, int sig_fd, int evfd, struct io_uring& ring, WakeupVariant wakeup) {
  rb.head.store(h + 1, std::memory_order_seq_cst);
  if (rb.consumer_sleeping.load(std::memory_order_seq_cst) == 1) {
    uint32_t expected = 1;
    if (rb.consumer_sleeping.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
      if (wakeup == WakeupVariant::FUTEX || wakeup == WakeupVariant::ADAPTIVE) {
        syscall(SYS_futex, &rb.consumer_sleeping, FUTEX_WAKE, 1, nullptr, nullptr, 0);
      } else if (wakeup == WakeupVariant::EVENTFD) {
        uint64_t val = 1;
        ssize_t w = write(evfd, &val, sizeof(val));
        (void)w;
      } else if (wakeup == WakeupVariant::URING) {
        char sig = 'W';
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (sqe) {
          io_uring_prep_write(sqe, sig_fd, &sig, 1, 0);
          io_uring_submit(&ring);
          struct io_uring_cqe *cqe;
          if (io_uring_wait_cqe(&ring, &cqe) == 0) {
            io_uring_cqe_seen(&ring, cqe);
          }
        }
      }
    }
  }
}

void wait_for_data(RingBuffer& rb, uint64_t t, int sig_fd, int evfd, struct io_uring& ring, WakeupVariant wakeup) {
  if (rb.head.load(std::memory_order_acquire) != t) return;

  if (wakeup == WakeupVariant::SPIN) {
    while (rb.head.load(std::memory_order_acquire) == t) {
      CPU_PAUSE();
    }
  } else if (wakeup == WakeupVariant::BACKOFF) {
    int backoff = 1;
    while (rb.head.load(std::memory_order_acquire) == t) {
      for (int i = 0; i < backoff; ++i) {
        CPU_PAUSE();
      }
      if (backoff < 1024) backoff *= 2;
      else sched_yield();
    }
  } else if (wakeup == WakeupVariant::ADAPTIVE) {
    bool found = false;
    for (int i = 0; i < 2000; ++i) {
      if (rb.head.load(std::memory_order_acquire) != t) {
        found = true;
        break;
      }
      CPU_PAUSE();
    }
    if (!found) {
      rb.consumer_sleeping.store(1, std::memory_order_seq_cst);
      if (rb.head.load(std::memory_order_seq_cst) == t) {
        syscall(SYS_futex, &rb.consumer_sleeping, FUTEX_WAIT, 1, nullptr, nullptr, 0);
      } else {
        rb.consumer_sleeping.store(0, std::memory_order_relaxed);
      }
    }
  } else if (wakeup == WakeupVariant::FUTEX) {
    rb.consumer_sleeping.store(1, std::memory_order_seq_cst);
    if (rb.head.load(std::memory_order_seq_cst) == t) {
      syscall(SYS_futex, &rb.consumer_sleeping, FUTEX_WAIT, 1, nullptr, nullptr, 0);
    } else {
      rb.consumer_sleeping.store(0, std::memory_order_relaxed);
    }
  } else if (wakeup == WakeupVariant::EVENTFD) {
    rb.consumer_sleeping.store(1, std::memory_order_seq_cst);
    if (rb.head.load(std::memory_order_seq_cst) == t) {
      uint64_t val;
      ssize_t r = read(evfd, &val, sizeof(val));
      (void)r;
    }
    rb.consumer_sleeping.store(0, std::memory_order_relaxed);
  } else if (wakeup == WakeupVariant::URING) {
    rb.consumer_sleeping.store(1, std::memory_order_seq_cst);
    if (rb.head.load(std::memory_order_seq_cst) == t) {
      struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
      if (sqe) {
        char sig_buf[8];
        io_uring_prep_read(sqe, sig_fd, sig_buf, 1, 0);
        io_uring_submit_and_wait(&ring, 1);
        struct io_uring_cqe *cqe;
        if (io_uring_peek_cqe(&ring, &cqe) == 0) {
          io_uring_cqe_seen(&ring, cqe);
        }
      }
    } else {
      rb.consumer_sleeping.store(0, std::memory_order_relaxed);
    }
  }
}

int main(int argc, char* argv[]) {
  set_affinity(PRODUCER_CORE);

  WakeupVariant wakeup = WakeupVariant::URING;
  ArrivalRegime regime = ArrivalRegime::SATURATED;
  std::string mode = "throughput";
  double offered_rate = 50000.0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      mode = argv[i + 1];
    } else if (std::strcmp(argv[i], "--wakeup") == 0 && i + 1 < argc) {
      std::string w = argv[i + 1];
      if (w == "spin") wakeup = WakeupVariant::SPIN;
      else if (w == "backoff") wakeup = WakeupVariant::BACKOFF;
      else if (w == "adaptive") wakeup = WakeupVariant::ADAPTIVE;
      else if (w == "futex") wakeup = WakeupVariant::FUTEX;
      else if (w == "eventfd") wakeup = WakeupVariant::EVENTFD;
      else if (w == "uring") wakeup = WakeupVariant::URING;
    } else if (std::strcmp(argv[i], "--regime") == 0 && i + 1 < argc) {
      std::string r = argv[i + 1];
      if (r == "saturated") regime = ArrivalRegime::SATURATED;
      else if (r == "bursty") regime = ArrivalRegime::BURSTY;
      else if (r == "offered") regime = ArrivalRegime::OFFERED;
    } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
      offered_rate = std::stod(argv[i + 1]);
    }
  }

  std::cout << "[Producer] io_uring IPC SPSC Ring Buffer Producer running.\n"
            << "Mode: " << mode << " | Wakeup: " << static_cast<int>(wakeup) 
            << " | Regime: " << static_cast<int>(regime) << "\n";

  int shm_fd = shm_open(SHM_RING_NAME, O_RDWR, 0666);
  if (shm_fd < 0) {
    std::perror("shm_open");
    return 1;
  }

  auto *layout = static_cast<SharedMemoryLayout *>(mmap(nullptr, sizeof(SharedMemoryLayout),
                                             PROT_READ | PROT_WRITE, MAP_SHARED,
                                             shm_fd, 0));
  if (layout == MAP_FAILED) {
    std::perror("mmap");
    return 1;
  }

  // Set up signaling based on variant
  int sig_fd_a_to_b = -1;
  int sig_fd_b_to_a = -1;
  int evfd_a_to_b = -1;
  int evfd_b_to_a = -1;
  struct io_uring ring;

  if (wakeup == WakeupVariant::URING) {
    if (io_uring_queue_init(64, &ring, 0) < 0) {
      std::perror("io_uring_queue_init");
      return 1;
    }
    sig_fd_a_to_b = open(SIGNAL_PATH_A_TO_B, O_WRONLY);
    if (sig_fd_a_to_b < 0) {
      std::perror("open signal path A to B");
      return 1;
    }
    if (mode == "latency") {
      sig_fd_b_to_a = open(SIGNAL_PATH_B_TO_A, O_RDONLY);
      if (sig_fd_b_to_a < 0) {
        std::perror("open signal path B to A");
        return 1;
      }
    }
  } else if (wakeup == WakeupVariant::EVENTFD) {
    int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, EVENTFD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    while (connect(client_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      usleep(500);
    }
    evfd_a_to_b = recv_fd(client_sock);
    if (mode == "latency") {
      evfd_b_to_a = recv_fd(client_sock);
    }
    close(client_sock);
  }

  std::mt19937 gen(1337);
  std::exponential_distribution<double> dist(offered_rate);

  if (mode == "latency") {
    // Latency Mode (Ping-Pong round trip timed on Client A)
    std::string csv_name = "uring_";
    if (wakeup == WakeupVariant::SPIN) csv_name += "spin";
    else if (wakeup == WakeupVariant::BACKOFF) csv_name += "backoff";
    else if (wakeup == WakeupVariant::ADAPTIVE) csv_name += "adaptive";
    else if (wakeup == WakeupVariant::FUTEX) csv_name += "futex";
    else if (wakeup == WakeupVariant::EVENTFD) csv_name += "eventfd";
    else if (wakeup == WakeupVariant::URING) csv_name += "uring";
    csv_name += "_latency.csv";

    std::ofstream csv(csv_name);
    csv << "message_size_bytes,run,avg_latency_us,stddev_us,p50_us,p90_us,p95_us,p99_us,p999_us\n";

    for (size_t sz : MESSAGE_SIZES) {
      std::cout << "[Producer] Sweep sizing: " << sz << " Bytes\n";

      for (int run = 0; run <= NUM_RUNS; ++run) {
        // Critical run startup boundary
        while (layout->ring_a_to_b.head.load(std::memory_order_relaxed) != 0 ||
               layout->ring_a_to_b.tail.load(std::memory_order_relaxed) != 0 ||
               layout->ring_b_to_a.head.load(std::memory_order_relaxed) != 0 ||
               layout->ring_b_to_a.tail.load(std::memory_order_relaxed) != 0) {
          CPU_PAUSE();
        }

        size_t trips = (run == 0) ? 1000 : 100000;
        std::vector<double> run_latencies;
        run_latencies.reserve(trips);

        for (size_t trip = 0; trip < trips; ++trip) {
          uint64_t start_ns = now_ns();

          // 1. Send to Server B (ring_a_to_b)
          uint64_t h = layout->ring_a_to_b.head.load(std::memory_order_relaxed);
          while ((h - layout->ring_a_to_b.tail.load(std::memory_order_acquire)) >= NUM_SLOTS) {
            CPU_PAUSE();
          }

          auto& slot = layout->ring_a_to_b.slots[h % NUM_SLOTS];
          slot.send_ns = start_ns;
          slot.size = static_cast<uint32_t>(sz);
          // (Data payload touch isn't strictly necessary on send, but we set it)
          slot.data[0] = 'X';

          publish(layout->ring_a_to_b, h, sig_fd_a_to_b, evfd_a_to_b, ring, wakeup);

          // 2. Read Echo back from Server B (ring_b_to_a)
          uint64_t t = layout->ring_b_to_a.tail.load(std::memory_order_relaxed);
          wait_for_data(layout->ring_b_to_a, t, sig_fd_b_to_a, evfd_b_to_a, ring, wakeup);

          auto& echo_slot = layout->ring_b_to_a.slots[t % NUM_SLOTS];
          
          // Touch echo data
          volatile char checksum = 0;
          for (size_t i = 0; i < echo_slot.size; i += 64) {
            checksum += echo_slot.data[i];
          }
          (void)checksum;

          uint64_t end_ns = now_ns();
          double rtt_us = static_cast<double>(end_ns - start_ns) / 1000.0;
          run_latencies.push_back(rtt_us / 2.0);

          layout->ring_b_to_a.tail.store(t + 1, std::memory_order_release);
        }

        if (run == 0) {
          std::cout << "  [Warmup] Completed\n";
        } else {
          LatencyStats s = compute_latency_stats(run_latencies);
          std::cout << "  Run " << run << " -> Median: " << s.p50 << " us | P99: " << s.p99 << " us\n";
          csv << sz << "," << run << "," << s.avg << "," << s.stddev << "," 
              << s.p50 << "," << s.p90 << "," << s.p95 << "," << s.p99 << "," << s.p999 << "\n";
        }
        usleep(2000);
      }
    }
    csv.close();
  } else {
    // Throughput Mode
    std::string csv_suffix = "";
    if (regime == ArrivalRegime::BURSTY) csv_suffix = "_bursty";
    else if (regime == ArrivalRegime::OFFERED) csv_suffix = "_offered";

    std::string csv_name = "uring_";
    if (wakeup == WakeupVariant::SPIN) csv_name += "spin";
    else if (wakeup == WakeupVariant::BACKOFF) csv_name += "backoff";
    else if (wakeup == WakeupVariant::ADAPTIVE) csv_name += "adaptive";
    else if (wakeup == WakeupVariant::FUTEX) csv_name += "futex";
    else if (wakeup == WakeupVariant::EVENTFD) csv_name += "eventfd";
    else if (wakeup == WakeupVariant::URING) csv_name += "uring";
    csv_name += csv_suffix + "_throughput.csv";

    // Wait, the consumer will calculate throughput. But wait!
    // Since we sweep through enums, it's easiest if consumer calculates and writes CSV.
    // The producer just acts as the data generator.
    for (size_t sz : MESSAGE_SIZES) {
      std::cout << "[Producer] Saturated Sizing: " << sz << " Bytes\n";

      for (int run = 0; run <= NUM_RUNS; ++run) {
        while (layout->ring_a_to_b.head.load(std::memory_order_relaxed) != 0 ||
               layout->ring_a_to_b.tail.load(std::memory_order_relaxed) != 0) {
          CPU_PAUSE();
        }

        size_t produced = 0;
        while (produced < TOTAL_BYTES) {
          // Offered load rate limiter
          if (regime == ArrivalRegime::OFFERED) {
            double delay = dist(gen);
            uint64_t next_send = now_ns() + static_cast<uint64_t>(delay * 1e9);
            while (now_ns() < next_send) {
              CPU_PAUSE();
            }
          } else if (regime == ArrivalRegime::BURSTY) {
            // sleep 1ms between messages to let ring empty
            usleep(1000);
          }

          uint64_t h = layout->ring_a_to_b.head.load(std::memory_order_relaxed);
          while ((h - layout->ring_a_to_b.tail.load(std::memory_order_acquire)) >= NUM_SLOTS) {
            CPU_PAUSE();
          }

          auto& slot = layout->ring_a_to_b.slots[h % NUM_SLOTS];
          slot.send_ns = now_ns();
          slot.size = static_cast<uint32_t>(sz);
          slot.data[0] = 'X';

          publish(layout->ring_a_to_b, h, sig_fd_a_to_b, evfd_a_to_b, ring, wakeup);
          produced += sz;
        }
        usleep(2000);
      }
    }
  }

  // Cleanup
  if (wakeup == WakeupVariant::URING) {
    io_uring_queue_exit(&ring);
    close(sig_fd_a_to_b);
    if (sig_fd_b_to_a >= 0) close(sig_fd_b_to_a);
  } else if (wakeup == WakeupVariant::EVENTFD) {
    close(evfd_a_to_b);
    if (evfd_b_to_a >= 0) close(evfd_b_to_a);
  }

  munmap(layout, sizeof(SharedMemoryLayout));
  close(shm_fd);
  std::cout << "[Producer] Exit successfully.\n";
  return 0;
}
