#include <liburing.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include "common.h"

#if defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#  define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#  define CPU_PAUSE() ((void)0)
#endif

static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

int main() {
    set_affinity(PRODUCER_CORE);
    std::cout << "[Producer] Initializing io_uring Ring Buffer Benchmark Loops...\n";

    // Establish access tracking window to memory-mapped POSIX node
    int shm_fd = shm_open(SHM_RING_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { std::perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, sizeof(RingBuffer)) < 0) { std::perror("ftruncate"); return 1; }

    auto* rb = static_cast<RingBuffer*>(mmap(nullptr, sizeof(RingBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (rb == MAP_FAILED) { std::perror("mmap"); return 1; }

    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags          = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
    params.sq_thread_cpu  = SQPOLL_CORE;
    params.sq_thread_idle = 100000; 

    struct io_uring ring;
    bool sqpoll_active = (io_uring_queue_init_params(256, &ring, &params) == 0);
    if (!sqpoll_active) {
        std::cerr << "[Producer] SQPOLL unavailable (CAP_SYS_ADMIN required), falling back to standard ring mode.\n";
        std::memset(&params, 0, sizeof(params));
        io_uring_queue_init_params(256, &ring, &params);
    }

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "[Producer] Processing Size Target: " << sz << " Bytes\n";
        std::vector<char> payload(sz, 'X');

        for (int run = 0; run <= NUM_RUNS; ++run) {
            // Symmetrically wait for the consumer to reset the memory index trackers
            while(rb->head.load(std::memory_order_relaxed) != 0 || rb->tail.load(std::memory_order_relaxed) != 0) {
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

                auto& slot = rb->slots[h % NUM_SLOTS];
                std::memcpy(slot.data, payload.data(), sz);
                slot.size = static_cast<uint32_t>(sz);
                slot.send_ns = now_ns();

                rb->head.store(h + 1, std::memory_order_release);

                struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
                if (sqe) {
                    io_uring_prep_nop(sqe);
                    if (!sqpoll_active) io_uring_submit(&ring);

                    struct io_uring_cqe* cqe;
                    if (io_uring_peek_cqe(&ring, &cqe) == 0) {
                        io_uring_cqe_seen(&ring, cqe);
                    }
                }
                produced += sz;
            }
            usleep(2000); 
        }
    }

    io_uring_queue_exit(&ring);
    munmap(rb, sizeof(RingBuffer));
    close(shm_fd);
    std::cout << "[Producer] io_uring Production complete.\n";
    return 0;
}
