#include <sys/mman.h>
#include <mqueue.h>
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

    std::ofstream csv("mq_results.csv");
    csv << "message_size_bytes,run,throughput_gbps,avg_latency_us,stddev_us,p50_us,p95_us,p99_us\n";

    std::vector<char> wire_buf(sizeof(MessageHeader) + MAX_PAYLOAD);
    auto tel = std::make_unique<Telemetry>();

    std::cout << "[Consumer] Sync established. Listening for active producer loops...\n";

    for (size_t sz : MESSAGE_SIZES) {
        std::cout << "\n=========================================\n";
        std::cout << "Testing Message Size: " << sz << " Bytes\n";
        std::cout << "=========================================\n";

        size_t current_total_target = get_total_bytes(sz);
        std::string dynamic_name = std::string(MQ_NAME) + "_" + std::to_string(sz);

        for (int run = 0; run <= NUM_RUNS; ++run) {
            std::memset(tel.get(), 0, sizeof(Telemetry));
            size_t consumed = 0;
            uint64_t lat_idx = 0;

            mqd_t mq = (mqd_t)-1;
            while (mq == (mqd_t)-1) {
                mq = mq_open(dynamic_name.c_str(), O_RDONLY);
                if (mq == (mqd_t)-1) usleep(100); 
            }

            auto wall_start = std::chrono::high_resolution_clock::now();
            while (consumed < current_total_target) {
                ssize_t bytes_read = mq_receive(mq, wire_buf.data(), wire_buf.size(), nullptr);
                if (bytes_read <= 0) break;

                uint64_t recv_ns = now_ns();
                auto* hdr = reinterpret_cast<MessageHeader*>(wire_buf.data());

                if (lat_idx < MAX_LAT_SAMPLES) {
                    tel->latencies[lat_idx++] = static_cast<double>(recv_ns - hdr->send_ns) / 1000.0;
                }
                consumed += hdr->payload_size;
            }
            auto wall_end = std::chrono::high_resolution_clock::now();
            tel->execution_time_sec = std::chrono::duration<double>(wall_end - wall_start).count();
            tel->latency_count = lat_idx;

            mq_close(mq);

            Stats s = compute_stats(tel.get(), sz);
            if (run == 0) {
                std::cout << "  [Warmup]  throughput=" << s.throughput_gbps << " GB/s\n";
            } else {
                std::cout << "  Run " << run << " -> Throughput: " << s.throughput_gbps << " GB/s | p50 Latency: " << s.p50 << " us\n";
                csv << sz << "," << run << "," << s.throughput_gbps << "," << s.avg << "," 
                    << s.stddev << "," << s.p50 << "," << s.p95 << "," << s.p99 << "\n";
            }
        }
    }
    csv.close();
    return 0;
}
