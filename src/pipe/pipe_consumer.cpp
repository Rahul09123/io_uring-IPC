#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <memory>
#include <string>
#include <atomic>
#include "common.h"

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

static bool read_all(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static bool write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

static inline size_t get_total_bytes(size_t msg_sz) {
    if (msg_sz <= 1024)     return 32ULL * 1024 * 1024;  
    if (msg_sz <= 65536)    return 256ULL * 1024 * 1024; 
    return 2ULL * 1024 * 1024 * 1024;                    
}

struct Stats { double avg, stddev, p50, p95, p99, throughput_gbps; };

static Stats compute_throughput_stats(Telemetry* tel, size_t msg_sz) {
    Stats s{};
    s.throughput_gbps = (get_total_bytes(msg_sz) / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
    return s;
}

int main(int argc, char* argv[]) {
    set_affinity(CONSUMER_CORE);

    std::string mode = "throughput";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[i + 1];
        }
    }

    std::cout << "[Consumer] Initializing Pipe Consumer (Mode: " << mode << ")...\n";

    if (mode == "latency") {
        std::vector<char> buf(sizeof(MessageHeader) + MAX_PAYLOAD);

        for (size_t sz : MESSAGE_SIZES) {
            std::string fifo_a_to_b = std::string(PIPE_FIFO_PATH_A_TO_B) + "_" + std::to_string(sz);
            std::string fifo_b_to_a = std::string(PIPE_FIFO_PATH_B_TO_A) + "_" + std::to_string(sz);

            unlink(fifo_a_to_b.c_str());
            unlink(fifo_b_to_a.c_str());

            if (mkfifo(fifo_a_to_b.c_str(), 0666) == -1) { std::perror("mkfifo a_to_b"); return 1; }
            if (mkfifo(fifo_b_to_a.c_str(), 0666) == -1) { std::perror("mkfifo b_to_a"); return 1; }

            int read_fd = open(fifo_a_to_b.c_str(), O_RDONLY);
            if (read_fd == -1) { std::perror("open a_to_b"); return 1; }

            int write_fd = open(fifo_b_to_a.c_str(), O_WRONLY);
            if (write_fd == -1) { std::perror("open b_to_a"); return 1; }
            fcntl(write_fd, F_SETPIPE_SZ, static_cast<int>(MAX_PAYLOAD));

            for (int run = 0; run <= NUM_RUNS; ++run) {
                size_t num_trips = (run == 0) ? 1000 : 100000;
                for (size_t trip = 0; trip < num_trips; ++trip) {
                    if (!read_all(read_fd, buf.data(), sizeof(MessageHeader) + sz)) break;
                    
                    // Touch payload for checksum
                    volatile char checksum = 0;
                    char* payload = buf.data() + sizeof(MessageHeader);
                    for (size_t i = 0; i < sz; i += 64) checksum += payload[i];
                    (void)checksum;

                    if (!write_all(write_fd, buf.data(), sizeof(MessageHeader) + sz)) break;
                }
            }
            close(read_fd);
            close(write_fd);
            unlink(fifo_a_to_b.c_str());
            unlink(fifo_b_to_a.c_str());
        }
    } else {
        // Throughput Mode
        shm_unlink(SHM_TEL_NAME);
        int shm_fd = shm_open(SHM_TEL_NAME, O_CREAT | O_RDWR, 0666);
        if (shm_fd < 0) { std::perror("shm_open telemetry failed"); return 1; }
        if (ftruncate(shm_fd, sizeof(Telemetry)) < 0) { return 1; }

        auto* tel = static_cast<Telemetry*>(mmap(nullptr, sizeof(Telemetry), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
        if (tel == MAP_FAILED) { return 1; }

        std::ofstream csv("pipe_throughput.csv");
        csv << "message_size_bytes,run,throughput_gbps\n";

        std::vector<char> payload_buf(MAX_PAYLOAD);
        std::cout << "[Consumer] Pipe listener engine active...\n";

        for (size_t sz : MESSAGE_SIZES) {
            std::string dynamic_fifo = std::string(PIPE_FIFO_PATH) + "_" + std::to_string(sz);
            size_t current_total_target = get_total_bytes(sz);

            unlink(dynamic_fifo.c_str());
            if (mkfifo(dynamic_fifo.c_str(), 0666) == -1) { std::perror("mkfifo failed"); return 1; }

            int fd = open(dynamic_fifo.c_str(), O_RDONLY);
            if (fd == -1) { std::perror("open for read failed"); return 1; }

            for (int run = 0; run <= NUM_RUNS; ++run) {
                std::memset(tel, 0, sizeof(Telemetry));
                size_t consumed = 0;
                MessageHeader hdr;

                auto wall_start = std::chrono::high_resolution_clock::now();
                while (consumed < current_total_target) {
                    if (!read_all(fd, &hdr, sizeof(MessageHeader))) break;
                    if (!read_all(fd, payload_buf.data(), hdr.payload_size)) break;

                    volatile char checksum = 0;
                    for (size_t i = 0; i < hdr.payload_size; i += 64) checksum += payload_buf[i];
                    (void)checksum;

                    consumed += hdr.payload_size;
                }
                auto wall_end = std::chrono::high_resolution_clock::now();
                tel->execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();

                Stats s = compute_throughput_stats(tel, sz);
                if (run == 0) {
                    std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
                } else {
                    std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s\n";
                    csv << sz << "," << run << "," << s.throughput_gbps << "\n";
                }
            }
            close(fd);
            unlink(dynamic_fifo.c_str());
        }
        csv.close();
        munmap(tel, sizeof(Telemetry));
        close(shm_fd);
        shm_unlink(SHM_TEL_NAME);
    }
    return 0;
}
