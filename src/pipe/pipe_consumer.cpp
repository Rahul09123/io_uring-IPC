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
#include "common.h"

static void set_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

static inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
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

static inline size_t get_total_bytes(size_t msg_sz) {
    if (msg_sz <= 1024)     return 32ULL * 1024 * 1024;  
    if (msg_sz <= 65536)    return 256ULL * 1024 * 1024; 
    return 2ULL * 1024 * 1024 * 1024;                    
}

struct Stats { double avg, stddev, p50, p95, p99, throughput_gbps; };

static Stats compute_stats(Telemetry* tel, size_t msg_sz) {
    Stats s{};
    if (tel->latency_count == 0) return s;
    std::vector<double> lats(tel->latencies, tel->latencies + tel->latency_count);
    lats.erase(std::remove_if(lats.begin(), lats.end(), [](double v){ return v < 0.0; }), lats.end());
    if (lats.empty()) return s;
    
    std::sort(lats.begin(), lats.end());
    double sum = std::accumulate(lats.begin(), lats.end(), 0.0);
    s.avg = sum / lats.size();
    
    double var = 0.0;
    for (double v : lats) var += (v - s.avg) * (v - s.avg);
    s.stddev = std::sqrt(var / lats.size());
    
    s.p50 = lats[lats.size() * 50 / 100];
    s.p95 = lats[lats.size() * 95 / 100];
    s.p99 = lats[lats.size() * 99 / 100];
    s.throughput_gbps = (get_total_bytes(msg_sz) / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
    return s;
}

int main() {
    set_affinity(CONSUMER_CORE);

    shm_unlink(SHM_TEL_NAME);
    // Remove IORING_SETUP_SQ_AFF completely from the POSIX shm_open flags
    int shm_fd = shm_open(SHM_TEL_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { std::perror("shm_open telemetry failed"); return 1; }
    if (ftruncate(shm_fd, sizeof(Telemetry)) < 0) { return 1; }

    auto* tel = static_cast<Telemetry*>(mmap(nullptr, sizeof(Telemetry), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (tel == MAP_FAILED) { return 1; }

    std::ofstream csv("pipe_results.csv");
    csv << "message_size_bytes,run,throughput_gbps,avg_latency_us,stddev_us,p50_us,p95_us,p99_us\n";

    std::vector<char> payload_buf(MAX_PAYLOAD);
    std::cout << "[Consumer] Sync established. Pipe listener engine active...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "\n=========================================\n";
        std::cout << "Testing Message Size: " << sz << " Bytes\n";
        std::cout << "=========================================\n";

        std::string dynamic_fifo = std::string(PIPE_FIFO_PATH) + "_" + std::to_string(sz);
        size_t current_total_target = get_total_bytes(sz);

        unlink(dynamic_fifo.c_str());
        if (mkfifo(dynamic_fifo.c_str(), 0666) == -1) { std::perror("mkfifo failed"); return 1; }

        // FIXED: Open channel ONCE outside the run matrix loop block
        int fd = open(dynamic_fifo.c_str(), O_RDONLY);
        if (fd == -1) { std::perror("open for read failed"); return 1; }

        for (int run = 0; run <= NUM_RUNS; ++run) {
            std::memset(tel, 0, sizeof(Telemetry));
            size_t consumed = 0;
            uint64_t lat_idx = 0;
            MessageHeader hdr;

            auto wall_start = std::chrono::high_resolution_clock::now();
            while (consumed < current_total_target) {
                if (!read_all(fd, &hdr, sizeof(MessageHeader))) break;
                if (!read_all(fd, payload_buf.data(), hdr.payload_size)) break;

                uint64_t recv_ns = now_ns();

                volatile char checksum = 0;
                for (size_t i = 0; i < hdr.payload_size; i += 64) checksum += payload_buf[i];
                (void)checksum;

                if (lat_idx < MAX_LAT_SAMPLES) {
                    tel->latencies[lat_idx++] = static_cast<double>(recv_ns - hdr.send_ns) / 1000.0;
                }
                consumed += hdr.payload_size;
            }
            auto wall_end = std::chrono::high_resolution_clock::now();
            tel->execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();
            tel->latency_count = lat_idx;

            Stats s = compute_stats(tel, sz);
            if (run == 0) {
                std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
            } else {
                std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s | p50: " << s.p50 << " us\n";
                csv << sz << "," << run << "," << s.throughput_gbps << "," << s.avg << "," 
                    << s.stddev << "," << s.p50 << "," << s.p95 << "," << s.p99 << "\n";
            }
        }
        close(fd);
        unlink(dynamic_fifo.c_str());
    }
    csv.close();
    munmap(tel, sizeof(Telemetry));
    close(shm_fd);
    shm_unlink(SHM_TEL_NAME);
    return 0;
}
