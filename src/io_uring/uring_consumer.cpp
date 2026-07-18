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
#include <string>
#include <atomic>
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

static void send_fd(int socket, int fd) {
  struct msghdr msg = {0};
  char buf[CMSG_SPACE(sizeof(int))] = {0};
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  *((int *)CMSG_DATA(cmsg)) = fd;

  struct iovec io;
  char dummy = 'F';
  io.iov_base = &dummy;
  io.iov_len = 1;
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  if (sendmsg(socket, &msg, 0) < 0) {
    std::perror("sendmsg");
  }
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
  set_affinity(CONSUMER_CORE);

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

  std::cout << "[Consumer] io_uring IPC SPSC Ring Buffer Consumer running.\n"
            << "Mode: " << mode << " | Wakeup: " << static_cast<int>(wakeup) 
            << " | Regime: " << static_cast<int>(regime) << "\n";

  shm_unlink(SHM_RING_NAME);
  int shm_fd = shm_open(SHM_RING_NAME, O_CREAT | O_RDWR, 0666);
  if (shm_fd < 0) {
    std::perror("shm_open");
    return 1;
  }
  if (ftruncate(shm_fd, sizeof(SharedMemoryLayout)) < 0) {
    std::perror("ftruncate");
    return 1;
  }

  auto *layout = static_cast<SharedMemoryLayout *>(mmap(nullptr, sizeof(SharedMemoryLayout),
                                             PROT_READ | PROT_WRITE, MAP_SHARED,
                                             shm_fd, 0));
  if (layout == MAP_FAILED) {
    std::perror("mmap");
    return 1;
  }

  // Set up signaling paths
  int sig_fd_a_to_b = -1;
  int sig_fd_b_to_a = -1;
  int evfd_a_to_b = -1;
  int evfd_b_to_a = -1;
  struct io_uring ring;

  if (wakeup == WakeupVariant::URING) {
    if (io_uring_queue_init(256, &ring, 0) < 0) {
      std::perror("io_uring_queue_init");
      return 1;
    }
    mkfifo(SIGNAL_PATH_A_TO_B, 0666);
    sig_fd_a_to_b = open(SIGNAL_PATH_A_TO_B, O_RDWR);
    if (sig_fd_a_to_b < 0) {
      std::perror("open signal path A to B");
      return 1;
    }
    if (mode == "latency") {
      mkfifo(SIGNAL_PATH_B_TO_A, 0666);
      sig_fd_b_to_a = open(SIGNAL_PATH_B_TO_A, O_RDWR);
      if (sig_fd_b_to_a < 0) {
        std::perror("open signal path B to A");
        return 1;
      }
    }
  } else if (wakeup == WakeupVariant::EVENTFD) {
    evfd_a_to_b = eventfd(0, 0);
    if (mode == "latency") {
      evfd_b_to_a = eventfd(0, 0);
    }
    int listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, EVENTFD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(EVENTFD_SOCKET_PATH);
    bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_sock, 1);
    int conn_sock = accept(listen_sock, nullptr, nullptr);
    send_fd(conn_sock, evfd_a_to_b);
    if (mode == "latency") {
      send_fd(conn_sock, evfd_b_to_a);
    }
    close(conn_sock);
    close(listen_sock);
    unlink(EVENTFD_SOCKET_PATH);
  }

  if (mode == "latency") {
    // Latency Mode (Echo Server)
    for (size_t sz : MESSAGE_SIZES) {
      for (int run = 0; run <= NUM_RUNS; ++run) {
        layout->ring_a_to_b.tail.store(0, std::memory_order_relaxed);
        layout->ring_a_to_b.head.store(0, std::memory_order_release);
        layout->ring_a_to_b.consumer_sleeping.store(0, std::memory_order_relaxed);

        layout->ring_b_to_a.tail.store(0, std::memory_order_relaxed);
        layout->ring_b_to_a.head.store(0, std::memory_order_release);
        layout->ring_b_to_a.consumer_sleeping.store(0, std::memory_order_relaxed);

        size_t trips = (run == 0) ? 1000 : 100000;
        for (size_t trip = 0; trip < trips; ++trip) {
          // 1. Read from Client A (ring_a_to_b)
          uint64_t t = layout->ring_a_to_b.tail.load(std::memory_order_relaxed);
          wait_for_data(layout->ring_a_to_b, t, sig_fd_a_to_b, evfd_a_to_b, ring, wakeup);

          auto& slot = layout->ring_a_to_b.slots[t % NUM_SLOTS];

          // Touch received data & build echo checksum
          volatile char checksum = 0;
          for (size_t i = 0; i < slot.size; i += 64) {
            checksum += slot.data[i];
          }
          (void)checksum;

          // 2. Echo back to Client A (ring_b_to_a)
          uint64_t h = layout->ring_b_to_a.head.load(std::memory_order_relaxed);
          while ((h - layout->ring_b_to_a.tail.load(std::memory_order_acquire)) >= NUM_SLOTS) {
            CPU_PAUSE();
          }

          auto& echo_slot = layout->ring_b_to_a.slots[h % NUM_SLOTS];
          echo_slot.send_ns = slot.send_ns;
          echo_slot.size = slot.size;
          echo_slot.data[0] = 'Y'; // modification

          publish(layout->ring_b_to_a, h, sig_fd_b_to_a, evfd_b_to_a, ring, wakeup);

          layout->ring_a_to_b.tail.store(t + 1, std::memory_order_release);
        }
      }
    }
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

    std::ofstream csv(csv_name);
    csv << "message_size_bytes,run,throughput_gbps\n";

    for (size_t sz : MESSAGE_SIZES) {
      for (int run = 0; run <= NUM_RUNS; ++run) {
        layout->ring_a_to_b.tail.store(0, std::memory_order_relaxed);
        layout->ring_a_to_b.head.store(0, std::memory_order_release);
        layout->ring_a_to_b.consumer_sleeping.store(0, std::memory_order_relaxed);

        size_t consumed = 0;
        auto wall_start = std::chrono::high_resolution_clock::now();

        while (consumed < TOTAL_BYTES) {
          uint64_t t = layout->ring_a_to_b.tail.load(std::memory_order_relaxed);
          wait_for_data(layout->ring_a_to_b, t, sig_fd_a_to_b, evfd_a_to_b, ring, wakeup);

          auto& slot = layout->ring_a_to_b.slots[t % NUM_SLOTS];
          volatile char checksum = 0;
          for (size_t i = 0; i < slot.size; i += 64) {
            checksum += slot.data[i];
          }
          (void)checksum;

          consumed += slot.size;
          layout->ring_a_to_b.tail.store(t + 1, std::memory_order_release);
        }

        auto wall_end = std::chrono::high_resolution_clock::now();
        double execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();
        double throughput_gbps = (TOTAL_BYTES / (1024.0 * 1024.0 * 1024.0)) / execution_time_sec;

        if (run == 0) {
          std::cout << "  [Warmup]  throughput=" << throughput_gbps << " GB/s\n";
        } else {
          std::cout << "  Run " << run << " -> Throughput: " << throughput_gbps << " GB/s\n";
          csv << sz << "," << run << "," << throughput_gbps << "\n";
        }
        usleep(1000);
      }
    }
    csv.close();
  }

  // Cleanup
  if (wakeup == WakeupVariant::URING) {
    io_uring_queue_exit(&ring);
    close(sig_fd_a_to_b);
    if (sig_fd_b_to_a >= 0) close(sig_fd_b_to_a);
    unlink(SIGNAL_PATH_A_TO_B);
    if (mode == "latency") {
      unlink(SIGNAL_PATH_B_TO_A);
    }
  } else if (wakeup == WakeupVariant::EVENTFD) {
    close(evfd_a_to_b);
    if (evfd_b_to_a >= 0) close(evfd_b_to_a);
  }

  munmap(layout, sizeof(SharedMemoryLayout));
  close(shm_fd);
  shm_unlink(SHM_RING_NAME);
  std::cout << "[Consumer] Exit successfully.\n";
  return 0;
}
