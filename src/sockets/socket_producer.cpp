#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <fstream>
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

static bool write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
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

int main(int argc, char* argv[]) {
    set_affinity(PRODUCER_CORE);

    std::string mode = "throughput";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[i + 1];
        }
    }

    std::cout << "[Producer] Initializing Unix Domain Socket Producer (Mode: " << mode << ")...\n";

    if (mode == "latency") {
        std::ofstream csv("socket_latency.csv");
        csv << "message_size_bytes,run,avg_latency_us,stddev_us,p50_us,p90_us,p95_us,p99_us,p999_us\n";

        for (size_t sz : MESSAGE_SIZES) {
            std::cout << "[Producer] Testing Latency for Message Size: " << sz << " Bytes\n";
            std::string dynamic_path = std::string(SOCKET_PATH) + "_" + std::to_string(sz);

            std::vector<char> send_buf(sizeof(MessageHeader) + sz, 'X');
            std::vector<char> recv_buf(sizeof(MessageHeader) + sz);
            auto* hdr = reinterpret_cast<MessageHeader*>(send_buf.data());
            hdr->payload_size = static_cast<uint32_t>(sz);

            for (int run = 0; run <= NUM_RUNS; ++run) {
                int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (sockfd < 0) { std::perror("socket creation failed"); return 1; }

                int buf_sz = static_cast<int>(MAX_PAYLOAD * 2);
                setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
                setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));

                struct sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::strncpy(addr.sun_path, dynamic_path.c_str(), sizeof(addr.sun_path) - 1);

                while (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                    usleep(500);
                }

                size_t num_trips = (run == 0) ? 1000 : 100000;
                std::vector<double> run_latencies;
                run_latencies.reserve(num_trips);

                for (size_t trip = 0; trip < num_trips; ++trip) {
                    uint64_t start_ns = now_ns();
                    hdr->send_ns = start_ns;

                    if (!write_all(sockfd, send_buf.data(), send_buf.size())) {
                        std::cerr << "[Producer] send failure\n";
                        break;
                    }

                    if (!read_all(sockfd, recv_buf.data(), recv_buf.size())) {
                        std::cerr << "[Producer] recv failure\n";
                        break;
                    }

                    uint64_t end_ns = now_ns();
                    double rtt_us = static_cast<double>(end_ns - start_ns) / 1000.0;
                    run_latencies.push_back(rtt_us / 2.0);
                }

                if (run == 0) {
                    std::cout << "  [Warmup] Completed\n";
                } else {
                    LatencyStats s = compute_latency_stats(run_latencies);
                    std::cout << "  Run " << run << " -> Median Latency: " << s.p50 << " us | P99: " << s.p99 << " us\n";
                    csv << sz << "," << run << "," << s.avg << "," << s.stddev << "," 
                        << s.p50 << "," << s.p90 << "," << s.p95 << "," << s.p99 << "," << s.p999 << "\n";
                }
                close(sockfd);
                usleep(1000);
            }
        }
        csv.close();
    } else {
        // Throughput Mode
        for (size_t sz : MESSAGE_SIZES) {
            std::cout << "[Producer] Testing Throughput for Message Size: " << sz << " Bytes\n";
            std::string dynamic_path = std::string(SOCKET_PATH) + "_" + std::to_string(sz);

            std::vector<char> wire(sizeof(MessageHeader) + sz, 'X');
            auto* hdr = reinterpret_cast<MessageHeader*>(wire.data());
            hdr->payload_size = static_cast<uint32_t>(sz);

            for (int run = 0; run <= NUM_RUNS; ++run) {
                int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (sockfd < 0) { std::perror("socket creation failed"); return 1; }

                int buf_sz = static_cast<int>(MAX_PAYLOAD * 2);
                setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));

                struct sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::strncpy(addr.sun_path, dynamic_path.c_str(), sizeof(addr.sun_path) - 1);

                while (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                    usleep(500);
                }

                size_t produced = 0;
                while (produced < TOTAL_BYTES) {
                    hdr->send_ns = now_ns();
                    if (!write_all(sockfd, wire.data(), wire.size())) {
                        std::cerr << "[Producer] send failure\n";
                        break;
                    }
                    produced += sz;
                }
                close(sockfd);
                usleep(1000);
            }
        }
    }
    std::cout << "[Producer] Socket Run Complete.\n";
    return 0;
}
