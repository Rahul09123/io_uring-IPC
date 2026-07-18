#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <unistd.h>
#include <sched.h>
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

    std::cout << "[Producer] Initializing POSIX Message Queue Producer (Mode: " << mode << ")...\n";

    if (mode == "latency") {
        std::ofstream csv("mq_latency.csv");
        csv << "message_size_bytes,run,avg_latency_us,stddev_us,p50_us,p90_us,p95_us,p99_us,p999_us\n";

        for (size_t sz : MESSAGE_SIZES) {
            std::cout << "[Producer] Testing Latency for Message Size: " << sz << " Bytes\n";
            std::string mq_name_a_to_b = "/ipc_mq_a_to_b_" + std::to_string(sz);
            std::string mq_name_b_to_a = "/ipc_mq_b_to_a_" + std::to_string(sz);

            mqd_t write_mq = -1;
            while (write_mq == (mqd_t)-1) {
                write_mq = mq_open(mq_name_a_to_b.c_str(), O_WRONLY);
                if (write_mq == (mqd_t)-1) usleep(100);
            }

            mqd_t read_mq = -1;
            while (read_mq == (mqd_t)-1) {
                read_mq = mq_open(mq_name_b_to_a.c_str(), O_RDONLY);
                if (read_mq == (mqd_t)-1) usleep(100);
            }

            std::vector<char> send_buf(sizeof(MessageHeader) + sz, 'X');
            std::vector<char> recv_buf(sizeof(MessageHeader) + sz + 4096);
            auto* hdr = reinterpret_cast<MessageHeader*>(send_buf.data());
            hdr->payload_size = static_cast<uint32_t>(sz);

            for (int run = 0; run <= NUM_RUNS; ++run) {
                size_t num_trips = (run == 0) ? 1000 : 100000;
                std::vector<double> run_latencies;
                run_latencies.reserve(num_trips);

                for (size_t trip = 0; trip < num_trips; ++trip) {
                    uint64_t start_ns = now_ns();
                    hdr->send_ns = start_ns;

                    if (mq_send(write_mq, send_buf.data(), send_buf.size(), 0) == -1) {
                        std::perror("mq_send failed");
                        break;
                    }

                    ssize_t bytes_read = mq_receive(read_mq, recv_buf.data(), recv_buf.size(), nullptr);
                    if (bytes_read < 0) {
                        std::perror("mq_receive failed");
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
            }
            mq_close(write_mq);
            mq_close(read_mq);
            usleep(5000);
        }
        csv.close();
    } else {
        // Throughput Mode
        for (size_t sz : MESSAGE_SIZES) {
            std::cout << "[Producer] Testing Throughput for Message Size: " << sz << " Bytes\n";
            std::string unique_mq_name = "/ipc_mq_bench_" + std::to_string(sz);

            std::vector<char> wire(sizeof(MessageHeader) + sz, 'X');
            auto* hdr = reinterpret_cast<MessageHeader*>(wire.data());
            hdr->payload_size = static_cast<uint32_t>(sz);

            size_t current_total_target = get_total_bytes(sz);

            mqd_t mq = -1;
            while (mq == (mqd_t)-1) {
                mq = mq_open(unique_mq_name.c_str(), O_WRONLY);
                if (mq == (mqd_t)-1) usleep(100); 
            }

            for (int run = 0; run <= NUM_RUNS; ++run) {
                size_t produced = 0;
                while (produced < current_total_target) {
                    hdr->send_ns = now_ns();
                    if (mq_send(mq, wire.data(), wire.size(), 0) == -1) {
                        std::perror("mq_send failed");
                        break;
                    }
                    produced += sz;
                }
                usleep(2000);
            }
            mq_close(mq);
            usleep(5000);
        }
    }
    std::cout << "[Producer] Run complete.\n";
    return 0;
}
