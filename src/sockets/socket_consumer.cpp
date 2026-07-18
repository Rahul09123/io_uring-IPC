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

struct Stats { double avg, stddev, p50, p95, p99, throughput_gbps; };

static Stats compute_throughput_stats(Telemetry* tel) {
    Stats s{};
    s.throughput_gbps = (TOTAL_BYTES / (1024.0 * 1024.0 * 1024.0)) / tel->execution_time_sec;
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

    std::cout << "[Consumer] Initializing Unix Domain Socket Consumer (Mode: " << mode << ")...\n";

    if (mode == "latency") {
        std::vector<char> buf(sizeof(MessageHeader) + MAX_PAYLOAD);

        for (size_t sz : MESSAGE_SIZES) {
            std::string dynamic_path = std::string(SOCKET_PATH) + "_" + std::to_string(sz);

            for (int run = 0; run <= NUM_RUNS; ++run) {
                int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (listen_fd < 0) { std::perror("socket"); return 1; }

                int buf_sz = static_cast<int>(MAX_PAYLOAD * 2);
                setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
                setsockopt(listen_fd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));

                struct sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::strncpy(addr.sun_path, dynamic_path.c_str(), sizeof(addr.sun_path) - 1);
                
                unlink(dynamic_path.c_str());
                if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                    std::perror("bind"); return 1;
                }

                if (listen(listen_fd, 5) < 0) { std::perror("listen"); return 1; }

                int client_fd = accept(listen_fd, nullptr, nullptr);
                if (client_fd < 0) { std::perror("accept"); return 1; }

                size_t num_trips = (run == 0) ? 1000 : 100000;
                for (size_t trip = 0; trip < num_trips; ++trip) {
                    if (!read_all(client_fd, buf.data(), sizeof(MessageHeader) + sz)) break;

                    volatile char checksum = 0;
                    char* payload = buf.data() + sizeof(MessageHeader);
                    for (size_t i = 0; i < sz; i += 64) checksum += payload[i];
                    (void)checksum;

                    if (!write_all(client_fd, buf.data(), sizeof(MessageHeader) + sz)) break;
                }
                close(client_fd);
                close(listen_fd);
                unlink(dynamic_path.c_str());
            }
        }
    } else {
        // Throughput Mode
        std::ofstream csv("socket_throughput.csv");
        csv << "message_size_bytes,run,throughput_gbps\n";

        std::vector<char> payload_buf(MAX_PAYLOAD);
        auto tel = std::make_unique<Telemetry>();

        std::cout << "[Consumer] Socket listener engine active...\n";

        for (size_t sz : MESSAGE_SIZES) {
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

                int client_fd = accept(listen_fd, nullptr, nullptr);
                if (client_fd < 0) { std::perror("accept"); return 1; }

                size_t consumed = 0;
                MessageHeader hdr;

                auto wall_start = std::chrono::high_resolution_clock::now();
                while (consumed < TOTAL_BYTES) {
                    if (!read_all(client_fd, &hdr, sizeof(MessageHeader))) break;
                    if (!read_all(client_fd, payload_buf.data(), hdr.payload_size)) break;

                    volatile char checksum = 0;
                    for (size_t i = 0; i < hdr.payload_size; i += 64) checksum += payload_buf[i];
                    (void)checksum;

                    consumed += hdr.payload_size;
                }
                auto wall_end = std::chrono::high_resolution_clock::now();
                tel->execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();

                close(client_fd);
                close(listen_fd);
                unlink(dynamic_path.c_str());

                Stats s = compute_throughput_stats(tel.get());
                if (run == 0) {
                    std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
                } else {
                    std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s\n";
                    csv << sz << "," << run << "," << s.throughput_gbps << "\n";
                }
            }
        }
        csv.close();
    }
    return 0;
}
