#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
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

struct Stats { double avg, stddev, p50, p95, p99, throughput_gbps; };

static Stats compute_stats(Telemetry* tel) {
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
    s.throughput_gbps = (TOTAL_BYTES / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
    return s;
}

int main() {
    set_affinity(CONSUMER_CORE);

    std::ofstream csv("socket_results.csv");
    csv << "message_size_bytes,run,throughput_gbps,avg_latency_us,stddev_us,p50_us,p95_us,p99_us\n";

    std::vector<char> payload_buf(MAX_PAYLOAD);
    auto tel = std::make_unique<Telemetry>();

    std::cout << "[Consumer] Sync established. Socket server listening...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "\n=========================================\n";
        std::cout << "Testing Message Size: " << sz << " Bytes\n";
        std::cout << "=========================================\n";

        std::string dynamic_path = std::string(SOCKET_PATH) + "_" + std::to_string(sz);

        for (int run = 0; run <= NUM_RUNS; ++run) {
            std::memset(tel.get(), 0, sizeof(Telemetry));

            int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (listen_fd < 0) { std::perror("socket"); return 1; }

            int buf_sz = static_cast<int>(MAX_PAYLOAD * 2);
            setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));

            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, dynamic_path.c_str(), sizeof(addr.sun_path) - 1);
            
            unlink(dynamic_path.c_str());
            if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                std::perror("bind"); return 1;
            }

            if (listen(listen_fd, 5) < 0) { std::perror("listen"); return 1; }

            // Accept connection hook from client producer
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) { std::perror("accept"); return 1; }

            size_t consumed = 0;
            uint64_t lat_idx = 0;
            MessageHeader hdr;

            auto wall_start = std::chrono::high_resolution_clock::now();
            while (consumed < TOTAL_BYTES) {
                if (!read_all(client_fd, &hdr, sizeof(MessageHeader))) break;
                if (!read_all(client_fd, payload_buf.data(), hdr.payload_size)) break;

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

            close(client_fd);
            close(listen_fd);
            unlink(dynamic_path.c_str());

            Stats s = compute_stats(tel.get());
            if (run == 0) {
                std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
            } else {
                std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s | p50: " << s.p50 << " us | p99: " << s.p99 << " us\n";
                csv << sz << "," << run << "," << s.throughput_gbps << "," << s.avg << "," 
                    << s.stddev << "," << s.p50 << "," << s.p95 << "," << s.p99 << "\n";
            }
        }
    }
    csv.close();
    return 0;
}
